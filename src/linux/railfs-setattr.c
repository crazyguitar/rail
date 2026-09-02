// SPDX-License-Identifier: GPL-2.0
//
// Changing what the peer records about a file.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// Each of these is its own operation on the wire, so one chmod -R that also
// moves a timestamp costs two round trips rather than one.
int railfs_push_attrs(struct railfs_conn *conn, const char *path, const struct iattr *attr)
{
	struct railfs_meta_req req = { .path = path };
	int err = 0;

	if (attr->ia_valid & ATTR_SIZE) {
		req.op = RAILFS_META_TRUNCATE;
		req.size = attr->ia_size;
		err = railfs_meta_send(conn, &req);
	}

	if (!err && (attr->ia_valid & ATTR_MODE)) {
		req.op = RAILFS_META_SETMODE;
		req.mode = attr->ia_mode & RAILFS_MODE_BITS;
		err = railfs_meta_send(conn, &req);
	}

	if (!err && (attr->ia_valid & ATTR_MTIME)) {
		req.op = RAILFS_META_SETMTIME;
		req.mtime = attr->ia_mtime.tv_sec;
		err = railfs_meta_send(conn, &req);
	}

	return err;
}

// O_TRUNC, truncate(2), chmod and utimes all land here. Ownership does not
// travel: the peer runs as its own user and would refuse a chown anyway.
int railfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	const unsigned int forwarded = ATTR_SIZE | ATTR_MODE | ATTR_MTIME;
	struct railfs_conn *conn;
	struct inode *inode = d_inode(dentry);
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	const char *path = inode->i_private;
	int err;

	err = setattr_prepare(idmap, dentry, attr);
	if (err) {
		goto out;
	}

	if (!(attr->ia_valid & forwarded)) {
		goto apply;
	}

	if (!opts || !opts->pool || !path) {
		err = -ENOTCONN;
		goto out;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		err = filemap_write_and_wait(inode->i_mapping);
		if (err) {
			goto out;
		}
	}

	conn = railfs_pool_take(opts->pool);
	err = railfs_push_attrs(conn, path, attr);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		goto out;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		truncate_setsize(inode, attr->ia_size);
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
	}

	RAILFS_I(inode)->mine = true;
apply:
	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	err = 0;
out:
	return err;
}
