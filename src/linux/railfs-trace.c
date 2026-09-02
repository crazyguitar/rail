// SPDX-License-Identifier: GPL-2.0
//
// Per-phase counters for a mount, so a tuning round says where the time went
// rather than which change happened to make the number move.

#include <linux/debugfs.h>
#include <linux/percpu.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "railfs-trace.h"

struct railfs_counter {
	u64 ns;
	u64 calls;
	u64 bytes;
};

struct railfs_counters {
	struct railfs_counter phase[RAILFS_PHASE_MAX];
};

static DEFINE_PER_CPU(struct railfs_counters, railfs_counters);
static struct dentry *railfs_debug_dir;
static atomic_t railfs_inflight = ATOMIC_INIT(0);
static atomic_t railfs_inflight_max = ATOMIC_INIT(0);

static atomic_t railfs_busy = ATOMIC_INIT(0);
static atomic_t railfs_busy_max = ATOMIC_INIT(0);

static void railfs_watermark(atomic_t *now_at, atomic_t *high, int delta)
{
	int now = atomic_add_return(delta, now_at);
	int seen = atomic_read(high);

	while (now > seen && atomic_cmpxchg(high, seen, now) != seen) {
		seen = atomic_read(high);
	}
}

void railfs_trace_inflight(int delta) { railfs_watermark(&railfs_inflight, &railfs_inflight_max, delta); }

void railfs_trace_busy(int delta) { railfs_watermark(&railfs_busy, &railfs_busy_max, delta); }

static const char *const railfs_phase_names[RAILFS_PHASE_MAX] = {
	[RAILFS_PHASE_READ_TOTAL] = "read.total",     [RAILFS_PHASE_READ_ALLOC] = "read.alloc",
	[RAILFS_PHASE_READ_WIRE] = "read.wire",	    [RAILFS_PHASE_READ_CTL] = "read.ctl",
	[RAILFS_PHASE_READ_PULL] = "read.pull",	    [RAILFS_PHASE_READ_DIGEST] = "read.digest",
	[RAILFS_PHASE_READ_LAND] = "read.land",	    [RAILFS_PHASE_READ_AROUND] = "read.around",
	[RAILFS_PHASE_WRITE_TOTAL] = "write.total",   [RAILFS_PHASE_WRITE_GATHER] = "write.gather",
	[RAILFS_PHASE_WRITE_WIRE] = "write.wire",	    [RAILFS_PHASE_WRITE_DIGEST] = "write.digest",
	[RAILFS_PHASE_STAT] = "stat",		    [RAILFS_PHASE_POOL_WAIT] = "pool.wait",
};

// Called from process and workqueue context, never from an interrupt, so
// disabling preemption around the update is enough to own the cpu's slot.
void railfs_trace_add(enum railfs_phase phase, u64 began, u64 bytes)
{
	struct railfs_counters *mine;
	u64 spent = railfs_now() - began;

	if (phase >= RAILFS_PHASE_MAX) {
		return;
	}

	mine = get_cpu_ptr(&railfs_counters);
	mine->phase[phase].ns += spent;
	mine->phase[phase].calls++;
	mine->phase[phase].bytes += bytes;
	put_cpu_ptr(&railfs_counters);
}

static void railfs_trace_reset(void)
{
	int cpu;

	atomic_set(&railfs_inflight_max, atomic_read(&railfs_inflight));
	atomic_set(&railfs_busy_max, atomic_read(&railfs_busy));

	for_each_possible_cpu(cpu) {
		memset(per_cpu_ptr(&railfs_counters, cpu), 0, sizeof(struct railfs_counters));
	}
}

static int railfs_stats_show(struct seq_file *m, void *unused)
{
	unsigned int phase;

	seq_printf(m, "inflight now %d, most %d\n", atomic_read(&railfs_inflight), atomic_read(&railfs_inflight_max));
	seq_printf(m, "conns busy now %d, most %d\n", atomic_read(&railfs_busy), atomic_read(&railfs_busy_max));
	seq_printf(m, "%-14s %10s %14s %12s %10s\n", "phase", "calls", "ns", "MB", "us/call");

	for (phase = 0; phase < RAILFS_PHASE_MAX; phase++) {
		struct railfs_counter total = {};
		int cpu;

		for_each_possible_cpu(cpu) {
			struct railfs_counters *theirs = per_cpu_ptr(&railfs_counters, cpu);

			total.ns += theirs->phase[phase].ns;
			total.calls += theirs->phase[phase].calls;
			total.bytes += theirs->phase[phase].bytes;
		}

		if (!total.calls) {
			continue;
		}

		seq_printf(m, "%-14s %10llu %14llu %12llu %10llu\n", railfs_phase_names[phase], total.calls, total.ns,
			   total.bytes >> 20, div64_u64(total.ns, total.calls * 1000));
	}

	return 0;
}

static int railfs_stats_open(struct inode *inode, struct file *file) { return single_open(file, railfs_stats_show, NULL); }

static ssize_t railfs_stats_write(struct file *file, const char __user *from, size_t len, loff_t *at)
{
	railfs_trace_reset();
	return len;
}

static const struct file_operations railfs_stats_fops = {
	.owner = THIS_MODULE,
	.open = railfs_stats_open,
	.read = seq_read,
	.write = railfs_stats_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void railfs_trace_start(void)
{
	railfs_trace_reset();

	// Debugfs missing is not a reason to refuse a mount, so this is reported
	// by its absence rather than by failing.
	railfs_debug_dir = debugfs_create_dir("railfs", NULL);
	debugfs_create_file("stats", 0644, railfs_debug_dir, NULL, &railfs_stats_fops);
}

void railfs_trace_stop(void)
{
	debugfs_remove_recursive(railfs_debug_dir);
	railfs_debug_dir = NULL;
}
