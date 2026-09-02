/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_MSG_H
#define RAILFS_MSG_H

/* What the wire says: the messages the daemon understands and the shapes it
 * sends back. How those become bytes is railfs-proto.h.
 */

#include <linux/types.h>

/* Mirrors include/rail/proto/message.h. A mismatch here is a desync the
 * daemon reports as "bad frame magic", so the two move together.
 */
#define RAILFS_WIRE_MAGIC 0x5241494cu /* "RAIL" */
#define RAILFS_WIRE_VERSION 1
#define RAILFS_HEADER_SIZE 10 /* magic(4) + type(2) + length(4) */
#define RAILFS_DATA_HEADER_SIZE 12 /* key(8) + length(4) */
#define RAILFS_DIGEST_SIZE 16

/* Which hash the session verifies with, matching enum class Sum in
 * include/rail/app/checksum.h. The kernel has xxh64 and not xxh3, so it asks
 * for the one it can compute rather than turning verification off.
 */
#define RAILFS_SUM_XXH3 1
#define RAILFS_SUM_XXH64 2

/* The daemon builds its pool from what Hello asks for and does not supply a
 * default, so zero here means a pool with no pages and a read that waits for
 * one forever.
 *
 * One megabyte, measured with the readahead window held large enough that the
 * page size is not also deciding how many fetches are in flight - which is
 * what made two look 59% better in an earlier round. Held that way, 1/2/4/8
 * MiB all land within 4.2/3.9/4.1/4.1 GB/s, and the smallest keeps the
 * per-connection dma landing region affordable at high connection counts.
 */
#define RAILFS_PAGE_SIZE (1u << 20)
#define RAILFS_PAGE_COUNT 4

#define RAILFS_META_MKDIR 1
#define RAILFS_META_UNLINK 2
#define RAILFS_META_RMDIR 3
#define RAILFS_META_RENAME 4
#define RAILFS_META_TRUNCATE 5
#define RAILFS_META_SETMODE 6
#define RAILFS_META_SETMTIME 7
#define RAILFS_META_FSYNC 8
#define RAILFS_META_SYMLINK 9
#define RAILFS_META_READLINK 10
#define RAILFS_META_HARDLINK 11

enum railfs_msg_type {
	RAILFS_MSG_HELLO = 1,
	RAILFS_MSG_HELLO_ACK = 2,
	RAILFS_MSG_PEER_ENDPOINT = 19,
	RAILFS_MSG_READ = 16,
	RAILFS_MSG_WRITE = 17,
	RAILFS_MSG_META = 24,
	RAILFS_MSG_META_REPLY = 25,
	RAILFS_MSG_TRANSFER_REPLY = 18,
	RAILFS_MSG_STATFS = 26,
	RAILFS_MSG_STATFS_REPLY = 27,
	RAILFS_MSG_STAT = 12,
	RAILFS_MSG_STAT_REPLY = 13,
	RAILFS_MSG_LIST = 14,
	RAILFS_MSG_LIST_REPLY = 15,
};

struct railfs_attrs {
	u64 size;
	u32 mode;
	s64 mtime;
	u8 directory;
	u8 link;
	u32 links;
};

struct railfs_space {
	u64 block_size;
	u64 blocks;
	u64 blocks_free;
	u64 files;
	u64 files_free;
};

struct railfs_dirent {
	char *name;
	struct railfs_attrs attrs;
};

#endif
