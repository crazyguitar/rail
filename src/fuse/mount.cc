#define FUSE_USE_VERSION 31

#include "rail/fuse/mount.h"

#include "rail/fuse/files.h"
#include "rail/fuse/inodes.h"
#include "rail/io/loop.h"
#include "rail/io/runner.h"
#include "rail/io/stream.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <fuse_lowlevel.h>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rail::fuse {

namespace {

constexpr ::fuse_ino_t kRoot = 1;
constexpr uint64_t kSequentialSlack = 2u << 20;
constexpr int kDrainTick = 10;
constexpr ::fuse_ino_t kNullFile = 2;
constexpr char kNullName[] = "data.bin";
constexpr size_t kPatternSize = 1u << 20;
constexpr unsigned kSpliceCaps = FUSE_CAP_SPLICE_READ | FUSE_CAP_SPLICE_WRITE | FUSE_CAP_SPLICE_MOVE;

struct Shard {
  vfs::Remotes Pool;
  std::vector<std::byte> Scratch;
  Files Open;
  std::unordered_map<uint64_t, std::vector<proto::ListEntry>> OpenDirs;
  std::unordered_map<std::string, uint64_t> Written;
  std::vector<Coro<void>> Running;
  uint64_t NextDir = 1;
  uint64_t Transfer = 0;

  int Wake = -1;
  std::mutex Lock;
  std::deque<std::function<void()>> Queue;
  std::atomic<bool> Stopping{false};
  std::thread Thread;

  explicit Shard(const vfs::RemoteOptions &Opts) : Pool(Opts) {}
};

// A shard costs a session, and an rdma session costs a fifth of a second to
// tear down without parallelising, so past eight of them an unmount is slower
// than anyone waits for. Eight is where that curve is still flat.
size_t shardCount(const MountOptions &Opts) {
  if (Opts.Threads > 0) return Opts.Threads;
  const unsigned Cores = std::thread::hardware_concurrency();
  return std::clamp<size_t>(Cores ? Cores : 4, 4, 8);
}

struct Mount {
  const MountOptions &Opts;
  ::fuse_session *Session = nullptr;
  std::atomic<uint64_t> Reads{0};
  std::atomic<uint64_t> ReadBytes{0};
  std::atomic<uint64_t> Fetched{0};
  std::atomic<size_t> Reading{0};
  Inodes Known;
  std::mutex KnownLock;
  vfs::RemoteOptions PerShard;
  std::vector<std::unique_ptr<Shard>> Shards;

  size_t Lanes = 1;

  explicit Mount(const MountOptions &Opts) : Opts(Opts), Known(Opts.Remote.Root), PerShard(Opts.Remote), Lanes(shardCount(Opts)) {
    PerShard.Sessions = std::max<size_t>(1, Opts.Remote.Sessions / Lanes);
  }
};

thread_local size_t ThisShard = 0;

Shard &shard(Mount &M) { return *M.Shards[ThisShard]; }

size_t ownerOf(const Mount &M, ::fuse_ino_t Ino) { return M.Shards.size() < 2 ? 0 : static_cast<size_t>(Ino) % M.Shards.size(); }

uint64_t packHandle(size_t Shard, uint64_t Local) { return (static_cast<uint64_t>(Shard) << 56) | Local; }
size_t shardOfHandle(uint64_t Handle) { return static_cast<size_t>(Handle >> 56); }
uint64_t localOfHandle(uint64_t Handle) { return Handle & ((uint64_t{1} << 56) - 1); }

void onOwner(Mount &M, size_t Owner, std::function<void()> Work) {
  if (Owner == ThisShard || Owner >= M.Shards.size()) {
    Work();
    return;
  }

  Shard &S = *M.Shards[Owner];
  {
    std::lock_guard<std::mutex> Held(S.Lock);
    S.Queue.push_back(std::move(Work));
  }

  const uint64_t One = 1;
  [[maybe_unused]] const ssize_t Ignored = ::write(S.Wake, &One, sizeof(One));
}

bool knownIno(Mount &M, ::fuse_ino_t Ino) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  return M.Known.known(Ino);
}

std::string pathOf(Mount &M, ::fuse_ino_t Ino) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  return M.Known.path(Ino);
}

std::string pathUnder(Mount &M, ::fuse_ino_t Parent, const std::string &Name) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  return Inodes::join(M.Known.path(Parent), Name);
}

::fuse_ino_t insertIno(Mount &M, ::fuse_ino_t Parent, const std::string &Name) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  return M.Known.insert(Parent, Name);
}

::fuse_ino_t reserveIno(Mount &M, ::fuse_ino_t Parent, const std::string &Name) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  return M.Known.reserve(Parent, Name);
}

void forgetIno(Mount &M, ::fuse_ino_t Ino, uint64_t Count) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  M.Known.forget(Ino, Count);
}

void releaseIno(Mount &M, ::fuse_ino_t Parent, const std::string &Name) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  M.Known.release(Parent, Name);
}

void dropIno(Mount &M, ::fuse_ino_t Parent, const std::string &Name) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  M.Known.drop(Parent, Name);
}

void reparentIno(Mount &M, ::fuse_ino_t Parent, const std::string &Name, ::fuse_ino_t NewParent, const std::string &NewName) {
  std::lock_guard<std::mutex> Held(M.KnownLock);
  M.Known.reparent(Parent, Name, NewParent, NewName);
}

std::atomic<int> Interrupted{-1};

void noticeSignal(int) {
  const int Fd = Interrupted.load(std::memory_order_relaxed);
  if (Fd < 0) return;

  const char Wake = 1;
  [[maybe_unused]] const ssize_t Ignored = ::write(Fd, &Wake, 1);
}

