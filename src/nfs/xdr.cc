#include "rail/nfs/xdr.h"

namespace rail::nfs {

namespace {

size_t padding(size_t Length) { return (4 - (Length % 4)) % 4; }

} // namespace

void XdrWriter::pad(size_t Length) {
  for (size_t I = 0; I < padding(Length); I++) Out.push_back(std::byte{0});
}

void XdrWriter::u32(uint32_t V) {
  for (int Shift = 24; Shift >= 0; Shift -= 8) Out.push_back(static_cast<std::byte>((V >> Shift) & 0xff));
}

void XdrWriter::u64(uint64_t V) {
  u32(static_cast<uint32_t>(V >> 32));
  u32(static_cast<uint32_t>(V & 0xffffffff));
}

void XdrWriter::boolean(bool V) { u32(V ? 1 : 0); }

void XdrWriter::fixed(std::span<const std::byte> V) {
  Out.insert(Out.end(), V.begin(), V.end());
  pad(V.size());
}

void XdrWriter::opaque(std::span<const std::byte> V) {
  u32(static_cast<uint32_t>(V.size()));
  fixed(V);
}

void XdrWriter::opaqueTail(std::span<const std::byte> V) {
  u32(static_cast<uint32_t>(V.size()));
  Tail = V;
  TailPad = padding(V.size());
}

void XdrWriter::text(std::string_view V) { opaque({reinterpret_cast<const std::byte *>(V.data()), V.size()}); }

bool XdrReader::want(size_t Length) {
  if (!Ok) return false;
  if (In.size() - At < Length) {
    Ok = false;
    return false;
  }
  return true;
}

uint32_t XdrReader::u32() {
  if (!want(4)) return 0;
  uint32_t V = 0;
  for (size_t I = 0; I < 4; I++) V = (V << 8) | static_cast<uint32_t>(In[At + I]);
  At += 4;
  return V;
}

uint64_t XdrReader::u64() {
  const uint64_t High = u32();
  const uint64_t Low = u32();
  return (High << 32) | Low;
}

bool XdrReader::boolean() { return u32() != 0; }

std::span<const std::byte> XdrReader::fixed(size_t Length) {
  const size_t Total = Length + padding(Length);
  if (!want(Total)) return {};
  auto View = In.subspan(At, Length);
  At += Total;
  return View;
}

std::span<const std::byte> XdrReader::opaque(size_t Limit) {
  const uint32_t Length = u32();
  if (!Ok) return {};
  if (Length > Limit) {
    Ok = false;
    return {};
  }
  return fixed(Length);
}

std::string XdrReader::text(size_t Limit) {
  auto View = opaque(Limit);
  if (!Ok) return {};
  return std::string(reinterpret_cast<const char *>(View.data()), View.size());
}

} // namespace rail::nfs
