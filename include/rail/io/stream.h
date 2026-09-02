#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/result.h"

#include <cstddef>
#include <span>
#include <sys/epoll.h>

namespace rail {

// Suspends until Fd is ready for Events. TimeoutMs > 0 also resumes after that
// long with no event, which is what lets a caller enforce its own deadline
// when the peer can go silent.
// Whether the far end of a connection has gone. A transport waiting for the
// peer to act has to ask, or it waits for something that is never coming.
bool peerGone(int Fd);

struct WaitFor {
  int Fd;
  uint32_t Events;
  int TimeoutMs = 0;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> H) const {
    if (TimeoutMs > 0) Loop::get().waitFor(Fd, Events, TimeoutMs, H);
    else Loop::get().wait(Fd, Events, H);
  }

  void await_resume() const noexcept {}
};

// Non-blocking I/O over any fd: socket, pipe, or the ssh stdio pair. Unlike
// the libefaxx original this registers for EPOLLOUT and suspends on a full
// write buffer instead of returning a partial count and letting callers spin.
class Stream {
public:
  Stream() = default;
  explicit Stream(int Fd);
  Stream(const Stream &) = delete;
  Stream &operator=(const Stream &) = delete;
  Stream(Stream &&Other) noexcept;
  Stream &operator=(Stream &&Other) noexcept;
  ~Stream();

  static Result<Stream> connect(const std::string &Host, uint16_t Port);
  // Shared, when several threads each want their own listener on the one port:
  // the kernel then hands each incoming connection to exactly one of them, so
  // no accept has to be serialised in user space.
  static Result<Stream> listenOn(uint16_t Port, bool LoopbackOnly = false, bool Shared = false);

  Coro<Result<size_t>> read(std::span<std::byte> Dst);
  Coro<Result<void>> readExact(std::span<std::byte> Dst);
  Coro<Result<void>> writeAll(std::span<const std::byte> Src);

  // Both parts in one call, so a header and the payload behind it leave as one
  // packet. Sending them separately costs a round trip per transfer on a
  // TCP_NODELAY socket, which is most of a small read's time.
  Coro<Result<void>> writeAll(std::span<const std::byte> First, std::span<const std::byte> Second);
  Coro<Result<Stream>> accept();

  // One attempt, without suspending. An invalid Stream means nothing was
  // waiting, which lets a caller watch something else while it waits.
  Result<Stream> tryAccept();

  int fd() const noexcept { return Fd; }
  bool valid() const noexcept { return Fd >= 0; }
  void close();

  // Local port of a listening socket, for announcing an ephemeral bind.
  Result<uint16_t> localPort() const;

private:
  int Fd = -1;
};

Result<void> setNonBlocking(int Fd);

} // namespace rail
