#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"

#include <stdexcept>

namespace rail {

// Drives a coroutine to completion on this thread's event loop.
template <typename T> T run(Coro<T> C) {
  auto H = C.handle();
  H.resume();
  Loop::get().runUntil(H);
  if (!H.done()) throw std::runtime_error("the event loop ran out of work with the coroutine still waiting");
  return C.result();
}

} // namespace rail
