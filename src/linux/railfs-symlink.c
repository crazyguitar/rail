// SPDX-License-Identifier: GPL-2.0
//
// Making a symbolic link and reading what it points at.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// A link's target is copied to the peer as written and never resolved here: it
// means whatever it means on the machine that holds it.
int railfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	struct railfs_meta_req req = { .op = RAILFS_META_SYMLINK, .target = symname };
	struct railfs_attrs attrs = {};
	struct railfs_conn *conn;
	struct inode *inode;
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

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta_send(conn, &req);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		goto out;
	}

	attrs.mode = RAILFS_LINK_MODE;
	attrs.link = 1;
	attrs.size = strlen(symname);

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

// The target lives on the peer and asking for it blocks, which an RCU walk
// cannot do; -ECHILD sends the walk back holding a reference.
const char *railfs_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	const char *path = inode->i_private;
	struct railfs_meta_req req = { .op = RAILFS_META_READLINK, .path = path };
	struct railfs_conn *conn;
	char *target = NULL;
	int err;

	if (!dentry) {
		return ERR_PTR(-ECHILD);
	}

	if (!opts || !opts->pool || !path) {
		return ERR_PTR(-ENOTCONN);
	}

	req.link = &target;

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta_send(conn, &req);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		return ERR_PTR(err);
	}

	// An empty target is not a path anything can be resolved against, and
	// handing it back would make the link look like a name for the cwd.
	if (!target || !*target) {
		kfree(target);
		return ERR_PTR(-EIO);
	}

	set_delayed_call(done, kfree_link, target);
	return target;
}
