// SPDX-License-Identifier: GPL-2.0
//
// Resolving a name to an inode.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

struct dentry *railfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct railfs_conn *conn;
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	const char *here = dir->i_private ? dir->i_private : ".";
	struct railfs_attrs attrs = {};
	struct inode *inode = NULL;
	struct dentry *result;
	char *path = NULL;
	bool found = false;
	int err;

	if (!opts || !opts->pool) {
		result = ERR_PTR(-ENOTCONN);
		goto out;
	}

	path = railfs_path_under(here, dentry->d_name.name);
	if (!path) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}

	// One name, not the whole directory. Listing the parent to resolve a
	// single entry made a directory of n names cost n listings to walk.
	conn = railfs_pool_take(opts->pool);
	err = railfs_stat(conn, path, &attrs, &found);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		result = ERR_PTR(err);
		goto out;
	}

	if (found) {
		inode = railfs_inode_for(dir->i_sb, &attrs, path);
	}

	// A name that is not there is not an error: the dentry is cached negative
	// so the next lookup does not cost another round trip. It carries the time
	// it was asked, because a negative dentry has no inode to hold one.
	dentry->d_time = jiffies;
	result = d_splice_alias(inode, dentry);
out:
	kfree(path);
	return result;
}
