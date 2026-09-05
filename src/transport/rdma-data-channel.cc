#include "rail/transport/data-channel.h"

#include "rail/io/loop.h"
#include "rail/io/stream.h" // WaitFor
#include "rail/transport/rdma-device.h"
#include "rail/transport/rdma-devices.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <memory>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unordered_map>
#include <vector>

namespace rail {

namespace {

// Receives one channel may have posted at once, and so the depth of the ring
// the peer writes its clear-to-send records into.
constexpr uint32_t kSlots = 512;

// A floor on the wait, and nothing more. Arming happens before the sleep and
// the queue is polled once more after arming, so a completion that lands in
// between is seen rather than slept through.
//
// This used to be a second and it used to matter: the event loop armed its
// descriptors EPOLLONESHOT, which disables one as it fires, so re-arming raced
// with the wakeup and about one transfer in twenty slept the whole second. The
// loop is level triggered now - a readable descriptor is reported every turn
// until it is drained - so a wakeup cannot go missing and this is only a floor.
constexpr int kCompletionTick = 5;

// How long the loop keeps polling the queue after the last completion. An
// interrupt costs more than a small page takes to cross the wire, so a run of
// them is answered by polling; once the channel goes quiet the loop stops and
// sleeps, and an idle mount does not hold a core.
constexpr auto kPollWindow = std::chrono::microseconds(2000);

// Receives posted per rail. A write with immediate consumes one whether or not
// it carried a buffer, so both the pages and the offers draw on this.
constexpr uint32_t kReceives = kSlots * 2;

constexpr uint32_t kQueueDepth = kReceives + 64;

// Immediate data is the only thing that travels with a write, so it says which
// of the two kinds this is. A plain write would be invisible to the far side -
// it raises no completion there - and the waiting coroutine would sleep until
// its timer instead of being woken.
constexpr uint32_t kIsCts = 1u << 31;

// Both ports of this adapter, used together. Two queue pairs have no order
// between them, so a page goes to one rail whole and the slot in the immediate
// puts them back in order.
constexpr size_t kMaxRails = 2;

// What the receiver writes into the sender's ring: "the page for this key goes
// here". The sender reads it out of its own memory, so asking costs nothing on
// the wire.
struct Cts {
  uint64_t Key;
  // One per rail, like the key beside it. A process registers one virtual
  // range with every rail and repeats the address here, but a kernel client
  // registers physical memory per device and gets a different address on each,
  // with no way to make them agree. Carrying both is what lets it offer a page
  // on more than one rail.
  uint64_t Addr[kMaxRails];
  uint32_t Length;
  uint32_t Slot;
  // One per rail: the same memory registered on two devices has a different
  // key on each, so the sender needs the one belonging to the rail it picks.
  uint32_t Rkey[kMaxRails];
  // Kept for the wire layout. A record is taken on its completion, not by
  // watching this change.
  uint32_t Seq;
};
static_assert(sizeof(Cts) == 48, "the ring is written by rdma, so its layout is wire format");

// Sent once each way, inside the endpoint blob the control channel already
// carries for UCX.
struct RailWire {
  uint8_t Gid[16];
  uint32_t Qpn;
  uint32_t CtsRkey;
};

struct Wire {
  uint32_t Rails;
  uint32_t Slots;
  uint64_t CtsAddr;
  uint32_t Mtu;
  uint32_t Pad;
  RailWire Line[kMaxRails];
};

std::string pack(const Wire &W) { return std::string(reinterpret_cast<const char *>(&W), sizeof(W)); }

bool unpack(const std::string &Blob, Wire &W) {
  if (Blob.size() != sizeof(Wire)) return false;
  std::memcpy(&W, Blob.data(), sizeof(W));
  return true;
}

// One rail. A device, its protection domain, the queue pair that talks to the
// peer, and the two regions the peer is allowed to reach: the pages, and the
// ring it writes clear-to-send records into.
class Rail {
public:
  Result<void> open(const RdmaPort &Port, std::span<std::byte> Ring) {
    auto Shared = RdmaDevice::open(Port.Device);
    if (!Shared) return std::unexpected(Shared.error());
    Owner = *Shared;
    Context = Owner->context();
    Domain = Owner->domain();

    if (auto R = Memory::get().attach(Owner); !R) return R;

    Number = Port.Port;
    if (ibv_query_port(Context, Number, &Attr) != 0) return failErrno("ibv_query_port");
    if (ibv_query_gid(Context, Number, 0, &Gid) != 0) return failErrno("ibv_query_gid");

    Comp = ibv_create_comp_channel(Context);
    if (!Comp) return failMessage("ibv_create_comp_channel failed");
    if (::fcntl(Comp->fd, F_SETFL, ::fcntl(Comp->fd, F_GETFL) | O_NONBLOCK) != 0) return failErrno("fcntl on the completion channel");

    Queue = ibv_create_cq(Context, kQueueDepth * 2, nullptr, Comp, 0);
    if (!Queue) return failMessage("ibv_create_cq failed");
    if (ibv_req_notify_cq(Queue, 0) != 0) return failErrno("ibv_req_notify_cq");

    const int Access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    RingRegion = ibv_reg_mr(Domain, Ring.data(), Ring.size(), Access);
    if (!RingRegion) return failErrno("ibv_reg_mr for the ring");

    ibv_qp_init_attr Init{};
    Init.send_cq = Queue;
    Init.recv_cq = Queue;
    Init.qp_type = IBV_QPT_RC;
    Init.cap.max_send_wr = kQueueDepth;
    Init.cap.max_recv_wr = kQueueDepth;
    Init.cap.max_send_sge = 1;
    Init.cap.max_recv_sge = 1;
    Init.cap.max_inline_data = sizeof(Cts);
    Pair = ibv_create_qp(Domain, &Init);
    if (!Pair) return failErrno("ibv_create_qp");

    Inline = Init.cap.max_inline_data >= sizeof(Cts);

    return toInit();
  }

