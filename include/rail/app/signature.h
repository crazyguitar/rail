#pragma once

#include "rail/fs/reader.h"
#include "rail/io/coro.h"
#include "rail/proto/message.h"
#include "rail/result.h"

#include <cstdint>

namespace rail {

// rsync BLOCK_SIZE. Files at or below its square use it unchanged.
inline constexpr uint32_t kDefaultBlockSize = 700;
// rsync BLOCKSUM_BIAS.
inline constexpr int kBlockSumBias = 10;

// rsync generator.c: sum_sizes_sqroot. MaxBlockSize comes from the data
// channel's traits rather than being a constant, so an RDMA channel can use
// larger blocks than rsync's 128 KiB.
uint32_t chooseBlockLength(uint64_t FileLength, uint32_t MaxBlockSize);

// rsync does not send a fixed-width strong hash: it truncates, sized so the
// collision probability stays bounded as files grow. Returns bytes of strong
// hash actually compared, in [2, 16]. The -32 in the formula credits the 32
// bits the rolling weak sum already contributes.
uint32_t chooseStrongLength(uint64_t FileLength, uint32_t BlockLength);

// Length of block Index, accounting for a short final block.
uint32_t blockLengthAt(const proto::Signature &Sig, uint32_t Index);

// Reads the basis file and produces the per-block sums the sender matches
// against. An absent basis file yields an empty signature.
Coro<Result<proto::Signature>> buildSignature(FileReader &Basis, uint32_t BlockLength, uint32_t StrongLength);

} // namespace rail
