// SPDX-License-Identifier: GPL-2.0
//
// Kernel RDMA. The fabric is found through an ib_client, and a mount builds a
// rail on every port that is up, to the two the wire carries: each with a
// protection domain, a completion queue, a queue pair and its own landing
// memory, plus one ring the peer may write its requests into. Two ports on this
// hardware are two separate ib_devices, so memory registered with one means
// nothing to the other - which is why a clear-to-send names an address per rail
// rather than one for all of them.

#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/scatterlist.h>

#include <rdma/ib_cache.h>
#include <rdma/ib_verbs.h>

#include "railfs-rdma.h"
#include "railfs-gds.h"
#include "railfs-proto.h"

#define RAILFS_CQ_DEPTH 64

// The landing region has to hold whatever page size the session negotiated, so
// this is that number rather than one of its own. They were separate constants
// that had to agree, and raising the negotiated one alone made every read fail
// with EAGAIN once the daemon started sending pages the region could not hold.
#define RAILFS_PAGE_BYTES RAILFS_PAGE_SIZE

// Folios one send may carry, each its own scatter entry. The adapters here
// report thirty; a page-sized flush of megabyte folios needs four.
#define RAILFS_MAX_SGE 8

// One page offered to the peer and not yet collected. Every slot has its own
// completion because the peer answers them in whatever order its reader
// reaches them, and a single completion cannot say which one arrived.
struct railfs_slot {
	struct completion done;
	int err;
	// Which rail carried it. The peer picks one per page, so where the bytes
	// are is not known until the completion says.
	u32 line;
};

// The landing region holds one page per stream slot and one more for a push to
// stage its bytes in, so a write never lands on a page a stream is waiting for.
#define RAILFS_LANDING_PAGES (RAILFS_STREAM_SLOTS + 1)
#define RAILFS_PUSH_SLOT RAILFS_STREAM_SLOTS

// How many receives each rail keeps posted. A payload write consumes one, and a
// rail with none left makes the peer wait on a receiver-not-ready retry rather
// than report anything.
#define RAILFS_RECV_DEPTH (RAILFS_CQ_DEPTH / 2)

// How many clear-to-sends a push will skip past before giving up. Only a
// request that already timed out leaves one behind, so more than a handful
// means the peer is answering something this side is no longer asking.
#define RAILFS_STALE_LIMIT 4

/* How long an exchange waits before deciding the peer is not coming back. The
 * fabric answers in microseconds, so anything approaching this is a rail that
 * has stopped rather than one that is busy; arming a region is quicker because
 * it is local work, not a round trip.
 */
#define RAILFS_WAIT_MS 30000
#define RAILFS_ARM_WAIT_MS 5000

// One port. Everything that belongs to a device rather than to the connection:
// its own protection domain, queue pair and landing memory, because two ports
// on this hardware are two separate ib_devices and memory registered with one
// means nothing to the other.
struct railfs_line {
	struct railfs_rdma *rail;
	struct ib_device *device;
	u32 port;
	u32 index;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	// Where a fetched page lands. Registered once and copied out of, rather
	// than registering the caller's page for every read: a registration is a
	// verb round trip, and one per read would cost more than the copy.
	void *landing;
	dma_addr_t landing_dma;
	struct ib_mr *landing_mr;
	// Where an outgoing clear-to-send is staged, kept apart from the ring the
	// peer writes its own requests into: sharing them races.
	void *offers;
	dma_addr_t offers_dma;
	// A managed completion queue dispatches through wr_cqe->done, not wr_id.
	// Leaving these unset is a null dereference in softirq context.
	struct ib_cqe recv_cqe;
	struct ib_cqe send_cqe;
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
	// An incoming clear-to-send, which is what a push waits for. Told apart
	// from a payload by the RAILFS_IS_CTS bit in the immediate.
	struct completion asked;
	struct completion sent;
	int asked_err;
	int sent_err;
	u32 asked_imm;
	bool broken;
	u32 seq;
	// Which rail the next push writes over.
	u32 turn;
};

static void *railfs_landing_at(struct railfs_line *line, u32 slot)
{
	return (u8 *)line->landing + (size_t)slot * RAILFS_PAGE_BYTES;
}

static dma_addr_t railfs_landing_dma_at(struct railfs_line *line, u32 slot)
{
	return line->landing_dma + (dma_addr_t)slot * RAILFS_PAGE_BYTES;
}

// How many rails both ends have. The peer resizes to whatever this side
// advertises, so a page may only be offered on a rail both of them built.
static u32 railfs_shared_lines(struct railfs_rdma *rail)
{
	return rail->live;
}