void catchSignals(void (*Handler)(int)) {
  struct ::sigaction Act{};
  Act.sa_handler = Handler;
  ::sigemptyset(&Act.sa_mask);
  for (const int Signal : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(Signal, &Act, nullptr);
}

bool serving(const Mount &M) { return !M.Opts.Remote.Host.empty(); }

// How many times this mount has changed the file itself. Anything it cached
// from before that count is bytes the peer no longer holds.
uint64_t writesTo(Mount &M, const std::string &Path) {
  auto It = shard(M).Written.find(Path);
  return It == shard(M).Written.end() ? 0 : It->second;
}

void spawn(Mount &M, Coro<void> Task) {
  for (size_t I = shard(M).Running.size(); I-- > 0;)
    if (shard(M).Running[I].done()) shard(M).Running.erase(shard(M).Running.begin() + static_cast<long>(I));

  Task.start();
  if (!Task.done()) shard(M).Running.push_back(std::move(Task));
}

bool safeName(const std::string &Name) { return !Name.empty() && Name != "." && Name != ".." && Name.find('/') == std::string::npos; }

// The daemon's errno when it sent one, so a refusal the kernel understands -
// EEXIST, ENOTEMPTY, EPERM - does not reach the caller as a blanket EIO.
//
// Only codes a filesystem can answer with: a transport failure carries an
// errno too, and handing ECONNABORTED to a process calling fsync tells it
// something no filesystem can mean. Anything else is EIO, as it was before.
int errnoOf(const Error &Why) {
  static constexpr int kAnswerable[] = {ENOENT, EEXIST, ENOTEMPTY, EACCES, EPERM, EISDIR,       ENOTDIR, EMLINK, EXDEV,  ENOSPC,
                                        EDQUOT, EMFILE, EFBIG,     EINVAL, EROFS, ENAMETOOLONG, ELOOP,   EBUSY,  ENODEV, ETXTBSY};
  if (Why.Code.category() != std::generic_category()) return EIO;
  const int Number = Why.Code.value();
  return std::find(std::begin(kAnswerable), std::end(kAnswerable), Number) != std::end(kAnswerable) ? Number : EIO;
}

void fillRemoteAttr(struct ::stat &S, ::fuse_ino_t Ino, const proto::FileAttrs &A) {
  S.st_ino = Ino;
  S.st_mode = (A.Link ? S_IFLNK : A.Directory ? S_IFDIR : S_IFREG) | (A.Mode & 07777);
  S.st_nlink = A.Directory ? 2 : std::max<uint32_t>(1, A.Links);
  S.st_size = static_cast<off_t>(A.Size);
  S.st_mtime = static_cast<time_t>(A.Mtime);
  S.st_atime = S.st_mtime;
  S.st_ctime = S.st_mtime;
  S.st_blksize = 4096;
  S.st_blocks = static_cast<blkcnt_t>((A.Size + 511) / 512);
  S.st_uid = ::getuid();
  S.st_gid = ::getgid();
}

Mount &self(::fuse_req_t Req) { return *static_cast<Mount *>(::fuse_req_userdata(Req)); }

const std::vector<std::byte> &pattern() {
  static const std::vector<std::byte> P = [] {
    std::vector<std::byte> V(kPatternSize);
    for (size_t I = 0; I < V.size(); I++) V[I] = static_cast<std::byte>((I * 31 + 7) & 0xff);
    return V;
  }();
  return P;
}

void fillAttr(struct ::stat &S, ::fuse_ino_t Ino, uint64_t Size) {
  S.st_ino = Ino;
  S.st_uid = ::getuid();
  S.st_gid = ::getgid();
  S.st_blksize = 4096;
  if (Ino == kRoot) {
    S.st_mode = S_IFDIR | 0755;
    S.st_nlink = 2;
    return;
  }
  S.st_mode = S_IFREG | 0444;
  S.st_nlink = 1;
  S.st_size = static_cast<off_t>(Size);
  S.st_blocks = static_cast<blkcnt_t>((Size + 511) / 512);
}

void replyBytes(Mount &M, ::fuse_req_t Req, const std::byte *Data, size_t Length) {
  if (!M.Opts.Splice || Length == 0) {
    ::fuse_reply_buf(Req, reinterpret_cast<const char *>(Data), Length);
    return;
  }
  ::fuse_bufvec Bv = FUSE_BUFVEC_INIT(Length);
  Bv.buf[0].mem = const_cast<void *>(static_cast<const void *>(Data));
  ::fuse_reply_data(Req, &Bv, FUSE_BUF_SPLICE_MOVE);
}

constexpr size_t kMaxReplyPages = 8;

// A read is answered from the pages it happens to fall in, which need not be
// adjacent. More than kMaxReplyPages means the mount was given a page smaller
// than a read, and one copy is better than a refusal.
void replyFrom(Mount &M, ::fuse_req_t Req, AddressSpace &Space, uint64_t Offset, size_t Length) {
  struct Gathered {
    ::fuse_bufvec V;
    ::fuse_buf More[kMaxReplyPages - 1];
  };

  Gathered G{};
  G.V.count = 0;
  size_t Done = 0;
  while (Done < Length && G.V.count < kMaxReplyPages) {
    const auto At = Space.at(Offset + Done, Length - Done);
    if (!At.Where || At.Length == 0) break;
    G.V.buf[G.V.count].size = At.Length;
    G.V.buf[G.V.count].mem = At.Where->bytes() + At.Offset;
    G.V.count++;
    Done += At.Length;
  }

  if (Done != Length) {
    auto &Scratch = shard(M).Scratch;
    if (Scratch.size() < Length) Scratch.resize(Length);
    for (size_t I = 0; I < Length;) {
      const auto At = Space.at(Offset + I, Length - I);
      if (!At.Where || At.Length == 0) break;
      std::memcpy(Scratch.data() + I, At.Where->bytes() + At.Offset, At.Length);
      I += At.Length;
    }
    replyBytes(M, Req, Scratch.data(), Length);
    return;
  }

  if (!M.Opts.Splice) {
    if (G.V.count == 1) {
      replyBytes(M, Req, static_cast<const std::byte *>(G.V.buf[0].mem), Length);
      return;
    }
    auto &Scratch = shard(M).Scratch;
    if (Scratch.size() < Length) Scratch.resize(Length);
    size_t I = 0;
    for (size_t B = 0; B < G.V.count; B++) {
      std::memcpy(Scratch.data() + I, G.V.buf[B].mem, G.V.buf[B].size);
      I += G.V.buf[B].size;
    }
    replyBytes(M, Req, Scratch.data(), Length);
    return;
  }

  ::fuse_reply_data(Req, &G.V, FUSE_BUF_SPLICE_MOVE);
}

void onInit(void *Data, ::fuse_conn_info *Conn) {
  const auto &Opts = static_cast<Mount *>(Data)->Opts;
  if (Opts.Splice) Conn->want |= Conn->capable & kSpliceCaps;
  else Conn->want &= ~kSpliceCaps;
  if (Opts.MaxRead > 0) Conn->max_read = Opts.MaxRead;
  Conn->max_write = 1u << 20;
  Conn->max_background = Opts.MaxBackground;
  Conn->congestion_threshold = Opts.MaxBackground * 3 / 4;
}

void nullLookup(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (Parent != kRoot || M.Opts.NullSize == 0 || std::strcmp(Name, kNullName) != 0) {
    ::fuse_reply_err(Req, ENOENT);
    return;
  }
  ::fuse_entry_param E{};
  E.ino = kNullFile;
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  fillAttr(E.attr, kNullFile, M.Opts.NullSize);
  ::fuse_reply_entry(Req, &E);
}

void nullGetAttr(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info *) {
  Mount &M = self(Req);
  if (Ino != kRoot && Ino != kNullFile) {
    ::fuse_reply_err(Req, ENOENT);
    return;
  }
  struct ::stat S{};
  fillAttr(S, Ino, M.Opts.NullSize);
  ::fuse_reply_attr(Req, &S, M.Opts.AttrTimeout);
}

void nullOpen(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info *Fi) {
  if (Ino != kNullFile) {
    ::fuse_reply_err(Req, EISDIR);
    return;
  }
  if ((Fi->flags & O_ACCMODE) != O_RDONLY) {
    ::fuse_reply_err(Req, EACCES);
    return;
  }
  if (self(Req).Opts.DirectIo) Fi->direct_io = 1;
  ::fuse_reply_open(Req, Fi);
}

void nullRead(::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, off_t Off, ::fuse_file_info *) {
  Mount &M = self(Req);
  if (Ino != kNullFile) {
    ::fuse_reply_err(Req, EISDIR);
    return;
  }

  const uint64_t At = static_cast<uint64_t>(Off);
  if (At >= M.Opts.NullSize) {
    ::fuse_reply_buf(Req, nullptr, 0);
    return;
  }

  const size_t Want = static_cast<size_t>(std::min<uint64_t>(Size, M.Opts.NullSize - At));
  M.Reads++;
  M.ReadBytes += Want;
  if (shard(M).Scratch.size() < Want) shard(M).Scratch.resize(Want);

  const auto &P = pattern();
  for (size_t I = 0; I < Want;) {
    const size_t Into = static_cast<size_t>((At + I) % kPatternSize);
    const size_t Run = std::min(Want - I, kPatternSize - Into);
    std::memcpy(shard(M).Scratch.data() + I, P.data() + Into, Run);
    I += Run;
  }
  replyBytes(M, Req, shard(M).Scratch.data(), Want);
}

void nullReadDir(::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, off_t Off, ::fuse_file_info *) {
  Mount &M = self(Req);
  if (Ino != kRoot) {
    ::fuse_reply_err(Req, ENOTDIR);
    return;
  }

  std::vector<std::pair<std::string, ::fuse_ino_t>> Entries{{".", kRoot}, {"..", kRoot}};
  if (M.Opts.NullSize > 0) Entries.emplace_back(kNullName, kNullFile);

  std::vector<char> Buf(Size);
  size_t Used = 0;
  for (size_t I = static_cast<size_t>(Off); I < Entries.size(); I++) {
    struct ::stat S{};
    fillAttr(S, Entries[I].second, M.Opts.NullSize);
    const size_t Need = ::fuse_add_direntry(Req, nullptr, 0, Entries[I].first.c_str(), nullptr, 0);
    if (Used + Need > Size) break;
    Used += ::fuse_add_direntry(Req, Buf.data() + Used, Size - Used, Entries[I].first.c_str(), &S, static_cast<off_t>(I + 1));
  }
  ::fuse_reply_buf(Req, Buf.data(), Used);
}

Coro<void> doLookup(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Parent, std::string Name) {
  if (!knownIno(M, Parent) || !safeName(Name)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Info = co_await Held->client().stat(Path);
  if (!Info) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Info->Found) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  ::fuse_entry_param E{};
  E.ino = insertIno(M, Parent, Name);
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  fillRemoteAttr(E.attr, E.ino, Info->Attrs);
  ::fuse_reply_entry(Req, &E);
}

struct InUse {
  Mount &M;
  uint64_t Handle;

  InUse(Mount &M, uint64_t Handle) : M(M), Handle(Handle) {
    if (File *F = shard(M).Open.get(Handle)) F->InFlightOps++;
  }

  InUse(const InUse &) = delete;
  InUse &operator=(const InUse &) = delete;

  ~InUse() {
    File *F = shard(M).Open.get(Handle);
    if (!F) return;
    if (--F->InFlightOps == 0 && F->Doomed) shard(M).Open.close(Handle);
  }
};

uint64_t handleOn(const File &F, size_t Slot) { return Slot < F.OnSlot.size() ? F.OnSlot[Slot].Handle : 0; }

// One handle per session rather than one per file: a handle belongs to the
// session that opened it, so a file with a single one can only ever have one
// request in flight however many readers it has.
Coro<Result<vfs::Remotes::Lease>> heldFor(Mount &M, File &F) {
  auto Held = co_await shard(M).Pool.take();
  if (!Held) co_return std::unexpected(Held.error());

  const size_t Slot = Held->index();
  if (F.OnSlot.size() <= Slot) F.OnSlot.resize(shard(M).Pool.count());
  if (F.OnSlot[Slot].Handle != 0 && F.OnSlot[Slot].Generation == Held->generation()) co_return std::move(*Held);

  co_await F.Opening.take();
  if (F.OnSlot[Slot].Handle != 0 && F.OnSlot[Slot].Generation == Held->generation()) {
    F.Opening.give();
    co_return std::move(*Held);
  }

  auto Opened = co_await Held->client().openFile(F.Path, F.Writable);
  F.Opening.give();
  if (!Opened) {
    Held->discard();
    co_return std::unexpected(Opened.error());
  }
  if (!Opened->Ok) co_return failMessage(Opened->Error);

  F.OnSlot[Slot].Handle = Opened->Handle;
  F.OnSlot[Slot].Generation = Held->generation();
  co_return std::move(*Held);
}

Coro<Result<uint64_t>> currentSize(Mount &M, const std::string &Path) {
  auto Held = co_await shard(M).Pool.take();
  if (!Held) co_return std::unexpected(Held.error());

  auto Info = co_await Held->client().stat(Path);
  if (!Info) {
    Held->discard();
    co_return std::unexpected(Info.error());
  }
  if (!Info->Found) co_return failMessage("vanished");
  co_return Info->Attrs.Size;
}

Coro<Result<void>> flushOnce(Mount &M, File &F, AddressSpace &Sending, size_t Length, uint64_t Base) {
  auto Held = co_await heldFor(M, F);
  if (!Held) co_return std::unexpected(Held.error());

  const size_t Slice = static_cast<size_t>(Held->client().maxTransfer());

  if (Length >= 2 * Slice) {
    if (M.Opts.Verbose) std::fprintf(stderr, "railfs: storing %s at %llu for %zu\n", F.Path.c_str(), static_cast<unsigned long long>(Base), Length);

    auto Put = co_await Held->client().storeFrom(Sending, Length, F.Path, Base, false, handleOn(F, Held->index()));
    if (Put) co_return Result<void>{};

    if (M.Opts.Verbose) std::fprintf(stderr, "railfs: streamed store failed, rewriting ranged: %s\n", Put.error().message().c_str());
  }

  Sending.rebase(Base);
  size_t Done = 0;
  while (Done < Length) {
    const auto At = Sending.at(Base + Done, std::min(Slice, Length - Done));
    if (!At.Where) co_return failMessage("a ranged write started outside the pending window");
    const std::span<const std::byte> Piece(At.Where->bytes() + At.Offset, At.Length);
    auto W = co_await Held->client().write(F.Path, Base + Done, Piece, false, handleOn(F, Held->index()));
    if (!W) {
      Held->discard();
      co_return std::unexpected(W.error());
    }
    Done += At.Length;
  }
  co_return Result<void>{};
}

Coro<Result<void>> flushPending(Mount &M, File &F) {
  if (F.WriteFailed) co_return failMessage("a background flush of this file failed");
  if (F.PendingLen == 0 && !F.Flushing) co_return Result<void>{};

  co_await F.Writing.take();
  if (F.PendingLen == 0) {
    F.Writing.give();
    co_return Result<void>{};
  }

  // Swapped rather than sent in place, so writes arriving during the flush
  // accumulate in the other window instead of waiting for the fabric.
  std::swap(F.Pending, F.Draining);
  F.DrainingLen = F.PendingLen;
  F.PendingLen = 0;

  const uint64_t Base = F.WriteBase;
  F.WriteBase = Base + F.DrainingLen;
  F.Flushing = true;

  auto Done = co_await flushOnce(M, F, F.Draining, F.DrainingLen, Base);

  F.DrainingLen = 0;
  F.Flushing = false;
  shard(M).Written[F.Path]++;
  F.Writing.give();
  if (!Done) {
    F.WriteFailed = true;
    co_return std::unexpected(Done.error());
  }
  co_return Result<void>{};
}

// A full window goes to the fabric without the write that filled it waiting
// for it, so the next window fills while this one is in flight. The failure a
// detached flush cannot report lands on the file, for the next one to raise.
Coro<void> drainPending(Mount &M, uint64_t Handle) {
  const InUse Hold(M, Handle);

  File *F = shard(M).Open.get(Handle);
  if (!F) co_return;

  if (auto Done = co_await flushPending(M, *F); Done) co_return;

  File *Still = shard(M).Open.get(Handle);
  if (Still) Still->WriteFailed = true;
}

Coro<Result<void>> flushPath(Mount &M, const std::string &Path) {
  for (uint64_t Handle = shard(M).Open.nextPendingOn(Path, 0); Handle != 0; Handle = shard(M).Open.nextPendingOn(Path, Handle)) {
    const InUse Hold(M, Handle);
    File *F = shard(M).Open.get(Handle);
    if (!F) continue;
    if (auto R = co_await flushPending(M, *F); !R) co_return std::unexpected(R.error());
  }
  co_return Result<void>{};
}

Coro<void> doGetAttr(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino) {
  if (!knownIno(M, Ino)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  if (auto R = co_await flushPath(M, pathOf(M, Ino)); !R) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Info = co_await Held->client().stat(pathOf(M, Ino));
  if (!Info) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Info->Found) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  struct ::stat S{};
  fillRemoteAttr(S, Ino, Info->Attrs);
  ::fuse_reply_attr(Req, &S, M.Opts.AttrTimeout);
}

Coro<void> doOpenDir(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info Fi) {
  if (!knownIno(M, Ino)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Listed = co_await Held->client().list(pathOf(M, Ino));
  if (!Listed) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Listed->Found) {
    ::fuse_reply_err(Req, ENOTDIR);
    co_return;
  }

  std::vector<proto::ListEntry> Entries = std::move(Listed->Entries);
  std::ranges::sort(Entries, [](const proto::ListEntry &A, const proto::ListEntry &B) { return A.Name < B.Name; });

  const uint64_t Handle = shard(M).NextDir++;
  shard(M).OpenDirs.emplace(Handle, std::move(Entries));
  Fi.fh = packHandle(ThisShard, Handle);
  ::fuse_reply_open(Req, &Fi);
}

uint64_t readAhead(Mount &M, const File &F) { return std::min<uint64_t>({M.Opts.Remote.Readahead, shard(M).Transfer, F.Covered}); }

Coro<Result<void>> fillWindow(Mount &M, File &F, size_t Size, uint64_t Off) {
  const uint64_t Span = std::max<uint64_t>(readAhead(M, F), Size);

  auto Held = co_await heldFor(M, F);
  if (!Held) co_return std::unexpected(Held.error());

  const uint64_t At = writesTo(M, F.Path);
  const size_t Room = std::min<size_t>(Span, Held->client().maxTransfer());
  const uint64_t Start = Off;

  Page *Landing = F.Have.page(Room);
  auto Got = Landing ? co_await Held->client().read(F.Path, Start, *Landing, handleOn(F, Held->index()))
                     : co_await Held->client().read(F.Path, Start, F.Have.room(Room), handleOn(F, Held->index()));
  if (!Got) {
    Held->discard();
    co_return std::unexpected(Got.error());
  }

  M.Fetched += Got->Bytes;
  F.Have.Path = F.Path;
  F.Have.Start = Start;
  F.Have.FileSize = Got->FileSize;
  F.Have.Stamp = At;
  F.Have.Bytes = Got->Bytes;
  co_return Result<void>{};
}

Coro<void> directRead(Mount &M, ::fuse_req_t Req, File &F, size_t Size, uint64_t Off) {
  auto Held = co_await heldFor(M, F);
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  Page *Landing = F.Have.page(Size);
  std::vector<std::byte> Fallback;
  if (!Landing) Fallback.resize(Size);

  auto Got = Landing ? co_await Held->client().read(F.Path, Off, *Landing, handleOn(F, Held->index()))
                     : co_await Held->client().read(F.Path, Off, Fallback, handleOn(F, Held->index()));
  if (!Got) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  M.Fetched += Got->Bytes;
  const std::byte *From = Landing ? Landing->bytes() : Fallback.data();
  replyBytes(M, Req, From, std::min<size_t>(Size, Got->Bytes));
}

Coro<void> windowedRead(Mount &M, ::fuse_req_t Req, File &F, size_t Size, uint64_t Off) {
  if (readAhead(M, F) <= Size) {
    co_await directRead(M, Req, F, Size, Off);
    co_return;
  }

  if (F.Have.Stamp != writesTo(M, F.Path) || !F.Have.covers(F.Path, Off, Size)) {
    co_await F.Filling.take();

    Result<void> Filled{};
    if (F.Have.Stamp != writesTo(M, F.Path) || !F.Have.covers(F.Path, Off, Size)) Filled = co_await fillWindow(M, F, Size, Off);

    F.Filling.give();
    if (!Filled) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
  }

  const uint64_t Into = Off - F.Have.Start;
  const size_t Mine = Into < F.Have.Bytes ? std::min<size_t>(Size, F.Have.Bytes - static_cast<size_t>(Into)) : 0;
  if (Mine == 0 && Off < F.Have.FileSize) {
    F.Have.Bytes = 0;
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  replyBytes(M, Req, F.Have.bytes().data() + Into, Mine);
}

Coro<Result<uint64_t>> fetchChunk(Mount &M, File &F, uint64_t Start, AddressSpace &Into) {
  auto Held = co_await heldFor(M, F);
  if (!Held) co_return std::unexpected(Held.error());

  auto Got = co_await Held->client().fetchInto(F.Path, Start, Into, handleOn(F, Held->index()));
  if (!Got) {
    Held->discard();
    co_return std::unexpected(Got.error());
  }
  M.Fetched += *Got;
  co_return *Got;
}

Coro<void> settle(File &F) {
  if (!F.InFlight) co_return;
  [[maybe_unused]] auto Ignored = co_await F.Fetching.join();
  F.InFlight = false;
  F.Fetching = {};
}

template <class T> struct Uninitialised : std::allocator<T> {
  using std::allocator<T>::allocator;

  template <class U> struct rebind {
    using other = Uninitialised<U>;
  };

  template <class U> void construct(U *At) { ::new (static_cast<void *>(At)) U; }

  template <class U, class... Rest> void construct(U *At, Rest &&...Args) { std::construct_at(At, std::forward<Rest>(Args)...); }
};

// A write buffer is overwritten by the splice copy the instant it exists, so
// sizing one must not also zero it.
using RawBytes = std::vector<std::byte, Uninitialised<std::byte>>;

bool append(AddressSpace &Window, size_t At, std::span<const std::byte> Data) {
  size_t Done = 0;
  while (Done < Data.size()) {
    const auto Where = Window.at(At + Done, Data.size() - Done);
    if (!Where.Where || Where.Length == 0) return false;
    std::memcpy(Where.Where->bytes() + Where.Offset, Data.data() + Done, Where.Length);
    Done += Where.Length;
  }
  return true;
}

// Claiming releases what the window already holds, so a window with bytes in
// it is left alone: growing it mid-accumulation would drop them silently.
bool reserve(File &F, AddressSpace &Window, uint64_t Room, size_t Holding = 0) {
  if (Window.capacity() >= Room && Window.pageSize() == F.Page) return true;
  if (Holding > 0) return false;

  const size_t Pages = static_cast<size_t>((Room + F.Page - 1) / F.Page);
  Window.claim(Memory::get(), static_cast<size_t>(F.Page), Pages);
  return Window.capacity() >= Room;
}

uint64_t chunkOf(const Mount &M, const File &F) { return std::max<uint64_t>(F.Page, (M.Opts.StreamChunk / F.Page) * F.Page); }

void prefetch(Mount &M, File &F) {
  if (F.InFlight) return;
  const uint64_t Next = F.StreamStart + chunkOf(M, F);
  if (F.StreamLen == 0 || Next >= F.Size) return;

  const uint64_t Room = std::min<uint64_t>(chunkOf(M, F), F.Size - Next);
  reserve(F, F.Spare, Room);

  F.SpareStart = Next;
  F.SpareStamp = writesTo(M, F.Path);
  F.Fetching = fetchChunk(M, F, Next, F.Spare);
  F.Fetching.start();
  F.InFlight = true;

  if (M.Opts.Verbose)
    std::fprintf(stderr,
                 "railfs: prefetching %s from %llu for %llu\n",
                 F.Path.c_str(),
                 static_cast<unsigned long long>(Next),
                 static_cast<unsigned long long>(Room));
}

Coro<void> streamedRead(Mount &M, ::fuse_req_t Req, File &F, size_t Size, uint64_t Off) {
  const uint64_t Chunk = chunkOf(M, F);
  const uint64_t Start = (Off / Chunk) * Chunk;

  if (std::min<uint64_t>(Off + Size, F.Size) > Start + Chunk) {
    co_await windowedRead(M, Req, F, Size, Off);
    co_return;
  }

  const uint64_t At = writesTo(M, F.Path);
  if (!F.covers(Off, Size, At)) {
    if (Start >= F.Size) {
      if (F.CheckedAtSize == F.Size && F.Size != 0) {
        ::fuse_reply_buf(Req, nullptr, 0);
        co_return;
      }
      co_await settle(F);
      auto Grown = co_await currentSize(M, F.Path);
      if (!Grown) {
        F.StreamLen = 0;
        F.Streaming = false;
        co_await windowedRead(M, Req, F, Size, Off);
        co_return;
      }
      F.Size = *Grown;
      F.CheckedAtSize = F.Size;
      if (Start >= F.Size) {
        ::fuse_reply_buf(Req, nullptr, 0);
        co_return;
      }
    }

    if (F.InFlight && F.SpareStart == Start && F.SpareStamp == At) {
      auto Got = co_await F.Fetching.join();
      F.InFlight = false;
      F.Fetching = {};
      if (!Got) {
        F.StreamLen = 0;
        F.Streaming = false;
        F.Failed = true;
        co_await windowedRead(M, Req, F, Size, Off);
        co_return;
      }
      std::swap(F.Stream, F.Spare);
      F.StreamStart = Start;
      F.StreamLen = static_cast<size_t>(*Got);
      F.StreamStamp = F.SpareStamp;
    } else {
      co_await settle(F);

      const uint64_t Room = std::min<uint64_t>(Chunk, F.Size - Start);
      reserve(F, F.Stream, Room);

      if (M.Opts.Verbose)
        std::fprintf(stderr,
                     "railfs: streaming %s from %llu for %llu\n",
                     F.Path.c_str(),
                     static_cast<unsigned long long>(Start),
                     static_cast<unsigned long long>(Room));

      auto Got = co_await fetchChunk(M, F, Start, F.Stream);
      if (!Got) {
        F.StreamLen = 0;
        F.Streaming = false;
        F.Failed = true;
        co_await windowedRead(M, Req, F, Size, Off);
        co_return;
      }
      F.StreamStart = Start;
      F.StreamLen = static_cast<size_t>(*Got);
      F.StreamStamp = At;
    }
  }

  if (Off < F.StreamStart || Off - F.StreamStart >= F.StreamLen) {
    if (Off < F.Size) {
      F.StreamLen = 0;
      F.Streaming = false;
      co_await windowedRead(M, Req, F, Size, Off);
      co_return;
    }
    ::fuse_reply_buf(Req, nullptr, 0);
    co_return;
  }

  const uint64_t Into = Off - F.StreamStart;
  const size_t Mine = std::min<size_t>(Size, F.StreamLen - static_cast<size_t>(Into));
  replyFrom(M, Req, F.Stream, F.StreamStart + Into, Mine);

  prefetch(M, F);
}

Coro<void> doRead(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, uint64_t Off, uint64_t Handle) {
  M.Reads++;
  M.ReadBytes += Size;

  if (Size == 0) {
    ::fuse_reply_buf(Req, nullptr, 0);
    co_return;
  }

  const InUse Hold(M, Handle);

  File *F = shard(M).Open.get(Handle);
  if (!F) {
    ::fuse_reply_err(Req, EBADF);
    co_return;
  }

  if (shard(M).Open.nextPendingOn(F->Path, 0) != 0)
    if (auto R = co_await flushPath(M, F->Path); !R) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }

  if (!F->Started) {
    F->Started = true;
    F->Frontier = Off;
  }

  const bool NearFrontier = Off + kSequentialSlack >= F->Frontier && Off <= F->Frontier + kSequentialSlack;
  if (!NearFrontier) {
    co_await settle(*F);
    F->Covered = 0;
    F->Frontier = Off;
    F->Streaming = false;
    F->StreamLen = 0;
  }

  const uint64_t End = std::min(Off + Size, F->Size);
  if (End > F->Frontier) F->Covered += std::min<uint64_t>(Size, End - F->Frontier);
  F->Frontier = std::max(F->Frontier, End);

  if (!F->Failed && !F->Streaming && M.Opts.StreamAfter > 0 && F->Covered >= M.Opts.StreamAfter) F->Streaming = true;

  if (F->Streaming) {
    co_await streamedRead(M, Req, *F, Size, Off);
    co_return;
  }
  co_await windowedRead(M, Req, *F, Size, Off);
}

Coro<void> doOpen(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info Fi) {
  if (!knownIno(M, Ino)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  File F;
  F.Path = pathOf(M, Ino);
  F.Writable = (Fi.flags & O_ACCMODE) != O_RDONLY;
  F.OnSlot.resize(shard(M).Pool.count());

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  const std::string &Path = F.Path;
  auto Opened = co_await Held->client().openFile(Path, F.Writable);
  if (!Opened) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Opened->Found) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  if (Opened->Attrs.Directory) {
    ::fuse_reply_err(Req, EISDIR);
    co_return;
  }
  if (!Opened->Ok) {
    ::fuse_reply_err(Req, Opened->Errno != 0 ? static_cast<int>(Opened->Errno) : EIO);
    co_return;
  }

  F.OnSlot[Held->index()] = {Opened->Handle, Held->generation()};
  F.Size = Opened->Attrs.Size;
  F.Page = Held->client().maxTransfer();
  if (F.Writable && (Fi.flags & O_TRUNC)) {
    if (auto R = co_await Held->client().truncate(Path, 0); !R) {
      Held->discard();
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
    F.Size = 0;
  }
  Fi.fh = packHandle(ThisShard, shard(M).Open.open(std::move(F)));
  if (M.Opts.DirectIo) Fi.direct_io = 1;
  ::fuse_reply_open(Req, &Fi);
}

Coro<void> doWrite(Mount &M, ::fuse_req_t Req, RawBytes Data, uint64_t Off, uint64_t Handle) {
  const InUse Hold(M, Handle);

  File *F = shard(M).Open.get(Handle);
  if (!F || !F->Writable) {
    ::fuse_reply_err(Req, EBADF);
    co_return;
  }

  if (F->WriteFailed) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  // An empty window starts wherever this write does, with no need to wait on a
  // flush that is draining the other one.
  if (F->PendingLen == 0) {
    F->WriteBase = Off;
  } else if (Off != F->WriteBase + F->PendingLen) {
    if (auto R = co_await flushPending(M, *F); !R) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
    F->WriteBase = Off;
  }

  const size_t Wrote = Data.size();
  if (!reserve(*F, F->Pending, M.Opts.WriteChunk + Wrote, F->PendingLen)) {
    if (auto R = co_await flushPending(M, *F); !R) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
    F->WriteBase = Off;
    if (!reserve(*F, F->Pending, M.Opts.WriteChunk + Wrote, F->PendingLen)) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
  }

  F->Pending.rebase(0);
  if (!append(F->Pending, F->PendingLen, Data)) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  F->PendingLen += Wrote;
  F->StreamLen = 0;
  F->Size = std::max(F->Size, Off + Wrote);

  // A free window takes the flush detached, so this write does not wait on the
  // fabric. Both windows busy is the only thing that holds it up.
  const bool Full = F->PendingLen >= M.Opts.WriteChunk;
  const bool Waits = Full && F->Flushing;

  if (Full && !Waits) spawn(M, drainPending(M, Handle));

  if (Waits) {
    const auto Flushed = co_await flushPending(M, *F);
    if (!Flushed) {
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
  }

  ::fuse_reply_write(Req, Wrote);
}

Coro<void> doCreate(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Parent, std::string Name, mode_t Mode, ::fuse_file_info Fi) {
  if (!knownIno(M, Parent) || !safeName(Name)) {
    releaseIno(M, Parent, Name);
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    releaseIno(M, Parent, Name);
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (auto W = co_await Held->client().write(Path, 0, {}, true); !W) {
    Held->discard();
    releaseIno(M, Parent, Name);
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (auto C = co_await Held->client().setMode(Path, Mode & 07777); !C) {
    Held->discard();
    releaseIno(M, Parent, Name);
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  File F;
  F.Path = Path;
  F.OnSlot.resize(shard(M).Pool.count());
  F.Page = Held->client().maxTransfer();
  F.Writable = true;

  if (auto Opened = co_await Held->client().openFile(Path, true); Opened && Opened->Ok)
    F.OnSlot[Held->index()] = {Opened->Handle, Held->generation()};

  ::fuse_entry_param E{};
  E.ino = insertIno(M, Parent, Name);
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  proto::FileAttrs A;
  A.Mode = Mode & 07777;
  fillRemoteAttr(E.attr, E.ino, A);

  Fi.fh = packHandle(ThisShard, shard(M).Open.open(std::move(F)));
  if (M.Opts.DirectIo) Fi.direct_io = 1;
  ::fuse_reply_create(Req, &E, &Fi);
}

Coro<void> doFlush(Mount &M, ::fuse_req_t Req, uint64_t Handle, bool Sync) {
  const InUse Hold(M, Handle);

  File *F = shard(M).Open.get(Handle);
  if (!F) {
    ::fuse_reply_err(Req, 0);
    co_return;
  }

  if (auto R = co_await flushPending(M, *F); !R) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Sync || !F->Writable) {
    ::fuse_reply_err(Req, 0);
    co_return;
  }

  auto Held = co_await heldFor(M, *F);
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  auto Synced = co_await Held->client().fsync(F->Path, handleOn(*F, Held->index()));
  if (!Synced) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  ::fuse_reply_err(Req, 0);
}

Coro<void> doMakeDirectory(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Parent, std::string Name, mode_t Mode) {
  if (!knownIno(M, Parent) || !safeName(Name)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (auto R = co_await Held->client().makeDirectory(Path, Mode & 07777); !R) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(R.error()));
    co_return;
  }

  ::fuse_entry_param E{};
  E.ino = insertIno(M, Parent, Name);
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  proto::FileAttrs A;
  A.Directory = true;
  A.Mode = Mode & 07777;
  fillRemoteAttr(E.attr, E.ino, A);
  ::fuse_reply_entry(Req, &E);
}

Coro<void> doReadLink(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino) {
  if (!knownIno(M, Ino)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathOf(M, Ino);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Target = co_await Held->client().readLink(Path);
  if (!Target) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(Target.error()));
    co_return;
  }

  // Handed back as the peer stored it. The kernel resolves it here, which is
  // what makes a link to an absolute path mean this machine's path.
  ::fuse_reply_readlink(Req, Target->c_str());
}

Coro<void> doSymlink(Mount &M, ::fuse_req_t Req, std::string Target, ::fuse_ino_t Parent, std::string Name) {
  if (!knownIno(M, Parent) || !safeName(Name)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (auto R = co_await Held->client().makeLink(Path, Target); !R) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(R.error()));
    co_return;
  }

  ::fuse_entry_param E{};
  E.ino = insertIno(M, Parent, Name);
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  proto::FileAttrs A;
  A.Link = true;
  A.Mode = 0777;
  A.Size = Target.size();
  fillRemoteAttr(E.attr, E.ino, A);
  ::fuse_reply_entry(Req, &E);
}

// A second name for a file the daemon already has. The mount keys an inode by
// its path, so the new name gets its own; what the two names share is the file
// on the far side, and the link count says so.
Coro<void> doHardLink(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_ino_t Parent, std::string Name) {
  if (!knownIno(M, Ino) || !knownIno(M, Parent) || !safeName(Name)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathOf(M, Ino);
  const std::string Second = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (auto R = co_await Held->client().hardLink(Path, Second); !R) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(R.error()));
    co_return;
  }

  auto Info = co_await Held->client().stat(Second);
  if (!Info) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(Info.error()));
    co_return;
  }
  if (!Info->Found) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }

  ::fuse_entry_param E{};
  E.ino = insertIno(M, Parent, Name);
  E.attr_timeout = M.Opts.AttrTimeout;
  E.entry_timeout = M.Opts.EntryTimeout;
  fillRemoteAttr(E.attr, E.ino, Info->Attrs);
  ::fuse_reply_entry(Req, &E);
}

