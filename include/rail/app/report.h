#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace rail {

// LiteralBytes and MatchedBytes are what the delta end-to-end tests assert on.
// They are counted from real behaviour, never estimated: without that, a test
// asserting "the delta worked" passes against a dumb whole-file copy.
struct Report {
  uint64_t Files = 0;
  uint64_t FileSize = 0;
  uint64_t LiteralBytes = 0;
  uint64_t MatchedBytes = 0;
  uint64_t HashHits = 0;
  uint64_t FalseAlarms = 0;
  std::chrono::nanoseconds ScanTime{0};
  std::chrono::nanoseconds TransferTime{0};
  std::string Backend;
  // Active RDMA ports the transfer could stripe over. Empty means it ran over
  // sockets, which is the difference between gigabytes and megabytes a second.
  std::string Rails;
  bool DeltaUsed = false;

  std::string toJson() const;
};

} // namespace rail
