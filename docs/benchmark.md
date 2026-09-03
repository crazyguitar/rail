# Benchmark

```bash
salloc -N2 make bench                                    # everything, 5 repetitions
salloc -N2 make bench FILTER=BM_RdmaSend                 # the fabric alone
salloc -N2 make bench BENCH='--cpus 8-15'                # pinned, for comparable numbers
scripts/run-bench.sh --local --peer 10.0.0.2 --fabric 10.10.0.2
```

Both nodes are taken exclusively: a benchmark sharing a machine measures the
sharing. `--fabric` is the address dialled — left out it falls back to the ssh
route, and the run reports wifi.

In Kubernetes:

```bash
helm install rail-bench charts/bench -n rail-test \
  --set peer=10.0.0.2 --set fabric=10.10.0.2 --set ssh.user=you \
  --set node=<node> --set cpus=8-15

kubectl -n rail-test logs -f job/rail-bench
```

Same values as the test chart in `docs/testing.md`, plus `fabric` and `cpus`.
`filter` here is a google/benchmark regex, so everything is `.` rather than `*`.

`BM_RdmaSend` is buffers only, so it measures the fabric. `BM_Read` and
`BM_Write` mount by themselves and produce the tables below:

```bash
salloc -N2 make bench FILTER='BM_Read|BM_Write/'
```

GiB/s between two hosts on the same RoCE fabric, median of seven repetitions
with the standard deviation beside it, `--cpus 8-15` and the daemon on the peer
left unpinned. `q` streams, each moving a 1 GiB file of its own. Reads drop
both page caches first; writes end in `fsync` at both ends.

Read:

| q | p2p | nvme | rail | nfs | fuse | railfs |
| --- | --- | --- | --- | --- | --- | --- |
| 4 | 20.99 ±0.09 | 10.86 ±0.08 | 9.64 ±1.74 | 4.47 ±0.09 | 5.61 ±0.21 | 9.73 ±0.13 |
| 8 | 21.05 ±0.12 | 10.94 ±0.03 | 9.25 ±0.73 | 6.12 ±0.04 | 8.86 ±0.28 | 10.07 ±0.05 |
| 16 | 21.04 ±0.07 | 10.68 ±0.03 | 8.55 ±0.56 | 8.46 ±0.07 | 8.66 ±0.41 | 10.02 ±0.05 |

Write:

| q | p2p | nvme | rail | nfs | fuse | railfs |
| --- | --- | --- | --- | --- | --- | --- |
| 4 | 21.03 ±0.10 | 9.93 ±0.16 | 4.70 ±0.24 | 8.35 ±0.20 | 4.41 ±0.13 | 8.66 ±0.49 |
| 8 | 21.07 ±0.19 | 10.51 ±0.09 | 8.60 ±0.18 | 9.04 ±0.10 | 4.73 ±0.20 | 9.36 ±0.14 |
| 16 | 21.09 ±0.04 | 10.79 ±0.09 | 10.73 ±0.08 | 9.34 ±0.09 | 5.05 ±0.33 | 9.85 ±0.08 |

`p2p` opens no file and `nvme` is this machine's own disk, so a row near either
is bound by the fabric or a drive rather than the filesystem above it.

Two columns move more than their deviation says between runs on the same code.
`rail` reads at `q=4` are bimodal, near 9.7 or near 7, depending on where the
peer's daemon threads land. `fuse` writes vary with how far the peer's drive
has recovered from the case before, by 20% from one pass to the next; a pass
of that column on its own reads higher than a pass inside the whole table.

`--daemon-cpus LIST` pins `raild` on the peer as well. It costs every write
column 5 to 20%, because the daemon's io_uring workers inherit the mask, so it
is off unless asked for.

## By thread count

`fio`, `--direct=1`, one 512 MiB file per thread, `nvme` at
`read_ahead_kb=8192`. Reads are the median of three, writes of five, one target
at a time: a write straight after another's lands on a drive that has not
recovered.

Read, 256 KiB:

| threads | nvme | nfs | fuse | railfs |
| --- | --- | --- | --- | --- |
| 1 | 1.14 | 0.35 | 1.41 | 6.76 |
| 2 | 1.82 | 0.64 | 2.63 | 8.70 |
| 4 | 3.12 | 1.38 | 4.96 | 10.10 |
| 8 | 4.73 | 1.68 | 7.34 | 9.95 |
| 16 | 8.41 | 1.87 | 7.34 | 9.91 |
| 32 | 9.81 | 1.51 | 8.11 | 9.86 |
| 64 | 9.76 | 1.71 | 7.61 | 9.78 |
| 128 | 9.70 | 9.31 | 7.55 | 9.75 |

