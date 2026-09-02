/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_TRACE_H
#define RAILFS_TRACE_H

#include <linux/ktime.h>
#include <linux/types.h>

/* Where a mount's time goes, counted per phase rather than guessed at. Reading
 * /sys/kernel/debug/railfs/stats prints one line per phase with the calls, the
 * bytes and the nanoseconds; writing anything to it zeroes them.
 *
 * The counters are per-cpu and the only cost on the hot path is two ktime
 * reads, so this stays compiled in - a tuning run that needs a special build
 * measures a different kernel from the one that ships.
 */
enum railfs_phase {
	RAILFS_PHASE_READ_TOTAL,	 /* one fetch, end to end */
	RAILFS_PHASE_READ_ALLOC,	 /* the landing buffer for it */
	RAILFS_PHASE_READ_WIRE,	 /* request out, payload in */
	RAILFS_PHASE_READ_CTL,	 /* the request out and its reply back */
	RAILFS_PHASE_READ_PULL,	 /* the page itself, offered and collected */
	RAILFS_PHASE_READ_DIGEST,	 /* hashing what arrived */
	RAILFS_PHASE_READ_LAND,	 /* copying it into the folios asked for */
	RAILFS_PHASE_READ_AROUND,	 /* and into the neighbours of a widened block */
	RAILFS_PHASE_WRITE_TOTAL,	 /* one writeback run */
	RAILFS_PHASE_WRITE_GATHER, /* copying folios into the send buffer */
	RAILFS_PHASE_WRITE_WIRE,	 /* payload out, reply in */
	RAILFS_PHASE_WRITE_DIGEST, /* hashing what is being sent */
	RAILFS_PHASE_STAT,	 /* a metadata round trip */
	RAILFS_PHASE_POOL_WAIT,	 /* waiting for a free connection */
	RAILFS_PHASE_MAX,
};

void railfs_trace_add(enum railfs_phase phase, u64 began, u64 bytes);
void railfs_trace_start(void);
void railfs_trace_stop(void);

/* How many fetches are outstanding right now, and the most there have ever
 * been. A rate divided by a latency only infers this; the watermark says
 * whether the client is actually asking for as much at once as it may.
 */
void railfs_trace_inflight(int delta);

/* The same for connections held. A mount that never has two busy at once is
 * not spreading across its pool, which is a property worth asserting on
 * directly rather than inferring from how long something took.
 */
void railfs_trace_busy(int delta);

static inline u64 railfs_now(void) { return ktime_get_ns(); }

#endif
