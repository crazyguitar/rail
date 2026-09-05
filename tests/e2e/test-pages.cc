#include "rail/address-space.h"
#include "rail/file-service.h"
#include "rail/io/runner.h"
#include "rail/memory.h"
#include "rail/page-pool.h"
#include "rail/stream/sink.h"
#ifdef RAIL_HAVE_RDMA
#include "rail/transport/rdma-device.h"
#include "rail/transport/rdma-devices.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace rail;

namespace {

constexpr size_t kFrame = 1u << 20;

} // namespace

TEST(Pages, AllocRoundsUpToFrames) {
  Memory M(1, 32u << 20, kFrame);
  rail::Run E = M.alloc(kFrame + 1);
  ASSERT_TRUE(E.valid());
  EXPECT_EQ(E.size(), 2 * kFrame);
  EXPECT_NE(E.bytes(), nullptr);
}

TEST(Pages, RunsDoNotOverlap) {
  Memory M(1, 32u << 20, kFrame);
  rail::Run First = M.alloc(4 * kFrame);
  rail::Run Second = M.alloc(4 * kFrame);
  ASSERT_TRUE(First.valid());
  ASSERT_TRUE(Second.valid());

  EXPECT_EQ(First.size(), 4 * kFrame);
  EXPECT_NE(First.bytes(), Second.bytes());

  const std::byte *End = First.bytes() + First.size();
  const std::byte *Other = Second.bytes();
  EXPECT_TRUE(End <= Other || Other + Second.size() <= First.bytes()) << "two live runs must not overlap";
}

TEST(Pages, RunsFreeOnDestruction) {
  Memory M(1, 32u << 20, kFrame);
  const size_t Whole = M.freeFrames();
  {
    rail::Run E = M.alloc(8 * kFrame);
    ASSERT_TRUE(E.valid());
    EXPECT_EQ(M.freeFrames(), Whole - 8);
  }
  EXPECT_EQ(M.freeFrames(), Whole);
}

TEST(Pages, AllocRefusesWhenFull) {
  Memory M(1, 4u << 20, kFrame);
  rail::Run Whole = M.alloc(4 * kFrame);
  ASSERT_TRUE(Whole.valid());

  rail::Run None = M.alloc(kFrame);
  EXPECT_FALSE(None.valid());
  EXPECT_EQ(M.stats().Exhausted, 1u);
}

TEST(Pages, AllocRefusesWhenFragmented) {
  Memory M(1, 4u << 20, kFrame);
  rail::Run A = M.alloc(kFrame);
  rail::Run B = M.alloc(kFrame);
  rail::Run C = M.alloc(kFrame);
  ASSERT_TRUE(A.valid());
  ASSERT_TRUE(B.valid());
  ASSERT_TRUE(C.valid());

  B = rail::Run{};
  EXPECT_EQ(M.freeFrames(), 2u) << "one frame in the middle and one at the end";

  rail::Run Two = M.alloc(2 * kFrame);
  EXPECT_FALSE(Two.valid()) << "two free frames that are not adjacent cannot serve a two-frame run";

  rail::Run One = M.alloc(kFrame);
  EXPECT_TRUE(One.valid()) << "a one-frame run still fits in the hole";
}

TEST(Pages, RunMoveLeavesSourceInert) {
  Memory M(1, 32u << 20, kFrame);
  const size_t Whole = M.freeFrames();

  rail::Run A = M.alloc(2 * kFrame);
  std::byte *Where = A.bytes();
  rail::Run B = std::move(A);

  EXPECT_FALSE(A.valid());
  EXPECT_EQ(B.bytes(), Where);
  EXPECT_EQ(M.freeFrames(), Whole - 2) << "a move must not free the run twice or leak it";
}

TEST(Pages, BorrowedPageOwnsNothing) {
  Memory M(1, 32u << 20, kFrame);
  const size_t Whole = M.freeFrames();

  rail::Run E = M.alloc(4 * kFrame);
  ASSERT_TRUE(E.valid());
  {
    Page P = E.borrow(kFrame, kFrame);
    ASSERT_TRUE(P.valid());
    EXPECT_EQ(P.bytes(), E.bytes() + kFrame);
    EXPECT_EQ(P.capacity(), kFrame);
    EXPECT_EQ(P.region(), E.region());
  }
  EXPECT_EQ(M.freeFrames(), Whole - 4) << "dropping a borrowed page must not free the run under it";

  EXPECT_FALSE(E.borrow(3 * kFrame, 2 * kFrame).valid()) << "a borrow past the end of the run is refused";
}

