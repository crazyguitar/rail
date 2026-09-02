#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rail {

// rsync's rolling weak checksum (checksum.c: get_checksum1). Low 16 bits are
// s1, high 16 are s2. Bytes are treated as SIGNED, matching rsync's schar;
// using unsigned here still produces a working checksum but makes rollWeak
// inconsistent with weakChecksum, which silently destroys match rates.
uint32_t weakChecksum(std::span<const std::byte> Block);

// Slides the window one byte in constant time. Out leaves, In enters.
inline uint32_t rollWeak(uint32_t Sum, std::byte Out, std::byte In, uint32_t BlockLen) {
  const auto O = static_cast<uint32_t>(static_cast<int8_t>(Out));
  const auto I = static_cast<uint32_t>(static_cast<int8_t>(In));
  uint32_t S1 = Sum & 0xffff;
  uint32_t S2 = Sum >> 16;
  S1 = (S1 - O + I) & 0xffff;
  S2 = (S2 - BlockLen * O + S1) & 0xffff;
  return S1 | (S2 << 16);
}

// rsync mixes both halves of the weak sum into the bucket index.
inline uint32_t weakHash(uint32_t Weak) { return ((Weak >> 16) + (Weak & 0xffff)) & 0xffff; }

using Digest = std::array<std::byte, 16>;

// Which hash a session agreed on. xxh3-128 is what this project has always
// used; xxh64 exists because a kernel client can only compute that one, and a
// check both ends can perform beats a stronger one only half of them can.
enum class Sum : uint8_t { XxH3 = 1, XxH64 = 2 };

Digest strongChecksum(std::span<const std::byte> Block);
std::string toHex(const Digest &D);

// Streaming xxh3-128 for whole-file verification.
class Hasher {
public:
  explicit Hasher(Sum Which = Sum::XxH3);
  Hasher(const Hasher &) = delete;
  Hasher &operator=(const Hasher &) = delete;
  ~Hasher();

  // Starts a fresh digest. A session hashes one file after another, and each
  // needs its own.
  void reset();

  void update(std::span<const std::byte> Data);
  Digest digest() const;

private:
  Sum Which = Sum::XxH3;
  void *State = nullptr;
};

// A Hasher that can be switched off. Off, it hashes nothing and agrees with
// every digest it is shown, so a caller that trusts the wire pays neither the
// bandwidth nor a branch at each site. Keeping the decision here rather than at
// each call site is what stops one path from being left checking while the
// rest are not.
class Verifier {
public:
  explicit Verifier(bool On, Sum Which = Sum::XxH3) : On(On), H(Which) {}

  void update(std::span<const std::byte> Data) {
    if (On) H.update(Data);
  }

  void reset() {
    if (On) H.reset();
  }

  Digest digest() const { return On ? H.digest() : Digest{}; }

  bool matches(const Digest &Theirs) const { return !On || H.digest() == Theirs; }

  bool on() const { return On; }

private:
  bool On;
  Hasher H;
};

} // namespace rail
