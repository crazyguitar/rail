// SPDX-License-Identifier: GPL-2.0
//
// How much room the export has.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

int railfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct railfs_options *opts = sb->s_fs_info;
	struct railfs_space space = {};
	int err;

	buf->f_type = RAILFS_MAGIC;
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = NAME_MAX;

	if (!opts || !opts->pool) {
		return 0;
	}

	err = railfs_pool_space_of(opts->pool, railfs_export_root(opts), &space);

	// A peer that cannot answer is not a reason to fail df; the mount still
	// works and the geometry above is honest about what is unknown.
	if (err) {
		return 0;
	}

	if (space.block_size) {
		buf->f_bsize = (long)space.block_size;
	}

	buf->f_blocks = space.blocks;
	buf->f_bfree = space.blocks_free;
	buf->f_bavail = space.blocks_free;
	buf->f_files = space.files;
	buf->f_ffree = space.files_free;
	return 0;
}
