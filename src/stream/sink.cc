#include "rail/stream/sink.h"

#include <algorithm>
#include <cstring>

namespace rail {

Coro<Result<void>> FileSink::awaitWrite(Uring::Write &Op) {
  auto Landed = co_await Uring::get().await(Op);
  if (!Landed) co_return std::unexpected(Landed.error());
  co_return Result<void>{};
}

Result<void> DescriptorSink::submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) {
  Op.Fd = Fd;
  Op.Src = Src;
  Op.Offset = Offset;
  return Uring::get().submit(Op);
}

Coro<Result<void>> DescriptorSink::awaitWrite(Uring::Write &Op) {
  auto Landed = co_await Uring::get().await(Op);
  if (!Landed) co_return std::unexpected(Landed.error());
  co_return Result<void>{};
}

Result<void> BufferSource::submitRead(Uring::Read &Op, std::span<std::byte> Dst, uint64_t Offset) {
  if (Offset < Base) return failMessage("a streamed read started before the buffer");
  const uint64_t At = Offset - Base;
  if (At > From.size()) return failMessage("a streamed read started past the buffer");

  const size_t Take = std::min<size_t>(Dst.size(), static_cast<size_t>(From.size() - At));
  std::memcpy(Dst.data(), From.data() + At, Take);
  Op.Result = static_cast<int>(Take);
  Op.Done = true;
  return Result<void>{};
}

Coro<Result<size_t>> BufferSource::awaitRead(Uring::Read &Op) { co_return static_cast<size_t>(Op.Result); }

Page *AddressSpaceSource::sending(uint64_t Offset, size_t Length) {
  const auto Where = Space.at(Offset, Length);
  if (!Where.Where || Where.Offset != 0 || Where.Length != Length) return nullptr;
  if (!Where.Where->region()) return nullptr;
  return Where.Where;
}

Result<void> AddressSpaceSource::submitRead(Uring::Read &Op, std::span<std::byte> Dst, uint64_t Offset) {
  const auto Where = Space.at(Offset, Dst.size());
  if (!Where.Where) return failMessage("a streamed read started outside the address space");
  const size_t Take = std::min(Dst.size(), Where.Length);
  std::memcpy(Dst.data(), Where.Where->bytes() + Where.Offset, Take);
  Op.Result = static_cast<int>(Take);
  Op.Done = true;
  return Result<void>{};
}

Coro<Result<size_t>> AddressSpaceSource::awaitRead(Uring::Read &Op) { co_return static_cast<size_t>(Op.Result); }

Page *AddressSpaceSink::landing(uint64_t Offset, size_t Length) {
  const auto Where = Space.at(Offset, Length);
  if (!Where.Where || Where.Offset != 0 || Where.Length != Length) return nullptr;
  if (!Where.Where->region()) return nullptr;
  return Where.Where;
}

Result<void> AddressSpaceSink::submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) {
  const auto Where = Space.at(Offset, Src.size());
  if (!Where.Where || Where.Length != Src.size()) return failMessage("a streamed write landed outside the address space");
  std::memcpy(Where.Where->bytes() + Where.Offset, Src.data(), Src.size());
  Op.Result = static_cast<int>(Src.size());
  Op.Done = true;
  return {};
}

Coro<Result<void>> AddressSpaceSink::awaitWrite(Uring::Write &Op) {
  (void)Op;
  co_return Result<void>{};
}

Result<void> BufferSink::submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) {
  if (Offset < Base) return failMessage("a streamed write landed before the buffer");
  const uint64_t At = Offset - Base;
  if (At > Into.size()) return failMessage("a streamed write landed past the buffer");

  if (Src.size() > Into.size() - At) return failMessage("a streamed write ran past the end of the buffer");

  const size_t Put = Src.size();
  std::memcpy(Into.data() + At, Src.data(), Put);
  Op.Result = static_cast<int>(Put);
  Op.Done = true;
  return Result<void>{};
}

Coro<Result<void>> BufferSink::awaitWrite(Uring::Write &) { co_return Result<void>{}; }

} // namespace rail
