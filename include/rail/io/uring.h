#pragma once

#include "rail/io/coro.h"
#include "rail/result.h"

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
  struct Completion {
    int Result = 0;
    bool Done = false;
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

  bool usable() const { return EventFd >= 0; }

  // Operations the ring is sized for. Going far past this leaves the kernel
  // absorbing completion overflow, which works but is not worth relying on.
  static size_t depth();

  Result<void> submit(Write &W);
  Result<void> submit(Read &R);

  // Resumes when W has fully landed, resubmitting whatever a short write left.
  Coro<Result<void>> await(Write &W);

  // Resumes with the bytes read; short only at end of file.
  Coro<Result<size_t>> await(Read &R);

  // Takes every completion the ring has, for whoever submitted it, and wakes
  // the coroutines waiting on the ones it finished. Public because that second
  // half is a promise worth testing: an owner whose completion someone else
  // took must still be resumed.
  void reap();

private:
  Result<void> post(Write &W);
  Result<void> post(Read &R);

  io_uring *Ring = nullptr;
  int EventFd = -1;
};

} // namespace rail
