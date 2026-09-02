#include "rail/app/checksum.h"
#include "rail/app/differ.h"
#include "rail/app/signature.h"
#include "rail/fs/reader.h"
#include "rail/fs/writer.h"
#include "rail/io/stream.h"
#include "rail/io/trace.h"
#include "rail/proto/control-channel.h"
#include "rail/session.h"
#include "rail/transport/data-channel.h"
#include "rail/transport/rdma-devices.h"

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

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

Nanos since(Clock::time_point Start) { return std::chrono::duration_cast<Nanos>(Clock::now() - Start); }

// Owns the ssh child pid. The pipe fds are handed to Streams, which own them.
//
// The destructor reaps: a push has many early-return paths between spawning
// ssh and waiting for it, and every one of them would otherwise leave a zombie
// behind. That matters more than it looks, because this is a library and the
// planned FUSE server will run pushes in a long-lived process.
struct Child {
  pid_t Pid = -1;
  int ToChild = -1;
  int FromChild = -1;

  Child() = default;
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;
  Child(Child &&Other) noexcept : Pid(std::exchange(Other.Pid, -1)), ToChild(Other.ToChild), FromChild(Other.FromChild) {}

  Child &operator=(Child &&Other) noexcept {
    if (this != &Other) {
      terminate();
      Pid = std::exchange(Other.Pid, -1);
      ToChild = Other.ToChild;
      FromChild = Other.FromChild;
    }
    return *this;
  }

  ~Child() { terminate(); }

  Result<void> wait() {
    if (Pid < 0) return {};
    int Status = 0;
    const pid_t Waited = ::waitpid(Pid, &Status, 0);
    Pid = -1;
    if (Waited < 0) return failErrno("waitpid");
    if (WIFEXITED(Status) && WEXITSTATUS(Status) == 0) return {};
    return failMessage(std::format("remote rail exited with status {}", WIFEXITED(Status) ? WEXITSTATUS(Status) : -1));
  }

  // Abandoning a transfer must not leave ssh running: the peer would sit in
  // its receive loop until the pipe closes.
  void terminate() noexcept {
    if (Pid < 0) return;
    ::kill(Pid, SIGTERM);
    int Status = 0;
    ::waitpid(Pid, &Status, 0);
    Pid = -1;
  }
};

Result<Child> spawn(const std::vector<std::string> &Argv) {
  int In[2];
  int Out[2];
  if (::pipe(In) != 0) return failErrno("pipe");
  if (::pipe(Out) != 0) {
    ::close(In[0]);
    ::close(In[1]);
    return failErrno("pipe");
  }

  const pid_t Pid = ::fork();
  if (Pid < 0) {
    ::close(In[0]);
    ::close(In[1]);
    ::close(Out[0]);
    ::close(Out[1]);
    return failErrno("fork");
  }

  if (Pid == 0) {
    ::dup2(In[0], STDIN_FILENO);
    ::dup2(Out[1], STDOUT_FILENO);
    ::close(In[0]);
    ::close(In[1]);
    ::close(Out[0]);
    ::close(Out[1]);

    std::vector<char *> Raw;
    Raw.reserve(Argv.size() + 1);
    for (const auto &A : Argv) Raw.push_back(const_cast<char *>(A.c_str()));
    Raw.push_back(nullptr);
    ::execvp(Raw[0], Raw.data());
    ::_exit(127);
  }

  ::close(In[0]);
  ::close(Out[1]);

  Child C;
  C.Pid = Pid;
  C.ToChild = In[1];
  C.FromChild = Out[0];
  if (auto R = setNonBlocking(C.ToChild); !R) return std::unexpected(R.error());
  if (auto R = setNonBlocking(C.FromChild); !R) return std::unexpected(R.error());
  return C;
}

std::string shellQuote(const std::string &Word) {
  std::string Quoted = "'";
  for (const char C : Word) {
    if (C == '\'') Quoted += "'\\''";
    else Quoted += C;
  }
  return Quoted + "'";
}

// The address the far side should dial for a byte-stream transport. ssh puts
// the client's address in the environment, and rdma ignores the host entirely
// because endpoints travel on the control channel.
std::string dialBack() {
  const char *Client = ::getenv("SSH_CLIENT");
  if (!Client) return {};
  const std::string_view Whole(Client);
  return std::string(Whole.substr(0, Whole.find(' ')));
}

// A pull runs the transfer options on the far side, so they travel with the
// command the way rsync passes its flags.
std::vector<std::string> pullArgv(const RemotePath &Src, const SyncOptions &Opts) {
  std::string Remote = Opts.RemoteCommand + " --server --sender";
  if (Opts.Recursive) Remote += " --recursive";
  if (Opts.Policy != DeltaPolicy::Never) Remote += " --no-whole-file";
  if (Opts.BlockSize != 0) Remote += " --block-size " + std::to_string(Opts.BlockSize);
  if (Opts.Backend != "tcp") Remote += " --backend " + shellQuote(Opts.Backend);
  Remote += " " + shellQuote(Src.Path.string());
  return {"ssh", "-o", "BatchMode=yes", Src.Host, Remote};
}

