#include "rail/io/loop.h"
#include "rail/io/offload.h"
#include "rail/io/runner.h"
#include "rail/io/stream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <future>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rail;

namespace {

constexpr size_t kMoreThanFits = 16u << 20;
constexpr int kWatchdogMs = 2000;

Coro<Result<void>> fillTheSocket(Stream &Out, const std::vector<std::byte> &Bytes) { co_return co_await Out.writeAll(Bytes); }

Coro<Result<size_t>> readOne(Stream &In) {
  std::byte One{};
  co_return co_await In.read({&One, 1});
}

Coro<void> drain(Stream &In, size_t Bytes) {
  std::vector<std::byte> Sink(1u << 20);
  size_t Got = 0;
  while (Got < Bytes) {
    auto N = co_await In.read(Sink);
    if (!N || *N == 0) co_return;
    Got += *N;
  }
}

// On a descriptor of its own: a wake resumes every waiter parked on one, so a
// watchdog sharing the drain's socket would fire on the drain's first read.
Coro<void> giveUp(int Silent, Stream &Peer) {
  co_await WaitFor{Silent, EPOLLIN, kWatchdogMs};
  Peer.close();
}

struct Never {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};

Coro<int> parkedForever() {
  co_await Never{};
  co_return 1;
}

} // namespace

TEST(Loop, RunRefusesToReturnAnUnfinishedResult) {
  bool Threw = false;
  bool Returned = false;
  std::thread([&] {
    EXPECT_EQ(run(offLoop([] { return 7; })), 7);
    try {
      run(parkedForever());
      Returned = true;
    } catch (const std::runtime_error &) {
      Threw = true;
    }
  }).join();

  EXPECT_TRUE(Threw);
  EXPECT_FALSE(Returned) << "run() returned a value the coroutine never produced";
}

// A blocked job must not hold up the next: a quick one runs beside a sleeping
// one. The sleeper is joined, since run() would resume it as if its wait fired.
TEST(Loop, ABlockedOffLoopJobDoesNotHoldUpAnother) {
  ASSERT_GE(OffLoopPool::get().workers(), 2u);

  const auto Took = run([]() -> Coro<std::chrono::steady_clock::duration> {
    auto Slow = offLoop([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return 1;
    });
    Slow.start();

    const auto Started = std::chrono::steady_clock::now();
    const int Quick = co_await offLoop([] { return 2; });
    const auto Elapsed = std::chrono::steady_clock::now() - Started;
    EXPECT_EQ(Quick, 2);

    [[maybe_unused]] const int Late = co_await Slow.join();
    co_return Elapsed;
  }());

  EXPECT_LT(Took, std::chrono::milliseconds(200)) << "the quick job waited behind the blocked one";
}

TEST(Loop, OffLoopWorkComesBackToTheLoop) {
  const auto Loop = std::this_thread::get_id();
  std::thread::id Ran;
  const int Got = run(offLoop([&] {
    Ran = std::this_thread::get_id();
    return 42;
  }));
  EXPECT_EQ(Got, 42);
  EXPECT_NE(Ran, Loop) << "the work ran on the loop thread";
}

TEST(Loop, OffLoopResourceFailureDoesNotRunFallibleWorkInline) {
  std::thread([] {
    // Initialize the loop before limiting descriptors; restore the limit before
    // making assertions so the rest of the suite can still open files.
    (void)Loop::get();
    struct rlimit Before{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &Before), 0);
    auto Limited = Before;
    Limited.rlim_cur = 0;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &Limited), 0);
    bool Ran = false;
    auto Got = run(offLoop([&]() -> Result<int> {
      Ran = true;
      return 42;
    }));
    const int Restored = ::setrlimit(RLIMIT_NOFILE, &Before);
    ASSERT_EQ(Restored, 0);
    EXPECT_FALSE(Ran);
    ASSERT_FALSE(Got);
    EXPECT_EQ(Got.error().Code.value(), EMFILE);
  }).join();
}

TEST(Loop, OffLoopCompletionsRemainDistinctAfterCancellation) {
  std::promise<void> Started, Release;
  auto Released = Release.get_future().share();
  {
    auto Abandoned = offLoop([&Started, Released]() -> Result<std::string> {
      Started.set_value();
      Released.wait();
      return "abandoned";
    });
    Abandoned.start();
    Started.get_future().wait();
  }
  // The cancelled worker still owns its notification descriptor while other
  // jobs complete. Its eventual wake must not complete a different request.
  auto First = run(offLoop([]() -> Result<std::string> { return "first"; }));
  Release.set_value();
  ASSERT_TRUE(First);
  EXPECT_EQ(*First, "first");
  for (int I = 0; I < 100; ++I) {
    const auto Expected = std::to_string(I);
    auto Got = run(offLoop([Expected]() -> Result<std::string> { return Expected; }));
    ASSERT_TRUE(Got);
    EXPECT_EQ(*Got, Expected);
  }
}

// A connection is read by one coroutine and written by another. Registering the
// descriptor for only the newest waiter left the write asleep until the peer
// sent something of its own, which cost a mount 64 seconds.
TEST(Loop, ABlockedWriteWakesWhileAReadIsWaiting) {
  std::signal(SIGPIPE, SIG_IGN);

  int Pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, Pair), 0);

  Stream Near(Pair[0]);
  Stream Far(Pair[1]);
  const std::vector<std::byte> Bytes(kMoreThanFits, std::byte{7});

  auto Writing = fillTheSocket(Near, Bytes);
  Writing.start();

  auto Waiting = readOne(Near);
  Waiting.start();

  auto Draining = drain(Far, kMoreThanFits);
  Draining.start();

  int Silent[2] = {-1, -1};
  ASSERT_EQ(::pipe(Silent), 0);
  auto Watchdog = giveUp(Silent[0], Far);
  Watchdog.start();

  Loop::get().runUntil(Writing.handle());
  ASSERT_TRUE(Writing.done()) << "the write never woke: it is parked on a descriptor registered for someone else";
  auto Wrote = Writing.result();
  EXPECT_TRUE(Wrote.has_value()) << (Wrote ? "" : Wrote.error().message());

  ::close(Silent[0]);
  ::close(Silent[1]);
}
