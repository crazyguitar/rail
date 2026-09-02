// SPDX-License-Identifier: GPL-2.0
//
// The rail control frame, in kernel C. Values are written in host order the
// way the C++ codec writes them, so this is little-endian on the machines that
// run it and would need byte-swapping before it ever ran anywhere else.

#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/xxhash.h>
#include <linux/string.h>

#include "railfs-proto.h"

// A truncated write used to be silent, which put a short frame on the wire and
// left the peer reading the next message from the middle of this one.
static void put_raw(struct railfs_cursor *c, const void *p, size_t n)
{
	if (n > c->len || c->at > c->len - n) {
		c->overrun = true;
		return;
	}
	memcpy(c->buf + c->at, p, n);
	c->at += n;
}

int railfs_cursor_ok(const struct railfs_cursor *c) { return !c->overrun; }

void railfs_put_raw(struct railfs_cursor *c, const void *p, size_t n) { put_raw(c, p, n); }

void railfs_put_u8(struct railfs_cursor *c, u8 v) { put_raw(c, &v, sizeof(v)); }
void railfs_put_u16(struct railfs_cursor *c, u16 v) { put_raw(c, &v, sizeof(v)); }
void railfs_put_u32(struct railfs_cursor *c, u32 v) { put_raw(c, &v, sizeof(v)); }
void railfs_put_u64(struct railfs_cursor *c, u64 v) { put_raw(c, &v, sizeof(v)); }

void railfs_put_str(struct railfs_cursor *c, const char *s)
{
	size_t n = s ? strlen(s) : 0;

	railfs_put_u32(c, (u32)n);
	if (n) {
		put_raw(c, s, n);
	}
}

static int get_raw(struct railfs_cursor *c, void *p, size_t n)
{
	if (n > c->len || c->at > c->len - n) {
		return -EBADMSG;
	}
	memcpy(p, c->buf + c->at, n);
	c->at += n;
	return 0;
}

int railfs_get_u8(struct railfs_cursor *c, u8 *v) { return get_raw(c, v, sizeof(*v)); }
int railfs_get_u16(struct railfs_cursor *c, u16 *v) { return get_raw(c, v, sizeof(*v)); }
int railfs_get_u64(struct railfs_cursor *c, u64 *v) { return get_raw(c, v, sizeof(*v)); }
int railfs_get_u32(struct railfs_cursor *c, u32 *v) { return get_raw(c, v, sizeof(*v)); }

int railfs_get_str(struct railfs_cursor *c, char **out)
{
	u32 n;
	int err = railfs_get_u32(c, &n);

	if (err) {
		return err;
	}
	if (n > c->len || c->at > c->len - n) {
		return -EBADMSG;
	}

	*out = kmalloc(n + 1, GFP_KERNEL);
	if (!*out) {
		return -ENOMEM;
	}

	memcpy(*out, c->buf + c->at, n);
	(*out)[n] = '\0';
	c->at += n;
	return 0;
}

int railfs_get_attrs(struct railfs_cursor *c, struct railfs_attrs *a)
{
	int err;

	err = railfs_get_u64(c, &a->size);
	if (!err) {
		err = railfs_get_u32(c, &a->mode);
	}
	if (!err) {
		err = get_raw(c, &a->mtime, sizeof(a->mtime));
	}
	if (!err) {
		err = railfs_get_u8(c, &a->directory);
	}
	if (!err) {
		err = railfs_get_u8(c, &a->link);
	}
	if (!err) {
		err = railfs_get_u32(c, &a->links);
	}
	return err;
}

int railfs_skip(struct railfs_cursor *c, size_t n)
{
	if (n > c->len || c->at > c->len - n) {
		return -EBADMSG;
	}
	c->at += n;
	return 0;
}

// Eight bytes of a sixteen byte field, the rest zero - the same shape the C++
// side writes for xxh64, so the two digests can be compared directly.
void railfs_digest(const void *data, size_t len, u8 *out)
{
	u64 sum = xxh64(data, len, 0);

	memset(out, 0, RAILFS_DIGEST_SIZE);
	memcpy(out, &sum, sizeof(sum));
}

void railfs_frame(u8 *buf, u16 type, u32 payload_len)
{
	u32 magic = RAILFS_WIRE_MAGIC;

	memcpy(buf, &magic, 4);
	memcpy(buf + 4, &type, 2);
	memcpy(buf + 6, &payload_len, 4);
}

void railfs_digest_folios(struct folio **folios, unsigned int nr, size_t len, u8 *out)
{
	struct xxh64_state state;
	size_t left = len;
	unsigned int i;
	u64 sum;

	xxh64_reset(&state, 0);

	for (i = 0; i < nr && left; i++) {
		size_t bytes = min_t(size_t, left, folio_size(folios[i]));
		void *at = kmap_local_folio(folios[i], 0);

		xxh64_update(&state, at, bytes);
		kunmap_local(at);
		left -= bytes;
	}

	sum = xxh64_digest(&state);

	memset(out, 0, RAILFS_DIGEST_SIZE);
	memcpy(out, &sum, sizeof(sum));
}