std::vector<std::string> sshArgv(const RemotePath &Dst, const SyncOptions &Opts) {
  std::string Remote = Opts.RemoteCommand + " --server";
  if (Opts.Durable == Durability::Fsync) Remote += " --fsync";
  Remote += " " + shellQuote(Dst.Path.string());
  return {"ssh", "-o", "BatchMode=yes", Dst.Host, Remote};
}

constexpr size_t kReceiptWindow = 64;

// Scanning costs a full read of the source at roughly 1-2 GB/s single
// threaded, against an RDMA path that reaches far more than that, so the delta
// is not always worth computing. Auto is deliberately crude here: it only
// skips the scan when the destination is so different in size that most blocks
// cannot match anyway. Tuning it needs the ScanTime and TransferTime numbers
// the Report now records.
bool shouldUseDelta(const proto::Signature &Sig, uint64_t SourceSize, DeltaPolicy Policy) {
  if (Policy == DeltaPolicy::Never) return false;
  if (Sig.Sums.empty() || Sig.FileLength == 0) return false;
  if (Policy == DeltaPolicy::Always) return true;

  const uint64_t Larger = std::max(SourceSize, Sig.FileLength);
  const uint64_t Smaller = std::min(SourceSize, Sig.FileLength);
  return Smaller * 2 >= Larger; // within 2x
}

// Page size, pool size and window for a transfer of this many bytes.
//
// Pages reach their cap quickly because every page costs a control round trip
// over ssh, about 5 ms, which the fabric measurements do not see: end to end a
// 2 GiB file moves at 1315 MB/s with 64 MiB pages against 773 with 32 MiB.
// Small transfers still get small pages, so a few megabytes does not reserve a
// pool sized for a checkpoint. Depth is deliberately not scaled: with the loop
// driving the fabric, one operation in flight reaches 132 Gbps against 142 for
// sixteen, so a deeper window costs registered memory and buys almost nothing.
SyncOptions withGeometry(SyncOptions Opts, uint64_t Bytes) {
  constexpr uint64_t kSmallestPage = 1u << 20;
  constexpr uint64_t kLargestPage = 64u << 20;

  if (Opts.PageSize == 0) {
    // Enough pages to overlap, so aim for a fraction of the whole transfer.
    uint64_t PageBytes = kSmallestPage;
    while (PageBytes < kLargestPage && PageBytes * 4 < Bytes) PageBytes *= 2;
    Opts.PageSize = static_cast<size_t>(PageBytes);
  }
  if (Opts.WindowPages == 0) Opts.WindowPages = 2;
  if (Opts.PageCount == 0) Opts.PageCount = Opts.WindowPages + 4;

  // One page cannot feed both a read and a send at the same time.
  Opts.PageCount = std::max<size_t>(2, Opts.PageCount);
  return Opts;
}

// One entry in the transfer. Name is where it lands under the destination, and
// is the only path the receiver ever sees.
struct Item {
  std::filesystem::path Local;
  std::string Name;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
  bool Directory = false;
  bool Link = false;
  std::string Target;
};

// Mode and mtime for a directory entry. Regular files carry theirs in the
// FileReader metadata instead.
Item directoryItem(const std::filesystem::path &P, std::string Name) {
  Item It{P, std::move(Name), 0, 0, true};
  struct ::stat St{};
  if (::stat(P.c_str(), &St) == 0) {
    It.Mode = St.st_mode & 07777;
    It.Mtime = St.st_mtime;
  }
  return It;
}

// Mode and mtime taken without following it, which is the only way to see the
// link rather than whatever it names.
Item linkItem(const std::filesystem::path &P, std::string Name, std::string Target) {
  Item It{P, std::move(Name), 0, 0, false, true, std::move(Target)};
  struct ::stat St{};
  if (::lstat(P.c_str(), &St) == 0) {
    It.Mode = St.st_mode & 07777;
    It.Mtime = St.st_mtime;
  }
  return It;
}

// Builds the transfer list. rsync's trailing slash decides whether the source
// directory itself appears under the destination: "dir/" sends its contents,
// "dir" sends the directory. Symlinks and device nodes are skipped rather than
// followed, so a link can never make the transfer escape the tree.
// Every regular file and directory beneath Src, named relative to Root.
bool sizeAllows(const std::filesystem::path &P, const SyncOptions &Opts) {
  if (Opts.MinSize == 0 && Opts.MaxSize == 0) return true;

  std::error_code EC;
  const auto Size = std::filesystem::file_size(P, EC);
  if (EC) return true;
  if (Opts.MinSize && Size < Opts.MinSize) return false;
  if (Opts.MaxSize && Size > Opts.MaxSize) return false;
  return true;
}

