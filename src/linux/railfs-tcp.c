// SPDX-License-Identifier: GPL-2.0
//
// A kernel socket speaking the rail control channel. Enough of it to
// negotiate: the mount dials the daemon, says Hello and reads the acknowledge,
// which is what proves the protocol works from here before any file does.

#include <linux/inet.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <net/sock.h>

#include "railfs-tcp.h"
#include "railfs-trace.h"
#include "railfs-proto.h"

// GFP_NOFS everywhere below, not GFP_KERNEL. Reclaim can enter this
// filesystem through ->writepages, which sends on the same path an
// allocation here would be waiting on, so an allocation that may reclaim
// filesystem pages can deadlock against itself.

#define RAILFS_MAX_FRAME (1u << 20)

/* Room for a hello: a version, a backend name and eight fixed-width fields. */
#define RAILFS_HELLO_BYTES 256

static int send_all(struct socket *sock, const void *buf, size_t len)
{
	struct kvec vec = { .iov_base = (void *)buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
	size_t done = 0;

	if (!sock) {
		return -ENOTCONN;
	}

	while (done < len) {
		vec.iov_base = (void *)((const u8 *)buf + done);
		vec.iov_len = len - done;

		int n = kernel_sendmsg(sock, &msg, &vec, 1, len - done);

		if (n <= 0) {
			return n ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

static int recv_all(struct socket *sock, void *buf, size_t len)
{
	struct kvec vec;
	struct msghdr msg = { .msg_flags = MSG_WAITALL };
	size_t done = 0;

	if (!sock) {
		return -ENOTCONN;
	}

	while (done < len) {
		vec.iov_base = (u8 *)buf + done;
		vec.iov_len = len - done;

		int n = kernel_recvmsg(sock, &msg, &vec, 1, len - done, MSG_WAITALL);

		if (n <= 0) {
			return n ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

// The daemon answers a Hello with a HelloAck naming the backend it agreed to.
// Anything else means the two ends disagree about the protocol.
// What this mount asks the daemon for. Split from the exchange itself so the
// fields and their order - which have to match the codec exactly - read as one
// list rather than being buried in the send and receive around them.
static int railfs_say_hello_frame(u8 *frame, size_t cap, bool rdma, bool verify, u32 *len)
{
	struct railfs_cursor c;

	c.buf = frame;
	c.len = cap;
	c.at = RAILFS_HEADER_SIZE;
	c.overrun = false;

	railfs_put_u16(&c, RAILFS_WIRE_VERSION);
	railfs_put_str(&c, rdma ? "rdma" : "tcp");
	railfs_put_u32(&c, 0);
	railfs_put_u64(&c, RAILFS_PAGE_COUNT);
	railfs_put_u64(&c, RAILFS_PAGE_SIZE);
	railfs_put_u64(&c, 1);
	railfs_put_u8(&c, 0);
	railfs_put_u8(&c, verify ? 1 : 0);
	railfs_put_u8(&c, RAILFS_SUM_XXH64);

	if (!railfs_cursor_ok(&c)) {
		return -EOVERFLOW;
	}

	railfs_frame(frame, RAILFS_MSG_HELLO, (u32)(c.at - RAILFS_HEADER_SIZE));
	*len = (u32)c.at;
	return 0;
}

static int say_hello(struct socket *sock, bool rdma, bool verify, char **endpoint_out, u32 *endpoint_len)
{
	u8 header[RAILFS_HEADER_SIZE];
	struct railfs_cursor c;
	char *backend = NULL;
	char *endpoint = NULL;
	u32 endpoint_bytes = 0;
	u8 *payload = NULL;
	u8 *frame = NULL;
	size_t cap = RAILFS_HELLO_BYTES;
	u32 magic, len;
	u32 sent = 0;
	u16 type;
	int err;

	frame = kzalloc(cap, GFP_NOFS);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}

	err = railfs_say_hello_frame(frame, cap, rdma, verify, &sent);
	if (err) {
		goto out;
	}

	err = send_all(sock, frame, sent);
	if (err) {
		goto out;
	}

	err = recv_all(sock, header, sizeof(header));
	if (err) {
		goto out;
	}

	memcpy(&magic, header, 4);
	memcpy(&type, header + 4, 2);
	memcpy(&len, header + 6, 4);

	if (magic != RAILFS_WIRE_MAGIC) {
		pr_err("railfs: bad frame magic %08x\n", magic);
		err = -EPROTO;
		goto out;
	}
	if (len > RAILFS_MAX_FRAME) {
		err = -EMSGSIZE;
		goto out;
	}
	if (type != RAILFS_MSG_HELLO_ACK) {
		pr_err("railfs: expected HelloAck, got message type %u\n", type);
		err = -EPROTO;
		goto out;
	}

	payload = kmalloc(len ? len : 1, GFP_NOFS);
	if (!payload) {
		err = -ENOMEM;
		goto out;
	}

	err = recv_all(sock, payload, len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = len;
	c.at = 0;

	err = railfs_get_str(&c, &backend);
	if (err) {
		goto out;
	}

	{
		size_t before = c.at;

		err = railfs_get_str(&c, &endpoint);
		if (err) {
			goto out;
		}

		// The rdma endpoint is a struct, not text, so its length matters and
		// cannot be recovered with strlen.
		endpoint_bytes = (u32)(c.at - before - 4);
	}

	pr_info("railfs: negotiated with the daemon, backend %s\n", backend);
	*endpoint_out = endpoint;
	*endpoint_len = endpoint_bytes;
	endpoint = NULL;
out:
	kfree(endpoint);
	kfree(backend);
	kfree(payload);
	kfree(frame);
	return err;
}

// One frame off a channel, checked against the type that was expected. The
// caller owns *payload on success.
// Every request begins the same way: a frame big enough for it, the next id on
// this connection, and a cursor sitting after the header with that id already
// written. Returns NULL only when there is no memory.
static u8 *railfs_request(struct railfs_conn *conn, size_t cap, struct railfs_cursor *c, u64 *id)
{
	u8 *frame = kzalloc(cap, GFP_NOFS);

	if (!frame) {
		return NULL;
	}

	mutex_lock(&conn->lock);
	*id = ++conn->next_id;
	mutex_unlock(&conn->lock);

	c->buf = frame;
	c->len = cap;
	c->at = RAILFS_HEADER_SIZE;
	c->overrun = false;
	railfs_put_u64(c, *id);
	return frame;
}

static int recv_frame(struct socket *sock, u16 want, u8 **payload, u32 *payload_len)
{
	u8 header[RAILFS_HEADER_SIZE];
	u32 magic;
	u32 len;
	u16 type;
	int err;

	err = recv_all(sock, header, sizeof(header));
	if (err) {
		goto out;
	}

	memcpy(&magic, header, 4);
	memcpy(&type, header + 4, 2);
	memcpy(&len, header + 6, 4);

	if (magic != RAILFS_WIRE_MAGIC) {
		err = -EPROTO;
		goto out;
	}

	if (len > RAILFS_MAX_FRAME) {
		err = -EMSGSIZE;
		goto out;
	}

	if (type != want) {
		pr_err("railfs: wanted message %u, got %u\n", want, type);
		err = -EPROTO;
		goto out;
	}

	*payload = kmalloc(len ? len : 1, GFP_NOFS);
	if (!*payload) {
		err = -ENOMEM;
		goto out;
	}

	if (len) {
		err = recv_all(sock, *payload, len);
		if (err) {
			kfree(*payload);
			*payload = NULL;
			goto out;
		}
	}

	*payload_len = len;
out:
	return err;
}

// One request, one reply, under the connection lock: the daemon answers in
// order on a single channel, so two callers interleaving would take each
// other's replies.
static int exchange(struct railfs_conn *conn, u8 *frame, size_t frame_len, u16 want, u8 **payload, u32 *payload_len)
{
	int err;

	mutex_lock(&conn->lock);

	err = send_all(conn->sock, frame, frame_len);
	if (!err) {
		err = recv_frame(conn->sock, want, payload, payload_len);
	}

	mutex_unlock(&conn->lock);
	return err;
}

void railfs_free_dirents(struct railfs_dirent *entries, u32 count)
{
	u32 i;

	if (!entries) {
		return;
	}

	for (i = 0; i < count; i++) {
		kfree(entries[i].name);
	}
	kfree(entries);
}

int railfs_space_of(struct railfs_conn *conn, const char *path, struct railfs_space *out)
{
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u64 replied = 0;
	size_t cap;
	u64 id;
	u8 ok = 0;
	int err;

	cap = RAILFS_HEADER_SIZE + 8 + 4 + strlen(path);

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_str(&c, path);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_STATFS, (u32)(c.at - RAILFS_HEADER_SIZE));

	err = exchange(conn, frame, c.at, RAILFS_MSG_STATFS_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_get_u64(&c, &replied);
	if (!err && replied != id) {
		err = -EPROTO;
	}
	if (!err) {
		err = railfs_get_u8(&c, &ok);
	}
	if (!err) {
		err = railfs_get_u64(&c, &out->block_size);
	}
	if (!err) {
		err = railfs_get_u64(&c, &out->blocks);
	}
	if (!err) {
		err = railfs_get_u64(&c, &out->blocks_free);
	}
	if (!err) {
		err = railfs_get_u64(&c, &out->files);
	}
	if (!err) {
		err = railfs_get_u64(&c, &out->files_free);
	}
	if (!err && !ok) {
		err = -EIO;
	}
out:
	kfree(payload);
	kfree(frame);
	return err;
}

int railfs_stat(struct railfs_conn *conn, const char *path, struct railfs_attrs *out, bool *found)
{
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u64 replied = 0;
	u64 asked = railfs_now();
	size_t cap;
	u64 id;
	u8 here = 0;
	int err;

	*found = false;

	cap = RAILFS_HEADER_SIZE + 8 + 4 + strlen(path);

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_str(&c, path);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_STAT, (u32)(c.at - RAILFS_HEADER_SIZE));

	err = exchange(conn, frame, c.at, RAILFS_MSG_STAT_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_get_u64(&c, &replied);
	if (err) {
		goto out;
	}

	if (replied != id) {
		pr_err("railfs: stat reply id %llu does not match request %llu\n", replied, id);
		err = -EPROTO;
		goto out;
	}

	err = railfs_get_u8(&c, &here);
	if (err) {
		goto out;
	}

	if (!here) {
		goto out;
	}

	err = railfs_get_attrs(&c, out);
	if (err) {
		goto out;
	}

	*found = true;
out:
	railfs_trace_add(RAILFS_PHASE_STAT, asked, 0);
	kfree(payload);
	kfree(frame);
	return err;
}

int railfs_list(struct railfs_conn *conn, const char *path, struct railfs_dirent **out, u32 *count)
{
	struct railfs_dirent *entries = NULL;
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u32 n = 0;
	u32 i;
	size_t cap;
	u64 id;
	u8 found = 0;
	int err;

	cap = RAILFS_HEADER_SIZE + 8 + 4 + strlen(path);

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_str(&c, path);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_LIST, (u32)(c.at - RAILFS_HEADER_SIZE));

	err = exchange(conn, frame, c.at, RAILFS_MSG_LIST_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	{
		u64 replied = 0;

		err = railfs_get_u64(&c, &replied);
		if (!err && replied != id) {
			pr_err("railfs: reply id %llu does not match request %llu\n", replied, id);
			err = -EPROTO;
		}
	}

	if (!err) {
		err = railfs_get_u8(&c, &found);
	}

	if (!err) {
		err = railfs_get_u32(&c, &n);
	}

	if (err) {
		goto out;
	}

	if (!found) {
		err = -ENOENT;
		goto out;
	}

	// Smallest an entry can be: a four byte name length and the attributes.
	// Without this a count off the wire asks for an allocation of any size.
	if ((u64)n * 30 > payload_len) {
		err = -EBADMSG;
		goto out;
	}

	entries = kcalloc(n ? n : 1, sizeof(*entries), GFP_NOFS);
	if (!entries) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < n; i++) {
		err = railfs_get_str(&c, &entries[i].name);
		if (!err) {
			err = railfs_get_attrs(&c, &entries[i].attrs);
		}
		if (err) {
			railfs_free_dirents(entries, n);
			entries = NULL;
			goto out;
		}
	}

	*out = entries;
	*count = n;
	entries = NULL;
out:
	railfs_free_dirents(entries, n);
	kfree(payload);
	kfree(frame);
	return err;
}

// The daemon answers a read on the control channel first and only then puts
// the payload on the data channel, so this reads them in that order. It always
// sends as many bytes as were asked for even at end of file - the reply says
// how many of them are real - because a byte-stream transport hangs on a short
// frame.
// The daemon hashed what it sent. Checking it here is what catches a page that
// arrived in the wrong place, which is the failure a one-sided transport makes
// possible and nothing else would notice.
static int railfs_matches_digest(const struct railfs_cursor *c, const u8 *payload, const void *buf, u32 len, const char *path, u64 offset)
{
	u8 want[RAILFS_DIGEST_SIZE];
	u8 got[RAILFS_DIGEST_SIZE];

	// It sits straight after Ok in the reply, so the cursor is already on it.
	if (c->at + sizeof(want) > c->len) {
		pr_err("railfs: read reply carried no digest\n");
		return -EBADMSG;
	}

	memcpy(want, payload + c->at, sizeof(want));

	// Everything that was asked for, with the tail past end of file zeroed:
	// the digest covers the whole frame, not the bytes that turned out real.
	railfs_digest(buf, len, got);

	if (memcmp(want, got, sizeof(want)) != 0) {
		pr_err("railfs: %s at %llu did not match its digest\n", path, offset);
		return -EBADMSG;
	}

	return 0;
}

// The reply to a ranged read: who it answers, how much the peer had, and how
// big the file is. Split out because the read itself is about moving bytes and
// this is about reading four fields in order.
static int railfs_read_reply(struct railfs_cursor *c, u64 id, u32 *reply_len, u64 *file_size)
{
	u64 replied = 0;
	u8 ok = 0;
	int err;

	err = railfs_get_u64(c, &replied);
	if (!err) {
		err = railfs_get_u32(c, reply_len);
	}
	if (!err) {
		err = railfs_get_u64(c, file_size);
	}
	if (!err) {
		err = railfs_get_u8(c, &ok);
	}
	if (err) {
		return err;
	}

	if (replied != id) {
		pr_err("railfs: read reply id %llu does not match request %llu\n", replied, id);
		return -EPROTO;
	}

	return ok ? 0 : -EIO;
}

// The bytes themselves, off whichever channel this connection has. Returns how
// many arrived, which the tcp path learns from a header and the fabric knows in
// advance because the peer sends everything that was asked for.
static int railfs_take_payload(struct railfs_conn *conn, u64 id, void *buf, struct sg_table *gpu, u32 len, u32 *got)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	u32 frame_len = 0;
	u64 key = 0;
	int err;

	if (conn->rail) {
		err = gpu ? railfs_rdma_fetch_sg(conn->rail, id, gpu, len) : railfs_rdma_fetch(conn->rail, id, buf, len);
		if (err < 0) {
			return err;
		}

		*got = len;
		return 0;
	}

	if (gpu) {
		return -EOPNOTSUPP;
	}

	err = recv_all(conn->data, header, sizeof(header));
	if (err) {
		return err;
	}

	memcpy(&key, header, 8);
	memcpy(&frame_len, header + 8, 4);

	if (key != id) {
		pr_err("railfs: data frame for key %llu, wanted %llu\n", key, id);
		return -EPROTO;
	}

	if (frame_len > len) {
		return -EMSGSIZE;
	}

	err = recv_all(conn->data, buf, frame_len);
	if (err) {
		return err;
	}

	*got = frame_len;
	return 0;
}

static bool railfs_gpu_allowed(const struct railfs_conn *conn)
{
	if (!conn->rail) {
		pr_warn_once("railfs: gpu buffers need an rdma mount\n");
		return false;
	}

	if (conn->verify) {
		pr_warn_once("railfs: gpu buffers need a noverify mount; the cpu cannot hash gpu memory\n");
		return false;
	}

	return true;
}

static int railfs_read_from(struct railfs_conn *conn, const char *path, u64 offset, void *buf, struct sg_table *gpu, u32 len)
{
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u32 reply_len = 0;
	u32 frame_len = 0;
	u64 file_size = 0;
	u64 wire = railfs_now();
	u64 digest;
	u64 ctl;
	u64 pull;
	size_t cap;
	u64 id;
	int err;

	if (len > RAILFS_PAGE_SIZE) {
		len = RAILFS_PAGE_SIZE;
	}

	// One of the two carries the payload. A connection has exactly one.
	if (!conn->data && !conn->rail) {
		return -EOPNOTSUPP;
	}

	if (gpu && !railfs_gpu_allowed(conn)) {
		return -EOPNOTSUPP;
	}

	cap = RAILFS_HEADER_SIZE + 8 + 4 + strlen(path) + 8 + 4 + 8;

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_str(&c, path);
	railfs_put_u64(&c, offset);
	railfs_put_u32(&c, len);
	railfs_put_u64(&c, 0);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_READ, (u32)(c.at - RAILFS_HEADER_SIZE));

	// Held across both channels: the payload belongs to whoever asked for it,
	// and a second reader taking it would leave this one waiting forever.
	mutex_lock(&conn->lock);

	ctl = railfs_now();

	err = send_all(conn->sock, frame, c.at);
	if (err) {
		goto unlock;
	}

	err = recv_frame(conn->sock, RAILFS_MSG_TRANSFER_REPLY, &payload, &payload_len);
	if (err) {
		goto unlock;
	}

	railfs_trace_add(RAILFS_PHASE_READ_CTL, ctl, len);

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_read_reply(&c, id, &reply_len, &file_size);
	if (err) {
		goto unlock;
	}

	pull = railfs_now();
	err = railfs_take_payload(conn, id, buf, gpu, len, &frame_len);
	railfs_trace_add(RAILFS_PHASE_READ_PULL, pull, len);
	if (err) {
		goto unlock;
	}

	err = reply_len < frame_len ? reply_len : frame_len;
	railfs_trace_add(RAILFS_PHASE_READ_WIRE, wire, frame_len);

	// A session that asked for no verification is answered with a zeroed
	// digest field, not with a shorter frame, so checking it here would fail
	// every read rather than skip the check.
	digest = railfs_now();
	if (conn->verify && !gpu && railfs_matches_digest(&c, payload, buf, frame_len, path, offset)) {
		err = -EBADMSG;
	}
	railfs_trace_add(RAILFS_PHASE_READ_DIGEST, digest, frame_len);
unlock:
	mutex_unlock(&conn->lock);
out:
	kfree(payload);
	kfree(frame);
	return err;
}

int railfs_read(struct railfs_conn *conn, const char *path, u64 offset, void *buf, u32 len)
{
	return railfs_read_from(conn, path, offset, buf, NULL, len);
}

int railfs_read_sg(struct railfs_conn *conn, const char *path, u64 offset, struct sg_table *pages, u32 len)
{
	return railfs_read_from(conn, path, offset, NULL, pages, len);
}

// The mirror of railfs_take_payload: the bytes out, on whichever channel this
// connection has.
// A fabric connection maps the folio and sends it where it lies. A tcp one has
// to walk it into the socket, and kmap gives it a linear view to do that from.
static int railfs_give_payload_folios(struct railfs_conn *conn, u64 id, struct folio **folios, unsigned int nr, u32 len)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	u32 left = len;
	unsigned int i;
	int err;

	if (conn->rail) {
		err = railfs_rdma_push_folios(conn->rail, id, folios, nr, len);
		return err < 0 ? err : 0;
	}

	memcpy(header, &id, 8);
	memcpy(header + 8, &len, 4);

	err = send_all(conn->data, header, sizeof(header));
	if (err) {
		return err;
	}

	for (i = 0; i < nr && left; i++) {
		// One mapping per folio, which holds because this builds for kernels
		// without highmem. On one that had it, this would see a page.
		u32 bytes = min_t(u32, left, (u32)folio_size(folios[i]));
		void *at = kmap_local_folio(folios[i], 0);

		err = send_all(conn->data, at, bytes);
		kunmap_local(at);
		if (err) {
			return err;
		}
		left -= bytes;
	}

	return left ? -EMSGSIZE : 0;
}

static int railfs_give_payload_sg(struct railfs_conn *conn, u64 id, struct sg_table *pages, u32 len)
{
	int err;

	if (!conn->rail) {
		return -EOPNOTSUPP;
	}

	err = railfs_rdma_push_sg(conn->rail, id, pages, len);
	return err < 0 ? err : 0;
}

static int railfs_give_payload(struct railfs_conn *conn, u64 id, const void *buf, u32 len)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	int err;

	if (conn->rail) {
		err = railfs_rdma_push(conn->rail, id, buf, len);
		return err < 0 ? err : 0;
	}

	memcpy(header, &id, 8);
	memcpy(header + 8, &len, 4);

	err = send_all(conn->data, header, sizeof(header));
	if (err) {
		return err;
	}

	return send_all(conn->data, buf, len);
}

// Without truncate an overwrite shorter than what was there leaves the old
// tail on the peer, and the mount reports a size that does not match the file.
// mkdir, unlink, rmdir, chmod, utimes and the link operations travel the same
// way, so they share this.
int railfs_meta_send(struct railfs_conn *conn, const struct railfs_meta_req *req)
{
	const char *target = req->target ? req->target : "";
	struct railfs_cursor c;
	char *message = NULL;
	char *reported = NULL;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u64 replied = 0;
	u32 code = 0;
	size_t cap;
	u64 id;
	u8 ok = 0;
	int err;

	cap = RAILFS_HEADER_SIZE + 8 + 2 + 4 + strlen(req->path) + 4 + strlen(target) + 8 + 4 + 8 + 8;

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_u16(&c, req->op);
	railfs_put_str(&c, req->path);
	railfs_put_str(&c, target);
	railfs_put_u64(&c, req->size);
	railfs_put_u32(&c, req->mode);
	railfs_put_u64(&c, (u64)req->mtime);
	railfs_put_u64(&c, 0);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_META, (u32)(c.at - RAILFS_HEADER_SIZE));

	err = exchange(conn, frame, c.at, RAILFS_MSG_META_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	// The daemon says why in the reply, and readlink says what it found in the
	// same field a refusal leaves empty. Without reading both, every refusal
	// reaches the caller as EIO and a link has no target.
	err = railfs_get_u64(&c, &replied);
	if (!err) {
		err = railfs_get_u8(&c, &ok);
	}
	if (!err) {
		err = railfs_get_str(&c, &message);
	}
	if (!err) {
		err = railfs_get_str(&c, &reported);
	}
	if (!err) {
		err = railfs_get_u32(&c, &code);
	}
	if (err) {
		goto out;
	}

	if (replied != id) {
		err = -EPROTO;
		goto out;
	}

	if (!ok) {
		// A code the peer invented is no use to a caller here.
		err = (code > 0 && code < MAX_ERRNO) ? -(int)code : -EIO;
		goto out;
	}

	if (req->link) {
		*req->link = reported;
		reported = NULL;
	}

	err = 0;
out:
	kfree(message);
	kfree(reported);
	kfree(payload);
	kfree(frame);
	return err;
}

int railfs_meta(struct railfs_conn *conn, u16 op, const char *path, u64 size)
{
	struct railfs_meta_req req = {
		.op = op,
		.path = path,
		.size = size,
	};

	return railfs_meta_send(conn, &req);
}

int railfs_meta_to(struct railfs_conn *conn, u16 op, const char *path, const char *target, u64 size)
{
	struct railfs_meta_req req = {
		.op = op,
		.path = path,
		.target = target,
		.size = size,
	};

	return railfs_meta_send(conn, &req);
}

// The daemon opens a write target with O_CREAT, so an empty truncating write
// is what brings a file into existence. There is no separate create on the
// wire.
int railfs_create_file(struct railfs_conn *conn, const char *path)
{
	int err = railfs_write(conn, path, 0, "", 0, true);

	return err < 0 ? err : 0;
}

// A write is the mirror of a read: the request names the bytes, the data
// channel carries them, and the reply says how many landed. The daemon checks
// the payload digest only when the session asked for verification, which this
// one does not - the kernel has no xxhash.
static int railfs_write_from(struct railfs_conn *conn, const char *path, u64 offset, const void *buf, struct folio **folios,
			   unsigned int nr, struct sg_table *gpu, u32 len, bool truncate)
{
	u8 digest[RAILFS_DIGEST_SIZE] = {};
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	u32 reply_len = 0;
	u64 file_size = 0;
	u64 replied = 0;
	u64 mark;
	u64 wire;
	size_t cap;
	u64 id;
	u8 ok = 0;
	int err;

	if (len > RAILFS_PAGE_SIZE) {
		len = RAILFS_PAGE_SIZE;
	}

	// One of the two carries the payload. A connection has exactly one.
	if (!conn->data && !conn->rail) {
		return -EOPNOTSUPP;
	}

	if (gpu && !railfs_gpu_allowed(conn)) {
		return -EOPNOTSUPP;
	}

	// Straight out of the folio when there is one, so nothing is copied only to
	// be hashed.
	mark = railfs_now();
	if (folios) {
		railfs_digest_folios(folios, nr, len, digest);
	} else if (!gpu) {
		railfs_digest(buf, len, digest);
	}
	railfs_trace_add(RAILFS_PHASE_WRITE_DIGEST, mark, len);

	cap = RAILFS_HEADER_SIZE + 8 + 4 + strlen(path) + 8 + 4 + 1 + RAILFS_DIGEST_SIZE + 8;

	frame = railfs_request(conn, cap, &c, &id);
	if (!frame) {
		err = -ENOMEM;
		goto out;
	}
	railfs_put_str(&c, path);
	railfs_put_u64(&c, offset);
	railfs_put_u32(&c, len);
	railfs_put_u8(&c, truncate ? 1 : 0);
	railfs_put_raw(&c, digest, sizeof(digest));
	railfs_put_u64(&c, 0);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_WRITE, (u32)(c.at - RAILFS_HEADER_SIZE));

	mutex_lock(&conn->lock);
	wire = railfs_now();

	err = send_all(conn->sock, frame, c.at);
	if (err) {
		goto unlock;
	}

	// The daemon posts its receive as soon as it has the request, so the bytes
	// follow immediately on the data channel with the request id as the key.
	if (folios) {
		err = railfs_give_payload_folios(conn, id, folios, nr, len);
	} else if (gpu) {
		err = railfs_give_payload_sg(conn, id, gpu, len);
	} else {
		err = railfs_give_payload(conn, id, buf, len);
	}
	if (err) {
		goto unlock;
	}

	err = recv_frame(conn->sock, RAILFS_MSG_TRANSFER_REPLY, &payload, &payload_len);
	if (err) {
		goto unlock;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_get_u64(&c, &replied);
	if (!err) {
		err = railfs_get_u32(&c, &reply_len);
	}
	if (!err) {
		err = railfs_get_u64(&c, &file_size);
	}
	if (!err) {
		err = railfs_get_u8(&c, &ok);
	}
	if (err) {
		goto unlock;
	}

	if (replied != id) {
		pr_err("railfs: write reply id %llu does not match request %llu\n", replied, id);
		err = -EPROTO;
		goto unlock;
	}

	err = ok ? (int)reply_len : -EIO;
	railfs_trace_add(RAILFS_PHASE_WRITE_WIRE, wire, len);
unlock:
	mutex_unlock(&conn->lock);
out:
	kfree(payload);
	kfree(frame);
	return err;
}

// The fabric equivalent of joining the data channel: the daemon's endpoint is
// its rail rather than an address, so this drives the queue pair to ready
// against it and sends back the one it should reach us on.
static int join_fabric(struct railfs_conn *conn, const char *endpoint, u32 len, const struct railfs_wire *mine)
{
	u8 frame[RAILFS_HEADER_SIZE + 4 + sizeof(struct railfs_wire)];
	struct railfs_cursor c;
	struct railfs_wire peer;
	int err;

	if (len != sizeof(peer)) {
		pr_err("railfs: rdma endpoint is %u bytes, wanted %zu\n", len, sizeof(peer));
		return -EPROTO;
	}

	memcpy(&peer, endpoint, sizeof(peer));

	err = railfs_rdma_meet(conn->rail, &peer);
	if (err) {
		return err;
	}

	c.buf = frame;
	c.len = sizeof(frame);
	c.at = RAILFS_HEADER_SIZE;
	c.overrun = false;
	railfs_put_u32(&c, (u32)sizeof(*mine));
	railfs_put_raw(&c, mine, sizeof(*mine));

	if (!railfs_cursor_ok(&c)) {
		return -EOVERFLOW;
	}

	railfs_frame(frame, RAILFS_MSG_PEER_ENDPOINT, (u32)(c.at - RAILFS_HEADER_SIZE));
	return send_all(conn->sock, frame, c.at);
}

// The daemon answers HelloAck with "addr:port" for its data channel, waits for
// a connection on it, and only then reads the endpoint the client sends back.
// Skipping either leaves it reading a message it did not ask for, which it
// reports as a protocol error and closes.
static int join_data_channel(struct railfs_conn *conn, const char *endpoint)
{
	struct sockaddr_in addr = {};
	struct railfs_cursor c;
	u8 frame[RAILFS_HEADER_SIZE + 4];
	char host[64];
	const char *colon;
	unsigned int port;
	int err;

	colon = strrchr(endpoint, ':');
	if (!colon || colon == endpoint || (size_t)(colon - endpoint) >= sizeof(host)) {
		err = -EPROTO;
		goto out;
	}

	memcpy(host, endpoint, colon - endpoint);
	host[colon - endpoint] = '\0';

	if (kstrtouint(colon + 1, 10, &port) || port > U16_MAX) {
		err = -EPROTO;
		goto out;
	}

	if (!in4_pton(host, -1, (u8 *)&addr.sin_addr.s_addr, -1, NULL)) {
		err = -EPROTO;
		goto out;
	}

	err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &conn->data);
	if (err) {
		goto out;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16)port);

	err = kernel_connect(conn->data, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (err) {
		pr_err("railfs: data channel to %s:%u failed: %d\n", host, port, err);
		goto out;
	}

	c.buf = frame;
	c.len = sizeof(frame);
	c.at = RAILFS_HEADER_SIZE;
	c.overrun = false;
	railfs_put_str(&c, "");

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_PEER_ENDPOINT, (u32)(c.at - RAILFS_HEADER_SIZE));
	err = send_all(conn->sock, frame, c.at);
out:
	return err;
}

