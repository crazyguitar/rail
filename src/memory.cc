#include "rail/memory.h"

#ifdef RAIL_HAVE_RDMA
#include "rail/transport/rdma-device.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace rail {

struct MemoryRegion {
  std::byte *Base = nullptr;
  size_t Length = 0;
  std::vector<bool> Taken;
  size_t Used = 0;
#ifdef RAIL_HAVE_RDMA
  std::vector<ibv_mr *> Registered;
#endif

  ~MemoryRegion() {
#ifdef RAIL_HAVE_RDMA
    for (ibv_mr *One : Registered)
      if (One) ibv_dereg_mr(One);
#endif
    std::free(Base);
  }

  MemoryRegion() = default;
  MemoryRegion(const MemoryRegion &) = delete;
  MemoryRegion &operator=(const MemoryRegion &) = delete;
};

namespace {

constexpr size_t kAlignment = 4096;
constexpr size_t kDefaultRegionBytes = 1ull << 30;
constexpr size_t kDefaultFrameSize = 1u << 20;
constexpr size_t kMaxRegions = 64;
constexpr size_t kReclaimableShare = 4;

size_t framesFor(size_t Bytes, size_t FrameSize) { return (Bytes + FrameSize - 1) / FrameSize; }

size_t availableBytes() {
  std::FILE *F = std::fopen("/proc/meminfo", "re");
  if (!F) return 0;

  char Line[256];
  unsigned long long Kb = 0;
  while (std::fgets(Line, sizeof(Line), F))
    if (std::sscanf(Line, "MemAvailable: %llu kB", &Kb) == 1) break;
  std::fclose(F);
  return static_cast<size_t>(Kb) * 1024;
}

size_t askedForRegions() {
  const char *Set = std::getenv("RAIL_MEMORY_REGIONS");
  return Set ? std::strtoul(Set, nullptr, 10) : 0;
}

size_t askedForRegionBytes() {
  const char *Set = std::getenv("RAIL_MEMORY_REGION_BYTES");
  const unsigned long long Asked = Set ? std::strtoull(Set, nullptr, 10) : 0;
  return Asked >= kDefaultFrameSize ? static_cast<size_t>(Asked) : kDefaultRegionBytes;
}

} // namespace

Page::Page(Page &&Other) noexcept
    : Ptr(Other.Ptr), Len(Other.Len), Capacity(Other.Capacity), Home(Other.Home), Hook(Other.Hook), Lender(Other.Lender), Index(Other.Index) {
  Other.reset();
}

Page &Page::operator=(Page &&Other) noexcept {
  if (this != &Other) {
    if (Hook) Hook(Lender, Index);
    Ptr = Other.Ptr;
    Len = Other.Len;
    Capacity = Other.Capacity;
    Home = Other.Home;
    Hook = Other.Hook;
    Lender = Other.Lender;
    Index = Other.Index;
    Other.reset();
  }
  return *this;
}

Page::~Page() {
  if (Hook) Hook(Lender, Index);
}

void Page::reset() {
  Ptr = nullptr;
  Len = 0;
  Capacity = 0;
  Home = nullptr;
  Hook = nullptr;
  Lender = nullptr;
  Index = 0;
}

Run::Run(Run &&Other) noexcept
    : Ptr(Other.Ptr), Length(Other.Length), Home(Other.Home), First(Other.First), Frames(Other.Frames), Owner(Other.Owner) {
  Other.reset();
}

Run &Run::operator=(Run &&Other) noexcept {
  if (this != &Other) {
    if (Owner) Owner->release(Home, First, Frames);
    Ptr = Other.Ptr;
    Length = Other.Length;
    Home = Other.Home;
    First = Other.First;
    Frames = Other.Frames;
    Owner = Other.Owner;
    Other.reset();
  }
  return *this;
}

Run::~Run() {
  if (Owner) Owner->release(Home, First, Frames);
}

void Run::reset() {
  Ptr = nullptr;
  Length = 0;
  Home = nullptr;
  First = 0;
  Frames = 0;
  Owner = nullptr;
}

Page Run::borrow(size_t Offset, size_t Bytes) const {
  if (!Ptr || Offset > Length || Bytes > Length - Offset) return Page{};
  return Page(Ptr + Offset, Bytes, Home);
}

Memory::Memory(size_t Regions, size_t RegionBytes, size_t FrameSize) : RegionBytes(RegionBytes), FrameSize(FrameSize), RegionCeiling(Regions) {
  if (!grow()) throw std::bad_alloc();
}

Memory::~Memory() = default;

// Registering a gigabyte faults it in and costs about 0.29 s, so none of it
// happens under the allocation lock: a reader that has to grow must not stall
// every other reader while it does.
std::unique_ptr<MemoryRegion> Memory::build() {
  auto Fresh = std::make_unique<MemoryRegion>();
  void *Raw = nullptr;
  if (::posix_memalign(&Raw, kAlignment, RegionBytes) != 0) return nullptr;
  Fresh->Base = static_cast<std::byte *>(Raw);
  Fresh->Length = RegionBytes;
  Fresh->Taken.assign(RegionBytes / FrameSize, false);

#ifdef RAIL_HAVE_RDMA
  std::vector<std::shared_ptr<RdmaDevice>> Attached;
  {
    const std::lock_guard<std::mutex> Held(Lock);
    Attached = Devices;
  }
  if (auto R = registerWith(*Fresh, Attached); !R) return nullptr;
#endif

  return Fresh;
}

bool Memory::grow() {
  const std::lock_guard<std::mutex> Alone(Growing);
  return growLocked();
}