Coro<void> doRemove(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Parent, std::string Name, bool Directory) {
  if (!knownIno(M, Parent) || !safeName(Name)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string Path = pathUnder(M, Parent, Name);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto R = Directory ? co_await Held->client().removeDirectory(Path) : co_await Held->client().removeFile(Path);
  if (!R) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(R.error()));
    co_return;
  }

  dropIno(M, Parent, Name);
  ::fuse_reply_err(Req, 0);
}

Coro<void> doRename(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Parent, std::string Name, ::fuse_ino_t NewParent, std::string NewName) {
  if (!knownIno(M, Parent) || !knownIno(M, NewParent) || !safeName(Name) || !safeName(NewName)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  const std::string From = pathUnder(M, Parent, Name);
  const std::string To = pathUnder(M, NewParent, NewName);

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (auto R = co_await Held->client().rename(From, To); !R) {
    Held->discard();
    ::fuse_reply_err(Req, errnoOf(R.error()));
    co_return;
  }

  reparentIno(M, Parent, Name, NewParent, NewName);
  ::fuse_reply_err(Req, 0);
}

Coro<void> doSetAttr(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, struct ::stat Attr, int ToSet) {
  if (!knownIno(M, Ino)) {
    ::fuse_reply_err(Req, ENOENT);
    co_return;
  }
  if (ToSet & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
    ::fuse_reply_err(Req, EPERM);
    co_return;
  }
  const std::string Path = pathOf(M, Ino);

  if (auto R = co_await flushPath(M, Path); !R) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  if (ToSet & FUSE_SET_ATTR_SIZE) {
    if (auto R = co_await Held->client().truncate(Path, static_cast<uint64_t>(Attr.st_size)); !R) {
      Held->discard();
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
    shard(M).Written[Path]++;
  }
  if (ToSet & FUSE_SET_ATTR_MODE)
    if (auto R = co_await Held->client().setMode(Path, Attr.st_mode & 07777); !R) {
      Held->discard();
      ::fuse_reply_err(Req, EIO);
      co_return;
    }
  if (ToSet & FUSE_SET_ATTR_MTIME)
    if (auto R = co_await Held->client().setMtime(Path, static_cast<int64_t>(Attr.st_mtime)); !R) {
      Held->discard();
      ::fuse_reply_err(Req, EIO);
      co_return;
    }

  auto Info = co_await Held->client().stat(Path);
  if (!Info || !Info->Found) {
    if (!Info) Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  struct ::stat S{};
  fillRemoteAttr(S, Ino, Info->Attrs);
  ::fuse_reply_attr(Req, &S, M.Opts.AttrTimeout);
}

Coro<void> doStatFs(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino) {
  auto Held = co_await shard(M).Pool.take();
  if (!Held) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  auto Got = co_await Held->client().statFs(knownIno(M, Ino) ? pathOf(M, Ino) : M.Opts.Remote.Root);
  if (!Got) {
    Held->discard();
    ::fuse_reply_err(Req, EIO);
    co_return;
  }
  if (!Got->Ok) {
    ::fuse_reply_err(Req, EIO);
    co_return;
  }

  struct ::statvfs S{};
  S.f_bsize = Got->BlockSize;
  S.f_frsize = Got->BlockSize;
  S.f_blocks = Got->Blocks;
  S.f_bfree = Got->BlocksFree;
  S.f_bavail = Got->BlocksFree;
  S.f_files = Got->Files;
  S.f_ffree = Got->FilesFree;
  S.f_namemax = 255;
  ::fuse_reply_statfs(Req, &S);
}

void onLookup(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (!serving(M)) {
    nullLookup(Req, Parent, Name);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Parent, Name = std::string(Name)] { spawn(M, doLookup(M, Req, Parent, Name)); });
}

void onGetAttr(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    nullGetAttr(Req, Ino, Fi);
    return;
  }
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino] { spawn(M, doGetAttr(M, Req, Ino)); });
}

