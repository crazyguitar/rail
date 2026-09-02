#pragma once

#include "rail/io/coro.h"
#include "rail/io/stream.h"
#include "rail/proto/message.h"
#include "rail/result.h"

#include "rail/io/loop.h"

#include <deque>
#include <format>
#include <vector>

namespace rail::proto {

// Session control: low-rate, latency-tolerant messages. Constructed over any
// Stream, which is what lets it run on the ssh stdio pipe on the server and on
// a socket in tests without the protocol knowing the difference.
//
// Frame: magic(4) | type(2) | length(4) | payload(length)
class ControlChannel {
public:
  ControlChannel() = default;
  explicit ControlChannel(Stream S) : S(std::move(S)) {}

  // Separate read and write fds, for the ssh pipe where they differ.
  ControlChannel(Stream ReadSide, Stream WriteSide) : S(std::move(ReadSide)), W(std::move(WriteSide)), Split(true) {}

  Coro<Result<void>> send(const Message &M);
  Coro<Result<Message>> receive();

  // Holds the channel across several messages. A streamed transfer reads the
  // frames that follow its request straight off this channel, so a message
  // another caller slips in between would be taken for one of them.
  //
  //   co_await Control.claim();
  //   const ControlChannel::Claim Mine(Control);
  //   co_await Control.sendClaimed(Request);
  //   ... payload on the data channel ...
  //   co_await Control.sendClaimed(Digest);
  auto claim();
  Coro<Result<void>> sendClaimed(const Message &M);

  struct Claim {
    ControlChannel &C;

    explicit Claim(ControlChannel &C) : C(C) {}
    Claim(const Claim &) = delete;
    Claim &operator=(const Claim &) = delete;
    ~Claim() { C.Order.give(); }
  };

  // Receives and requires a particular alternative, so callers do not repeat
  // the same "wrong message type" error handling everywhere.
  template <typename T> Coro<Result<T>> expect() {
    auto M = co_await receive();
    if (!M) co_return std::unexpected(M.error());
    if (auto *V = std::get_if<T>(&*M)) co_return *V;
    co_return failMessage(std::format("unexpected message {}", typeName(typeOf(*M))));
  }

  // Takes a private copy of stdout for the protocol and points fd 1 at stderr,
  // so a library writing to stdout cannot land mid-frame. Both server modes
  // need it: whichever end of a transfer runs under ssh talks over stdio.
  static Result<ControlChannel> overStdio();

  bool valid() const { return S.valid(); }
  int readFd() const { return S.fd(); }
  void close();

private:
  Stream &writer() { return Split ? W : S; }

  // One frame on the wire at a time. Encoding fills a buffer this channel
  // owns and writing it can suspend part-way, so two senders would splice
  // their frames together and the peer would read neither. Ownership passes
  // straight to the next waiter: clearing the flag first lets a sender that
  // arrives in between take it as well.
  class Sending {
  public:
    auto take() {
      struct Awaiter {
        Sending *G;
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

        // A coroutine destroyed while it waits takes its handle with it. The
        // awaiter lives in that coroutine's frame, so this runs then, and what
        // is left behind is a queue give() can resume into safely.
        ~Awaiter() {
          if (Queued) std::erase(G->Waiting, Queued);
        }
      };
      return Awaiter{this};
    }

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

  // Gives the frame back on every exit, including a coroutine destroyed while
  // its write is suspended.
  struct Turn {
    Sending &G;

    explicit Turn(Sending &G) : G(G) {}
    Turn(const Turn &) = delete;
    Turn &operator=(const Turn &) = delete;
    ~Turn() { G.give(); }
  };

  Stream S;
  Stream W;
  bool Split = false;
  Sending Order;
  std::vector<std::byte> OutBuf;
  std::vector<std::byte> InBuf;
};

inline auto ControlChannel::claim() { return Order.take(); }

} // namespace rail::proto
