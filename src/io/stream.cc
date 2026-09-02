#include "rail/io/stream.h"

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace rail {

Result<void> setNonBlocking(int Fd) {
  const int Flags = ::fcntl(Fd, F_GETFL, 0);
  if (Flags < 0) return failErrno("fcntl(F_GETFL)");
  if (::fcntl(Fd, F_SETFL, Flags | O_NONBLOCK) < 0) return failErrno("fcntl(F_SETFL)");
  return {};
}

Stream::Stream(int Fd) : Fd(Fd) {}

Stream::Stream(Stream &&Other) noexcept : Fd(std::exchange(Other.Fd, -1)) {}

Stream &Stream::operator=(Stream &&Other) noexcept {
  if (this != &Other) {
    close();
    Fd = std::exchange(Other.Fd, -1);
  }
  return *this;
}

Stream::~Stream() { close(); }

void Stream::close() {
  if (Fd >= 0) {
    Loop::get().forget(Fd);
    ::close(Fd);
    Fd = -1;
  }
}

Result<Stream> Stream::connect(const std::string &Host, uint16_t Port) {
  addrinfo Hints{};
  Hints.ai_family = AF_INET;
  Hints.ai_socktype = SOCK_STREAM;

  addrinfo *Info = nullptr;
  const std::string Service = std::to_string(Port);
  if (::getaddrinfo(Host.c_str(), Service.c_str(), &Hints, &Info) != 0) return failMessage("getaddrinfo " + Host);

  int Sock = ::socket(Info->ai_family, SOCK_STREAM, 0);
  if (Sock < 0) {
    ::freeaddrinfo(Info);
    return failErrno("socket");
  }

  const int Rc = ::connect(Sock, Info->ai_addr, Info->ai_addrlen);
  ::freeaddrinfo(Info);
  if (Rc < 0) {
    ::close(Sock);
    return failErrno("connect " + Host);
  }

  const int One = 1;
  ::setsockopt(Sock, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));
  if (auto R = setNonBlocking(Sock); !R) {
    ::close(Sock);
    return std::unexpected(R.error());
  }
  return Stream(Sock);
}

Result<Stream> Stream::listenOn(uint16_t Port, bool LoopbackOnly, bool Shared) {
  int Sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (Sock < 0) return failErrno("socket");

  const int One = 1;
  ::setsockopt(Sock, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));

  // Refused rather than ignored: without it the second thread's bind fails
  // with EADDRINUSE and the daemon would come up with fewer threads than it
  // was asked for, which reads as the machine being slow.
  if (Shared && ::setsockopt(Sock, SOL_SOCKET, SO_REUSEPORT, &One, sizeof(One)) < 0) {
    ::close(Sock);
    return failErrno("SO_REUSEPORT");
  }

  sockaddr_in Addr{};
  Addr.sin_family = AF_INET;
  Addr.sin_addr.s_addr = htonl(LoopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
  Addr.sin_port = htons(Port);
  if (::bind(Sock, reinterpret_cast<sockaddr *>(&Addr), sizeof(Addr)) < 0) {
    ::close(Sock);
    return failErrno("bind");
  }
  if (::listen(Sock, 8) < 0) {
    ::close(Sock);
    return failErrno("listen");
  }
  if (auto R = setNonBlocking(Sock); !R) {
    ::close(Sock);
    return std::unexpected(R.error());
  }
  return Stream(Sock);
}

Result<uint16_t> Stream::localPort() const {
  sockaddr_in Addr{};
  socklen_t Len = sizeof(Addr);
  if (::getsockname(Fd, reinterpret_cast<sockaddr *>(&Addr), &Len) < 0) return failErrno("getsockname");
  return ntohs(Addr.sin_port);
}

Coro<Result<size_t>> Stream::read(std::span<std::byte> Dst) {
  for (;;) {
    const ssize_t N = ::read(Fd, Dst.data(), Dst.size());
    if (N >= 0) co_return static_cast<size_t>(N);
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await WaitFor{Fd, EPOLLIN};
      continue;
    }
    co_return failErrno("read");
  }
}