  ~Rail() {
    if (Pair) ibv_destroy_qp(Pair);
    if (RingRegion) ibv_dereg_mr(RingRegion);
    if (Queue) ibv_destroy_cq(Queue);
    if (Comp) ibv_destroy_comp_channel(Comp);
  }

  Rail(const Rail &) = delete;
  Rail &operator=(const Rail &) = delete;
  Rail() = default;

  void describe(RailWire &W) const {
    std::memcpy(W.Gid, &Gid, sizeof(W.Gid));
    W.Qpn = Pair->qp_num;
    W.CtsRkey = RingRegion->rkey;
  }

  Result<void> join(const RailWire &Peer, uint32_t Mtu) {
    Remote = Peer;
    Path = Mtu;
    if (auto R = toReady(); !R) return R;
    return toSending();
  }

  ibv_cq *queue() const { return Queue; }
  ibv_comp_channel *channel() const { return Comp; }
  ibv_qp *pair() const { return Pair; }
  size_t slot() const { return Owner->slot(); }
  uint32_t ringKey() const { return RingRegion->lkey; }
  bool inlineCts() const { return Inline; }

  // An arrival consumes one whether or not it carried a buffer.
  void spent() {
    if (Stocked > 0) Stocked--;
  }

  uint32_t stocked() const { return Stocked; }
  void stocked(uint32_t Count) { Stocked = Count; }
  const RailWire &peer() const { return Remote; }

private:
  Result<void> toInit() {
    ibv_qp_attr A{};
    A.qp_state = IBV_QPS_INIT;
    A.pkey_index = 0;
    A.port_num = Number;
    A.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(Pair, &A, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) return failErrno("ibv_modify_qp to init");
    return {};
  }

  Result<void> toReady() {
    ibv_qp_attr A{};
    A.qp_state = IBV_QPS_RTR;
    A.path_mtu = static_cast<ibv_mtu>(Path);
    A.dest_qp_num = Remote.Qpn;
    A.rq_psn = 0;
    A.max_dest_rd_atomic = 1;
    A.min_rnr_timer = 12;
    A.ah_attr.is_global = 1;
    A.ah_attr.port_num = Number;
    A.ah_attr.grh.hop_limit = 64;
    A.ah_attr.grh.sgid_index = 0;
    std::memcpy(&A.ah_attr.grh.dgid, Remote.Gid, sizeof(Remote.Gid));

    const int Mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(Pair, &A, Mask) != 0) return failErrno("ibv_modify_qp to rtr");
    return {};
  }