Result<std::vector<Item>> walkTree(const std::filesystem::path &Src, const std::filesystem::path &Root, const SyncOptions &Opts) {
  std::error_code EC;
  std::filesystem::recursive_directory_iterator Walk(Src, std::filesystem::directory_options::skip_permission_denied, EC);
  if (EC) return failMessage(std::format("cannot read {}: {}", Src.string(), EC.message()));

  std::vector<Item> Items;
  const std::filesystem::recursive_directory_iterator End;
  for (; Walk != End; Walk.increment(EC)) {
    if (EC) break;
    const auto &Entry = *Walk;

    const auto Relative = std::filesystem::relative(Entry.path(), Root, EC).generic_string();
    if (EC || Relative.empty()) continue;

    // is_directory and is_regular_file follow links, so the link itself has to
    // be tested first or a symlink's target is silently copied as content.
    if (Entry.is_symlink(EC)) {
      // relative() canonicalises, and canonicalising a link gives the name of
      // what it points at - which would file this link under its target's name
      // and replace that file with a link to itself. Lexically, so the path is
      // the one on disk rather than the one it resolves to.
      const auto Named = Entry.path().lexically_relative(Root).generic_string();
      if (Named.empty()) continue;
      if (!Opts.Filter.allows(Named, false)) continue;
      auto Points = std::filesystem::read_symlink(Entry.path(), EC);
      if (EC) {
        std::fprintf(stderr, "rail: skipping %s, cannot read the link: %s\n", Entry.path().string().c_str(), EC.message().c_str());
        continue;
      }
      Items.push_back(linkItem(Entry.path(), Named, Points.generic_string()));
      continue;
    }

    if (Entry.is_directory(EC)) {
      // An excluded directory is never descended into, so a rule naming a
      // directory covers everything under it without naming any of it.
      if (!Opts.Filter.allows(Relative, true)) {
        Walk.disable_recursion_pending();
        continue;
      }
      Items.push_back(directoryItem(Entry.path(), Relative));
    } else if (Entry.is_regular_file(EC)) {
      if (!Opts.Filter.allows(Relative, false)) continue;
      if (!sizeAllows(Entry.path(), Opts)) continue;
      Items.push_back({Entry.path(), Relative, 0, 0, false});
    } else {
      std::fprintf(stderr, "rail: skipping %s, not a regular file\n", Entry.path().string().c_str());
    }
  }
  return Items;
}

// Builds the transfer list. rsync's trailing slash decides whether the source
// directory itself appears under the destination: "dir/" sends its contents,
// "dir" sends the directory.
Result<std::vector<Item>> enumerate(const std::filesystem::path &Src, const SyncOptions &Opts) {
  std::error_code EC;
  const auto Status = std::filesystem::symlink_status(Src, EC);
  if (EC) return failMessage(std::format("cannot stat {}: {}", Src.string(), EC.message()));

  if (std::filesystem::is_regular_file(Status)) {
    const std::string Name = Src.filename().string();
    if (!Opts.Filter.allows(Name, false) || !sizeAllows(Src, Opts)) return std::vector<Item>{};
    return std::vector<Item>{{Src, Name, 0, 0, false}};
  }
  if (!std::filesystem::is_directory(Status)) return failMessage(std::format("{} is not a regular file or directory", Src.string()));

  if (!Opts.Recursive) {
    std::fprintf(stderr, "rail: skipping directory %s\n", Src.string().c_str());
    return std::vector<Item>{};
  }

  // A bare relative name like "data" has no parent component, and relativising
  // against an empty path yields empty names, which silently sends nothing.
  std::filesystem::path Root = Opts.SourceContentsOnly ? Src : Src.parent_path();
  if (Root.empty()) Root = ".";

  auto Items = walkTree(Src, Root, Opts);
  if (!Items) return std::unexpected(Items.error());

  // With a trailing slash the source directory itself is not part of the
  // transfer, so its mode must not be stamped onto the destination.
  if (!Opts.SourceContentsOnly) Items->push_back(directoryItem(Src, std::filesystem::relative(Src, Root, EC).generic_string()));

  // A directory has to exist before anything lands inside it.
  std::sort(Items->begin(), Items->end(), [](const Item &A, const Item &B) { return A.Name < B.Name; });
  const bool Filtering = !Opts.Filter.empty() || Opts.MinSize != 0 || Opts.MaxSize != 0;
  if (Items->empty() && !Filtering) return failMessage(std::format("recursing {} produced nothing to send", Src.string()));
  return Items;
}

// Sender side. Split into named steps so the flow reads as a sequence rather
// than one long function.
class Push {
public:
  Push(proto::ControlChannel C, std::unique_ptr<DataChannel> Ch, SyncOptions O) : Control(std::move(C)), Channel(std::move(Ch)), Opts(std::move(O)) {
    Window = std::max<size_t>(1, std::min({Opts.WindowPages, Channel->traits().MaxInFlight, Opts.PageCount - 1}));
    ReadWindow = std::clamp<size_t>(Opts.PageCount - Window, 1, Uring::depth());
  }

