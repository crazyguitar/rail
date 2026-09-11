// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/sched/mm.h>
#include <linux/uio.h>

#include "railfs.h"
#include "railfs-gds.h"
#include "railfs-nvfs.h"
#include "railfs-tcp.h"

#define RAILFS_GDS_NEEDS (nvfs_ft_map_sglist | nvfs_ft_is_gpu_page)

static struct nvfs_dma_rw_ops *railfs_nvfs;
static DECLARE_RWSEM(railfs_nvfs_lock);
static bool railfs_gds_on = true;

module_param_named(gds, railfs_gds_on, bool, 0644);
MODULE_PARM_DESC(gds, "land GPUDirect Storage buffers straight from the fabric (default on)");

int railfs_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops)
{
	if (!ops || (ops->ft_bmap & RAILFS_GDS_NEEDS) != RAILFS_GDS_NEEDS || !ops->nvfs_is_gpu_page ||
	    !ops->nvfs_dma_map_sg_attrs || !ops->nvfs_dma_unmap_sg) {
		return -EINVAL;
	}

	down_write(&railfs_nvfs_lock);
	WRITE_ONCE(railfs_nvfs, ops);
	up_write(&railfs_nvfs_lock);
	pr_info("railfs: nvidia-fs registered, gds is %s\n", READ_ONCE(railfs_gds_on) ? "on" : "off");
	return 0;
}
EXPORT_SYMBOL(railfs_register_nvfs_dma_ops);

void railfs_unregister_nvfs_dma_ops(void)
{
	down_write(&railfs_nvfs_lock);
	WRITE_ONCE(railfs_nvfs, NULL);
	up_write(&railfs_nvfs_lock);
	pr_info("railfs: nvidia-fs unregistered\n");
}
EXPORT_SYMBOL(railfs_unregister_nvfs_dma_ops);

int railfs_gds_map(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct nvfs_dma_rw_ops *ops = READ_ONCE(railfs_nvfs);
	struct scatterlist *sg;
	int i;
	int completed = 0;
	int mapped;

	if (!ops) {
		return -ENODEV;
	}

	/* nvidia-fs does not unwind earlier entries when a later mapping fails. */
	for_each_sg(sgl, sg, nents, i) {
		mapped = ops->nvfs_dma_map_sg_attrs(dev, sg, 1, dir, 0);
		if (mapped != 1) {
			goto unmap;
		}
		completed++;
	}
	return nents;

unmap:
	for_each_sg(sgl, sg, completed, i) {
		ops->nvfs_dma_unmap_sg(dev, sg, 1, dir);
	}
	return mapped == NVFS_BAD_REQ ? -EINVAL : -EIO;
}

void railfs_gds_unmap(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct nvfs_dma_rw_ops *ops = READ_ONCE(railfs_nvfs);

	if (ops) {
		ops->nvfs_dma_unmap_sg(dev, sgl, nents, dir);
	}
}

bool railfs_gds_claims(const struct iov_iter *iter)
{
	struct nvfs_dma_rw_ops *ops;
	struct iov_iter peek;
	struct page *page = NULL;
	struct page **pages = &page;
	size_t offset;
	bool gpu = false;

	down_read(&railfs_nvfs_lock);
	ops = railfs_nvfs;
	if (!ops || !iov_iter_count(iter)) {
		goto out;
	}

	peek = *iter;
	if (railfs_extract_pages(&peek, &pages, PAGE_SIZE, 1, &offset) <= 0) {
		goto out;
	}

	gpu = ops->nvfs_is_gpu_page(page);

	railfs_release_pages(&peek, pages, 1);

out:
	up_read(&railfs_nvfs_lock);
	return gpu;
}

struct railfs_gds_chunk {
	struct page **pages;
	unsigned int nr;
	struct sg_table table;
	u32 len;
	struct iov_iter iter;
};

static void railfs_gds_unpin(struct railfs_gds_chunk *chunk)
{
	if (chunk->table.sgl) {
		sg_free_table(&chunk->table);
	}

	railfs_release_pages(&chunk->iter, chunk->pages, chunk->nr);

	kvfree(chunk->pages);
}

static void railfs_gds_scatter(struct railfs_gds_chunk *chunk, size_t offset)
{
	struct scatterlist *sg;
	size_t left = chunk->len;
	unsigned int i;

	for_each_sg(chunk->table.sgl, sg, chunk->nr, i) {
		size_t bytes = min(left, PAGE_SIZE - offset);

		sg_set_page(sg, chunk->pages[i], bytes, offset);
		left -= bytes;
		offset = 0;
	}
}