TEST(Pages, PoolRefusesOversizedPage) {
  ::testing::internal::CaptureStderr();
  PagePool Pool(1, 2ull << 30);
  const std::string Said = ::testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Pool.backed()) << "no region can hold a single 2 GiB page";
  EXPECT_NE(Said.find(std::to_string(2ull << 30)), std::string::npos)
      << "the refusal has to name the size it could not register, since the constructor cannot return it: " << Said;

  Page None = run(Pool.acquire());
  EXPECT_FALSE(None.valid()) << "an unbacked pool must hand back an invalid page, not a page over null";
}

TEST(Pages, PoolExceedsRegionSize) {
  PagePool Pool(5, 256u << 20);
  EXPECT_TRUE(Pool.backed()) << "1.25 GiB of pages must not need 1.25 GiB contiguous";
  EXPECT_EQ(Pool.footprint(), 5ull * (256u << 20));

  std::vector<Page> Held;
  for (int I = 0; I < 5; I++) {
    Held.push_back(run(Pool.acquire()));
    ASSERT_TRUE(Held.back().valid()) << "page " << I;
  }
  for (size_t I = 1; I < Held.size(); I++) EXPECT_NE(Held[0].bytes(), Held[I].bytes());
}

TEST(Pages, PoolPagesAreDistinct) {
  PagePool Pool(4, 1u << 20);
  ASSERT_TRUE(Pool.backed());

  Page First = run(Pool.acquire());
  Page Second = run(Pool.acquire());
  ASSERT_TRUE(First.valid());
  ASSERT_TRUE(Second.valid());

  EXPECT_EQ(First.capacity(), 1u << 20);
  EXPECT_NE(First.bytes(), Second.bytes());
}

// A grower that waited its turn must take from the region the last one added,
// not add another. The barrier lands the race nearly every round; the rounds
// make it certain.
TEST(Pages, ConcurrentGrowthAddsOnlyWhatIsNeeded) {
  constexpr size_t kThreads = 4;
  for (int Round = 0; Round < 20; Round++) {
    Memory M(4, 2 * kFrame, kFrame);
    std::barrier Go(static_cast<std::ptrdiff_t>(kThreads));
    std::vector<rail::Run> Held(kThreads);
    std::vector<std::thread> Threads;
    for (size_t I = 0; I < kThreads; I++)
      Threads.emplace_back([&, I] {
        Go.arrive_and_wait();
        Held[I] = M.alloc(kFrame);
      });
    for (auto &T : Threads) T.join();

    for (const auto &R : Held) ASSERT_TRUE(R.valid());
    EXPECT_EQ(M.stats().RegionsGrown, 2u) << "round " << Round << " grew a region nobody needed";
  }
}

TEST(Pages, GrowthStopsAtCeiling) {
  Memory M(2, kFrame * 2, kFrame);
  EXPECT_EQ(M.totalFrames(), 2u);
  EXPECT_EQ(M.stats().RegionsGrown, 1u);

  std::vector<rail::Run> Held;
  for (int I = 0; I < 4; I++) {
    Held.push_back(M.alloc(kFrame));
    ASSERT_TRUE(Held.back().valid()) << "frame " << I << " should have come from a grown region";
  }
  EXPECT_EQ(M.stats().RegionsGrown, 2u);
  EXPECT_EQ(M.totalFrames(), 4u);

  rail::Run Past = M.alloc(kFrame);
  EXPECT_FALSE(Past.valid());
  EXPECT_EQ(M.stats().RegionsGrown, 2u) << "the ceiling must stop growth";
  EXPECT_EQ(M.stats().Exhausted, 1u);
}

TEST(Pages, GrowthIsLazy) {
  Memory M(8, kFrame * 2, kFrame);
  EXPECT_EQ(M.stats().RegionsGrown, 1u) << "only the first region is paid for at construction";
  EXPECT_EQ(M.totalFrames(), 2u);
}

TEST(Pages, CeilingFollowsAvailableMemory) {
  EXPECT_EQ(regionsThatFit(128ull << 30, 1ull << 30), 32u) << "a quarter of 128 GiB is 32 regions of 1 GiB";
  EXPECT_EQ(regionsThatFit(4ull << 30, 1ull << 30), 1u);
  EXPECT_EQ(regionsThatFit(0, 1ull << 30), 1u) << "an unreadable meminfo must still leave one region";
  EXPECT_EQ(regionsThatFit(1ull << 40, 1ull << 30), 64u) << "the ceiling is capped however much memory there is";
  EXPECT_EQ(regionsThatFit(128ull << 30, 0), 1u);
}

