// SPDX-License-Identifier: GPL-2.0
//
// Making a directory.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// Since 6.15 mkdir returns a dentry rather than an int: NULL means the one it
// was given has been instantiated, and an error is an ERR_PTR.
struct dentry *railfs_mkdir_op(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct railfs_conn *conn;
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	struct railfs_attrs attrs = {};
	struct inode *inode = NULL;
	struct dentry *result = NULL;
	char *path;
	int err;

	if (!opts || !opts->pool) {
		return ERR_PTR(-ENOTCONN);
	}

	path = railfs_child_path(dir, dentry);
	if (!path) {
		return ERR_PTR(-ENOMEM);
	}

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta(conn, RAILFS_META_MKDIR, path, 0);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		result = ERR_PTR(err);
		goto out;
	}

	attrs.mode = mode & RAILFS_MODE_BITS;
	attrs.directory = 1;
	inode = railfs_inode_for(dir->i_sb, &attrs, path);
	if (!inode) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}

	inc_nlink(dir);
	d_instantiate(dentry, inode);
out:
	kfree(path);
	return result;
}