  Result<void> toSending() {
    ibv_qp_attr A{};
    A.qp_state = IBV_QPS_RTS;
    A.timeout = 14;
    A.retry_cnt = 7;
    A.rnr_retry = 7;
    A.sq_psn = 0;
    A.max_rd_atomic = 1;

    const int Mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(Pair, &A, Mask) != 0) return failErrno("ibv_modify_qp to rts");
    return {};
  }

  std::shared_ptr<RdmaDevice> Owner;
  ibv_context *Context = nullptr;
  ibv_pd *Domain = nullptr;
  ibv_comp_channel *Comp = nullptr;
  ibv_cq *Queue = nullptr;
  ibv_mr *RingRegion = nullptr;
  uint32_t Stocked = 0;
  ibv_qp *Pair = nullptr;
  ibv_port_attr Attr{};
  ibv_gid Gid{};
  uint8_t Number = 1;
  bool Inline = false;
  RailWire Remote{};
  uint32_t Path = static_cast<uint32_t>(IBV_MTU_1024);
};

class RdmaDataChannel final : public DataChannel {
public:
  RdmaDataChannel(size_t PageCount, size_t PageSize) : Pool(PageCount, PageSize), Ring(kSlots * 2) {}

  DataChannelTraits traits() const override { return {true, 1u << 22, kSlots / 2, "rdma"}; }
  PagePool &pool() override { return Pool; }

  Result<void> prepare() override { return start(); }

  void watch(int Fd) override { Alive = Fd; }

  bool wantsPeerEndpoint() const override { return true; }

  Coro<Result<std::string>> listen() override {
    if (auto R = start(); !R) co_return std::unexpected(R.error());
    co_return blob();
  }

  Coro<Result<void>> connect(const std::string &Endpoint) override {
    if (auto R = start(); !R) co_return std::unexpected(R.error());
    co_return meet(Endpoint);
  }

  Coro<Result<void>> acceptPeer() override { co_return Result<void>{}; }

  Coro<Result<std::string>> localEndpoint() override {
    if (auto R = start(); !R) co_return std::unexpected(R.error());
    co_return blob();
  }

  Coro<Result<void>> attachPeer(const std::string &Endpoint) override { co_return meet(Endpoint); }

  // Says where the page for this key should land, then waits for it. The peer
  // writes straight into the page, so nothing is copied and nothing is matched.
  Coro<Result<void>> recv(Page &Buf, uint64_t Key, size_t Length) override {
    Buf.resize(Length);
    if (Buf.size() != Length) co_return failMessage("receive larger than the page it would land in");
    if (!Joined) co_return failMessage("rdma channel is not connected");

    if (Free.empty()) co_return failMessage("no receive slot left on the rdma channel");
    const uint32_t Slot = takeSlot();
    Landing[Slot] = Waiting{Buf.bytes(), Length, Key, false};

    if (auto R = advertise(Slot, Key, Buf, Length); !R) {
      freeSlot(Slot);
      co_return R;
    }

    auto Landed = co_await until([&] { return Landing[Slot].Done; });
    freeSlot(Slot);
    co_return Landed;
  }

  // Reads its own memory to find where the peer wants this page, then writes it
  // there. The write carries the slot as immediate data, which is the only
  // notification the peer needs.
  Coro<Result<void>> send(Page &Buf, uint64_t Key) override {
    if (!Joined) co_return failMessage("rdma channel is not connected");

    Cts Where{};
    if (auto R = co_await claim(Key, Where); !R) co_return R;
    if (Buf.size() > Where.Length) co_return failMessage("page is larger than the room the peer offered");

    const uint64_t Id = ++Posted;
    Sent[Id] = false;

    // A page goes to one rail whole. Splitting it would cross two queue pairs,
    // which have no ordering between them, and the peer would have to put the
    // halves back together for nothing.
    const size_t Which = Turn++ % Lines.size();
    Rail &Chosen = *Lines[Which];

    ibv_sge Piece{};
    Piece.addr = reinterpret_cast<uint64_t>(Buf.bytes());
    Piece.length = static_cast<uint32_t>(Buf.size());
    Piece.lkey = Memory::get().lkeyOf(Buf, Chosen.slot());
    if (Piece.lkey == 0) co_return failMessage("a page was never registered with this rail");

    ibv_send_wr Work{};
    Work.wr_id = Id;
    Work.sg_list = &Piece;
    Work.num_sge = 1;
    Work.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    Work.send_flags = IBV_SEND_SIGNALED;
    Work.imm_data = htonl(Where.Slot);
    Work.wr.rdma.remote_addr = Where.Addr[Which];
    Work.wr.rdma.rkey = Where.Rkey[Which];

    ibv_send_wr *Bad = nullptr;
    if (ibv_post_send(Chosen.pair(), &Work, &Bad) != 0) {
      Sent.erase(Id);
      co_return failErrno("ibv_post_send");
    }

    // Waited for on purpose. The completion says the adapter has finished
    // reading this page, and only then may it go back to the pool.
    auto Done = co_await until([&] { return Sent[Id]; });
    Sent.erase(Id);
    co_return Done;
  }

