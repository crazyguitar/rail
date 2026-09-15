// SPDX-License-Identifier: GPL-2.0
//
// Kernel RDMA: a rail per active port, each its own device with its own
// landing memory, so a clear-to-send names an address per rail. A read holds
// a slot on every rail; a write is matched to its clear-to-send by key.

#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/sched/signal.h>

#include <rdma/ib_cache.h>
#include <rdma/ib_verbs.h>

#include "railfs-rdma.h"
#include "railfs-gds.h"
#include "railfs-proto.h"
#include "rdma-link-rate.h"

// Work requests a queue pair may hold each way. Offers and pushes together
// never exceed the slots, and fences are one at a time, so this is generous.
#define RAILFS_CQ_DEPTH 128

// The landing region has to hold whatever page size the session negotiated, so
// this is that number rather than one of its own. They were separate constants
// that had to agree, and raising the negotiated one alone made every read fail
// with EAGAIN once the daemon started sending pages the region could not hold.
#define RAILFS_PAGE_BYTES RAILFS_PAGE_SIZE

// Folios one send may carry, each its own scatter entry. The adapters here
// report thirty; a page-sized flush of megabyte folios needs four.
#define RAILFS_MAX_SGE 8

// Receives each rail keeps posted. Payloads and clear-to-sends both consume
// one, and a rail with none left stalls the peer rather than report anything.
#define RAILFS_RECV_DEPTH (RAILFS_CQ_DEPTH / 2)

/* How long an exchange waits before deciding the peer is not coming back. The
 * fabric answers in microseconds, so anything approaching this is a rail that
 * has stopped rather than one that is busy; arming a region is quicker because
 * it is local work, not a round trip.
 */
#define RAILFS_WAIT_MS 30000
#define RAILFS_ARM_WAIT_MS 5000

// Whether anyone still waits for the page a slot was offered for. The landing
// and the caller's departure race, and exactly one of them gives the slot
// back, so they meet on a cmpxchg rather than on a plain flag.
#define RAILFS_SLOT_LIVE 0
#define RAILFS_SLOT_ORPHAN 1
#define RAILFS_SLOT_LANDED 2

// One page offered to the peer and not yet collected. Every slot has its own
// completion because the peer answers them in whatever order its reader
// reaches them, and a single completion cannot say which one arrived.
struct railfs_slot {
	struct completion done;
	int err;
	// Which rail carried it. The peer picks one per page, so where the bytes
	// are is not known until the completion says.
	u32 line;
	bool gpu;
	atomic_t state;
};

// One slot's page on one rail: its own allocation, so the pages need not be
// contiguous, under one region per rail that maps them in slot order.
struct railfs_landing {
	void *cpu;
	dma_addr_t dma;
};

// One port. Everything that belongs to a device rather than to the connection:
// its own protection domain, queue pair and landing memory, because two ports
// on this hardware are two separate ib_devices and memory registered with one
// means nothing to the other.
struct railfs_line {
	struct railfs_rdma *rail;
	struct ib_device *device;
	u32 port;
	u32 index;
	u32 rate_mbps;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	// Where a fetched page lands, one per slot. Registered once and copied
	// out of: a registration per read would cost more than the copy.
	struct railfs_landing landing[RAILFS_STREAM_SLOTS];
	struct ib_mr *landing_mr;
	// Where an outgoing clear-to-send is staged, kept apart from the ring the
	// peer writes its own requests into: sharing them races.
	void *offers;
	dma_addr_t offers_dma;
	// A managed completion queue dispatches through wr_cqe->done, not wr_id.
	// Leaving these unset is a null dereference in softirq context.
	struct ib_cqe recv_cqe;
	struct ib_cqe cts_cqe;
	struct ib_cqe arm_cqe;
	struct completion armed;
	int arm_err;
	struct ib_mr *gpu_mr;
	struct sg_table gpu_table;
	unsigned int gpu_nents;
	enum dma_data_direction gpu_dir;
};

// One connection's rails. The peer spreads pages across them and says which it
// used in the immediate, so they need no order between them.
struct railfs_rdma {
	struct railfs_line line[RAILFS_MAX_RAILS];
	// How many were built, which is what close has to give back. Kept apart
	// from how many are usable: a peer with fewer rails leaves the rest built
	// but never connected, and counting only those would leak the others.
	u32 lines;
	// How many both ends have, so how many may be offered on.
	u32 live;
	// The ring the peer drops its own clear-to-sends into. On the first rail
	// only: the wire carries a single ring address, and the peer writes it
	// over its first rail whatever it sends payloads over.
	void *ring;
	dma_addr_t ring_dma;
	struct ib_mr *ring_mr;
	struct railfs_wire peer;
	struct railfs_slot slot[RAILFS_STREAM_SLOTS];
	DECLARE_BITMAP(free, RAILFS_STREAM_SLOTS);
	wait_queue_head_t slot_room;
	// Writes waiting for the peer to name a landing, by key.
	spinlock_t pushes_lock;
	struct list_head pushes;
	// One gpu transfer at a time: the gpu region is per rail, not per slot.
	struct mutex gds_lock;
	// One breaker at a time: every transfer on the connection can time out.
	struct mutex break_lock;
	bool broken;
	// The peer notices a record by its sequence changing, so this counts
	// across the whole connection rather than per slot.
	atomic_t seq;
	// Which rail the next push writes over.
	atomic_t turn;
};

static void *railfs_landing_at(struct railfs_line *line, u32 slot)
{
	return line->landing[slot].cpu;
}

// Where the peer writes a slot's page: the region is one span of slots, so a
// slot is its offset from the base.
static u64 railfs_landing_remote_at(struct railfs_line *line, u32 slot)
{
	return line->landing_mr->iova + (u64)slot * RAILFS_PAGE_BYTES;
}

// How many rails both ends have. The peer resizes to whatever this side
// advertises, so a page may only be offered on a rail both of them built.
static u32 railfs_shared_lines(struct railfs_rdma *rail)
{
	return rail->live;
}

struct railfs_device {
	struct list_head link;
	struct ib_device *device;
};

static LIST_HEAD(railfs_known);
static DEFINE_MUTEX(railfs_known_lock);

static int railfs_rdma_probe(struct ib_device *device);
static int railfs_arm_region(struct railfs_line *line, struct ib_mr *mr);
static void railfs_rdma_forget_device(struct ib_device *device, void *data);

static struct ib_client railfs_ib_client = {
	.name = "railfs",
	.add = railfs_rdma_probe,
	.remove = railfs_rdma_forget_device,
};

