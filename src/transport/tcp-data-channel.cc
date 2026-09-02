#include "rail/io/loop.h"
#include "rail/io/stream.h"
#include "rail/transport/data-channel.h"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <span>
#include <unordered_map>

namespace rail {

namespace {

// One frame reaches the wire whole. A second sender waits its turn rather
// than interleaving with the first.
class Order {
public:
  auto take() {
    struct Awaiter {
      Order *G;
      std::coroutine_handle<> Queued{};

      bool await_ready() const noexcept { return !G->Held; }
      void await_suspend(std::coroutine_handle<> H) {
        Queued = H;
        G->Waiting.push_back(H);
      }
      void await_resume() noexcept {
        Queued = {};
        G->Held = true;
      }
      ~Awaiter() {
        if (Queued) std::erase(G->Waiting, Queued);
      }
    };
    return Awaiter{this};
  }

  void give() {
    if (Waiting.empty()) {
      Held = false;
      return;
    }
    auto H = Waiting.front();
    Waiting.pop_front();
    Loop::get().schedule(H);
  }

private:
  bool Held = false;
  std::deque<std::coroutine_handle<>> Waiting;
};

struct Turn {
  Order &G;

  explicit Turn(Order &G) : G(G) {}
  Turn(const Turn &) = delete;
  Turn &operator=(const Turn &) = delete;
  ~Turn() { G.give(); }
};

constexpr int kAcceptTickMs = 250;

class TcpDataChannel : public DataChannel {
public:
  TcpDataChannel(size_t PageCount, size_t PageSize, std::string Host) : Pool(PageCount, PageSize), Host(std::move(Host)) {}

  // Framed, so a page no longer has to wait for the one before it to be
  // answered. The block matches what the fabric backend uses; the depth is
  // what the pool can hold pages for.
  DataChannelTraits traits() const override { return {/*IsRdma=*/false, /*MaxBlockSize=*/1u << 22, /*MaxInFlight=*/16, /*Name=*/"tcp"}; }

  PagePool &pool() override { return Pool; }

  Coro<Result<std::string>> listen() override {
    if (!Pool.backed()) co_return failMessage("no registered memory for this channel's pages");
    auto L = Stream::listenOn(0);
    if (!L) co_return std::unexpected(L.error());
    auto Port = L->localPort();
    if (!Port) co_return std::unexpected(Port.error());
    Listener = std::move(*L);
    co_return std::format("{}:{}", reachableAddress(Host), *Port);
  }

  // The peer only has to be there while we are waiting for it. Its end of the
  // control channel going away means no connection is ever coming, and waiting
  // for one is waiting forever - which is what a dead ssh helper used to cost.
  void watch(int Fd) override { Alive = Fd; }

  Coro<Result<void>> acceptPeer() override {
    for (;;) {
      auto S = Listener.tryAccept();
      if (!S) co_return std::unexpected(S.error());

      if (S->valid()) {
        Peer = std::move(*S);
        Listener.close();
        co_return Result<void>{};
      }

      co_await WaitFor{Listener.fd(), EPOLLIN, Alive >= 0 ? kAcceptTickMs : 0};
      if (Alive >= 0 && peerGone(Alive)) co_return failMessage("the peer went away before it connected");
    }
  }

  Coro<Result<void>> connect(const std::string &Endpoint) override {
    if (!Pool.backed()) co_return failMessage("no registered memory for this channel's pages");
    const size_t Colon = Endpoint.rfind(':');
    if (Colon == std::string::npos) co_return failMessage("bad tcp endpoint: " + Endpoint);

    const std::string Address = Endpoint.substr(0, Colon);
    const std::string PortText = Endpoint.substr(Colon + 1);

    uint16_t Port = 0;
    const auto *Begin = PortText.data();
    const auto *End = Begin + PortText.size();
    if (std::from_chars(Begin, End, Port).ec != std::errc{}) co_return failMessage("bad tcp endpoint: " + Endpoint);

    auto S = Stream::connect(Address.empty() ? Host : Address, Port);
    if (!S) co_return std::unexpected(S.error());
    Peer = std::move(*S);
    co_return Result<void>{};
  }

  // Every payload carries its key and its length, the way AFP, SMB and HTTP/2
  // all frame a shared connection. Without a header a byte stream can only
  // carry one transfer at a time - a second would interleave with the first
  // and both would arrive as nonsense - which held this backend to one page
  // per round trip.
  //
  //   key(8) | length(4) | payload
  //
  // Sends take turns so a frame reaches the wire whole. Receives do not: they
  // register their buffer under a key, and one reader hands each frame to
  // whoever asked for it, in whatever order the frames turn up.
  Coro<Result<void>> send(Page &Buf, uint64_t Key) override {
    co_await Sending.take();
    const Turn Mine(Sending);

    std::byte Header[kHeaderSize];
    const uint32_t Length = static_cast<uint32_t>(Buf.size());
    std::memcpy(Header, &Key, 8);
    std::memcpy(Header + 8, &Length, 4);

    co_return co_await Peer.writeAll(Header, Buf.data());
  }

