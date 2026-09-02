// SPDX-License-Identifier: GPL-2.0
//
// Removing a name, and removing a directory.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

int railfs_remove(struct inode *dir, struct dentry *dentry, u16 op)
{
	struct railfs_conn *conn;
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	char *path;
	int err;

	if (!opts || !opts->pool) {
		return -ENOTCONN;
	}

	path = railfs_child_path(dir, dentry);
	if (!path) {
		return -ENOMEM;
	}

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta(conn, op, path, 0);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		goto out;
	}

	// The peer no longer has it, so neither does the dcache. A file may answer
	// to more than one name, so an unlink takes one away rather than declaring
	// the whole inode gone; a directory really is gone.
	if (op == RAILFS_META_RMDIR) {
		if (d_inode(dentry)) {
			clear_nlink(d_inode(dentry));
		}
		drop_nlink(dir);
	} else if (d_inode(dentry)) {
		drop_nlink(d_inode(dentry));
	}
out:
	kfree(path);
	return err;
}

int railfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return railfs_remove(dir, dentry, RAILFS_META_UNLINK);
}

int railfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	return railfs_remove(dir, dentry, RAILFS_META_RMDIR);
}