  void close() override {
    if (Failure.empty()) Failure = "channel closed";
    wake();
  }

private:
  struct Waiting {
    std::byte *Into = nullptr;
    size_t Room = 0;
    uint64_t Key = 0;
    bool Done = false;
  };

  Result<void> start() {
    if (Started) return {};

    if (!Pool.backed()) return failMessage("no registered memory for this channel's pages");

    auto Ports = activeRdmaPorts();
    if (Ports.empty()) return failMessage("no active rdma port");
    if (Ports.size() > kMaxRails) Ports.resize(kMaxRails);

    // Fewer rails than the machine offers, so one rail's ceiling can be
    // measured against two on the same hardware in the same minutes. The peer
    // resizes to whatever this side advertises, so capping one end is enough.
    if (const char *Capped = std::getenv("RAIL_RAILS")) {
      const size_t Want = std::strtoul(Capped, nullptr, 10);
      if (Want > 0 && Want < Ports.size()) Ports.resize(Want);
    }

    const std::span<std::byte> Records{reinterpret_cast<std::byte *>(Ring.data()), Ring.size() * sizeof(Cts)};

    for (const auto &Port : Ports) {
      auto One = std::make_unique<Rail>();
      if (auto R = One->open(Port, Records); !R) return R;
      Lines.push_back(std::move(One));
    }

    Free.clear();
    for (uint32_t I = kSlots; I-- > 0;) Free.push_back(I);
    Landing.assign(kSlots, {});

    Pump = Loop::get().drive([this] { return pump(); });
    Started = true;
    return {};
  }

  std::string blob() const {
    Wire W{};
    W.Rails = static_cast<uint32_t>(Lines.size());
    W.Slots = kSlots;
    W.CtsAddr = reinterpret_cast<uint64_t>(Ring.data());
    W.Mtu = static_cast<uint32_t>(IBV_MTU_1024);
    for (size_t I = 0; I < Lines.size(); I++) Lines[I]->describe(W.Line[I]);
    return pack(W);
  }

  // Both ends must agree on how many rails are in play, so the smaller wins.
  Result<void> meet(const std::string &Endpoint) {
    Wire Peer{};
    if (!unpack(Endpoint, Peer)) return failMessage("malformed rdma endpoint");
    if (Peer.Rails == 0) return failMessage("peer offered no rdma rail");
    if (Lines.empty()) return failMessage("this side has no rdma rail open yet");

    if (Peer.Rails < Lines.size()) Lines.resize(Peer.Rails);
    PeerRing = Peer.CtsAddr;

    for (size_t I = 0; I < Lines.size(); I++) {
      PeerRingKey[I] = Peer.Line[I].CtsRkey;
      if (auto R = Lines[I]->join(Peer.Line[I], Peer.Mtu); !R) return R;
    }

    if (auto R = stock(); !R) return R;
    Joined = true;
    return {};
  }

  uint32_t takeSlot() {
    const uint32_t Slot = Free.back();
    Free.pop_back();
    return Slot;
  }

  void freeSlot(uint32_t Slot) {
    Landing[Slot] = {};
    Free.push_back(Slot);
  }

  // A write with immediate consumes a receive work request even though it
  // carries no buffer, so the queue has to be kept stocked or the peer's write
  // is refused for want of one.
  Result<void> postReceive(Rail &On) {
    ibv_recv_wr Work{};
    Work.wr_id = 0;
    Work.sg_list = nullptr;
    Work.num_sge = 0;

    ibv_recv_wr *Bad = nullptr;
    if (ibv_post_recv(On.pair(), &Work, &Bad) != 0) return failErrno("ibv_post_recv");

    On.stocked(On.stocked() + 1);
    return {};
  }