  Push(Child K, std::unique_ptr<DataChannel> C, SyncOptions O)
      : Kid(std::move(K)), Control(Stream(Kid.FromChild), Stream(Kid.ToChild)), Channel(std::move(C)), Opts(std::move(O)) {
    // The pool feeds reading and sending at once, so both windows have to fit
    // inside it: asking for more than it holds waits for a page that only this
    // coroutine could give back. A byte-stream channel also caps sends at one,
    // since concurrent sends would interleave on the wire.
    Window = std::max<size_t>(1, std::min({Opts.WindowPages, Channel->traits().MaxInFlight, Opts.PageCount - 1}));
    ReadWindow = std::clamp<size_t>(Opts.PageCount - Window, 1, Uring::depth());
  }

  Coro<Result<Report>> run(const std::vector<Item> &Items);

private:
  Coro<Result<void>> carry(const std::vector<Item> &Items);
  Coro<Result<Report>> push(const std::vector<Item> &Items);
  Coro<Result<void>> pushOne(const Item &It);
  Coro<Result<void>> sendDirectory(const Item &It);
  Coro<Result<void>> sendLink(const Item &It);

  // Waits for every started send, which reads from a buffer this object owns.
  Coro<void> quiesce();

  Coro<Result<void>> negotiate();
  Coro<Result<proto::Signature>> exchangeHeader(const Item &It);
  Coro<Result<void>> sendLiteral(uint64_t SrcOffset, uint64_t DstOffset, uint32_t Length);
  Coro<Result<size_t>> sendPage(uint64_t Src, uint64_t Dst, uint32_t Left);
  void corruptOnceIfAsked(Page &Buf);

  // Counts bytes accounted for in this file, matched as well as sent.
  void advanced(uint64_t Bytes) {
    MovingDone += Bytes;
    if (Opts.OnProgress) Opts.OnProgress(Moving, MovingDone, MovingSize);
  }

  // Waits until at most Keep sends are still in flight.
  Coro<Result<void>> awaitSends(size_t Keep);

  Coro<Result<void>> sendWholeFile();

  // Tops up the reads in flight, submitting but not waiting.
  Coro<Result<void>> fill(uint64_t &Offset);

  // Sends the oldest page whose read has finished.
  Coro<Result<void>> shipReady();
  Coro<Result<void>> sendDelta(const proto::Signature &Sig);
  Coro<Result<void>> confirm();

  Coro<Result<void>> reapReceipts(size_t Keep);

  std::optional<FileReader> Reader;
  Child Kid;
  proto::ControlChannel Control;
  std::unique_ptr<DataChannel> Channel;
  SyncOptions Opts;
  size_t Window = 1;
  bool Corrupted = false;
  // Set before the header goes out, so the receiver knows no instructions are
  // coming and can derive the page layout itself.
  bool Streaming = false;
  // Whether the source was opened for direct reads, which fixes the read size.
  bool Aligned = false;

  // The file being sent, for progress reporting.
  std::string Moving;
  uint64_t MovingSize = 0;
  uint64_t MovingDone = 0;
  Hasher Whole;
  Report Rep;
  // Reset for every file, since each carries its own digest and counters.
  Report File;

  // Sends handed to the channel but not yet complete. Each keeps its buffer,
  // which the transport reads from until the send lands.
  struct Outstanding {
    Page Buf;
    Coro<Result<void>> Op;
  };
  std::deque<Outstanding> InFlight;

  // Pages read from the source but not yet sent.
  struct Reading {
    Page Buf;
    Uring::Read Op;
    uint64_t Offset = 0;
    uint32_t Length = 0;
  };
  std::deque<Reading> Prefetch;
  size_t ReadWindow = 1;

  struct Awaited {
    std::string Name;
    uint64_t LiteralBytes = 0;
    uint64_t MatchedBytes = 0;
  };
  std::deque<Awaited> AwaitingReceipt;

  uint64_t TagBase = 0;
};

Coro<Result<void>> Push::awaitSends(size_t Keep) {
  while (InFlight.size() > Keep) {
    Scoped T("tx.retire");
    auto Landed = co_await InFlight.front().Op.join();
    InFlight.pop_front();
    if (!Landed) co_return std::unexpected(Landed.error());
  }
  co_return Result<void>{};
}

