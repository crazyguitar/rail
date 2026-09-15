#pragma once

#include <cstdint>

namespace rail::e2e {

// Bytes a TCP socket carried each way. Its own translation unit: the struct
// lives in <linux/tcp.h>, which cannot share a file with <netinet/tcp.h>.
struct SocketBytes {
  uint64_t Sent = 0;
  uint64_t Received = 0;
  bool operator==(const SocketBytes &) const = default;
};

SocketBytes bytesOn(int Fd);

} // namespace rail::e2e