Read, 1 MiB:

| threads | nvme | nfs | fuse | railfs |
| --- | --- | --- | --- | --- |
| 1 | 5.95 | 0.83 | 2.59 | 6.41 |
| 2 | 6.06 | 1.72 | 5.78 | 9.09 |
| 4 | 9.48 | 3.36 | 8.70 | 9.85 |
| 8 | 9.98 | 3.71 | 9.05 | 10.08 |
| 16 | 10.04 | 4.35 | 9.52 | 9.96 |
| 32 | 9.94 | 3.88 | 9.44 | 9.87 |
| 64 | 9.72 | 4.12 | 9.38 | 9.75 |
| 128 | 9.64 | 6.77 | 9.40 | 9.73 |

Write, 256 KiB:

| threads | nvme | nfs | fuse | railfs |
| --- | --- | --- | --- | --- |
| 1 | 1.09 | 0.05 | 1.91 | 2.79 |
| 2 | 1.60 | 0.08 | 2.49 | 5.68 |
| 4 | 3.24 | 0.16 | 3.55 | 8.97 |
| 8 | 6.22 | 0.28 | 4.59 | 10.00 |
| 16 | 10.30 | 0.37 | 5.34 | 10.36 |
| 32 | 10.95 | 0.47 | 6.37 | 10.78 |
| 64 | 11.10 | 0.57 | 8.02 | 10.87 |
| 128 | 11.07 | 0.70 | 9.13 | 10.94 |

Write, 1 MiB:

| threads | nvme | nfs | fuse | railfs |
| --- | --- | --- | --- | --- |
| 1 | 6.67 | 0.19 | 1.58 | 2.87 |
| 2 | 8.55 | 0.34 | 2.50 | 6.02 |
| 4 | 9.85 | 0.67 | 3.70 | 9.05 |
| 8 | 10.64 | 1.13 | 4.69 | 10.10 |
| 16 | 10.96 | 1.14 | 5.96 | 10.38 |
| 32 | 11.00 | 1.37 | 7.05 | 10.69 |
| 64 | 10.74 | 1.67 | 8.26 | 10.81 |
| 128 | 10.62 | 2.02 | 9.26 | 10.93 |

O_DIRECT means different things per row: `nvme` and `nfs` lose their readahead
to it, `railfs` keeps its own and answers the flag on the peer.

One stream, `dd`, 512 MiB, O_DIRECT:

| | nvme | nfs | fuse | railfs |
| --- | --- | --- | --- | --- |
| read | 4.65 | 0.90 | 3.64 | 8.37 |
| write | 4.21 | 0.19 | 2.05 | 2.24 |

## With fio and dd

Drop both page caches first or the number is memory:

```bash
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
ssh <peer> 'sync; echo 3 | sudo tee /proc/sys/vm/drop_caches'
```

`dd`, one stream. `iflag=direct` costs nothing above 8 KiB; `conv=fsync`
keeps the cache out of the write:

```bash
dd if=/mnt/8g.bin of=/dev/null bs=1M iflag=direct
dd if=/dev/zero of=/mnt/out.bin bs=1M count=8192 conv=fsync
```

`fio`, q streams on q files of their own. Write them before reading them:

```bash
fio --name=read --directory=/mnt --rw=write --bs=1M --size=1G \
    --numjobs=8 --end_fsync=1 --group_reporting

fio --name=read --directory=/mnt --rw=read --bs=1M --size=1G \
    --numjobs=8 --direct=1 --group_reporting

fio --name=write --directory=/mnt --rw=write --bs=1M --size=1G \
    --numjobs=8 --end_fsync=1 --group_reporting
```

`--numjobs` is the `q` of the tables. `dd` reports GB/s and `fio` GiB/s.

The thread sweep, buffered:

```bash
for t in 1 2 4 8 16 32 64 128; do
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
  ssh <peer> 'sync; echo 3 | sudo tee /proc/sys/vm/drop_caches'
  fio --name=scale --directory=/mnt --rw=read --bs=256k --size=512M \
      --numjobs=$t --group_reporting
done
```

Raise the readahead for the `nvme` column or you measure the default: 2.0 GiB/s
at the stock 128 KiB against 9.9 at 1 MiB.

```bash
echo 1024 | sudo tee /sys/block/nvme0n1/queue/read_ahead_kb   # 1 MiB tables
echo 8192 | sudo tee /sys/block/nvme0n1/queue/read_ahead_kb   # thread sweep
```
