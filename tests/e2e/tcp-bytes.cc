#include "tcp-bytes.h"

#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace rail::e2e {

SocketBytes bytesOn(int Fd) {
  ::tcp_info Info{};
  ::socklen_t Len = sizeof(Info);
  if (::getsockopt(Fd, IPPROTO_TCP, TCP_INFO, &Info, &Len) != 0) return {};
  return {Info.tcpi_bytes_sent, Info.tcpi_bytes_received};
}

} // namespace rail::e2e
