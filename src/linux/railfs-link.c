// SPDX-License-Identifier: GPL-2.0
//
// A second name for one file.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// A second name for one file, so the bytes have to be on the peer before it
// exists - a page cache holding them under the first name is not enough.
int railfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	struct railfs_meta_req req = { .op = RAILFS_META_HARDLINK };
	struct railfs_attrs attrs = {};
	struct railfs_path *first = NULL;
	struct inode *second;
	char *path = NULL;
	int err;

	if (!opts || !opts->pool) {
		return -ENOTCONN;
	}

	first = railfs_path_hold(inode);
	if (!first) {
		return -ENOTCONN;
	}

	path = railfs_child_path(dir, dentry);
	if (!path) {
		err = -ENOMEM;
		goto out;
	}

	err = filemap_write_and_wait(inode->i_mapping);
	if (err) {
		goto out;
	}

	req.path = first->name;
	req.target = path;

	err = railfs_pool_meta_send(opts->pool, &req);
	if (err) {
		goto out;
	}

	// A second inode, not a second reference: an inode here remembers the one
	// path its reads ask for, so sharing it makes the surviving name
	// unreadable once the other is unlinked.
	attrs.size = i_size_read(inode);
	attrs.mode = inode->i_mode & RAILFS_MODE_BITS;
	attrs.mtime = inode_get_mtime_sec(inode);
	attrs.links = inode->i_nlink + 1;

	second = railfs_inode_for(dir->i_sb, &attrs, path);
	if (!second) {
		err = -ENOMEM;
		goto out;
	}

	inc_nlink(inode);
	inode_set_ctime_current(inode);
	d_instantiate(dentry, second);
	err = 0;
out:
	kfree(path);
	railfs_path_put(first);
	return err;
}