bool Memory::growLocked() {
  {
    const std::lock_guard<std::mutex> Held(Lock);
    if (Regions.size() >= RegionCeiling) return false;
  }

  auto Fresh = build();
  if (!Fresh) return false;

  const std::lock_guard<std::mutex> Held(Lock);
  Regions.push_back(std::move(Fresh));
  Stats.RegionsGrown++;
  return true;
}

Run Memory::takeLocked(size_t Frames) {
  for (const auto &One : Regions) {
    const size_t Count = One->Taken.size();
    if (Frames > Count - One->Used) continue;

    size_t Consecutive = 0;
    for (size_t I = 0; I < Count; I++) {
      Consecutive = One->Taken[I] ? 0 : Consecutive + 1;
      if (Consecutive < Frames) continue;

      const size_t First = I + 1 - Frames;
      for (size_t J = First; J <= I; J++) One->Taken[J] = true;
      One->Used += Frames;
      Stats.Allocated++;
      return Run(One->Base + First * FrameSize, Frames * FrameSize, One.get(), First, Frames, this);
    }
  }
  return Run{};
}

Run Memory::alloc(size_t Bytes) {
  if (Bytes == 0) return Run{};
  const size_t Frames = framesFor(Bytes, FrameSize);

  {
    const std::lock_guard<std::mutex> Held(Lock);
    if (Frames > RegionBytes / FrameSize) {
      Stats.Exhausted++;
      return Run{};
    }
    if (Run Mine = takeLocked(Frames); Mine.valid()) return Mine;
  }

  // A second look once we are the one grower: whoever held this before us may
  // have added the region we need, and another on top of it would be wasted.
  const std::lock_guard<std::mutex> Alone(Growing);
  {
    const std::lock_guard<std::mutex> Held(Lock);
    if (Run Mine = takeLocked(Frames); Mine.valid()) return Mine;
  }

  if (!growLocked()) {
    const std::lock_guard<std::mutex> Held(Lock);
    Stats.Exhausted++;
    return Run{};
  }

  const std::lock_guard<std::mutex> Held(Lock);
  Run Mine = takeLocked(Frames);
  if (!Mine.valid()) Stats.Exhausted++;
  return Mine;
}

void Memory::release(MemoryRegion *R, size_t First, size_t Frames) {
  const std::lock_guard<std::mutex> Held(Lock);
  for (size_t I = First; I < First + Frames; I++) R->Taken[I] = false;
  R->Used -= Frames;
}

size_t Memory::freeFrames() const {
  const std::lock_guard<std::mutex> Held(Lock);
  size_t Free = 0;
  for (const auto &One : Regions)
    for (size_t I = 0; I < One->Taken.size(); I++)
      if (!One->Taken[I]) Free++;
  return Free;
}

size_t Memory::totalFrames() const {
  const std::lock_guard<std::mutex> Held(Lock);
  size_t Total = 0;
  for (const auto &One : Regions) Total += One->Taken.size();
  return Total;
}

void Memory::countDirect() {
  const std::lock_guard<std::mutex> Held(Lock);
  Stats.Direct++;
}

void Memory::countCopied() {
  const std::lock_guard<std::mutex> Held(Lock);
  Stats.Copied++;
}

MemoryStats Memory::stats() const {
  const std::lock_guard<std::mutex> Held(Lock);
  return Stats;
}

namespace {

size_t defaultCeiling() {
  const size_t Asked = askedForRegions();
  return Asked > 0 ? Asked : regionsThatFit(availableBytes(), kDefaultRegionBytes);
}

Memory &neverDestroyed() {
  static Memory *Only = new Memory(defaultCeiling(), askedForRegionBytes(), kDefaultFrameSize);
  return *Only;
}

} // namespace

size_t regionsThatFit(size_t Available, size_t RegionBytes) {
  if (RegionBytes == 0) return 1;
  const size_t Fits = (Available / kReclaimableShare) / RegionBytes;
  if (Fits < 1) return 1;
  return Fits > kMaxRegions ? kMaxRegions : Fits;
}

#ifdef RAIL_HAVE_RDMA

Result<void> Memory::registerWith(MemoryRegion &R, const std::vector<std::shared_ptr<RdmaDevice>> &With) {
  for (const auto &Device : With) {
    const size_t Slot = Device->slot();
    if (R.Registered.size() <= Slot) R.Registered.resize(Slot + 1, nullptr);
    if (R.Registered[Slot]) continue;

    const int Access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ibv_mr *Made = ibv_reg_mr(Device->domain(), R.Base, R.Length, Access);
    if (!Made) return failErrno("ibv_reg_mr for a memory region");
    R.Registered[Slot] = Made;
  }
  return {};
}

// Under the grow lock too, so a device cannot arrive between a build's device
// snapshot and its push and be missed by both.
Result<void> Memory::attach(const std::shared_ptr<RdmaDevice> &Device) {
  const std::lock_guard<std::mutex> Alone(Growing);
  const std::lock_guard<std::mutex> Held(Lock);
  for (const auto &Known : Devices)
    if (Known->slot() == Device->slot()) return Result<void>{};

  const std::vector<std::shared_ptr<RdmaDevice>> Incoming{Device};
  for (const auto &One : Regions)
    if (auto R = registerWith(*One, Incoming); !R) return R;

  Devices.push_back(Device);
  return {};
}

uint32_t Memory::rkeyOf(const Page &P, size_t Slot) const {
  const MemoryRegion *R = P.region();
  if (!R || Slot >= R->Registered.size() || !R->Registered[Slot]) return 0;
  return R->Registered[Slot]->rkey;
}

uint32_t Memory::lkeyOf(const Page &P, size_t Slot) const {
  const MemoryRegion *R = P.region();
  if (!R || Slot >= R->Registered.size() || !R->Registered[Slot]) return 0;
  return R->Registered[Slot]->lkey;
}

#endif

Memory &Memory::get() { return neverDestroyed(); }

} // namespace rail
