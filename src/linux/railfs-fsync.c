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
	struct railfs_path *path;
	int err;

	err = file_write_and_wait_range(file, start, end);
	if (err) {
		return err;
	}

	if (!opts || !opts->pool) {
		return 0;
	}

	path = railfs_path_hold(inode);
	if (!path) {
		return 0;
	}

	err = railfs_pool_meta(opts->pool, RAILFS_META_FSYNC, path->name, 0);
	railfs_path_put(path);
	return err;
}