Coro<Result<void>> Push::negotiate() {
  if (auto R = Channel->prepare(); !R) co_return std::unexpected(R.error());

  proto::Hello H;
  H.Backend = Opts.Backend;
  H.BlockSize = Opts.BlockSize;
  H.PageCount = Opts.PageCount;
  H.PageSize = Opts.PageSize;
  H.WindowPages = Window;
  H.Recursive = Opts.Recursive;
  if (auto R = co_await Control.send(H); !R) co_return std::unexpected(R.error());

  auto Ack = co_await Control.expect<proto::HelloAck>();
  if (!Ack) co_return std::unexpected(Ack.error());
  if (Ack->Backend != Opts.Backend) co_return failMessage(std::format("server refused backend {}", Opts.Backend));

  Channel->watch(Control.readFd());
  if (auto R = co_await Channel->connect(Ack->ChannelEndpoint); !R) co_return R;

  // A queue pair has to know both ends before either can use it, so the side
  // that dialled sends its own endpoint back. A transport that does not ask
  // for one is left exactly as it was.
  if (!Channel->wantsPeerEndpoint()) co_return Result<void>{};

  auto Local = co_await Channel->localEndpoint();
  if (!Local) co_return std::unexpected(Local.error());

  proto::PeerEndpoint Mine;
  Mine.Blob = *Local;
  co_return co_await Control.send(Mine);
}

Coro<Result<proto::Signature>> Push::exchangeHeader(const Item &It) {
  proto::FileHeader FH;
  FH.Name = It.Name;
  FH.WantSignature = Opts.Policy != DeltaPolicy::Never;
  FH.Streamed = Streaming;
  FH.Size = Reader->meta().Size;
  FH.Mode = Reader->meta().Mode;
  FH.Mtime = Reader->meta().Mtime;
  if (auto R = co_await Control.send(FH); !R) co_return std::unexpected(R.error());
  if (!FH.WantSignature) co_return proto::Signature{};

  if (auto R = co_await reapReceipts(0); !R) co_return std::unexpected(R.error());
  co_return co_await Control.expect<proto::Signature>();
}

// Reads one page, announces it on the control channel and starts its transfer.
// Returns how many bytes the page covered.
// Fault injection for the end-to-end tests. Corrupting after the page has been
// hashed leaves the wire disagreeing with the digest, which is exactly how a
// silent fabric or disk fault presents.
void Push::corruptOnceIfAsked(Page &Buf) {
  if (!Opts.FlipOneLiteralBit || Corrupted) return;
  Corrupted = true;
  Buf.bytes()[0] ^= std::byte{0x01};
}

Coro<Result<size_t>> Push::sendPage(uint64_t Src, uint64_t Dst, uint32_t Left) {
  Page Buf;
  Buf = co_await Channel->pool().acquire();
  if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");
  Buf.resize(static_cast<size_t>(std::min<uint64_t>(Buf.capacity(), Left)));

  Result<size_t> N;
  {
    Scoped T("tx.read");
    N = co_await Reader->read(Buf.data(), Src);
  }
  if (!N) co_return std::unexpected(N.error());
  if (*N == 0) co_return failMessage("short read on source");
  Buf.resize(*N);

  {
    // Hashing stays on this path so the digest follows destination order.
    Scoped T("tx.hash");
    Whole.update(Buf.data());
  }

  corruptOnceIfAsked(Buf);

  // A streamed transfer names nothing: the receiver derives every page from
  // the file size, so this round trip over a slow link disappears entirely.
  if (!Streaming) {
    proto::Literal Lit;
    Lit.Offset = Dst;
    Lit.Length = static_cast<uint32_t>(*N);

    Scoped T("tx.control");
    if (auto R = co_await Control.send(Lit); !R) co_return std::unexpected(R.error());
  }

  // The buffer must outlive the send and must not move: the operation holds it
  // by reference, so it is parked first and the coroutine created against its
  // final address.
  Scoped T("tx.send");
  InFlight.push_back({});
  InFlight.back().Buf = std::move(Buf);
  InFlight.back().Op = Channel->send(InFlight.back().Buf, TagBase + Dst);
  InFlight.back().Op.start();
  co_return *N;
}

Coro<Result<void>> Push::sendLiteral(uint64_t SrcOffset, uint64_t DstOffset, uint32_t Length) {
  uint32_t Left = Length;
  uint64_t Src = SrcOffset;
  uint64_t Dst = DstOffset;

  while (Left > 0) {
    if (auto R = co_await awaitSends(Window - 1); !R) co_return std::unexpected(R.error());

    auto Sent = co_await sendPage(Src, Dst, Left);
    if (!Sent) co_return std::unexpected(Sent.error());

    Src += *Sent;
    Dst += *Sent;
    Left -= static_cast<uint32_t>(*Sent);
    advanced(*Sent);
  }
  co_return Result<void>{};
}

uint64_t alignUp(uint64_t N) { return (N + kDirectAlignment - 1) & ~(kDirectAlignment - 1); }

