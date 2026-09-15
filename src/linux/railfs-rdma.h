/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_RDMA_H
#define RAILFS_RDMA_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

#include <rdma/ib_verbs.h>

#include "railfs-compat.h"
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
/* Marks immediate data as a control frame; a payload carries neither bit. */
#define RAILFS_IS_CTRL (1u << 30)
#define RAILFS_RING_BYTES (RAILFS_CTS_SLOTS * RAILFS_CTS_BYTES)

/* Pages one connection can have on the wire at once, each a landing page on
 * every rail since the peer picks the rail. Matches what the connection
 * admits, so an admitted transfer always finds a slot.
 */
#define RAILFS_STREAM_SLOTS RAILFS_CONN_DEPTH

struct railfs_rail_wire {
	u8 gid[16];
	u32 qpn;
	u32 cts_rkey;
	/* This side's reply ring, per rail: the same memory has a different DMA
	 * address on every device.
	 */
	u64 ctrl_addr;
	u32 ctrl_rkey;
	u32 pad;
} __packed;

struct railfs_wire {
	u32 rails;
	u32 slots;
	u64 cts_addr;
	u32 mtu;
	u32 pad;
	u32 ctrl_slots;
	u32 req_bytes;
	u32 reply_bytes;
	u32 pad2;
	struct railfs_rail_wire line[RAILFS_MAX_RAILS];
} __packed;

/* One clear-to-send record, written by rdma into the peer's ring. Layout is
 * wire format: struct Cts in src/transport/rdma-data-channel.cc.
 */
struct railfs_cts {
	u64 key;
	/* One per rail: this side registers per device and gets a different
	 * address on each.
	 */
	u64 addr[RAILFS_MAX_RAILS];
	u32 length;
	u32 slot;
	u32 rkey[RAILFS_MAX_RAILS];
	u32 seq;
} __packed;

struct railfs_rdma;

/* One write waiting for its clear-to-send, matched by key. Shared with the
 * send completion, which may outlive the caller, so it is counted not owned.
 */
struct railfs_push {
	struct list_head link;
	struct kref ref;
	u64 key;
	struct railfs_cts cts;
	struct completion asked;
	struct completion sent;
	struct ib_cqe cqe;
	int asked_err;
	int sent_err;
};

int railfs_rdma_start(void);
void railfs_rdma_stop(void);

/* Builds a rail on every active port and fills wire with what the peer needs
 * to reach them.
 */
struct railfs_rdma *railfs_rdma_open(struct railfs_wire *wire);
void railfs_rdma_close(struct railfs_rdma *rdma);

/* Drives the queue pairs to ready against the peer named in wire. */
int railfs_rdma_meet(struct railfs_rdma *rdma, const struct railfs_wire *wire);

/* Flushes every queue pair and fails everything waiting. The rail does not
 * come back; the connection it belongs to is replaced.
 */
void railfs_rdma_break(struct railfs_rdma *rdma);

/* A read in two halves: begin offers a slot for key, end waits for the page
 * (*at is NULL for gpu pages). Every begin owes an end, every end a release.
 */
int railfs_rdma_fetch_begin(struct railfs_rdma *rdma, u64 key, u32 len, struct sg_table *gpu, u32 *slot);
int railfs_rdma_fetch_end(struct railfs_rdma *rdma, u32 slot, const void **at);
void railfs_rdma_slot_release(struct railfs_rdma *rdma, u32 slot);

/* A slot whose page is still coming but nobody waits for: released by the
 * landing itself, not before, so a later read cannot be handed the old page.
 */
void railfs_rdma_slot_orphan(struct railfs_rdma *rdma, u32 slot);

/* A write in two halves: expect registers the push before the request goes
 * out, push waits for the clear-to-send, forget drops the caller's reference.
 */
struct railfs_push *railfs_rdma_expect(struct railfs_rdma *rdma, u64 key);
void railfs_rdma_forget(struct railfs_rdma *rdma, struct railfs_push *push);
int railfs_rdma_push(struct railfs_rdma *rdma, struct railfs_push *push, const void *buf, u32 len);
int railfs_rdma_push_folios(struct railfs_rdma *rdma, struct railfs_push *push, struct folio **folios, unsigned int nr, u32 len);
int railfs_rdma_push_sg(struct railfs_rdma *rdma, struct railfs_push *push, struct sg_table *pages, u32 len);

/* Control frames over the fabric: a request slot per call on one rail, the
 * reply at twice that index in this side's ring. The rail moves bytes and
 * reports arrivals and failed sends in softirq; a callback returning nonzero
 * is a protocol error that strands the rail.
 */
typedef int (*railfs_ctrl_fn)(void *ctx, u32 line, u32 slot, int err);
void railfs_rdma_ctrl_watch(struct railfs_rdma *rdma, railfs_ctrl_fn on_reply, railfs_ctrl_fn on_sent, void *ctx);

/* The next rail in turn and a free slot on it; waits killably. */
int railfs_rdma_ctrl_take(struct railfs_rdma *rdma, u32 *line, u32 *slot);
void railfs_rdma_ctrl_give(struct railfs_rdma *rdma, u32 line, u32 slot);

int railfs_rdma_ctrl_send(struct railfs_rdma *rdma, u32 line, u32 slot, const void *frame, u32 len);

/* The reply in a slot, header checked; valid until the slot is given back. */
int railfs_rdma_ctrl_reply(struct railfs_rdma *rdma, u32 line, u32 slot, u16 *type, u8 **payload, u32 *len);

#endif
