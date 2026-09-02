#include "rail/app/checksum.h"
#include "rail/app/report.h"
#include "rail/app/signature.h"
#include "rail/fs/reader.h"
#include "rail/fs/safe-path.h"
#include "rail/fs/writer.h"
#include "rail/io/stream.h"
#include "rail/io/trace.h"
#include "rail/proto/control-channel.h"
#include "rail/session.h"
#include "rail/transport/data-channel.h"

#include <algorithm>
#include <csignal>
#include <deque>
#include <format>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace rail {

namespace {

class Receive {
public:
  Receive(std::filesystem::path Dst, Durability D) : Dst(std::move(Dst)), Durable(D) {}
  Receive(std::filesystem::path Dst, Durability D, proto::ControlChannel C) : Dst(std::move(Dst)), Durable(D), Control(std::move(C)) {}

  Coro<Result<void>> run();
  const Report &report() const { return Rep; }

private:
  Coro<Result<void>> negotiate();
  Coro<Result<void>> openTarget(const proto::FileHeader &FH);
  Coro<Result<void>> serve();
  Coro<Result<proto::Message>> nextMessage();
  Coro<Result<void>> receiveFile(const proto::FileHeader &FH);
  Coro<Result<void>> receiveStream(const proto::FileHeader &FH);
  Coro<Result<void>> makeDirectory(const proto::FileHeader &FH);
  Coro<Result<void>> makeLink(const proto::FileHeader &FH);

  // Turns a peer-supplied name into a path inside the destination, or fails.
  Result<std::filesystem::path> resolve(const std::string &Name) const;

  // Waits for every posted receive and submitted write, so no buffer is
  // recycled while the fabric or the kernel still owns it.
  Coro<void> quiesce();

  Coro<Result<void>> applyLiteral(const proto::Literal &Lit);

  // Waits until at most Keep receives are still posted, hashing each completed
  // page in arrival order and handing it to the disk.
  Coro<Result<void>> storeReceived(size_t Keep);

  // Waits until at most Keep writes are still in flight.
  Coro<Result<void>> awaitWrites(size_t Keep);

  Coro<Result<void>> applyCopy(const proto::Copy &C);
  Coro<Result<void>> receiveDelta();
  Coro<Result<void>> complete(const proto::Done &D);

  std::filesystem::path Dst;
  Durability Durable = Durability::PageCache;
  bool Recursive = false;
  proto::ControlChannel Control;
  std::unique_ptr<DataChannel> Channel;
  std::optional<FileReader> Basis;
  std::optional<FileWriter> Writer;
  proto::Signature Sig;
  proto::Receipt Received;
  Report Rep;
  Hasher Whole;
  std::vector<std::byte> CopyScratch;

  // Pages handed to io_uring but not yet on disk. Each holds its buffer, since
  // the kernel reads from it until the write completes.
  struct InFlightWrite {
    Page Buf;
    Uring::Write Op;
  };
  std::deque<InFlightWrite> Writing;
  size_t WriteWindow = 1;

  uint64_t TagBase = 0;

