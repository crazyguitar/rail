#include "rail/fs/writer.h"

#define _GNU_SOURCE

#include <atomic>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

namespace rail {

namespace {

// The pool is already aligned and pages are multiples of the alignment, so
// only the final short page needs rounding up; commit() truncates the padding
// away.
uint64_t roundUp(uint64_t N) { return (N + kDirectAlignment - 1) & ~(kDirectAlignment - 1); }

// Unique within this process, so two sessions or two threads staging the same
// destination never share one temp - which let one truncate the other's bytes
// and rename a spliced file into place.
uint64_t nextTempId() {
  static std::atomic<uint64_t> Counter{0};
  return Counter.fetch_add(1, std::memory_order_relaxed);
}

// The page cache costs a copy at about 2.7 GB/s on this hardware where the
// same device takes direct writes at 6.4. Falls back when the filesystem
// refuses O_DIRECT, which some do.
int openDestination(const std::filesystem::path &Temp, bool Direct) {
  const int Flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
  if (Direct)
    if (const int Fd = ::open(Temp.c_str(), Flags | O_DIRECT, 0600); Fd >= 0) return Fd;
  return ::open(Temp.c_str(), Flags, 0600);
}

} // namespace

Result<FileWriter> FileWriter::create(const std::filesystem::path &Dst, const FileMeta &Meta, Durability D, bool Direct) {
  const std::filesystem::path Dir = Dst.parent_path().empty() ? std::filesystem::path(".") : Dst.parent_path();
  const std::filesystem::path Temp = Dir / std::format(".rail.tmp.{}.{}.{}", ::getpid(), nextTempId(), Dst.filename().string());

  const int Fd = openDestination(Temp, Direct);
  if (Fd < 0) return failErrno("open " + Temp.string());

  // The open falls back when the filesystem refuses O_DIRECT, so what was
  // asked for is not necessarily what was granted.
  const bool Granted = Direct && (::fcntl(Fd, F_GETFL) & O_DIRECT) != 0;

  return FileWriter(Fd, Temp, Dst, Meta, D, Granted);
}

FileWriter::FileWriter(FileWriter &&Other) noexcept
    : Fd(std::exchange(Other.Fd, -1)), TempPath(std::move(Other.TempPath)), FinalPath(std::move(Other.FinalPath)), Meta(Other.Meta),
      Durable(Other.Durable), Direct(Other.Direct), Committed(std::exchange(Other.Committed, true)) {}

FileWriter &FileWriter::operator=(FileWriter &&Other) noexcept {
  if (this != &Other) {
    abandon();
    Fd = std::exchange(Other.Fd, -1);
    TempPath = std::move(Other.TempPath);
    FinalPath = std::move(Other.FinalPath);
    Meta = Other.Meta;
    Durable = Other.Durable;
    Direct = Other.Direct;
    Committed = std::exchange(Other.Committed, true);
  }
  return *this;
}

FileWriter::~FileWriter() { abandon(); }

void FileWriter::abandon() {
  if (Fd >= 0) {
    ::close(Fd);
    Fd = -1;
  }
  if (!Committed && !TempPath.empty()) {
    std::error_code Ignored;
    std::filesystem::remove(TempPath, Ignored);
  }
}

Coro<Result<void>> FileWriter::write(std::span<const std::byte> Src, uint64_t Offset) {
  size_t Done = 0;
  while (Done < Src.size()) {
    const ssize_t N = ::pwrite(Fd, Src.data() + Done, Src.size() - Done, static_cast<off_t>(Offset + Done));
    if (N < 0) {
      if (errno == EINTR) continue;
      co_return failErrno("pwrite");
    }
    Done += static_cast<size_t>(N);
  }
  co_return Result<void>{};
}

Result<void> FileWriter::submitWrite(Uring::Write &W, std::span<const std::byte> Src, uint64_t Offset) {
  W.Fd = Fd;
  W.Src = Src;
  W.Offset = Offset;
  return Uring::get().submit(W);
}

size_t FileWriter::writeLength(size_t Length) const { return Direct ? static_cast<size_t>(roundUp(Length)) : Length; }

Result<void> FileWriter::commit() {
  if (::ftruncate(Fd, static_cast<off_t>(Meta.Size)) < 0) return failErrno("ftruncate");
  if (Durable == Durability::Fsync && ::fsync(Fd) < 0) return failErrno("fsync");
  if (Meta.Mode != 0 && ::fchmod(Fd, Meta.Mode) < 0) return failErrno("fchmod");

  if (Meta.Mtime != 0) {
    timespec Times[2];
    Times[0].tv_sec = Meta.Mtime;
    Times[0].tv_nsec = 0;
    Times[1].tv_sec = Meta.Mtime;
    Times[1].tv_nsec = 0;
    if (::futimens(Fd, Times) < 0) return failErrno("futimens");
  }

  ::close(Fd);
  Fd = -1;

  std::error_code Ec;
  std::filesystem::rename(TempPath, FinalPath, Ec);
  if (Ec) return fail(Ec, "rename to " + FinalPath.string());

  Committed = true;
  return {};
}

} // namespace rail
