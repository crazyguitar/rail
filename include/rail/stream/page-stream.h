#pragma once

#include "rail/app/checksum.h"
#include "rail/io/coro.h"
#include "rail/result.h"
#include "rail/stream/sink.h"
#include "rail/transport/data-channel.h"

#include <cstdint>
#include <deque>

namespace rail {

struct StreamGeometry {
  size_t Window = 2;
  size_t ReadAhead = 4;
  size_t WriteDepth = 2;

  static StreamGeometry forChannel(DataChannel &Channel);
};

class PageSender {
public:
  // Which hash the session agreed on, not the default: a client that can only
  // compute one of them says so in Hello, and a stream that hashed with the
  // other would fail every transfer on a digest neither end could reconcile.
  PageSender(DataChannel &Channel,
             PageSource &Source,
             uint64_t TagBase,
             StreamGeometry G,
             bool FlipOneBit = false,
             bool Verify = true,
             Sum Which = Sum::XxH3,
             size_t AbortAfterPages = 0)
      : Channel(Channel), Source(Source), TagBase(TagBase), G(G), FlipOneBit(FlipOneBit), AbortAfterPages(AbortAfterPages), Whole(Verify, Which) {}

  PageSender(const PageSender &) = delete;
  PageSender &operator=(const PageSender &) = delete;

  Coro<Result<uint64_t>> stream(uint64_t Offset, uint64_t Length);

  Digest digest() const { return Whole.digest(); }

  bool matches(const Digest &Theirs) const { return Whole.matches(Theirs); }

private:
  struct Reading {
    Page Buf;
    Page *Direct = nullptr;
    Uring::Read Op;
    uint64_t Key = 0;
    uint32_t Length = 0;
    bool Failed = false;
  };

  struct Sending {
    Page Buf;
    Page *Direct = nullptr;
    Coro<Result<void>> Op;
  };

  Coro<Result<uint64_t>> run(uint64_t Offset, uint64_t Length);
  Coro<Result<void>> fill(uint64_t &Cursor, uint64_t End);
  Coro<Result<void>> shipReady();
  Coro<size_t> readOrZero(Reading &Ready);
  static void markFailed(Reading &R);
  Coro<Result<void>> drain(size_t Keep);

  Coro<void> quiesce();

  DataChannel &Channel;
  PageSource &Source;
  uint64_t TagBase;
  StreamGeometry G;
  bool FlipOneBit = false;
  size_t AbortAfterPages = 0;
  size_t Pooled = 0;
  Verifier Whole;
  Result<void> Failure = Result<void>{};
  std::deque<Reading> Prefetch;
  std::deque<Sending> InFlight;
};

class PageReceiver {
public:
  PageReceiver(DataChannel &Channel, PageSink &Sink, uint64_t TagBase, StreamGeometry G, bool Verify = true, Sum Which = Sum::XxH3)
      : Channel(Channel), Sink(Sink), TagBase(TagBase), G(G), Whole(Verify, Which) {}

  PageReceiver(const PageReceiver &) = delete;
  PageReceiver &operator=(const PageReceiver &) = delete;

  Coro<Result<uint64_t>> land(uint64_t Offset, uint64_t Length);

  Digest digest() const { return Whole.digest(); }

  bool matches(const Digest &Theirs) const { return Whole.matches(Theirs); }

private:
  struct Posted {
    Page Buf;
    Page *Direct = nullptr;
    Coro<Result<void>> Op;
    uint64_t Offset = 0;
    uint32_t Length = 0;
  };

  struct Writing {
    Page Buf;
    Uring::Write Op;
  };

  Coro<Result<uint64_t>> run(uint64_t Offset, uint64_t Length);
  Coro<Result<void>> store(size_t Keep);
  Coro<Result<void>> awaitWrites(size_t Keep);
  Coro<void> quiesce();

  DataChannel &Channel;
  PageSink &Sink;
  uint64_t TagBase;
  StreamGeometry G;
  Verifier Whole;
  std::deque<Posted> Receiving;
  std::deque<Writing> Writes;
};

} // namespace rail
