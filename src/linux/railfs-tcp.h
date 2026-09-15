/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_TCP_H
#define RAILFS_TCP_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/net.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "railfs-compat.h"

#include "railfs-rdma.h"

struct railfs_peer {
	char *host;
	u16 port;
	bool rdma;
	bool verify;
};

/* One session to the daemon, shared by every caller on the mount: every
 * reply names its request, and a reader thread hands it to whoever asked.
 */
struct railfs_conn {
	struct kref ref;
	struct socket *sock;
	struct socket *data;
	struct railfs_rdma *rail;
	struct mutex send_lock;
	struct mutex data_lock;
	spinlock_t calls_lock;
	struct list_head calls;
	struct task_struct *reader;
	struct task_struct *data_reader;
	atomic64_t next_id;
	/* Transfers on the wire, bounded to what the daemon pooled pages for. */
	atomic_t transfers;
	wait_queue_head_t room;
	/* Where a tcp read lands, one buffer per transfer the connection admits. */
	void *landing[RAILFS_CONN_DEPTH];
	DECLARE_BITMAP(landing_free, RAILFS_CONN_DEPTH);
	void *scratch;
	unsigned int users;
	/* Whether the session agreed to hash what it carries. Kept here because a
	 * reply always has a digest field: unverified it is zeroed rather than
	 * absent, so the reader has to know not to look at it.
	 */
	bool verify;
	bool dead;
	int why;
	/* Which call holds each request slot, under calls_lock, for softirq. */
	struct railfs_call *owner[RAILFS_MAX_RAILS][RAILFS_CTRL_SLOTS];
	/* A bad reply seen in softirq; the waiting caller kills the connection. */
	int ring_fault;
};

#include "railfs-proto.h"

struct railfs_conn *railfs_connect(const struct railfs_peer *peer);
void railfs_conn_put(struct railfs_conn *conn);

/* Several connections to the same peer, taken in turn. Each carries many
 * requests, so more than one only spreads a mount over the daemon's threads.
 */
#define RAILFS_MAX_CONNS 64
#define RAILFS_WIRE_ATTEMPTS 2

struct railfs_pool {
	struct railfs_conn *conns[RAILFS_MAX_CONNS];
	unsigned int count;
	atomic_t next;
	spinlock_t lock;
	struct mutex revive_lock;
	struct railfs_peer peer;
	unsigned long retried[RAILFS_MAX_CONNS];
};

typedef int (*railfs_wire_op)(struct railfs_conn *conn, void *arg);

struct railfs_pool *railfs_pool_open(const struct railfs_peer *peer, unsigned int count);
void railfs_pool_close(struct railfs_pool *pool);

/* The next connection in turn, with a reference the caller gives back. */
struct railfs_conn *railfs_pool_take(struct railfs_pool *pool);

/* The next of the span connections starting at hint. A file's writeback keeps
 * to a window like this: every connection it touches is another daemon thread
 * contending for that inode.
 */
struct railfs_conn *railfs_pool_take_near(struct railfs_pool *pool, unsigned int hint, unsigned int span);
void railfs_pool_give(struct railfs_pool *pool, struct railfs_conn *conn);

int railfs_pool_call(struct railfs_pool *pool, railfs_wire_op op, void *arg);
int railfs_pool_call_near(struct railfs_pool *pool, unsigned int hint, unsigned int span, railfs_wire_op op, void *arg);
int railfs_pool_apply(struct railfs_pool *pool, railfs_wire_op op, void *arg);

/* Reads the attributes of one path. Sets found to false when the peer has no
 * such name, which is not an error.
 */
int railfs_stat(struct railfs_conn *conn, const char *path, struct railfs_attrs *out, bool *found);

/* Reads how much room the peer has where it serves from. */
int railfs_space_of(struct railfs_conn *conn, const char *path, struct railfs_space *out);

/* Lists a directory on the peer. On success *out holds *count entries the
 * caller frees with railfs_free_dirents().
 */
int railfs_list(struct railfs_conn *conn, const char *path, struct railfs_dirent **out, u32 *count);
void railfs_free_dirents(struct railfs_dirent *entries, u32 count);

/* Reads up to len bytes of path at offset into buf. Returns the byte count on
 * success, which may be short at end of file, or a negative errno.
 */
int railfs_read(struct railfs_conn *conn, const char *path, u64 offset, void *buf, u32 len);

/* The same read, leaving the bytes where the transport put them. They stay
 * valid until railfs_read_release, which every successful call owes.
 */
struct railfs_landed {
	struct railfs_conn *conn;
	const void *at;
	u32 slot;
};

int railfs_read_landed(struct railfs_conn *conn, const char *path, u64 offset, u32 len, struct railfs_landed *landed);
void railfs_read_release(struct railfs_landed *landed);

int railfs_read_sg(struct railfs_conn *conn, const char *path, u64 offset, struct sg_table *pages, u32 len);

/* One metadata operation on the peer. Each op reads only the fields it needs:
 * size by truncate, mode by chmod, mtime by utimes, target by the ops naming a
 * second path. link, when given, is where readlink leaves what it found for
 * the caller to kfree.
 */
struct railfs_meta_req {
	u16 op;
	const char *path;
	const char *target;
	u64 size;
	u32 mode;
	s64 mtime;
	char **link;
	/* Filled from the reply when the peer reported them, so a caller that
	 * has just made something need not ask what it is.
	 */
	struct railfs_attrs *made;
};

int railfs_meta_send(struct railfs_conn *conn, const struct railfs_meta_req *req);

/* mkdir, unlink, rmdir or truncate, where only a path and a size are wanted. */
int railfs_meta(struct railfs_conn *conn, u16 op, const char *path, u64 size);

/* The same, for the operations that name a second path. */
int railfs_meta_to(struct railfs_conn *conn, u16 op, const char *path, const char *target, u64 size);

/* Creates path if it is not there, and empties it if it is. */
int railfs_create_file(struct railfs_conn *conn, const char *path);

/* Writes len bytes of buf to path at offset. Returns the byte count the peer
 * accepted, or a negative errno.
 */
int railfs_write(struct railfs_conn *conn, const char *path, u64 offset, const void *buf, u32 len, bool truncate);

/* The same write, taking its bytes from the page cache. A fabric connection
 * sends the folio where it lies; a tcp one still stages a copy.
 */
int railfs_write_folios(struct railfs_conn *conn, const char *path, u64 offset, struct folio **folios, unsigned int nr, u32 len,
			bool truncate);

int railfs_write_sg(struct railfs_conn *conn, const char *path, u64 offset, struct sg_table *pages, u32 len, bool truncate);
int railfs_pool_stat(struct railfs_pool *pool, const char *path, struct railfs_attrs *out, bool *found);
int railfs_pool_list(struct railfs_pool *pool, const char *path, struct railfs_dirent **out, u32 *count);
int railfs_pool_space_of(struct railfs_pool *pool, const char *path, struct railfs_space *out);
int railfs_pool_meta_send(struct railfs_pool *pool, const struct railfs_meta_req *req);
int railfs_pool_meta(struct railfs_pool *pool, u16 op, const char *path, u64 size);
int railfs_pool_meta_to(struct railfs_pool *pool, u16 op, const char *path, const char *target, u64 size);
int railfs_pool_create_file(struct railfs_pool *pool, const char *path);

#endif
