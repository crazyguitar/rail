#include "rail/file-service.h"
#include "rail/memory.h"

#include "rail/app/checksum.h"
#include "rail/fs/reader.h"
#include "rail/fs/writer.h"
#include "rail/io/stream.h"
#include "rail/io/trace.h"
#include "rail/proto/control-channel.h"
#include "rail/stream/page-stream.h"
#include "rail/stream/sink.h"
#include "rail/transport/data-channel.h"

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <unordered_map>

namespace rail {

struct FileClient::Impl {
  proto::ControlChannel Control;
  std::unique_ptr<DataChannel> Channel;
  uint64_t NextId = 1;

  struct Waiting {
    std::optional<proto::Message> Reply;
    std::coroutine_handle<> Caller;
    bool Done = false;
  };
  std::unordered_map<uint64_t, std::deque<Waiting *>> Waiters;
  Coro<void> Reader;
  bool Closed = false;
  std::string Failure;

  // Registered before the request goes out: sending can suspend, and a reply
  // that arrives while nobody is waiting for it would be dropped.
  struct Exchange {
    Impl *Self = nullptr;
    uint64_t Id = 0;
    Waiting Slot;

    Exchange(Impl *Self, uint64_t Id) : Self(Self), Id(Id) { Self->Waiters[Id].push_back(&Slot); }
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;
    ~Exchange() {
      auto It = Self->Waiters.find(Id);
      if (It == Self->Waiters.end()) return;
      std::erase(It->second, &Slot);
      if (It->second.empty()) Self->Waiters.erase(It);
    }

    auto wait() {
      struct Awaiter {
        Exchange *Ex;

        bool await_ready() const noexcept { return Ex->Slot.Done || Ex->Self->Closed; }
        void await_suspend(std::coroutine_handle<> Caller) const { Ex->Slot.Caller = Caller; }

        Result<proto::Message> await_resume() const {
          if (!Ex->Slot.Reply) return failMessage(Ex->Self->Failure.empty() ? "the peer closed" : Ex->Self->Failure);
          return std::move(*Ex->Slot.Reply);
        }
      };
      return Awaiter{this};
    }
  };

  void fail(const std::string &Why) {
    Failure = Why;
    Closed = true;

    for (auto &[Id, Queue] : Waiters)
      for (Waiting *Slot : Queue) {
        Slot->Done = true;
        if (Slot->Caller) Loop::get().schedule(std::exchange(Slot->Caller, {}));
      }
  }

  Coro<void> pump() {
    for (;;) {
      auto M = co_await Control.receive();
      if (!M) {
        fail(M.error().message());
        co_return;
      }

      auto It = Waiters.find(proto::idOf(*M));
      if (It == Waiters.end() || It->second.empty()) continue;

      Waiting *Slot = It->second.front();
      It->second.pop_front();
      Slot->Reply = std::move(*M);
      Slot->Done = true;
      if (auto Caller = std::exchange(Slot->Caller, {})) Loop::get().schedule(Caller);
    }
  }

  // Where the next streamed transfer's tags start. It advances only once the
  // peer has answered, so two streams sharing a client would take the same
  // base and their pages would answer each other's receives. One at a time.
  uint64_t TagCursor = 0;

