/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_GDS_H
#define RAILFS_GDS_H

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/fs.h>
#include <linux/scatterlist.h>
#include <linux/uio.h>

bool railfs_gds_claims(const struct iov_iter *iter);
ssize_t railfs_gds_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t railfs_gds_write_iter(struct kiocb *iocb, struct iov_iter *from);

int railfs_gds_map(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir);
void railfs_gds_unmap(struct device *dev, struct scatterlist *sgl, int nents, enum dma_data_direction dir);

#endif
