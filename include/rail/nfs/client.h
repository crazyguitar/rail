#pragma once

#include "rail/io/coro.h"
#include "rail/io/stream.h"
#include "rail/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rail::nfs {

using Handle = std::vector<std::byte>;

struct Attrs {
  bool Directory = false;
  uint64_t Size = 0;
  uint64_t FileId = 0;
};

struct DirEntry {
  std::string Name;
  Handle File;
  Attrs Info;
};

struct Chunk {
  size_t Bytes = 0;
  bool Eof = false;
  uint64_t Offset = 0;
  std::span<std::byte> Into;
};

class NfsClient {
public:
  static Result<NfsClient> connect(const std::string &Host, uint16_t Port);

  NfsClient(const NfsClient &) = delete;
  NfsClient &operator=(const NfsClient &) = delete;
  NfsClient(NfsClient &&) noexcept = default;
  NfsClient &operator=(NfsClient &&) noexcept = default;

  Coro<Result<Handle>> mountRoot(const std::string &Path);
  Coro<Result<Attrs>> getAttr(const Handle &File);
  Coro<Result<Handle>> lookup(const Handle &Directory, const std::string &Name);
  Coro<Result<std::vector<DirEntry>>> listDirectory(const Handle &Directory);

  Coro<Result<Chunk>> read(const Handle &File, uint64_t Offset, std::span<std::byte> Into);

  // The destination is named when the read is sent, because a server that
  // answers several calls at once replies in whatever order they finish, and
  // the reply says which one it is.
  Coro<Result<void>> submitRead(const Handle &File, uint64_t Offset, std::span<std::byte> Into);
  Coro<Result<Chunk>> collectRead();

  size_t outstanding() const { return Pending.size(); }

private:
  explicit NfsClient(Stream S) : S(std::move(S)) {}

  Coro<Result<std::vector<std::byte>>> callNfs(uint32_t Procedure, std::span<const std::byte> Args);

  struct Asked {
    uint64_t Offset = 0;
    std::span<std::byte> Into;
  };

  Stream S;
  uint32_t Xid = 0;
  std::unordered_map<uint32_t, Asked> Pending;
};

} // namespace rail::nfs
