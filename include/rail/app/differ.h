#pragma once

#include "rail/app/report.h"
#include "rail/fs/reader.h"
#include "rail/io/coro.h"
#include "rail/proto/message.h"
#include "rail/result.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace rail {

// Emitted in file order. Copy reuses a destination block; Literal means these
// source bytes follow on the data channel.
struct Instruction {
  enum class Kind { Copy, Literal };

  Kind K = Kind::Literal;
  uint32_t BlockIndex = 0; // Copy
  uint64_t SrcOffset = 0;  // offset in the source file
  uint64_t DstOffset = 0;  // where it lands in the destination
  uint32_t Length = 0;

  // Copy only: the matched bytes, still sitting in the differ's window. Saves
  // the sender a second read of every matched block just to hash it. Valid
  // only for the duration of the sink call - never retain it.
  std::span<const std::byte> Bytes;
};

using InstructionSink = std::function<Coro<Result<void>>(const Instruction &)>;

// Consumes a Signature and a source file, produces instructions. Touches no
// socket, which is what makes the hardest algorithm here exercisable without
// any network at all.
class Differ {
public:
  Differ(const proto::Signature &Sig, uint32_t MaxChainLen = 32);

  Coro<Result<void>> diff(FileReader &Src, const InstructionSink &Out, Report &Rep);

private:
  void buildHashTable();
  int32_t lookup(uint32_t Weak, std::span<const std::byte> Window, Report &Rep);

  Coro<Result<void>> fillWindow(FileReader &Src, uint64_t At);
  Coro<Result<void>> ensureWindow(FileReader &Src, uint64_t At);
  Coro<Result<void>> emitLiteral(const InstructionSink &Out, uint64_t Offset, uint64_t Length, Report &Rep);
  Coro<Result<void>> emitCopy(const InstructionSink &Out, int32_t Index, uint64_t SrcOffset, std::span<const std::byte> Bytes, Report &Rep);

  // The basis file's final block is short whenever the file is not a whole
  // multiple of the block length, and the main scan can never match it: it
  // stops once a full-width window no longer fits. Returns true when it
  // matched and consumed the tail.
  Coro<Result<bool>> matchFinalBlock(FileReader &Src, const InstructionSink &Out, uint64_t From, Report &Rep);

  const proto::Signature &Sig;
  uint32_t MaxChainLen;
  uint32_t TableSize = 0;
  int32_t WantI = 0;
  uint64_t DstCursor = 0;

  std::vector<int32_t> HashTable;
  std::vector<int32_t> Chain;

  std::vector<std::byte> Window;
  uint64_t WindowBase = 0;
  size_t WindowLen = 0;
  uint64_t SourceLen = 0;
};

} // namespace rail