struct railfs_conn *railfs_connect(const char *host, u16 port, bool rdma, bool verify)
{
	struct __kernel_sock_timeval tv = { .tv_sec = 30, .tv_usec = 0 };
	struct sockaddr_in addr = {};
	struct railfs_conn *conn = NULL;
	struct railfs_wire mine = {};
	char *endpoint = NULL;
	u32 endpoint_len = 0;
	__be32 ip;
	int err;

	if (!host) {
		err = -EINVAL;
		goto fail;
	}

	// in_aton does not reject a name, it invents an address from one, so a
	// mount naming a host rather than an address would dial somewhere
	// arbitrary instead of failing.
	if (!in4_pton(host, -1, (u8 *)&ip, -1, NULL)) {
		pr_err("railfs: host=%s is not an IPv4 address\n", host);
		err = -EINVAL;
		goto fail;
	}

	conn = kzalloc(sizeof(*conn), GFP_NOFS);
	if (!conn) {
		err = -ENOMEM;
		goto fail;
	}

	mutex_init(&conn->lock);

	err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &conn->sock);
	if (err) {
		goto fail;
	}

	// Without a deadline a peer that stops answering leaves every reader
	// asleep holding the channel lock, and umount blocks behind them.
	sock_setsockopt(conn->sock, SOL_SOCKET, SO_RCVTIMEO_NEW, KERNEL_SOCKPTR(&tv), sizeof(tv));
	sock_setsockopt(conn->sock, SOL_SOCKET, SO_SNDTIMEO_NEW, KERNEL_SOCKPTR(&tv), sizeof(tv));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = htons(port);

	err = kernel_connect(conn->sock, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (err) {
		pr_err("railfs: connect to %s:%u failed: %d\n", host, port, err);
		goto fail;
	}

	if (rdma) {
		conn->rail = railfs_rdma_open(&mine);
		if (IS_ERR(conn->rail)) {
			err = PTR_ERR(conn->rail);
			conn->rail = NULL;
			goto fail;
		}
	}

	conn->verify = verify;

	err = say_hello(conn->sock, rdma, verify, &endpoint, &endpoint_len);
	if (err) {
		goto fail;
	}

	err = rdma ? join_fabric(conn, endpoint, endpoint_len, &mine) : join_data_channel(conn, endpoint);
	if (err) {
		goto fail;
	}

	kfree(endpoint);
	return conn;

fail:
	kfree(endpoint);
	railfs_disconnect(conn);
	return ERR_PTR(err);
}

