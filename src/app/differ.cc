#include "rail/app/differ.h"
#include "rail/app/checksum.h"
#include "rail/app/signature.h"

#include <algorithm>
#include <cstring>

namespace rail {

namespace {
// Bounds the source window held in memory. Nothing here scales with file size.
constexpr size_t kMaxWindow = 4u << 20;
} // namespace

Differ::Differ(const proto::Signature &Sig, uint32_t MaxChainLen) : Sig(Sig), MaxChainLen(MaxChainLen) {}

void Differ::buildHashTable() {
  const size_t Count = Sig.Sums.size();

  // rsync sizes the table for about 80% load.
  TableSize = static_cast<uint32_t>(Count / 8) * 10 + 11;
  if (TableSize < (1u << 16)) TableSize = 1u << 16;

  HashTable.assign(TableSize, -1);
  Chain.assign(Count, -1);
  for (size_t I = 0; I < Count; I++) {
    const uint32_t T = weakHash(Sig.Sums[I].Weak) % TableSize;
    Chain[I] = HashTable[T];
    HashTable[T] = static_cast<int32_t>(I);
  }
}

int32_t Differ::lookup(uint32_t Weak, std::span<const std::byte> Win, Report &Rep) {
  const uint32_t Entry = weakHash(Weak) % TableSize;
  int32_t I = HashTable[Entry];
  if (I < 0) return -1;

  Rep.HashHits++;

  bool StrongComputed = false;
  Digest Strong{};
  int32_t Best = -1;
  uint32_t ChainLen = 0;

  for (; I >= 0; I = Chain[static_cast<size_t>(I)]) {
    // Bound work on a pathological bucket. Skipped data goes out literally,
    // so correctness is unaffected and worst-case CPU stays bounded.
    if (++ChainLen > MaxChainLen) break;

    const auto Idx = static_cast<size_t>(I);
    if (Sig.Sums[Idx].Weak != Weak) continue;
    if (blockLengthAt(Sig, static_cast<uint32_t>(I)) != Win.size()) continue;

    if (!StrongComputed) {
      Strong = strongChecksum(Win);
      StrongComputed = true;
    }
    if (std::memcmp(Strong.data(), Sig.Sums[Idx].Strong.data(), Sig.StrongLength) != 0) {
      Rep.FalseAlarms++;
      continue;
    }

    // want_i: prefer the block continuing the previous match, so copy runs
    // stay contiguous and the receiver reads sequentially.
    if (I == WantI) return I;
    if (Best < 0) Best = I;
  }
  return Best;
}

Coro<Result<void>> Differ::fillWindow(FileReader &Src, uint64_t At) {
  const size_t Want = static_cast<size_t>(std::min<uint64_t>(Window.size(), SourceLen - At));
  auto N = co_await Src.read({Window.data(), Want}, At);
  if (!N) co_return std::unexpected(N.error());
  WindowBase = At;
  WindowLen = *N;
  co_return Result<void>{};
}

Coro<Result<void>> Differ::ensureWindow(FileReader &Src, uint64_t At) {
  // Refill only when the window no longer covers the block starting at At.
  // Refilling unconditionally re-reads the entire window on every match, which
  // on an unchanged file means re-reading it once per block.
  const uint64_t Need = std::min<uint64_t>(At + Sig.BlockLength, SourceLen);
  if (At >= WindowBase && Need <= WindowBase + WindowLen) co_return Result<void>{};
  co_return co_await fillWindow(Src, At);
}

Coro<Result<void>> Differ::emitLiteral(const InstructionSink &Out, uint64_t Offset, uint64_t Length, Report &Rep) {
  while (Length > 0) {
    const uint32_t N = static_cast<uint32_t>(std::min<uint64_t>(Length, kMaxWindow));
    Instruction Ins;
    Ins.K = Instruction::Kind::Literal;
    Ins.SrcOffset = Offset;
    Ins.DstOffset = DstCursor;
    Ins.Length = N;
    if (auto R = co_await Out(Ins); !R) co_return std::unexpected(R.error());

    Rep.LiteralBytes += N;
    Offset += N;
    DstCursor += N;
    Length -= N;
  }
  co_return Result<void>{};
}

Coro<Result<void>> Differ::emitCopy(const InstructionSink &Out, int32_t Index, uint64_t SrcOffset, std::span<const std::byte> Bytes, Report &Rep) {
  Instruction Ins;
  Ins.K = Instruction::Kind::Copy;
  Ins.BlockIndex = static_cast<uint32_t>(Index);
  Ins.SrcOffset = SrcOffset;
  Ins.DstOffset = DstCursor;
  Ins.Length = static_cast<uint32_t>(Bytes.size());
  Ins.Bytes = Bytes;
  if (auto R = co_await Out(Ins); !R) co_return std::unexpected(R.error());

  Rep.MatchedBytes += Bytes.size();
  DstCursor += Bytes.size();
  co_return Result<void>{};
}

Coro<Result<bool>> Differ::matchFinalBlock(FileReader &Src, const InstructionSink &Out, uint64_t From, Report &Rep) {
  const auto LastIdx = static_cast<uint32_t>(Sig.Sums.size() - 1);
  const uint32_t LastLen = blockLengthAt(Sig, LastIdx);
  if (LastLen == 0 || LastLen >= Sig.BlockLength || SourceLen < LastLen) co_return false;

  // That block can only ever match where the remaining source length equals
  // its own, so check that one offset instead of rescanning with a shrinking
  // window.
  const uint64_t At = SourceLen - LastLen;
  if (At < From) co_return false;

  std::vector<std::byte> Tail(LastLen);
  auto N = co_await Src.read(Tail, At);
  if (!N) co_return std::unexpected(N.error());
  if (*N != LastLen) co_return false;

  const std::span<const std::byte> View{Tail.data(), *N};
  if (weakChecksum(View) != Sig.Sums[LastIdx].Weak) co_return false;
  if (std::memcmp(strongChecksum(View).data(), Sig.Sums[LastIdx].Strong.data(), Sig.StrongLength) != 0) co_return false;

  if (At > From) {
    if (auto R = co_await emitLiteral(Out, From, At - From, Rep); !R) co_return std::unexpected(R.error());
  }
  if (auto R = co_await emitCopy(Out, static_cast<int32_t>(LastIdx), At, View, Rep); !R) co_return std::unexpected(R.error());
  co_return true;
}

Coro<Result<void>> Differ::diff(FileReader &Src, const InstructionSink &Out, Report &Rep) {
  SourceLen = Src.meta().Size;
  const uint32_t Block = Sig.BlockLength;

  DstCursor = 0;
  WantI = 0;

  if (Sig.Sums.empty() || Block == 0 || SourceLen < Block) co_return co_await emitLiteral(Out, 0, SourceLen, Rep);

  buildHashTable();

  Window.assign(static_cast<size_t>(std::min<uint64_t>(kMaxWindow, SourceLen)), std::byte{});
  WindowBase = 0;
  WindowLen = 0;
  if (auto R = co_await fillWindow(Src, 0); !R) co_return std::unexpected(R.error());

  uint64_t Offset = 0;
  uint64_t LastMatch = 0;
  uint32_t Weak = 0;
  bool Stale = true; // Weak must be recomputed rather than rolled

  while (Offset + Block <= SourceLen) {
    if (auto R = co_await ensureWindow(Src, Offset); !R) co_return std::unexpected(R.error());

    const size_t W = static_cast<size_t>(Offset - WindowBase);
    if (Stale) {
      Weak = weakChecksum({Window.data() + W, Block});
      Stale = false;
    }

    const int32_t I = lookup(Weak, {Window.data() + W, Block}, Rep);
    if (I < 0) {
      if (Offset + Block >= SourceLen) break;

      // Roll one byte. Refill first when the incoming byte lies past the
      // window, which also invalidates the rolled sum.
      if (W + Block >= WindowLen) {
        if (auto R = co_await fillWindow(Src, Offset); !R) co_return std::unexpected(R.error());
        Stale = true;
        continue;
      }
      Weak = rollWeak(Weak, Window[W], Window[W + Block], Block);
      Offset++;
      continue;
    }

    if (Offset > LastMatch) {
      if (auto R = co_await emitLiteral(Out, LastMatch, Offset - LastMatch, Rep); !R) co_return std::unexpected(R.error());
    }

    const uint32_t Matched = blockLengthAt(Sig, static_cast<uint32_t>(I));
    if (auto R = co_await emitCopy(Out, I, Offset, {Window.data() + W, Matched}, Rep); !R) co_return std::unexpected(R.error());

    Offset += Matched;
    LastMatch = Offset;
    WantI = I + 1;
    Stale = true;
  }

  if (LastMatch < SourceLen) {
    auto Matched = co_await matchFinalBlock(Src, Out, LastMatch, Rep);
    if (!Matched) co_return std::unexpected(Matched.error());
    if (*Matched) LastMatch = SourceLen;
  }

  if (LastMatch < SourceLen) {
    if (auto R = co_await emitLiteral(Out, LastMatch, SourceLen - LastMatch, Rep); !R) co_return std::unexpected(R.error());
  }

  co_return Result<void>{};
}

} // namespace rail