  Result<void> stock() {
    for (auto &One : Lines)
      for (uint32_t I = 0; I < kReceives; I++)
        if (auto R = postReceive(*One); !R) return R;
    return {};
  }

  // Round-robin like the pages are, and for the same reason: every offer going
  // out on one rail made it spend receive buffers at twice the rate of the
  // others. Only rails the peer registered its ring on are usable, and a peer
  // may have registered just the first - the kernel mount does.
  size_t offerRail() {
    for (size_t I = 0; I < Lines.size(); I++) {
      const size_t Which = (Offers + I) % Lines.size();
      if (PeerRingKey[Which] != 0) {
        Offers = Which + 1;
        return Which;
      }
    }
    return 0;
  }

  Result<void> advertise(uint32_t Slot, uint64_t Key, Page &Into, size_t Length) {
    Cts Record{Key, {}, static_cast<uint32_t>(Length), Slot, {}, 0};
    for (size_t I = 0; I < Lines.size(); I++) {
      Record.Addr[I] = reinterpret_cast<uint64_t>(Into.bytes());
      Record.Rkey[I] = Memory::get().rkeyOf(Into, Lines[I]->slot());
      if (Record.Rkey[I] == 0) return failMessage("a page was never registered with this rail");
    }
    Record.Seq = ++Stamp;
    Cts &Staged = Ring[kSlots + Slot];
    Staged = Record;

    const size_t Which = offerRail();
    Rail &Post = *Lines[Which];

    ibv_sge Piece{};
    Piece.addr = reinterpret_cast<uint64_t>(&Staged);
    Piece.length = sizeof(Cts);
    Piece.lkey = Post.ringKey();

    ibv_send_wr Work{};
    Work.wr_id = 0;
    Work.sg_list = &Piece;
    Work.num_sge = 1;
    Work.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    Work.imm_data = htonl(kIsCts | Slot);
    Work.send_flags = IBV_SEND_SIGNALED | (Post.inlineCts() ? IBV_SEND_INLINE : 0);
    Work.wr.rdma.remote_addr = PeerRing + Slot * sizeof(Cts);
    Work.wr.rdma.rkey = PeerRingKey[Which];

    ibv_send_wr *Bad = nullptr;
    if (ibv_post_send(Post.pair(), &Work, &Bad) != 0) return failErrno("ibv_post_send for the clear to send");
    return {};
  }

  void offered(uint32_t Slot) {
    if (Slot >= kSlots) return;
    Cts Record{};
    std::memcpy(&Record, &Ring[Slot], sizeof(Record));
    Held[Record.Key].push_back(Record);
  }

  bool harvest(uint64_t Key, Cts &Out) {
    auto It = Held.find(Key);
    if (It == Held.end() || It->second.empty()) return false;

    Out = It->second.front();
    It->second.pop_front();
    if (It->second.empty()) Held.erase(It);
    return true;
  }

  Coro<Result<void>> claim(uint64_t Key, Cts &Out) {
    co_return co_await until([&] { return harvest(Key, Out); });
  }

  // Poll, arm, poll again, and only then sleep. The second poll is what makes
  // the sleep safe: a completion landing between the first poll and the arm
  // would otherwise raise no event and this would wait on the timer instead.
  template <class Ready> Coro<Result<void>> until(Ready Done) {
    Pending++;
    const Leave Mine{this};
    for (;;) {
      drive();
      refill();
      if (Done()) co_return Result<void>{};
      if (!Failure.empty()) co_return failMessage(Failure);

      for (auto &One : Lines)
        if (ibv_req_notify_cq(One->queue(), 0) != 0) co_return failErrno("ibv_req_notify_cq");

      drive();
      if (Done()) co_return Result<void>{};
      if (!Failure.empty()) co_return failMessage(Failure);

      co_await Either{this};
      absorb();

      // The peer only has to be there while we are waiting on it. If its end
      // of the control channel has gone, no completion is ever coming and
      // waiting for one is waiting forever.
      if (Alive >= 0 && gone(Alive)) co_return failMessage("peer closed while waiting for it");
    }
  }

  void refill() {
    for (auto &One : Lines) {
      while (One->stocked() < kReceives) {
        if (auto R = postReceive(*One); !R) return;
      }
    }
  }