void onForget(::fuse_req_t Req, ::fuse_ino_t Ino, uint64_t Count) {
  forgetIno(self(Req), Ino, Count);
  ::fuse_reply_none(Req);
}

void onOpen(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    nullOpen(Req, Ino, Fi);
    return;
  }
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino, Info = *Fi] { spawn(M, doOpen(M, Req, Ino, Info)); });
}

Coro<void> doRelease(Mount &M, ::fuse_req_t Req, uint64_t Handle) {
  {
    const InUse Hold(M, Handle);
    if (File *F = shard(M).Open.get(Handle)) {
      F->Doomed = true;
      co_await settle(*F);
      [[maybe_unused]] auto Flushed = co_await flushPending(M, *F);
      for (size_t Slot = 0; Slot < F->OnSlot.size(); Slot++) {
        const File::Pinned Mine = F->OnSlot[Slot];
        if (Mine.Handle == 0) continue;
        if (auto Held = co_await shard(M).Pool.takeAt(Slot); Held && Mine.Generation == Held->generation()) {
          [[maybe_unused]] auto Closed = co_await Held->client().closeFile(Mine.Handle);
        }
      }
    }
  }
  ::fuse_reply_err(Req, 0);
}

void onRelease(::fuse_req_t Req, ::fuse_ino_t, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, 0);
    return;
  }
  onOwner(M, shardOfHandle(Fi->fh), [&M, Req, Handle = localOfHandle(Fi->fh)] { spawn(M, doRelease(M, Req, Handle)); });
}