static int railfs_rdma_probe(struct ib_device *device)
{
	struct railfs_device *known = kzalloc(sizeof(*known), GFP_KERNEL);

	if (!known) {
		return -ENOMEM;
	}

	known->device = device;
	ib_set_client_data(device, &railfs_ib_client, known);
	mutex_lock(&railfs_known_lock);
	list_add_tail(&known->link, &railfs_known);
	mutex_unlock(&railfs_known_lock);
	return 0;
}

static void railfs_rdma_forget_device(struct ib_device *device, void *data)
{
	struct railfs_device *known = data;

	mutex_lock(&railfs_known_lock);
	list_del(&known->link);
	mutex_unlock(&railfs_known_lock);
	kfree(known);
}

// A port worth building a rail on, as found: the line itself is far bigger
// than what choosing between ports needs.
struct railfs_port {
	struct ib_device *device;
	u32 port;
	u32 rate_mbps;
};

static bool railfs_line_precedes(const struct railfs_port *a, const struct railfs_port *b)
{
	int names;

	if (a->rate_mbps != b->rate_mbps) {
		return a->rate_mbps > b->rate_mbps;
	}
	names = strcmp(a->device->name, b->device->name);
	return names ? names < 0 : a->port < b->port;
}

// Match userspace rail ordering.
static u32 railfs_active_lines(struct railfs_port *out, u32 room)
{
	struct railfs_device *known;
	u32 found = 0;

	mutex_lock(&railfs_known_lock);

	list_for_each_entry(known, &railfs_known, link) {
		struct ib_device *device = known->device;
		u32 port;

		rdma_for_each_port(device, port) {
			struct ib_port_attr attr;
			struct railfs_port candidate = { .device = device, .port = port };
			u32 position;
			u32 shift;

			if (ib_query_port(device, port, &attr)) {
				continue;
			}

			if (attr.state != IB_PORT_ACTIVE) {
				continue;
			}

			candidate.rate_mbps = rail_rdma_rate_mbps(attr.active_speed, attr.active_width);
			for (position = 0; position < found; position++) {
				if (railfs_line_precedes(&candidate, &out[position])) {
					break;
				}
			}
			if (position == room) {
				continue;
			}
			for (shift = min(found, room - 1); shift > position; shift--) {
				out[shift] = out[shift - 1];
			}
			out[position] = candidate;
			found = min(found + 1, room);
		}
	}

	mutex_unlock(&railfs_known_lock);
	return found;
}

static int railfs_qp_to_init(struct ib_qp *qp, u32 port)
{
	struct ib_qp_attr attr = {};
	int mask;

	attr.qp_state = IB_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = port;
	attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;

	mask = IB_QP_STATE | IB_QP_PKEY_INDEX | IB_QP_PORT | IB_QP_ACCESS_FLAGS;
	return ib_modify_qp(qp, &attr, mask);
}

// RoCE needs the destination mac, not only the gid. Userspace verbs resolve it
// behind ibv_modify_qp; in the kernel the address handle carries it, and an
// unset one is a write nothing answers - reported as retries exceeded rather
// than as a bad address.
//
// These are RoCE v1 link-local gids, so the mac is the EUI-64 in the low eight
// bytes: flip the universal bit and drop the ff:fe in the middle.
static void railfs_dmac_from_gid(const u8 *gid, u8 *dmac)
{
	dmac[0] = gid[8] ^ 0x02;
	dmac[1] = gid[9];
	dmac[2] = gid[10];
	dmac[3] = gid[13];
	dmac[4] = gid[14];
	dmac[5] = gid[15];
}

// Named for the peer: its gid and queue pair number are what make this rail
// point somewhere rather than at itself.
static int railfs_qp_to_rtr(struct railfs_line *line, const u8 *peer_gid, u32 peer_qpn, u32 mtu)
{
	struct ib_qp_attr attr = {};
	union ib_gid gid;
	int mask;

	memcpy(gid.raw, peer_gid, sizeof(gid.raw));

	attr.qp_state = IB_QPS_RTR;
	attr.path_mtu = mtu ? (enum ib_mtu)mtu : IB_MTU_1024;
	attr.dest_qp_num = peer_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;

	attr.ah_attr.type = rdma_ah_find_type(line->device, line->port);
	rdma_ah_set_port_num(&attr.ah_attr, line->port);
	rdma_ah_set_grh(&attr.ah_attr, &gid, 0, 0, 64, 0);
	rdma_ah_set_dgid_raw(&attr.ah_attr, gid.raw);

	if (attr.ah_attr.type == RDMA_AH_ATTR_TYPE_ROCE) {
		railfs_dmac_from_gid(peer_gid, attr.ah_attr.roce.dmac);
	}

	mask = IB_QP_STATE | IB_QP_AV | IB_QP_PATH_MTU | IB_QP_DEST_QPN | IB_QP_RQ_PSN | IB_QP_MAX_DEST_RD_ATOMIC | IB_QP_MIN_RNR_TIMER;
	return ib_modify_qp(line->qp, &attr, mask);
}

static int railfs_qp_to_rts(struct ib_qp *qp)
{
	struct ib_qp_attr attr = {};
	int mask;

	attr.qp_state = IB_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 1;

	mask = IB_QP_STATE | IB_QP_TIMEOUT | IB_QP_RETRY_CNT | IB_QP_RNR_RETRY | IB_QP_SQ_PSN | IB_QP_MAX_QP_RD_ATOMIC;
	return ib_modify_qp(qp, &attr, mask);
}

// One page per slot and one region over all of them. A page is an order-8
// allocation the allocator can usually find; the span would need them
// physically contiguous, which a busy machine cannot promise.
static int railfs_landing_open(struct railfs_line *line)
{
	struct sg_table table;
	struct scatterlist *sg;
	struct ib_mr *mr;
	u32 slot;
	int mapped;
	int err;

	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		struct railfs_landing *landing = &line->landing[slot];

		landing->cpu = dma_alloc_coherent(line->device->dma_device, RAILFS_PAGE_BYTES, &landing->dma, GFP_KERNEL);
		if (!landing->cpu) {
			return -ENOMEM;
		}
	}

	mr = ib_alloc_mr(line->pd, IB_MR_TYPE_MEM_REG, RAILFS_STREAM_SLOTS);
	if (IS_ERR(mr)) {
		return PTR_ERR(mr);
	}
	line->landing_mr = mr;

	err = sg_alloc_table(&table, RAILFS_STREAM_SLOTS, GFP_KERNEL);
	if (err) {
		return err;
	}

	for_each_sg(table.sgl, sg, RAILFS_STREAM_SLOTS, slot) {
		sg_dma_address(sg) = line->landing[slot].dma;
		sg_dma_len(sg) = RAILFS_PAGE_BYTES;
	}

	mapped = ib_map_mr_sg(mr, table.sgl, RAILFS_STREAM_SLOTS, NULL, RAILFS_PAGE_BYTES);
	sg_free_table(&table);
	if (mapped != RAILFS_STREAM_SLOTS) {
		return mapped < 0 ? mapped : -EINVAL;
	}

	return 0;
}

