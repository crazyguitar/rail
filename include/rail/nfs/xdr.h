#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rail::nfs {

// A reply's header bytes and, kept apart, the payload it must not copy: a read
// answers out of the page the fabric landed it in, so the bytes go on the wire
// straight from there.
struct XdrPayload {
  // An xdr opaque pads to four bytes, so three is the most that can follow.
  static constexpr size_t kMaxPad = 3;

  XdrPayload() = default;
  XdrPayload(std::span<const std::byte> Body) : Body(Body) {}
  XdrPayload(std::span<const std::byte> Body, std::span<const std::byte> Tail, size_t Pad)
      : Body(Body), Tail(Tail), Pad(Pad < kMaxPad ? Pad : kMaxPad) {}

  size_t size() const { return Body.size() + Tail.size() + Pad; }

  std::span<const std::byte> Body;
  std::span<const std::byte> Tail;
  size_t Pad = 0;
};

class XdrWriter {
public:
  void u32(uint32_t V);
  void u64(uint64_t V);
  void boolean(bool V);
  void fixed(std::span<const std::byte> V);
  void opaque(std::span<const std::byte> V);
  // The payload is referenced rather than copied, so it must outlive the send.
  void opaqueTail(std::span<const std::byte> V);
  void text(std::string_view V);

  size_t size() const { return Out.size() + Tail.size() + TailPad; }
  std::span<const std::byte> bytes() const { return Out; }
  XdrPayload payload() const { return {Out, Tail, TailPad}; }
  // Both drop the tail: size() counts it, so leaving it behind would let a
  // truncated reply carry a payload the caller already decided against.
  void truncate(size_t To) {
    Tail = {};
    TailPad = 0;
    Out.resize(To);
  }

  void clear() {
    Tail = {};
    TailPad = 0;
    Out.clear();
  }

private:
  void pad(size_t Length);

  std::vector<std::byte> Out;
  std::span<const std::byte> Tail;
  size_t TailPad = 0;
};

class XdrReader {
public:
  explicit XdrReader(std::span<const std::byte> In) : In(In) {}

  uint32_t u32();
  uint64_t u64();
  bool boolean();
  std::span<const std::byte> fixed(size_t Length);
  std::span<const std::byte> opaque(size_t Limit);
  std::string text(size_t Limit);

  bool ok() const { return Ok; }
  size_t left() const { return Ok ? In.size() - At : 0; }

private:
  bool want(size_t Length);

  std::span<const std::byte> In;
  size_t At = 0;
  bool Ok = true;
};

} // namespace rail::nfs
