#include "rail/file-service.h"
#include "rail/fs/writer.h"
#include "rail/io/runner.h"
#include "rail/memory.h"

#include "rail/app/checksum.h"
#include "rail/fs/safe-path.h"
#include "rail/io/offload.h"
#include "rail/io/stream.h"
#include "rail/io/trace.h"
#include "rail/io/uring.h"
#include "rail/proto/control-channel.h"
#include "rail/stream/page-stream.h"
#include "rail/stream/sink.h"
#include "rail/transport/data-channel.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace rail {

namespace {

// Without following: a link is reported as itself, so a client can see one, and
// a dangling one is a link rather than a missing file. What it points at is the
// client's to resolve - the target may name a path that only means something
// there.
proto::FileAttrs attrsOf(const std::filesystem::path &P, bool &Found) {
  proto::FileAttrs A;
  struct ::stat St{};
  if (::lstat(P.c_str(), &St) != 0) {
    Found = false;
    return A;
  }
  Found = true;
  A.Size = static_cast<uint64_t>(St.st_size);
  A.Mode = St.st_mode & 07777;
  A.Mtime = St.st_mtime;
  A.Directory = S_ISDIR(St.st_mode);
  A.Link = S_ISLNK(St.st_mode);
  A.Links = static_cast<uint32_t>(St.st_nlink);
  return A;
}

proto::ListReply listDirectory(const std::filesystem::path &Dir) {
  proto::ListReply Listed;
  std::error_code EC;
  for (const auto &Entry : std::filesystem::directory_iterator(Dir, EC)) {
    proto::ListEntry E;
    E.Name = Entry.path().filename().string();
    bool Found = false;
    E.Attrs = attrsOf(Entry.path(), Found);
    if (Found) Listed.Entries.push_back(std::move(E));
  }
  Listed.Found = !EC;
  return Listed;
}

// One per daemon, not one per session. A mount spreads its operations over
// however many connections it has, so the session that unlinks a file is not
// the one holding a descriptor to it - and a cache per session means every
// other session goes on pinning the blocks of a file nobody can name any more.
class FdCache {
public:
  explicit FdCache(size_t Limit) : Limit(Limit) {}

  FdCache(const FdCache &) = delete;
  FdCache &operator=(const FdCache &) = delete;

  class Handle {
  public:
    explicit Handle(int Fd) : Fd(Fd) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() {
      if (Fd >= 0) ::close(Fd);
    }

    int fd() const { return Fd; }

  private:
    int Fd = -1;
  };

  using Held = std::shared_ptr<Handle>;

  Result<Held> get(const std::filesystem::path &Path, int Flags) {
    const std::lock_guard<std::mutex> Only(Guard);
    const std::string Key = std::format("{}:{}", Flags, Path.string());

    struct ::stat Now{};
    const bool Known = ::stat(Path.c_str(), &Now) == 0;

    if (auto It = Open.find(Key); It != Open.end()) {
      if (Known && It->second.Device == Now.st_dev && It->second.Inode == Now.st_ino) {
        Order.splice(Order.begin(), Order, It->second.Position);
        return It->second.File;
      }
      Order.erase(It->second.Position);
      Open.erase(It);
    }

    const int Fd = ::open(Path.c_str(), Flags, 0644);
    if (Fd < 0) return failErrno(std::format("open {}", Path.string()));

    // A ranged reader walks forward but asks one block at a time, so the
    // kernel's default readahead window is the ceiling on how fast it can be
    // served. Direct reads are faster in bulk but give this up, and measured
    // 35% slower for the block sizes a mount uses.
    ::posix_fadvise(Fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    if (Open.size() >= Limit) evictOldest();

    auto File = std::make_shared<Handle>(Fd);
    Order.push_front(Key);
    Open.emplace(Key, Entry{File, Order.begin(), Known ? Now.st_dev : 0, Known ? Now.st_ino : 0});
    return File;
  }

  // Drops whatever is cached for a path, whichever flags it was opened with.
  // Without this an unlinked file keeps its descriptor here and so keeps its
  // blocks: deleting through the mount frees nothing, and the export fills up
  // until a writer gets ENOSPC for space nothing is using.
  void forget(const std::filesystem::path &Path) {
    const std::lock_guard<std::mutex> Only(Guard);
    const std::string Tail = ":" + Path.string();

    for (auto It = Open.begin(); It != Open.end();) {
      if (It->first.size() <= Tail.size() || !It->first.ends_with(Tail)) {
        ++It;
        continue;
      }

      Order.erase(It->second.Position);
      It = Open.erase(It);
    }
  }

private:
  struct Entry {
    Held File;
    std::list<std::string>::iterator Position;
    dev_t Device = 0;
    ino_t Inode = 0;
  };

  void evictOldest() {
    if (Order.empty()) return;
    Open.erase(Order.back());
    Order.pop_back();
  }

  size_t Limit;
  std::mutex Guard;
  std::list<std::string> Order;
  std::unordered_map<std::string, Entry> Open;
};

// Shared by every session this daemon serves, so an unlink on one connection
// releases what another was holding.
FdCache &openFiles() {
  static FdCache Cache{256};
  return Cache;
}

std::filesystem::path pathOfOpenFile(int Fd) { return std::filesystem::path("/proc/self/fd") / std::to_string(Fd); }

// A client holds a descriptor on the daemon for every file it opens, and a
// client that never closes would take them from every other client. Refusing
// past a bound turns that into one client's open failing.
constexpr size_t kMaxPinned = 4096;

class Service {
public:
  Service(std::filesystem::path Root, bool FlipOneBit) : Root(std::move(Root)), FlipOneBit(FlipOneBit) {}

