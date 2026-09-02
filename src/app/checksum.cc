#include "rail/app/checksum.h"

#include <cstdio>
#include <cstring>
#include <xxhash.h>

namespace rail {

uint32_t weakChecksum(std::span<const std::byte> Block) {
  const auto *Buf = reinterpret_cast<const int8_t *>(Block.data());
  const auto Len = static_cast<int32_t>(Block.size());

  uint32_t S1 = 0;
  uint32_t S2 = 0;
  int32_t I = 0;
  for (; I < Len - 4; I += 4) {
    S2 += 4 * (S1 + static_cast<uint32_t>(Buf[I])) + 3 * static_cast<uint32_t>(Buf[I + 1]) + 2 * static_cast<uint32_t>(Buf[I + 2]) +
          static_cast<uint32_t>(Buf[I + 3]);
    S1 += static_cast<uint32_t>(Buf[I]) + static_cast<uint32_t>(Buf[I + 1]) + static_cast<uint32_t>(Buf[I + 2]) + static_cast<uint32_t>(Buf[I + 3]);
  }
  for (; I < Len; I++) {
    S1 += static_cast<uint32_t>(Buf[I]);
    S2 += S1;
  }
  return (S1 & 0xffff) + (S2 << 16);
}

Digest strongChecksum(std::span<const std::byte> Block) {
  const XXH128_hash_t H = XXH3_128bits(Block.data(), Block.size());
  Digest D{};
  XXH128_canonical_t C;
  XXH128_canonicalFromHash(&C, H);
  std::memcpy(D.data(), C.digest, sizeof(C.digest));
  return D;
}

std::string toHex(const Digest &D) {
  static const char *Hex = "0123456789abcdef";
  std::string S;
  S.reserve(D.size() * 2);
  for (std::byte B : D) {
    const auto V = static_cast<uint8_t>(B);
    S.push_back(Hex[V >> 4]);
    S.push_back(Hex[V & 0x0f]);
  }
  return S;
}

Hasher::Hasher(Sum Which) : Which(Which) {
  if (Which == Sum::XxH64) {
    State = XXH64_createState();
    XXH64_reset(static_cast<XXH64_state_t *>(State), 0);
    return;
  }

  State = XXH3_createState();
  XXH3_128bits_reset(static_cast<XXH3_state_t *>(State));
}

void Hasher::reset() {
  if (Which == Sum::XxH64) {
    XXH64_reset(static_cast<XXH64_state_t *>(State), 0);
    return;
  }
  XXH3_128bits_reset(static_cast<XXH3_state_t *>(State));
}

Hasher::~Hasher() {
  if (Which == Sum::XxH64) {
    XXH64_freeState(static_cast<XXH64_state_t *>(State));
    return;
  }
  XXH3_freeState(static_cast<XXH3_state_t *>(State));
}

void Hasher::update(std::span<const std::byte> Data) {
  if (Which == Sum::XxH64) {
    XXH64_update(static_cast<XXH64_state_t *>(State), Data.data(), Data.size());
    return;
  }
  XXH3_128bits_update(static_cast<XXH3_state_t *>(State), Data.data(), Data.size());
}

Digest Hasher::digest() const {
  Digest D{};

  // Eight bytes of a sixteen byte field. The rest stays zero so the wire shape
  // is the same whichever hash the session agreed on.
  if (Which == Sum::XxH64) {
    const XXH64_hash_t H = XXH64_digest(static_cast<const XXH64_state_t *>(State));
    std::memcpy(D.data(), &H, sizeof(H));
    return D;
  }

  const XXH128_hash_t H = XXH3_128bits_digest(static_cast<const XXH3_state_t *>(State));
  std::memcpy(D.data(), &H.low64, 8);
  std::memcpy(D.data() + 8, &H.high64, 8);
  return D;
}

} // namespace rail