struct railfs_pool *railfs_pool_open(const char *host, u16 port, unsigned int count, bool rdma, bool verify)
{
	struct railfs_pool *pool;
	unsigned int i;
	int err;

	if (count < 1) {
		count = 1;
	}

	if (count > RAILFS_MAX_CONNS) {
		count = RAILFS_MAX_CONNS;
	}

	pool = kzalloc(sizeof(*pool), GFP_NOFS);
	if (!pool) {
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->waiters);

	for (i = 0; i < count; i++) {
		struct railfs_conn *conn = railfs_connect(host, port, rdma, verify);

		if (IS_ERR(conn)) {
			err = PTR_ERR(conn);
			goto fail;
		}

		pool->conns[i] = conn;
		pool->count++;
	}

	return pool;

fail:
	railfs_pool_close(pool);
	return ERR_PTR(err);
}

void railfs_pool_close(struct railfs_pool *pool)
{
	unsigned int i;

	if (!pool) {
		return;
	}

	for (i = 0; i < pool->count; i++) {
		railfs_disconnect(pool->conns[i]);
	}

	kfree(pool);
}

// Waits rather than fails when every connection is busy: the caller is a
// filesystem operation with nowhere else to go.
// Every connection held, as a bitmap. Its own function because the obvious
// expression is undefined at the width of the word it is building.
static unsigned long railfs_pool_all(const struct railfs_pool *pool)
{
	if (pool->count >= BITS_PER_LONG) {
		return ~0UL;
	}

	return (1UL << pool->count) - 1;
}