  Coro<Result<void>> serveAndClose(Stream Client) {
    auto Outcome = co_await serve(std::move(Client));
    Control.close();
    if (Channel) Channel->close();

    // Every session prints its thread's stages as it goes, and dumping clears
    // them, so a tuning run sums the lines rather than reading one of them.
    // Off unless RAIL_TRACE is set.
    Trace::dump("raild");
    co_return Outcome;
  }

  Coro<Result<void>> serve(Stream Client) {
    Control = proto::ControlChannel(std::move(Client));

    if (auto R = co_await negotiate(); !R) co_return std::unexpected(R.error());

    Result<void> Outcome{};
    for (;;) {
      auto M = co_await Control.receive();
      if (!M) break;
      if (std::get_if<proto::End>(&*M)) break;

      for (size_t I = Running.size(); I-- > 0;)
        if (Running[I].done()) {
          if (auto R = Running[I].result(); !R && Outcome) Outcome = std::unexpected(R.error());
          Running.erase(Running.begin() + static_cast<long>(I));
        }

      while (Running.size() >= kMaxInFlight) {
        if (auto R = co_await Running.front().join(); !R && Outcome) Outcome = std::unexpected(R.error());
        Running.erase(Running.begin());
      }

      if (!Outcome) break;

      // A store reads the frames that follow it - the payload, then a digest -
      // straight off this channel, so it cannot run beside a loop that is also
      // reading. Everything else only answers, and answering is serialised by
      // the channel itself.
      if (std::get_if<proto::StoreRequest>(&*M)) {
        if (auto R = co_await quiesce(); !R) Outcome = std::unexpected(R.error());
        if (!Outcome) break;
        if (auto R = co_await dispatch(std::move(*M)); !R) Outcome = std::unexpected(R.error());
        if (!Outcome) break;
        continue;
      }

      Running.push_back(dispatch(std::move(*M)));
      Running.back().start();
    }

    // Nothing may outlive the session: the control channel and the data
    // channel close the moment this returns.
    if (auto R = co_await quiesce(); !R && Outcome) Outcome = std::unexpected(R.error());
    co_return Outcome;
  }

  Coro<Result<void>> quiesce() {
    Result<void> Outcome{};
    for (auto &Task : Running)
      if (auto R = co_await Task.join(); !R && Outcome) Outcome = std::unexpected(R.error());
    Running.clear();
    co_return Outcome;
  }

  Coro<Result<void>> dispatch(proto::Message M) {
    if (auto *S = std::get_if<proto::StatRequest>(&M)) co_return co_await onStat(*S);
    if (auto *L = std::get_if<proto::ListRequest>(&M)) co_return co_await onList(*L);
    if (auto *Rd = std::get_if<proto::ReadRequest>(&M)) co_return co_await onRead(*Rd);
    if (auto *Wr = std::get_if<proto::WriteRequest>(&M)) co_return co_await onWrite(*Wr);
    if (auto *F = std::get_if<proto::FetchRequest>(&M)) co_return co_await onFetch(*F);
    if (auto *Meta = std::get_if<proto::MetaRequest>(&M)) co_return co_await onMeta(*Meta);
    if (auto *Fs = std::get_if<proto::StatFsRequest>(&M)) co_return co_await onStatFs(*Fs);
    if (auto *St = std::get_if<proto::StoreRequest>(&M)) co_return co_await onStore(*St);
    if (auto *Op = std::get_if<proto::OpenRequest>(&M)) co_return co_await onOpen(*Op);
    if (auto *Cl = std::get_if<proto::CloseRequest>(&M)) co_return co_await onClose(*Cl);
    co_return failMessage(std::format("unexpected message {}", proto::typeName(proto::typeOf(M))));
  }

