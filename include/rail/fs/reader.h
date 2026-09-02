#pragma once

#include "rail/io/coro.h"
#include "rail/io/uring.h"
#include "rail/result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <sys/stat.h>

namespace rail {

struct FileMeta {
  uint64_t Size = 0;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
};

// Reads into a caller-supplied buffer and never allocates, so file data can
// land directly in registered memory. Milestone 3 swaps pread for io_uring
// behind this same signature.
// Direct reads bypass the page cache, which this hardware serves ten times
// faster, but every offset and length must be aligned. Only a caller that
// reads whole pages in order can promise that; a rolling scan or a basis read
// cannot, and asks for Buffered.
enum class Access { Buffered, Direct };

class FileReader {
public:
  static Result<FileReader> open(const std::filesystem::path &Path, Access A = Access::Buffered);

  FileReader() = default;
  FileReader(const FileReader &) = delete;
  FileReader &operator=(const FileReader &) = delete;
  FileReader(FileReader &&Other) noexcept;
  FileReader &operator=(FileReader &&Other) noexcept;
  ~FileReader();

  Coro<Result<size_t>> read(std::span<std::byte> Dst, uint64_t Offset);

  // Starts a read without waiting, so several can be outstanding at once. The
  // device serves a queue far faster than one request at a time. Dst must stay
  // alive until awaitRead returns.
  Result<void> submitRead(Uring::Read &R, std::span<std::byte> Dst, uint64_t Offset);
  Coro<Result<size_t>> awaitRead(Uring::Read &R);

  // True when reads must be aligned, which the caller arranges by rounding the
  // length up; the file simply ends early on the last page.
  bool direct() const;
  const FileMeta &meta() const { return Meta; }
  bool valid() const { return Fd >= 0; }

private:
  FileReader(int Fd, FileMeta M) : Fd(Fd), Meta(M) {}

  int Fd = -1;
  FileMeta Meta;
};

} // namespace rail