  Coro<Result<void>> recv(Page &Buf, uint64_t Key, size_t Length) override {
    // resize() clamps, so asking for more than a page holds would leave Room
    // describing memory this buffer does not own and the frame would overrun
    // it. Refuse instead: a caller asking for more than a page is a bug in the
    // caller, and it is cheaper to say so than to find the corruption later.
    Buf.resize(Length);
    if (Buf.size() != Length) co_return failMessage(std::format("receive of {} bytes exceeds the {} byte page", Length, Buf.capacity()));

    // One posted receive per key. A second would leave the first waiting on a
    // frame that is handed to neither of them, and whichever unposts first
    // would take the other's entry with it.
    Posted Mine{Buf.bytes(), {Buf.bytes(), Length}, false};
    if (!Waiting.emplace(Key, &Mine).second) co_return failMessage(std::format("key {} already has a receive posted", Key));
    const Unpost Drop(Waiting, Key);

    // The reader may be parked on a frame whose key had not been asked for
    // yet. This might be it.
    wakeReaders();

    // Whoever finds nobody reading takes it on. It has to be a loop: the one
    // driving stops as soon as its own frame lands, and someone still waiting
    // has to pick the socket up rather than sleep beside it.
    for (;;) {
      if (Mine.Done) co_return Result<void>{};
      if (!Failure.empty()) co_return failMessage(Failure);

      if (!Reading) {
        Reading = true;
        auto Outcome = co_await deliverUntil(Mine);
        Reading = false;
        wakeReaders();
        if (!Outcome) co_return Outcome;
        continue;
      }

      co_await Idle{this};
    }
  }

  void close() override {
    // Anyone parked in recv() is waiting for a frame that is no longer coming,
    // and closing the socket underneath them says nothing they can see. Give
    // them a reason first, then wake them, or shutdown waits for them forever.
    if (Failure.empty()) Failure = "channel closed";
    wakeReaders();

    Peer.close();
    Listener.close();
  }

private:
  static constexpr size_t kHeaderSize = 12; // key(8) + length(4)

  struct Posted {
    std::byte *Into;
    std::span<std::byte> Room;
    bool Done;
  };

  // Reads frames and hands each to whoever asked for that key, until this
  // caller's own frame has landed. A frame for a key nobody has posted yet
  // waits: the peer only sends what the control channel has already asked for,
  // so the receive is on its way.
  Coro<Result<void>> deliverUntil(Posted &Mine) {
    while (!Mine.Done) {
      std::byte Header[kHeaderSize];
      if (auto R = co_await Peer.readExact(Header); !R) co_return std::unexpected(fail(R.error()));

      uint64_t Key = 0;
      uint32_t Length = 0;
      std::memcpy(&Key, Header, 8);
      std::memcpy(&Length, Header + 8, 4);

      auto It = Waiting.find(Key);
      while (It == Waiting.end()) {
        co_await Idle{this};
        It = Waiting.find(Key);
      }

      // A frame has to be exactly what was asked for. Anything else means the
      // stream has lost its place, and reading the wrong number of bytes here
      // would consume the next frame's header and desynchronise everything
      // after it - which is far harder to find than a refusal right now.
      Posted *Theirs = It->second;
      if (Length != Theirs->Room.size())
        co_return std::unexpected(fail(
            failMessage(std::format("frame for key {} carries {} bytes against the {} posted for it", Key, Length, Theirs->Room.size())).error()));

      if (auto R = co_await Peer.readExact({Theirs->Into, Length}); !R) co_return std::unexpected(fail(R.error()));
      Theirs->Done = true;
      if (Theirs != &Mine) wakeReaders();
    }
    co_return Result<void>{};
  }

  // A read failure belongs to everyone waiting on this socket, not just to
  // whoever happened to be driving it.
  Error fail(const Error &Why) {
    Failure = Why.message();
    wakeReaders();
    return Why;
  }

  void wakeReaders() {
    auto Woken = std::move(Idlers);
    Idlers.clear();
    for (auto H : Woken)
      if (H && !H.done()) Loop::get().schedule(H);
  }

  struct Idle {
    TcpDataChannel *C;
    std::coroutine_handle<> Queued{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> H) {
      Queued = H;
      C->Idlers.push_back(H);
    }
    void await_resume() noexcept { Queued = {}; }
    ~Idle() {
      if (Queued) std::erase(C->Idlers, Queued);
    }
  };

  // Takes the posted receive out however this one ends, so a frame can never
  // be handed to a buffer whose owner has gone.
  struct Unpost {
    std::unordered_map<uint64_t, Posted *> &Where;
    uint64_t Key;

    ~Unpost() { Where.erase(Key); }
  };

  PagePool Pool;
  std::string Host;
  int Alive = -1;
  Stream Listener;
  Stream Peer;
  Order Sending;
  std::unordered_map<uint64_t, Posted *> Waiting;
  std::deque<std::coroutine_handle<>> Idlers;
  std::string Failure;
  bool Reading = false;
};

} // namespace

std::unique_ptr<DataChannel> makeTcpDataChannel(size_t PageCount, size_t PageSize, std::string Host) {
  return std::make_unique<TcpDataChannel>(PageCount, PageSize, std::move(Host));
}

} // namespace rail