// Drained before it goes: a completion still in flight would otherwise reach
// railfs_on_landed after the queue pair is gone and repost a receive on it.
static void railfs_line_close(struct railfs_line *line)
{
	u32 slot;

	if (line->qp) {
		ib_drain_qp(line->qp);
		ib_destroy_qp(line->qp);
		line->qp = NULL;
	}

	if (line->gpu_mr) {
		ib_dereg_mr(line->gpu_mr);
	}

	if (line->gpu_table.sgl) {
		sg_free_table(&line->gpu_table);
	}

	if (line->landing_mr) {
		ib_dereg_mr(line->landing_mr);
	}

	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		struct railfs_landing *landing = &line->landing[slot];

		if (landing->cpu) {
			dma_free_coherent(line->device->dma_device, RAILFS_PAGE_BYTES, landing->cpu, landing->dma);
		}
	}

	if (line->offers) {
		dma_free_coherent(line->device->dma_device, RAILFS_STREAM_SLOTS * RAILFS_CTS_BYTES, line->offers, line->offers_dma);
	}

	if (line->cq) {
		ib_free_cq(line->cq);
	}

	if (line->pd) {
		ib_dealloc_pd(line->pd);
	}
}

void railfs_rdma_close(struct railfs_rdma *rail)
{
	u32 i;

	if (!rail) {
		return;
	}

	// Every queue pair goes quiet before the ring does: a peer write to it can
	// still be in flight, and would land in freed memory.
	for (i = 0; i < rail->lines; i++) {
		if (rail->line[i].qp) {
			ib_drain_qp(rail->line[i].qp);
		}
	}

	// The ring belongs to the first rail's protection domain, so it goes back
	// before that rail does.
	if (rail->ring_mr) {
		ib_dereg_mr(rail->ring_mr);
	}

	if (rail->ring) {
		dma_free_coherent(rail->line[0].device->dma_device, RAILFS_RING_BYTES, rail->ring, rail->ring_dma);
	}

	for (i = 0; i < rail->lines; i++) {
		railfs_line_close(&rail->line[i]);
	}

	kfree(rail);
}

// Spread over whatever the device offers, rather than nought for all of them:
//
//   conn0 - line0 - cq --> vector 0    cpu0   cpu1   cpu2  ...  cpu19
//   conn0 - line1 - cq --> vector 1    +--+   +--+   +--+       +--+
//   conn1 - line0 - cq --> vector 2    |##|   |##|   |##|       |##|
//   conn1 - line1 - cq --> vector 3    +--+   +--+   +--+       +--+
//    ...                               comp0  comp1  comp2      comp19
//
// Counted per module rather than per mount, so a second mount carries on from
// where the first left off instead of piling onto the vectors it already used.
static atomic_t railfs_vector = ATOMIC_INIT(0);

static u32 railfs_next_vector(struct ib_device *device)
{
	u32 vectors = device->num_comp_vectors;

	if (vectors < 1) {
		return 0;
	}

	return (u32)atomic_fetch_inc(&railfs_vector) % vectors;
}

static int railfs_line_open(struct railfs_line *line)
{
	struct ib_qp_init_attr init = {};
	int err;

	line->pd = ib_alloc_pd(line->device, 0);
	if (IS_ERR(line->pd)) {
		err = PTR_ERR(line->pd);
		line->pd = NULL;
		goto out;
	}

	// Sends and receives complete on the same queue, so it holds both.
	line->cq = ib_alloc_cq(line->device, NULL, 2 * RAILFS_CQ_DEPTH, railfs_next_vector(line->device), IB_POLL_SOFTIRQ);
	if (IS_ERR(line->cq)) {
		err = PTR_ERR(line->cq);
		line->cq = NULL;
		goto out;
	}

	init.send_cq = line->cq;
	init.recv_cq = line->cq;
	init.qp_type = IB_QPT_RC;
	init.cap.max_send_wr = RAILFS_CQ_DEPTH;
	init.cap.max_recv_wr = RAILFS_CQ_DEPTH;
	init.cap.max_send_sge = RAILFS_MAX_SGE;
	init.cap.max_recv_sge = 1;

	line->qp = ib_create_qp(line->pd, &init);
	if (IS_ERR(line->qp)) {
		err = PTR_ERR(line->qp);
		line->qp = NULL;
		goto out;
	}

	err = railfs_landing_open(line);
	if (err) {
		goto out;
	}

	line->offers = dma_alloc_coherent(line->device->dma_device, RAILFS_STREAM_SLOTS * RAILFS_CTS_BYTES, &line->offers_dma, GFP_KERNEL);
	if (!line->offers) {
		err = -ENOMEM;
		goto out;
	}

	init_completion(&line->armed);
	err = railfs_qp_to_init(line->qp, line->port);
out:
	return err;
}

// The ring the peer writes its requests into. One for the connection, on the
// first rail, because the wire carries a single ring address.
static int railfs_ring_open(struct railfs_rdma *rail)
{
	struct railfs_line *line = &rail->line[0];
	struct scatterlist sg;
	struct ib_mr *mr;
	int mapped;

	rail->ring = dma_alloc_coherent(line->device->dma_device, RAILFS_RING_BYTES, &rail->ring_dma, GFP_KERNEL);
	if (!rail->ring) {
		return -ENOMEM;
	}

	mr = ib_alloc_mr(line->pd, IB_MR_TYPE_MEM_REG, 1);
	if (IS_ERR(mr)) {
		return PTR_ERR(mr);
	}

	rail->ring_mr = mr;

	sg_init_table(&sg, 1);
	sg_dma_address(&sg) = rail->ring_dma;
	sg_dma_len(&sg) = RAILFS_RING_BYTES;

	mapped = ib_map_mr_sg(mr, &sg, 1, NULL, RAILFS_RING_BYTES);
	if (mapped != 1) {
		return mapped < 0 ? mapped : -EINVAL;
	}

	return 0;
}