struct railfs_conn *railfs_pool_take_near(struct railfs_pool *pool, unsigned int hint, unsigned int span)
{
	struct railfs_conn *conn = NULL;
	unsigned long window = 0;
	unsigned int i;

	if (span < 1 || span >= pool->count) {
		return railfs_pool_take(pool);
	}

	for (i = 0; i < span; i++) {
		window |= 1UL << ((hint + i) % pool->count);
	}

	for (;;) {
		spin_lock(&pool->lock);

		for (i = 0; i < span; i++) {
			unsigned int at = (hint + i) % pool->count;

			if (!test_bit(at, &pool->busy)) {
				__set_bit(at, &pool->busy);
				conn = pool->conns[at];
				break;
			}
		}

		spin_unlock(&pool->lock);

		if (conn) {
			railfs_trace_busy(1);
			return conn;
		}

		// On this window's own bits, not the pool's: waiting for any connection
		// returns at once while anything outside the window is idle, which
		// spins rather than sleeps.
		wait_event(pool->waiters, (pool->busy & window) != window);
	}
}

struct railfs_conn *railfs_pool_take(struct railfs_pool *pool)
{
	struct railfs_conn *conn = NULL;
	unsigned int i;

	for (;;) {
		spin_lock(&pool->lock);

		for (i = 0; i < pool->count; i++) {
			if (!test_bit(i, &pool->busy)) {
				__set_bit(i, &pool->busy);
				conn = pool->conns[i];
				break;
			}
		}

		spin_unlock(&pool->lock);

		if (conn) {
			railfs_trace_busy(1);
			return conn;
		}

		// Not (1UL << count) - 1: at count == BITS_PER_LONG that shift is
		// undefined, and here yields a mask of zero, which spins.
		wait_event(pool->waiters, pool->busy != railfs_pool_all(pool));
	}
}

