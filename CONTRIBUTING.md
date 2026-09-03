# Contributing

## Build

Linux only. See [docs/build.md](docs/build.md) for the packages needed; then:

```bash
make                       # everything, the kernel module included
```

CI builds the tree on Ubuntu 24.04 for amd64 and arm64, checks that
`railfs.ko` came out, runs the tests that need no peer, and builds the Debian
packages. It runs exactly this:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
RAIL_MEMORY_REGIONS=4 ./build/tests/e2e/rail-e2e --gtest_filter='Codec.*:SafePath.*:Loop.*:Uring.*:Xdr.*:Pages.*'
dpkg-buildpackage -us -uc -b
```

## Test

Everything else needs two machines with RDMA between them; see
[docs/testing.md](docs/testing.md). Run the whole suite, for both backends,
before opening a pull request:

```bash
salloc -N2 scripts/run-e2e.sh
scripts/sanitize.sh            # the same suite under ASan and UBSan
```

A test that fails once and passes on retry is a bug, not noise. Find the
cause rather than rerunning until green.

## Performance

A change may not cost throughput, however small the loss and whatever it
fixes. Measure the paths you touched against `main` on the same machines in
the same minutes, medians of several repetitions, and put the numbers in the
pull request:

```bash
salloc -N2 make bench FILTER='BM_Read|BM_Write/' BENCH='--cpus 8-15 --repetitions 7'
```

The figures in [docs/benchmark.md](docs/benchmark.md) say what the hardware
does today; a number well below them is a problem to chase, not a result to
report.

## Ground rules

- A buffer the transport still references never moves: await the operation
  in place, then move the entry.
- Every window is bounded by what the transport allows in flight and by what
  the page pool holds; either limit exceeded corrupts data or deadlocks.
- A posted receive always gets its send, on error paths too, or the peer waits
  forever. Report failures in a trailing message instead.
- Join every outstanding send, receive and write before the owner dies.
- A check that has never fired is not a check: break it on purpose once and
  watch it fail.

## Style

LLVM style, 150 columns, `make format` before committing. Files use hyphens,
never underscores, and drop a prefix the directory already supplies
(`fs/reader.cc`). Tests are `test-*.cc` and named in plain English
(`MetadataStaysInRoot`). Intent goes into names and structure; a comment is
for a why the code cannot say, in three lines at most. `src/linux` follows
the kernel's own style and is GPL-2.0; everything else is Apache-2.0.

## Pull requests

One logical change per pull request, with the template filled in: what was
run, on what, and what it printed. If a path could not be tested on hardware,
say so and say what was verified instead.
