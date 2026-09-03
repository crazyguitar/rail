/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_H
#define RAILFS_H

/* What the parts of the filesystem share: the mount's options, the inode this
 * module hangs off the vfs one, and the tuning constants. Everything here is
 * private to the module - the wire is in railfs-proto.h.
 */

#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/kref.h>
#include <linux/wait.h>

#include "railfs-tcp.h"

#define RAILFS_MAGIC 0x5241494c /* "RAIL" */
#define RAILFS_DEFAULT_PORT 18600

/* The permission bits the wire carries: setuid, setgid, sticky and rwx for
 * user, group and other. Ownership does not travel, so nothing above these.
 */
#define RAILFS_MODE_BITS 07777

/* A symbolic link's own mode. What it points at is what has permissions; the
 * link itself is conventionally left wide open.
 */
#define RAILFS_LINK_MODE 0777
#define RAILFS_DEFAULT_CONNS 32
#define RAILFS_FETCH_BYTES (1u << 20)
#define RAILFS_READAHEAD_BYTES (256u << 20)
#define RAILFS_BLOCK_BYTES (1u << 18)
#define RAILFS_MINFOLIO_BYTES (1u << 18)
#define RAILFS_MAX_FETCH_BYTES RAILFS_PAGE_SIZE

// Flushes the mount may have on the wire at once. Sixteen held the writeback
// to nine or ten in flight and 5.0 GiB/s; a hundred and twenty eight reaches
// thirty two and 5.6, which is what the peer's drive does with the same
// sixteen writers. Each one costs a page-sized buffer while it is in flight.
// Folios one write may scatter across, matching what a queue pair is built to
// take. Past this a flush is staged into a buffer instead.
#define RAILFS_MAX_SEND_FOLIOS 8

#define RAILFS_FLUSH_LIMIT 128

// How many connections one file's writeback may spread over. Measured on a
// single writer: four connections give 1.58 GiB/s where sixteen give 1.37,
// because every extra connection is another daemon thread contending for the
// one inode. Spreading files is still worth it - eight writers over eight
// files want sixteen connections, not four - so this bounds one file rather
// than the mount.
#define RAILFS_FLUSH_SPAN 4

// The largest folio the page cache may hand out here, so one always fits in a
// single fetch.
#define RAILFS_FETCH_ORDER (const_ilog2(RAILFS_FETCH_BYTES) - PAGE_SHIFT)

// A read is clamped to the negotiated page, so a fetch larger than one comes
// back short and leaves folios unlocked without being uptodate - which reads
// as a slow mount rather than as the mistake it is. And a fetch smaller than a
// page would make the folio order above underflow to something enormous.
static_assert(RAILFS_FETCH_BYTES <= RAILFS_PAGE_SIZE, "a fetch cannot exceed the page the session negotiated");
static_assert(RAILFS_FETCH_BYTES >= PAGE_SIZE, "a fetch has to be at least one page");
static_assert(RAILFS_BLOCK_BYTES <= RAILFS_FETCH_BYTES, "a readahead block has to fit in one fetch");

// How many fetches may be on the wire at once. It used to be the connection
// count, which is a different question - a fetch holds a connection only for
// its exchange, so the two need not match, and tying them capped the readahead
// at however many connections were mounted.
#define RAILFS_FETCH_INFLIGHT 64

// Long enough that walking a tree does not restat every name, short enough
// that a change on the peer is noticed without unmounting. Opening a file
// always asks, so this only bounds how stale a stat may be.
#define RAILFS_DEFAULT_ACTIMEO 30
#define RAILFS_MAX_ACTIMEO 3600

// What mount.railfs folds the HOST:EXPORT spec into. The transport is not here
// yet, so these are carried and reported back rather than dialled.
struct railfs_options {
	char *host;
	char *export;
	u16 port;
	unsigned int conns;
	// What a readahead window is cut into, how far ahead the kernel reads, and
	// the granularity a short fetch is widened to. All bytes, all powers of
	// two, and all defaulted to the constants above.
	unsigned int fetch;
	unsigned int readahead;
	unsigned int block;
	// A fault ramps its readahead from order zero, so without a floor an
	// mmap is filled in folios far below the fetch a read gets.
	unsigned int minfolio;
	// How many connections one file's writeback spreads over, and how many
	// flushes the mount keeps in flight.
	unsigned int flush_span;
	unsigned int flush_limit;
	bool rdma;
	// The fabric checks its own crc, so a mount that trusts it can stop
	// hashing every page at both ends. Off by default: a wire error that
	// nothing checks is a corrupt file nobody reports.
	bool noverify;
	// How long a name or a set of attributes may be believed without asking
	// the peer again, in jiffies. Zero means ask every time.
	unsigned long actimeo;
	bool actimeo_set;
	// Who everything on this mount belongs to. The wire carries no ownership,
	// so this is asked for rather than assumed - claiming root left a mount
	// nobody else could write to.
	kuid_t uid;
	kgid_t gid;
	struct railfs_pool *pool;
};

static inline unsigned int railfs_fetch_order(const struct railfs_options *opts)
{
	return ilog2(opts->fetch) - PAGE_SHIFT;
}

