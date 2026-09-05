#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/io/stream.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/eventfd.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rail {

// Persistent workers for calls that would block the loop: a handoff, not a
// thread, per call. Never destroyed, like Memory.
class OffLoopPool {
public:
  static OffLoopPool &get() {
    static OffLoopPool *Only = new OffLoopPool(std::max<size_t>(2, std::min<size_t>(8, std::thread::hardware_concurrency())));
    return *Only;
  }

  void post(std::move_only_function<void()> Job) {
    {
      const std::lock_guard<std::mutex> Held(Lock);
      Queue.push_back(std::move(Job));
    }
    Ready.notify_one();
  }

  size_t workers() const { return Threads.size(); }

private:
  explicit OffLoopPool(size_t Workers) {
    for (size_t I = 0; I < Workers; I++) Threads.emplace_back([this] { serve(); });
  }

  void serve() {
    for (;;) {
      std::move_only_function<void()> Job;
      {
        std::unique_lock<std::mutex> Held(Lock);
        Ready.wait(Held, [this] { return !Queue.empty(); });
        Job = std::move(Queue.front());
        Queue.pop_front();
      }
      Job();
    }
  }

  std::mutex Lock;
  std::condition_variable Ready;
  std::deque<std::move_only_function<void()>> Queue;
  std::vector<std::thread> Threads;
};

template <class Fn> Coro<std::invoke_result_t<Fn>> offLoop(Fn Work) {
  using R = std::invoke_result_t<Fn>;

  // Shared with the job, so a coroutine destroyed early still leaves the job
  // somewhere to land, and the eventfd closes only with the last reference.
  struct Slot {
    std::optional<R> Out;
    std::atomic<bool> Landed{false};
    int Done = -1;
    ~Slot() {
      if (Done >= 0) ::close(Done);
    }
  };
  auto State = std::make_shared<Slot>();
  State->Done = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (State->Done < 0) co_return Work();

  struct Unwatch {
    int Fd;
    ~Unwatch() { Loop::get().forget(Fd); }
  } Guard{State->Done};

  OffLoopPool::get().post([State, Work = std::move(Work)]() mutable {
    State->Out.emplace(Work());
    State->Landed.store(true, std::memory_order_release);
    const uint64_t One = 1;
    [[maybe_unused]] auto Wrote = ::write(State->Done, &One, sizeof(One));
  });

  while (!State->Landed.load(std::memory_order_acquire)) co_await WaitFor{State->Done, EPOLLIN};
  co_return std::move(*State->Out);
}

} // namespace rail
