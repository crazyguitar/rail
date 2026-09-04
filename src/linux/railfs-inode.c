// SPDX-License-Identifier: GPL-2.0
//
// Inode lifetime, and keeping one in step with the peer.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

struct inode *railfs_alloc_inode(struct super_block *sb)
{
	struct railfs_inode *self = alloc_inode_sb(sb, railfs_inode_cache, GFP_KERNEL);

	if (!self) {
		return NULL;
	}

	self->checked = 0;
	self->mtime = 0;
	self->size = 0;
	self->mine = false;
	self->window = UINT_MAX;
	return &self->vfs;
}

void railfs_free_inode(struct inode *inode)
{
	kmem_cache_free(railfs_inode_cache, RAILFS_I(inode));
}

void railfs_init_once(void *p)
{
	struct railfs_inode *self = p;

	inode_init_once(&self->vfs);
}

struct railfs_path *railfs_path_new(const char *name)
{
	size_t len = strlen(name) + 1;
	struct railfs_path *path = kmalloc(sizeof(*path) + len, GFP_NOFS);

	if (!path) {
		return NULL;
	}

	kref_init(&path->ref);
	memcpy(path->name, name, len);
	return path;
}

static void railfs_path_release(struct kref *ref)
{
	kfree(container_of(ref, struct railfs_path, ref));
}

void railfs_path_put(struct railfs_path *path)
{
	if (path) {
		kref_put(&path->ref, railfs_path_release);
	}
}

struct railfs_path *railfs_path_hold(struct inode *inode)
{
	struct railfs_path *path;

	spin_lock(&inode->i_lock);
	path = inode->i_private;
	if (path) {
		kref_get(&path->ref);
	}
	spin_unlock(&inode->i_lock);
	return path;
}

void railfs_path_replace(struct inode *inode, struct railfs_path *fresh)
{
	struct railfs_path *old;

	spin_lock(&inode->i_lock);
	old = inode->i_private;
	inode->i_private = fresh;
	spin_unlock(&inode->i_lock);
	railfs_path_put(old);
}

void railfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	railfs_path_replace(inode, NULL);
}

// Only ever from the inode constructor: the page cache says not to tune this
// on i_size and not to touch it while the inode is live, because the flags it
// writes are not set atomically.
void railfs_tune_folios(struct inode *inode)
{
	struct railfs_options *opts = inode->i_sb->s_fs_info;

	if (!opts || !S_ISREG(inode->i_mode)) {
		return;
	}

	// Unconditional: the call carries the maximum as well, and skipping it when
	// only the floor happens to match leaves the mapping with no large folios
	// at all, which cost a third of the write rate.
	mapping_set_folio_order_range(inode->i_mapping, railfs_minfolio_order(opts, i_size_read(inode)),
				      railfs_fetch_order(opts));
}

static int railfs_inode_names(struct inode *inode, void *wanted)
{
	const struct railfs_path *asked = wanted;
	struct railfs_path *path;
	bool same;

	spin_lock(&inode->i_lock);
	path = inode->i_private;
	same = path && strcmp(path->name, asked->name) == 0;
	spin_unlock(&inode->i_lock);
	return same;
}

static int railfs_inode_take_name(struct inode *inode, void *wanted)
{
	struct railfs_path *path = wanted;

	inode->i_private = railfs_path_get(path);
	inode->i_ino = railfs_ino_of(path->name);
	return 0;
}

static void railfs_install_ops(struct inode *inode, umode_t mode)
{
	if (S_ISDIR(mode)) {
		inode->i_op = &railfs_dir_inode_ops;
		inode->i_fop = &railfs_dir_ops;
		inc_nlink(inode);
	} else if (S_ISLNK(mode)) {
		inode->i_op = &railfs_link_inode_ops;
	} else {
		inode->i_op = &railfs_file_inode_ops;
		inode->i_fop = &railfs_file_ops;
		inode->i_mapping->a_ops = &railfs_aops;

		// Big folios, but never bigger than one fetch: a folio that cannot be
		// filled by a single request falls back to reading itself a round trip
		// at a time, measured 1.47 against 3.77 GiB/s.
		railfs_tune_folios(inode);
	}
}

static umode_t railfs_mode_of(const struct railfs_attrs *a)
{
	umode_t mode = a->mode & RAILFS_MODE_BITS;

	if (a->directory) {
		return mode | S_IFDIR;
	}

	if (a->link) {
		return mode | S_IFLNK;
	}

	return mode | S_IFREG;
}

