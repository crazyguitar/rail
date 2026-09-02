#include "rail/transport/data-channel.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rail {

std::string sshLocalAddress() {
  const char *Conn = ::getenv("SSH_CONNECTION");
  if (!Conn) return "127.0.0.1";
  std::string S(Conn);
  size_t Start = 0;
  for (int Field = 0; Field < 2; Field++) {
    Start = S.find(' ', Start);
    if (Start == std::string::npos) return "127.0.0.1";
    Start++;
  }
  const size_t End = S.find(' ', Start);
  return S.substr(Start, End == std::string::npos ? End : End - Start);
}

namespace {

struct Socket {
  int Fd;

  explicit Socket(int Fd) : Fd(Fd) {}
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  ~Socket() {
    if (Fd >= 0) ::close(Fd);
  }
};

struct Resolved {
  ::addrinfo *List = nullptr;

  Resolved() = default;
  Resolved(const Resolved &) = delete;
  Resolved &operator=(const Resolved &) = delete;
  ~Resolved() {
    if (List) ::freeaddrinfo(List);
  }
};

std::string textOf(const ::sockaddr_storage &Addr) {
  const void *Raw = Addr.ss_family == AF_INET6 ? static_cast<const void *>(&reinterpret_cast<const ::sockaddr_in6 *>(&Addr)->sin6_addr)
                                               : static_cast<const void *>(&reinterpret_cast<const ::sockaddr_in *>(&Addr)->sin_addr);

  char Text[INET6_ADDRSTRLEN] = {};
  return ::inet_ntop(Addr.ss_family, Raw, Text, sizeof(Text)) ? Text : std::string{};
}

// Which of this machine's addresses a packet to Peer would leave from. Asked of
// the routing table rather than guessed: connecting a udp socket sends nothing
// but fills in the source address the kernel would pick.
std::string routedAddress(const std::string &Peer) {
  ::addrinfo Want{};
  Want.ai_family = AF_UNSPEC;
  Want.ai_socktype = SOCK_DGRAM;

  Resolved Found;
  if (::getaddrinfo(Peer.c_str(), "9", &Want, &Found.List) != 0 || !Found.List) return {};

  const Socket Probe(::socket(Found.List->ai_family, SOCK_DGRAM, 0));
  if (Probe.Fd < 0) return {};
  if (::connect(Probe.Fd, Found.List->ai_addr, Found.List->ai_addrlen) != 0) return {};

  ::sockaddr_storage Mine{};
  ::socklen_t Len = sizeof(Mine);
  if (::getsockname(Probe.Fd, reinterpret_cast<::sockaddr *>(&Mine), &Len) != 0) return {};

  return textOf(Mine);
}

} // namespace

// Where the peer should dial back. Inside an ssh session SSH_CONNECTION names
// it; a pull runs the listening side outside one, and answering 127.0.0.1 there
// sends the peer to its own loopback. Nothing is a better answer than that: the
// far side falls back to the address ssh already told it we came from.
std::string reachableAddress(const std::string &Peer) {
  if (::getenv("SSH_CONNECTION")) return sshLocalAddress();
  return routedAddress(Peer);
}

std::unique_ptr<DataChannel> makeDataChannel(const std::string &Backend, size_t PageCount, size_t PageSize, std::string Host) {
  if (Backend == "tcp") return makeTcpDataChannel(PageCount, PageSize, std::move(Host));
#ifdef RAIL_HAVE_RDMA
  if (Backend == "rdma") return makeRdmaDataChannel(PageCount, PageSize);
#endif
  return nullptr;
}

} // namespace rail