// Keeps several reads in flight so the device is asked for more than one page
// at a time. One reader at a time gets 1.2 GB/s from this disk where four get
// 2.8, and on a cold source the read is most of the transfer.
Coro<Result<void>> Push::fill(uint64_t &Offset) {
  const uint64_t Size = Reader->meta().Size;

  while (Prefetch.size() < ReadWindow && Offset < Size) {
    Page Buf;
    Buf = co_await Channel->pool().acquire();
    if (!Buf.valid()) co_return failMessage("out of registered memory for a transfer page");
    Buf.resize(static_cast<size_t>(std::min<uint64_t>(Buf.capacity(), Size - Offset)));

    Prefetch.push_back({});
    Reading &R = Prefetch.back();
    R.Buf = std::move(Buf);
    R.Offset = Offset;
    R.Length = static_cast<uint32_t>(R.Buf.size());

    // A direct read needs an aligned length. The last page asks for more than
    // the file holds and simply comes back short.
    const size_t Ask = Aligned ? std::min(alignUp(R.Length), R.Buf.capacity()) : R.Length;

    if (auto S = Reader->submitRead(R.Op, {R.Buf.bytes(), Ask}, R.Offset); !S) {
      Prefetch.pop_back();
      co_return std::unexpected(S.error());
    }
    Offset += R.Length;
  }
  co_return Result<void>{};
}

// Hands one page that has finished reading to the fabric.
Coro<Result<void>> Push::shipReady() {
  // Awaited in place: the submission gave the kernel the address of this
  // operation, so moving it out of the deque first would leave the completion
  // landing on the old one and this wait never ending.
  Reading &Ready = Prefetch.front();

  Result<size_t> N;
  {
    Scoped T("tx.read");
    N = co_await Reader->awaitRead(Ready.Op);
  }
  if (!N) co_return std::unexpected(N.error());
  if (*N < Ready.Length) co_return failMessage("short read on source");

  {
    // Hashing follows destination order, which is the order pages complete.
    Scoped T("tx.hash");
    Whole.update(Ready.Buf.data());
  }
  corruptOnceIfAsked(Ready.Buf);

  const uint64_t At = Ready.Offset;
  const uint32_t Length = Ready.Length;

  // A streamed receiver derives the page itself. Any other one is told,
  // because it has no way to know where this page belongs.
  if (!Streaming) {
    proto::Literal Lit;
    Lit.Offset = At;
    Lit.Length = Length;

    Scoped C("tx.control");
    if (auto R = co_await Control.send(Lit); !R) co_return std::unexpected(R.error());
  }

  Scoped T("tx.send");
  InFlight.push_back({});
  InFlight.back().Buf = std::move(Ready.Buf);
  Prefetch.pop_front();

  InFlight.back().Op = Channel->send(InFlight.back().Buf, TagBase + At);
  InFlight.back().Op.start();

  File.LiteralBytes += Length;
  advanced(Length);
  co_return Result<void>{};
}

Coro<Result<void>> Push::sendWholeFile() {
  uint64_t Offset = 0;

  while (Offset < Reader->meta().Size || !Prefetch.empty()) {
    if (auto R = co_await fill(Offset); !R) co_return std::unexpected(R.error());
    if (Prefetch.empty()) break;

    if (auto R = co_await awaitSends(Window - 1); !R) co_return std::unexpected(R.error());
    if (auto R = co_await shipReady(); !R) co_return std::unexpected(R.error());
  }
  co_return Result<void>{};
}

Coro<Result<void>> Push::sendDelta(const proto::Signature &Sig) {
  const auto ScanStart = Clock::now();
  Differ D(Sig);

  InstructionSink Sink = [&](const Instruction &Ins) -> Coro<Result<void>> {
    if (Ins.K == Instruction::Kind::Literal) co_return co_await sendLiteral(Ins.SrcOffset, Ins.DstOffset, Ins.Length);

    // A matched block is byte-identical on both sides, so only the instruction
    // travels. Its bytes come straight from the differ's window, which is why
    // hashing a copy costs no extra read.
    Whole.update(Ins.Bytes);
    advanced(Ins.Bytes.size());

    proto::Copy C;
    C.BlockIndex = Ins.BlockIndex;
    C.Count = 1;
    C.DstOffset = Ins.DstOffset;
    co_return co_await Control.send(C);
  };

  if (auto R = co_await D.diff(*Reader, Sink, File); !R) co_return std::unexpected(R.error());
  File.ScanTime = since(ScanStart);
  co_return Result<void>{};
}

Coro<Result<void>> Push::confirm() {
  proto::Done D;
  D.WholeFileHash = Whole.digest();
  if (auto R = co_await Control.send(D); !R) co_return std::unexpected(R.error());

  AwaitingReceipt.push_back({Moving, File.LiteralBytes, File.MatchedBytes});
  co_return co_await reapReceipts(kReceiptWindow);
}