void onRead(::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, off_t Off, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    nullRead(Req, Ino, Size, Off, Fi);
    return;
  }
  onOwner(M, Fi ? shardOfHandle(Fi->fh) : ownerOf(M, Ino), [&M, Req, Ino, Size, Off, Handle = Fi ? localOfHandle(Fi->fh) : 0] {
    spawn(M, doRead(M, Req, Ino, Size, static_cast<uint64_t>(Off), Handle));
  });
}

void onOpenDir(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_open(Req, Fi);
    return;
  }
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino, Info = *Fi] { spawn(M, doOpenDir(M, Req, Ino, Info)); });
}

void replyDir(Mount &M, ::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, off_t Off, uint64_t Handle) {
  auto It = shard(M).OpenDirs.find(Handle);
  if (It == shard(M).OpenDirs.end()) {
    ::fuse_reply_err(Req, EBADF);
    return;
  }
  const std::vector<proto::ListEntry> &Entries = It->second;

  std::vector<char> Buf(Size);
  size_t Used = 0;
  for (size_t I = static_cast<size_t>(Off); I < Entries.size() + 2; I++) {
    std::string Name;
    struct ::stat S{};
    if (I < 2) {
      Name = I == 0 ? "." : "..";
      S.st_ino = Ino;
      S.st_mode = S_IFDIR | 0755;
    } else {
      const auto &E = Entries[I - 2];
      Name = E.Name;
      fillRemoteAttr(S, reserveIno(M, Ino, E.Name), E.Attrs);
    }

    const size_t Need = ::fuse_add_direntry(Req, nullptr, 0, Name.c_str(), nullptr, 0);
    if (Used + Need > Size) break;
    Used += ::fuse_add_direntry(Req, Buf.data() + Used, Size - Used, Name.c_str(), &S, static_cast<off_t>(I + 1));
  }
  ::fuse_reply_buf(Req, Buf.data(), Used);
}