TEST(Pages, CeilingHonoursOverride) {
  const size_t Ceiling = Memory::get().regionCeiling();
  RecordProperty("RegionCeiling", static_cast<int>(Ceiling));

  const char *Set = std::getenv("RAIL_MEMORY_REGIONS");
  if (Set) {
    EXPECT_EQ(Ceiling, std::strtoul(Set, nullptr, 10)) << "RAIL_MEMORY_REGIONS must win over what the machine reports";
    return;
  }

  EXPECT_GE(Ceiling, 1u);
  EXPECT_LE(Ceiling, 64u);
  EXPECT_EQ(Memory::get().frameSize(), 1u << 20);
}

#ifdef RAIL_HAVE_RDMA

TEST(Pages, DeviceSharesOneDomain) {
  const auto Ports = activeRdmaPorts();
  if (Ports.empty()) GTEST_SKIP() << "no active rdma port on this host";

  auto First = RdmaDevice::open(Ports.front().Device);
  ASSERT_TRUE(First) << First.error().message();
  auto Second = RdmaDevice::open(Ports.front().Device);
  ASSERT_TRUE(Second) << Second.error().message();

  EXPECT_EQ((*First)->domain(), (*Second)->domain()) << "a second open must not allocate a second protection domain";
  EXPECT_EQ((*First)->context(), (*Second)->context());
  EXPECT_EQ((*First)->slot(), (*Second)->slot());
  EXPECT_EQ(First->get(), Second->get()) << "both callers should hold the same device";
}

TEST(Pages, DevicesGetDistinctSlots) {
  const auto Ports = activeRdmaPorts();
  if (Ports.empty()) GTEST_SKIP() << "no active rdma port on this host";

  std::vector<std::shared_ptr<RdmaDevice>> Held;
  std::vector<size_t> Slots;
  for (const auto &Port : Ports) {
    auto One = RdmaDevice::open(Port.Device);
    ASSERT_TRUE(One) << One.error().message();
    Held.push_back(*One);
    Slots.push_back((*One)->slot());
  }

  for (size_t I = 0; I < Held.size(); I++)
    for (size_t J = I + 1; J < Held.size(); J++)
      if (Held[I]->name() != Held[J]->name()) EXPECT_NE(Slots[I], Slots[J]) << "two devices shared slot " << Slots[I];
}

TEST(Pages, ReopenedDeviceKeepsSlot) {
  const auto Ports = activeRdmaPorts();
  if (Ports.empty()) GTEST_SKIP() << "no active rdma port on this host";

  size_t Was = 0;
  {
    auto One = RdmaDevice::open(Ports.front().Device);
    ASSERT_TRUE(One) << One.error().message();
    Was = (*One)->slot();
  }

  auto Again = RdmaDevice::open(Ports.front().Device);
  ASSERT_TRUE(Again) << Again.error().message();
  EXPECT_EQ((*Again)->slot(), Was) << "a slot outlives its device, or a live registration table would point at the wrong keys";
}

TEST(Pages, AttachRegistersEveryRegion) {
  const auto Ports = activeRdmaPorts();
  if (Ports.empty()) GTEST_SKIP() << "no active rdma port on this host";

  auto Dev = RdmaDevice::open(Ports.front().Device);
  ASSERT_TRUE(Dev) << Dev.error().message();
  const size_t Slot = (*Dev)->slot();

  Memory M(2, kFrame * 2, kFrame);
  ASSERT_TRUE(M.attach(*Dev)) << "attach must register the region that already exists";

  rail::Run First = M.alloc(kFrame);
  ASSERT_TRUE(First.valid());
  const Page Borrowed = First.borrow(0, kFrame);
  EXPECT_NE(M.rkeyOf(Borrowed, Slot), 0u);
  EXPECT_NE(M.lkeyOf(Borrowed, Slot), 0u);

  rail::Run Second = M.alloc(kFrame);
  rail::Run Grown = M.alloc(2 * kFrame);
  ASSERT_TRUE(Grown.valid()) << "this run comes from a region grown after attach";
  const Page Later = Grown.borrow(0, kFrame);
  EXPECT_NE(M.rkeyOf(Later, Slot), 0u) << "a region grown after attach must be registered too";
}