struct railfs_rdma *railfs_rdma_open(struct railfs_wire *wire)
{
	struct railfs_port found[RAILFS_MAX_RAILS] = {};
	struct railfs_rdma *rail;
	union ib_gid gid;
	u32 lines;
	u32 slot;
	u32 i;
	int err;

	lines = railfs_active_lines(found, RAILFS_MAX_RAILS);
	if (!lines) {
		return ERR_PTR(-ENODEV);
	}

	rail = kzalloc(sizeof(*rail), GFP_KERNEL);
	if (!rail) {
		return ERR_PTR(-ENOMEM);
	}

	// Usable from the first failure on: close and break walk these whatever
	// was built.
	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		init_completion(&rail->slot[slot].done);
		atomic_set(&rail->slot[slot].state, RAILFS_SLOT_LIVE);
	}

	bitmap_fill(rail->free, RAILFS_STREAM_SLOTS);
	init_waitqueue_head(&rail->slot_room);
	spin_lock_init(&rail->pushes_lock);
	INIT_LIST_HEAD(&rail->pushes);
	mutex_init(&rail->gds_lock);
	mutex_init(&rail->break_lock);

	for (i = 0; i < lines; i++) {
		rail->line[i].device = found[i].device;
		rail->line[i].port = found[i].port;
		rail->line[i].rate_mbps = found[i].rate_mbps;
		rail->line[i].rail = rail;
		rail->line[i].index = i;
	}

	// Counted before anything can fail, so close gives back whatever was
	// built rather than only what a later count would admit to.
	rail->lines = lines;

	for (i = 0; i < lines; i++) {
		err = railfs_line_open(&rail->line[i]);
		if (err) {
			goto fail;
		}
	}

	err = railfs_ring_open(rail);
	if (err) {
		goto fail;
	}

	memset(wire, 0, sizeof(*wire));
	wire->rails = lines;
	wire->slots = RAILFS_CTS_SLOTS;
	wire->cts_addr = (u64)rail->ring_dma;
	wire->mtu = IB_MTU_1024;

	for (i = 0; i < lines; i++) {
		err = rdma_query_gid(rail->line[i].device, rail->line[i].port, 0, &gid);
		if (err) {
			goto fail;
		}

		memcpy(wire->line[i].gid, gid.raw, sizeof(wire->line[i].gid));
		wire->line[i].qpn = rail->line[i].qp->qp_num;
		// The wire carries one ring address, so only the first rail's domain
		// can hold it. A later rail advertises no key rather than one that
		// would mean nothing there.
		wire->line[i].cts_rkey = i == 0 ? rail->ring_mr->rkey : 0;

		pr_info("railfs: rdma rail %u on %s port %u, qp %u\n", i, rail->line[i].device->name, rail->line[i].port,
			rail->line[i].qp->qp_num);
	}

	return rail;

fail:
	railfs_rdma_close(rail);
	return ERR_PTR(err);
}

static void railfs_on_armed(struct ib_cq *cq, struct ib_wc *wc)
{
	struct railfs_line *line = container_of(wc->wr_cqe, struct railfs_line, arm_cqe);

	line->arm_err = wc->status == IB_WC_SUCCESS ? 0 : -EIO;
	if (wc->status != IB_WC_SUCCESS) {
		pr_err("railfs: region would not arm: %s\n", ib_wc_status_msg(wc->status));
	}
	complete(&line->armed);
}

// A send work request, so the queue pair has to be ready. Fences share one
// completion per rail and run one at a time: at meet, then under the gds lock.
static int railfs_fence(struct railfs_line *line, struct ib_send_wr *wr)
{
	const struct ib_send_wr *bad;
	int err;

	line->arm_cqe.done = railfs_on_armed;
	line->arm_err = 0;
	reinit_completion(&line->armed);

	wr->wr_cqe = &line->arm_cqe;
	wr->send_flags = IB_SEND_SIGNALED;

	err = ib_post_send(line->qp, wr, &bad);
	if (err) {
		return err;
	}

	if (!wait_for_completion_timeout(&line->armed, msecs_to_jiffies(RAILFS_ARM_WAIT_MS))) {
		return -ETIMEDOUT;
	}

	return line->arm_err;
}

static int railfs_arm(struct railfs_line *line, struct ib_mr *mr, int access)
{
	struct ib_reg_wr reg = {};

	reg.wr.opcode = IB_WR_REG_MR;
	reg.mr = mr;
	reg.key = mr->rkey;
	reg.access = access;

	return railfs_fence(line, &reg.wr);
}

static int railfs_arm_region(struct railfs_line *line, struct ib_mr *mr)
{
	return railfs_arm(line, mr, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE);
}

static int railfs_disarm(struct railfs_line *line, struct ib_mr *mr)
{
	struct ib_send_wr inv = {};

	inv.opcode = IB_WR_LOCAL_INV;
	inv.ex.invalidate_rkey = mr->rkey;

	return railfs_fence(line, &inv);
}

static int railfs_gpu_prepare(struct railfs_line *line)
{
	int err;

	if (line->gpu_mr) {
		return 0;
	}

	line->gpu_mr = ib_alloc_mr(line->pd, IB_MR_TYPE_MEM_REG, RAILFS_GDS_MAX_SG);
	if (IS_ERR(line->gpu_mr)) {
		err = PTR_ERR(line->gpu_mr);
		line->gpu_mr = NULL;
		return err;
	}

	err = sg_alloc_table(&line->gpu_table, RAILFS_GDS_MAX_SG, GFP_NOFS);
	if (err) {
		ib_dereg_mr(line->gpu_mr);
		line->gpu_mr = NULL;
		return err;
	}

	return 0;
}

static int railfs_gpu_copy(struct railfs_line *line, struct sg_table *pages, unsigned int nents)
{
	struct scatterlist *src = pages->sgl;
	struct scatterlist *dst;
	unsigned int i;

	if (nents > RAILFS_GDS_MAX_SG) {
		return -EMSGSIZE;
	}

	for_each_sg(line->gpu_table.sgl, dst, nents, i) {
		sg_set_page(dst, sg_page(src), src->length, src->offset);
		src = sg_next(src);
	}

	line->gpu_nents = nents;
	return 0;
}

