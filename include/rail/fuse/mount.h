#pragma once

#include "rail/io/coro.h"
#include "rail/result.h"
#include "rail/vfs/remotes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rail::fuse {

struct MountOptions {
  vfs::RemoteOptions Remote;
  std::filesystem::path MountPoint;
  uint64_t NullSize = 0;
  double AttrTimeout = 20;
  double EntryTimeout = 20;
  uint32_t MaxBackground = 64;
  uint32_t MaxRead = 0;
  uint64_t StreamAfter = 16u << 20;
  uint64_t StreamChunk = 64u << 20;
  uint64_t WriteChunk = 64u << 20;
  // Zero asks for one shard per core, bounded. A write is copied and hashed by
  // the shard that owns the file, so too few shards is a cpu wall well below
  // the fabric, and too many is an unmount that crawls.
  size_t Threads = 0;
  bool Verbose = false;
  bool DirectIo = true;
  bool Splice = true;
  std::vector<std::string> Extra;
};

Coro<Result<void>> serveMount(const MountOptions &Opts);

} // namespace rail::fuse
