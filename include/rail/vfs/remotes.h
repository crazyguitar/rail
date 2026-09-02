#pragma once

#include "rail/address-space.h"
#include "rail/file-service.h"
#include "rail/io/coro.h"
#include "rail/io/loop.h"
#include "rail/result.h"

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rail::vfs {

struct RemoteOptions {
  std::string Host;
  std::string Root = ".";
  // Sessions to the daemon. A session now answers several requests at once, so
  // this is how much of the fabric to hold open rather than how much
  // concurrency is available.
  size_t Sessions = 4;
  // Requests one session may have in flight. Bounded by the buffer pool the
  // client posts receives from - a page each - so it cannot exceed what
  // FileClient::maxOutstanding allows.
  size_t Depth = 8;
  // Bytes to read from the daemon at once when a client reads forward. A
  // kernel caps an NFS read at 1 MiB whatever the export offers, and a 1 MiB
  // ranged read is a fifth as quick as a large one, so a sequential reader is
  // served out of a window fetched in one call.
  size_t Readahead = 8u << 20;
  ServiceOptions Service;
};

// Suspends callers until whoever holds it gives it back. Replies share one
// socket and a write can suspend part way, so two handlers writing at once
// would interleave their frames.
class Gate {
public:
  auto take() {
    struct Awaiter {
      Gate *G;
      std::coroutine_handle<> Queued{};

      bool await_ready() const noexcept { return !G->Held; }
      void await_suspend(std::coroutine_handle<> H) {
        Queued = H;
        G->Waiting.push_back(H);
      }
      void await_resume() noexcept {
        Queued = {};
        G->Held = true;
      }

      // A coroutine destroyed while it waits takes its handle with it, rather
      // than leaving one for give() to resume into freed memory.
      ~Awaiter() {
        if (Queued) std::erase(G->Waiting, Queued);
      }
    };
    return Awaiter{this};
  }

  // Ownership passes straight to the next waiter. Clearing the flag and then
  // waking one lets a coroutine that arrives in between take it as well, and
  // two writers on one socket interleave their frames.
  void give() {
    if (Waiting.empty()) {
      Held = false;
      return;
    }
    auto H = Waiting.front();
    Waiting.pop_front();
    Loop::get().schedule(H);
  }

private:
  bool Held = false;
  std::deque<std::coroutine_handle<>> Waiting;
};

// Bytes already fetched from the daemon, and which part of which file they are.
struct Window {
  std::string Path;
  uint64_t Start = 0;
  uint64_t FileSize = 0;
  uint64_t Stamp = 0;
  // Valid bytes, kept apart from the buffer so a refill reuses the allocation
  // rather than freeing it and zeroing a fresh one.
  size_t Bytes = 0;
  std::vector<std::byte> Data;
  AddressSpace Pages;

  bool covers(const std::string &Want, uint64_t Offset, size_t Length) const {
    return Bytes > 0 && Path == Want && Offset >= Start && Offset + Length <= Start + Bytes;
  }

  std::span<std::byte> room(size_t Want) {
    if (Data.size() < Want) Data.resize(Want);
    Bytes = 0;
    Path.clear();
    return std::span<std::byte>(Data.data(), Want);
  }

  // One page, since a ranged read never exceeds one transfer. Null when the
  // memory is not registered, which leaves the read on the copying path.
  Page *page(size_t Want) {
    if (Pages.capacity() < Want || Pages.pageSize() < Want) Pages.claim(Memory::get(), Want, 1);
    Pages.rebase(0);
    Bytes = 0;
    Path.clear();
    const auto At = Pages.at(0, Want);
    return At.Where && At.Where->region() ? At.Where : nullptr;
  }

  std::span<const std::byte> bytes() const {
    if (Pages.backed()) return {const_cast<AddressSpace &>(Pages).at(0, Bytes).Where->bytes(), Bytes};
    return {Data.data(), Bytes};
  }
};

// Sessions to the daemon, handed out one at a time. A session answers one
// request at a time, so this is what decides how many reads a mount can have
// running at once.
class Remotes {
public:
  Remotes(const RemoteOptions &Opts) : Opts(Opts), Slots(std::max<size_t>(1, Opts.Sessions)) {}

  Remotes(const Remotes &) = delete;
  Remotes &operator=(const Remotes &) = delete;

  class Lease {
  public:
    Lease() = default;
    Lease(Remotes *Owner, size_t Index) : Owner(Owner), Index(Index) {}
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&Other) noexcept
        : Owner(std::exchange(Other.Owner, nullptr)), Index(Other.Index), Generation(Other.Generation), Held(std::move(Other.Held)),
          Paper(std::move(Other.Paper)) {}
    ~Lease() {
      if (Owner) Owner->give(Index, std::move(Paper));
    }

    FileClient &client() const { return *Held; }
    Window &scratch() const { return *Paper; }
    size_t index() const { return Index; }
    uint64_t generation() const { return Generation; }

    // Takes the session out of the pool without taking it from whoever else is
    // using it: their reads keep the client alive through this handle until
    // they finish with it.
    void discard() const {
      if (Owner->Slots[Index].Client == Held) Owner->Slots[Index].Client.reset();
    }

  private:
    friend class Remotes;

    Remotes *Owner = nullptr;
    size_t Index = 0;
    uint64_t Generation = 0;
    std::shared_ptr<FileClient> Held;
    std::unique_ptr<Window> Paper;
  };

  Coro<Result<Lease>> take() {
    for (;;) {
      size_t Emptiest = Slots.size();
      for (size_t I = 0; I < Slots.size(); I++)
        if (Slots[I].Users < depthFor(I) && (Emptiest == Slots.size() || Slots[I].Users < Slots[Emptiest].Users)) Emptiest = I;

      if (Emptiest < Slots.size()) co_return co_await claim(Emptiest);
      co_await Free{this, Slots.size()};
    }
  }

  // A particular slot, because the mount pins an open handle to the session
  // that opened it.
  Coro<Result<Lease>> takeAt(size_t Index) {
    while (Slots[Index].Users >= depthFor(Index)) co_await Free{this, Index};
    co_return co_await claim(Index);
  }

  size_t count() const { return Slots.size(); }

  // Connects every session at once. Left to itself each one is brought up by
  // the first read that lands on it, and a UCX wireup takes a second or two,
  // so a mount would pay it again and again at random moments instead of once.
  Coro<Result<void>> warm() {
    std::vector<Coro<Result<void>>> Opening;
    for (size_t I = 0; I < Slots.size(); I++) {
      if (Slots[I].Client || Slots[I].Opening) continue;
      Opening.push_back(open(I));
      Opening.back().start();
    }

    Result<void> Outcome{};
    for (auto &Task : Opening)
      if (auto R = co_await Task.join(); !R) Outcome = std::unexpected(R.error());
    co_return Outcome;
  }

  Coro<void> close() {
    for (auto &S : Slots)
      if (S.Client) co_await S.Client->close();
  }

private:
  friend class Lease;

  struct Free {
    Remotes *R;
    size_t Index;
    std::coroutine_handle<> Queued{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> H) {
      Queued = H;
      queue().push_back(H);
    }
    void await_resume() noexcept { Queued = {}; }

    // A coroutine given up on while it waits for a session takes its handle
    // with it. This awaiter lives in that coroutine's frame, so this runs when
    // the frame goes, and what is left is a queue wake() can resume safely.
    ~Free() {
      if (Queued) std::erase(queue(), Queued);
    }

  private:
    std::deque<std::coroutine_handle<>> &queue() const { return Index < R->Slots.size() ? R->Slots[Index].Waiting : R->Waiting; }
  };

  Coro<Result<Lease>> claim(size_t Index) {
    Slots[Index].Users++;
    Lease Mine(this, Index);
    Mine.Paper = paper();

    // Only one caller may bring a session up. The rest wait rather than open a
    // second connection over the top of the first.
    while (Slots[Index].Opening) co_await Free{this, Index};

    if (!Slots[Index].Client) {
      Slots[Index].Opening = true;
      auto Made = co_await FileClient::connect(Opts.Host, Opts.Service);
      Slots[Index].Opening = false;
      wake(Index);
      if (!Made) co_return std::unexpected(Made.error());
      Slots[Index].Client = std::shared_ptr<FileClient>(std::move(*Made));
      Slots[Index].Generation++;
    }

    Mine.Held = Slots[Index].Client;
    Mine.Generation = Slots[Index].Generation;
    co_return Mine;
  }

  Coro<Result<void>> open(size_t Index) {
    Slots[Index].Users++;
    Slots[Index].Opening = true;

    auto Made = co_await FileClient::connect(Opts.Host, Opts.Service);
    Slots[Index].Opening = false;
    if (Made) {
      Slots[Index].Client = std::shared_ptr<FileClient>(std::move(*Made));
      Slots[Index].Generation++;
    }
    give(Index, nullptr);
    if (!Made) co_return std::unexpected(Made.error());
    co_return Result<void>{};
  }

  // What one session may carry, and never more than the transport allows: a
  // byte stream says one, because two payloads sharing it interleave on the
  // wire and corrupt each other in silence. The client folds that together
  // with what its buffer pool can hold. Until a session is up it is one, which
  // is the answer that is never wrong.
  size_t depthFor(size_t Index) const {
    if (!Slots[Index].Client) return 1;
    return std::max<size_t>(1, std::min(Opts.Depth, Slots[Index].Client->maxOutstanding()));
  }

  std::unique_ptr<Window> paper() {
    if (Papers.empty()) return std::make_unique<Window>();
    auto Sheet = std::move(Papers.back());
    Papers.pop_back();
    return Sheet;
  }

  void give(size_t Index, std::unique_ptr<Window> Sheet) {
    if (Sheet) Papers.push_back(std::move(Sheet));
    Slots[Index].Users--;
    wake(Index);
  }

  void wake(size_t Index) {
    auto &Own = Slots[Index].Waiting;
    if (!Own.empty()) {
      auto H = Own.front();
      Own.pop_front();
      Loop::get().schedule(H);
      return;
    }
    if (Waiting.empty()) return;
    auto H = Waiting.front();
    Waiting.pop_front();
    Loop::get().schedule(H);
  }

  struct Slot {
    std::shared_ptr<FileClient> Client;
    std::deque<std::coroutine_handle<>> Waiting;
    uint64_t Generation = 0;
    size_t Users = 0;
    bool Opening = false;
  };

  const RemoteOptions &Opts;
  std::vector<Slot> Slots;
  std::vector<std::unique_ptr<Window>> Papers;
  std::deque<std::coroutine_handle<>> Waiting;
};

} // namespace rail::vfs