// The sender's counters are authoritative: HashHits and FalseAlarms come from
// the scan, which only runs here. Taking them from the receiver, which never
// scans, reported zero for both on every transfer. The receiver's tally is
// still worth having as a check that both sides agree on what moved.
Coro<Result<void>> Push::reapReceipts(size_t Keep) {
  while (AwaitingReceipt.size() > Keep) {
    auto Got = co_await Control.expect<proto::Receipt>();
    if (!Got) co_return std::unexpected(Got.error());

    const Awaited Expected = std::move(AwaitingReceipt.front());
    AwaitingReceipt.pop_front();

    if (Got->LiteralBytes != Expected.LiteralBytes || Got->MatchedBytes != Expected.MatchedBytes) {
      co_return failMessage(std::format("sender and receiver disagree on {}: sender sent {} literal / {} matched, receiver saw {} / {}",
                                        Expected.Name,
                                        Expected.LiteralBytes,
                                        Expected.MatchedBytes,
                                        Got->LiteralBytes,
                                        Got->MatchedBytes));
    }
  }
  co_return Result<void>{};
}

Coro<void> Push::quiesce() {
  while (!InFlight.empty()) {
    [[maybe_unused]] auto Ignored = co_await InFlight.front().Op.join();
    InFlight.pop_front();
  }
}

Coro<Result<Report>> Push::run(const std::vector<Item> &Items) {
  auto R = co_await push(Items);
  co_await quiesce();
  co_return R;
}

// One file: header, signature exchange, payload, digest check. The session
// stays open across files, so the ssh pipe and the fabric endpoint are set up
// once for the whole tree.
// A directory is one message: it carries no payload and needs no signature.
// One message, like a directory: what a link points at is all of it.
Coro<Result<void>> Push::sendLink(const Item &It) {
  proto::FileHeader FH;
  FH.Name = It.Name;
  FH.Mode = It.Mode;
  FH.Mtime = It.Mtime;
  FH.Link = true;
  FH.Target = It.Target;
  co_return co_await Control.send(FH);
}

Coro<Result<void>> Push::sendDirectory(const Item &It) {
  proto::FileHeader FH;
  FH.Name = It.Name;
  FH.Mode = It.Mode;
  FH.Mtime = It.Mtime;
  FH.Directory = true;
  co_return co_await Control.send(FH);
}

Coro<Result<void>> Push::pushOne(const Item &It) {
  if (It.Directory) co_return co_await sendDirectory(It);
  if (It.Link) co_return co_await sendLink(It);

  // Decided before the source is opened: only a streamed transfer reads whole
  // pages in order, which is what direct reads require. A delta scans the file
  // at arbitrary offsets and must stay buffered.
  Streaming = Opts.Policy == DeltaPolicy::Never && Channel->traits().IsRdma;

  auto Opened = FileReader::open(It.Local, Streaming ? Access::Direct : Access::Buffered);
  if (!Opened) co_return std::unexpected(Opened.error());
  Reader.emplace(std::move(*Opened));
  Aligned = Reader->direct();

  File = Report{};
  Whole.reset();
  Corrupted = false;
  Moving = It.Name;
  MovingSize = Reader->meta().Size;
  MovingDone = 0;

  auto Sig = co_await exchangeHeader(It);
  if (!Sig) co_return std::unexpected(Sig.error());

  const bool Delta = shouldUseDelta(*Sig, Reader->meta().Size, Opts.Policy);
  auto Sent = Delta ? co_await sendDelta(*Sig) : co_await sendWholeFile();
  if (!Sent) co_return std::unexpected(Sent.error());

  // The window leaves sends in flight, and those bytes are not transferred
  // until they land. Counting the file as done before draining would credit us
  // with a whole window of pages we have not finished sending.
  if (auto R = co_await awaitSends(0); !R) co_return std::unexpected(R.error());
  if (auto R = co_await confirm(); !R) co_return std::unexpected(R.error());

  TagBase += proto::tagSpan(Reader->meta().Size, Channel->pool().pageSize());

  Rep.Files++;
  Rep.FileSize += Reader->meta().Size;
  if (Opts.OnFile) Opts.OnFile(It.Name, Reader->meta().Size);
  Rep.LiteralBytes += File.LiteralBytes;
  Rep.MatchedBytes += File.MatchedBytes;
  Rep.HashHits += File.HashHits;
  Rep.FalseAlarms += File.FalseAlarms;
  Rep.ScanTime += File.ScanTime;
  Rep.DeltaUsed = Rep.DeltaUsed || Delta;
  co_return Result<void>{};
}

Coro<Result<void>> Push::carry(const std::vector<Item> &Items) {
  if (auto R = co_await negotiate(); !R) co_return R;

  const auto Start = Clock::now();
  for (const Item &It : Items)
    if (auto R = co_await pushOne(It); !R) co_return R;

  if (auto R = co_await reapReceipts(0); !R) co_return R;

  // Nothing distinguishes the last file from a peer that died between files
  // unless the stream is closed explicitly.
  if (auto R = co_await Control.send(proto::End{}); !R) co_return R;
  Rep.TransferTime = since(Start);
  co_return Result<void>{};
}

