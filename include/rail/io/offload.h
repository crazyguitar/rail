#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/io/stream.h"
#include "rail/result.h"

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

// Reuse completion descriptors on their owning loop. Workers only hold a
// reference to the descriptor; registration and recycling stay on the loop.
class OffLoopSignals {
public:
  struct Signal {
    int Fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ~Signal() {
      if (Fd >= 0) ::close(Fd);
    }
  };

  static OffLoopSignals &get() {
    (void)Loop::get(); // Destroy registered signals before their loop.
    thread_local OffLoopSignals Cache;
    return Cache;
  }

  ~OffLoopSignals() {
    for (const auto &S : Free) Loop::get().forget(S->Fd);
  }

  std::shared_ptr<Signal> take() {
    if (!Free.empty()) {
      auto S = std::move(Free.back());
      Free.pop_back();
      return S;
    }
    auto S = std::make_shared<Signal>();
    if (S->Fd >= 0) Loop::get().share(S->Fd, EPOLLIN);
    return S;
  }

  void put(std::shared_ptr<Signal> S) {
    if (Free.size() < 16) {
      Loop::get().idle(S->Fd);
      Free.push_back(std::move(S));
    } else Loop::get().forget(S->Fd);
  }

private:
  std::vector<std::shared_ptr<Signal>> Free;
};

template <class Fn> Coro<std::invoke_result_t<Fn>> offLoop(Fn Work) {
  using R = std::invoke_result_t<Fn>;

  // Shared with the job, so a coroutine destroyed early still leaves the job
  // somewhere to land, and the eventfd closes only with the last reference.
  struct Slot {
    std::optional<R> Out;
    std::atomic<bool> Landed{false};
    std::shared_ptr<OffLoopSignals::Signal> Signal;
  };
  auto State = std::make_shared<Slot>();
  State->Signal = OffLoopSignals::get().take();
  if (State->Signal->Fd < 0) {
    // Fallible jobs must not turn descriptor exhaustion into a blocking call
    // on the serving loop (for example, truncate waiting for a file lease).
    if constexpr (std::is_constructible_v<R, std::unexpected<Error>>) co_return failErrno("eventfd");
    else co_return Work();
  }

  struct Unwatch {
    std::shared_ptr<OffLoopSignals::Signal> Signal;
    bool Reuse = false;
    ~Unwatch() {
      if (Reuse) OffLoopSignals::get().put(std::move(Signal));
      else Loop::get().forget(Signal->Fd);
    }
  } Guard{State->Signal};

  OffLoopPool::get().post([State, Work = std::move(Work)]() mutable {
    State->Out.emplace(Work());
    State->Landed.store(true, std::memory_order_release);
    const uint64_t One = 1;
    ssize_t Wrote;
    do {
      Wrote = ::write(State->Signal->Fd, &One, sizeof(One));
    } while (Wrote < 0 && errno == EINTR);
  });

  while (!State->Landed.load(std::memory_order_acquire)) co_await WaitFor{State->Signal->Fd, EPOLLIN};
  uint64_t Count;
  ssize_t Read;
  do {
    Read = ::read(State->Signal->Fd, &Count, sizeof(Count));
  } while (Read < 0 && errno == EINTR);
  // A consumed notification proves the worker has written it. If completion
  // was observed before that write, leave this descriptor with the worker.
  Guard.Reuse = Read == sizeof(Count);
  co_return std::move(*State->Out);
}

} // namespace rail
