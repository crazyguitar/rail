// SPDX-License-Identifier: GPL-2.0
//
// The rail control channel, shared by every caller on the mount: a request is
// registered by id before it goes out, and a reader thread hands each reply
// to the call that asked. A tcp data socket has a reader of its own.

#include <linux/inet.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <net/tcp_states.h>

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

#define RAILFS_SOCKET_DEADLINE_SECONDS 30
#define RAILFS_REPLY_WAIT_MS 30000
#define RAILFS_REVIVE_INTERVAL (5 * HZ)

// One request between its send and its reply. Lives in the caller's frame and
// stays on the connection's list until the caller takes it off, so a reader
// that finds it there may fill it in under the list lock.
struct railfs_call {
	struct list_head link;
	u64 id;
	u16 want;
	int err;
	u8 *payload;
	u32 payload_len;
	struct completion replied;
	/* Whether the reader has already handed this call its reply and its
	 * bytes, so a connection dying afterwards does not fail an answer that
	 * arrived. Both are set under calls_lock.
	 */
	bool answered;
	bool delivered;
	/* Over the fabric: the rail and slot this call holds, and whether payload
	 * points into the reply ring, which lives until the slot is given back.
	 */
	u32 line;
	u32 slot;
	bool on_ring;
	bool borrowed;
	/* Where a tcp read's bytes land, when the call expects some. */
	void *buf;
	u32 room;
	u32 got;
	int data_err;
	bool wants_data;
	/* The data reader is receiving into buf outside the lock, so the owner
	 * waits for released before it leaves.
	 */
	bool busy;
	struct completion landed;
	struct completion released;
};

static void railfs_conn_kill(struct railfs_conn *conn, int why);

static void railfs_set_deadlines(struct socket *sock)
{
	struct __kernel_sock_timeval tv = { .tv_sec = RAILFS_SOCKET_DEADLINE_SECONDS, .tv_usec = 0 };

	sock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO_NEW, KERNEL_SOCKPTR(&tv), sizeof(tv));
	sock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO_NEW, KERNEL_SOCKPTR(&tv), sizeof(tv));
}

