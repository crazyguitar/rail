#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/io/stream.h"

#include <optional>
#include <sys/eventfd.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace rail {

template <class Fn> Coro<std::invoke_result_t<Fn>> offLoop(Fn Work) {
  struct Worker {
    std::thread Thread;
    int Done;

    ~Worker() {
      if (Thread.joinable()) Thread.join();
      if (Done < 0) return;
      Loop::get().forget(Done);
      ::close(Done);
    }
  };

  Worker W{{}, ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  if (W.Done < 0) co_return Work();

  std::optional<std::invoke_result_t<Fn>> Out;
  W.Thread = std::thread([&] {
    Out.emplace(Work());
    const uint64_t One = 1;
    [[maybe_unused]] auto Wrote = ::write(W.Done, &One, sizeof(One));
  });

  co_await WaitFor{W.Done, EPOLLIN};
  W.Thread.join();
  co_return std::move(*Out);
}

} // namespace rail
