#pragma once

#include "rail/io/loop.h"

#include <algorithm>
#include <coroutine>
#include <deque>

namespace rail {

// One coroutine at a time, in arrival order: take() waits for the turn, give()
// hands it to the next waiter or frees it when nobody waits.
class Turn {
public:
  struct Awaiter {
    Turn *G;
    std::coroutine_handle<> H{};
    bool Queued = false;
    bool Handed = false;

    explicit Awaiter(Turn *Owner) : G(Owner) {}
    Awaiter(const Awaiter &) = delete;
    Awaiter &operator=(const Awaiter &) = delete;

    bool await_ready() const noexcept { return !G->Held; }

    void await_suspend(std::coroutine_handle<> Handle) {
      H = Handle;
      Queued = true;
      G->Waiting.push_back(this);
    }

    void await_resume() noexcept {
      Queued = false;
      Handed = false;
      G->Held = true;
    }

    // Destroyed while queued: leave the queue. Destroyed after give() handed
    // it the turn but before it resumed: pass the turn on, or it is lost and
    // every later take() waits forever.
    ~Awaiter() {
      if (Queued) std::erase(G->Waiting, this);
      else if (Handed) G->give();
    }
  };

  Awaiter take() { return Awaiter(this); }

  void give() {
    if (Waiting.empty()) {
      Held = false;
      return;
    }
    Awaiter *Next = Waiting.front();
    Waiting.pop_front();
    Next->Queued = false;
    Next->Handed = true;
    Loop::get().schedule(Next->H);
  }

private:
  bool Held = false;
  std::deque<Awaiter *> Waiting;
};

// Gives the turn back however the holder's coroutine ends, including one
// destroyed part way through.
struct Holding {
  Turn &G;

  explicit Holding(Turn &G) : G(G) {}
  Holding(const Holding &) = delete;
  Holding &operator=(const Holding &) = delete;
  ~Holding() { G.give(); }
};

} // namespace rail
