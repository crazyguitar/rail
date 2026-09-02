#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>

namespace rail::e2e {

namespace {

// Large enough that connection setup and the first page do not dominate: at
// several GB/s a 1 GiB file is over in well under a second. RAIL_THROUGHPUT_BYTES
// overrides it for a quicker run.
uint64_t transferSize() {
  if (const char *Env = ::getenv("RAIL_THROUGHPUT_BYTES")) return std::strtoull(Env, nullptr, 10);
  return 8ull << 30;
}

} // namespace

class Throughput : public ::testing::TestWithParam<std::string> {};

TEST_P(Throughput, LargeFileOneDirection) {
  const uint64_t Size = transferSize();
  const auto Local = makeFile("throughput.bin", Size, 31);
  const auto Remote = remotePath("throughput.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, {GetParam(), {"-W"}});

  EXPECT_EQ(R.LiteralBytes, Size);

  // Only the size is checked here. Digesting the destination means streaming it
  // back over sftp at a few MB/s, which for a file this large takes far longer
  // than the transfer being measured; every other case verifies the bytes.
  EXPECT_EQ(peer().stat(Remote).transform([](const RemoteStat &S) { return S.Size; }).value_or(0), Size);

  const double Seconds = std::chrono::duration<double>(R.TransferTime).count();
  ASSERT_GT(Seconds, 0.0);

  const double MBps = double(Size) / 1e6 / Seconds;
  RecordProperty("MBps", static_cast<int>(MBps));
  RecordProperty("rails", R.Rails);
  std::printf("[ THROUGHPUT ] %-4s  this host -> %s  %.0f MB/s  rails=%s\n",
              GetParam().c_str(),
              peerHost().c_str(),
              MBps,
              R.Rails.empty() ? "-" : R.Rails.c_str());
}

INSTANTIATE_TEST_SUITE_P(Backends, Throughput, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