Coro<Result<Report>> Push::push(const std::vector<Item> &Items) {
  Rep.Backend = Opts.Backend;
  if (Channel->traits().IsRdma) Rep.Rails = describe(activeRdmaPorts());

  auto Outcome = co_await carry(Items);
  Trace::dump("tx");

  // Every exit closes, success or not. A transfer that gave up part way still
  // has sends posted, and leaving them for the destructor abandons operations
  // the adapter is still working through.
  Control.close();
  Channel->close();

  if (!Outcome) co_return std::unexpected(Outcome.error());
  if (auto R = Kid.wait(); !R) co_return std::unexpected(R.error());
  co_return Rep;
}

// Receiver side, running on the far end of the ssh pipe.
} // namespace

Result<RemotePath> RemotePath::parse(std::string_view Spec) {
  const auto Colon = Spec.find(':');
  if (Colon == std::string_view::npos || Colon == 0) return failMessage(std::format("expected host:path, got {}", Spec));
  RemotePath R;
  R.Host = std::string(Spec.substr(0, Colon));
  R.Path = std::string(Spec.substr(Colon + 1));
  if (R.Path.empty()) return failMessage(std::format("empty remote path in {}", Spec));
  return R;
}

Coro<Result<Report>> pushPath(const std::filesystem::path &Src, const RemotePath &Dst, const SyncOptions &Given) {
  auto Items = enumerate(Src, Given);
  if (!Items) co_return std::unexpected(Items.error());

  // rsync exits cleanly when there is nothing to send, which is what a
  // directory without --recursive amounts to.
  if (Items->empty()) {
    Report Empty;
    Empty.Backend = Given.Backend;
    co_return Empty;
  }

  // The pool has to suit the largest file: one geometry serves the session.
  uint64_t Largest = 0;
  for (const Item &It : *Items) {
    std::error_code EC;
    const auto Size = It.Directory ? 0 : std::filesystem::file_size(It.Local, EC);
    if (!EC) Largest = std::max<uint64_t>(Largest, Size);
  }
  const SyncOptions Opts = withGeometry(Given, Largest);

  if (Opts.DryRun) {
    Report Planned;
    Planned.Backend = Opts.Backend;
    for (const Item &It : *Items) {
      // Neither carries payload, and file_size on a link would report whatever
      // it points at - counting a byte the transfer never sends.
      if (It.Directory || It.Link) continue;
      std::error_code EC;
      const auto Size = std::filesystem::file_size(It.Local, EC);
      if (EC) continue;
      Planned.Files++;
      Planned.FileSize += Size;
      if (Opts.OnFile) Opts.OnFile(It.Name, Size);
    }
    co_return Planned;
  }

  auto Channel = makeDataChannel(Opts.Backend, Opts.PageCount, Opts.PageSize, Dst.Host);
  if (!Channel) co_return failMessage(std::format("unknown backend: {}", Opts.Backend));

  auto Kid = spawn(sshArgv(Dst, Opts));
  if (!Kid) co_return std::unexpected(Kid.error());

  Push P(std::move(*Kid), std::move(Channel), Opts);
  co_return co_await P.run(*Items);
}

// The far side of a pull: enumerate locally and send over stdio.
Coro<Result<Report>> servePush(const std::filesystem::path &Src, const SyncOptions &Given) {
  auto Items = enumerate(Src, Given);
  if (!Items) co_return std::unexpected(Items.error());

  if (Items->empty()) {
    Report Empty;
    Empty.Backend = Given.Backend;
    co_return Empty;
  }

  uint64_t Largest = 0;
  for (const Item &It : *Items) {
    std::error_code EC;
    const auto Size = It.Directory ? 0 : std::filesystem::file_size(It.Local, EC);
    if (!EC) Largest = std::max<uint64_t>(Largest, Size);
  }
  const SyncOptions Opts = withGeometry(Given, Largest);

  auto Stdio = proto::ControlChannel::overStdio();
  if (!Stdio) co_return std::unexpected(Stdio.error());

  auto Channel = makeDataChannel(Opts.Backend, Opts.PageCount, Opts.PageSize, dialBack());
  if (!Channel) co_return failMessage(std::format("unknown backend: {}", Opts.Backend));

  Push P(std::move(*Stdio), std::move(Channel), Opts);
  co_return co_await P.run(*Items);
}

Coro<Result<Report>> pullPath(const RemotePath &Src, const std::filesystem::path &Dst, const SyncOptions &Opts) {
  auto Kid = spawn(pullArgv(Src, Opts));
  if (!Kid) co_return std::unexpected(Kid.error());

  proto::ControlChannel Control(Stream(Kid->FromChild), Stream(Kid->ToChild));
  auto Done = co_await receiveOver(std::move(Control), Dst, Opts.Durable);
  if (!Done) co_return std::unexpected(Done.error());
  if (auto R = Kid->wait(); !R) co_return std::unexpected(R.error());
  co_return *Done;
}

} // namespace rail
