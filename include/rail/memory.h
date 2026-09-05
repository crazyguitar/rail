#pragma once

#include "rail/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace rail {

class AddressSpace;
class Memory;
class RdmaDevice;
struct MemoryRegion;

struct MemoryStats {
  uint64_t Allocated = 0;
  uint64_t Direct = 0;
  uint64_t Copied = 0;
  uint64_t Exhausted = 0;
  uint64_t RegionsGrown = 0;
};

// One transfer page. Not an OS page: these are megabytes, sized so RDMA
// transfers are efficient. The hook is how a page returns itself to whoever
// lent it, and is null on a page that only borrows its bytes.
class Page {
public:
  using Release = void (*)(void *, size_t);

  Page() = default;
  Page(const Page &) = delete;
  Page &operator=(const Page &) = delete;
  Page(Page &&Other) noexcept;
  Page &operator=(Page &&Other) noexcept;
  ~Page();

  Page(std::byte *P, size_t Cap, MemoryRegion *R, Release Hook, void *Lender, size_t Index)
      : Ptr(P), Len(Cap), Capacity(Cap), Home(R), Hook(Hook), Lender(Lender), Index(Index) {}

  std::span<std::byte> data() { return {Ptr, Len}; }
  std::span<const std::byte> data() const { return {Ptr, Len}; }
  std::byte *bytes() { return Ptr; }

  size_t size() const { return Len; }
  size_t capacity() const { return Capacity; }
  void resize(size_t N) { Len = N < Capacity ? N : Capacity; }
  bool valid() const { return Ptr != nullptr; }

  MemoryRegion *region() const { return Home; }

private:
  friend class Run;
  friend class AddressSpace;
  Page(std::byte *P, size_t Cap, MemoryRegion *R) : Ptr(P), Len(Cap), Capacity(Cap), Home(R) {}

  void reset();

  std::byte *Ptr = nullptr;
  size_t Len = 0;
  size_t Capacity = 0;
  MemoryRegion *Home = nullptr;
  Release Hook = nullptr;
  void *Lender = nullptr;
  size_t Index = 0;
};

class Run {
public:
  Run() = default;
  Run(const Run &) = delete;
  Run &operator=(const Run &) = delete;
  Run(Run &&Other) noexcept;
  Run &operator=(Run &&Other) noexcept;
  ~Run();

  std::byte *bytes() const { return Ptr; }
  size_t size() const { return Length; }
  bool valid() const { return Ptr != nullptr; }
  MemoryRegion *region() const { return Home; }

  Page borrow(size_t Offset, size_t Bytes) const;

private:
  friend class Memory;
  Run(std::byte *P, size_t Length, MemoryRegion *R, size_t First, size_t Frames, Memory *O)
      : Ptr(P), Length(Length), Home(R), First(First), Frames(Frames), Owner(O) {}

  void reset();

  std::byte *Ptr = nullptr;
  size_t Length = 0;
  MemoryRegion *Home = nullptr;
  size_t First = 0;
  size_t Frames = 0;
  Memory *Owner = nullptr;
};

class Memory {
public:
  Memory(size_t Regions, size_t RegionBytes, size_t FrameSize);
  Memory(const Memory &) = delete;
  Memory &operator=(const Memory &) = delete;
  ~Memory();

  static Memory &get();

  Run alloc(size_t Bytes);

  size_t frameSize() const { return FrameSize; }
  size_t regionCeiling() const { return RegionCeiling; }
  size_t capacity() const { return RegionCeiling * RegionBytes; }

#ifdef RAIL_HAVE_RDMA
  Result<void> attach(const std::shared_ptr<RdmaDevice> &Device);
  uint32_t rkeyOf(const Page &P, size_t Slot) const;
  uint32_t lkeyOf(const Page &P, size_t Slot) const;
#endif
  size_t freeFrames() const;
  size_t totalFrames() const;
  MemoryStats stats() const;
  void countDirect();
  void countCopied();

private:
  friend class Run;

  void release(MemoryRegion *R, size_t First, size_t Frames);
  std::unique_ptr<MemoryRegion> build();
  bool grow();
  bool growLocked();
#ifdef RAIL_HAVE_RDMA
  Result<void> registerWith(MemoryRegion &R, const std::vector<std::shared_ptr<RdmaDevice>> &With);
#endif
  Run takeLocked(size_t Frames);

  mutable std::mutex Lock;
  std::mutex Growing;
  std::vector<std::unique_ptr<MemoryRegion>> Regions;
  size_t RegionBytes = 0;
  size_t FrameSize = 0;
  size_t RegionCeiling = 0;
  MemoryStats Stats;
#ifdef RAIL_HAVE_RDMA
  std::vector<std::shared_ptr<RdmaDevice>> Devices;
#endif
};

size_t regionsThatFit(size_t Available, size_t RegionBytes);

} // namespace rail