void onReadDir(::fuse_req_t Req, ::fuse_ino_t Ino, size_t Size, off_t Off, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    nullReadDir(Req, Ino, Size, Off, Fi);
    return;
  }

  onOwner(M, Fi ? shardOfHandle(Fi->fh) : ownerOf(M, Ino), [&M, Req, Ino, Size, Off, Handle = Fi ? localOfHandle(Fi->fh) : 0] {
    replyDir(M, Req, Ino, Size, Off, Handle);
  });
}

void onReleaseDir(::fuse_req_t Req, ::fuse_ino_t, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!Fi) {
    ::fuse_reply_err(Req, 0);
    return;
  }

  onOwner(M, shardOfHandle(Fi->fh), [&M, Req, Handle = localOfHandle(Fi->fh)] {
    shard(M).OpenDirs.erase(Handle);
    ::fuse_reply_err(Req, 0);
  });
}

void onStatFs(::fuse_req_t Req, ::fuse_ino_t Ino) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, ENOSYS);
    return;
  }
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino] { spawn(M, doStatFs(M, Req, Ino)); });
}

// The kernel splices into a pipe and this copies once out of it, where taking
// a plain buffer would cost that copy plus the one libfuse makes materialising
// the pipe. The bytes land on the heap rather than in registered memory: they
// are copied again into the file's pending window and never reach the fabric
// from here, so registering them would buy a global lock and nothing else.
void onWriteBuf(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_bufvec *Bufv, off_t Off, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }

  RawBytes Data(::fuse_buf_size(Bufv));

  ::fuse_bufvec Into = FUSE_BUFVEC_INIT(Data.size());
  Into.buf[0].mem = Data.data();

  const ssize_t Took = ::fuse_buf_copy(&Into, Bufv, FUSE_BUF_SPLICE_MOVE);
  if (Took < 0) {
    ::fuse_reply_err(Req, static_cast<int>(-Took));
    return;
  }
  Data.resize(static_cast<size_t>(Took));

  onOwner(M, Fi ? shardOfHandle(Fi->fh) : ownerOf(M, Ino), [&M, Req, Data = std::move(Data), Off, Handle = Fi ? localOfHandle(Fi->fh) : 0]() mutable {
    spawn(M, doWrite(M, Req, std::move(Data), static_cast<uint64_t>(Off), Handle));
  });
}