void railfs_pool_give(struct railfs_pool *pool, struct railfs_conn *conn)
{
	unsigned int i;

	railfs_trace_busy(-1);
	spin_lock(&pool->lock);

	for (i = 0; i < pool->count; i++) {
		if (pool->conns[i] == conn) {
			__clear_bit(i, &pool->busy);
			break;
		}
	}

	spin_unlock(&pool->lock);
	wake_up(&pool->waiters);
}

void railfs_disconnect(struct railfs_conn *conn)
{
	if (!conn) {
		return;
	}
	railfs_rdma_close(conn->rail);

	if (conn->data) {
		sock_release(conn->data);
	}
	if (conn->sock) {
		sock_release(conn->sock);
	}
	kfree(conn);
}

int railfs_write(struct railfs_conn *conn, const char *path, u64 offset, const void *buf, u32 len, bool truncate)
{
	return railfs_write_from(conn, path, offset, buf, NULL, 0, NULL, len, truncate);
}

int railfs_write_folios(struct railfs_conn *conn, const char *path, u64 offset, struct folio **folios, unsigned int nr, u32 len,
		      bool truncate)
{
	return railfs_write_from(conn, path, offset, NULL, folios, nr, NULL, len, truncate);
}

int railfs_write_sg(struct railfs_conn *conn, const char *path, u64 offset, struct sg_table *pages, u32 len, bool truncate)
{
	return railfs_write_from(conn, path, offset, NULL, NULL, 0, pages, len, truncate);
}
