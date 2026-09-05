#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/io/stream.h"
#include "rail/io/uring.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <unistd.h>
#include <utility>
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

// Bounds both the test and its cleanup. A hang terminates the test process;
// ordinary completion disarms the alarm before the next test starts.
struct Watchdog {
  Watchdog() { ::alarm(5); }
  ~Watchdog() { ::alarm(0); }
};

// Used only with a watchdog active. Closing the writer
// releases a pending pipe read even on an assertion exit; drain it before
// destroying the operation or its buffer. Destroy the waiter first so cleanup
// also exercises cancellation rather than resuming an abandoned coroutine.
struct PendingPipeRead {
  Uring U;
  int Pipes[2] = {-1, -1};
  std::byte Byte{};
  Uring::Read Op;
  Coro<Result<size_t>> Wait;
  bool Submitted = false;

  ~PendingPipeRead() {
    Wait = {};
    if (Pipes[1] >= 0) ::close(Pipes[1]);
    if (Submitted) drain();
    if (Pipes[0] >= 0) ::close(Pipes[0]);
  }

  void drain() {
    while (!Op.Done) {
      U.reap();
      if (!Op.Done) ::usleep(100);
    }
  }

  void start() {
    ASSERT_TRUE(U.usable());
    ASSERT_EQ(::pipe2(Pipes, O_CLOEXEC), 0);
    Op.Fd = Pipes[0];
    Op.Dst = {&Byte, 1};
    Op.Offset = static_cast<uint64_t>(-1);
    ASSERT_TRUE(U.submit(Op).has_value());
    Submitted = true;
    Wait = U.await(Op);
    Wait.start();
    ASSERT_FALSE(Wait.done());
  }
};

Coro<void> waitQuietly(int Fd, int Milliseconds) { co_await WaitFor{Fd, EPOLLIN, Milliseconds}; }

void cancelPipeRead(bool AfterReap) {
  PendingPipeRead P;
  ASSERT_NO_FATAL_FAILURE(P.start());
  if (AfterReap) {
    ASSERT_EQ(::write(P.Pipes[1], "x", 1), 1);
    P.drain();
    // reap() queued the owner, but the loop has not resumed it yet.
    ASSERT_FALSE(P.Wait.done());
  }

  P.Wait = {};
  ASSERT_FALSE(P.Op.Waiter);
  if (!AfterReap) {
    ASSERT_EQ(::write(P.Pipes[1], "x", 1), 1);
    P.drain();
  }
  EXPECT_EQ(P.Op.Result, 1);
  EXPECT_EQ(P.Byte, std::byte{'x'});

  // Flush the loop's ready queue before allocating another uring waiter: a
  // freed coroutine address must not be hidden by allocator reuse.
  auto Quiet = waitQuietly(P.Pipes[0], 20);
  Quiet.start();
  Loop::get().runUntil(Quiet.handle());
  ASSERT_TRUE(Quiet.done());
  Quiet.result();
}

void checkIdleReaper() {
  PendingPipeRead P;
  ASSERT_NO_FATAL_FAILURE(P.start());
  ASSERT_EQ(::write(P.Pipes[1], "x", 1), 1);
  Loop::get().runUntil(P.Wait.handle());
  ASSERT_TRUE(P.Wait.done());
  ASSERT_TRUE(P.Wait.result().has_value());

  const size_t Before = P.U.reaps();
  auto Quiet = waitQuietly(P.Pipes[0], 1100);
  Quiet.start();
  Loop::get().runUntil(Quiet.handle());
  ASSERT_TRUE(Quiet.done());
  Quiet.result();
  EXPECT_EQ(P.U.reaps(), Before) << "an idle ring kept draining with no outstanding operations";
}

void checkReaperRestarts(bool Cancel) {
  PendingPipeRead P;
  ASSERT_NO_FATAL_FAILURE(P.start());
  if (Cancel) {
    // Cancel only the waiter. The pending read and its buffer stay alive.
    P.Wait = {};
  } else {
    ASSERT_EQ(::write(P.Pipes[1], "x", 1), 1);
    Loop::get().runUntil(P.Wait.handle());
    ASSERT_TRUE(P.Wait.done());
    ASSERT_TRUE(P.Wait.result().has_value());
  }

  // Drive the loop beyond the completion tick so the old reaper exits before
  // another await. Without this gap the test only exercises a running reaper.
  const size_t Before = P.U.reaps();
  auto Quiet = waitQuietly(P.Pipes[0], 1100);
  Quiet.start();
  Loop::get().runUntil(Quiet.handle());
  ASSERT_TRUE(Quiet.done());
  Quiet.result();
  ASSERT_EQ(P.U.reaps(), Before);

  // After cancellation, await the original read; after completion, submit a
  // fresh one. Both must wake through the restarted reaper without manual reap.
  if (!Cancel) ASSERT_TRUE(P.U.submit(P.Op).has_value());
  P.Wait = P.U.await(P.Op);
  P.Wait.start();
  ASSERT_FALSE(P.Wait.done());
  ASSERT_EQ(::write(P.Pipes[1], "y", 1), 1);
  Loop::get().runUntil(P.Wait.handle());
  ASSERT_TRUE(P.Wait.done());
  auto Got = P.Wait.result();
  ASSERT_TRUE(Got.has_value());
  EXPECT_EQ(*Got, 1u);
  EXPECT_EQ(P.Byte, std::byte{'y'});
}

} // namespace

