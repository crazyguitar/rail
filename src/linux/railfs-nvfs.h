/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_NVFS_H
#define RAILFS_NVFS_H

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>

struct request_queue;
struct request;
struct nvfs_rdma_info;

#define NVFS_IO_ERR -1
#define NVFS_BAD_REQ -2

enum nvfs_ft_bits {
	nvfs_ft_prep_sglist = 1ULL << 0,
	nvfs_ft_map_sglist = 1ULL << 1,
	nvfs_ft_is_gpu_page = 1ULL << 2,
	nvfs_ft_device_priority = 1ULL << 3,
	nvfs_ft_get_gpu_sglist_rdma_info = 1ULL << 4,
};

struct nvfs_dma_rw_ops {
	unsigned long long ft_bmap;
	int (*nvfs_blk_rq_map_sg)(struct request_queue *q, struct request *req, struct scatterlist *sglist);
	int (*nvfs_dma_map_sg_attrs)(struct device *device, struct scatterlist *sglist, int nents, enum dma_data_direction dma_dir,
				     unsigned long attrs);
	int (*nvfs_dma_unmap_sg)(struct device *device, struct scatterlist *sglist, int nents, enum dma_data_direction dma_dir);
	bool (*nvfs_is_gpu_page)(struct page *page);
	unsigned int (*nvfs_gpu_index)(struct page *page);
	unsigned int (*nvfs_device_priority)(struct device *dev, unsigned int gpu_index);
	int (*nvfs_get_gpu_sglist_rdma_info)(struct scatterlist *sglist, int nents, struct nvfs_rdma_info *rdma_infop);
};

int railfs_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops);
void railfs_unregister_nvfs_dma_ops(void);

#endif
