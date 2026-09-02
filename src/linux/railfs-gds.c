// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "railfs.h"
#include "railfs-gds.h"
#include "railfs-nvfs.h"
#include "railfs-tcp.h"

#define RAILFS_GDS_NEEDS (nvfs_ft_map_sglist | nvfs_ft_is_gpu_page)

static struct nvfs_dma_rw_ops *railfs_nvfs;
static bool railfs_gds_on = true;

module_param_named(gds, railfs_gds_on, bool, 0644);
MODULE_PARM_DESC(gds, "land GPUDirect Storage buffers straight from the fabric (default on)");

int railfs_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops)
{
	if (!ops || (ops->ft_bmap & RAILFS_GDS_NEEDS) != RAILFS_GDS_NEEDS) {
		return -EINVAL;
	}

	WRITE_ONCE(railfs_nvfs, ops);
	pr_info("railfs: nvidia-fs registered, gds is %s\n", READ_ONCE(railfs_gds_on) ? "on" : "off");
	return 0;
}
EXPORT_SYMBOL(railfs_register_nvfs_dma_ops);

void railfs_unregister_nvfs_dma_ops(void)
{
	WRITE_ONCE(railfs_nvfs, NULL);
	pr_info("railfs: nvidia-fs unregistered\n");
}
EXPORT_SYMBOL(railfs_unregister_nvfs_dma_ops);

static struct nvfs_dma_rw_ops *railfs_gds_ops(void)
{
	return READ_ONCE(railfs_gds_on) ? READ_ONCE(railfs_nvfs) : NULL;
}

int railfs_gds_map(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct nvfs_dma_rw_ops *ops = READ_ONCE(railfs_nvfs);
	int mapped;

	if (!ops) {
		return -ENODEV;
	}

	mapped = ops->nvfs_dma_map_sg_attrs(dev, sgl, nents, dir, 0);
	if (mapped == NVFS_BAD_REQ) {
		return -EINVAL;
	}

	return mapped < 0 ? -EIO : mapped;
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
	struct nvfs_dma_rw_ops *ops = railfs_gds_ops();
	struct iov_iter peek;
	struct page *page = NULL;
	struct page **pages = &page;
	size_t offset;
	bool gpu;

	if (!ops || !iov_iter_count(iter)) {
		return false;
	}

	peek = *iter;
	if (iov_iter_extract_pages(&peek, &pages, PAGE_SIZE, 1, 0, &offset) <= 0) {
		return false;
	}

	gpu = ops->nvfs_is_gpu_page(page);

	if (iov_iter_extract_will_pin(&peek)) {
		unpin_user_page(page);
	}

	return gpu;
}

struct railfs_gds_chunk {
	struct page **pages;
	unsigned int nr;
	struct sg_table table;
	u32 len;
	bool pinned;
};

static void railfs_gds_unpin(struct railfs_gds_chunk *chunk)
{
	if (chunk->table.sgl) {
		sg_free_table(&chunk->table);
	}

	if (chunk->pinned) {
		unpin_user_pages(chunk->pages, chunk->nr);
	}

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

	got = iov_iter_extract_pages(iter, &chunk->pages, want, RAILFS_GDS_MAX_SG, 0, &offset);
	if (got <= 0) {
		return got ? (int)got : -EFAULT;
	}

	chunk->pinned = iov_iter_extract_will_pin(iter);
	chunk->len = (u32)got;
	chunk->nr = DIV_ROUND_UP(offset + got, PAGE_SIZE);

	err = sg_alloc_table(&chunk->table, chunk->nr, GFP_KERNEL);
	if (err) {
		railfs_gds_unpin(chunk);
		return err;
	}

	railfs_gds_scatter(chunk, offset);
	return 0;
}

static int railfs_gds_read_chunk(struct railfs_options *opts, const char *path, loff_t pos, struct railfs_gds_chunk *chunk)
{
	struct railfs_conn *conn = railfs_pool_take(opts->pool);
	int got = railfs_read_sg(conn, path, pos, &chunk->table, chunk->len);

	railfs_pool_give(opts->pool, conn);
	return got;
}

static int railfs_gds_write_chunk(struct railfs_options *opts, const char *path, loff_t pos, struct railfs_gds_chunk *chunk)
{
	struct railfs_conn *conn = railfs_pool_take(opts->pool);
	int put = railfs_write_sg(conn, path, pos, &chunk->table, chunk->len, false);

	railfs_pool_give(opts->pool, conn);
	return put;
}

static ssize_t railfs_gds_move(struct kiocb *iocb, struct iov_iter *iter, bool writing)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	const char *path = inode->i_private;
	loff_t pos = iocb->ki_pos;
	ssize_t done = 0;
	int err = 0;

	if (!opts || !opts->pool || !path) {
		return -ENOTCONN;
	}

	while (iov_iter_count(iter)) {
		struct railfs_gds_chunk chunk;
		size_t want = min_t(size_t, iov_iter_count(iter), RAILFS_PAGE_SIZE);
		int moved;

		err = railfs_gds_pin(iter, want, &chunk);
		if (err) {
			break;
		}

		moved = writing ? railfs_gds_write_chunk(opts, path, pos, &chunk) : railfs_gds_read_chunk(opts, path, pos, &chunk);
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

	iocb->ki_pos = pos;
	return done ? done : err;
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
	return done;
}
