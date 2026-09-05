#include "rail/stream/page-stream.h"

#include "rail/io/trace.h"

#include <algorithm>
#include <cstring>

namespace rail {

namespace {

uint64_t alignUp(uint64_t N) { return (N + kDirectAlignment - 1) & ~(kDirectAlignment - 1); }

} // namespace

StreamGeometry StreamGeometry::forChannel(DataChannel &Channel) {
  StreamGeometry G;
  const size_t Pages = Channel.pool().pageCount();

  const size_t InFlight = Channel.traits().MaxInFlight;

  G.Window = std::max<size_t>(1, std::min({Pages / 2, Pages - 1, InFlight}));
  G.ReadAhead = std::max<size_t>(1, std::min(Pages > G.Window + 1 ? Pages - G.Window - 1 : 1, Uring::depth()));

  G.WriteDepth = std::min<size_t>(2, G.ReadAhead);
  return G;
}

// A page whose read failed. Drop it and the peer hangs on its posted receive;
// await its read and quiesce hangs on a completion that never comes.
//
//   drop        -->  x            peer recv waits forever
//   this: zeros -->  [00000000]   peer recv completes, digest fails it
//         op.Done -->  await returns 0 at once, never blocks
void PageSender::markFailed(Reading &R) {
  R.Failed = true;
  R.Op.Done = true;
  R.Op.Result = 0;
}

Coro<Result<void>> PageSender::fill(uint64_t &Cursor, uint64_t End) {
  const uint64_t PageBytes = Channel.pool().pageSize();

  while (Prefetch.size() < G.ReadAhead && Cursor < End) {
    const size_t Take = static_cast<size_t>(std::min<uint64_t>(PageBytes, End - Cursor));

    // A page the caller owns is never flipped: the fault injector exists to
    // corrupt what goes on the wire, not the caller's own data.
    if (!FlipOneBit)
      if (Page *Mine = Source.sending(Cursor, Take)) {
        Mine->resize(Take);
        Prefetch.push_back({});
        Reading &Straight = Prefetch.back();
        Straight.Direct = Mine;
        Straight.Key = Cursor;
        Straight.Length = static_cast<uint32_t>(Take);
        Cursor += Take;
        continue;
      }

    Page Buf;
    {
      Scoped T("tx.acquire");
      Buf = co_await Channel.pool().acquire();
    }
    if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");
    Buf.resize(static_cast<size_t>(std::min<uint64_t>(Buf.capacity(), End - Cursor)));

    Prefetch.push_back({});
    Reading &R = Prefetch.back();
    R.Buf = std::move(Buf);
    R.Key = Cursor;
    R.Length = static_cast<uint32_t>(R.Buf.size());

    const bool Abort = AbortAfterPages > 0 && Pooled >= AbortAfterPages;
    Pooled++;
    if (Abort) {
      if (Failure) Failure = failMessage("stream aborted before reading a page");
      markFailed(R);
      Cursor += R.Length;
      continue;
    }

    const size_t Ask = Source.direct() ? std::min<size_t>(alignUp(R.Length), R.Buf.capacity()) : R.Length;
    if (auto S = Source.submitRead(R.Op, {R.Buf.bytes(), Ask}, R.Key); !S) {
      if (Failure) Failure = std::unexpected(S.error());
      markFailed(R);
    }
    Cursor += R.Length;
  }
  co_return Result<void>{};
}

Coro<Result<void>> PageSender::shipReady() {
  Reading &Ready = Prefetch.front();

  if (Ready.Direct) {
    const uint64_t Key = Ready.Key;
    {
      Scoped T("tx.hash");
      Whole.update(Ready.Direct->data());
    }

    InFlight.push_back({});
    InFlight.back().Direct = Ready.Direct;
    Prefetch.pop_front();

    Scoped T("tx.send");
    InFlight.back().Op = Channel.send(*InFlight.back().Direct, TagBase + Key);
    InFlight.back().Op.start();
    co_return Result<void>{};
  }

  const size_t Got = co_await readOrZero(Ready);

  const uint64_t Key = Ready.Key;
  Ready.Buf.resize(std::min<size_t>(Ready.Length, Got));

  {
    Scoped T("tx.hash");
    Whole.update(Ready.Buf.data());
  }

  InFlight.push_back({});
  InFlight.back().Buf = std::move(Ready.Buf);
  Prefetch.pop_front();

  if (FlipOneBit && InFlight.back().Buf.size() > 8) {
    FlipOneBit = false;
    InFlight.back().Buf.bytes()[7] ^= std::byte{0x01};
  }

  Scoped T("tx.send");
  InFlight.back().Op = Channel.send(InFlight.back().Buf, TagBase + Key);
  InFlight.back().Op.start();
  co_return Result<void>{};
}

Coro<size_t> PageSender::readOrZero(Reading &Ready) {
  if (!Ready.Failed) {
    Scoped T("tx.read");
    auto Got = co_await Source.awaitRead(Ready.Op);
    if (Got) co_return *Got;
    if (Failure) Failure = std::unexpected(Got.error());
  }
  std::memset(Ready.Buf.bytes(), 0, Ready.Length);
  co_return Ready.Length;
}

Coro<Result<void>> PageSender::drain(size_t Keep) {
  while (InFlight.size() > Keep) {
    Scoped T("tx.retire");
    auto Landed = co_await InFlight.front().Op.join();
    InFlight.pop_front();
    if (!Landed) co_return std::unexpected(Landed.error());
  }
  co_return Result<void>{};
}

Coro<void> PageSender::quiesce() {
  while (!InFlight.empty()) {
    [[maybe_unused]] auto Ignored = co_await InFlight.front().Op.join();
    InFlight.pop_front();
  }
  while (!Prefetch.empty()) {
    [[maybe_unused]] auto Ignored = co_await Source.awaitRead(Prefetch.front().Op);
    Prefetch.pop_front();
  }
}

Coro<Result<uint64_t>> PageSender::stream(uint64_t Offset, uint64_t Length) {
  auto Outcome = co_await run(Offset, Length);
  co_await quiesce();
  co_return Outcome;
}

Coro<Result<uint64_t>> PageSender::run(uint64_t Offset, uint64_t Length) {
  const uint64_t End = Offset + Length;
  uint64_t Cursor = Offset;

  while (Cursor < End || !Prefetch.empty()) {
    if (auto R = co_await fill(Cursor, End); !R) co_return std::unexpected(R.error());
    if (Prefetch.empty()) break;
    if (auto R = co_await drain(G.Window - 1); !R) co_return std::unexpected(R.error());
    if (auto R = co_await shipReady(); !R) co_return std::unexpected(R.error());
  }

  if (auto R = co_await drain(0); !R) co_return std::unexpected(R.error());
  if (!Failure) co_return std::unexpected(Failure.error());
  co_return Length;
}

Coro<Result<void>> PageReceiver::store(size_t Keep) {
  while (Receiving.size() > Keep) {
    Posted &P = Receiving.front();

    Result<void> Landed;
    {
      Scoped T("rx.recv");
      Landed = co_await P.Op.join();
    }
    if (!Landed) {
      Receiving.pop_front();
      co_return std::unexpected(Landed.error());
    }

    {
      Scoped T("rx.hash");
      Whole.update(P.Direct ? P.Direct->data() : P.Buf.data());
    }

    if (P.Direct) {
      Receiving.pop_front();
      continue;
    }

    Writes.push_back({});
    Writing &W = Writes.back();
    W.Buf = std::move(P.Buf);
    const uint64_t At = P.Offset;
    Receiving.pop_front();

    Scoped T("rx.submit");
    const size_t Padded = std::min(Sink.writeLength(W.Buf.size()), W.Buf.capacity());
    if (auto R = Sink.submitWrite(W.Op, {W.Buf.bytes(), Padded}, At); !R) {
      Writes.pop_back();
      co_return std::unexpected(R.error());
    }
  }
  co_return Result<void>{};
}

Coro<Result<void>> PageReceiver::awaitWrites(size_t Keep) {
  while (Writes.size() > Keep) {
    Scoped T("rx.drain");
    auto Landed = co_await Sink.awaitWrite(Writes.front().Op);
    Writes.pop_front();
    if (!Landed) co_return std::unexpected(Landed.error());
  }
  co_return Result<void>{};
}

Coro<void> PageReceiver::quiesce() {
  while (!Receiving.empty()) {
    [[maybe_unused]] auto Ignored = co_await Receiving.front().Op.join();
    Receiving.pop_front();
  }
  while (!Writes.empty()) {
    [[maybe_unused]] auto Ignored = co_await Sink.awaitWrite(Writes.front().Op);
    Writes.pop_front();
  }
}

Coro<Result<uint64_t>> PageReceiver::land(uint64_t Offset, uint64_t Length) {
  auto Outcome = co_await run(Offset, Length);
  co_await quiesce();
  co_return Outcome;
}

Coro<Result<uint64_t>> PageReceiver::run(uint64_t Offset, uint64_t Length) {
  const uint64_t End = Offset + Length;
  const uint64_t PageBytes = Channel.pool().pageSize();

  for (uint64_t At = Offset; At < End;) {
    if (auto R = co_await store(G.Window - 1); !R) co_return std::unexpected(R.error());
    if (auto R = co_await awaitWrites(G.WriteDepth); !R) co_return std::unexpected(R.error());

    const uint32_t Take = static_cast<uint32_t>(std::min<uint64_t>(PageBytes, End - At));

    if (Page *Home = Sink.landing(At, Take)) {
      Home->resize(Take);
      Receiving.push_back({});
      Posted &Straight = Receiving.back();
      Straight.Direct = Home;
      Straight.Offset = At;
      Straight.Length = Take;
      Straight.Op = Channel.recv(*Home, TagBase + At, Take);
      Straight.Op.start();
      Memory::get().countDirect();
      At += Take;
      continue;
    }

    Page Buf;
    {
      Scoped T("rx.acquire");
      Buf = co_await Channel.pool().acquire();
    }
    if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");

    Buf.resize(Take);

    Receiving.push_back({});
    Posted &P = Receiving.back();
    P.Buf = std::move(Buf);
    P.Offset = At;
    P.Length = Take;
    P.Op = Channel.recv(P.Buf, TagBase + At, Take);
    P.Op.start();
    Memory::get().countCopied();
    At += Take;
  }

  if (auto R = co_await store(0); !R) co_return std::unexpected(R.error());
  if (auto R = co_await awaitWrites(0); !R) co_return std::unexpected(R.error());
  co_return Length;
}

} // namespace rail
