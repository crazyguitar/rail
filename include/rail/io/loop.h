#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rail {

// Single-threaded epoll event loop. Milestone 2 adds a pool of these; nothing
// in the public surface changes when it does.
class Loop {
public:
  static Loop &get();

  Loop();
  Loop(const Loop &) = delete;
  Loop &operator=(const Loop &) = delete;
  ~Loop();

  // Drives ready coroutines, fd events and poll predicates until H completes.
  void runUntil(std::coroutine_handle<> H);

  void schedule(std::coroutine_handle<> H);

  // Registers Fd for a one-shot wakeup of H on Events.
  void wait(int Fd, uint32_t Events, std::coroutine_handle<> H);

  // As wait(), but also resumes H after TimeoutMs even with no event.
  void waitFor(int Fd, uint32_t Events, int TimeoutMs, std::coroutine_handle<> H);
  void forget(int Fd);

  void share(int Fd, uint32_t Events);

  void cancel(std::coroutine_handle<> H);

  void wake(int Fd);

  // Some transports only advance while something calls into them. Handing one
  // to the loop lets it advance every iteration, rather than only when a
  // coroutine happens to resume: a fabric that needs progress to produce the
  // very event that would wake us would otherwise wait for a timer.
  //
  // Returns true when the call found work to do.
  using ProgressFn = std::function<bool()>;

  // Holds a transport in the loop for as long as it lives. The loop calls into
  // the transport, so leaving one registered past its owner would call into
  // freed memory; ending the registration is therefore not left to the caller
  // to remember.
  class Driver {
  public:
    Driver() = default;
    Driver(const Driver &) = delete;
    Driver &operator=(const Driver &) = delete;
    Driver(Driver &&Other) noexcept;
    Driver &operator=(Driver &&Other) noexcept;
    ~Driver();

    // Ends it early, for an owner that tears down before it is destroyed.
    void stop();

  private:
    friend class Loop;

    explicit Driver(uint64_t Id) : Id(Id) {}

    uint64_t Id = 0;
  };

  [[nodiscard]] Driver drive(ProgressFn Fn);

private:
  // Runs one turn for Until, and does not block once it has finished: the
  // coroutine can complete while resuming, and waiting after that costs a
  // whole idle timeout per run().
  bool step(std::coroutine_handle<> Until);
  bool hasWork() const;

  int EpollFd = -1;
  struct Wake {
    std::coroutine_handle<> H;
    int Fd = -1;
  };
  std::deque<Wake> Ready;

  // Several coroutines can wait on one descriptor, and not for the same thing:
  // a connection is read by one and written by another. Each waiter carries its
  // own interest so the descriptor can be registered for all of them at once -
  // registering only the newest left a blocked write asleep until the peer
  // happened to send something.
  struct Parked {
    std::coroutine_handle<> H;
    uint32_t Events = 0;
  };
  std::unordered_map<int, std::vector<Parked>> Waiters;
  std::unordered_set<int> Shared;
  int Registered = 0;
  struct Timer {
    std::chrono::steady_clock::time_point When;
    int Fd;
    std::coroutine_handle<> H;
  };
  std::vector<Timer> Timed;

  friend class Driver;

  void undrive(uint64_t Id);

  std::vector<std::pair<uint64_t, ProgressFn>> Driven;
  uint64_t NextDriverId = 1;
  int Quiet = 0;
};

} // namespace rail