static int railfs_gpu_bind(struct railfs_line *line, struct sg_table *pages, enum dma_data_direction dir, int access)
{
	struct ib_mr *mr;
	int nents = pages->nents;
	int mapped;
	int err;

	err = railfs_gpu_prepare(line);
	if (err) {
		return err;
	}
	mr = line->gpu_mr;

	err = railfs_gpu_copy(line, pages, nents);
	if (err) {
		return err;
	}

	mapped = railfs_gds_map(line->device->dma_device, line->gpu_table.sgl, nents, dir);
	if (mapped != nents) {
		line->gpu_nents = 0;
		return mapped < 0 ? mapped : -EIO;
	}

	line->gpu_dir = dir;

	ib_update_fast_reg_key(mr, ib_inc_rkey(mr->rkey));

	mapped = ib_map_mr_sg(mr, line->gpu_table.sgl, nents, NULL, PAGE_SIZE);
	if (mapped != nents) {
		err = mapped < 0 ? mapped : -EIO;
		goto unmap;
	}

	err = railfs_arm(line, mr, access);
	if (err) {
		WRITE_ONCE(line->rail->broken, true);
		ib_drain_qp(line->qp);
		goto unmap;
	}

	return 0;

unmap:
	railfs_gds_unmap(line->device->dma_device, line->gpu_table.sgl, nents, dir);
	line->gpu_nents = 0;
	return err;
}

static void railfs_gpu_unbind(struct railfs_line *line)
{
	if (!line->gpu_nents) {
		return;
	}

	if (READ_ONCE(line->rail->broken) || railfs_disarm(line, line->gpu_mr)) {
		WRITE_ONCE(line->rail->broken, true);
		ib_drain_qp(line->qp);
	}

	railfs_gds_unmap(line->device->dma_device, line->gpu_table.sgl, line->gpu_nents, line->gpu_dir);
	line->gpu_nents = 0;
}

static void railfs_on_landed(struct ib_cq *cq, struct ib_wc *wc);

static void railfs_push_free(struct kref *ref)
{
	kfree(container_of(ref, struct railfs_push, ref));
}

static void railfs_push_put(struct railfs_push *push)
{
	kref_put(&push->ref, railfs_push_free);
}

// A payload write consumes a receive, so every rail keeps a stock of them. A
// rail that runs out does not fail: the peer retries until it is told to stop,
// which reads as a stall rather than as an error.
static int railfs_stock(struct railfs_line *line, u32 want)
{
	const struct ib_recv_wr *bad;
	struct ib_recv_wr rq = {};
	u32 i;
	int err;

	line->recv_cqe.done = railfs_on_landed;
	rq.wr_cqe = &line->recv_cqe;
	rq.num_sge = 0;

	for (i = 0; i < want; i++) {
		err = ib_post_recv(line->qp, &rq, &bad);
		if (err) {
			return err;
		}
	}

	return 0;
}

int railfs_rdma_meet(struct railfs_rdma *rail, const struct railfs_wire *wire)
{
	u32 shared;
	u32 i;
	int err;

	if (wire->rails < 1) {
		return -EPROTO;
	}

	rail->peer = *wire;
	shared = wire->rails < rail->lines ? wire->rails : rail->lines;

	for (i = 0; i < shared; i++) {
		struct railfs_line *line = &rail->line[i];

		err = railfs_qp_to_rtr(line, wire->line[i].gid, wire->line[i].qpn, wire->mtu);
		if (err) {
			return err;
		}

		err = railfs_qp_to_rts(line->qp);
		if (err) {
			return err;
		}

		// ib_map_mr_sg only stages the mapping. Until this work request lands
		// the region is not live on the device and its rkey means nothing,
		// which the peer reports as a protection error rather than a bad key.
		err = railfs_arm_region(line, line->landing_mr);
		if (err) {
			pr_err("railfs: could not arm the landing region on rail %u: %d\n", i, err);
			return err;
		}

		if (i == 0) {
			err = railfs_arm_region(line, rail->ring_mr);
			if (err) {
				pr_err("railfs: could not arm the ring: %d\n", err);
				return err;
			}
		}

		err = railfs_stock(line, RAILFS_RECV_DEPTH);
		if (err) {
			pr_err("railfs: could not stock receives on rail %u: %d\n", i, err);
			return err;
		}

		pr_info("railfs: rdma rail %u up, local qp %u to peer qp %u\n", i, line->qp->qp_num, wire->line[i].qpn);
	}

	// Rails the peer did not build carry nothing, and offering on one would
	// name a queue pair that never reached ready. They stay counted in lines
	// so close still gives them back.
	rail->live = shared;
	return 0;
}

// A push that will never be answered, released with the error rather than left
// to time out. Under the pushes lock.
static void railfs_push_fail(struct railfs_push *push, int err)
{
	list_del_init(&push->link);
	push->asked_err = err;
	complete(&push->asked);
}

// A failure carries no usable immediate, so there is no way to tell which slot
// it belonged to. Everyone waiting is released with the error rather than left
// to time out one after another, thirty seconds apart.
static void railfs_strand_all(struct railfs_rdma *rail, int err)
{
	struct railfs_push *push;
	struct railfs_push *next;
	u32 slot;

	WRITE_ONCE(rail->broken, true);

	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		rail->slot[slot].err = err;
		complete(&rail->slot[slot].done);
	}

	spin_lock_bh(&rail->pushes_lock);
	list_for_each_entry_safe(push, next, &rail->pushes, link) {
		railfs_push_fail(push, err);
	}
	spin_unlock_bh(&rail->pushes_lock);

	wake_up(&rail->slot_room);
}

// The peer said where it wants the bytes for some key. Whoever registered that
// key is handed the record; a record nobody asked for belongs to a request
// that already timed out, and is dropped.
static void railfs_on_cts(struct railfs_rdma *rail, u32 slot)
{
	struct railfs_cts record;
	struct railfs_push *push;

	if (slot >= RAILFS_CTS_SLOTS) {
		railfs_strand_all(rail, -EPROTO);
		return;
	}

	memcpy(&record, (u8 *)rail->ring + (size_t)slot * RAILFS_CTS_BYTES, sizeof(record));

	spin_lock_bh(&rail->pushes_lock);
	list_for_each_entry(push, &rail->pushes, link) {
		if (push->key != record.key) {
			continue;
		}

		list_del_init(&push->link);
		push->cts = record;
		push->asked_err = 0;
		complete(&push->asked);
		spin_unlock_bh(&rail->pushes_lock);
		return;
	}
	spin_unlock_bh(&rail->pushes_lock);

	pr_warn_ratelimited("railfs: dropping a stale clear to send for key %llu\n", record.key);
}

