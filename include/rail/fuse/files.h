#pragma once

#include "rail/address-space.h"
#include "rail/io/coro.h"
#include "rail/result.h"
#include "rail/vfs/remotes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rail::fuse {

struct File {
  std::string Path;
  uint64_t Size = 0;
  bool Started = false;
  uint64_t Frontier = 0;
  uint64_t CheckedAtSize = 0;
  uint64_t Covered = 0;
  bool Failed = false;
  bool Streaming = false;
  uint64_t Page = 1u << 20;
  uint64_t StreamStart = 0;
  size_t StreamLen = 0;
  uint64_t StreamStamp = 0;
  AddressSpace Stream;
  vfs::Window Have;
  vfs::Gate Filling;
  // One opener at a time. Several readers can now share a session, and two of
  // them arriving on the same one would each open the file and each keep a
  // handle the daemon never hears about again.
  vfs::Gate Opening;
  struct Pinned {
    uint64_t Handle = 0;
    uint64_t Generation = 0;
  };
  std::vector<Pinned> OnSlot;

  int InFlightOps = 0;
  bool Doomed = false;
  bool Writable = false;
  bool Flushing = false;
  bool WriteFailed = false;
  vfs::Gate Writing;
  uint64_t WriteBase = 0;
  AddressSpace Pending;
  size_t PendingLen = 0;
  AddressSpace Draining;
  size_t DrainingLen = 0;

  bool InFlight = false;
  uint64_t SpareStart = 0;
  uint64_t SpareStamp = 0;
  AddressSpace Spare;
  Coro<Result<uint64_t>> Fetching;

  bool covers(uint64_t Offset, size_t Length, uint64_t Stamp) const {
    return StreamLen > 0 && StreamStamp == Stamp && Offset >= StreamStart && Offset + Length <= StreamStart + StreamLen;
  }
};

class Files {
public:
  uint64_t open(File F);
  File *get(uint64_t Handle);
  void close(uint64_t Handle);
  size_t live() const { return Open.size(); }
  uint64_t nextPendingOn(const std::string &Path, uint64_t After) const;

private:
  std::unordered_map<uint64_t, File> Open;
  uint64_t Next = 1;
};

} // namespace rail::fuse
