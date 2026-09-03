#include "rail/io/uring.h"

#include "rail/io/loop.h"
#include "rail/io/stream.h" // WaitFor

#include <format>
#include <liburing.h>
#include <limits>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace rail {

namespace {

constexpr unsigned kQueueDepth = 64;

// A completion can land between reaping and re-arming, so waiting is bounded
// rather than indefinite. The eventfd counter makes that rare, not impossible.
constexpr int kCompletionTick = 250;

} // namespace

size_t Uring::depth() { return kQueueDepth; }

Uring &Uring::get() {
  thread_local Uring U;
  return U;
}

Uring::Uring() {
  Ring = new io_uring{};
  if (io_uring_queue_init(kQueueDepth, Ring, 0) < 0) {
    delete Ring;
    Ring = nullptr;
    return;
  }

  EventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (EventFd < 0 || io_uring_register_eventfd(Ring, EventFd) < 0) {
    if (EventFd >= 0) ::close(EventFd);
    EventFd = -1;
    io_uring_queue_exit(Ring);
    delete Ring;
    Ring = nullptr;
  }
}

Uring::~Uring() {
  if (EventFd >= 0) ::close(EventFd);
  if (Ring) {
    io_uring_queue_exit(Ring);
    delete Ring;
  }
}

Result<void> Uring::post(Write &W) {
  // prep_write takes a 32-bit length, so an oversized page would wrap.
  if (W.Src.size() > std::numeric_limits<unsigned>::max()) return failMessage("write is too large for one io_uring submission");

  io_uring_sqe *Sqe = io_uring_get_sqe(Ring);
  if (!Sqe) return failMessage("io_uring submission queue is full");

  io_uring_prep_write(Sqe, W.Fd, W.Src.data(), static_cast<unsigned>(W.Src.size()), W.Offset);
  io_uring_sqe_set_data(Sqe, static_cast<Completion *>(&W));

  W.Done = false;
  if (io_uring_submit(Ring) < 0) return failMessage("io_uring_submit failed");
  return {};
}

Result<void> Uring::submit(Write &W) {
  if (!usable()) return failMessage("io_uring is unavailable");
  return post(W);
}

Result<void> Uring::post(Read &R) {
  io_uring_sqe *Sqe = io_uring_get_sqe(Ring);
  if (!Sqe) return failMessage("io_uring submission queue is full");

  io_uring_prep_read(Sqe, R.Fd, R.Dst.data(), static_cast<unsigned>(R.Dst.size()), R.Offset);
  io_uring_sqe_set_data(Sqe, static_cast<Completion *>(&R));

  R.Done = false;
  if (io_uring_submit(Ring) < 0) return failMessage("io_uring_submit failed");
  return {};
}

Result<void> Uring::submit(Fsync &F) {
  if (!usable()) return failMessage("io_uring is unavailable");

  io_uring_sqe *Sqe = io_uring_get_sqe(Ring);
  if (!Sqe) return failMessage("io_uring submission queue is full");

  io_uring_prep_fsync(Sqe, F.Fd, 0);
  io_uring_sqe_set_data(Sqe, static_cast<Completion *>(&F));

  F.Done = false;
  if (io_uring_submit(Ring) < 0) return failMessage("io_uring_submit failed");
  return {};
}

Coro<Result<void>> Uring::await(Fsync &F) {
  while (!F.Done) {
    co_await WaitFor{EventFd, EPOLLIN, kCompletionTick};
    reap();
  }
  if (F.Result < 0) co_return fail(std::error_code(-F.Result, std::generic_category()), "io_uring fsync");
  co_return Result<void>{};
}

Result<void> Uring::submit(Read &R) {
  if (!usable()) return failMessage("io_uring is unavailable");
  if (R.Dst.size() > std::numeric_limits<unsigned>::max()) return failMessage("read is too large for one io_uring submission");
  return post(R);
}

// A short read means end of file, so it stops rather than asking again.
Coro<Result<size_t>> Uring::await(Read &R) {
  size_t Total = 0;
  for (;;) {
    while (!R.Done) {
      co_await WaitFor{EventFd, EPOLLIN, kCompletionTick};
      reap();
    }

    if (R.Result < 0) co_return fail(std::error_code(-R.Result, std::generic_category()), "io_uring read");

    Total += static_cast<size_t>(R.Result);
    if (R.Result == 0 || static_cast<size_t>(R.Result) == R.Dst.size()) co_return Total;

    R.Dst = R.Dst.subspan(static_cast<size_t>(R.Result));
    R.Offset += static_cast<uint64_t>(R.Result);
    if (auto S = post(R); !S) co_return std::unexpected(S.error());
  }
}

// One ring serves every read on the thread, so this drains completions that
// belong to other coroutines and takes the descriptor's count with them. They
// have to be woken here: a coroutine whose read finished in someone else's
// drain was left parked on an event already spent, and slept out its timeout
// with the bytes it asked for sitting ready.
void Uring::reap() {
  uint64_t Ticks = 0;
  [[maybe_unused]] auto Ignored = ::read(EventFd, &Ticks, sizeof(Ticks));

  bool Any = false;
  for (;;) {
    io_uring_cqe *Cqe = nullptr;
    if (io_uring_peek_cqe(Ring, &Cqe) != 0 || !Cqe) break;

    if (auto *Finished = static_cast<Completion *>(io_uring_cqe_get_data(Cqe))) {
      Finished->Result = Cqe->res;
      Finished->Done = true;
      Any = true;
    }
    io_uring_cqe_seen(Ring, Cqe);
  }

  if (Any) Loop::get().wake(EventFd);
}

Coro<Result<void>> Uring::await(Write &W) {
  for (;;) {
    while (!W.Done) {
      co_await WaitFor{EventFd, EPOLLIN, kCompletionTick};
      reap();
    }

    if (W.Result < 0) co_return fail(std::error_code(-W.Result, std::generic_category()), "io_uring write");
    if (W.Result == 0 && !W.Src.empty()) co_return failMessage("io_uring write made no progress");

    const auto Written = static_cast<size_t>(W.Result);
    if (Written == W.Src.size()) co_return Result<void>{};

    // A short write is legal; carry on from where it stopped.
    W.Src = W.Src.subspan(Written);
    W.Offset += Written;
    if (auto R = post(W); !R) co_return std::unexpected(R.error());
  }
}

} // namespace rail
