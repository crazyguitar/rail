#include "harness.h"

#include <format>
#include <gtest/gtest.h>

namespace rail::e2e {

// The tool exists to move files larger than RAM, so nothing on the transfer
// path may scale with file size.
//
// This compares peak RSS for a small and a large push rather than asserting an
// absolute ceiling. A backend brings its own fixed footprint once its rails
// are up, and that has nothing to do with whether rail buffers the file.
// Comparing two sizes cancels it out and measures the property that matters.
class Memory : public ::testing::TestWithParam<std::string> {
protected:
  static constexpr uint64_t kSmall = 4u << 20;
  static constexpr uint64_t kLarge = 256u << 20;

  // Generous: the pool is 8 MiB and page cache is not counted in RSS, so real
  // growth from buffering the file would be orders of magnitude past this.
  static constexpr uint64_t kAllowedGrowthKb = 32u << 10;

  // The pool is pinned rather than derived. By default it grows with the
  // transfer, up to a cap, which is a deliberate memory-for-bandwidth trade;
  // what must never happen is buffering proportional to the file, and pinning
  // the geometry is what isolates that.
  PushOptions smallPool() const { return {GetParam(), {"-W", "--pages=8", "--page-size=1M"}}; }

  // Verifying by size, not digest. sftp reads run at a few MB/s, so streaming
  // 256 MiB back costs over two minutes and dominates the suite. Content is
  // already covered: rail verifies its own whole-file hash on the receiver
  // and fails the push on a mismatch, and the Basic and Delta suites compare
  // digests directly on smaller files.
  uint64_t pushAndMeasure(const std::string &Name, uint64_t Size, uint32_t Seed) {
    const auto Local = makeFile(Name, Size, Seed);
    const auto Remote = remotePath(Name);
    EXPECT_TRUE(peer().removeFile(Remote));

    const uint64_t PeakKb = pushMeasuringPeakRss(Local, Remote, smallPool());
    EXPECT_EQ(peer().stat(Remote).transform([](const RemoteStat &S) { return S.Size; }).value_or(0), Size);
    return PeakKb;
  }
};

TEST_P(Memory, PeakRssDoesNotScaleWithFileSize) {
  const uint64_t SmallKb = pushAndMeasure("mem-small.bin", kSmall, 33);
  const uint64_t LargeKb = pushAndMeasure("mem-large.bin", kLarge, 34);

  RecordProperty("SmallPeakKb", static_cast<int>(SmallKb));
  RecordProperty("LargePeakKb", static_cast<int>(LargeKb));

  const uint64_t GrowthKb = LargeKb > SmallKb ? LargeKb - SmallKb : 0;
  EXPECT_LT(GrowthKb, kAllowedGrowthKb) << std::format("peak RSS grew {} KiB going from a {} MiB file to a {} MiB one ({} -> {} KiB)",
                                                       GrowthKb,
                                                       kSmall >> 20,
                                                       kLarge >> 20,
                                                       SmallKb,
                                                       LargeKb);
}

INSTANTIATE_TEST_SUITE_P(Backends, Memory, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