#define RAILFS_MAX_DEVICES 8

static struct ib_device *railfs_known[RAILFS_MAX_DEVICES];
static DEFINE_MUTEX(railfs_known_lock);

static int railfs_rdma_probe(struct ib_device *device);
static int railfs_arm_region(struct railfs_line *line, struct ib_mr *mr);
static void railfs_rdma_forget(struct ib_device *device, void *data);

static struct ib_client railfs_ib_client = {
	.name = "railfs",
	.add = railfs_rdma_probe,
	.remove = railfs_rdma_forget,
};

static int railfs_rdma_probe(struct ib_device *device)
{
	unsigned int i;

	mutex_lock(&railfs_known_lock);

	for (i = 0; i < RAILFS_MAX_DEVICES; i++) {
		if (!railfs_known[i]) {
			railfs_known[i] = device;
			break;
		}
	}

	mutex_unlock(&railfs_known_lock);
	return 0;
}

static void railfs_rdma_forget(struct ib_device *device, void *data)
{
	unsigned int i;

	mutex_lock(&railfs_known_lock);

	for (i = 0; i < RAILFS_MAX_DEVICES; i++) {
		if (railfs_known[i] == device) {
			railfs_known[i] = NULL;
			break;
		}
	}

	mutex_unlock(&railfs_known_lock);
}