  Coro<Result<void>> negotiate() {
    auto H = co_await Control.expect<proto::Hello>();
    if (!H) co_return std::unexpected(H.error());
    if (H->Version != proto::kVersion) co_return failMessage("protocol version mismatch");

    Verify = H->Verify;

    if (H->Sum != static_cast<uint8_t>(Sum::XxH3) && H->Sum != static_cast<uint8_t>(Sum::XxH64))
      co_return failMessage(std::format("unknown checksum {}", static_cast<unsigned>(H->Sum)));

    Agreed = static_cast<Sum>(H->Sum);

    Channel = makeDataChannel(H->Backend, H->PageCount, H->PageSize, "");
    if (!Channel) co_return failMessage(std::format("unknown backend: {}", H->Backend));

    auto Endpoint = co_await Channel->listen();
    if (!Endpoint) co_return std::unexpected(Endpoint.error());

    proto::HelloAck Ack;
    Ack.Backend = H->Backend;
    Ack.ChannelEndpoint = *Endpoint;
    Channel->watch(Control.readFd());
    if (auto R = co_await Control.send(Ack); !R) co_return std::unexpected(R.error());

    auto Peer = co_await Control.expect<proto::PeerEndpoint>();
    if (!Peer) co_return std::unexpected(Peer.error());
    if (auto R = co_await Channel->attachPeer(Peer->Blob); !R) co_return std::unexpected(R.error());

    co_return co_await Channel->acceptPeer();
  }

