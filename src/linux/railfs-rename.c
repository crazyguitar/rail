// SPDX-License-Identifier: GPL-2.0
//
// Moving a name to another name.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

int railfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir,
		  struct dentry *new_dentry, unsigned int flags)
{
	struct railfs_options *opts = old_dir->i_sb->s_fs_info;
	struct inode *inode = d_inode(old_dentry);
	struct railfs_path *fresh = NULL;
	char *from = NULL;
	char *to = NULL;
	int err;

	if (flags) {
		err = -EINVAL;
		goto out;
	}

	if (!opts || !opts->pool) {
		err = -ENOTCONN;
		goto out;
	}

	from = railfs_child_path(old_dir, old_dentry);
	to = railfs_child_path(new_dir, new_dentry);
	fresh = to ? railfs_path_new(to) : NULL;

	if (!from || !to || !fresh) {
		err = -ENOMEM;
		goto out;
	}

	err = filemap_write_and_wait(inode->i_mapping);
	if (err) {
		goto out;
	}

	err = railfs_pool_meta_to(opts->pool, RAILFS_META_RENAME, from, to, 0);
	if (err) {
		goto out;
	}

	// The inode remembers the name it answers to, and it has just moved.
	railfs_rehash_inode(inode, fresh);
	fresh = NULL;

	// Every cached child under a renamed directory remembers the old prefix.
	if (S_ISDIR(inode->i_mode)) {
		shrink_dcache_parent(old_dentry);
	}
out:
	railfs_path_put(fresh);
	kfree(from);
	kfree(to);
	return err;
}