// A region built while a second device attaches must still get its key. The
// 32 MiB registration is a milliseconds-wide window; the attacher starts
// 0.2 ms in, so it lands inside. A miss would read as a pass, never a failure.
TEST(Pages, ARegionBuiltDuringAttachGetsItsKeys) {
  const auto Ports = activeRdmaPorts();
  std::vector<std::string> Names;
  for (const auto &P : Ports)
    if (std::find(Names.begin(), Names.end(), P.Device) == Names.end()) Names.push_back(P.Device);
  if (Names.size() < 2) GTEST_SKIP() << "needs two rdma devices";

  auto First = RdmaDevice::open(Names[0]);
  ASSERT_TRUE(First) << First.error().message();
  auto Second = RdmaDevice::open(Names[1]);
  ASSERT_TRUE(Second) << Second.error().message();
  ASSERT_NE((*First)->slot(), (*Second)->slot());
  const size_t Slot = (*Second)->slot();

  constexpr size_t kRegion = 32 * kFrame;
  for (int Round = 0; Round < 10; Round++) {
    Memory M(4, kRegion, kFrame);
    ASSERT_TRUE(M.attach(*First));
    rail::Run Filled = M.alloc(kRegion);
    ASSERT_TRUE(Filled.valid()) << "the constructor's region should hold a whole run";

    std::barrier Go(2);
    rail::Run Grown;
    Result<void> Attached;
    std::thread Grower([&] {
      Go.arrive_and_wait();
      Grown = M.alloc(kFrame);
    });
    std::thread Attacher([&] {
      Go.arrive_and_wait();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      Attached = M.attach(*Second);
    });
    Grower.join();
    Attacher.join();

    ASSERT_TRUE(Attached) << Attached.error().message();
    ASSERT_TRUE(Grown.valid()) << "the frame must come from a grown region";
    const Page Later = Grown.borrow(0, kFrame);
    EXPECT_NE(M.rkeyOf(Later, Slot), 0u) << "round " << Round << ": a region built while the device attached has no key for it";
  }
}

TEST(Pages, UnregisteredMemoryHasNoKeys) {
  Memory M(1, kFrame * 2, kFrame);
  rail::Run R = M.alloc(kFrame);
  ASSERT_TRUE(R.valid());

  const Page Borrowed = R.borrow(0, kFrame);
  EXPECT_EQ(M.rkeyOf(Borrowed, 0), 0u) << "nothing was attached, so there is no key to report";
  EXPECT_EQ(M.lkeyOf(Borrowed, 0), 0u);
  EXPECT_EQ(M.rkeyOf(Borrowed, 99), 0u) << "a slot past the table must not read out of bounds";
  EXPECT_EQ(M.rkeyOf(Page{}, 0), 0u) << "a page with no region has no key";
}

TEST(Pages, MissingDeviceFails) {
  auto Missing = RdmaDevice::open("no-such-device");
  EXPECT_FALSE(Missing);
}

#endif

TEST(Pages, SessionCeilingRespectsMemory) {
  ServiceOptions Fat;
  Fat.PageCount = 64;
  Fat.PageSize = 1ull << 30;
  EXPECT_EQ(sessionsAffordable(Fat), 1u) << "64 GiB of pages per session cannot serve two sessions";

  ServiceOptions Normal;
  const size_t Held = sessionsAffordable(Normal);
  const size_t Backed = Memory::get().capacity() / (Normal.PageCount * Normal.PageSize);
  EXPECT_LE(Held, Backed) << "the daemon advertised " << Held << " sessions but memory backs only " << Backed;
  EXPECT_GE(Held, 1u);
}

TEST(Pages, AddressSpaceMapsOffsets) {
  Memory M(1, 32u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, 4 * kFrame, 4);
  ASSERT_TRUE(Space.registered());
  EXPECT_EQ(Space.capacity(), 16 * kFrame);
  EXPECT_EQ(Space.pageSize(), 4 * kFrame);

  Space.rebase(0);
  const auto First = Space.at(0, 4 * kFrame);
  ASSERT_NE(First.Where, nullptr);
  EXPECT_EQ(First.Offset, 0u);
  EXPECT_EQ(First.Length, 4 * kFrame);

  const auto Second = Space.at(4 * kFrame, 4 * kFrame);
  ASSERT_NE(Second.Where, nullptr);
  EXPECT_NE(Second.Where, First.Where) << "consecutive pages are distinct objects";
  EXPECT_NE(Second.Where->bytes(), First.Where->bytes());
}

TEST(Pages, AddressSpaceClipsToOnePage) {
  Memory M(1, 32u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, 4 * kFrame, 4);
  ASSERT_TRUE(Space.registered());
  Space.rebase(0);

  const uint64_t Straddle = 4 * kFrame - 4096;
  const auto At = Space.at(Straddle, 4 * kFrame);
  ASSERT_NE(At.Where, nullptr);
  EXPECT_EQ(At.Offset, 4 * kFrame - 4096);
  EXPECT_EQ(At.Length, 4096u) << "a lookup must stop at the page edge, never span two";
}

