// SPDX-License-Identifier: GPL-2.0
//
// Making a regular file.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

int railfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	struct railfs_attrs attrs = {};
	struct inode *inode = NULL;
	char *path;
	int err;

	if (!opts || !opts->pool) {
		return -ENOTCONN;
	}

	path = railfs_child_path(dir, dentry);
	if (!path) {
		return -ENOMEM;
	}

	err = railfs_pool_create_file(opts->pool, path);
	if (err) {
		goto out;
	}

	attrs.mode = mode & RAILFS_MODE_BITS;
	inode = railfs_inode_for(dir->i_sb, &attrs, path);
	if (!inode) {
		err = -ENOMEM;
		goto out;
	}

	d_instantiate(dentry, inode);
	err = 0;
out:
	kfree(path);
	return err;
}