Coro<Result<void>> Stream::readExact(std::span<std::byte> Dst) {
  size_t Done = 0;
  while (Done < Dst.size()) {
    auto N = co_await read(Dst.subspan(Done));
    if (!N) co_return std::unexpected(N.error());
    if (*N == 0) co_return failMessage("peer closed");
    Done += *N;
  }
  co_return Result<void>{};
}

Coro<Result<void>> Stream::writeAll(std::span<const std::byte> Src) {
  size_t Done = 0;
  while (Done < Src.size()) {
    const ssize_t N = ::write(Fd, Src.data() + Done, Src.size() - Done);
    if (N > 0) {
      Done += static_cast<size_t>(N);
      continue;
    }
    if (N < 0 && errno == EINTR) continue;
    if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await WaitFor{Fd, EPOLLOUT};
      continue;
    }
    co_return failErrno("write");
  }
  co_return Result<void>{};
}

Coro<Result<void>> Stream::writeAll(std::span<const std::byte> First, std::span<const std::byte> Second) {
  ::iovec Parts[2];
  size_t Done = 0;
  const size_t Total = First.size() + Second.size();

  while (Done < Total) {
    // Rebuilt each turn: a partial write can land anywhere, including inside
    // the first part.
    const size_t FirstLeft = Done < First.size() ? First.size() - Done : 0;
    const size_t SecondFrom = Done < First.size() ? 0 : Done - First.size();

    int Count = 0;
    if (FirstLeft > 0) Parts[Count++] = {const_cast<std::byte *>(First.data()) + Done, FirstLeft};
    if (SecondFrom < Second.size()) Parts[Count++] = {const_cast<std::byte *>(Second.data()) + SecondFrom, Second.size() - SecondFrom};

    const ssize_t N = ::writev(Fd, Parts, Count);
    if (N > 0) {
      Done += static_cast<size_t>(N);
      continue;
    }
    if (N < 0 && errno == EINTR) continue;
    if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await WaitFor{Fd, EPOLLOUT};
      continue;
    }
    co_return failErrno("writev");
  }
  co_return Result<void>{};
}

bool peerGone(int Fd) {
  ::pollfd Watch{Fd, POLLIN | POLLRDHUP, 0};
  if (::poll(&Watch, 1, 0) <= 0) return false;
  if (Watch.revents & (POLLHUP | POLLERR | POLLRDHUP)) return true;

  // A pipe answers ENOTSOCK rather than zero, which is why the poll above is
  // what actually catches an ssh helper dying.
  char Peek = 0;
  return ::recv(Fd, &Peek, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

Result<Stream> Stream::tryAccept() {
  for (;;) {
    sockaddr_storage Addr{};
    socklen_t Len = sizeof(Addr);
    const int Client = ::accept(Fd, reinterpret_cast<sockaddr *>(&Addr), &Len);

    if (Client < 0 && errno == EINTR) continue;
    if (Client < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Stream();
    if (Client < 0) return failErrno("accept");

    const int One = 1;
    ::setsockopt(Client, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));
    if (auto R = setNonBlocking(Client); !R) {
      ::close(Client);
      return std::unexpected(R.error());
    }
    return Stream(Client);
  }
}

Coro<Result<Stream>> Stream::accept() {
  for (;;) {
    sockaddr_storage Addr{};
    socklen_t Len = sizeof(Addr);
    const int Client = ::accept(Fd, reinterpret_cast<sockaddr *>(&Addr), &Len);
    if (Client >= 0) {
      const int One = 1;
      ::setsockopt(Client, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));
      if (auto R = setNonBlocking(Client); !R) {
        ::close(Client);
        co_return std::unexpected(R.error());
      }
      co_return Stream(Client);
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await WaitFor{Fd, EPOLLIN};
      continue;
    }
    co_return failErrno("accept");
  }
}

} // namespace rail