  // Receives posted to the fabric but not yet complete.
  struct Posted {
    Page Buf;
    Coro<Result<void>> Op;
    uint64_t Offset = 0;
    uint32_t Length = 0;
  };
  std::deque<Posted> Receiving;
  size_t RecvWindow = 1;
};

Coro<Result<void>> Receive::negotiate() {
  auto H = co_await Control.expect<proto::Hello>();
  if (!H) co_return std::unexpected(H.error());
  if (H->Version != proto::kVersion) co_return failMessage("protocol version mismatch");

  Rep.Backend = H->Backend;
  const size_t PageCount = H->PageCount ? H->PageCount : 6;
  // Split the pool between receives in flight and writes in flight, keeping
  // one page spare so acquiring never waits on work the loop has not begun.
  Recursive = H->Recursive;
  RecvWindow = std::max<size_t>(1, std::min(H->WindowPages ? H->WindowPages : 1, PageCount / 2));
  WriteWindow = PageCount > RecvWindow + 1 ? PageCount - RecvWindow - 1 : 0;
  const size_t PageSize = H->PageSize ? H->PageSize : 64u << 20;
  Channel = makeDataChannel(H->Backend, PageCount, PageSize, "");
  if (!Channel) co_return failMessage(std::format("unknown backend: {}", H->Backend));

  auto Endpoint = co_await Channel->listen();
  if (!Endpoint) co_return std::unexpected(Endpoint.error());

  proto::HelloAck Ack;
  Ack.Backend = H->Backend;
  Ack.ChannelEndpoint = *Endpoint;
  Channel->watch(Control.readFd());
  if (auto R = co_await Control.send(Ack); !R) co_return std::unexpected(R.error());

  if (Channel->wantsPeerEndpoint()) {
    auto Peer = co_await Control.expect<proto::PeerEndpoint>();
    if (!Peer) co_return std::unexpected(Peer.error());
    if (auto R = co_await Channel->attachPeer(Peer->Blob); !R) co_return R;
  }

  co_return co_await Channel->acceptPeer();
}

// A peer names every destination, so every name is checked before it is used.
// An absolute path or a parent reference would land the write outside the
// directory this server was given.
Result<std::filesystem::path> Receive::resolve(const std::string &Name) const {
  if (Name.empty()) return failMessage("refusing empty path from peer");

  auto Safe = underRoot(Dst, Name);
  if (!Safe) return std::unexpected(Safe.error());

  // Without recursion the destination names the file itself unless it is an
  // existing directory, which is how rsync behaves for a single file.
  if (!Recursive) return std::filesystem::is_directory(Dst) ? *Safe : Dst;
  return *Safe;
}

Coro<Result<void>> Receive::makeDirectory(const proto::FileHeader &FH) {
  auto Target = resolve(FH.Name);
  if (!Target) co_return std::unexpected(Target.error());

  std::error_code EC;
  std::filesystem::create_directories(*Target, EC);
  if (EC && !std::filesystem::is_directory(*Target)) co_return failMessage(std::format("mkdir {}: {}", Target->string(), EC.message()));

  if (FH.Mode != 0) ::chmod(Target->c_str(), FH.Mode);
  co_return Result<void>{};
}

// The target is written as the sender read it, never resolved here: it names a
// path on whichever machine ends up following it, and resolving it now would
// turn a link into whatever this side happens to hold at that name.
Coro<Result<void>> Receive::makeLink(const proto::FileHeader &FH) {
  auto Where = resolve(FH.Name);
  if (!Where) co_return std::unexpected(Where.error());
  if (FH.Target.empty()) co_return failMessage(std::format("refusing a link to nowhere: {}", FH.Name));

  std::error_code EC;
  if (Recursive && Where->has_parent_path()) std::filesystem::create_directories(Where->parent_path(), EC);

  // A tree copied twice must land the same way, and symlink() will not replace
  // what is already there.
  std::filesystem::remove(*Where, EC);
  if (::symlink(FH.Target.c_str(), Where->c_str()) != 0) co_return failErrno(std::format("symlink {}", Where->string()));
  co_return Result<void>{};
}

Coro<Result<void>> Receive::openTarget(const proto::FileHeader &FH) {
  FileMeta Meta;
  Meta.Size = FH.Size;
  Meta.Mode = FH.Mode;
  Meta.Mtime = FH.Mtime;

  auto Resolved = resolve(FH.Name);
  if (!Resolved) co_return std::unexpected(Resolved.error());
  const std::filesystem::path Target = *Resolved;

  // Only a recursive transfer may create directories, and only ones below the
  // destination it was given. Creating the destination's own parent would make
  // a mistyped path succeed quietly, where rsync reports it.
  if (Recursive && Target.has_parent_path()) {
    std::error_code EC;
    std::filesystem::create_directories(Target.parent_path(), EC);
  }

  // The basis is the existing destination, opened before the writer creates
  // its temp file and kept open: Copy instructions read from it after the real
  // path has been renamed away.
  if (auto Opened = FileReader::open(Target); Opened) Basis.emplace(std::move(*Opened));

  // Direct writes need every offset aligned, and offsets are page multiples,
  // so an odd page size would leave it to the filesystem to accept or reject.
  const bool Direct = FH.Streamed && Channel->pool().pageSize() % kDirectAlignment == 0;

  auto Created = FileWriter::create(Target, Meta, Durable, Direct);
  if (!Created) co_return std::unexpected(Created.error());
  Writer.emplace(std::move(*Created));

  if (!FH.WantSignature) co_return Result<void>{};

  if (Basis && Basis->meta().Size > 0) {
    const uint32_t Block = chooseBlockLength(FH.Size, Channel->traits().MaxBlockSize);
    auto Built = co_await buildSignature(*Basis, Block, chooseStrongLength(FH.Size, Block));
    if (!Built) co_return std::unexpected(Built.error());
    Sig = std::move(*Built);
  }
  co_return co_await Control.send(Sig);
}

Coro<Result<void>> Receive::applyLiteral(const proto::Literal &Lit) {
  // The pool would otherwise run dry and deadlock: every page would be held by
  // a receive or a write that the loop is not waiting for.
  // Page::resize clamps, so an oversized length would silently truncate the
  // page and only surface much later as a whole-file hash mismatch.
  if (Lit.Length > Channel->pool().pageSize()) co_return failMessage(std::format("literal of {} bytes exceeds the page size", Lit.Length));

  if (auto R = co_await storeReceived(RecvWindow - 1); !R) co_return std::unexpected(R.error());
  if (auto R = co_await awaitWrites(WriteWindow); !R) co_return std::unexpected(R.error());

  Page Buf;
  Buf = co_await Channel->pool().acquire();
  if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");

  // Post and return, so the next control message is read and the next receive
  // posted while the fabric is still filling this page.
  Scoped T("rx.post");
  Receiving.push_back({});
  Posted &P = Receiving.back();
  P.Buf = std::move(Buf);
  P.Offset = Lit.Offset;
  P.Length = Lit.Length;
  P.Op = Channel->recv(P.Buf, TagBase + P.Offset, P.Length);
  P.Op.start();
  co_return Result<void>{};
}

// Completed pages are hashed in arrival order, which is the order the sender
// hashed in, and then handed to io_uring.
Coro<Result<void>> Receive::storeReceived(size_t Keep) {
  while (Receiving.size() > Keep) {
    Posted &P = Receiving.front();

    Result<void> Landed;
    {
      Scoped T("rx.recv");
      Landed = co_await P.Op.join();
    }
    if (!Landed) {
      Receiving.pop_front();
      co_return std::unexpected(Landed.error());
    }

    {
      Scoped T("rx.hash");
      Whole.update(P.Buf.data());
    }

    Scoped T("rx.submit");
    Writing.push_back({});
    InFlightWrite &W = Writing.back();
    W.Buf = std::move(P.Buf);
    const uint64_t Offset = P.Offset;
    Received.LiteralBytes += P.Length;
    Receiving.pop_front();

    // A direct write needs an aligned length, and the padding past the file's
    // end is cut off by the truncate in commit(). Nothing is padded otherwise.
    const size_t Padded = std::min(Writer->writeLength(W.Buf.size()), W.Buf.capacity());
    const std::span<const std::byte> Aligned{W.Buf.bytes(), Padded};

    if (auto R = Writer->submitWrite(W.Op, Aligned, Offset); !R) {
      Writing.pop_back();
      co_return std::unexpected(R.error());
    }
  }
  co_return Result<void>{};
}

Coro<Result<void>> Receive::awaitWrites(size_t Keep) {
  while (Writing.size() > Keep) {
    Scoped T("rx.drain");
    auto Landed = co_await Uring::get().await(Writing.front().Op);
    Writing.pop_front();
    if (!Landed) co_return std::unexpected(Landed.error());
  }
  co_return Result<void>{};
}

Coro<Result<void>> Receive::applyCopy(const proto::Copy &C) {
  if (auto R = co_await storeReceived(0); !R) co_return std::unexpected(R.error());
  if (auto R = co_await awaitWrites(0); !R) co_return std::unexpected(R.error());
  if (!Basis) co_return failMessage("Copy instruction with no basis file");

  const uint32_t Length = blockLengthAt(Sig, C.BlockIndex);
  if (Length == 0) co_return failMessage("Copy references an out-of-range block");

  CopyScratch.resize(Length);
  auto N = co_await Basis->read(CopyScratch, uint64_t(C.BlockIndex) * Sig.BlockLength);
  if (!N) co_return std::unexpected(N.error());
  if (*N != Length) co_return failMessage("short read from basis file");

  Whole.update({CopyScratch.data(), *N});
  if (auto R = co_await Writer->write({CopyScratch.data(), *N}, C.DstOffset); !R) co_return std::unexpected(R.error());
  Received.MatchedBytes += Length;
  co_return Result<void>{};
}

Coro<Result<void>> Receive::complete(const proto::Done &D) {
  if (auto R = co_await storeReceived(0); !R) co_return std::unexpected(R.error());
  if (auto R = co_await awaitWrites(0); !R) co_return std::unexpected(R.error());
  if (Whole.digest() != D.WholeFileHash) co_return failMessage("whole-file hash mismatch");
  {
    Scoped T("rx.commit");
    if (auto R = Writer->commit(); !R) co_return std::unexpected(R.error());
  }
  Trace::dump("rx");

  Rep.Files++;
  Rep.LiteralBytes += Received.LiteralBytes;
  Rep.MatchedBytes += Received.MatchedBytes;
  Rep.FileSize += Received.LiteralBytes + Received.MatchedBytes;
  co_return co_await Control.send(Received);
}

// The fabric and the kernel keep writing into the pages until their operations
// finish, so nothing may return them to the pool before then. Errors here are
// dropped: the caller already has the failure that matters.
Coro<void> Receive::quiesce() {
  while (!Receiving.empty()) {
    [[maybe_unused]] auto Ignored = co_await Receiving.front().Op.join();
    Receiving.pop_front();
  }
  while (!Writing.empty()) {
    [[maybe_unused]] auto Ignored = co_await Uring::get().await(Writing.front().Op);
    Writing.pop_front();
  }
}

Coro<Result<void>> Receive::run() {
  auto R = co_await serve();

  // A transfer that failed leaves receives posted for pages the sender is
  // never going to send. Close first so they fail, or quiesce waits on a peer
  // that has already given up and the whole session hangs behind it.
  if (!R && Channel) Channel->close();

  co_await quiesce();
  co_return R;
}

// Reading the control channel is timed, because how long the receiver sits
// here is what says whether the sender is keeping up.
Coro<Result<proto::Message>> Receive::nextMessage() {
  Scoped T("rx.control");
  co_return co_await Control.receive();
}

Coro<Result<void>> Receive::serve() {
  // A pull hands us the ssh pipes; running under ssh we take stdio ourselves.
  if (!Control.valid()) {
    auto Stdio = proto::ControlChannel::overStdio();
    if (!Stdio) co_return std::unexpected(Stdio.error());
    Control = std::move(*Stdio);
  }

  if (auto R = co_await negotiate(); !R) co_return std::unexpected(R.error());

  for (;;) {
    auto M = co_await nextMessage();
    if (!M) co_return std::unexpected(M.error());

    if (std::get_if<proto::End>(&*M)) co_return Result<void>{};

    if (auto *FH = std::get_if<proto::FileHeader>(&*M)) {
      if (FH->Directory) {
        if (auto R = co_await makeDirectory(*FH); !R) co_return std::unexpected(R.error());
        continue;
      }
      if (FH->Link) {
        if (auto R = co_await makeLink(*FH); !R) co_return std::unexpected(R.error());
        continue;
      }
      if (auto R = co_await receiveFile(*FH); !R) co_return std::unexpected(R.error());
      continue;
    }

    co_return failMessage(std::format("unexpected message {}", proto::typeName(proto::typeOf(*M))));
  }
}

// A streamed file needs no instructions: the pages are the file in order, each
// keyed by its offset, so the receiver posts receives as fast as the pool
// allows instead of waiting for the peer to name each one.
Coro<Result<void>> Receive::receiveStream(const proto::FileHeader &FH) {
  const uint64_t PageBytes = Channel->pool().pageSize();

  for (uint64_t Offset = 0; Offset < FH.Size;) {
    if (auto R = co_await storeReceived(RecvWindow - 1); !R) co_return std::unexpected(R.error());
    if (auto R = co_await awaitWrites(WriteWindow); !R) co_return std::unexpected(R.error());

    Page Buf;
    Buf = co_await Channel->pool().acquire();
    if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");

    const uint32_t Length = static_cast<uint32_t>(std::min<uint64_t>(PageBytes, FH.Size - Offset));

    Scoped T("rx.post");
    Receiving.push_back({});
    Posted &P = Receiving.back();
    P.Buf = std::move(Buf);
    P.Offset = Offset;
    P.Length = Length;
    P.Op = Channel->recv(P.Buf, TagBase + P.Offset, P.Length);
    P.Op.start();
    Offset += Length;
  }

  if (auto R = co_await storeReceived(0); !R) co_return std::unexpected(R.error());
  if (auto R = co_await awaitWrites(0); !R) co_return std::unexpected(R.error());

  auto M = co_await nextMessage();
  if (!M) co_return std::unexpected(M.error());
  if (auto *D = std::get_if<proto::Done>(&*M)) co_return co_await complete(*D);
  co_return failMessage(std::format("expected Done, got {}", proto::typeName(proto::typeOf(*M))));
}

Coro<Result<void>> Receive::receiveDelta() {
  for (;;) {
    auto M = co_await nextMessage();
    if (!M) co_return std::unexpected(M.error());

    if (auto *Lit = std::get_if<proto::Literal>(&*M)) {
      if (auto R = co_await applyLiteral(*Lit); !R) co_return std::unexpected(R.error());
      continue;
    }
    if (auto *C = std::get_if<proto::Copy>(&*M)) {
      if (auto R = co_await applyCopy(*C); !R) co_return std::unexpected(R.error());
      continue;
    }
    if (auto *D = std::get_if<proto::Done>(&*M)) co_return co_await complete(*D);

    co_return failMessage(std::format("unexpected message {}", proto::typeName(proto::typeOf(*M))));
  }
}

// Everything between a FileHeader and its Done belongs to one file.
Coro<Result<void>> Receive::receiveFile(const proto::FileHeader &FH) {
  Whole.reset();
  Received = proto::Receipt{};
  Sig = proto::Signature{};
  Basis.reset();

  if (auto R = co_await openTarget(FH); !R) co_return std::unexpected(R.error());

  auto Received = FH.Streamed ? co_await receiveStream(FH) : co_await receiveDelta();
  if (!Received) co_return std::unexpected(Received.error());

  TagBase += proto::tagSpan(FH.Size, Channel->pool().pageSize());
  co_return Result<void>{};
}

} // namespace

Coro<Result<void>> serveReceive(const std::filesystem::path &Dst, Durability D) {
  Receive R(Dst, D);
  co_return co_await R.run();
}

Coro<Result<Report>> receiveOver(proto::ControlChannel C, const std::filesystem::path &Dst, Durability D) {
  Receive R(Dst, D, std::move(C));
  auto Done = co_await R.run();
  if (!Done) co_return std::unexpected(Done.error());
  co_return R.report();
}

} // namespace rail
