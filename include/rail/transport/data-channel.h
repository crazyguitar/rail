#pragma once

#include "rail/io/coro.h"
#include "rail/page-pool.h"
#include "rail/result.h"

#include <memory>
#include <string>

namespace rail {

struct DataChannelTraits {
  bool IsRdma = false;
  // rsync caps blocks at 128 KiB, tuned for slow links. An RDMA channel
  // reports more, because small transfers waste RDMA and a 100 GB file at
  // 128 KiB would carry a signature of tens of megabytes.
  uint32_t MaxBlockSize = 1u << 17;
  // Pages the sender may keep in flight. A byte-stream channel must say 1:
  // concurrent sends interleave on the wire and corrupt the payload. A
  // message-oriented channel tags each page and can carry many at once.
  size_t MaxInFlight = 1;
  const char *Name = "";
};

// Moves file payload between a Page the caller owns and a remote location.
// Never allocates: the pool belongs to the channel, so an RDMA backend can
// hand out pre-registered memory and nothing above this layer has to know.
class DataChannel {
public:
  virtual ~DataChannel() = default;

  virtual DataChannelTraits traits() const = 0;
  virtual PagePool &pool() = 0;

  // Server side. Returns the endpoint blob the client needs, which travels
  // back over the ssh pipe inside HelloAck.
  virtual Coro<Result<std::string>> listen() = 0;

  // Client side. Endpoint is whatever listen() produced.
  virtual Coro<Result<void>> connect(const std::string &Endpoint) = 0;

  // Server side, after the client has connected.
  virtual Coro<Result<void>> acceptPeer() = 0;

  // A sync transfer only ever sends from the side that connected, so the
  // listening side needs nothing back. A file service answers reads, so both
  // ends have to be able to send. Transports whose accepted connection is
  // already bidirectional leave these alone.
  virtual Coro<Result<std::string>> localEndpoint() { co_return std::string{}; }
  virtual Coro<Result<void>> attachPeer(const std::string &Endpoint) {
    (void)Endpoint;
    co_return Result<void>{};
  }

  virtual Result<void> prepare() { return {}; }

  // The descriptor that says the peer is still there, normally the control
  // channel's. A transport that waits for the peer to act has to watch this
  // too, or a peer that dies while we wait for it is never noticed.
  virtual void watch(int Fd) { (void)Fd; }

  // Whether the side that dialled has to send its own endpoint back. A queue
  // pair needs both ends before either can use it; a connection that is
  // already bidirectional once accepted does not, and asking for one it never
  // sends leaves the listener waiting for a message that is not coming.
  virtual bool wantsPeerEndpoint() const { return false; }

  virtual Coro<Result<void>> send(Page &Buf, uint64_t Key) = 0;
  virtual Coro<Result<void>> recv(Page &Buf, uint64_t Key, size_t Length) = 0;

  virtual void close() = 0;
};

// The client reaches the server through an ssh alias, which is not a DNS name
// and cannot be resolved by either side. sshd sets SSH_CONNECTION to
// "<client-ip> <client-port> <server-ip> <server-port>", so the server can
// report the address the client actually used to reach it.
std::string sshLocalAddress();

// The address Peer can reach this machine on, for the side that listens.
std::string reachableAddress(const std::string &Peer);

// Host is the peer's name for the client side and empty for the server.
std::unique_ptr<DataChannel> makeTcpDataChannel(size_t PageCount, size_t PageSize, std::string Host);

#ifdef RAIL_HAVE_RDMA

// Native verbs. One-sided writes straight into the page the peer named, with
// the slot as immediate data so its completion queue is the only notification.
std::unique_ptr<DataChannel> makeRdmaDataChannel(size_t PageCount, size_t PageSize);
#endif

std::unique_ptr<DataChannel> makeDataChannel(const std::string &Backend, size_t PageCount, size_t PageSize, std::string Host);

} // namespace rail
