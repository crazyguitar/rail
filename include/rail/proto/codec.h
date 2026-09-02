#pragma once

#include "rail/proto/message.h"
#include "rail/result.h"

#include <cstring>
#include <span>

namespace rail::proto {

// Field-by-field little-endian encoding. Nothing is memcpy'd as a struct, so
// the wire format does not depend on padding or field order in memory.
class Writer {
public:
  explicit Writer(std::vector<std::byte> &Out) : Out(Out) {}

  void u8(uint8_t V) { raw(&V, sizeof(V)); }
  void u16(uint16_t V) { raw(&V, sizeof(V)); }
  void u32(uint32_t V) { raw(&V, sizeof(V)); }
  void u64(uint64_t V) { raw(&V, sizeof(V)); }
  void i64(int64_t V) { raw(&V, sizeof(V)); }

  void str(const std::string &S) {
    u32(static_cast<uint32_t>(S.size()));
    raw(S.data(), S.size());
  }

  void hash(const std::array<std::byte, 16> &H) { raw(H.data(), H.size()); }

  void bytes(std::span<const std::byte> B) {
    u32(static_cast<uint32_t>(B.size()));
    raw(B.data(), B.size());
  }

private:
  void raw(const void *P, size_t N) {
    const auto *B = static_cast<const std::byte *>(P);
    Out.insert(Out.end(), B, B + N);
  }

  std::vector<std::byte> &Out;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> In) : In(In) {}

  bool ok() const { return Ok; }

  uint8_t u8() { return read<uint8_t>(); }
  uint16_t u16() { return read<uint16_t>(); }
  uint32_t u32() { return read<uint32_t>(); }
  uint64_t u64() { return read<uint64_t>(); }
  int64_t i64() { return read<int64_t>(); }

  std::string str() {
    const uint32_t N = u32();
    if (!Ok || Pos + N > In.size()) {
      Ok = false;
      return {};
    }
    std::string S(reinterpret_cast<const char *>(In.data() + Pos), N);
    Pos += N;
    return S;
  }

  std::array<std::byte, 16> hash() {
    std::array<std::byte, 16> H{};
    if (!Ok || Pos + H.size() > In.size()) {
      Ok = false;
      return H;
    }
    std::memcpy(H.data(), In.data() + Pos, H.size());
    Pos += H.size();
    return H;
  }

  size_t remaining() const { return In.size() - Pos; }

private:
  template <typename T> T read() {
    T V{};
    if (!Ok || Pos + sizeof(T) > In.size()) {
      Ok = false;
      return V;
    }
    std::memcpy(&V, In.data() + Pos, sizeof(T));
    Pos += sizeof(T);
    return V;
  }

  std::span<const std::byte> In;
  size_t Pos = 0;
  bool Ok = true;
};

void encode(const Message &M, std::vector<std::byte> &Payload);
Result<Message> decode(Type T, std::span<const std::byte> Payload);

} // namespace rail::proto
