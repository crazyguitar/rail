#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/result.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace rail {

// Drives a coroutine to completion on this thread's event loop.
template <typename T> T run(Coro<T> C) {
  auto H = C.handle();
  H.resume();
  Loop::get().runUntil(H);
  if (!H.done()) throw std::runtime_error("the event loop ran out of work with the coroutine still waiting");
  return C.result();
}

template <typename T> Result<T> runToResult(Coro<Result<T>> C) {
  try {
    return run(std::move(C));
  } catch (const std::exception &E) {
    return failMessage(E.what());
  }
}

} // namespace rail