// Every receive on any rail arrives here, and the immediate says what it is: a
// clear-to-send the peer wants answered, or the payload for a slot this side
// offered. Runs in softirq, so it may not sleep and may not modify a queue
// pair - reposting a receive is a doorbell, which is allowed.
static void railfs_on_landed(struct ib_cq *cq, struct ib_wc *wc)
{
	struct railfs_line *line = container_of(wc->wr_cqe, struct railfs_line, recv_cqe);
	struct railfs_rdma *rail = line->rail;
	const struct ib_recv_wr *bad;
	struct ib_recv_wr rq = {};
	u32 imm;

	if (wc->status != IB_WC_SUCCESS) {
		if (wc->status != IB_WC_WR_FLUSH_ERR) {
			pr_err_ratelimited("railfs: rdma payload failed on rail %u: %s\n", line->index, ib_wc_status_msg(wc->status));
		}
		railfs_strand_all(rail, -EIO);
		return;
	}

	rq.wr_cqe = &line->recv_cqe;
	rq.num_sge = 0;
	if (ib_post_recv(line->qp, &rq, &bad)) {
		pr_err("railfs: could not replace a receive on rail %u\n", line->index);
	}

	imm = wc->wc_flags & IB_WC_WITH_IMM ? be32_to_cpu(wc->ex.imm_data) : 0;

	if (imm & RAILFS_IS_CTS) {
		railfs_on_cts(rail, imm & ~RAILFS_IS_CTS);
		return;
	}

	if (imm >= RAILFS_STREAM_SLOTS) {
		pr_err("railfs: payload for slot %u, which this rail does not have\n", imm);
		railfs_strand_all(rail, -EPROTO);
		return;
	}

	rail->slot[imm].err = 0;
	rail->slot[imm].line = line->index;

	// Claim the slot before completing it. Losing the exchange means the
	// caller has already gone, and the page is ours to throw away.
	if (atomic_cmpxchg(&rail->slot[imm].state, RAILFS_SLOT_LIVE, RAILFS_SLOT_LANDED) == RAILFS_SLOT_ORPHAN) {
		railfs_rdma_slot_release(rail, imm);
		return;
	}

	complete(&rail->slot[imm].done);
}

// A timeout leaves work posted: the receive that never matched, or a send still
// on the wire. Moving every queue pair to error flushes them, so a late
// completion cannot release the next operation instead of its own. The rail
// does not come back - every later request is refused rather than answered
// with someone else's data.
static void railfs_rail_break(struct railfs_rdma *rail)
{
	struct ib_qp_attr attr = {};
	u32 i;

	// Once, and one caller at a time: concurrent timeouts would otherwise
	// race the same queue pairs through the transition. A late caller still
	// wakes the sleepers, since its own timeout sent it here.
	mutex_lock(&rail->break_lock);
	if (READ_ONCE(rail->broken)) {
		mutex_unlock(&rail->break_lock);
		goto wake;
	}

	WRITE_ONCE(rail->broken, true);
	attr.qp_state = IB_QPS_ERR;

	for (i = 0; i < rail->lines; i++) {
		if (rail->line[i].qp && ib_modify_qp(rail->line[i].qp, &attr, IB_QP_STATE)) {
			pr_err("railfs: could not flush rail %u after a timeout\n", i);
		}
	}
	mutex_unlock(&rail->break_lock);

wake:
	// The timeout paths reach here without railfs_strand_all, so this is the
	// only wake anyone parked for a slot gets.
	wake_up(&rail->slot_room);
}

void railfs_rdma_break(struct railfs_rdma *rail)
{
	if (!rail) {
		return;
	}

	railfs_rail_break(rail);
	railfs_strand_all(rail, -ENOTCONN);
}

// The clear-to-send is posted and never waited for, so this only reports. It
// must not touch the completion a push waits on: a late one from a read would
// release a write whose payload is still on the wire.
static void railfs_on_cts_sent(struct ib_cq *cq, struct ib_wc *wc)
{
	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR) {
		pr_err_ratelimited("railfs: rdma clear to send failed: %s\n", ib_wc_status_msg(wc->status));
	}
}

// Holds the reference the post took, so a push whose caller timed out and
// left is still whole when its flushed completion arrives.
static void railfs_on_sent(struct ib_cq *cq, struct ib_wc *wc)
{
	struct railfs_push *push = container_of(wc->wr_cqe, struct railfs_push, cqe);

	push->sent_err = wc->status == IB_WC_SUCCESS ? 0 : -EIO;
	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR) {
		pr_err_ratelimited("railfs: rdma send failed: %s\n", ib_wc_status_msg(wc->status));
	}
	complete(&push->sent);
	railfs_push_put(push);
}

// Writes one clear-to-send into the peer's ring naming where this page may land
// on every rail, and lets the peer pick. Returns as soon as the request is on
// the wire - the payload arrives later and raises the slot's completion, which
// is the only notification there is.
static int railfs_rdma_offer_at(struct railfs_rdma *rail, u32 slot, u64 key, u32 len, const u64 *addr, const u32 *rkey)
{
	struct railfs_line *post = &rail->line[0];
	const struct ib_send_wr *bad_send;
	struct ib_rdma_wr wr = {};
	struct railfs_cts cts = {};
	struct ib_sge sge = {};
	u32 shared;
	u32 i;

	if (slot >= RAILFS_STREAM_SLOTS) {
		return -EINVAL;
	}

	if (len > RAILFS_PAGE_BYTES) {
		return -EMSGSIZE;
	}

	if (READ_ONCE(rail->broken)) {
		return -ENOTCONN;
	}

	shared = railfs_shared_lines(rail);
	if (!shared) {
		return -ENOTCONN;
	}

	// Armed before the clear-to-send goes out: from here the peer may land a
	// page at any moment, and the landing has to find the slot claimable.
	reinit_completion(&rail->slot[slot].done);
	rail->slot[slot].err = 0;
	rail->slot[slot].line = 0;
	atomic_set(&rail->slot[slot].state, RAILFS_SLOT_LIVE);

	post->cts_cqe.done = railfs_on_cts_sent;

	cts.key = key;
	cts.length = len;
	cts.slot = slot;

	for (i = 0; i < shared; i++) {
		cts.addr[i] = addr[i];
		cts.rkey[i] = rkey[i];
	}

	cts.seq = (u32)atomic_inc_return(&rail->seq);

	memcpy((u8 *)post->offers + (size_t)slot * RAILFS_CTS_BYTES, &cts, sizeof(cts));

	sge.addr = post->offers_dma + (dma_addr_t)slot * RAILFS_CTS_BYTES;
	sge.length = sizeof(cts);
	sge.lkey = post->pd->local_dma_lkey;

	// With immediate data: a plain write lands silently and raises no
	// completion over there, and the immediate carries the slot.
	wr.wr.wr_cqe = &post->cts_cqe;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
	wr.wr.ex.imm_data = cpu_to_be32(RAILFS_IS_CTS | cts.slot);
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = rail->peer.cts_addr + (u64)cts.slot * RAILFS_CTS_BYTES;
	wr.rkey = rail->peer.line[0].cts_rkey;

	return ib_post_send(post->qp, &wr.wr, &bad_send);
}