TEST(Pages, AddressSpaceHonoursBase) {
  Memory M(1, 32u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, kFrame, 2);
  ASSERT_TRUE(Space.registered());
  Space.rebase(1ull << 30);

  EXPECT_EQ(Space.at(0, 4096).Where, nullptr) << "an offset before the base is not mapped";
  EXPECT_EQ(Space.at((1ull << 30) + 2 * kFrame, 4096).Where, nullptr) << "an offset past the end is not mapped";

  const auto At = Space.at((1ull << 30) + 4096, 4096);
  ASSERT_NE(At.Where, nullptr);
  EXPECT_EQ(At.Offset, 4096u);
}

TEST(Pages, FailedClaimFallsBackToHeap) {
  Memory M(1, 4u << 20, kFrame);
  const size_t Whole = M.freeFrames();

  AddressSpace Space;
  Space.claim(M, 2 * kFrame, 4);
  EXPECT_FALSE(Space.registered()) << "eight frames cannot come from a four-frame memory";
  EXPECT_TRUE(Space.backed()) << "it must still serve the file, on the heap";
  EXPECT_EQ(M.freeFrames(), Whole) << "a partial claim must be rolled back";
  EXPECT_EQ(Space.capacity(), 8 * kFrame);

  Space.rebase(0);
  const auto At = Space.at(0, 2 * kFrame);
  ASSERT_NE(At.Where, nullptr);
  EXPECT_EQ(At.Where->region(), nullptr) << "a heap page has no region, so it is never advertised";

  AddressSpaceSink Sink(Space);
  EXPECT_EQ(Sink.landing(0, 2 * kFrame), nullptr) << "a heap-backed space must stay on the pooled path";
}

TEST(Pages, ReleaseFreesEveryPage) {
  Memory M(1, 32u << 20, kFrame);
  const size_t Whole = M.freeFrames();

  AddressSpace Space;
  Space.claim(M, 4 * kFrame, 4);
  ASSERT_TRUE(Space.registered());
  EXPECT_EQ(M.freeFrames(), Whole - 16);

  Space.release();
  EXPECT_EQ(M.freeFrames(), Whole);
  EXPECT_FALSE(Space.backed());
  EXPECT_EQ(Space.at(0, 4096).Where, nullptr);
}

TEST(Pages, SinkOffersWholePagesOnly) {
  Memory M(1, 32u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, 4 * kFrame, 4);
  ASSERT_TRUE(Space.registered());
  Space.rebase(0);

  AddressSpaceSink Sink(Space);
  EXPECT_NE(Sink.landing(0, 4 * kFrame), nullptr) << "a whole page from its start lands directly";
  EXPECT_NE(Sink.landing(4 * kFrame, 4 * kFrame), nullptr);

  EXPECT_EQ(Sink.landing(4096, 4 * kFrame), nullptr) << "an offset inside a page cannot take a whole page";
  EXPECT_EQ(Sink.landing(0, 4 * kFrame + 1), nullptr) << "a transfer larger than a page must not land directly";
  EXPECT_EQ(Sink.landing(16 * kFrame, 4 * kFrame), nullptr) << "past the end of the space";
}

TEST(Pages, HeapSinkOffersNothing) {
  std::vector<std::byte> Heap(4096);
  BufferSink Fallback(std::span<std::byte>(Heap), 0);
  EXPECT_EQ(Fallback.landing(0, 4096), nullptr) << "the heap path must stay on the pooled route";
}

TEST(Pages, SourceOffersWholePagesOnly) {
  Memory M(1, 32u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, 4 * kFrame, 4);
  ASSERT_TRUE(Space.registered());
  Space.rebase(0);

  AddressSpaceSource Source(Space);
  EXPECT_NE(Source.sending(0, 4 * kFrame), nullptr);
  EXPECT_NE(Source.sending(4 * kFrame, 4 * kFrame), nullptr);

  EXPECT_EQ(Source.sending(4096, 4 * kFrame), nullptr) << "an offset inside a page cannot send a whole page";
  EXPECT_EQ(Source.sending(0, 4 * kFrame + 1), nullptr) << "a transfer larger than a page must not send directly";
  EXPECT_EQ(Source.sending(16 * kFrame, 4 * kFrame), nullptr) << "past the end of the space";
}

TEST(Pages, HeapSourceOffersNothing) {
  Memory M(1, 4u << 20, kFrame);
  AddressSpace Space;
  Space.claim(M, 2 * kFrame, 4);
  ASSERT_FALSE(Space.registered());
  Space.rebase(0);

  AddressSpaceSource Source(Space);
  EXPECT_EQ(Source.sending(0, 2 * kFrame), nullptr) << "a heap-backed space must stay on the pooled path";
}
