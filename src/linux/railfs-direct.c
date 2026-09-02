// SPDX-License-Identifier: GPL-2.0
//
// O_DIRECT. The daemon opens the export direct, so this side serves the request
// as it always does and evicts what that left cached.

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uio.h>

#include "railfs.h"
#include "railfs-gds.h"

// In runs rather than per call: at 256 KiB that is two thousand evictions a
// read, and a write waits on the peer for each one.
#define RAILFS_DIRECT_RUN (32u << 20)

struct railfs_run {
	spinlock_t lock;
	loff_t start;
	loff_t end;
	bool dirty;
};

// Written pages reach the peer before they go, or the drop takes the only copy
// of the write with it.
static void railfs_evict_range(struct file *file, loff_t start, loff_t end, bool dirty)
{
	if (end <= start) {
		return;
	}

	if (dirty) {
		filemap_write_and_wait_range(file->f_mapping, start, end - 1);
	}

	invalidate_mapping_pages(file->f_mapping, start >> PAGE_SHIFT, (end - 1) >> PAGE_SHIFT);
}

// Extends the run, and evicts whatever that displaced: the run once it is wide
// enough to be worth the walk, or the old one when this request left it.
static void railfs_evict_later(struct file *file, loff_t start, loff_t end, bool writing)
{
	struct railfs_run *run = file->private_data;
	bool dirty = writing;
	loff_t from = 0;
	loff_t to = 0;

	if (!run) {
		railfs_evict_range(file, start, end, writing);
		return;
	}

	spin_lock(&run->lock);

	if (run->end != start) {
		from = run->start;
		to = run->end;
		dirty = run->dirty;
		run->start = start;
		run->dirty = writing;
	} else {
		run->dirty |= writing;
	}

	run->end = end;

	if (!to && run->end - run->start >= RAILFS_DIRECT_RUN) {
		from = run->start;
		to = run->end;
		dirty = run->dirty;
		run->start = run->end;
		run->dirty = false;
	}

	spin_unlock(&run->lock);
	railfs_evict_range(file, from, to, dirty);
}

// Only a file opened for it carries the run; every other open pays nothing.
int railfs_direct_open(struct file *file)
{
	struct railfs_run *run;

	// Refused before ->read_iter is ever reached without this.
	file->f_mode |= FMODE_CAN_ODIRECT;

	if (!(file->f_flags & O_DIRECT)) {
		return 0;
	}

	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run) {
		return -ENOMEM;
	}

	spin_lock_init(&run->lock);
	file->private_data = run;
	return 0;
}

// The file is going, so nothing may be left pending behind it.
void railfs_direct_release(struct file *file)
{
	struct railfs_run *run = file->private_data;

	if (!run) {
		return;
	}

	file->private_data = NULL;
	railfs_evict_range(file, run->start, run->end, run->dirty);
	kfree(run);
}

// The generic path flushes around every direct request, for the sake of one
// that goes around the page cache. These go through it, so they skip that.
ssize_t railfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	loff_t start = iocb->ki_pos;
	ssize_t done;

	if (!(iocb->ki_flags & IOCB_DIRECT)) {
		return generic_file_read_iter(iocb, to);
	}

	if (railfs_gds_claims(to)) {
		return railfs_gds_read_iter(iocb, to);
	}

	iocb->ki_flags &= ~IOCB_DIRECT;
	done = generic_file_read_iter(iocb, to);
	iocb->ki_flags |= IOCB_DIRECT;

	if (done > 0) {
		railfs_evict_later(iocb->ki_filp, start, start + done, false);
	}

	return done;
}

ssize_t railfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	loff_t start = iocb->ki_pos;
	ssize_t done;

	if (!(iocb->ki_flags & IOCB_DIRECT)) {
		return generic_file_write_iter(iocb, from);
	}

	if (railfs_gds_claims(from)) {
		return railfs_gds_write_iter(iocb, from);
	}

	iocb->ki_flags &= ~IOCB_DIRECT;
	done = generic_file_write_iter(iocb, from);
	iocb->ki_flags |= IOCB_DIRECT;

	if (done > 0) {
		railfs_evict_later(iocb->ki_filp, start, start + done, true);
	}

	return done;
}
