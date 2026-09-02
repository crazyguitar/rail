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
	const char *from_here = old_dir->i_private ? old_dir->i_private : ".";
	const char *to_here = new_dir->i_private ? new_dir->i_private : ".";
	struct railfs_conn *conn;
	char *from = NULL;
	char *to = NULL;
	char *kept = NULL;
	int err;

	if (flags) {
		err = -EINVAL;
		goto out;
	}

	if (!opts || !opts->pool) {
		err = -ENOTCONN;
		goto out;
	}

	from = railfs_path_under(from_here, old_dentry->d_name.name);
	to = railfs_path_under(to_here, new_dentry->d_name.name);
	kept = to ? kstrdup(to, GFP_NOFS) : NULL;

	if (!from || !to || !kept) {
		err = -ENOMEM;
		goto out;
	}

	err = filemap_write_and_wait(inode->i_mapping);
	if (err) {
		goto out;
	}

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta_to(conn, RAILFS_META_RENAME, from, to, 0);
	railfs_pool_give(opts->pool, conn);

	if (err) {
		goto out;
	}

	// The inode remembers the name it answers to, and it has just moved.
	swap(inode->i_private, kept);

	// Every cached child under a renamed directory remembers the old prefix.
	if (S_ISDIR(inode->i_mode)) {
		shrink_dcache_parent(old_dentry);
	}
out:
	kfree(kept);
	kfree(from);
	kfree(to);
	return err;
}