static void railfs_fill_new_inode(struct inode *inode, const struct railfs_attrs *a)
{
	struct railfs_options *opts = inode->i_sb->s_fs_info;

	inode->i_mode = railfs_mode_of(a);
	inode->i_uid = opts ? opts->uid : GLOBAL_ROOT_UID;
	inode->i_gid = opts ? opts->gid : GLOBAL_ROOT_GID;
	simple_inode_init_ts(inode);
	i_size_write(inode, a->size);
	railfs_install_ops(inode, inode->i_mode);

	// Without these a listing dates every file to the moment the mount saw it,
	// and a second name for one file reads as the only one.
	if (a->mtime) {
		struct timespec64 when = { .tv_sec = a->mtime, .tv_nsec = 0 };

		inode_set_mtime_to_ts(inode, when);
		inode_set_atime_to_ts(inode, when);
		inode_set_ctime_to_ts(inode, when);
	}

	if (a->links) {
		set_nlink(inode, a->links);
	}

	RAILFS_I(inode)->size = a->size;
	RAILFS_I(inode)->mtime = a->mtime;
	RAILFS_I(inode)->checked = jiffies;
	RAILFS_I(inode)->mine = false;
}

struct inode *railfs_inode_for(struct super_block *sb, const struct railfs_attrs *a, const char *path)
{
	struct railfs_path *name = railfs_path_new(path);
	struct inode *inode;

	if (!name) {
		return NULL;
	}

	inode = iget5_locked(sb, railfs_ino_of(path), railfs_inode_names, railfs_inode_take_name, name);
	railfs_path_put(name);
	if (!inode) {
		return NULL;
	}

	if (!(inode->i_state & I_NEW)) {
		return inode;
	}

	railfs_fill_new_inode(inode, a);
	unlock_new_inode(inode);
	return inode;
}

void railfs_rehash_inode(struct inode *inode, struct railfs_path *fresh)
{
	remove_inode_hash(inode);
	railfs_path_replace(inode, fresh);
	inode->i_ino = railfs_ino_of(fresh->name);
	__insert_inode_hash(inode, inode->i_ino);
}

// Asks the peer what it holds now. Nothing else in this mount ever notices a
// file rewritten behind its back, so without this a read serves whatever the
// page cache happened to keep, for as long as the inode lives.
int railfs_refresh(struct inode *inode, bool force)
{
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	struct railfs_inode *self = RAILFS_I(inode);
	struct railfs_attrs attrs = {};
	struct railfs_path *path;
	bool found = false;
	int err;

	if (!opts || !opts->pool) {
		return 0;
	}

	if (!force && self->checked && time_before(jiffies, self->checked + opts->actimeo)) {
		return 0;
	}

	path = railfs_path_hold(inode);
	if (!path) {
		return 0;
	}

	err = railfs_pool_stat(opts->pool, path->name, &attrs, &found);
	railfs_path_put(path);
	if (err) {
		return err;
	}

	if (!found) {
		return -ENOENT;
	}

	// A size or mtime that moved means the cached pages belong to a file that
	// is gone - unless this mount is what moved them.
	if (self->mine) {
		self->mine = false;
	} else if (S_ISREG(inode->i_mode) && (attrs.size != self->size || attrs.mtime != self->mtime)) {
		// Anything this client has not sent yet would be lost by the
		// invalidate, so it goes first and the peer's answer wins the rest.
		err = filemap_write_and_wait(inode->i_mapping);
		if (err) {
			return err;
		}

		// A mapped or re-dirtied page can refuse to go. Recording the peer's
		// answer anyway would mark the inode fresh with stale pages still
		// here, so this leaves it stale and asks again.
		if (invalidate_inode_pages2(inode->i_mapping)) {
			return 0;
		}

		i_size_write(inode, attrs.size);
	}

	if (attrs.mtime) {
		struct timespec64 when = { .tv_sec = attrs.mtime, .tv_nsec = 0 };

		inode_set_mtime_to_ts(inode, when);
		inode_set_ctime_to_ts(inode, when);
	}

	inode->i_mode = (inode->i_mode & S_IFMT) | (attrs.mode & RAILFS_MODE_BITS);
	self->size = attrs.size;
	self->mtime = attrs.mtime;
	self->checked = jiffies;
	return 0;
}

// A name is only as good as the last time the peer confirmed it. Since 6.15 the
// parent and the name arrive alongside the dentry, and neither is wanted here.
int railfs_revalidate(struct inode *dir, const struct qstr *name, struct dentry *dentry, unsigned int flags)
{
	struct railfs_options *opts = dentry->d_sb->s_fs_info;
	struct inode *inode = d_inode(dentry);
	int err;

	// Asking the peer sleeps, which an RCU walk may not do.
	if (flags & LOOKUP_RCU) {
		return -ECHILD;
	}

	if (!opts || !opts->pool) {
		return 1;
	}

	// A name the peer did not have is worth asking about again once it has had
	// time to appear.
	if (!inode) {
		return time_before(jiffies, dentry->d_time + opts->actimeo);
	}

	err = railfs_refresh(inode, false);
	if (err == -ENOENT) {
		return 0;
	}

	// A peer that cannot be reached is not a reason to throw a name away. The
	// operation that follows will fail with something more useful than a
	// missing file.
	return 1;
}