// Bounded by the file as well as by the fetch: a floor wider than the file
// itself buys nothing and pins a whole folio per small file.
static inline unsigned int railfs_minfolio_order(const struct railfs_options *opts, loff_t size)
{
	unsigned int want = min(ilog2(opts->minfolio), ilog2(opts->fetch)) - PAGE_SHIFT;
	unsigned int fits = 0;

	if (size > PAGE_SIZE) {
		fits = ilog2(size) - PAGE_SHIFT;
	}

	return min(want, fits);
}

// What the peer said the last time it was asked, so a change made behind this
// mount's back can be told from one this mount made itself.
struct railfs_inode {
	struct inode vfs;
	unsigned long checked;
	s64 mtime;
	u64 size;
	// Set when this mount is the one that moved the file. The peer's mtime
	// will differ on the next look either way, and only this tells the two
	// cases apart.
	bool mine;
};

struct railfs_path {
	struct kref ref;
	char name[];
};

struct railfs_path *railfs_path_new(const char *name);
struct railfs_path *railfs_path_hold(struct inode *inode);
void railfs_path_put(struct railfs_path *path);
void railfs_path_replace(struct inode *inode, struct railfs_path *fresh);

static inline struct railfs_path *railfs_path_get(struct railfs_path *path)
{
	kref_get(&path->ref);
	return path;
}


extern struct kmem_cache *railfs_inode_cache;

static inline struct railfs_inode *RAILFS_I(struct inode *inode)
{
	return container_of(inode, struct railfs_inode, vfs);
}

/* Reading pages in and writing them back, in railfs-read.c and railfs-write.c.
 * Both directions run on one workqueue, which the module owns.
 */
extern struct workqueue_struct *railfs_page_wq;
int railfs_fill_folio(struct folio *folio);
int railfs_read_folio(struct file *file, struct folio *folio);

/* O_DIRECT is served by the buffered path, then evicted from the cache. */
ssize_t railfs_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t railfs_write_iter(struct kiocb *iocb, struct iov_iter *from);
int railfs_direct_open(struct file *file);
void railfs_direct_release(struct file *file);
void railfs_readahead(struct readahead_control *rac);
int railfs_writepages(struct address_space *mapping, struct writeback_control *wbc);
int railfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len,
		     struct folio **foliop, void **fsdata);
int railfs_write_end(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len,
		   unsigned int copied, struct folio *folio, void *fsdata);

/* One operation per file, each declared here because the operation tables
 * that name them live with the superblock.
 */
struct dentry *railfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
int railfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *railfs_mkdir_op(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode);
int railfs_remove(struct inode *dir, struct dentry *dentry, u16 op);
int railfs_unlink(struct inode *dir, struct dentry *dentry);
int railfs_rmdir(struct inode *dir, struct dentry *dentry);
int railfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir,
		struct dentry *new_dentry, unsigned int flags);
int railfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname);
const char *railfs_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done);
int railfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
bool railfs_name_is_usable(const char *name);
void railfs_prime(struct dentry *parent, const struct railfs_attrs *a, const char *path, const struct qstr *name);
int railfs_readdir(struct file *file, struct dir_context *ctx);
int railfs_file_open(struct inode *inode, struct file *file);
int railfs_file_release(struct inode *inode, struct file *file);
int railfs_file_flush(struct file *file, fl_owner_t id);
int railfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
int railfs_push_attrs(struct railfs_pool *pool, const char *path, const struct iattr *attr);
int railfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr);
int railfs_statfs(struct dentry *dentry, struct kstatfs *buf);
struct inode *railfs_alloc_inode(struct super_block *sb);
void railfs_free_inode(struct inode *inode);
void railfs_init_once(void *p);
void railfs_evict_inode(struct inode *inode);
void railfs_tune_folios(struct inode *inode);
struct inode *railfs_inode_for(struct super_block *sb, const struct railfs_attrs *a, const char *path);
void railfs_rehash_inode(struct inode *inode, struct railfs_path *fresh);
int railfs_refresh(struct inode *inode, bool force);
int railfs_revalidate(struct inode *dir, const struct qstr *name, struct dentry *dentry, unsigned int flags);

/* Paths and inode numbers, shared by every operation that names a file. */
const char *railfs_export_root(const struct railfs_options *opts);
char *railfs_path_of(struct dentry *dentry);
char *railfs_path_under(const char *parent, const char *name);
char *railfs_child_path(struct inode *dir, struct dentry *dentry);
unsigned long railfs_ino_of(const char *name);

/* The operation tables, which live with the superblock in railfs-main.c because
 * that is what hands them to a new inode.
 */
extern const struct inode_operations railfs_dir_inode_ops;
extern const struct inode_operations railfs_file_inode_ops;
extern const struct inode_operations railfs_link_inode_ops;
extern const struct file_operations railfs_dir_ops;
extern const struct file_operations railfs_file_ops;
extern const struct dentry_operations railfs_dentry_ops;
extern const struct address_space_operations railfs_aops;

int railfs_pages_init(void);
void railfs_pages_exit(void);

#endif