  bool absorb() {
    for (auto &One : Lines) {
      ibv_cq *Which = nullptr;
      void *Context = nullptr;
      unsigned Seen = 0;
      while (ibv_get_cq_event(One->channel(), &Which, &Context) == 0) Seen++;
      if (Seen > 0) ibv_ack_cq_events(One->queue(), Seen);
    }

    const bool Found = drive();
    refill();
    return Found;
  }

  bool drive() {
    bool Found = false;
    for (auto &One : Lines)
      if (driveOne(*One)) Found = true;
    if (Found) Woke = std::chrono::steady_clock::now();
    return Found;
  }

  // Polls while a transfer is live so a completion is seen without waiting for
  // an interrupt, and reports nothing once the window since the last one has
  // passed, which lets the loop go back to sleeping.
  bool pump() {
    const bool Found = drive();
    return Found || (Pending > 0 && std::chrono::steady_clock::now() - Woke < kPollWindow);
  }

  bool driveOne(Rail &On) {
    bool Any = false;
    ibv_wc Done[16];
    for (;;) {
      const int Count = ibv_poll_cq(On.queue(), 16, Done);
      if (Count <= 0) return Any;
      Any = true;

      for (int I = 0; I < Count; I++) {
        if (Done[I].status != IBV_WC_SUCCESS) {
          if (Failure.empty()) Failure = ibv_wc_status_str(Done[I].status);
          continue;
        }

        if (Done[I].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
          const uint32_t Immediate = ntohl(Done[I].imm_data);
          const uint32_t Slot = Immediate & ~kIsCts;

          On.spent();

          if (Immediate & kIsCts) offered(Slot);
          else if (Slot < kSlots) Landing[Slot].Done = true;
          continue;
        }
        auto It = Sent.find(Done[I].wr_id);
        if (It != Sent.end()) It->second = true;
      }
    }
  }

  // Woken by a completion or the tick, never by the control fd: the loop wakes
  // every waiter on a descriptor, so that would resume every in-flight op on
  // each frame. until() checks the peer on the tick instead.
  struct Either {
    RdmaDataChannel *C;
    std::coroutine_handle<> H{};

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> Handle) {
      H = Handle;
      bool First = true;
      for (auto &One : C->Lines) {
        if (First) Loop::get().waitFor(One->channel()->fd, EPOLLIN, kCompletionTick, H);
        else Loop::get().wait(One->channel()->fd, EPOLLIN, H);
        First = false;
      }
    }

    void await_resume() {
      if (H) Loop::get().cancel(H);
    }
  };

  static bool gone(int Fd) {
    ::pollfd Watch{Fd, POLLIN | POLLRDHUP, 0};
    if (::poll(&Watch, 1, 0) <= 0) return false;
    if (Watch.revents & (POLLHUP | POLLERR | POLLRDHUP)) return true;

    char Peek = 0;
    const ssize_t Saw = ::recv(Fd, &Peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (Saw == 0) return true;
    return Saw < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOTSOCK;
  }

  void wake() {
    for (auto &One : Lines) Loop::get().wake(One->channel()->fd);
  }

  // Says when the last coroutine stops waiting, however it leaves.
  struct Leave {
    RdmaDataChannel *C;
    ~Leave() { C->Pending--; }
  };

  PagePool Pool;
  std::vector<Cts> Ring;
  std::vector<Waiting> Landing;
  std::vector<uint32_t> Free;
  std::unordered_map<uint64_t, std::deque<Cts>> Held;
  std::unordered_map<uint64_t, bool> Sent;
  std::vector<std::unique_ptr<Rail>> Lines;
  uint64_t PeerRing = 0;
  size_t Offers = 0;
  uint32_t PeerRingKey[kMaxRails]{};
  size_t Turn = 0;
  std::string Failure;
  uint64_t Posted = 0;
  uint32_t Stamp = 0xFFFFFFF0;
  int Alive = -1;
  size_t Pending = 0;
  std::chrono::steady_clock::time_point Woke{};
  Loop::Driver Pump;
  bool Started = false;
  bool Joined = false;
};

} // namespace

std::unique_ptr<DataChannel> makeRdmaDataChannel(size_t PageCount, size_t PageSize) { return std::make_unique<RdmaDataChannel>(PageCount, PageSize); }

} // namespace rail
