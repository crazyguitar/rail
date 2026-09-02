
#include "fixtures.h"

#include <cstring>

namespace {
using namespace rail;
using namespace rail::bench;

void BM_KernelRead(benchmark::State &State) {
  const uint64_t Size = static_cast<uint64_t>(State.range(0));
  const size_t Block = static_cast<size_t>(State.range(1));

  if (!kernelMountReady(State)) return;
  coldData(peerHost());

  AlignedPages Landing(1, Block);
  if (!allocated(State, Landing)) return;

  const std::string Source = onKernelMount(targetFor(kTargetSize));
  measure(State, Size, [&]() -> Result<void> {
    OpenFd Fd(Source, O_RDONLY);
    if (!Fd.valid()) return failErrno("open the fixture through the kernel mount");
    return readWholeFile(Fd.get(), 0, Size, Landing[0]);
  });
}

void BM_WriteOverKernel(benchmark::State &State) {
  const uint64_t Size = static_cast<uint64_t>(State.range(0));
  const size_t Block = static_cast<size_t>(State.range(1));

  if (!kernelMountReady(State)) return;
  coldData(peerHost());

  AlignedPages Source(1, Block);
  if (!allocated(State, Source)) return;
  std::memset(Source[0].data(), 0x5a, Block);

  const std::string Target = onKernelMount("bench-kwrite.bin");
  measure(State, Size, [&]() -> Result<void> {
    OpenFd Fd(Target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!Fd.valid()) return failErrno("create a file through the kernel mount");

    for (uint64_t Done = 0; Done < Size;) {
      const size_t Want = static_cast<size_t>(std::min<uint64_t>(Block, Size - Done));
      const ssize_t Wrote = ::pwrite(Fd.get(), Source[0].data(), Want, static_cast<off_t>(Done));
      if (Wrote < 0) return failErrno("pwrite through the kernel mount");
      if (static_cast<size_t>(Wrote) != Want) return failMessage("a write through the kernel mount did not land");
      Done += Want;
    }

    if (::fsync(Fd.get()) != 0) return failErrno("fsync through the kernel mount");
    return Fd.release();
  });

  ::unlink(Target.c_str());
}

} // namespace

BENCHMARK(BM_KernelRead)->Args({1ll << 30, kBlockBytes})->Args({4ll << 30, kBlockBytes})->Args({4ll << 30, 8ll << 20})->Apply(wholeFile);
BENCHMARK(BM_WriteOverKernel)->Args({1ll << 30, kBlockBytes})->Args({8ll << 30, kBlockBytes})->Apply(wholeFile);