static int railfs_gds_pin(struct iov_iter *iter, size_t want, struct railfs_gds_chunk *chunk)
{
	size_t offset = 0;
	ssize_t got;
	int err;

	memset(chunk, 0, sizeof(*chunk));

	got = railfs_extract_pages(iter, &chunk->pages, want, RAILFS_GDS_MAX_SG, &offset);
	if (got <= 0) {
		kvfree(chunk->pages);
		return got ? (int)got : -EFAULT;
	}

	chunk->iter = *iter;
	chunk->len = (u32)got;
	chunk->nr = DIV_ROUND_UP(offset + got, PAGE_SIZE);

	err = sg_alloc_table(&chunk->table, chunk->nr, GFP_NOFS);
	if (err) {
		iov_iter_revert(iter, got);
		railfs_gds_unpin(chunk);
		return err;
	}

	railfs_gds_scatter(chunk, offset);
	return 0;
}

static int railfs_gds_read_chunk(struct railfs_options *opts, const char *path, loff_t pos, struct railfs_gds_chunk *chunk)
{
	struct railfs_conn *conn = railfs_pool_take(opts->pool);
	int got;

	if (!conn) {
		return -ERESTARTSYS;
	}
	got = railfs_read_sg(conn, path, pos, &chunk->table, chunk->len);

	railfs_pool_give(opts->pool, conn);
	return got;
}

static int railfs_gds_write_chunk(struct railfs_options *opts, const char *path, loff_t pos, struct railfs_gds_chunk *chunk)
{
	struct railfs_conn *conn = railfs_pool_take(opts->pool);
	int put;

	if (!conn) {
		return -ERESTARTSYS;
	}
	put = railfs_write_sg(conn, path, pos, &chunk->table, chunk->len, false);

	railfs_pool_give(opts->pool, conn);
	return put;
}

static ssize_t railfs_gds_transfer(struct kiocb *iocb, struct iov_iter *iter, bool writing)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	struct railfs_path *path;
	loff_t pos = iocb->ki_pos;
	ssize_t done = 0;
	int err = 0;

	if (!opts || !opts->pool) {
		return -ENOTCONN;
	}
	path = railfs_path_hold(inode);
	if (!path) {
		return -ENOENT;
	}

	while (iov_iter_count(iter)) {
		struct railfs_gds_chunk chunk;
		size_t want = min_t(size_t, iov_iter_count(iter), RAILFS_PAGE_SIZE);
		int moved;

		if (!writing) {
			loff_t available = i_size_read(inode) - pos;

			if (available <= 0) {
				break;
			}
			want = min_t(size_t, want, available);
		}

		err = railfs_gds_pin(iter, want, &chunk);
		if (err) {
			break;
		}

		moved = writing ? railfs_gds_write_chunk(opts, path->name, pos, &chunk) : railfs_gds_read_chunk(opts, path->name, pos, &chunk);
		railfs_gds_unpin(&chunk);

		if (moved < 0) {
			iov_iter_revert(iter, chunk.len);
			err = moved;
			break;
		}

		pos += moved;
		done += moved;

		if ((u32)moved < chunk.len) {
			iov_iter_revert(iter, chunk.len - moved);
			break;
		}
	}

	railfs_path_put(path);
	iocb->ki_pos = pos;
	return done ? done : err;
}

static ssize_t railfs_gds_move(struct kiocb *iocb, struct iov_iter *iter, bool writing)
{
	ssize_t result = -EOPNOTSUPP;
	unsigned int nofs;

	down_read(&railfs_nvfs_lock);
	if (railfs_nvfs && READ_ONCE(railfs_gds_on)) {
		nofs = memalloc_nofs_save();
		result = railfs_gds_transfer(iocb, iter, writing);
		memalloc_nofs_restore(nofs);
	}
	up_read(&railfs_nvfs_lock);
	return result;
}

ssize_t railfs_gds_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t end = iocb->ki_pos + iov_iter_count(to);
	int err;

	err = filemap_write_and_wait_range(mapping, iocb->ki_pos, end - 1);
	if (err) {
		return err;
	}

	return railfs_gds_move(iocb, to, false);
}

static int railfs_gds_drop_cache(struct address_space *mapping, loff_t start, loff_t end)
{
	int err = filemap_write_and_wait_range(mapping, start, end - 1);

	if (err) {
		return err;
	}

	return invalidate_inode_pages2_range(mapping, start >> PAGE_SHIFT, (end - 1) >> PAGE_SHIFT);
}

ssize_t railfs_gds_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	loff_t start;
	loff_t end;
	ssize_t done;
	int err;

	inode_lock(inode);

	done = generic_write_checks(iocb, from);
	if (done <= 0) {
		goto out;
	}

	err = file_modified(file);
	if (err) {
		done = err;
		goto out;
	}

	start = iocb->ki_pos;
	end = start + iov_iter_count(from);

	err = railfs_gds_drop_cache(file->f_mapping, start, end);
	if (err) {
		done = err;
		goto out;
	}

	done = railfs_gds_move(iocb, from, true);
	if (done <= 0) {
		goto out;
	}

	RAILFS_I(inode)->mine = true;
	if (iocb->ki_pos > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos);
	}

	invalidate_inode_pages2_range(file->f_mapping, start >> PAGE_SHIFT, (iocb->ki_pos - 1) >> PAGE_SHIFT);
out:
	inode_unlock(inode);
	return done > 0 ? generic_write_sync(iocb, done) : done;
}
