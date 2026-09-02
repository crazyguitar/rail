// SPDX-License-Identifier: GPL-2.0
//
// Opening a file and what closing it has to flush.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// Close-to-open, the bargain every network filesystem without delegations
// makes: what a reader sees is whatever the peer held when the file was
// opened, and a writer's bytes are there by the time close returns.
//
// Not forced. The walk that resolved this name ran d_revalidate a moment ago,
// so forcing here asks the same question twice and costs a round trip on every
// open; actimeo is what bounds how old the answer may be.
int railfs_file_open(struct inode *inode, struct file *file)
{
	int err = railfs_refresh(inode, false);

	if (err == -ENOENT) {
		return -ESTALE;
	}

	if (err) {
		return err;
	}

	err = railfs_direct_open(file);
	if (err) {
		return err;
	}

	err = generic_file_open(inode, file);
	if (err) {
		goto error;
	}

	return 0;

error:
	railfs_direct_release(file);
	return err;
}

int railfs_file_release(struct inode *inode, struct file *file)
{
	railfs_direct_release(file);
	return 0;
}

int railfs_file_flush(struct file *file, fl_owner_t id)
{
	return file_write_and_wait(file);
}
