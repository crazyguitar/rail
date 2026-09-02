#pragma once

#include "rail/io/coro.h"
#include "rail/io/loop.h"

namespace rail {

// Drives a coroutine to completion on this thread's event loop.
template <typename T> T run(Coro<T> C) {
  auto H = C.handle();
  H.resume();
  Loop::get().runUntil(H);
  return C.result();
}

} // namespace rail