static bool railfs_slot_free(const struct railfs_rdma *rail)
{
	return !bitmap_empty(rail->free, RAILFS_STREAM_SLOTS);
}

// Waits rather than fails when every slot is offered: the caller is a
// filesystem operation with nowhere else to go, and a slot comes back as soon
// as a page lands.
static int railfs_slot_take(struct railfs_rdma *rail, u32 *slot)
{
	for (;;) {
		u32 at;

		if (READ_ONCE(rail->broken)) {
			return -ENOTCONN;
		}

		at = find_first_bit(rail->free, RAILFS_STREAM_SLOTS);
		if (at < RAILFS_STREAM_SLOTS && test_and_clear_bit(at, rail->free)) {
			rail->slot[at].gpu = false;
			atomic_set(&rail->slot[at].state, RAILFS_SLOT_LIVE);
			*slot = at;
			return 0;
		}

		if (wait_event_killable(rail->slot_room, READ_ONCE(rail->broken) || railfs_slot_free(rail))) {
			return -ERESTARTSYS;
		}
	}
}

void railfs_rdma_slot_release(struct railfs_rdma *rail, u32 slot)
{
	if (slot >= RAILFS_STREAM_SLOTS) {
		return;
	}

	set_bit(slot, rail->free);
	wake_up(&rail->slot_room);
}

// A broken rail never delivers, so the slot is simply lost with it.
void railfs_rdma_slot_orphan(struct railfs_rdma *rail, u32 slot)
{
	if (slot >= RAILFS_STREAM_SLOTS) {
		return;
	}

	if (READ_ONCE(rail->broken)) {
		railfs_rdma_slot_release(rail, slot);
		return;
	}

	// Losing the exchange means the page landed while this caller was
	// leaving, so nothing else is coming and the slot is ours to give back.
	if (atomic_cmpxchg(&rail->slot[slot].state, RAILFS_SLOT_LIVE, RAILFS_SLOT_ORPHAN) != RAILFS_SLOT_LIVE) {
		railfs_rdma_slot_release(rail, slot);
	}
}

static void railfs_gpu_unbind_all(struct railfs_rdma *rail, u32 bound)
{
	u32 i;

	for (i = 0; i < bound; i++) {
		railfs_gpu_unbind(&rail->line[i]);
	}
}

int railfs_rdma_fetch_begin(struct railfs_rdma *rail, u64 key, u32 len, struct sg_table *gpu, u32 *slot)
{
	u64 addr[RAILFS_MAX_RAILS] = {};
	u32 rkey[RAILFS_MAX_RAILS] = {};
	u32 shared = railfs_shared_lines(rail);
	u32 bound = 0;
	u32 i;
	int err;

	if (READ_ONCE(rail->broken) || !shared) {
		return -ENOTCONN;
	}

	err = railfs_slot_take(rail, slot);
	if (err) {
		return err;
	}

	if (gpu) {
		mutex_lock(&rail->gds_lock);
		rail->slot[*slot].gpu = true;

		for (i = 0; i < shared; i++) {
			err = railfs_gpu_bind(&rail->line[i], gpu, DMA_FROM_DEVICE, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE);
			if (err) {
				goto fail;
			}

			bound++;
			addr[i] = rail->line[i].gpu_mr->iova;
			rkey[i] = rail->line[i].gpu_mr->rkey;
		}
	} else {
		for (i = 0; i < shared; i++) {
			addr[i] = railfs_landing_remote_at(&rail->line[i], *slot);
			rkey[i] = rail->line[i].landing_mr->rkey;
		}
	}

	err = railfs_rdma_offer_at(rail, *slot, key, len, addr, rkey);
	if (err) {
		goto fail;
	}

	return 0;

fail:
	if (gpu) {
		railfs_gpu_unbind_all(rail, bound);
		mutex_unlock(&rail->gds_lock);
	}
	railfs_rdma_slot_release(rail, *slot);
	return err;
}

static int railfs_rdma_await(struct railfs_rdma *rail, u32 slot)
{
	if (!wait_for_completion_timeout(&rail->slot[slot].done, msecs_to_jiffies(RAILFS_WAIT_MS))) {
		railfs_rail_break(rail);
		return -ETIMEDOUT;
	}

	if (rail->slot[slot].err) {
		return rail->slot[slot].err;
	}

	if (rail->slot[slot].line >= rail->live) {
		return -EPROTO;
	}

	return 0;
}

// The landing stays valid until the slot is released, whichever way this ends;
// the slot is the caller's to release either way.
int railfs_rdma_fetch_end(struct railfs_rdma *rail, u32 slot, const void **at)
{
	int err;

	*at = NULL;

	if (slot >= RAILFS_STREAM_SLOTS) {
		return -EINVAL;
	}

	err = railfs_rdma_await(rail, slot);

	if (rail->slot[slot].gpu) {
		railfs_gpu_unbind_all(rail, railfs_shared_lines(rail));
		mutex_unlock(&rail->gds_lock);
		return err;
	}

	if (!err) {
		*at = railfs_landing_at(&rail->line[rail->slot[slot].line], slot);
	}
	return err;
}

struct railfs_push *railfs_rdma_expect(struct railfs_rdma *rail, u64 key)
{
	struct railfs_push *push = kzalloc(sizeof(*push), GFP_NOFS);

	if (!push) {
		return NULL;
	}

	kref_init(&push->ref);
	INIT_LIST_HEAD(&push->link);
	push->key = key;
	init_completion(&push->asked);
	init_completion(&push->sent);

	spin_lock_bh(&rail->pushes_lock);
	list_add_tail(&push->link, &rail->pushes);
	spin_unlock_bh(&rail->pushes_lock);
	return push;
}

static void railfs_push_unlink(struct railfs_rdma *rail, struct railfs_push *push)
{
	spin_lock_bh(&rail->pushes_lock);
	list_del_init(&push->link);
	spin_unlock_bh(&rail->pushes_lock);
}

void railfs_rdma_forget(struct railfs_rdma *rail, struct railfs_push *push)
{
	if (!push) {
		return;
	}

	railfs_push_unlink(rail, push);
	railfs_push_put(push);
}

