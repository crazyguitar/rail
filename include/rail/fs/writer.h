#pragma once

#include "rail/fs/durability.h"
#include "rail/fs/reader.h" // FileMeta
#include "rail/io/coro.h"
#include "rail/io/uring.h"
#include "rail/result.h"

#include <filesystem>
#include <span>

namespace rail {

// Alignment a direct write needs: buffer, length and offset must all be
// multiples of it, so the caller checks its page size against this.
inline constexpr size_t kDirectAlignment = 4096;

// Writes to a temp file beside the destination and renames on commit, so a
// failed or killed transfer never leaves a partial file at the real path. The
// destructor unlinks the temp unless commit() succeeded.
class FileWriter {
public:
  // Direct asks to bypass the page cache, which is worth about 2.4x on this
  // hardware. It requires every write to be aligned, so only a transfer with no
  // copy instructions can use it.
  static Result<FileWriter> create(const std::filesystem::path &Dst, const FileMeta &Meta, Durability D, bool Direct);

  FileWriter() = default;
  FileWriter(const FileWriter &) = delete;
  FileWriter &operator=(const FileWriter &) = delete;
  FileWriter(FileWriter &&Other) noexcept;
  FileWriter &operator=(FileWriter &&Other) noexcept;
  ~FileWriter();

  Coro<Result<void>> write(std::span<const std::byte> Src, uint64_t Offset);

  // Hands the write to io_uring and returns at once. Src must stay alive until
  // the caller awaits W, which is what lets the receive loop run ahead.
  Result<void> submitWrite(Uring::Write &W, std::span<const std::byte> Src, uint64_t Offset);

  // Length rounded up to what a direct write needs, or unchanged when the file
  // is not open for direct writes. Padding is only safe when pages are written
  // whole and in order: on a delta path it would put stale bytes past a literal
  // and over data a copy has already placed there.
  size_t writeLength(size_t Length) const;

  Result<void> commit();

private:
  FileWriter(int Fd, std::filesystem::path Temp, std::filesystem::path Final, FileMeta M, Durability D, bool Direct)
      : Fd(Fd), TempPath(std::move(Temp)), FinalPath(std::move(Final)), Meta(M), Durable(D), Direct(Direct) {}

  void abandon();

  int Fd = -1;
  std::filesystem::path TempPath;
  std::filesystem::path FinalPath;
  FileMeta Meta;
  Durability Durable = Durability::PageCache;
  bool Direct = false;
  bool Committed = false;
};

} // namespace rail
