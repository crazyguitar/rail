#include "rail/io/loop.h"
#include "rail/io/uring.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <vector>

using namespace rail;

namespace {

constexpr size_t kBody = 4096;
constexpr int kDrainAttempts = 500;

std::filesystem::path aFileToRead() {
  const auto At = std::filesystem::temp_directory_path() / "rail-uring-test.bin";
  std::ofstream Out(At, std::ios::binary | std::ios::trunc);
  const std::vector<char> Body(kBody, '7');
  Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  return At;
}

} // namespace

// One ring serves every read on the thread, so a coroutine's completion is
// often taken by whoever drains next. Marking it done is not enough: the owner
// is parked on the descriptor whose count that drain just spent, and nothing
// else will ever signal it.
TEST(Uring, AReapWakesTheCoroutineWhoseReadItFinished) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";

  const auto At = aFileToRead();
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(Fd, 0);

  std::vector<std::byte> Into(kBody);
  Uring::Read R;
  R.Fd = Fd;
  R.Dst = Into;
  R.Offset = 0;
  ASSERT_TRUE(Uring::get().submit(R).has_value());

  auto Reading = Uring::get().await(R);
  Reading.start();
  ASSERT_FALSE(Reading.done()) << "the read finished before it could park";

  for (int I = 0; I < kDrainAttempts && !R.Done; I++) Uring::get().reap();
  ASSERT_TRUE(R.Done) << "the read never completed";

  const auto Began = std::chrono::steady_clock::now();
  Loop::get().runUntil(Reading.handle());
  const auto Waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Began).count();

  ::close(Fd);
  std::filesystem::remove(At);

  ASSERT_TRUE(Reading.done());
  EXPECT_TRUE(Reading.result().has_value());
  EXPECT_LT(Waited, 100) << "the owner slept " << Waited << "ms on an event another drain had already spent";
}
