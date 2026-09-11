# GPUDirect Storage

The kernel mount accepts cuFile GPU buffers over RDMA. `nvidia-fs` maps the GPU
pages for each NIC; the peer writes directly into those pages on reads and the
client NIC reads them on writes. The peer needs ordinary raild with RDMA support.

This is experimental filesystem support. Use `rdma,noverify`: the existing
payload checksum runs on the CPU and cannot hash GPU memory. TCP and verified
mounts reject GPU I/O. Normal CPU I/O keeps its existing behavior.

## Build

Build railfs against the running kernel and RDMA driver headers. With Mellanox
OFED, use its compatibility headers and symbol versions rather than stock RDMA
headers:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target rail-e2e -j8
ofa_headers=/usr/src/ofa_kernel/x86_64/$(uname -r)
make -C /lib/modules/$(uname -r)/build M="$PWD/src/linux" \
  KBUILD_EXTRA_SYMBOLS="$ofa_headers/Module.symvers" \
  NOSTDINC_FLAGS="-I$ofa_headers/include -include $ofa_headers/include/linux/compat-2.6.h" modules
```

The CUDA tests build when CMake finds nvcc and cuFile. The kernel module itself
has no CUDA toolkit dependency.

Apply [the companion driver patch](../patch/nvidia-fs-railfs.patch) to an
unpatched `gds-nvidia-fs` checkout. From the rail repository root:

```bash
git -C ../../fs/gds-nvidia-fs apply "$PWD/patch/nvidia-fs-railfs.patch"
```

The patch registers railfs's GPU DMA callbacks and excludes railfs from the
local-filesystem preallocation path. Skip applying it if those changes are
already present.

Build that driver with `make` in its `src` directory. Matching NVIDIA driver
sources, including `nv-p2p.h`, must be available under `/usr/src`.

## Run

Load railfs, then the patched nvidia-fs module. Mount railfs with `rdma,noverify`.
Set these variables before starting a cuFile application:

```bash
export CUFILE_EXPERIMENTAL_FS=1
export CUFILE_ALLOW_COMPAT_MODE=false
export CUFILE_FORCE_COMPAT_MODE=false
export CUFILE_USE_PCIP2PDMA=false
```

NVIDIA documents `CUFILE_EXPERIMENTAL_FS=1` for filesystems that are not yet
built into cuFile's supported list in its
[troubleshooting guide](https://docs.nvidia.com/gpudirect-storage/troubleshooting-guide/#environment-variables-used-by-gpudirect-storage).
The compatibility settings prevent successful CPU fallback from masking a
broken GPU path.

Use registered CUDA buffers and `O_DIRECT` file descriptors. Start with offsets
and lengths aligned to 4096 bytes. Reads stop at the cached file size and preserve
the buffer beyond EOF. If the peer reports a conflicting short read after the
request is sent, railfs rejects the transfer rather than accepting its padded
payload into GPU memory.

In a privileged container without udev, create the driver device nodes after
loading nvidia-fs:

```bash
for device in /sys/class/nvidia-fs-class/*; do
  numbers=$(cat "$device/dev")
  test -e "/dev/${device##*/}" || \
    mknod "/dev/${device##*/}" c "${numbers%:*}" "${numbers#*:}"
done
```

A privileged container can see GPUs outside its allocation. Set
`CUDA_VISIBLE_DEVICES` to the allocated GPU UUID; on the SkyPilot test container
this is `$NVIDIA_VISIBLE_DEVICES`.

## Test

Use an idle client with exclusive ownership of nvidia-fs. The suite loads and
unloads both test modules; unload nvidia-fs before starting it. The peer must
have raild built at the same path and be reachable by SSH.

```bash
export RAIL_GDS_TESTS=1
export RAIL_KO="$PWD/src/linux/railfs.ko"
export RAIL_NVFS_KO=/path/to/gds-nvidia-fs/src/nvidia-fs.ko
scripts/run-e2e.sh --local --peer rail-peer --filter 'Gds.*'
```

The suite checks GPU contents using CUDA kernels and checks native read/write
counters under `/proc/driver/nvidia-fs/stats`. It enables `rw_stats_enabled` when
loading nvidia-fs; disabled counters are not accepted as evidence of direct I/O.
EOF and rejected-I/O tests check that GPU buffer sentinels remain untouched.