static int send_all(struct socket *sock, const void *buf, size_t len)
{
	struct kvec vec = { .iov_base = (void *)buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
	size_t done = 0;
	int n;

	if (!sock) {
		return -ENOTCONN;
	}

	while (done < len) {
		vec.iov_base = (void *)((const u8 *)buf + done);
		vec.iov_len = len - done;

		n = kernel_sendmsg(sock, &msg, &vec, 1, len - done);
		if (n <= 0) {
			return n ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

// A reader's receive. A quiet peer trips the socket deadline for nothing, so
// that is not an error here: a call that waited too long kills the connection.
static int recv_all(struct railfs_conn *conn, struct socket *sock, void *buf, size_t len)
{
	struct kvec vec;
	struct msghdr msg = { .msg_flags = MSG_WAITALL };
	size_t done = 0;
	int n;

	if (!sock) {
		return -ENOTCONN;
	}

	while (done < len) {
		vec.iov_base = (u8 *)buf + done;
		vec.iov_len = len - done;

		n = kernel_recvmsg(sock, &msg, &vec, 1, len - done, MSG_WAITALL);
		if (n == -EAGAIN) {
			/* On a live connection this says only that the peer is quiet.
			 * On a dead one it is the shutdown, and -EAGAIN would travel
			 * on as the reason a call failed.
			 */
			if (!READ_ONCE(conn->dead) && !kthread_should_stop()) {
				continue;
			}
			return -ENOTCONN;
		}
		if (n <= 0) {
			return n ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

// The handshake's receive, before any reader exists: a deadline that passes
// means the daemon is not answering, and that is the error.
static int recv_now(struct socket *sock, void *buf, size_t len)
{
	struct kvec vec;
	struct msghdr msg = { .msg_flags = MSG_WAITALL };
	size_t done = 0;
	int n;

	while (done < len) {
		vec.iov_base = (u8 *)buf + done;
		vec.iov_len = len - done;

		n = kernel_recvmsg(sock, &msg, &vec, 1, len - done, MSG_WAITALL);
		if (n <= 0) {
			return n ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

static int railfs_read_header(const u8 *header, u16 *type, u32 *len)
{
	u32 magic;

	memcpy(&magic, header, 4);
	memcpy(type, header + 4, 2);
	memcpy(len, header + 6, 4);

	if (magic != RAILFS_WIRE_MAGIC) {
		pr_err("railfs: bad frame magic %08x\n", magic);
		return -EPROTO;
	}

	if (*len > RAILFS_MAX_FRAME) {
		return -EMSGSIZE;
	}

	return 0;
}

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

// The daemon answers a Hello with a HelloAck naming the backend it agreed to.
// Anything else means the two ends disagree about the protocol.
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
	u32 len;
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

	err = recv_now(sock, header, sizeof(header));
	if (err) {
		goto out;
	}

	err = railfs_read_header(header, &type, &len);
	if (err) {
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

	err = recv_now(sock, payload, len);
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

	pr_info_ratelimited("railfs: negotiated with the daemon, backend %s\n", backend);
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

// Every request begins the same way: a frame big enough for it, the next id on
// this connection, and a cursor sitting after the header with that id already
// written. Returns NULL only when there is no memory.
static u8 *railfs_request(struct railfs_conn *conn, size_t cap, struct railfs_cursor *c, u64 *id)
{
	u8 *frame = kzalloc(cap, GFP_NOFS);

	if (!frame) {
		return NULL;
	}

	*id = (u64)atomic64_inc_return(&conn->next_id);

	c->buf = frame;
	c->len = cap;
	c->at = RAILFS_HEADER_SIZE;
	c->overrun = false;
	railfs_put_u64(c, *id);
	return frame;
}

// Registered before the request goes out: a reply that arrives while nobody
// is waiting for it has nowhere to go.
static int railfs_call_begin(struct railfs_conn *conn, struct railfs_call *call, u64 id, u16 want, void *buf, u32 room)
{
	int err = 0;

	memset(call, 0, sizeof(*call));
	INIT_LIST_HEAD(&call->link);
	call->id = id;
	call->want = want;
	call->buf = buf;
	call->room = room;
	call->wants_data = buf != NULL;
	init_completion(&call->replied);
	init_completion(&call->landed);
	init_completion(&call->released);

	if (conn->rail) {
		err = railfs_rdma_ctrl_take(conn->rail, &call->line, &call->slot);
		if (err) {
			return err;
		}
		call->on_ring = true;
	}

	spin_lock_bh(&conn->calls_lock);
	if (READ_ONCE(conn->dead)) {
		err = -ENOTCONN;
	} else {
		list_add_tail(&call->link, &conn->calls);
		if (call->on_ring) {
			conn->owner[call->line][call->slot] = call;
		}
	}
	spin_unlock_bh(&conn->calls_lock);

	if (err && call->on_ring) {
		railfs_rdma_ctrl_give(conn->rail, call->line, call->slot);
	}
	if (!err) {
		railfs_trace_calls(1);
	}
	return err;
}

// Off the list, and not before the data reader has finished with the buffer.
// A caller that left mid-frame waits for the rest of it, but only so long: the
// socket is live, so nothing but a kill would wake a reader whose peer stalled.
static void railfs_call_end(struct railfs_conn *conn, struct railfs_call *call)
{
	bool busy;

	spin_lock_bh(&conn->calls_lock);
	list_del_init(&call->link);
	if (call->on_ring) {
		conn->owner[call->line][call->slot] = NULL;
	}
	busy = call->busy;
	spin_unlock_bh(&conn->calls_lock);

	if (busy && !wait_for_completion_timeout(&call->released, msecs_to_jiffies(RAILFS_REPLY_WAIT_MS))) {
		railfs_conn_kill(conn, -ETIMEDOUT);
		wait_for_completion(&call->released);
	}

	railfs_trace_calls(-1);
	if (!call->borrowed) {
		kfree(call->payload);
	}
	call->payload = NULL;
	if (call->on_ring) {
		railfs_rdma_ctrl_give(conn->rail, call->line, call->slot);
	}
}

// A reply that is late is a peer that has stopped, which kills the connection
// for everyone on it. A fatal signal is one caller's business: it leaves, and
// the reply is dropped when it comes.
static int railfs_wait_on(struct railfs_conn *conn, struct completion *done)
{
	long left = wait_for_completion_killable_timeout(done, msecs_to_jiffies(RAILFS_REPLY_WAIT_MS));

	if (left == 0) {
		railfs_conn_kill(conn, -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	if (left < 0) {
		return -ERESTARTSYS;
	}

	return 0;
}

static int railfs_call_wait(struct railfs_conn *conn, struct railfs_call *call)
{
	int err = railfs_wait_on(conn, &call->replied);

	if (err) {
		return err;
	}

	// A bad reply was noticed in softirq, which cannot kill the connection.
	if (READ_ONCE(conn->ring_fault)) {
		railfs_conn_kill(conn, READ_ONCE(conn->ring_fault));
	}

	return call->err;
}

static int railfs_call_wait_data(struct railfs_conn *conn, struct railfs_call *call)
{
	int err = railfs_wait_on(conn, &call->landed);

	if (err) {
		return err;
	}

	return call->data_err;
}

// A frame reaches the wire whole: into the daemon's ring at this call's slot,
// or under the send lock on tcp. A failed send takes the connection with it.
static int railfs_send(struct railfs_conn *conn, struct railfs_call *call, const void *frame, size_t len)
{
	int err;

	if (call && call->on_ring) {
		err = READ_ONCE(conn->dead) ? -ENOTCONN : railfs_rdma_ctrl_send(conn->rail, call->line, call->slot, frame, (u32)len);
		if (!err) {
			railfs_trace_ctrl_rail(call->line);
		}
	} else {
		mutex_lock(&conn->send_lock);
		err = READ_ONCE(conn->dead) ? -ENOTCONN : send_all(conn->sock, frame, len);
		mutex_unlock(&conn->send_lock);
		if (!err) {
			railfs_trace_tcp_frames(1);
		}
	}

	if (err) {
		railfs_conn_kill(conn, err);
	}
	return err;
}

static struct railfs_call *railfs_call_for(struct railfs_conn *conn, u64 id)
{
	struct railfs_call *call;

	list_for_each_entry(call, &conn->calls, link) {
		if (call->id == id) {
			return call;
		}
	}
	return NULL;
}

// One control frame, handed to whoever asked for it. The id is the first
// field of every reply, so it can be read before the reply is understood.
static int railfs_deliver(struct railfs_conn *conn, u16 type, u8 *payload, u32 len)
{
	struct railfs_call *call;
	u64 id;

	if (len < 8) {
		kfree(payload);
		return -EBADMSG;
	}

	memcpy(&id, payload, 8);

	spin_lock_bh(&conn->calls_lock);
	call = railfs_call_for(conn, id);
	if (!call) {
		spin_unlock_bh(&conn->calls_lock);
		kfree(payload);
		return 0;
	}

	if (call->want != type) {
		spin_unlock_bh(&conn->calls_lock);
		pr_err("railfs: wanted message %u, got %u\n", call->want, type);
		kfree(payload);
		return -EPROTO;
	}

	/* A second reply for one id would overwrite an answer the caller may
	 * already be reading, and leak the first payload.
	 */
	if (call->answered) {
		spin_unlock_bh(&conn->calls_lock);
		pr_err("railfs: a second reply for request %llu\n", id);
		kfree(payload);
		return -EPROTO;
	}

	call->payload = payload;
	call->payload_len = len;
	call->err = 0;
	call->answered = true;
	complete(&call->replied);
	spin_unlock_bh(&conn->calls_lock);
	return 0;
}

// A reply landed in this call's slot. Checked here so the caller only reads a
// frame that is whole and its own; nonzero means the rail strands and the
// waiter kills the connection.
static int railfs_on_ring_reply(void *ctx, u32 line, u32 slot, int err)
{
	struct railfs_conn *conn = ctx;
	struct railfs_call *call;
	u8 *payload = NULL;
	u32 len = 0;
	u64 id = 0;
	u16 type = 0;

	spin_lock(&conn->calls_lock);
	call = conn->owner[line][slot];
	if (!call || call->answered) {
		spin_unlock(&conn->calls_lock);
		return 0;
	}

	if (!err) {
		err = railfs_rdma_ctrl_reply(conn->rail, line, slot, &type, &payload, &len);
	}
	if (!err && len < 8) {
		err = -EBADMSG;
	}
	if (!err) {
		memcpy(&id, payload, 8);
		if (id != call->id) {
			pr_err_ratelimited("railfs: reply for request %llu landed in the slot of %llu\n", id, call->id);
			err = -EPROTO;
		}
	}
	if (!err && call->want != type) {
		pr_err_ratelimited("railfs: wanted message %u, got %u\n", call->want, type);
		err = -EPROTO;
	}

	if (err) {
		call->err = err;
		WRITE_ONCE(conn->ring_fault, err);
	} else {
		call->payload = payload;
		call->payload_len = len;
		call->borrowed = true;
		call->err = 0;
	}
	call->answered = true;
	complete(&call->replied);
	spin_unlock(&conn->calls_lock);
	return err;
}

// The request never reached the daemon, so no reply is coming.
static int railfs_on_ring_sent(void *ctx, u32 line, u32 slot, int err)
{
	struct railfs_conn *conn = ctx;
	struct railfs_call *call;

	spin_lock(&conn->calls_lock);
	call = conn->owner[line][slot];
	if (call && !call->answered) {
		call->err = err;
		call->answered = true;
		complete(&call->replied);
	}
	spin_unlock(&conn->calls_lock);
	return 0;
}

static int railfs_reader(void *arg)
{
	struct railfs_conn *conn = arg;
	unsigned int nofs = memalloc_nofs_save();
	int err = 0;

	while (!kthread_should_stop() && !READ_ONCE(conn->dead)) {
		u8 header[RAILFS_HEADER_SIZE];
		u8 *payload;
		u32 len;
		u16 type;

		err = recv_all(conn, conn->sock, header, sizeof(header));
		if (err) {
			break;
		}

		err = railfs_read_header(header, &type, &len);
		if (err) {
			break;
		}

		// After the handshake the socket carries nothing over the fabric.
		if (conn->rail) {
			pr_err("railfs: a control frame arrived on tcp after the handshake\n");
			railfs_trace_tcp_frames(1);
			err = -EPROTO;
			break;
		}

		payload = kmalloc(len ? len : 1, GFP_NOFS);
		if (!payload) {
			err = -ENOMEM;
			break;
		}

		err = len ? recv_all(conn, conn->sock, payload, len) : 0;
		if (err) {
			kfree(payload);
			break;
		}

		railfs_trace_tcp_frames(1);
		err = railfs_deliver(conn, type, payload, len);
		if (err) {
			break;
		}
	}

	railfs_conn_kill(conn, err ? err : -ENOTCONN);
	memalloc_nofs_restore(nofs);
	return 0;
}

// A frame whose caller has left: read past it, so the stream stays in step.
static int railfs_drain_frame(struct railfs_conn *conn, u32 len)
{
	if (!conn->scratch) {
		conn->scratch = kvmalloc(RAILFS_PAGE_SIZE, GFP_NOFS);
		if (!conn->scratch) {
			return -ENOMEM;
		}
	}

	return recv_all(conn, conn->data, conn->scratch, len);
}

// The tcp data socket: key(8) | length(4) | payload, in whatever order the
// daemon answered. The bytes go straight into the buffer the call registered,
// outside the lock, with the call marked busy so its owner waits for them.
static int railfs_take_frame(struct railfs_conn *conn)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	struct railfs_call *call;
	u32 frame_len = 0;
	u64 key = 0;
	int err;

	err = recv_all(conn, conn->data, header, sizeof(header));
	if (err) {
		return err;
	}

	memcpy(&key, header, 8);
	memcpy(&frame_len, header + 8, 4);

	if (frame_len > RAILFS_PAGE_SIZE) {
		return -EMSGSIZE;
	}

	spin_lock_bh(&conn->calls_lock);
	call = railfs_call_for(conn, key);
	if (!call || !call->wants_data) {
		spin_unlock_bh(&conn->calls_lock);
		return railfs_drain_frame(conn, frame_len);
	}

	if (frame_len > call->room) {
		spin_unlock_bh(&conn->calls_lock);
		return -EMSGSIZE;
	}

	call->busy = true;
	spin_unlock_bh(&conn->calls_lock);

	err = recv_all(conn, conn->data, call->buf, frame_len);

	spin_lock_bh(&conn->calls_lock);
	call->busy = false;
	call->got = frame_len;
	call->data_err = err;
	call->delivered = !err;
	complete(&call->landed);
	complete(&call->released);
	spin_unlock_bh(&conn->calls_lock);
	return err;
}

static int railfs_data_reader(void *arg)
{
	struct railfs_conn *conn = arg;
	unsigned int nofs = memalloc_nofs_save();
	int err = 0;

	while (!kthread_should_stop() && !READ_ONCE(conn->dead)) {
		err = railfs_take_frame(conn);
		if (err) {
			break;
		}
	}

	railfs_conn_kill(conn, err ? err : -ENOTCONN);
	memalloc_nofs_restore(nofs);
	return 0;
}

// Every call on the connection fails with the same reason, the sockets are
// shut so the readers stop, and the fabric is flushed. Once only; a second
// caller finds it done.
static void railfs_conn_kill(struct railfs_conn *conn, int why)
{
	struct railfs_call *call;

	spin_lock_bh(&conn->calls_lock);
	if (READ_ONCE(conn->dead)) {
		spin_unlock_bh(&conn->calls_lock);
		return;
	}

	WRITE_ONCE(conn->dead, true);
	conn->why = why;

	/* Only what is still outstanding is failed. A call already answered keeps
	 * its answer: the peer did that work, and failing it here would report an
	 * error for an operation that happened.
	 */
	list_for_each_entry(call, &conn->calls, link) {
		if (!call->answered) {
			call->err = why;
		}
		if (!call->delivered) {
			call->data_err = why;
		}
		complete(&call->replied);
		complete(&call->landed);
	}
	spin_unlock_bh(&conn->calls_lock);

	if (conn->sock) {
		kernel_sock_shutdown(conn->sock, SHUT_RDWR);
	}
	if (conn->data) {
		kernel_sock_shutdown(conn->data, SHUT_RDWR);
	}

	railfs_rdma_break(conn->rail);
	wake_up_all(&conn->room);
}

// How many transfers the connection carries at once, which is what the daemon
// pooled pages for. Waits rather than fails: the caller is a filesystem
// operation with nowhere else to go.
static int railfs_admit(struct railfs_conn *conn)
{
	u64 mark = railfs_now();

	for (;;) {
		if (READ_ONCE(conn->dead)) {
			return -ENOTCONN;
		}

		if (atomic_add_unless(&conn->transfers, 1, RAILFS_CONN_DEPTH)) {
			railfs_trace_add(RAILFS_PHASE_POOL_WAIT, mark, 0);
			return 0;
		}

		if (wait_event_killable(conn->room, READ_ONCE(conn->dead) || atomic_read(&conn->transfers) < RAILFS_CONN_DEPTH)) {
			return -ERESTARTSYS;
		}
	}
}

static void railfs_leave(struct railfs_conn *conn)
{
	atomic_dec(&conn->transfers);
	wake_up(&conn->room);
}

// A landing buffer for a tcp read, one per admitted transfer, so an admitted
// caller always finds one. Allocated the first time it is needed.
static int railfs_landing_take(struct railfs_conn *conn, u32 *at)
{
	u32 i;

	for (;;) {
		i = find_first_bit(conn->landing_free, RAILFS_CONN_DEPTH);
		if (i >= RAILFS_CONN_DEPTH) {
			return -ENOBUFS;
		}
		if (test_and_clear_bit(i, conn->landing_free)) {
			break;
		}
	}

	if (!conn->landing[i]) {
		conn->landing[i] = kvmalloc(RAILFS_PAGE_SIZE, GFP_NOFS);
		if (!conn->landing[i]) {
			set_bit(i, conn->landing_free);
			return -ENOMEM;
		}
	}

	*at = i;
	return 0;
}

static void railfs_landing_give(struct railfs_conn *conn, u32 at)
{
	set_bit(at, conn->landing_free);
}

static int railfs_reply_for(struct railfs_conn *conn, struct railfs_cursor *c, u64 id)
{
	u64 replied = 0;
	int err = railfs_get_u64(c, &replied);

	if (err) {
		return err;
	}

	if (replied == id) {
		return 0;
	}

	pr_err("railfs: reply id %llu does not match request %llu\n", replied, id);
	railfs_conn_kill(conn, -EPROTO);
	return -EPROTO;
}

// One request, one reply, with the reply's payload handed to the caller.
static int exchange(struct railfs_conn *conn, u8 *frame, size_t frame_len, u64 id, u16 want, u8 **payload, u32 *payload_len)
{
	struct railfs_call call;
	int err;

	err = railfs_call_begin(conn, &call, id, want, NULL, 0);
	if (err) {
		return err;
	}

	err = railfs_send(conn, &call, frame, frame_len);
	if (!err) {
		err = railfs_call_wait(conn, &call);
	}

	if (!err && call.borrowed) {
		// The ring bytes go with the call; the caller keeps them longer.
		*payload = kmemdup(call.payload, call.payload_len ? call.payload_len : 1, GFP_NOFS);
		*payload_len = call.payload_len;
		if (!*payload) {
			err = -ENOMEM;
		}
	} else if (!err) {
		*payload = call.payload;
		*payload_len = call.payload_len;
		call.payload = NULL;
	}

	railfs_call_end(conn, &call);
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

	err = exchange(conn, frame, c.at, id, RAILFS_MSG_STATFS_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_reply_for(conn, &c, id);
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

	err = exchange(conn, frame, c.at, id, RAILFS_MSG_STAT_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	err = railfs_reply_for(conn, &c, id);
	if (err) {
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
static int railfs_read_reply(struct railfs_conn *conn, struct railfs_cursor *c, u64 id, u32 *reply_len, u64 *file_size, u8 *ok)
{
	int err;

	err = railfs_reply_for(conn, c, id);
	if (!err) {
		err = railfs_get_u32(c, reply_len);
	}
	if (!err) {
		err = railfs_get_u64(c, file_size);
	}
	if (!err) {
		err = railfs_get_u8(c, ok);
	}
	return err;
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

// Where a read's bytes are waiting: a fabric slot or a tcp landing buffer,
// either of which the caller gives back once it has copied them out.
struct railfs_read_place {
	u32 slot;
	bool taken;
};

static void railfs_read_place_give(struct railfs_conn *conn, struct railfs_read_place *place)
{
	if (!place->taken) {
		return;
	}

	if (conn->rail) {
		railfs_rdma_slot_release(conn->rail, place->slot);
	} else {
		railfs_landing_give(conn, place->slot);
	}
	place->taken = false;
}

void railfs_read_release(struct railfs_landed *landed)
{
	struct railfs_read_place place = { .slot = landed->slot, .taken = true };

	if (!landed->conn) {
		return;
	}

	railfs_read_place_give(landed->conn, &place);
	railfs_leave(landed->conn);
	landed->conn = NULL;
	landed->at = NULL;
}

static void railfs_abandon_or_kill(struct railfs_conn *conn, int err, bool must_kill)
{
	if (err == -ERESTARTSYS && !must_kill) {
		return;
	}

	railfs_conn_kill(conn, err == -ERESTARTSYS ? -EINTR : err);
}

// A page is offered before the request goes out, except a gpu page, which
// waits for the reply to show the size still holds: the peer pads past end of
// file, and that must not land in the caller's buffer.
static int railfs_read_core(struct railfs_conn *conn, const char *path, u64 offset, void **buf, struct sg_table *gpu, u32 len,
			    struct railfs_landed *landed)
{
	struct railfs_read_place place = {};
	struct railfs_call call;
	struct railfs_cursor c;
	void *given = *buf;
	u8 *frame = NULL;
	u32 reply_len = 0;
	u32 frame_len = 0;
	u64 file_size = 0;
	u64 wire = railfs_now();
	u64 digest;
	u64 ctl;
	u64 pull;
	size_t cap;
	u64 id;
	u8 ok = 0;
	bool admitted = false;
	bool offered = false;
	bool ended = false;
	bool began = false;
	int err;

	if (len > RAILFS_PAGE_SIZE) {
		len = RAILFS_PAGE_SIZE;
	}

	if (!conn->data && !conn->rail) {
		return -EOPNOTSUPP;
	}

	if (gpu && !railfs_gpu_allowed(conn)) {
		return -EOPNOTSUPP;
	}

	if (!len) {
		return 0;
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

	err = railfs_admit(conn);
	if (err) {
		goto out;
	}
	admitted = true;

	if (conn->rail && !gpu) {
		err = railfs_rdma_fetch_begin(conn->rail, id, len, NULL, &place.slot);
		if (err) {
			railfs_abandon_or_kill(conn, err, false);
			goto out;
		}
		place.taken = true;
		offered = true;
	} else if (!conn->rail && !*buf) {
		err = railfs_landing_take(conn, &place.slot);
		if (err) {
			goto out;
		}
		place.taken = true;
		*buf = conn->landing[place.slot];
	}

	err = railfs_call_begin(conn, &call, id, RAILFS_MSG_TRANSFER_REPLY, conn->rail ? NULL : *buf, len);
	if (err) {
		goto out;
	}
	began = true;

	ctl = railfs_now();

	err = railfs_send(conn, &call, frame, c.at);
	if (err) {
		goto out;
	}

	err = railfs_call_wait(conn, &call);
	if (err) {
		railfs_abandon_or_kill(conn, err, gpu != NULL);
		goto out;
	}

	railfs_trace_add(RAILFS_PHASE_READ_CTL, ctl, len);

	c.buf = call.payload;
	c.len = call.payload_len;
	c.at = 0;

	err = railfs_read_reply(conn, &c, id, &reply_len, &file_size, &ok);
	if (err) {
		goto out;
	}

	if (gpu) {
		if (!ok || reply_len != len) {
			railfs_conn_kill(conn, -ESTALE);
			err = ok && !reply_len ? 0 : -ESTALE;
			goto out;
		}

		err = railfs_rdma_fetch_begin(conn->rail, id, len, gpu, &place.slot);
		if (err) {
			railfs_conn_kill(conn, err);
			goto out;
		}
		place.taken = true;
		offered = true;
	}

	pull = railfs_now();
	if (conn->rail) {
		const void *at = NULL;

		err = railfs_rdma_fetch_end(conn->rail, place.slot, &at);
		ended = true;
		if (!err && !gpu) {
			if (given) {
				memcpy(given, at, len);
			} else {
				*buf = (void *)at;
			}
		}
		frame_len = len;
	} else {
		err = railfs_call_wait_data(conn, &call);
		frame_len = call.got;
	}
	railfs_trace_add(RAILFS_PHASE_READ_PULL, pull, len);
	if (err) {
		railfs_abandon_or_kill(conn, err, gpu != NULL);
		goto out;
	}

	if (!ok) {
		err = -EIO;
		goto out;
	}

	err = reply_len < frame_len ? reply_len : frame_len;
	railfs_trace_add(RAILFS_PHASE_READ_WIRE, wire, frame_len);

	digest = railfs_now();
	if (conn->verify && !gpu && railfs_matches_digest(&c, call.payload, *buf, frame_len, path, offset)) {
		err = -EBADMSG;
	}
	railfs_trace_add(RAILFS_PHASE_READ_DIGEST, digest, frame_len);

	if (err > 0 && landed) {
		landed->conn = conn;
		landed->at = *buf;
		landed->slot = place.slot;
		place.taken = false;
		admitted = false;
	}
out:
	// An offered page is still coming. A caller that gave up on it leaves the
	// slot to the landing; anyone else waits, so the slot is never handed on
	// with the peer about to write into it.
	if (offered && !ended) {
		if (err == -ERESTARTSYS && !gpu) {
			railfs_rdma_slot_orphan(conn->rail, place.slot);
			place.taken = false;
		} else {
			const void *at = NULL;

			railfs_rdma_fetch_end(conn->rail, place.slot, &at);
		}
	}
	if (began) {
		railfs_call_end(conn, &call);
	}
	railfs_read_place_give(conn, &place);
	if (admitted) {
		railfs_leave(conn);
	}
	kfree(frame);
	return err;
}

int railfs_read(struct railfs_conn *conn, const char *path, u64 offset, void *buf, u32 len)
{
	void *into = buf;

	return railfs_read_core(conn, path, offset, &into, NULL, len, NULL);
}

int railfs_read_landed(struct railfs_conn *conn, const char *path, u64 offset, u32 len, struct railfs_landed *landed)
{
	void *into = NULL;

	landed->conn = NULL;
	landed->at = NULL;
	return railfs_read_core(conn, path, offset, &into, NULL, len, landed);
}

int railfs_read_sg(struct railfs_conn *conn, const char *path, u64 offset, struct sg_table *pages, u32 len)
{
	void *into = NULL;

	return railfs_read_core(conn, path, offset, &into, pages, len, NULL);
}

// The body of a listing, wherever it landed.
static int railfs_parse_listing(struct railfs_conn *conn, struct railfs_cursor *c, u64 id, u32 payload_len, struct railfs_dirent **out,
				u32 *count)
{
	struct railfs_dirent *entries = NULL;
	u32 n = 0;
	u32 i;
	u8 found = 0;
	int err;

	err = railfs_reply_for(conn, c, id);
	if (!err) {
		err = railfs_get_u8(c, &found);
	}
	if (!err) {
		err = railfs_get_u32(c, &n);
	}
	if (err) {
		return err;
	}

	if (!found) {
		return -ENOENT;
	}

	// Smallest an entry can be: a four byte name length and the attributes,
	// and the directory and its parent follow them. Without this a count off
	// the wire asks for an allocation of any size.
	if ((u64)n * 54 > payload_len) {
		return -EBADMSG;
	}

	entries = kcalloc(n ? n : 1, sizeof(*entries), GFP_NOFS);
	if (!entries) {
		return -ENOMEM;
	}

	for (i = 0; i < n; i++) {
		err = railfs_get_str(c, &entries[i].name);
		if (!err) {
			err = railfs_get_attrs(c, &entries[i].attrs);
		}
		if (err) {
			railfs_free_dirents(entries, n);
			return err;
		}
	}

	*out = entries;
	*count = n;
	return 0;
}

// A listing over the fabric arrives like a read: the encoded reply lands in
// an offered page and a TransferReply carries its length and digest. Follows
// railfs_read_core, including its care over a page still coming.
static int railfs_list_through_page(struct railfs_conn *conn, u8 *frame, size_t frame_len, u64 id, const char *path, struct railfs_dirent **out,
				    u32 *count)
{
	struct railfs_call call;
	struct railfs_cursor c;
	const void *at = NULL;
	u32 slot = 0;
	u32 reply_len = 0;
	u64 file_size = 0;
	u8 ok = 0;
	bool began = false;
	bool offered = false;
	bool ended = false;
	int err;

	err = railfs_admit(conn);
	if (err) {
		return err;
	}

	err = railfs_rdma_fetch_begin(conn->rail, id, RAILFS_PAGE_SIZE, NULL, &slot);
	if (err) {
		railfs_abandon_or_kill(conn, err, false);
		goto leave;
	}
	offered = true;

	err = railfs_call_begin(conn, &call, id, RAILFS_MSG_TRANSFER_REPLY, NULL, 0);
	if (err) {
		goto out;
	}
	began = true;

	err = railfs_send(conn, &call, frame, frame_len);
	if (err) {
		goto out;
	}

	err = railfs_call_wait(conn, &call);
	if (err) {
		railfs_abandon_or_kill(conn, err, false);
		goto out;
	}

	c.buf = call.payload;
	c.len = call.payload_len;
	c.at = 0;

	err = railfs_read_reply(conn, &c, id, &reply_len, &file_size, &ok);
	if (err) {
		goto out;
	}

	err = railfs_rdma_fetch_end(conn->rail, slot, &at);
	ended = true;
	if (err) {
		railfs_abandon_or_kill(conn, err, false);
		goto out;
	}

	if (!ok) {
		err = -EIO;
		goto out;
	}
	if (reply_len > RAILFS_PAGE_SIZE) {
		err = -EMSGSIZE;
		goto out;
	}

	if (conn->verify && railfs_matches_digest(&c, call.payload, at, reply_len, path, 0)) {
		err = -EBADMSG;
		goto out;
	}

	c.buf = (u8 *)at;
	c.len = reply_len;
	c.at = 0;
	err = railfs_parse_listing(conn, &c, id, reply_len, out, count);
out:
	if (offered && !ended) {
		if (err == -ERESTARTSYS) {
			railfs_rdma_slot_orphan(conn->rail, slot);
			offered = false;
		} else {
			railfs_rdma_fetch_end(conn->rail, slot, &at);
		}
	}
	if (began) {
		railfs_call_end(conn, &call);
	}
	if (offered) {
		railfs_rdma_slot_release(conn->rail, slot);
	}
leave:
	railfs_leave(conn);
	return err;
}

int railfs_list(struct railfs_conn *conn, const char *path, struct railfs_dirent **out, u32 *count)
{
	struct railfs_cursor c;
	u8 *payload = NULL;
	u8 *frame = NULL;
	u32 payload_len = 0;
	size_t cap;
	u64 id;
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

	if (conn->rail) {
		err = railfs_list_through_page(conn, frame, c.at, id, path, out, count);
		goto out;
	}

	err = exchange(conn, frame, c.at, id, RAILFS_MSG_LIST_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;
	err = railfs_parse_listing(conn, &c, id, payload_len, out, count);
out:
	kfree(payload);
	kfree(frame);
	return err;
}

// The bytes out on the tcp data socket, header and payload as one frame. A
// fabric connection maps the folio and sends it where it lies; this one has
// to walk it into the socket, and kmap gives it a linear view to do that from.
static int railfs_give_payload_folios(struct railfs_conn *conn, u64 id, struct folio **folios, unsigned int nr, u32 len)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	u32 left = len;
	unsigned int i;
	int err;

	memcpy(header, &id, 8);
	memcpy(header + 8, &len, 4);

	mutex_lock(&conn->data_lock);

	err = send_all(conn->data, header, sizeof(header));
	if (err) {
		goto out;
	}

	for (i = 0; i < nr && left; i++) {
		// One mapping per folio, which holds because this builds for kernels
		// without highmem. On one that had it, this would see a page.
		u32 bytes = min_t(u32, left, (u32)folio_size(folios[i]));
		void *at = kmap_local_folio(folios[i], 0);

		err = send_all(conn->data, at, bytes);
		kunmap_local(at);
		if (err) {
			goto out;
		}
		left -= bytes;
	}

	err = left ? -EMSGSIZE : 0;
out:
	mutex_unlock(&conn->data_lock);
	return err;
}

static int railfs_give_payload(struct railfs_conn *conn, u64 id, const void *buf, u32 len)
{
	u8 header[RAILFS_DATA_HEADER_SIZE];
	int err;

	memcpy(header, &id, 8);
	memcpy(header + 8, &len, 4);

	mutex_lock(&conn->data_lock);
	err = send_all(conn->data, header, sizeof(header));
	if (!err) {
		err = send_all(conn->data, buf, len);
	}
	mutex_unlock(&conn->data_lock);
	return err;
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
	u32 code = 0;
	size_t cap;
	u64 id;
	u8 ok = 0;
	int err;

	cap = RAILFS_HEADER_SIZE + 8 + 2 + 4 + strlen(req->path) + 4 + strlen(target) + 8 + 4 + 8 + 8 + 1;

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
	/* This mount never asks for an exclusive create. */
	railfs_put_u8(&c, 0);

	if (!railfs_cursor_ok(&c)) {
		err = -EOVERFLOW;
		goto out;
	}

	railfs_frame(frame, RAILFS_MSG_META, (u32)(c.at - RAILFS_HEADER_SIZE));

	err = exchange(conn, frame, c.at, id, RAILFS_MSG_META_REPLY, &payload, &payload_len);
	if (err) {
		goto out;
	}

	c.buf = payload;
	c.len = payload_len;
	c.at = 0;

	// The daemon says why in the reply, and readlink says what it found in the
	// same field a refusal leaves empty. Without reading both, every refusal
	// reaches the caller as EIO and a link has no target.
	err = railfs_reply_for(conn, &c, id);
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
	if (!err) {
		struct railfs_attrs attrs = {};

		err = railfs_get_attrs(&c, &attrs);
		if (!err && req->made) {
			*req->made = attrs;
		}
	}
	if (err) {
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

// The push is registered before the request goes out, so the clear-to-send
// the daemon answers with always finds it.
static int railfs_write_from(struct railfs_conn *conn, const char *path, u64 offset, const void *buf, struct folio **folios,
			     unsigned int nr, struct sg_table *gpu, u32 len, bool truncate)
{
	u8 digest[RAILFS_DIGEST_SIZE] = {};
	struct railfs_push *push = NULL;
	struct railfs_call call;
	struct railfs_cursor c;
	u8 *frame = NULL;
	u32 reply_len = 0;
	u64 file_size = 0;
	u64 mark;
	u64 wire;
	size_t cap;
	u64 id;
	u8 ok = 0;
	bool admitted = false;
	bool began = false;
	int err;

	if (len > RAILFS_PAGE_SIZE) {
		len = RAILFS_PAGE_SIZE;
	}

	if (!conn->data && !conn->rail) {
		return -EOPNOTSUPP;
	}

	if (gpu && !railfs_gpu_allowed(conn)) {
		return -EOPNOTSUPP;
	}

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

	err = railfs_admit(conn);
	if (err) {
		goto out;
	}
	admitted = true;

	if (conn->rail) {
		push = railfs_rdma_expect(conn->rail, id);
		if (!push) {
			err = -ENOMEM;
			goto out;
		}
	}

	err = railfs_call_begin(conn, &call, id, RAILFS_MSG_TRANSFER_REPLY, NULL, 0);
	if (err) {
		goto out;
	}
	began = true;

	wire = railfs_now();

	err = railfs_send(conn, &call, frame, c.at);
	if (err) {
		goto out;
	}

	if (conn->rail) {
		if (folios) {
			err = railfs_rdma_push_folios(conn->rail, push, folios, nr, len);
		} else if (gpu) {
			err = railfs_rdma_push_sg(conn->rail, push, gpu, len);
		} else {
			err = railfs_rdma_push(conn->rail, push, buf, len);
		}
		err = err < 0 ? err : 0;
	} else if (gpu) {
		err = -EOPNOTSUPP;
	} else if (folios) {
		err = railfs_give_payload_folios(conn, id, folios, nr, len);
	} else {
		err = railfs_give_payload(conn, id, buf, len);
	}
	if (err) {
		railfs_conn_kill(conn, err);
		goto out;
	}

	err = railfs_call_wait(conn, &call);
	if (err) {
		railfs_abandon_or_kill(conn, err, false);
		goto out;
	}

	c.buf = call.payload;
	c.len = call.payload_len;
	c.at = 0;

	err = railfs_reply_for(conn, &c, id);
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
		goto out;
	}

	err = ok ? (int)reply_len : -EIO;
	railfs_trace_add(RAILFS_PHASE_WRITE_WIRE, wire, len);
out:
	if (push) {
		railfs_rdma_forget(conn->rail, push);
	}
	if (began) {
		railfs_call_end(conn, &call);
	}
	if (admitted) {
		railfs_leave(conn);
	}
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

	railfs_set_deadlines(conn->data);

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

// Stopped rather than left: a reader blocked in a receive is woken by the
// shutdown the kill did, and the task is pinned so stopping one that has
// already left is safe.
static void railfs_stop_reader(struct task_struct **reader)
{
	if (!*reader) {
		return;
	}

	kthread_stop(*reader);
	put_task_struct(*reader);
	*reader = NULL;
}

static struct task_struct *railfs_start_reader(int (*run)(void *), struct railfs_conn *conn, const char *name)
{
	struct task_struct *task = kthread_create(run, conn, "%s", name);

	if (IS_ERR(task)) {
		return task;
	}

	get_task_struct(task);
	wake_up_process(task);
	return task;
}

static void railfs_conn_free(struct railfs_conn *conn)
{
	u32 i;

	if (!conn) {
		return;
	}

	railfs_conn_kill(conn, -ENOTCONN);
	railfs_stop_reader(&conn->reader);
	railfs_stop_reader(&conn->data_reader);
	railfs_rdma_close(conn->rail);

	for (i = 0; i < RAILFS_CONN_DEPTH; i++) {
		kvfree(conn->landing[i]);
	}
	kvfree(conn->scratch);

	if (conn->data) {
		sock_release(conn->data);
	}
	if (conn->sock) {
		sock_release(conn->sock);
	}
	kfree(conn);
}

static void railfs_conn_release(struct kref *ref)
{
	railfs_conn_free(container_of(ref, struct railfs_conn, ref));
}

void railfs_conn_put(struct railfs_conn *conn)
{
	if (conn) {
		kref_put(&conn->ref, railfs_conn_release);
	}
}

// The daemon's last frame on the socket: its queue pairs are ready. Before it
// a request could hit a queue pair still being driven to ready.
static int await_ready(struct socket *sock)
{
	u8 header[RAILFS_HEADER_SIZE];
	u32 len;
	u16 type;
	int err;

	err = recv_now(sock, header, sizeof(header));
	if (err) {
		return err;
	}

	err = railfs_read_header(header, &type, &len);
	if (err) {
		return err;
	}

	if (type != RAILFS_MSG_READY || len != 0) {
		pr_err("railfs: expected Ready, got message type %u\n", type);
		return -EPROTO;
	}
	return 0;
}

struct railfs_conn *railfs_connect(const struct railfs_peer *peer)
{
	struct sockaddr_in addr = {};
	struct railfs_conn *conn = NULL;
	struct railfs_wire mine = {};
	struct task_struct *task;
	char *endpoint = NULL;
	u32 endpoint_len = 0;
	__be32 ip;
	int err;

	if (!peer->host) {
		err = -EINVAL;
		goto fail;
	}

	// in_aton does not reject a name, it invents an address from one, so a
	// mount naming a host rather than an address would dial somewhere
	// arbitrary instead of failing.
	if (!in4_pton(peer->host, -1, (u8 *)&ip, -1, NULL)) {
		pr_err("railfs: host=%s is not an IPv4 address\n", peer->host);
		err = -EINVAL;
		goto fail;
	}

	conn = kzalloc(sizeof(*conn), GFP_NOFS);
	if (!conn) {
		err = -ENOMEM;
		goto fail;
	}

	kref_init(&conn->ref);
	mutex_init(&conn->send_lock);
	mutex_init(&conn->data_lock);
	spin_lock_init(&conn->calls_lock);
	INIT_LIST_HEAD(&conn->calls);
	init_waitqueue_head(&conn->room);
	bitmap_fill(conn->landing_free, RAILFS_CONN_DEPTH);

	err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &conn->sock);
	if (err) {
		goto fail;
	}

	// Without a deadline a peer that stops answering leaves a sender asleep
	// holding the send lock, and umount blocks behind it.
	railfs_set_deadlines(conn->sock);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = htons(peer->port);

	err = kernel_connect(conn->sock, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (err) {
		pr_err_ratelimited("railfs: connect to %s:%u failed: %d\n", peer->host, peer->port, err);
		goto fail;
	}

	if (peer->rdma) {
		conn->rail = railfs_rdma_open(&mine);
		if (IS_ERR(conn->rail)) {
			err = PTR_ERR(conn->rail);
			conn->rail = NULL;
			goto fail;
		}
		railfs_rdma_ctrl_watch(conn->rail, railfs_on_ring_reply, railfs_on_ring_sent, conn);
	}

	conn->verify = peer->verify;

	err = say_hello(conn->sock, peer->rdma, peer->verify, &endpoint, &endpoint_len);
	if (err) {
		goto fail;
	}

	err = peer->rdma ? join_fabric(conn, endpoint, endpoint_len, &mine) : join_data_channel(conn, endpoint);
	if (err) {
		goto fail;
	}

	if (peer->rdma) {
		err = await_ready(conn->sock);
		if (err) {
			goto fail;
		}
	}

	task = railfs_start_reader(railfs_reader, conn, "railfs-rx");
	if (IS_ERR(task)) {
		err = PTR_ERR(task);
		goto fail;
	}
	conn->reader = task;

	if (conn->data) {
		task = railfs_start_reader(railfs_data_reader, conn, "railfs-rxd");
		if (IS_ERR(task)) {
			err = PTR_ERR(task);
			goto fail;
		}
		conn->data_reader = task;
	}

	kfree(endpoint);
	return conn;

fail:
	kfree(endpoint);
	railfs_conn_free(conn);
	return ERR_PTR(err);
}

struct railfs_pool *railfs_pool_open(const struct railfs_peer *peer, unsigned int count)
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
	mutex_init(&pool->revive_lock);

	pool->peer = *peer;
	pool->peer.host = kstrdup(peer->host, GFP_NOFS);
	if (!pool->peer.host) {
		err = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < count; i++) {
		struct railfs_conn *conn = railfs_connect(&pool->peer);

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
		railfs_conn_put(pool->conns[i]);
	}

	kfree(pool->peer.host);
	kfree(pool);
}

static bool railfs_conn_closed(const struct railfs_conn *conn)
{
	return conn->sock->sk->sk_state != TCP_ESTABLISHED || (conn->data && conn->data->sk->sk_state != TCP_ESTABLISHED);
}

static bool railfs_conn_unusable(const struct railfs_conn *conn)
{
	return READ_ONCE(conn->dead) || railfs_conn_closed(conn);
}

// A fresh connection in the dead one's place. Whoever still holds the old one
// finishes failing on it and frees it with their last reference.
static void railfs_pool_revive(struct railfs_pool *pool, unsigned int at)
{
	struct railfs_conn *old;
	struct railfs_conn *fresh;
	unsigned int nofs;

	mutex_lock(&pool->revive_lock);

	// Whoever got here first may already have replaced it.
	old = pool->conns[at];
	if (!railfs_conn_unusable(old)) {
		goto out;
	}

	railfs_conn_kill(old, -ENOTCONN);

	if (pool->retried[at] && time_before(jiffies, pool->retried[at] + RAILFS_REVIVE_INTERVAL)) {
		goto out;
	}

	nofs = memalloc_nofs_save();
	fresh = railfs_connect(&pool->peer);
	memalloc_nofs_restore(nofs);
	pool->retried[at] = jiffies;
	if (IS_ERR(fresh)) {
		goto out;
	}

	spin_lock(&pool->lock);
	pool->conns[at] = fresh;
	spin_unlock(&pool->lock);

	railfs_conn_put(old);
	pr_info_ratelimited("railfs: connection %u to %s reopened\n", at, pool->peer.host);
out:
	mutex_unlock(&pool->revive_lock);
}

// Under the pool lock. The slot asked for, unless it is dead and another in
// the window is not: a slot inside its revive interval stays dead for seconds,
// and a caller sent there would fail while the rest answer. The search keeps
// to the window, so a file's writeback never reaches past its span.
static struct railfs_conn *railfs_pool_pick(struct railfs_pool *pool, unsigned int base, unsigned int span,
					    unsigned int step)
{
	unsigned int i;

	for (i = 0; i < span; i++) {
		struct railfs_conn *conn = pool->conns[(base + (step + i) % span) % pool->count];

		if (!railfs_conn_unusable(conn)) {
			return conn;
		}
	}

	return pool->conns[(base + step) % pool->count];
}

static void railfs_conn_used(struct railfs_conn *conn)
{
	bool first;

	spin_lock_bh(&conn->calls_lock);
	first = conn->users++ == 0;
	spin_unlock_bh(&conn->calls_lock);

	if (first) {
		railfs_trace_busy(1);
	}
}

static void railfs_conn_unused(struct railfs_conn *conn)
{
	bool last;

	spin_lock_bh(&conn->calls_lock);
	last = --conn->users == 0;
	spin_unlock_bh(&conn->calls_lock);

	if (last) {
		railfs_trace_busy(-1);
	}
}

struct railfs_conn *railfs_pool_take_near(struct railfs_pool *pool, unsigned int hint, unsigned int span)
{
	unsigned int turn = (unsigned int)atomic_inc_return(&pool->next);
	struct railfs_conn *conn;
	unsigned int base, step, at;

	if (span < 1 || span >= pool->count) {
		span = pool->count;
		hint = 0;
	}
	base = hint % pool->count;
	step = turn % span;
	at = (base + step) % pool->count;

	spin_lock(&pool->lock);
	if (railfs_conn_unusable(pool->conns[at])) {
		spin_unlock(&pool->lock);
		railfs_pool_revive(pool, at);
		spin_lock(&pool->lock);
	}
	conn = railfs_pool_pick(pool, base, span, step);
	kref_get(&conn->ref);
	spin_unlock(&pool->lock);

	railfs_conn_used(conn);
	return conn;
}

struct railfs_conn *railfs_pool_take(struct railfs_pool *pool)
{
	return railfs_pool_take_near(pool, 0, pool->count);
}

void railfs_pool_give(struct railfs_pool *pool, struct railfs_conn *conn)
{
	railfs_conn_unused(conn);
	railfs_conn_put(conn);
}

static bool railfs_any_loss(int err) { return true; }

static bool railfs_never_sent(int err) { return err == -ENOTCONN; }

static int railfs_pool_run(struct railfs_pool *pool, unsigned int hint, unsigned int span, railfs_wire_op op, void *arg,
			   bool (*worth_retrying)(int err))
{
	unsigned int attempt;
	int first = 0;
	int err = -ENOTCONN;

	for (attempt = 0; attempt < RAILFS_WIRE_ATTEMPTS; attempt++) {
		struct railfs_conn *conn = railfs_pool_take_near(pool, hint, span);
		bool lost;

		err = op(conn, arg);
		lost = READ_ONCE(conn->dead);
		railfs_pool_give(pool, conn);

		if (err >= 0 || !lost || !worth_retrying(err)) {
			return err;
		}

		if (!first) {
			first = err;
		}
	}

	return err == -ENOTCONN ? first : err;
}

int railfs_pool_call_near(struct railfs_pool *pool, unsigned int hint, unsigned int span, railfs_wire_op op, void *arg)
{
	return railfs_pool_run(pool, hint, span, op, arg, railfs_any_loss);
}

int railfs_pool_call(struct railfs_pool *pool, railfs_wire_op op, void *arg)
{
	return railfs_pool_run(pool, 0, pool->count, op, arg, railfs_any_loss);
}

int railfs_pool_apply(struct railfs_pool *pool, railfs_wire_op op, void *arg)
{
	return railfs_pool_run(pool, 0, pool->count, op, arg, railfs_never_sent);
}

struct railfs_stat_req {
	const char *path;
	struct railfs_attrs *out;
	bool *found;
};

static int railfs_stat_op(struct railfs_conn *conn, void *arg)
{
	struct railfs_stat_req *req = arg;

	return railfs_stat(conn, req->path, req->out, req->found);
}

int railfs_pool_stat(struct railfs_pool *pool, const char *path, struct railfs_attrs *out, bool *found)
{
	struct railfs_stat_req req = { .path = path, .out = out, .found = found };

	return railfs_pool_call(pool, railfs_stat_op, &req);
}

struct railfs_list_req {
	const char *path;
	struct railfs_dirent **out;
	u32 *count;
};

static int railfs_list_op(struct railfs_conn *conn, void *arg)
{
	struct railfs_list_req *req = arg;

	return railfs_list(conn, req->path, req->out, req->count);
}

int railfs_pool_list(struct railfs_pool *pool, const char *path, struct railfs_dirent **out, u32 *count)
{
	struct railfs_list_req req = { .path = path, .out = out, .count = count };

	return railfs_pool_call(pool, railfs_list_op, &req);
}

struct railfs_space_req {
	const char *path;
	struct railfs_space *out;
};

static int railfs_space_op(struct railfs_conn *conn, void *arg)
{
	struct railfs_space_req *req = arg;

	return railfs_space_of(conn, req->path, req->out);
}

int railfs_pool_space_of(struct railfs_pool *pool, const char *path, struct railfs_space *out)
{
	struct railfs_space_req req = { .path = path, .out = out };

	return railfs_pool_call(pool, railfs_space_op, &req);
}

static int railfs_meta_op(struct railfs_conn *conn, void *arg) { return railfs_meta_send(conn, arg); }

int railfs_pool_meta_send(struct railfs_pool *pool, const struct railfs_meta_req *req)
{
	return railfs_pool_apply(pool, railfs_meta_op, (void *)req);
}

int railfs_pool_meta(struct railfs_pool *pool, u16 op, const char *path, u64 size)
{
	struct railfs_meta_req req = { .op = op, .path = path, .size = size };

	return railfs_pool_meta_send(pool, &req);
}

int railfs_pool_meta_to(struct railfs_pool *pool, u16 op, const char *path, const char *target, u64 size)
{
	struct railfs_meta_req req = { .op = op, .path = path, .target = target, .size = size };

	return railfs_pool_meta_send(pool, &req);
}

static int railfs_create_op(struct railfs_conn *conn, void *arg) { return railfs_create_file(conn, arg); }

int railfs_pool_create_file(struct railfs_pool *pool, const char *path)
{
	return railfs_pool_apply(pool, railfs_create_op, (void *)path);
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
