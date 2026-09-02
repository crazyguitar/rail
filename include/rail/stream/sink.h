#pragma once

#include "rail/address-space.h"
#include "rail/fs/reader.h"
#include "rail/fs/writer.h"
#include "rail/io/coro.h"
#include "rail/io/uring.h"
#include "rail/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace rail {

class PageSource {
public:
  virtual ~PageSource() = default;

  virtual bool direct() const = 0;
  virtual Result<void> submitRead(Uring::Read &R, std::span<std::byte> Dst, uint64_t Offset) = 0;
  virtual Coro<Result<size_t>> awaitRead(Uring::Read &R) = 0;

  // Null when the source has no registered memory of its own for these bytes,
  // which leaves the transfer on the pooled path and the copy that goes with it.
  virtual Page *sending(uint64_t Offset, size_t Length) {
    (void)Offset;
    (void)Length;
    return nullptr;
  }
};

class PageSink {
public:
  virtual ~PageSink() = default;

  virtual size_t writeLength(size_t Length) const = 0;
  virtual Result<void> submitWrite(Uring::Write &W, std::span<const std::byte> Src, uint64_t Offset) = 0;
  virtual Coro<Result<void>> awaitWrite(Uring::Write &W) = 0;

  // Null when the sink has no registered memory of its own for these bytes,
  // which leaves the transfer on the pooled path and the copy that goes with it.
  virtual Page *landing(uint64_t Offset, size_t Length) {
    (void)Offset;
    (void)Length;
    return nullptr;
  }
};

class FileSource final : public PageSource {
public:
  explicit FileSource(FileReader &R) : R(R) {}

  bool direct() const override { return R.direct(); }
  Result<void> submitRead(Uring::Read &Op, std::span<std::byte> Dst, uint64_t Offset) override { return R.submitRead(Op, Dst, Offset); }
  Coro<Result<size_t>> awaitRead(Uring::Read &Op) override { return R.awaitRead(Op); }

private:
  FileReader &R;
};

class FileSink final : public PageSink {
public:
  explicit FileSink(FileWriter &W) : W(W) {}

  size_t writeLength(size_t Length) const override { return W.writeLength(Length); }
  Result<void> submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) override { return W.submitWrite(Op, Src, Offset); }
  Coro<Result<void>> awaitWrite(Uring::Write &Op) override;

private:
  FileWriter &W;
};

class DescriptorSink final : public PageSink {
public:
  explicit DescriptorSink(int Fd) : Fd(Fd) {}

  size_t writeLength(size_t Length) const override { return Length; }
  Result<void> submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) override;
  Coro<Result<void>> awaitWrite(Uring::Write &Op) override;

private:
  int Fd = -1;
};

class BufferSource final : public PageSource {
public:
  BufferSource(std::span<const std::byte> From, uint64_t Base) : From(From), Base(Base) {}

  bool direct() const override { return false; }
  Result<void> submitRead(Uring::Read &Op, std::span<std::byte> Dst, uint64_t Offset) override;
  Coro<Result<size_t>> awaitRead(Uring::Read &Op) override;

private:
  std::span<const std::byte> From;
  uint64_t Base = 0;
};

class AddressSpaceSource final : public PageSource {
public:
  explicit AddressSpaceSource(AddressSpace &Space) : Space(Space) {}

  bool direct() const override { return false; }
  Result<void> submitRead(Uring::Read &Op, std::span<std::byte> Dst, uint64_t Offset) override;
  Coro<Result<size_t>> awaitRead(Uring::Read &Op) override;
  Page *sending(uint64_t Offset, size_t Length) override;

private:
  AddressSpace &Space;
};

class AddressSpaceSink final : public PageSink {
public:
  explicit AddressSpaceSink(AddressSpace &Space) : Space(Space) {}

  size_t writeLength(size_t Length) const override { return Length; }
  Result<void> submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) override;
  Coro<Result<void>> awaitWrite(Uring::Write &Op) override;
  Page *landing(uint64_t Offset, size_t Length) override;

private:
  AddressSpace &Space;
};

class BufferSink final : public PageSink {
public:
  BufferSink(std::span<std::byte> Into, uint64_t Base) : Into(Into), Base(Base) {}

  size_t writeLength(size_t Length) const override { return Length; }
  Result<void> submitWrite(Uring::Write &Op, std::span<const std::byte> Src, uint64_t Offset) override;
  Coro<Result<void>> awaitWrite(Uring::Write &Op) override;

private:
  std::span<std::byte> Into;
  uint64_t Base = 0;
};

} // namespace rail