// Half the devices on this hardware are down, and a queue pair on one of those
// reaches ready and then carries nothing. Every active port is collected rather
// than only the first: two of them is twice the bandwidth, and one rail is what
// the mount used to be capped at.
static u32 railfs_active_lines(struct railfs_line *out, u32 room)
{
	u32 found = 0;
	unsigned int i;

	mutex_lock(&railfs_known_lock);

	for (i = 0; i < RAILFS_MAX_DEVICES && found < room; i++) {
		struct ib_device *device = railfs_known[i];
		u32 port;

		if (!device) {
			continue;
		}

		rdma_for_each_port(device, port) {
			struct ib_port_attr attr;

			if (ib_query_port(device, port, &attr)) {
				continue;
			}

			if (attr.state != IB_PORT_ACTIVE) {
				continue;
			}

			out[found].device = device;
			out[found].port = port;
			out[found].index = found;
			found++;
			break;
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

// A region the peer may write into. ib_alloc_mr plus a mapping is what the
// kernel offers in place of the userspace ibv_reg_mr over an arbitrary buffer.
// Both regions the peer may reach: the ring it drops requests into, and the
// buffer it writes payloads to.
static int railfs_region(struct railfs_line *line, size_t bytes, void **cpu, dma_addr_t *dma, struct ib_mr **out)
{
	struct scatterlist sg;
	struct ib_mr *mr;
	int mapped;

	*cpu = dma_alloc_coherent(line->device->dma_device, bytes, dma, GFP_KERNEL);
	if (!*cpu) {
		return -ENOMEM;
	}

	mr = ib_alloc_mr(line->pd, IB_MR_TYPE_MEM_REG, 1);
	if (IS_ERR(mr)) {
		return PTR_ERR(mr);
	}

	*out = mr;

	sg_init_table(&sg, 1);
	sg_dma_address(&sg) = *dma;
	sg_dma_len(&sg) = bytes;

	mapped = ib_map_mr_sg(mr, &sg, 1, NULL, bytes);
	if (mapped != 1) {
		return mapped < 0 ? mapped : -EINVAL;
	}

	return 0;
}

static void railfs_line_close(struct railfs_line *line)
{
	if (line->qp) {
		ib_destroy_qp(line->qp);
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

	if (line->landing) {
		dma_free_coherent(line->device->dma_device, RAILFS_LANDING_PAGES * RAILFS_PAGE_BYTES, line->landing, line->landing_dma);
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
//   conn63 - line1 - cq -> vector n
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

	// Measured neutral, 15.69 against 15.79 GiB/s. Here because one vector for
	// a mount's hundred and twenty-eight queues is wrong on its face.
	line->cq = ib_alloc_cq(line->device, NULL, RAILFS_CQ_DEPTH, railfs_next_vector(line->device), IB_POLL_SOFTIRQ);
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

	err = railfs_region(line, RAILFS_LANDING_PAGES * RAILFS_PAGE_BYTES, &line->landing, &line->landing_dma, &line->landing_mr);
	if (err) {
		goto out;
	}

	line->offers = dma_alloc_coherent(line->device->dma_device, RAILFS_STREAM_SLOTS * RAILFS_CTS_BYTES, &line->offers_dma, GFP_KERNEL);
	if (!line->offers) {
		err = -ENOMEM;
		goto out;
	}

	line->gpu_mr = ib_alloc_mr(line->pd, IB_MR_TYPE_MEM_REG, RAILFS_GDS_MAX_SG);
	if (IS_ERR(line->gpu_mr)) {
		err = PTR_ERR(line->gpu_mr);
		line->gpu_mr = NULL;
		goto out;
	}

	err = sg_alloc_table(&line->gpu_table, RAILFS_GDS_MAX_SG, GFP_KERNEL);
	if (err) {
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
	struct railfs_line found[RAILFS_MAX_RAILS] = {};
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

	for (i = 0; i < lines; i++) {
		rail->line[i] = found[i];
		rail->line[i].rail = rail;
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

	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		init_completion(&rail->slot[slot].done);
	}

	init_completion(&rail->asked);
	init_completion(&rail->sent);

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

// A send work request, so the queue pair has to be sending already: this runs
// after the transition to ready, not while the region is being allocated.
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
	struct ib_mr *mr = line->gpu_mr;
	int nents = pages->nents;
	int mapped;
	int err;

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

	if (railfs_disarm(line, line->gpu_mr)) {
		pr_err("railfs: could not invalidate the gpu region on rail %u\n", line->index);
	}

	railfs_gds_unmap(line->device->dma_device, line->gpu_table.sgl, line->gpu_nents, line->gpu_dir);
	line->gpu_nents = 0;
}

static void railfs_on_landed(struct ib_cq *cq, struct ib_wc *wc);

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

// A failure carries no usable immediate, so there is no way to tell which slot
// it belonged to. Everyone waiting is released with the error rather than left
// to time out one after another, thirty seconds apart.
static void railfs_strand_all(struct railfs_rdma *rail, int err)
{
	u32 slot;

	rail->broken = true;

	for (slot = 0; slot < RAILFS_STREAM_SLOTS; slot++) {
		rail->slot[slot].err = err;
		complete(&rail->slot[slot].done);
	}

	rail->asked_err = err;
	complete(&rail->asked);
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
		pr_err("railfs: rdma payload failed on rail %u: %s\n", line->index, ib_wc_status_msg(wc->status));
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
		rail->asked_err = 0;
		rail->asked_imm = imm;
		complete(&rail->asked);
		return;
	}

	if (imm >= RAILFS_STREAM_SLOTS) {
		pr_err("railfs: payload for slot %u, which this rail does not have\n", imm);
		railfs_strand_all(rail, -EPROTO);
		return;
	}

	rail->slot[imm].err = 0;
	rail->slot[imm].line = line->index;
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

	rail->broken = true;
	attr.qp_state = IB_QPS_ERR;

	for (i = 0; i < rail->lines; i++) {
		if (ib_modify_qp(rail->line[i].qp, &attr, IB_QP_STATE)) {
			pr_err("railfs: could not flush rail %u after a timeout\n", i);
		}
	}
}

// The clear-to-send is posted and never waited for, so this only reports. It
// must not touch the completion a push waits on: a late one from a read would
// release a write whose payload is still on the wire.
static void railfs_on_cts_sent(struct ib_cq *cq, struct ib_wc *wc)
{
	if (wc->status != IB_WC_SUCCESS) {
		pr_err("railfs: rdma clear to send failed: %s\n", ib_wc_status_msg(wc->status));
	}
}

static void railfs_on_sent(struct ib_cq *cq, struct ib_wc *wc)
{
	struct railfs_line *line = container_of(wc->wr_cqe, struct railfs_line, send_cqe);

	line->rail->sent_err = wc->status == IB_WC_SUCCESS ? 0 : -EIO;
	if (wc->status != IB_WC_SUCCESS) {
		pr_err("railfs: rdma send failed: %s\n", ib_wc_status_msg(wc->status));
	}
	complete(&line->rail->sent);
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

	if (rail->broken) {
		return -ENOTCONN;
	}

	shared = railfs_shared_lines(rail);
	if (!shared) {
		return -ENOTCONN;
	}

	reinit_completion(&rail->slot[slot].done);
	rail->slot[slot].err = 0;
	rail->slot[slot].line = 0;

	post->cts_cqe.done = railfs_on_cts_sent;

	cts.key = key;
	cts.length = len;
	cts.slot = slot;

	for (i = 0; i < shared; i++) {
		cts.addr[i] = addr[i];
		cts.rkey[i] = rkey[i];
	}

	// The peer notices a record by its sequence changing, so this counts
	// across the whole connection rather than per slot.
	cts.seq = ++rail->seq;

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

// Every rail names its own landing, because the same page registered on two
// devices has a different address and a different key on each.
int railfs_rdma_offer(struct railfs_rdma *rail, u32 slot, u64 key, u32 len)
{
	u64 addr[RAILFS_MAX_RAILS] = {};
	u32 rkey[RAILFS_MAX_RAILS] = {};
	u32 i;

	if (slot >= RAILFS_STREAM_SLOTS) {
		return -EINVAL;
	}

	for (i = 0; i < railfs_shared_lines(rail); i++) {
		addr[i] = (u64)railfs_landing_dma_at(&rail->line[i], slot);
		rkey[i] = rail->line[i].landing_mr->rkey;
	}

	return railfs_rdma_offer_at(rail, slot, key, len, addr, rkey);
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

// Waits for the page offered into this slot and copies it out of whichever
// rail's landing the peer chose. The slot is free to be offered again once this
// returns.
int railfs_rdma_collect(struct railfs_rdma *rail, u32 slot, void *buf, u32 len)
{
	int err;

	if (slot >= RAILFS_STREAM_SLOTS) {
		return -EINVAL;
	}

	if (len > RAILFS_PAGE_BYTES) {
		return -EMSGSIZE;
	}

	err = railfs_rdma_await(rail, slot);
	if (err) {
		return err;
	}

	memcpy(buf, railfs_landing_at(&rail->line[rail->slot[slot].line], slot), len);
	return len;
}

int railfs_rdma_fetch_sg(struct railfs_rdma *rail, u64 key, struct sg_table *pages, u32 len)
{
	u64 addr[RAILFS_MAX_RAILS] = {};
	u32 rkey[RAILFS_MAX_RAILS] = {};
	u32 shared = railfs_shared_lines(rail);
	u32 bound = 0;
	u32 i;
	int err;

	if (rail->broken || !shared) {
		return -ENOTCONN;
	}

	for (i = 0; i < shared; i++) {
		err = railfs_gpu_bind(&rail->line[i], pages, DMA_FROM_DEVICE, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE);
		if (err) {
			goto unbind;
		}

		bound++;
		addr[i] = rail->line[i].gpu_mr->iova;
		rkey[i] = rail->line[i].gpu_mr->rkey;
	}

	err = railfs_rdma_offer_at(rail, 0, key, len, addr, rkey);
	if (err) {
		goto unbind;
	}

	err = railfs_rdma_await(rail, 0);
	if (err == -ETIMEDOUT) {
		return err;
	}

unbind:
	for (i = 0; i < bound; i++) {
		railfs_gpu_unbind(&rail->line[i]);
	}

	return err ? err : (int)len;
}

// One page, offered and collected without letting go in between. What a ranged
// read does.
int railfs_rdma_fetch(struct railfs_rdma *rail, u64 key, void *buf, u32 len)
{
	int err = railfs_rdma_offer(rail, 0, key, len);

	if (err) {
		return err;
	}

	return railfs_rdma_collect(rail, 0, buf, len);
}

// The mirror of a fetch: here the peer says where it wants the bytes and this
// side writes them. The immediate carries the slot without the clear-to-send
// bit, which is how the peer tells a payload from a request.
// Waits for the peer to say where it wants the bytes for this key, skipping
// records left behind by requests that already timed out. The completion is
// never reinitialised: receives are stocked from the moment a rail comes up, so
// a record can arrive before this is called and resetting would lose it.
static int railfs_await_cts(struct railfs_rdma *rail, u64 key, struct railfs_cts *out)
{
	u32 asks = 0;
	u32 slot;

	for (;;) {
		if (++asks > RAILFS_STALE_LIMIT) {
			return -EPROTO;
		}

		if (!wait_for_completion_timeout(&rail->asked, msecs_to_jiffies(RAILFS_WAIT_MS))) {
			railfs_rail_break(rail);
			return -ETIMEDOUT;
		}

		if (rail->asked_err) {
			return rail->asked_err;
		}

		if (!(rail->asked_imm & RAILFS_IS_CTS)) {
			return -EPROTO;
		}

		slot = rail->asked_imm & ~RAILFS_IS_CTS;
		if (slot >= RAILFS_CTS_SLOTS) {
			return -EPROTO;
		}

		memcpy(out, (u8 *)rail->ring + (size_t)slot * RAILFS_CTS_BYTES, sizeof(*out));

		if (out->key == key) {
			return 0;
		}

		pr_warn("railfs: skipping a stale clear to send for key %llu, wanted %llu\n", out->key, key);
	}
}

static int railfs_rdma_push_from(struct railfs_rdma *rail, u64 key, const void *buf, struct folio **folios, unsigned int nr,
				 struct sg_table *pages, u32 len)
{
	const struct ib_send_wr *bad_send;
	struct railfs_line *post;
	struct railfs_line *bound = NULL;
	struct ib_rdma_wr wr = {};
	struct railfs_cts cts;
	struct ib_sge sge[RAILFS_MAX_SGE] = {};
	struct ib_device *mapped = NULL;
	dma_addr_t dma[RAILFS_MAX_SGE] = {};
	unsigned int mapped_nr = 0;
	unsigned int entries = 1;
	unsigned int i;
	u32 shared;
	u32 which;
	int err;

	if (len > RAILFS_PAGE_BYTES) {
		err = -EMSGSIZE;
		goto out;
	}

	if (rail->broken) {
		err = -ENOTCONN;
		goto out;
	}

	shared = railfs_shared_lines(rail);
	if (!shared) {
		err = -ENOTCONN;
		goto out;
	}

	which = rail->turn++ % shared;
	post = &rail->line[which];

	post->send_cqe.done = railfs_on_sent;

	err = railfs_await_cts(rail, key, &cts);
	if (err) {
		goto out;
	}

	if (len > cts.length) {
		err = -EMSGSIZE;
		goto out;
	}

	// A folio is mapped for the device and sent where it lies. Anything else
	// is staged through the landing region first, which is what a folio too
	// small to cover the write falls back to.
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
		err = railfs_gpu_bind(post, pages, DMA_TO_DEVICE, IB_ACCESS_LOCAL_WRITE);
		if (err) {
			goto out;
		}

		bound = post;
		sge[0].addr = post->gpu_mr->iova;
		sge[0].length = len;
		sge[0].lkey = post->gpu_mr->lkey;
	} else {
		memcpy(railfs_landing_at(post, RAILFS_PUSH_SLOT), buf, len);
		sge[0].addr = railfs_landing_dma_at(post, RAILFS_PUSH_SLOT);
		sge[0].length = len;
		sge[0].lkey = post->pd->local_dma_lkey;
	}

	reinit_completion(&rail->sent);
	rail->sent_err = 0;

	wr.wr.wr_cqe = &post->send_cqe;
	wr.wr.sg_list = sge;
	wr.wr.num_sge = entries;
	wr.wr.opcode = IB_WR_RDMA_WRITE_WITH_IMM;
	wr.wr.ex.imm_data = cpu_to_be32(cts.slot);
	wr.wr.send_flags = IB_SEND_SIGNALED;
	// The peer names one landing per rail, so the address has to be the one
	// belonging to the rail this write goes out on.
	wr.remote_addr = cts.addr[which];
	wr.rkey = cts.rkey[which];

	err = ib_post_send(post->qp, &wr.wr, &bad_send);
	if (err) {
		goto out;
	}

	if (!wait_for_completion_timeout(&rail->sent, msecs_to_jiffies(RAILFS_WAIT_MS))) {
		railfs_rail_break(rail);

		// The send never reported, so the device may still be reading from the
		// mapping. It is left in place deliberately: handing it back while the
		// adapter can still touch it is worse than leaking it on a path that
		// has already broken the rail.
		mapped = NULL;
		bound = NULL;
		err = -ETIMEDOUT;
		goto out;
	}

	err = rail->sent_err ? rail->sent_err : (int)len;
out:
	// After the completion, never before: the device owns the mappings until the
	// send they were made for has finished.
	if (mapped) {
		for (i = 0; i < mapped_nr; i++) {
			ib_dma_unmap_page(mapped, dma[i], sge[i].length, DMA_TO_DEVICE);
		}
	}
	if (bound) {
		railfs_gpu_unbind(bound);
	}
	return err;
}

int railfs_rdma_push(struct railfs_rdma *rail, u64 key, const void *buf, u32 len)
{
	return railfs_rdma_push_from(rail, key, buf, NULL, 0, NULL, len);
}

int railfs_rdma_push_folios(struct railfs_rdma *rail, u64 key, struct folio **folios, unsigned int nr, u32 len)
{
	return railfs_rdma_push_from(rail, key, NULL, folios, nr, NULL, len);
}

int railfs_rdma_push_sg(struct railfs_rdma *rail, u64 key, struct sg_table *pages, u32 len)
{
	return railfs_rdma_push_from(rail, key, NULL, NULL, 0, pages, len);
}

int railfs_rdma_start(void)
{
	return ib_register_client(&railfs_ib_client);
}

void railfs_rdma_stop(void)
{
	ib_unregister_client(&railfs_ib_client);
}