void onCreate(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name, mode_t Mode, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  if (!safeName(Name) || !knownIno(M, Parent)) {
    ::fuse_reply_err(Req, ENOENT);
    return;
  }

  const ::fuse_ino_t Mine = reserveIno(M, Parent, std::string(Name));
  onOwner(M, ownerOf(M, Mine), [&M, Req, Parent, Name = std::string(Name), Mode, Info = *Fi] {
    spawn(M, doCreate(M, Req, Parent, Name, Mode, Info));
  });
}

void onFlush(::fuse_req_t Req, ::fuse_ino_t, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, 0);
    return;
  }
  onOwner(M, Fi ? shardOfHandle(Fi->fh) : 0, [&M, Req, Handle = Fi ? localOfHandle(Fi->fh) : 0] { spawn(M, doFlush(M, Req, Handle, false)); });
}

void onFsync(::fuse_req_t Req, ::fuse_ino_t, int, ::fuse_file_info *Fi) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, 0);
    return;
  }
  onOwner(M, Fi ? shardOfHandle(Fi->fh) : 0, [&M, Req, Handle = Fi ? localOfHandle(Fi->fh) : 0] { spawn(M, doFlush(M, Req, Handle, true)); });
}

void onMkdir(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name, mode_t Mode) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Parent, Name = std::string(Name), Mode] { spawn(M, doMakeDirectory(M, Req, Parent, Name, Mode)); });
}

void onUnlink(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Parent, Name = std::string(Name)] { spawn(M, doRemove(M, Req, Parent, Name, false)); });
}

void onReadLink(::fuse_req_t Req, ::fuse_ino_t Ino) {
  Mount &M = self(Req);
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino] { spawn(M, doReadLink(M, Req, Ino)); });
}

void onSymlink(::fuse_req_t Req, const char *Target, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Target = std::string(Target), Parent, Name = std::string(Name)] {
    spawn(M, doSymlink(M, Req, Target, Parent, Name));
  });
}

void onLink(::fuse_req_t Req, ::fuse_ino_t Ino, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Ino, Parent, Name = std::string(Name)] { spawn(M, doHardLink(M, Req, Ino, Parent, Name)); });
}

void onRmdir(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Parent, Name = std::string(Name)] { spawn(M, doRemove(M, Req, Parent, Name, true)); });
}

void onRename(::fuse_req_t Req, ::fuse_ino_t Parent, const char *Name, ::fuse_ino_t NewParent, const char *NewName, unsigned int Flags) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  if (Flags != 0) {
    ::fuse_reply_err(Req, EINVAL);
    return;
  }
  onOwner(M, ownerOf(M, Parent), [&M, Req, Parent, Name = std::string(Name), NewParent, NewName = std::string(NewName)] {
    spawn(M, doRename(M, Req, Parent, Name, NewParent, NewName));
  });
}

void onSetAttr(::fuse_req_t Req, ::fuse_ino_t Ino, struct ::stat *Attr, int ToSet, ::fuse_file_info *) {
  Mount &M = self(Req);
  if (!serving(M)) {
    ::fuse_reply_err(Req, EROFS);
    return;
  }
  onOwner(M, ownerOf(M, Ino), [&M, Req, Ino, Want = *Attr, ToSet] { spawn(M, doSetAttr(M, Req, Ino, Want, ToSet)); });
}

constexpr ::fuse_lowlevel_ops kOps = {
    .init = onInit,
    .lookup = onLookup,
    .forget = onForget,
    .getattr = onGetAttr,
    .setattr = onSetAttr,
    .readlink = onReadLink,
    .mkdir = onMkdir,
    .unlink = onUnlink,
    .rmdir = onRmdir,
    .symlink = onSymlink,
    .rename = onRename,
    .link = onLink,
    .open = onOpen,
    .read = onRead,
    .flush = onFlush,
    .release = onRelease,
    .fsync = onFsync,
    .opendir = onOpenDir,
    .readdir = onReadDir,
    .releasedir = onReleaseDir,
    .statfs = onStatFs,
    .create = onCreate,
    .write_buf = onWriteBuf,
};

Result<void> buildArgs(::fuse_args &Args, const MountOptions &Opts) {
  if (::fuse_opt_add_arg(&Args, "railfs") != 0) return failMessage("fuse_opt_add_arg");
  if (Opts.MaxRead > 0) {
    const std::string Arg = "-omax_read=" + std::to_string(Opts.MaxRead);
    if (::fuse_opt_add_arg(&Args, Arg.c_str()) != 0) return failMessage("fuse_opt_add_arg");
  }
  for (const auto &O : Opts.Extra) {
    if (::fuse_opt_add_arg(&Args, "-o") != 0) return failMessage("fuse_opt_add_arg");
    if (::fuse_opt_add_arg(&Args, O.c_str()) != 0) return failMessage("fuse_opt_add_arg");
  }
  return Result<void>{};
}