  // One streamed transfer at a time on a client. Ownership passes straight to
  // the next waiter, so a transfer that arrives in between cannot take it.
  class OneStream {
  public:
    auto take() {
      struct Awaiter {
        OneStream *G;
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

        // A coroutine destroyed while it waits takes its handle with it.
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

  OneStream Streaming;

  // Gives the turn back however the transfer ends, including a coroutine
  // destroyed part way through one.
  struct Streamer {
    OneStream &G;

    explicit Streamer(OneStream &G) : G(G) {}
    Streamer(const Streamer &) = delete;
    Streamer &operator=(const Streamer &) = delete;
    ~Streamer() { G.give(); }
  };

  bool FlipOneBit = false;
  size_t AbortAfterPages = 0;
  bool Verify = true;
  Sum Agreed = Sum::XxH3;

  // Held one to a pointer, never inline in the deque: a posted receive is
  // writing into Buf by address, and a deque that moves its elements - which
  // an erase from the middle does - would leave the fabric filling memory that
  // has moved out from under it.
  struct Posted {
    Page Buf;
    Page *Direct = nullptr;
    Coro<Result<void>> Op;
    std::span<std::byte> Into;
    uint64_t Id = 0;
    std::unique_ptr<Exchange> Wait;
  };
  std::deque<std::unique_ptr<Posted>> Reads;

  Posted *posted(uint64_t Id) {
    for (auto &Slot : Reads)
      if (Slot->Id == Id) return Slot.get();
    return nullptr;
  }

  void forget(const Posted *Which) {
    for (auto It = Reads.begin(); It != Reads.end(); ++It)
      if (It->get() == Which) {
        Reads.erase(It);
        return;
      }
  }
};

FileClient::FileClient(std::unique_ptr<Impl> P) : P(std::move(P)) {}
FileClient::~FileClient() = default;

size_t FileClient::maxTransfer() const { return P->Channel->pool().pageSize(); }

bool FileClient::alive() const { return !P->Closed; }

size_t FileClient::maxOutstanding() const {
  const size_t Pages = P->Channel->pool().pageCount();
  const size_t ByPool = Pages > 2 ? Pages - 2 : 1;
  return std::min(ByPool, P->Channel->traits().MaxInFlight);
}

namespace {

// The daemon's errno rather than a bare protocol error, so a caller can tell
// EEXIST from EACCES and a mount can hand the kernel the right number.
std::unexpected<Error> refusal(const proto::MetaReply &Reply) {
  if (Reply.Errno == 0) return failMessage(Reply.Error);
  return fail(std::error_code(static_cast<int>(Reply.Errno), std::generic_category()), Reply.Error);
}

} // namespace

Coro<Result<std::unique_ptr<FileClient>>> FileClient::connect(const std::string &Host, const ServiceOptions &Opts) {
  auto Sock = Stream::connect(Host, Opts.Port);
  if (!Sock) co_return std::unexpected(Sock.error());

  auto P = std::make_unique<Impl>();
  P->Control = proto::ControlChannel(std::move(*Sock));
  P->FlipOneBit = Opts.FlipOneBit;
  P->AbortAfterPages = Opts.AbortAfterPages;
  P->Verify = Opts.Verify;
  P->Agreed = Opts.Checksum;
  P->Channel = makeDataChannel(Opts.Backend, Opts.PageCount, Opts.PageSize, Host);
  if (!P->Channel) co_return failMessage(std::format("unknown backend: {}", Opts.Backend));

  if (auto R = P->Channel->prepare(); !R) co_return std::unexpected(R.error());

  proto::Hello H;
  if (Opts.PretendVersion != 0) H.Version = Opts.PretendVersion;
  H.Backend = Opts.Backend;
  H.PageCount = Opts.PageCount;
  H.PageSize = Opts.PageSize;
  H.WindowPages = 1;
  H.Verify = Opts.Verify;
  H.Sum = static_cast<uint8_t>(Opts.Checksum);
  if (auto R = co_await P->Control.send(H); !R) co_return std::unexpected(R.error());

  auto Ack = co_await P->Control.expect<proto::HelloAck>();
  if (!Ack) co_return std::unexpected(Ack.error());
  P->Channel->watch(P->Control.readFd());
  if (auto R = co_await P->Channel->connect(Ack->ChannelEndpoint); !R) co_return std::unexpected(R.error());

  auto Local = co_await P->Channel->localEndpoint();
  if (!Local) co_return std::unexpected(Local.error());

  proto::PeerEndpoint Mine;
  Mine.Blob = *Local;
  if (auto R = co_await P->Control.send(Mine); !R) co_return std::unexpected(R.error());

  P->Reader = P->pump();
  P->Reader.start();

  co_return std::unique_ptr<FileClient>(new FileClient(std::move(P)));
}

template <typename T> static Result<T> asReply(Result<proto::Message> M) {
  if (!M) return std::unexpected(M.error());
  if (auto *V = std::get_if<T>(&*M)) return *V;
  return failMessage(std::format("unexpected message {}", proto::typeName(proto::typeOf(*M))));
}

Coro<Result<proto::StatReply>> FileClient::stat(const std::string &Path) {
  proto::StatRequest S;
  S.Id = P->NextId++;
  S.Path = Path;

  Impl::Exchange Ex(P.get(), S.Id);
  if (auto R = co_await P->Control.send(S); !R) co_return std::unexpected(R.error());
  co_return asReply<proto::StatReply>(co_await Ex.wait());
}

Coro<Result<proto::OpenReply>> FileClient::openFile(const std::string &Path, bool Writable) {
  proto::OpenRequest O;
  O.Id = P->NextId++;
  O.Path = Path;
  O.Writable = Writable;

  Impl::Exchange Ex(P.get(), O.Id);
  if (auto R = co_await P->Control.send(O); !R) co_return std::unexpected(R.error());
  co_return asReply<proto::OpenReply>(co_await Ex.wait());
}

Coro<Result<void>> FileClient::closeFile(uint64_t Handle) {
  proto::CloseRequest C;
  C.Id = P->NextId++;
  C.Handle = Handle;

  Impl::Exchange Ex(P.get(), C.Id);
  if (auto R = co_await P->Control.send(C); !R) co_return std::unexpected(R.error());
  auto Reply = asReply<proto::MetaReply>(co_await Ex.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return refusal(*Reply);
  co_return Result<void>{};
}

Coro<Result<proto::ListReply>> FileClient::list(const std::string &Path) {
  proto::ListRequest L;
  L.Id = P->NextId++;
  L.Path = Path;

  Impl::Exchange Ex(P.get(), L.Id);
  if (auto R = co_await P->Control.send(L); !R) co_return std::unexpected(R.error());
  co_return asReply<proto::ListReply>(co_await Ex.wait());
}

Coro<Result<uint64_t>> FileClient::submitRead(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle) {
  co_return co_await submitPosted(Path, Offset, Into, nullptr, Into.size(), Handle);
}

Coro<Result<uint64_t>> FileClient::submitRead(const std::string &Path, uint64_t Offset, Page &Into, uint64_t Handle) {
  Page *Landing = Into.region() ? &Into : nullptr;
  co_return co_await submitPosted(Path, Offset, std::span<std::byte>{Into.bytes(), Into.capacity()}, Landing, Into.capacity(), Handle);
}

Coro<Result<ReadOutcome>> FileClient::read(const std::string &Path, uint64_t Offset, Page &Into, uint64_t Handle) {
  auto Sent = co_await submitRead(Path, Offset, Into, Handle);
  if (!Sent) co_return std::unexpected(Sent.error());
  co_return co_await collectRead(*Sent);
}

Coro<Result<uint64_t>>
FileClient::submitPosted(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, Page *Landing, size_t Want, uint64_t Handle) {
  if (Want == 0) co_return failMessage("read of no bytes");
  if (Want > maxTransfer()) co_return failMessage(std::format("read of {} bytes exceeds the page size", Want));
  if (P->Reads.size() >= maxOutstanding()) co_return failMessage("too many reads in flight for the buffer pool");

  proto::ReadRequest Rd;
  Rd.Id = P->NextId++;
  Rd.Path = Path;
  Rd.Offset = Offset;
  Rd.Length = static_cast<uint32_t>(Want);
  Rd.Handle = Handle;

  // Its own entry from here on. Both awaits below can let another read in, and
  // the last thing this one may do is trust the back of the queue to be its.
  P->Reads.push_back(std::make_unique<Impl::Posted>());
  Impl::Posted *Mine = P->Reads.back().get();
  Mine->Id = Rd.Id;
  Mine->Into = Into;
  Mine->Wait = std::make_unique<Impl::Exchange>(P.get(), Rd.Id);

  if (auto R = co_await P->Control.send(Rd); !R) {
    P->forget(Mine);
    co_return std::unexpected(R.error());
  }

  if (Landing) {
    Landing->resize(Want);
    Mine->Direct = Landing;
    Mine->Op = P->Channel->recv(*Landing, Rd.Id, Want);
    Mine->Op.start();
    Memory::get().countDirect();
    co_return Rd.Id;
  }

  Mine->Buf = co_await P->Channel->pool().acquire();
  if (!Mine->Buf.valid()) {
    P->forget(Mine);
    co_return failMessage("out of registered memory for a transfer page");
  }
  Mine->Buf.resize(Want);
  Mine->Op = P->Channel->recv(Mine->Buf, Rd.Id, Want);
  Mine->Op.start();
  Memory::get().countCopied();
  co_return Rd.Id;
}

Coro<Result<ReadOutcome>> FileClient::collectRead() {
  if (P->Reads.empty()) co_return failMessage("no read in flight");
  co_return co_await collectRead(P->Reads.front()->Id);
}

Coro<Result<ReadOutcome>> FileClient::collectRead(uint64_t Id) {
  Impl::Posted *Mine = P->posted(Id);
  if (!Mine) co_return failMessage(std::format("no read in flight with id {}", Id));

  auto Reply = asReply<proto::TransferReply>(co_await Mine->Wait->wait());
  if (!Reply) {
    // The receive is still posted against this buffer, and a failed reply is
    // no reason to hand it back to the pool underneath the fabric. It ends one
    // way or another: a dead endpoint fails it, and silence hits the transfer
    // timeout.
    [[maybe_unused]] auto Landed = co_await Mine->Op.join();
    P->forget(Mine);
    co_return std::unexpected(Reply.error());
  }

  // Joined before the entry goes anywhere: the receive owns this buffer by
  // address until it finishes.
  auto Landed = co_await Mine->Op.join();
  bool Intact = true;
  if (Landed && Reply->Ok) {
    Page &Where = Mine->Direct ? *Mine->Direct : Mine->Buf;
    Verifier PageHash(P->Verify, P->Agreed);
    PageHash.update({Where.bytes(), Where.size()});
    Intact = PageHash.matches(Reply->Payload);
    if (Intact && !Mine->Direct) std::memcpy(Mine->Into.data(), Where.data().data(), Reply->Length);
  }
  P->forget(Mine);

  if (!Reply->Ok) co_return failMessage(Reply->Error);
  if (!Landed) co_return std::unexpected(Landed.error());
  if (!Intact) co_return failMessage("page hash mismatch");
  co_return ReadOutcome{static_cast<size_t>(Reply->Length), Reply->FileSize};
}

Coro<Result<ReadOutcome>> FileClient::read(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle) {
  auto Sent = co_await submitRead(Path, Offset, Into, Handle);
  if (!Sent) co_return std::unexpected(Sent.error());
  co_return co_await collectRead(*Sent);
}

Coro<Result<void>> FileClient::write(const std::string &Path, uint64_t Offset, std::span<const std::byte> From, bool Truncate, uint64_t Handle) {
  if (From.size() > maxTransfer()) co_return failMessage(std::format("write of {} bytes exceeds the page size", From.size()));

  proto::WriteRequest Wr;
  Wr.Id = P->NextId++;
  Wr.Path = Path;
  Wr.Offset = Offset;
  Wr.Length = static_cast<uint32_t>(From.size());
  Wr.Truncate = Truncate;
  Wr.Handle = Handle;

  Verifier PageHash(P->Verify, P->Agreed);
  PageHash.update(From);
  Wr.Payload = PageHash.digest();

  Impl::Exchange Ex(P.get(), Wr.Id);
  if (auto R = co_await P->Control.send(Wr); !R) co_return std::unexpected(R.error());

  Page Buf;
  Buf = co_await P->Channel->pool().acquire();
  if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");
  Buf.resize(From.size());
  if (!From.empty()) std::memcpy(Buf.bytes(), From.data(), From.size());
  if (auto R = co_await P->Channel->send(Buf, Wr.Id); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::TransferReply>(co_await Ex.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);
  co_return Result<void>{};
}

namespace {

proto::MetaRequest metaOf(proto::MetaOp Op, const std::string &Path) {
  proto::MetaRequest Meta;
  Meta.Op = Op;
  Meta.Path = Path;
  return Meta;
}

} // namespace

Coro<Result<void>> FileClient::sendMeta(const proto::MetaRequest &Given) {
  proto::MetaRequest Meta = Given;
  Meta.Id = P->NextId++;

  Impl::Exchange Ex(P.get(), Meta.Id);
  if (auto R = co_await P->Control.send(Meta); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::MetaReply>(co_await Ex.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return refusal(*Reply);
  co_return Result<void>{};
}

Coro<Result<void>> FileClient::makeDirectory(const std::string &Path, uint32_t Mode) {
  auto Meta = metaOf(proto::MetaOp::MakeDirectory, Path);
  Meta.Mode = Mode;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::removeFile(const std::string &Path) { co_return co_await sendMeta(metaOf(proto::MetaOp::RemoveFile, Path)); }

Coro<Result<void>> FileClient::removeDirectory(const std::string &Path) { co_return co_await sendMeta(metaOf(proto::MetaOp::RemoveDirectory, Path)); }

Coro<Result<void>> FileClient::makeLink(const std::string &Path, const std::string &Target) {
  auto Meta = metaOf(proto::MetaOp::Symlink, Path);
  Meta.Target = Target;
  co_return co_await sendMeta(Meta);
}

// Its own call rather than sendMeta, which throws the reply away: the target is
// the answer here.
Coro<Result<std::string>> FileClient::readLink(const std::string &Path) {
  proto::MetaRequest Meta = metaOf(proto::MetaOp::ReadLink, Path);
  Meta.Id = P->NextId++;

  Impl::Exchange Ex(P.get(), Meta.Id);
  if (auto R = co_await P->Control.send(Meta); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::MetaReply>(co_await Ex.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return refusal(*Reply);
  co_return Reply->Target;
}

Coro<Result<void>> FileClient::hardLink(const std::string &Path, const std::string &Target) {
  auto Meta = metaOf(proto::MetaOp::HardLink, Path);
  Meta.Target = Target;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::rename(const std::string &From, const std::string &To) {
  auto Meta = metaOf(proto::MetaOp::Rename, From);
  Meta.Target = To;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::truncate(const std::string &Path, uint64_t Size) {
  auto Meta = metaOf(proto::MetaOp::Truncate, Path);
  Meta.Size = Size;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::setMode(const std::string &Path, uint32_t Mode) {
  auto Meta = metaOf(proto::MetaOp::SetMode, Path);
  Meta.Mode = Mode;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::setMtime(const std::string &Path, int64_t Mtime) {
  auto Meta = metaOf(proto::MetaOp::SetMtime, Path);
  Meta.Mtime = Mtime;
  co_return co_await sendMeta(Meta);
}

Coro<Result<void>> FileClient::fsync(const std::string &Path, uint64_t Handle) {
  proto::MetaRequest Meta = metaOf(proto::MetaOp::Fsync, Path);
  Meta.Handle = Handle;
  co_return co_await sendMeta(Meta);
}

Coro<Result<proto::StatFsReply>> FileClient::statFs(const std::string &Path) {
  proto::StatFsRequest Fs;
  Fs.Id = P->NextId++;
  Fs.Path = Path;

  Impl::Exchange Ex(P.get(), Fs.Id);
  if (auto R = co_await P->Control.send(Fs); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::StatFsReply>(co_await Ex.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);
  co_return *Reply;
}

Coro<Result<uint64_t>> FileClient::fetch(const std::string &Path, const std::filesystem::path &Local) {
  co_await P->Streaming.take();
  const Impl::Streamer Alone(P->Streaming);

  const uint64_t PageBytes = P->Channel->pool().pageSize();

  proto::FetchRequest F;
  F.Id = P->NextId++;
  F.TagBase = P->TagCursor;
  F.Path = Path;
  F.Offset = 0;
  F.Length = ~uint64_t{0};

  Impl::Exchange Opened(P.get(), F.Id);
  Impl::Exchange Finished(P.get(), F.Id);
  if (auto R = co_await P->Control.send(F); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::StreamReply>(co_await Opened.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);

  P->TagCursor += proto::tagSpan(Reply->Length, PageBytes);
  if (Reply->Length == 0) co_return uint64_t{0};

  FileMeta Meta;
  Meta.Size = Reply->Length;
  Meta.Mode = 0644;
  auto Sink = FileWriter::create(Local, Meta, Durability::PageCache, PageBytes % kDirectAlignment == 0);
  if (!Sink) co_return std::unexpected(Sink.error());

  FileSink Landing(*Sink);
  PageReceiver Receiver(*P->Channel, Landing, F.TagBase, StreamGeometry::forChannel(*P->Channel), P->Verify, P->Agreed);
  auto Landed = co_await Receiver.land(0, Reply->Length);
  if (!Landed) co_return std::unexpected(Landed.error());

  auto Sent = asReply<proto::StreamDigest>(co_await Finished.wait());
  if (!Sent) co_return std::unexpected(Sent.error());
  if (!Sent->Ok) co_return failMessage(Sent->Error);
  if (!Receiver.matches(Sent->Whole)) co_return failMessage("whole-transfer hash mismatch");

  if (auto R = Sink->commit(); !R) co_return std::unexpected(R.error());
  Trace::dump("client");
  co_return Reply->Length;
}

Coro<Result<uint64_t>> FileClient::store(const std::filesystem::path &Local, const std::string &Path) {
  co_await P->Streaming.take();
  const Impl::Streamer Alone(P->Streaming);

  const uint64_t PageBytes = P->Channel->pool().pageSize();

  auto Source = FileReader::open(Local, Access::Direct);
  if (!Source) co_return std::unexpected(Source.error());
  const uint64_t Size = Source->meta().Size;

  proto::StoreRequest St;
  St.Id = P->NextId++;
  St.TagBase = P->TagCursor;
  St.Path = Path;
  St.Offset = 0;
  St.Length = Size;
  St.Truncate = true;

  Impl::Exchange Ready(P.get(), St.Id);
  Impl::Exchange Done(P.get(), St.Id);

  // Held until the digest goes out. The daemon reads the frames that follow a
  // store straight off this channel, so a request another writer sends in
  // between is taken for one of them and the session dies.
  co_await P->Control.claim();
  const proto::ControlChannel::Claim Talking(P->Control);
  if (auto R = co_await P->Control.sendClaimed(St); !R) co_return std::unexpected(R.error());

  P->TagCursor += proto::tagSpan(Size, PageBytes);

  auto Accepted = asReply<proto::StreamReply>(co_await Ready.wait());
  if (!Accepted) co_return std::unexpected(Accepted.error());
  if (!Accepted->Ok) co_return failMessage(Accepted->Error);

  FileSource Reading(*Source);
  const StreamGeometry Geometry = StreamGeometry::forChannel(*P->Channel);
  PageSender Sender(*P->Channel, Reading, St.TagBase, Geometry, P->FlipOneBit, P->Verify, P->Agreed, P->AbortAfterPages);

  proto::StreamDigest Digest;
  Digest.Id = St.Id;
  if (Size > 0) {
    auto Streamed = co_await Sender.stream(0, Size);
    if (!Streamed) {
      Digest.Error = Streamed.error().message();
    } else {
      Digest.Ok = true;
      Digest.Whole = Sender.digest();
    }
  } else {
    Digest.Ok = true;
    Digest.Whole = Sender.digest();
  }
  if (auto R = co_await P->Control.sendClaimed(Digest); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::StreamReply>(co_await Done.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);
  co_return Reply->Length;
}

Coro<Result<uint64_t>> FileClient::fetchInto(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle) {
  BufferSink Landing(Into, Offset);
  co_return co_await fetchThrough(Path, Offset, Into.size(), Landing, Handle);
}

Coro<Result<uint64_t>> FileClient::fetchInto(const std::string &Path, uint64_t Offset, AddressSpace &Into, uint64_t Handle) {
  Into.rebase(Offset);
  AddressSpaceSink Landing(Into);
  co_return co_await fetchThrough(Path, Offset, Into.capacity(), Landing, Handle);
}

Coro<Result<uint64_t>> FileClient::fetchThrough(const std::string &Path, uint64_t Offset, uint64_t Want, PageSink &Landing, uint64_t Handle) {
  co_await P->Streaming.take();
  const Impl::Streamer Alone(P->Streaming);

  const uint64_t PageBytes = P->Channel->pool().pageSize();
  if (Offset % kDirectAlignment != 0)
    co_return failMessage(std::format("a streamed fetch starts on a {} byte boundary, not {}", kDirectAlignment, Offset));
  if (Want == 0) co_return uint64_t{0};

  proto::FetchRequest F;
  F.Id = P->NextId++;
  F.TagBase = P->TagCursor;
  F.Path = Path;
  F.Offset = Offset;
  F.Length = Want;
  F.Handle = Handle;

  Impl::Exchange Opened(P.get(), F.Id);
  Impl::Exchange Finished(P.get(), F.Id);
  if (auto R = co_await P->Control.send(F); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::StreamReply>(co_await Opened.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);

  P->TagCursor += proto::tagSpan(Reply->Length, PageBytes);
  if (Reply->Length == 0) co_return uint64_t{0};

  PageReceiver Receiver(*P->Channel, Landing, F.TagBase, StreamGeometry::forChannel(*P->Channel), P->Verify, P->Agreed);
  auto Landed = co_await Receiver.land(Offset, Reply->Length);
  if (!Landed) co_return std::unexpected(Landed.error());

  auto Sent = asReply<proto::StreamDigest>(co_await Finished.wait());
  if (!Sent) co_return std::unexpected(Sent.error());
  if (!Sent->Ok) co_return failMessage(Sent->Error);
  if (!Receiver.matches(Sent->Whole)) co_return failMessage("whole-transfer hash mismatch");

  co_return Reply->Length;
}

Coro<Result<uint64_t>>
FileClient::storeFrom(std::span<const std::byte> From, const std::string &Path, uint64_t Offset, bool Truncate, uint64_t Handle) {
  BufferSource Outgoing(From, Offset);
  co_return co_await storeThrough(Path, Offset, From.size(), Truncate, Outgoing, Handle);
}

Coro<Result<uint64_t>>
FileClient::storeFrom(AddressSpace &From, size_t Length, const std::string &Path, uint64_t Offset, bool Truncate, uint64_t Handle) {
  From.rebase(Offset);
  AddressSpaceSource Outgoing(From);
  co_return co_await storeThrough(Path, Offset, Length, Truncate, Outgoing, Handle);
}

Coro<Result<uint64_t>>
FileClient::storeThrough(const std::string &Path, uint64_t Offset, uint64_t Length, bool Truncate, PageSource &Outgoing, uint64_t Handle) {
  co_await P->Streaming.take();
  const Impl::Streamer Alone(P->Streaming);

  const uint64_t PageBytes = P->Channel->pool().pageSize();

  proto::StoreRequest St;
  St.Id = P->NextId++;
  St.TagBase = P->TagCursor;
  St.Path = Path;
  St.Offset = Offset;
  St.Length = Length;
  St.Truncate = Truncate;
  St.Handle = Handle;

  Impl::Exchange Ready(P.get(), St.Id);
  Impl::Exchange Done(P.get(), St.Id);

  // Held until the digest goes out. The daemon reads the frames that follow a
  // store straight off this channel, so a request another writer sends in
  // between is taken for one of them and the session dies.
  co_await P->Control.claim();
  const proto::ControlChannel::Claim Talking(P->Control);
  if (auto R = co_await P->Control.sendClaimed(St); !R) co_return std::unexpected(R.error());

  P->TagCursor += proto::tagSpan(Length, PageBytes);

  auto Accepted = asReply<proto::StreamReply>(co_await Ready.wait());
  if (!Accepted) co_return std::unexpected(Accepted.error());
  if (!Accepted->Ok) co_return failMessage(Accepted->Error);

  const StreamGeometry Geometry = StreamGeometry::forChannel(*P->Channel);
  PageSender Sender(*P->Channel, Outgoing, St.TagBase, Geometry, P->FlipOneBit, P->Verify, P->Agreed, P->AbortAfterPages);

  proto::StreamDigest Digest;
  Digest.Id = St.Id;
  if (Length > 0) {
    auto Streamed = co_await Sender.stream(Offset, Length);
    if (!Streamed) {
      Digest.Error = Streamed.error().message();
    } else {
      Digest.Ok = true;
      Digest.Whole = Sender.digest();
    }
  } else {
    Digest.Ok = true;
    Digest.Whole = Sender.digest();
  }
  if (auto R = co_await P->Control.sendClaimed(Digest); !R) co_return std::unexpected(R.error());

  auto Reply = asReply<proto::StreamReply>(co_await Done.wait());
  if (!Reply) co_return std::unexpected(Reply.error());
  if (!Reply->Ok) co_return failMessage(Reply->Error);
  co_return Reply->Length;
}

Coro<void> FileClient::close() {
  [[maybe_unused]] auto Sent = co_await P->Control.send(proto::End{});

  co_await P->Reader.join();

  P->Control.close();
  P->Channel->close();
}

} // namespace rail
