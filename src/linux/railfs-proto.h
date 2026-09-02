/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_PROTO_H
#define RAILFS_PROTO_H

/* How a message becomes bytes and back. A cursor walks one buffer and latches
 * an overrun, so a truncated reply is caught once at the end rather than at
 * every field.
 */

#include <linux/mm.h>
#include <linux/types.h>

#include "railfs-msg.h"

struct railfs_cursor {
	u8 *buf;
	size_t len;
	size_t at;
	bool overrun;
};

int railfs_cursor_ok(const struct railfs_cursor *c);
void railfs_put_u8(struct railfs_cursor *c, u8 v);
void railfs_put_u16(struct railfs_cursor *c, u16 v);
void railfs_put_u32(struct railfs_cursor *c, u32 v);
void railfs_put_u64(struct railfs_cursor *c, u64 v);
void railfs_put_str(struct railfs_cursor *c, const char *s);
void railfs_put_raw(struct railfs_cursor *c, const void *p, size_t n);

int railfs_get_u16(struct railfs_cursor *c, u16 *v);
int railfs_get_u32(struct railfs_cursor *c, u32 *v);
int railfs_get_u64(struct railfs_cursor *c, u64 *v);
int railfs_get_u8(struct railfs_cursor *c, u8 *v);
int railfs_get_str(struct railfs_cursor *c, char **out);
int railfs_get_attrs(struct railfs_cursor *c, struct railfs_attrs *a);
int railfs_skip(struct railfs_cursor *c, size_t n);

/* Writes the frame header over the first RAILFS_HEADER_SIZE bytes, which the
 * caller left room for.
 */
void railfs_frame(u8 *buf, u16 type, u32 payload_len);

/* The session digest of one buffer, in the sixteen byte field the wire uses. */
void railfs_digest(const void *data, size_t len, u8 *out);

/* The same digest over a run of folios, so a write need not be copied
 * anywhere just to be hashed.
 */
void railfs_digest_folios(struct folio **folios, unsigned int nr, size_t len, u8 *out);

#endif
