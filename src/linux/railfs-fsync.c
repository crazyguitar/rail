// SPDX-License-Identifier: GPL-2.0
//
// Making a file durable on the peer.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// Writeback puts the pages on the peer, which leaves them in its page cache.
// The peer has to be told to flush them as well, or fsync returns while what it
// acknowledged is still only in memory there.
int railfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	const char *path = inode->i_private;
	struct railfs_conn *conn;
	int err;

	err = file_write_and_wait_range(file, start, end);
	if (err) {
		return err;
	}

	if (!opts || !opts->pool || !path) {
		return 0;
	}

	conn = railfs_pool_take(opts->pool);
	err = railfs_meta(conn, RAILFS_META_FSYNC, path, 0);
	railfs_pool_give(opts->pool, conn);
	return err;
}
