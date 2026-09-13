#pragma once

#include "rail/io/loop.h"
#include "rail/io/stream.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace rail {

// Connections handed to serving threads in turn rather than by SO_REUSEPORT,
// whose hash put five of a mount's eight connections on one thread.
class Inbox {
public:
  Inbox() : Wake(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  Inbox(const Inbox &) = delete;
  Inbox &operator=(const Inbox &) = delete;
  ~Inbox() {
    if (Wake >= 0) ::close(Wake);
  }

  bool usable() const { return Wake >= 0; }
  int wakeFd() const { return Wake; }
  bool stopped() const { return Stopping.load(); }

  void post(Stream Conn) {
    {
      const std::lock_guard<std::mutex> Held(Lock);
      Handed.push_back(std::move(Conn));
    }
    ring();
  }

  void stop() {
    Stopping.store(true);
    ring();
  }

  std::deque<Stream> take() {
    const std::lock_guard<std::mutex> Held(Lock);
    return std::exchange(Handed, {});
  }

  // Clears the ring so a stale one cannot spin the loop.
  void drain() {
    uint64_t Ticks = 0;
    while (::read(Wake, &Ticks, sizeof(Ticks)) == static_cast<ssize_t>(sizeof(Ticks))) {}
  }

  Coro<void> wakeup() {
    co_await WaitFor{Wake, EPOLLIN};
    drain();
  }

private:
  void ring() {
    const uint64_t One = 1;
    [[maybe_unused]] auto Wrote = ::write(Wake, &One, sizeof(One));
  }

  int Wake;
  std::atomic<bool> Stopping{false};
  std::mutex Lock;
  std::deque<Stream> Handed;
};

// Takes the inbox's descriptor out of this thread's loop when the serving
// coroutine ends, however it ends.
struct Attending {
  Inbox &In;

  explicit Attending(Inbox &In) : In(In) {}
  Attending(const Attending &) = delete;
  Attending &operator=(const Attending &) = delete;
  ~Attending() { Loop::get().forget(In.wakeFd()); }
};

} // namespace rail
