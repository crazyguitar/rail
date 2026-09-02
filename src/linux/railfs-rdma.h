/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_RDMA_H
#define RAILFS_RDMA_H

#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

#include "railfs-msg.h"

#define RAILFS_GDS_MAX_SG (RAILFS_PAGE_SIZE / PAGE_SIZE + 1)

/* Mirrors src/transport/rdma-data-channel.cc. The blob below is memcpy'd onto
 * the wire by the peer, so every field here is layout, not convenience.
 */
#define RAILFS_MAX_RAILS 2
#define RAILFS_CTS_SLOTS 512
#define RAILFS_CTS_BYTES 48

/* Marks immediate data as a clear-to-send rather than a payload, matching
 * kIsCts in src/transport/rdma-data-channel.cc.
 */
#define RAILFS_IS_CTS (1u << 31)
#define RAILFS_RING_BYTES (RAILFS_CTS_SLOTS * RAILFS_CTS_BYTES)

/* How many pages one rail can have on the wire at once. One, because a ranged
 * read offers a page and waits for it. A streamed fetch that raised this was
 * built twice and measured worse both times - see tune.md section 41 - and
 * each slot costs a megabyte of coherent landing memory per rail per
 * connection, because the peer chooses which rail carries a page and every
 * rail has to have somewhere to put it.
 */
#define RAILFS_STREAM_SLOTS 1

struct railfs_rail_wire {
	u8 gid[16];
	u32 qpn;
	u32 cts_rkey;
} __packed;

struct railfs_wire {
	u32 rails;
	u32 slots;
	u64 cts_addr;
	u32 mtu;
	u32 pad;
	struct railfs_rail_wire line[RAILFS_MAX_RAILS];
} __packed;

struct railfs_rdma;

int railfs_rdma_start(void);
void railfs_rdma_stop(void);

/* Builds a rail on the first active port and fills wire with what the peer
 * needs to reach it.
 */
struct railfs_rdma *railfs_rdma_open(struct railfs_wire *wire);
void railfs_rdma_close(struct railfs_rdma *rdma);

/* Drives the queue pair to ready against the peer named in wire. */
int railfs_rdma_meet(struct railfs_rdma *rdma, const struct railfs_wire *wire);

/* Tells the peer where to put the payload for key, waits for it to land, and
 * copies it out. Returns the byte count or a negative errno.
 */
int railfs_rdma_fetch(struct railfs_rdma *rdma, u64 key, void *buf, u32 len);

/* The same exchange split in two, so a caller can have RAILFS_STREAM_SLOTS of
 * them outstanding. Offer says where the page for key should land and returns
 * as soon as the request is on the wire; collect waits for that page and copies
 * it out. A slot may not be offered again until it has been collected.
 */
int railfs_rdma_offer(struct railfs_rdma *rdma, u32 slot, u64 key, u32 len);
int railfs_rdma_collect(struct railfs_rdma *rdma, u32 slot, void *buf, u32 len);

/* Waits for the peer to say where it wants the bytes, then writes them there.
 * Returns the byte count or a negative errno.
 */
int railfs_rdma_push(struct railfs_rdma *rdma, u64 key, const void *buf, u32 len);

/* The same push, reading out of the page cache rather than a staging copy.
 * The folio has to cover the whole length.
 */
int railfs_rdma_push_folios(struct railfs_rdma *rdma, u64 key, struct folio **folios, unsigned int nr, u32 len);

int railfs_rdma_fetch_sg(struct railfs_rdma *rdma, u64 key, struct sg_table *pages, u32 len);
int railfs_rdma_push_sg(struct railfs_rdma *rdma, u64 key, struct sg_table *pages, u32 len);

/* One clear-to-send record, written by rdma into the peer's ring. Layout is
 * wire format: struct Cts in src/transport/rdma-data-channel.cc.
 */
struct railfs_cts {
	u64 key;
	/* One per rail. A process repeats one virtual address here; this side
	 * registers per device and gets a different address on each, which is
	 * what the second address exists for.
	 */
	u64 addr[RAILFS_MAX_RAILS];
	u32 length;
	u32 slot;
	u32 rkey[RAILFS_MAX_RAILS];
	u32 seq;
} __packed;

#endif