  Coro<Result<void>> onOpen(const proto::OpenRequest &O) {
    proto::OpenReply Reply;
    Reply.Id = O.Id;

    auto Path = underRoot(Root, O.Path);
    if (!Path) {
      Reply.Error = Path.error().message();
      Reply.Errno = EACCES;
      co_return co_await Control.send(Reply);
    }

    Reply.Attrs = attrsOf(*Path, Reply.Found);
    if (!Reply.Found) {
      Reply.Error = std::format("no such file: {}", O.Path);
      Reply.Errno = ENOENT;
      co_return co_await Control.send(Reply);
    }
    if (Reply.Attrs.Directory) {
      Reply.Error = std::format("not a file: {}", O.Path);
      Reply.Errno = EISDIR;
      co_return co_await Control.send(Reply);
    }

    if (Pinned.size() >= kMaxPinned) {
      Reply.Error = std::format("too many open files on this connection, {} already", Pinned.size());
      Reply.Errno = EMFILE;
      co_return co_await Control.send(Reply);
    }

    const int Fd = ::open(Path->c_str(), (O.Writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (Fd < 0) {
      // Read before anything else can overwrite it, formatting included.
      Reply.Errno = static_cast<uint32_t>(errno);
      Reply.Error = failErrno(std::format("open {}", Path->string())).error().message();
      co_return co_await Control.send(Reply);
    }
    ::posix_fadvise(Fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    Reply.Handle = NextHandle++;
    Reply.Ok = true;
    Pinned.emplace(Reply.Handle, std::make_shared<FdCache::Handle>(Fd));
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onClose(const proto::CloseRequest &C) {
    proto::MetaReply Reply;
    Reply.Id = C.Id;
    Reply.Ok = Pinned.erase(C.Handle) > 0;
    if (!Reply.Ok) Reply.Error = "no such handle";
    co_return co_await Control.send(Reply);
  }

  Result<FdCache::Held> heldFor(uint64_t Handle, const std::string &Named, int Flags) {
    if (Handle != 0) {
      auto It = Pinned.find(Handle);
      if (It == Pinned.end()) return failMessage(std::format("no such handle {}", Handle));
      return It->second;
    }

    auto Path = underRoot(Root, Named);
    if (!Path) return std::unexpected(Path.error());
    if (Flags & O_CREAT) {
      std::error_code EC;
      std::filesystem::create_directories(Path->parent_path(), EC);
    }
    return openFiles().get(*Path, Flags);
  }

  Coro<Result<void>> onStat(const proto::StatRequest &S) {
    proto::StatReply Reply;
    Reply.Id = S.Id;
    if (auto Path = underRoot(Root, S.Path); Path) Reply.Attrs = attrsOf(*Path, Reply.Found);
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onList(const proto::ListRequest &L) {
    proto::ListReply Reply;
    Reply.Id = L.Id;

    auto Path = underRoot(Root, L.Path);
    if (Path) {
      auto Listed = co_await offLoop([Dir = *Path] { return listDirectory(Dir); });
      Reply.Found = Listed.Found;
      Reply.Entries = std::move(Listed.Entries);
    }
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onRead(const proto::ReadRequest &Rd) {
    proto::TransferReply Reply;
    Reply.Id = Rd.Id;

    Page Buf;
    {
      Scoped T("rd.acquire");
      Buf = co_await Channel->pool().acquire();
    }
    if (!Buf.valid()) {
      Reply.Error = "out of registered memory for a transfer page";
      co_return co_await Control.send(Reply);
    }

    // Only the page bounds it. The client posts a receive for exactly what it
    // asked for, so sending less than that hangs a byte-stream transport and
    // fails the digest on a tagged one.
    const uint32_t Want = static_cast<uint32_t>(std::min<size_t>(Rd.Length, Buf.capacity()));
    Buf.resize(Want);

    uint32_t Read = 0;
    {
      Scoped T("rd.disk");
      Read = co_await readInto(Rd, Buf, Want, Reply);
    }
    if (Read < Want) std::memset(Buf.bytes() + Read, 0, Want - Read);
    Reply.Length = Read;

    {
      Scoped T("rd.hash");
      Verifier PageHash(Verify, Agreed);
      PageHash.update({Buf.bytes(), Want});
      Reply.Payload = PageHash.digest();
    }

    {
      Scoped T("rd.reply");
      if (auto R = co_await Control.send(Reply); !R) co_return std::unexpected(R.error());
    }
    if (Want == 0) co_return Result<void>{};

    Buf.resize(Want);
    {
      Scoped T("rd.payload");
      co_return co_await Channel->send(Buf, Rd.Id);
    }
  }

  Coro<uint32_t> readInto(const proto::ReadRequest &Rd, Page &Buf, uint32_t Want, proto::TransferReply &Reply) {
    auto File = heldFor(Rd.Handle, Rd.Path, O_RDONLY);
    if (!File) {
      Reply.Error = File.error().message();
      co_return 0;
    }

    struct ::stat St{};
    if (::fstat((*File)->fd(), &St) == 0) Reply.FileSize = static_cast<uint64_t>(St.st_size);

    Uring::Read Op;
    Op.Fd = (*File)->fd();
    Op.Offset = Rd.Offset;
    Op.Dst = std::span<std::byte>(Buf.bytes(), Want);
    if (auto R = Uring::get().submit(Op); !R) {
      Reply.Error = R.error().message();
      co_return 0;
    }

    auto Got = co_await Uring::get().await(Op);
    if (!Got) {
      Reply.Error = Got.error().message();
      co_return 0;
    }

    Reply.Ok = true;
    co_return static_cast<uint32_t>(*Got);
  }

  Result<void> applyMeta(const proto::MetaRequest &Meta, const std::filesystem::path &Path) {
    switch (Meta.Op) {
    case proto::MetaOp::MakeDirectory:
      if (::mkdir(Path.c_str(), Meta.Mode ? Meta.Mode : 0755) != 0) return failErrno("mkdir");
      return {};
    case proto::MetaOp::RemoveFile:
      if (::unlink(Path.c_str()) != 0) return failErrno("unlink");
      openFiles().forget(Path);
      return {};
    case proto::MetaOp::RemoveDirectory:
      if (::rmdir(Path.c_str()) != 0) return failErrno("rmdir");
      return {};
    case proto::MetaOp::Truncate:
      if (::truncate(Path.c_str(), static_cast<off_t>(Meta.Size)) != 0) return failErrno("truncate");
      return {};
    case proto::MetaOp::SetMode:
      if (::chmod(Path.c_str(), Meta.Mode) != 0) return failErrno("chmod");
      return {};
    case proto::MetaOp::SetMtime: {
      struct ::timespec Times[2];
      Times[0].tv_sec = 0;
      Times[0].tv_nsec = UTIME_OMIT;
      Times[1].tv_sec = static_cast<time_t>(Meta.Mtime);
      Times[1].tv_nsec = 0;
      if (::utimensat(AT_FDCWD, Path.c_str(), Times, 0) != 0) return failErrno("utimensat");
      return {};
    }
    case proto::MetaOp::Fsync:
      return failMessage("fsync is answered before it reaches here");
    case proto::MetaOp::Symlink: {
      // The target is stored as the peer sent it and never resolved here: it is
      // read back on a machine where it may mean something quite different, and
      // underRoot would refuse an absolute one that is perfectly good there.
      if (Meta.Target.empty()) return failMessage("refusing a link to nowhere");
      if (::symlink(Meta.Target.c_str(), Path.c_str()) != 0) return failErrno("symlink");
      return {};
    }
    case proto::MetaOp::ReadLink:
      return failMessage("readlink returns a target and is handled by the caller");
    case proto::MetaOp::Rename:
      return failMessage("rename needs both paths and is handled by the caller");
    case proto::MetaOp::HardLink:
      return failMessage("a hard link needs both paths and is handled by the caller");
    }
    return failMessage("unknown metadata operation");
  }

  static void refused(proto::MetaReply &Reply, const Error &Why) {
    Reply.Error = Why.message();
    Reply.Errno = Why.Code.category() == std::generic_category() ? static_cast<uint32_t>(Why.Code.value()) : EACCES;
  }

  static void landed(proto::MetaReply &Reply, int Failed, const char *What) {
    if (Failed == 0) {
      Reply.Ok = true;
      return;
    }
    Reply.Errno = static_cast<uint32_t>(errno);
    Reply.Error = failErrno(What).error().message();
  }

  static Coro<Result<void>> syncFile(int Fd) {
    Uring::Fsync Op;
    Op.Fd = Fd;
    if (Uring::get().usable()) {
      if (auto R = Uring::get().submit(Op); !R) co_return R;
      co_return co_await Uring::get().await(Op);
    }
    if (::fsync(Fd) != 0) co_return failErrno("fsync");
    co_return Result<void>{};
  }

  Coro<Result<void>> onMeta(const proto::MetaRequest &Meta) {
    proto::MetaReply Reply;
    Reply.Id = Meta.Id;

    auto Path = underRoot(Root, Meta.Path);
    if (!Path) {
      refused(Reply, Path.error());
      co_return co_await Control.send(Reply);
    }

    // Rename and a hard link both name a second path, and neither may leave the
    // root, so they are checked together and differ only in the call.
    const bool Linking = Meta.Op == proto::MetaOp::HardLink;
    if (Linking || Meta.Op == proto::MetaOp::Rename) {
      auto Target = underRoot(Root, Meta.Target);
      if (!Target) {
        refused(Reply, Target.error());
        co_return co_await Control.send(Reply);
      }
      if (Linking) landed(Reply, ::link(Path->c_str(), Target->c_str()), "link");
      else landed(Reply, ::rename(Path->c_str(), Target->c_str()), "rename");
      co_return co_await Control.send(Reply);
    }

    if (Meta.Op == proto::MetaOp::Fsync) {
      auto File = heldFor(Meta.Handle, Meta.Path, O_WRONLY);
      if (!File) {
        refused(Reply, File.error());
        co_return co_await Control.send(Reply);
      }
      if (auto R = co_await syncFile((*File)->fd()); !R) refused(Reply, R.error());
      else Reply.Ok = true;
      co_return co_await Control.send(Reply);
    }

    if (Meta.Op == proto::MetaOp::ReadLink) {
      std::string Target(PATH_MAX, '\0');
      const ssize_t Wrote = ::readlink(Path->c_str(), Target.data(), Target.size());
      landed(Reply, Wrote < 0 ? -1 : 0, "readlink");
      if (Wrote >= 0) {
        Target.resize(static_cast<size_t>(Wrote));
        Reply.Target = std::move(Target);
      }
      co_return co_await Control.send(Reply);
    }

    if (auto R = applyMeta(Meta, *Path); !R) refused(Reply, R.error());
    else Reply.Ok = true;
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onStatFs(const proto::StatFsRequest &Fs) {
    proto::StatFsReply Reply;
    Reply.Id = Fs.Id;

    auto Path = underRoot(Root, Fs.Path);
    if (!Path) {
      Reply.Error = Path.error().message();
      co_return co_await Control.send(Reply);
    }

    struct ::statvfs Info{};
    if (::statvfs(Path->c_str(), &Info) != 0) {
      Reply.Error = failErrno("statvfs").error().message();
      co_return co_await Control.send(Reply);
    }

    Reply.Ok = true;
    Reply.BlockSize = Info.f_bsize;
    Reply.Blocks = Info.f_blocks;
    Reply.BlocksFree = Info.f_bavail;
    Reply.Files = Info.f_files;
    Reply.FilesFree = Info.f_ffree;
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onFetch(const proto::FetchRequest &F) {
    proto::StreamReply Reply;
    Reply.Id = F.Id;

    auto Held = heldFor(F.Handle, F.Path, O_RDONLY);
    if (!Held) {
      Reply.Error = Held.error().message();
      co_return co_await Control.send(Reply);
    }

    auto Source = FileReader::open(pathOfOpenFile((*Held)->fd()), Access::Direct);
    if (!Source) {
      Reply.Error = Source.error().message();
      co_return co_await Control.send(Reply);
    }

    const uint64_t Size = Source->meta().Size;
    if (F.Offset > Size) {
      Reply.Ok = true;
      co_return co_await Control.send(Reply);
    }

    Reply.Ok = true;
    Reply.Length = std::min(F.Length, Size - F.Offset);
    if (auto R = co_await Control.send(Reply); !R) co_return std::unexpected(R.error());
    if (Reply.Length == 0) co_return Result<void>{};

    FileSource Reading(*Source);
    PageSender Sender(*Channel, Reading, F.TagBase, StreamGeometry::forChannel(*Channel), FlipOneBit, Verify, Agreed);
    auto Sent = co_await Sender.stream(F.Offset, Reply.Length);
    Trace::dump("server");

    proto::StreamDigest Digest;
    Digest.Id = F.Id;
    if (Sent) {
      Digest.Ok = true;
      Digest.Whole = Sender.digest();
    } else {
      Digest.Error = Sent.error().message();
    }
    co_return co_await Control.send(Digest);
  }

  Coro<Result<void>> storeAtOffset(const proto::StoreRequest &St) {
    proto::StreamReply Reply;
    Reply.Id = St.Id;

    auto File = heldFor(St.Handle, St.Path, O_WRONLY | O_CREAT);
    if (!File) {
      Reply.Error = File.error().message();
      co_return co_await Control.send(Reply);
    }

    proto::StreamReply Ready;
    Ready.Id = St.Id;
    Ready.Ok = true;
    if (auto R = co_await Control.send(Ready); !R) co_return std::unexpected(R.error());

    DescriptorSink Landing((*File)->fd());
    PageReceiver Receiver(*Channel, Landing, St.TagBase, StreamGeometry::forChannel(*Channel), Verify, Agreed);
    auto Landed = co_await Receiver.land(St.Offset, St.Length);
    if (!Landed) {
      Reply.Error = Landed.error().message();
      [[maybe_unused]] auto Told = co_await Control.send(Reply);
      co_return std::unexpected(Landed.error());
    }

    auto Sent = co_await Control.expect<proto::StreamDigest>();
    if (!Sent) co_return std::unexpected(Sent.error());

    if (!Sent->Ok) {
      Reply.Error = Sent->Error;
      co_return co_await Control.send(Reply);
    }
    if (!Receiver.matches(Sent->Whole)) {
      Reply.Error = "whole-transfer hash mismatch";
      co_return co_await Control.send(Reply);
    }

    Reply.Ok = true;
    Reply.Length = St.Length;
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onStore(const proto::StoreRequest &St) {
    proto::StreamReply Reply;
    Reply.Id = St.Id;

    auto Path = underRoot(Root, St.Path);
    if (!Path) {
      Reply.Error = Path.error().message();
      co_return co_await Control.send(Reply);
    }

    if (St.Offset != 0 && St.Truncate) {
      Reply.Error = "a truncating store replaces the whole file and must start at offset 0";
      co_return co_await Control.send(Reply);
    }

    if (St.Handle != 0) {
      auto File = heldFor(St.Handle, St.Path, O_WRONLY);
      if (!File) {
        Reply.Error = File.error().message();
        co_return co_await Control.send(Reply);
      }
      if (St.Truncate && ::ftruncate((*File)->fd(), 0) != 0) {
        Reply.Error = failErrno("ftruncate").error().message();
        co_return co_await Control.send(Reply);
      }
      co_return co_await storeAtOffset(St);
    }

    if (!St.Truncate) co_return co_await storeAtOffset(St);

    std::error_code EC;
    std::filesystem::create_directories(Path->parent_path(), EC);

    FileMeta Meta;
    Meta.Size = St.Length;
    Meta.Mode = 0644;
    const bool Direct = Channel->pool().pageSize() % kDirectAlignment == 0;

    auto Sink = FileWriter::create(*Path, Meta, Durability::PageCache, Direct);
    if (!Sink) {
      Reply.Error = Sink.error().message();
      co_return co_await Control.send(Reply);
    }

    proto::StreamReply Ready;
    Ready.Id = St.Id;
    Ready.Ok = true;
    if (auto R = co_await Control.send(Ready); !R) co_return std::unexpected(R.error());

    FileSink Landing(*Sink);
    PageReceiver Receiver(*Channel, Landing, St.TagBase, StreamGeometry::forChannel(*Channel), Verify, Agreed);
    auto Landed = co_await Receiver.land(St.Offset, St.Length);
    if (!Landed) {
      Reply.Error = Landed.error().message();
      [[maybe_unused]] auto Told = co_await Control.send(Reply);
      co_return std::unexpected(Landed.error());
    }

    auto Sent = co_await Control.expect<proto::StreamDigest>();
    if (!Sent) co_return std::unexpected(Sent.error());

    if (!Sent->Ok) {
      Reply.Error = Sent->Error;
      co_return co_await Control.send(Reply);
    }
    if (!Receiver.matches(Sent->Whole)) {
      Reply.Error = "whole-transfer hash mismatch";
      co_return co_await Control.send(Reply);
    }

    if (auto R = Sink->commit(); !R) {
      Reply.Error = R.error().message();
      co_return co_await Control.send(Reply);
    }

    Reply.Ok = true;
    Reply.Length = St.Length;
    co_return co_await Control.send(Reply);
  }

  Coro<Result<void>> onWrite(const proto::WriteRequest &Wr) {
    proto::TransferReply Reply;
    Reply.Id = Wr.Id;

    Page Buf;
    {
      Scoped T("wr.acquire");
      Buf = co_await Channel->pool().acquire();
    }
    if (!Buf.valid()) {
      Reply.Error = "out of registered memory for a transfer page";
      co_return co_await Control.send(Reply);
    }
    if (Wr.Length > Buf.capacity()) {
      Reply.Error = std::format("write of {} bytes exceeds the page size", Wr.Length);
      co_return co_await Control.send(Reply);
    }
    Buf.resize(Wr.Length);

    {
      Scoped T("wr.payload");
      if (auto R = co_await Channel->recv(Buf, Wr.Id, Wr.Length); !R) co_return std::unexpected(R.error());
    }

    Verifier Landed(Verify, Agreed);
    {
      Scoped T("wr.hash");
      Landed.update({Buf.bytes(), Wr.Length});
    }
    if (!Landed.matches(Wr.Payload)) {
      Reply.Error = "page hash mismatch";
      co_return co_await Control.send(Reply);
    }

    // Straight to the disk when the shape allows it. The peer's page cache
    // otherwise takes every byte twice - once on the way in and again on the
    // flush - and that costs it 6.0 GiB/s against 10.8 with sixteen writers.
    // The cache of descriptors is keyed by flags, so this is its own entry and
    // readers keep the buffered one they had.
    const bool Aligned =
        Wr.Offset % kDirectAlignment == 0 && Wr.Length % kDirectAlignment == 0 && reinterpret_cast<uintptr_t>(Buf.bytes()) % kDirectAlignment == 0;

    auto File = heldFor(Wr.Handle, Wr.Path, O_WRONLY | O_CREAT | (Aligned ? O_DIRECT : 0));
    if (!File) {
      Reply.Error = File.error().message();
      co_return co_await Control.send(Reply);
    }

    if (Wr.Truncate && ::ftruncate((*File)->fd(), 0) != 0) {
      Reply.Error = std::format("cannot truncate {}", Wr.Path);
      co_return co_await Control.send(Reply);
    }

    Uring::Write Op;
    Op.Fd = (*File)->fd();
    Op.Offset = Wr.Offset;
    Op.Src = std::span<const std::byte>(Buf.bytes(), Wr.Length);
    {
      if (auto R = Uring::get().submit(Op); !R) {
        Reply.Error = R.error().message();
        co_return co_await Control.send(Reply);
      }

      if (auto R = co_await Uring::get().await(Op); !R) {
        Reply.Error = R.error().message();
        co_return co_await Control.send(Reply);
      }
    }

    Reply.Ok = true;
    Reply.Length = Wr.Length;
    co_return co_await Control.send(Reply);
  }

  std::filesystem::path Root;
  bool FlipOneBit = false;
  bool Verify = true;
  Sum Agreed = Sum::XxH3;
  std::unordered_map<uint64_t, FdCache::Held> Pinned;
  uint64_t NextHandle = 1;

  // Requests answered at once on one connection. Serving them one at a time
  // held a client to a request per round trip however much the daemon had
  // spare; the pool the payloads come from is what bounds the memory, so this
  // only has to stop the queue growing without limit.
  static constexpr size_t kMaxInFlight = 64;
  std::vector<Coro<Result<void>>> Running;
  proto::ControlChannel Control;
  std::unique_ptr<DataChannel> Channel;
};

} // namespace

namespace {

// A session costs a socket, and every file it holds open costs another. The
// bound is deliberately pessimistic: running out of descriptors takes down
// every client at once, and refusing one is far cheaper.
constexpr size_t kDescriptorsPerSession = 4;
constexpr size_t kDescriptorsKeptBack = 64;

// What one client actually costs, measured rather than reasoned about: the
// daemon's resident size over one to sixteen connections under load grows by
// this much per connection. Nearly all of it is the transfer pool a channel
// builds for itself - twelve pages of eight megabytes - so a smaller page
// geometry buys proportionally more clients.
constexpr size_t kBytesPerSession = 65u << 20;

// What of the machine's free memory a file server may plan to use. The rest is
// the page cache it is reading through, which is what makes it quick.
constexpr double kMemoryShare = 0.25;

// Answering is work, and one core cannot answer for an unbounded crowd. Loose
// enough not to bind on a machine with real memory.
constexpr size_t kSessionsPerCore = 256;

size_t descriptorCeiling() {
  ::rlimit Limit{};
  if (::getrlimit(RLIMIT_NOFILE, &Limit) != 0) return 256;
  const size_t Have = static_cast<size_t>(Limit.rlim_cur);
  if (Have <= kDescriptorsKeptBack) return 1;
  return (Have - kDescriptorsKeptBack) / kDescriptorsPerSession;
}

// MemAvailable, not MemFree: the page cache is reclaimable and a file server
// lives in it.
size_t memoryCeiling() {
  std::ifstream Info("/proc/meminfo");
  std::string Name;
  uint64_t Value = 0;
  std::string Unit;
  while (Info >> Name >> Value >> Unit)
    if (Name == "MemAvailable:") {
      const auto Usable = static_cast<double>(Value) * 1024.0 * kMemoryShare;
      return std::max<size_t>(1, static_cast<size_t>(Usable / kBytesPerSession));
    }
  return std::numeric_limits<size_t>::max();
}

// A session's pages are reserved from shared registered memory for as long as
// it lives, so the machine's free memory is not the only bound: promising more
// sessions than that memory can back turns a refusal into one at connect time.
size_t registeredCeiling(const ServiceOptions &Opts) {
  const size_t PerSession = Opts.PageCount * Opts.PageSize;
  if (PerSession == 0) return std::numeric_limits<size_t>::max();
  return std::max<size_t>(1, Memory::get().capacity() / PerSession);
}

size_t coreCeiling() {
  const unsigned Cores = std::thread::hardware_concurrency();
  return std::max<size_t>(1, Cores) * kSessionsPerCore;
}

} // namespace

size_t sessionsAffordable(const ServiceOptions &Opts, bool Explain) {
  if (Opts.MaxSessions > 0) return Opts.MaxSessions;

  const size_t ByFds = descriptorCeiling();
  const size_t ByMemory = memoryCeiling();
  const size_t ByPages = registeredCeiling(Opts);
  const size_t ByCores = coreCeiling();
  const size_t Held = std::min({ByFds, ByMemory, ByPages, ByCores});

  if (Explain)
    std::fprintf(stderr,
                 "raild: serving up to %zu clients (descriptors allow %zu, memory %zu, pages %zu, cores %zu)\n",
                 Held,
                 ByFds,
                 ByMemory,
                 ByPages,
                 ByCores);
  return Held;
}

namespace {

// Sleeps until either a client is waiting to be accepted or a session has ended
// and rung the doorbell. Both are registered for one handle and cancelled on
// resume; the loop is single threaded, so nothing fires in between.
struct ReapWait {
  int Listener;
  int Doorbell;
  std::coroutine_handle<> H{};

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> Handle) {
    H = Handle;
    Loop::get().wait(Listener, EPOLLIN, H);
    Loop::get().wait(Doorbell, EPOLLIN, H);
  }
  void await_resume() {
    if (H) Loop::get().cancel(H);
  }
};

} // namespace

Coro<Result<void>> serveFiles(const std::filesystem::path &Root, const ServiceOptions &Opts) {
  const size_t Threads = std::max<size_t>(1, Opts.Threads);
  const bool Sharing = Threads > 1;
  auto Listener = Stream::listenOn(Opts.Port, false, Sharing);
  if (!Listener) co_return std::unexpected(Listener.error());

  // Split, not repeated. The ceiling is what this machine can hold at once, so
  // giving every thread the whole of it would let the daemon accept as many
  // times over as it has threads - which is the memory bound this exists to
  // enforce. Only the first thread explains it; the rest would print the same
  // three lines again.
  const size_t Allowed = std::max<size_t>(1, sessionsAffordable(Opts, !Sharing) / Threads);

  // A session that ends rings this, so the accept loop reaps it at once instead
  // of holding its registered pages, queue pairs and descriptors until the next
  // client happens to connect.
  const int Doorbell = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (Doorbell < 0) co_return failErrno("eventfd for session reaping");

  std::vector<Coro<Result<void>>> Running;

  auto reap = [&Running] {
    for (size_t I = Running.size(); I-- > 0;)
      if (Running[I].done()) {
        if (auto R = Running[I].result(); !R) std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
        Running.erase(Running.begin() + static_cast<long>(I));
      }
  };

  // Bell is a parameter, not a capture: a coroutine lambda keeps its captures
  // in the closure object, not in the frame, so a capture would dangle if the
  // closure died before the session did.
  auto serve = [](std::unique_ptr<Service> Owner, Stream Client, int Bell) -> Coro<Result<void>> {
    auto Outcome = co_await Owner->serveAndClose(std::move(Client));
    const uint64_t One = 1;
    [[maybe_unused]] auto Wrote = ::write(Bell, &One, sizeof(One));
    co_return Outcome;
  };

  const auto stop = [Doorbell](Error Why) {
    Loop::get().forget(Doorbell);
    ::close(Doorbell);
    return std::unexpected(std::move(Why));
  };

  for (;;) {
    reap();

    auto Client = Listener->tryAccept();
    if (!Client) co_return stop(Client.error());

    if (Client->valid()) {
      if (Running.size() >= Allowed) {
        std::fprintf(stderr, "raild: refusing a client, %zu sessions already open\n", Running.size());
        continue;
      }
      auto Owner = std::make_unique<Service>(Root, Opts.FlipOneBit);
      Running.push_back(serve(std::move(Owner), std::move(*Client), Doorbell));
      Running.back().start();
      continue;
    }

    // Nothing waiting; sleep until a client arrives or a session ends, then
    // drain the doorbell so a stale ring cannot spin the loop.
    co_await ReapWait{Listener->fd(), Doorbell};
    uint64_t Ticks = 0;
    while (::read(Doorbell, &Ticks, sizeof(Ticks)) == static_cast<ssize_t>(sizeof(Ticks))) {}
  }
}

// One loop per thread is what makes this safe: Loop::get() and Uring::get() are
// both thread_local, a Service belongs to the thread that accepted it, and the
// only shared state left - the page allocator and the rdma device registry -
// already holds a mutex.
Result<void> serveFilesThreaded(const std::filesystem::path &Root, const ServiceOptions &Opts) {
  const size_t Wanted = std::max<size_t>(1, Opts.Threads);

  if (Wanted == 1) return run(serveFiles(Root, Opts));

  // Explained once here rather than once per thread, and with the split shown,
  // because "serving up to 288 clients" on eight threads means thirty-six each.
  const size_t Held = sessionsAffordable(Opts, true);
  std::fprintf(stderr, "raild: answering on %zu threads, %zu clients each\n", Wanted, std::max<size_t>(1, Held / Wanted));

  std::mutex Telling;
  Result<void> First{};
  std::vector<std::thread> Answering;

  for (size_t I = 0; I < Wanted; I++)
    Answering.emplace_back([&] {
      auto Outcome = runToResult(serveFiles(Root, Opts));
      if (Outcome) return;

      // Said now rather than at the join: the healthy threads serve forever,
      // so a thread that could not bind would otherwise leave the daemon
      // quietly running with fewer than it was asked for.
      std::fprintf(stderr, "raild: a serving thread stopped: %s\n", Outcome.error().message().c_str());

      const std::lock_guard<std::mutex> Held(Telling);
      if (First) First = std::unexpected(Outcome.error());
    });

  for (auto &One : Answering) One.join();
  return First;
}

} // namespace rail
