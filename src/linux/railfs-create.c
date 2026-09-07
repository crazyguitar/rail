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
	struct railfs_meta_req req = { .op = RAILFS_META_CREATE, .made = &attrs };
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

	req.path = path;
	req.mode = mode & RAILFS_MODE_BITS;

	err = railfs_pool_meta_send(opts->pool, &req);
	if (err) {
		goto out;
	}

	// Zeroed attributes are not attributes: a mode of nothing describes a
	// file this mount would then instantiate wrongly.
	if (!attrs.ino) {
		err = railfs_attrs_of_new(opts, path, &attrs);
		if (err) {
			goto out;
		}
	}

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
