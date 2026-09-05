#pragma once

#include "rail/io/coro.h"
#include "rail/result.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <span>

struct io_uring;

namespace rail {

// Asynchronous file writes for the event loop thread. Submitting is immediate
// and only the completion is awaited, so the loop can post the next receive
// while earlier pages are still on their way to disk. That decoupling is the
// point: a blocking write leaves the fabric with nowhere to put data.
class Uring {
public:
  static Uring &get();

  Uring();
  Uring(const Uring &) = delete;
  Uring &operator=(const Uring &) = delete;
  ~Uring();

  // What every operation reports back. A submission carries a pointer to this,
  // so reaping fills it in without knowing which kind of operation it was.
  // Waiter holds the one coroutine awaiting it: an operation is awaited once.
  struct Completion {
    int Result = 0;
    bool Done = false;
    std::coroutine_handle<> Waiter{};
  };

  // One outstanding operation. The caller owns it and must keep both it and
  // the buffer alive until await() returns.
  struct Write : Completion {
    std::span<const std::byte> Src;
    uint64_t Offset = 0;
    int Fd = -1;
  };

  struct Read : Completion {
    std::span<std::byte> Dst;
    uint64_t Offset = 0;
    int Fd = -1;
  };

  struct Fsync : Completion {
    int Fd = -1;
  };

  bool usable() const { return EventFd >= 0; }

  // Operations the ring is sized for. Going far past this leaves the kernel
  // absorbing completion overflow, which works but is not worth relying on.
  static size_t depth();

  Result<void> submit(Write &W);
  Result<void> submit(Read &R);
  Result<void> submit(Fsync &F);

  // Resumes when W has fully landed, resubmitting whatever a short write left.
  Coro<Result<void>> await(Write &W);

  // Resumes with the bytes read; short only at end of file.
  Coro<Result<size_t>> await(Read &R);

  Coro<Result<void>> await(Fsync &F);

  // Takes every completion the ring has, for whoever submitted it, and wakes
  // the coroutines waiting on the ones it finished. Public because that second
  // half is a promise worth testing: an owner whose completion someone else
  // took must still be resumed.
  void reap();

  // How many times the ring has been drained, for tests: one per wake of the
  // reaper, so a batch of completions costs one, not one per op.
  size_t reaps() const { return Reaps; }

private:
  Result<void> post(Write &W);
  Result<void> post(Read &R);

  // Parks on the operation itself, not on the shared eventfd, so a drain
  // resumes only the owners of what it found. Clears the stored handle if the
  // coroutine is destroyed while parked, so a drain never resumes a dead one,
  // and counts itself so the reaper knows when it can stop.
  struct Landed {
    Uring *U;
    Completion &C;
    bool Parked = false;

    bool await_ready() const noexcept { return C.Done; }
    void await_suspend(std::coroutine_handle<> H) {
      C.Waiter = H;
      Parked = true;
      U->Outstanding++;
    }
    void await_resume() noexcept { unpark(); }
    ~Landed() { unpark(); }

  private:
    void unpark() noexcept {
      if (!Parked) return;
      Parked = false;
      C.Waiter = {};
      U->Outstanding--;
    }
  };

  // One coroutine per ring parks on the eventfd and drains for everyone. It
  // stops once nothing is parked on an operation, so an idle ring costs no
  // wakeups; the next await starts it again.
  Coro<void> reaper();
  void ensureReaper();

  io_uring *Ring = nullptr;
  int EventFd = -1;
  Coro<void> Reaper;
  size_t Outstanding = 0;
  size_t Reaps = 0;
};

} // namespace rail
