#include "rail/app/signature.h"
#include "rail/app/checksum.h"

#include <algorithm>
#include <vector>

namespace rail {

uint32_t chooseBlockLength(uint64_t FileLength, uint32_t MaxBlockSize) {
  if (FileLength <= uint64_t(kDefaultBlockSize) * kDefaultBlockSize) return kDefaultBlockSize;

  uint32_t C = 1;
  for (uint64_t L = FileLength; L >>= 2;) C <<= 1;
  if (C == 0 || C >= MaxBlockSize) return MaxBlockSize;

  uint32_t Blength = 0;
  do {
    Blength |= C;
    if (FileLength < uint64_t(Blength) * Blength) Blength &= ~C;
    C >>= 1;
  } while (C >= 8); // round to a multiple of 8

  return std::max(Blength, kDefaultBlockSize);
}

uint32_t chooseStrongLength(uint64_t FileLength, uint32_t BlockLength) {
  int B = kBlockSumBias;
  for (uint64_t L = FileLength; L >>= 1;) B += 2;
  for (uint32_t C = BlockLength; (C >>= 1) && B;) B--;

  const int S2 = (B + 1 - 32 + 7) / 8;
  return static_cast<uint32_t>(std::clamp(S2, 2, 16));
}

uint32_t blockLengthAt(const proto::Signature &Sig, uint32_t Index) {
  if (Sig.BlockLength == 0 || Index >= Sig.Sums.size()) return 0;
  const uint64_t Start = uint64_t(Index) * Sig.BlockLength;
  const uint64_t Left = Sig.FileLength - Start;
  return static_cast<uint32_t>(std::min<uint64_t>(Left, Sig.BlockLength));
}

Coro<Result<proto::Signature>> buildSignature(FileReader &Basis, uint32_t BlockLength, uint32_t StrongLength) {
  proto::Signature Sig;
  Sig.BlockLength = BlockLength;
  Sig.StrongLength = StrongLength;
  Sig.FileLength = Basis.meta().Size;

  if (Sig.FileLength == 0 || BlockLength == 0) co_return Sig;

  const uint64_t Count = (Sig.FileLength + BlockLength - 1) / BlockLength;
  Sig.Sums.reserve(static_cast<size_t>(Count));

  std::vector<std::byte> Block(BlockLength);
  uint64_t Offset = 0;
  while (Offset < Sig.FileLength) {
    const size_t Want = static_cast<size_t>(std::min<uint64_t>(BlockLength, Sig.FileLength - Offset));
    auto N = co_await Basis.read({Block.data(), Want}, Offset);
    if (!N) co_return std::unexpected(N.error());
    if (*N == 0) break;

    const std::span<const std::byte> View{Block.data(), *N};
    proto::BlockSum S;
    S.Weak = weakChecksum(View);
    S.Strong = strongChecksum(View);
    Sig.Sums.push_back(S);

    Offset += *N;
  }

  co_return Sig;
}

} // namespace rail