Coro<void> drainQueue(Mount &M, Shard &S, int SessionFd) {
  for (;;) {
    if (S.Stopping) co_await WaitFor{S.Wake, EPOLLIN, kDrainTick};
    else co_await WaitFor{S.Wake, EPOLLIN};

    uint64_t Ticks = 0;
    while (::read(S.Wake, &Ticks, sizeof(Ticks)) > 0) {}

    std::deque<std::function<void()>> Batch;
    {
      std::lock_guard<std::mutex> Held(S.Lock);
      Batch.swap(S.Queue);
    }

    for (auto &Work : Batch) Work();

    if (!S.Stopping) continue;

    Loop::get().wake(SessionFd);

    const bool Quiet = M.Reading.load() == 0;
    bool Idle = false;
    {
      std::lock_guard<std::mutex> Held(S.Lock);
      Idle = S.Queue.empty();
    }

    if (Quiet && Idle) co_return;
  }
}

Coro<Result<void>> readRequests(Mount &M, Shard &S, int Fd) {
  ::fuse_buf Buf{};
  Result<void> Outcome{};
  M.Reading++;
  const bool Sharing = M.Shards.size() > 1;
  if (Sharing) Loop::get().share(Fd, EPOLLIN);

  while (!::fuse_session_exited(M.Session) && !S.Stopping) {
    co_await WaitFor{Fd, EPOLLIN};
    if (S.Stopping) break;

    const int Got = ::fuse_session_receive_buf(M.Session, &Buf);
    if (Got == -EINTR || Got == -EAGAIN) continue;
    if (Got == 0) break;
    if (Got < 0) {
      Outcome = fail(std::error_code(-Got, std::generic_category()), "fuse_session_receive_buf");
      break;
    }
    ::fuse_session_process_buf(M.Session, &Buf);
  }

  std::free(Buf.mem);
  M.Reading--;
  if (Sharing) Loop::get().forget(Fd);

  for (auto &Owned : M.Shards) {
    const uint64_t One = 1;
    [[maybe_unused]] const ssize_t Ignored = ::write(Owned->Wake, &One, sizeof(One));
  }
  co_return Outcome;
}

Coro<void> shardLoop(Mount &M, Shard &S, int Fd) {
  if (serving(M))
    if (auto Held = co_await S.Pool.take(); Held) S.Transfer = Held->client().maxTransfer();

  auto Reading = readRequests(M, S, Fd);
  auto Draining = drainQueue(M, S, Fd);
  Reading.start();
  Draining.start();

  if (!Reading.done()) [[maybe_unused]]
    auto Ignored = co_await Reading.join();
  if (!Draining.done()) co_await Draining.join();

  for (auto &Task : S.Running)
    if (!Task.done()) co_await Task.join();
  S.Running.clear();
  co_await S.Pool.close();
}

void shardThread(Mount &M, size_t Index, int Fd) {
  ThisShard = Index;
  Shard &S = *M.Shards[Index];

  run(shardLoop(M, S, Fd));

  Loop::get().forget(Fd);
  Loop::get().forget(S.Wake);
}

Coro<void> leaveOnSignal(Mount &M, int Wakeup, int SessionFd) {
  co_await WaitFor{Wakeup, EPOLLIN};

  char Drain[8];
  while (::read(Wakeup, Drain, sizeof(Drain)) > 0) {}

  ::fuse_session_exit(M.Session);
  Loop::get().wake(SessionFd);
}

} // namespace

Coro<Result<void>> serveMount(const MountOptions &Opts) {
  ::fuse_args Args = FUSE_ARGS_INIT(0, nullptr);
  if (auto R = buildArgs(Args, Opts); !R) {
    ::fuse_opt_free_args(&Args);
    co_return std::unexpected(R.error());
  }

  Mount M(Opts);
  for (size_t I = 0; I < M.Lanes; I++) {
    M.Shards.push_back(std::make_unique<Shard>(M.PerShard));
    M.Shards.back()->Wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (M.Shards.back()->Wake < 0) co_return failErrno("eventfd");
  }

  M.Session = ::fuse_session_new(&Args, &kOps, sizeof(kOps), &M);
  ::fuse_opt_free_args(&Args);
  if (!M.Session) co_return failMessage("fuse_session_new");

  if (::fuse_session_mount(M.Session, Opts.MountPoint.c_str()) != 0) {
    ::fuse_session_destroy(M.Session);
    co_return failMessage("fuse_session_mount");
  }

  const int Fd = ::fuse_session_fd(M.Session);
  if (auto R = setNonBlocking(Fd); !R) {
    ::fuse_session_unmount(M.Session);
    ::fuse_session_destroy(M.Session);
    co_return std::unexpected(R.error());
  }

  int Wakeup[2] = {-1, -1};
  const bool Watching = ::pipe2(Wakeup, O_CLOEXEC | O_NONBLOCK) == 0;
  if (Watching) {
    Interrupted.store(Wakeup[1], std::memory_order_relaxed);
    catchSignals(noticeSignal);
    spawn(M, leaveOnSignal(M, Wakeup[0], Fd));
  } else {
    std::fprintf(stderr, "railfs: no signal pipe; a kill will leave the mount attached\n");
  }

  if (serving(M)) {
    if (auto Held = co_await shard(M).Pool.take(); Held) shard(M).Transfer = Held->client().maxTransfer();
  }

  if (serving(M))
    if (auto R = co_await shard(M).Pool.warm(); !R)
      std::fprintf(stderr, "railfs: %s; serving anyway, sessions open on demand\n", R.error().message().c_str());

  if (serving(M) && M.Opts.WriteChunk < 2 * shard(M).Transfer)
    std::fprintf(stderr,
                 "railfs: --write-chunk %llu is below twice the %llu byte page, so writes stay ranged\n",
                 static_cast<unsigned long long>(M.Opts.WriteChunk),
                 static_cast<unsigned long long>(shard(M).Transfer));

  for (size_t I = 1; I < M.Shards.size(); I++) M.Shards[I]->Thread = std::thread(shardThread, std::ref(M), I, Fd);

  auto Draining = drainQueue(M, shard(M), Fd);
  Draining.start();

  Result<void> Outcome = co_await readRequests(M, shard(M), Fd);

  if (Watching) {
    catchSignals(SIG_DFL);
    Interrupted.store(-1, std::memory_order_relaxed);
    const char Wake = 1;
    [[maybe_unused]] const ssize_t Ignored = ::write(Wakeup[1], &Wake, 1);
  }

  for (auto &Owned : M.Shards) {
    {
      std::lock_guard<std::mutex> Held(Owned->Lock);
      Owned->Stopping = true;
    }
    const uint64_t One = 1;
    [[maybe_unused]] const ssize_t Ignored = ::write(Owned->Wake, &One, sizeof(One));
  }

  for (auto &Owned : M.Shards)
    if (Owned->Thread.joinable()) Owned->Thread.join();

  if (!Draining.done()) co_await Draining.join();

  for (auto &Task : shard(M).Running)
    if (!Task.done()) co_await Task.join();
  shard(M).Running.clear();
  co_await shard(M).Pool.close();

  if (Watching) {
    Loop::get().forget(Wakeup[0]);
    ::close(Wakeup[0]);
    ::close(Wakeup[1]);
  }

  const uint64_t Served = M.Reads.load();
  if (Served > 0)
    std::fprintf(stderr,
                 "railfs: %llu reads, %llu bytes requested, %llu bytes per read, %llu bytes fetched\n",
                 static_cast<unsigned long long>(Served),
                 static_cast<unsigned long long>(M.ReadBytes.load()),
                 static_cast<unsigned long long>(M.ReadBytes.load() / Served),
                 static_cast<unsigned long long>(M.Fetched.load()));

  const auto Pages = Memory::get().stats();
  if (Pages.Direct + Pages.Copied > 0)
    std::fprintf(stderr,
                 "railfs: %llu pages landed direct, %llu copied\n",
                 static_cast<unsigned long long>(Pages.Direct),
                 static_cast<unsigned long long>(Pages.Copied));

  Loop::get().forget(Fd);
  for (auto &Owned : M.Shards)
    if (Owned->Wake >= 0) ::close(Owned->Wake);
  ::fuse_session_unmount(M.Session);
  ::fuse_session_destroy(M.Session);
  co_return Outcome;
}

} // namespace rail::fuse