TEST(Uring, CancellingBeforeCompletionDoesNotResumeADeadCoroutine) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  cancelPipeRead(false);
}

TEST(Uring, CancellingAQueuedCompletionDoesNotResumeADeadCoroutine) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  cancelPipeRead(true);
}

TEST(Uring, AnIdleRingDoesNotKeepReaping) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  checkIdleReaper();
}

TEST(Uring, ReaperRestartsAfterCompletion) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  checkReaperRestarts(false);
}

TEST(Uring, ReaperRestartsAfterCancellation) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  checkReaperRestarts(true);
}

// reap() must resume the owner of what it drained. Submit a read, drain it by
// hand, then drive the loop: the owner has to finish, not sleep on a wake that
// already fired.
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

// One completion must wake its owner and nobody else. Reads on empty pipes
// stay pending until their writer writes, so completing one of many shows how
// many times the ring was drained for it: once, not once per waiter.
namespace {

void checkCompletionWakesOnlyItsOwner() {
  Uring U;
  ASSERT_TRUE(U.usable());

  constexpr size_t kReaders = 32;
  struct Pipe {
    int Fds[2] = {-1, -1};
    ~Pipe() {
      for (int Fd : Fds)
        if (Fd >= 0) ::close(Fd);
    }
  } Pipes[kReaders];
  for (auto &P : Pipes) ASSERT_EQ(::pipe2(P.Fds, O_CLOEXEC), 0);

  std::vector<std::byte> Bytes(kReaders);
  std::vector<Uring::Read> Ops(kReaders);
  std::vector<Coro<Result<size_t>>> Waits;

  // Declared last so it runs first: every read has to finish before the buffers
  // it points into go away, or a completion landing after a failed assertion
  // returns would write into memory the test no longer owns.
  struct FinishAll {
    Pipe *Pipes;
    size_t Count;
    std::vector<Coro<Result<size_t>>> &Waits;
    std::vector<Uring::Read> &Ops;
    Uring &U;
    size_t Submitted = 0;

    ~FinishAll() {
      Waits.clear();
      // EOF releases every pending read. Reap directly so a broken owner
      // wakeup cannot hang cleanup; the watchdog bounds kernel stalls.
      for (size_t I = 0; I < Count; I++) ::close(std::exchange(Pipes[I].Fds[1], -1));
      for (size_t I = 0; I < Submitted; I++)
        while (!Ops[I].Done) {
          U.reap();
          if (!Ops[I].Done) ::usleep(100);
        }
    }
  } Finish{Pipes, kReaders, Waits, Ops, U};

  for (size_t I = 0; I < kReaders; I++) {
    Ops[I].Fd = Pipes[I].Fds[0];
    Ops[I].Dst = std::span<std::byte>(&Bytes[I], 1);
    Ops[I].Offset = static_cast<uint64_t>(-1);
    ASSERT_TRUE(U.submit(Ops[I]).has_value());
    // Track the kernel operation even if allocating its coroutine throws.
    Finish.Submitted++;
    Waits.push_back(U.await(Ops[I]));
    Waits.back().start();
  }

  const size_t Before = U.reaps();
  ASSERT_EQ(::write(Pipes[0].Fds[1], "x", 1), 1);
  Loop::get().runUntil(Waits[0].handle());
  ASSERT_TRUE(Waits[0].done());
  EXPECT_TRUE(Waits[0].result().has_value());

  const size_t Drains = U.reaps() - Before;
  EXPECT_LE(Drains, 2u) << "one completion drained the ring " << Drains << " times: every waiter was woken for it";
  for (size_t I = 1; I < kReaders; I++) EXPECT_FALSE(Waits[I].done()) << "reader " << I << " finished with nothing to read";
}

} // namespace

TEST(Uring, ACompletionWakesOnlyItsOwner) {
  if (!Uring::get().usable()) GTEST_SKIP() << "io_uring is unavailable here";
  Watchdog Guard;
  checkCompletionWakesOnlyItsOwner();
}
