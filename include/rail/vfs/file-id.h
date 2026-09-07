#pragma once

#include "rail/proto/message.h"

#include <cstdint>

namespace rail::vfs {

// What a mount reports as a file's number. The peer's device and inode name a
// file whatever it is called, so a rename keeps it and two names for one file
// share it. Inode numbers repeat across a mount point, hence the device.
inline uint64_t fileIdOf(const proto::FileAttrs &A) {
  uint64_t Id = A.Ino ^ (A.Dev * 0x9E3779B97F4A7C15ULL);
  Id ^= Id >> 33;
  Id *= 0xFF51AFD7ED558CCDULL;
  Id ^= Id >> 33;
  return Id ? Id : 1;
}

} // namespace rail::vfs
