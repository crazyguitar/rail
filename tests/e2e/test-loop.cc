#include "rail/io/loop.h"
#include "rail/io/stream.h"

#include <gtest/gtest.h>

#include <csignal>
#include <sys/socket.h>
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

} // namespace

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