// Waits for the peer to say where it wants the bytes for this push's key. The
// completion handler has already matched the record to this push, so nothing
// here is anyone else's.
static int railfs_await_cts(struct railfs_rdma *rail, struct railfs_push *push)
{
	if (!wait_for_completion_timeout(&push->asked, msecs_to_jiffies(RAILFS_WAIT_MS))) {
		railfs_push_unlink(rail, push);
		railfs_rail_break(rail);
		return -ETIMEDOUT;
	}

	if (push->asked_err) {
		return push->asked_err;
	}

	if (push->cts.slot >= RAILFS_CTS_SLOTS) {
		return -EPROTO;
	}

	return 0;
}

// The mirror of a fetch: here the peer says where it wants the bytes and this
// side writes them. The immediate carries the slot without the clear-to-send
// bit, which is how the peer tells a payload from a request.
static int railfs_rdma_push_from(struct railfs_rdma *rail, struct railfs_push *push, const void *buf, struct folio **folios,
				 unsigned int nr, struct sg_table *pages, u32 len)
{
	const struct ib_send_wr *bad_send;
	struct railfs_line *post;
	struct railfs_line *bound = NULL;
	struct ib_rdma_wr wr = {};
	struct ib_sge sge[RAILFS_MAX_SGE] = {};
	struct ib_device *mapped = NULL;
	dma_addr_t dma[RAILFS_MAX_SGE] = {};
	unsigned int mapped_nr = 0;
	unsigned int entries = 1;
	unsigned int i;
	u32 staging = RAILFS_STREAM_SLOTS;
	u32 shared;
	u32 which;
	int err;

	if (len > RAILFS_PAGE_BYTES) {
		err = -EMSGSIZE;
		goto out;
	}

	if (READ_ONCE(rail->broken)) {
		err = -ENOTCONN;
		goto out;
	}

	shared = railfs_shared_lines(rail);
	if (!shared) {
		err = -ENOTCONN;
		goto out;
	}

	which = (u32)atomic_inc_return(&rail->turn) % shared;
	post = &rail->line[which];

	err = railfs_await_cts(rail, push);
	if (err) {
		goto out;
	}

	if (len > push->cts.length) {
		err = -EMSGSIZE;
		goto out;
	}

	// A folio is mapped for the device and sent where it lies. Anything else
	// is staged through a landing page first, which is what a folio too small
	// to cover the write falls back to.
	if (folios) {
		u32 left = len;

		if (nr > RAILFS_MAX_SGE) {
			err = -EMSGSIZE;
			goto out;
		}

		entries = nr;
		mapped = post->device;

		for (i = 0; i < nr && left; i++) {
			// Mapping runs from a folio's head page, so anything past it would
			// be memory the folio does not own.
			u32 bytes = min_t(u32, left, (u32)folio_size(folios[i]));

			dma[i] = ib_dma_map_page(post->device, folio_page(folios[i], 0), 0, bytes, DMA_TO_DEVICE);
			if (ib_dma_mapping_error(post->device, dma[i])) {
				err = -ENOMEM;
				goto out;
			}

			sge[i].addr = dma[i];
			sge[i].length = bytes;
			sge[i].lkey = post->pd->local_dma_lkey;
			mapped_nr++;
			left -= bytes;
		}

		if (left) {
			err = -EMSGSIZE;
			goto out;
		}
	} else if (pages) {
		mutex_lock(&rail->gds_lock);
		err = railfs_gpu_bind(post, pages, DMA_TO_DEVICE, IB_ACCESS_LOCAL_WRITE);
		if (err) {
			mutex_unlock(&rail->gds_lock);
			goto out;
		}

		bound = post;
		sge[0].addr = post->gpu_mr->iova;
		sge[0].length = len;
		sge[0].lkey = post->gpu_mr->lkey;
	} else {
		err = railfs_slot_take(rail, &staging);
		if (err) {
			goto out;
		}

		memcpy(railfs_landing_at(post, staging), buf, len);
		sge[0].addr = post->landing[staging].dma;
		sge[0].length = len;
		sge[0].lkey = post->pd->local_dma_lkey;
	}

	push->cqe.done = railfs_on_sent;

	wr.wr.wr_cqe = &push->cqe;
	wr.wr.sg_list = sge;
	wr.wr.num_sge = entries;
	wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
	wr.wr.ex.imm_data = cpu_to_be32(push->cts.slot);
	wr.wr.send_flags = IB_SEND_SIGNALED;
	// The peer names one landing per rail, so the address has to be the one
	// belonging to the rail this write goes out on.
	wr.remote_addr = push->cts.addr[which];
	wr.rkey = push->cts.rkey[which];

	kref_get(&push->ref);
	err = ib_post_send(post->qp, &wr.wr, &bad_send);
	if (err) {
		railfs_push_put(push);
		goto out;
	}

	if (!wait_for_completion_timeout(&push->sent, msecs_to_jiffies(RAILFS_WAIT_MS))) {
		// Breaking flushes the send but does not wait for it. Draining does,
		// which is what makes the mappings below safe to take back.
		railfs_rail_break(rail);
		ib_drain_qp(post->qp);
		err = -ETIMEDOUT;
		goto out;
	}

	err = push->sent_err ? push->sent_err : (int)len;
out:
	// Release mappings only after completion or a QP drain.
	if (mapped) {
		for (i = 0; i < mapped_nr; i++) {
			ib_dma_unmap_page(mapped, dma[i], sge[i].length, DMA_TO_DEVICE);
		}
	}
	if (bound) {
		railfs_gpu_unbind(bound);
		mutex_unlock(&rail->gds_lock);
	}
	if (staging < RAILFS_STREAM_SLOTS) {
		railfs_rdma_slot_release(rail, staging);
	}
	railfs_push_unlink(rail, push);
	return err;
}

int railfs_rdma_push(struct railfs_rdma *rail, struct railfs_push *push, const void *buf, u32 len)
{
	return railfs_rdma_push_from(rail, push, buf, NULL, 0, NULL, len);
}

int railfs_rdma_push_folios(struct railfs_rdma *rail, struct railfs_push *push, struct folio **folios, unsigned int nr, u32 len)
{
	return railfs_rdma_push_from(rail, push, NULL, folios, nr, NULL, len);
}

int railfs_rdma_push_sg(struct railfs_rdma *rail, struct railfs_push *push, struct sg_table *pages, u32 len)
{
	return railfs_rdma_push_from(rail, push, NULL, NULL, 0, pages, len);
}

int railfs_rdma_start(void)
{
	return ib_register_client(&railfs_ib_client);
}

void railfs_rdma_stop(void)
{
	ib_unregister_client(&railfs_ib_client);
}
