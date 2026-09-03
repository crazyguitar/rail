#include "rail/nfs/server.h"

#include "rail/vfs/remotes.h"

#include "rail/app/checksum.h"
#include "rail/io/runner.h"
#include "rail/io/stream.h"
#include "rail/nfs/rpc.h"
#include "rail/nfs/xdr.h"

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rail::nfs {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kErrNoEnt = 2;
constexpr uint32_t kErrIo = 5;
constexpr uint32_t kErrNotDir = 20;
constexpr uint32_t kErrInval = 22;
constexpr uint32_t kErrRoFs = 30;
constexpr uint32_t kErrNameTooLong = 63;
constexpr uint32_t kErrStale = 70;

constexpr uint32_t kAccessRead = 0x0001;
constexpr uint32_t kAccessLookup = 0x0002;
constexpr uint32_t kAccessModify = 0x0004;
constexpr uint32_t kAccessExtend = 0x0008;
constexpr uint32_t kAccessDelete = 0x0010;
constexpr uint32_t kAccessExecute = 0x0020;

constexpr uint32_t kTypeRegular = 1;
constexpr uint32_t kTypeDirectory = 2;

constexpr uint32_t kMountOk = 0;
constexpr uint32_t kMountNoEnt = 2;
constexpr uint32_t kMountIo = 5;

constexpr size_t kHandleSize = 64;
constexpr size_t kInlineLimit = kHandleSize - 8;
constexpr size_t kNameLimit = 255;
constexpr size_t kPathLimit = 4096;
constexpr size_t kReadLimit = 1u << 20;
constexpr size_t kWriteLimit = 1u << 20;
constexpr uint32_t kErrExist = 17;
// What a write promises. UNSTABLE means it is in the export's memory and the
// client must send a commit before it counts; FILE_SYNC would say it is already
// on the peer's disk, which a plain write does not make true.
constexpr uint32_t kUnstable = 0;
constexpr size_t kDirentSlack = 128;
constexpr uint64_t kFsId = 0x1b53ULL;

enum class Proc : uint32_t {
  Null = 0,
  GetAttr = 1,
  SetAttr = 2,
  Lookup = 3,
  Access = 4,
  ReadLink = 5,
  Read = 6,
  Write = 7,
  Create = 8,
  MakeDirectory = 9,
  Symlink = 10,
  MakeNode = 11,
  Remove = 12,
  RemoveDirectory = 13,
  Rename = 14,
  Link = 15,
  ReadDir = 16,
  ReadDirPlus = 17,
  FsStat = 18,
  FsInfo = 19,
  PathConf = 20,
  Commit = 21,
};

enum class MountProc : uint32_t {
  Null = 0,
  Mnt = 1,
  Dump = 2,
  Umnt = 3,
  UmntAll = 4,
  Export = 5,
};

Digest digestOf(const std::string &Path) {
  Hasher H;
  H.update({reinterpret_cast<const std::byte *>(Path.data()), Path.size()});
  return H.digest();
}

uint64_t fileIdOf(const std::string &Path) {
  const Digest D = digestOf(Path);
  uint64_t Id = 0;
  for (size_t I = 0; I < 8; I++) Id = (Id << 8) | static_cast<uint64_t>(D[I]);
  return Id ? Id : 1;
}

std::string joinPath(const std::string &Parent, const std::string &Name) { return Parent == "." ? Name : Parent + "/" + Name; }

bool insideExport(const std::string &Path, const std::string &Root) {
  if (Path.empty() || Path.front() == '/') return false;
  for (const auto &Part : std::filesystem::path(Path))
    if (Part == "..") return false;
  if (Root == ".") return true;
  return Path == Root || Path.starts_with(Root + "/");
}

std::string parentPath(const std::string &Path, const std::string &Root) {
  if (Path == Root) return Root;
  const size_t Cut = Path.rfind('/');
  const std::string Parent = Cut == std::string::npos ? std::string(".") : Path.substr(0, Cut);
  return insideExport(Parent, Root) ? Parent : Root;
}

bool namedSafely(const std::string &Name) { return !Name.empty() && Name != "." && Name != ".." && Name.find('/') == std::string::npos; }

class Handles {
public:
  std::vector<std::byte> encode(const std::string &Path) {
    const std::lock_guard<std::mutex> Held(Lock);
    std::vector<std::byte> H(kHandleSize, std::byte{0});
    if (Path.size() <= kInlineLimit) {
      H[0] = std::byte{1};
      H[1] = static_cast<std::byte>(Path.size());
      std::memcpy(H.data() + 8, Path.data(), Path.size());
      return H;
    }

    const Digest D = digestOf(Path);
    H[0] = std::byte{2};
    std::memcpy(H.data() + 8, D.data(), D.size());
    Long.insert_or_assign(std::string(reinterpret_cast<const char *>(D.data()), D.size()), Path);
    return H;
  }

  std::optional<std::string> decode(std::span<const std::byte> H) const {
    const std::lock_guard<std::mutex> Held(Lock);
    if (H.size() != kHandleSize) return std::nullopt;

    if (H[0] == std::byte{1}) {
      const size_t Length = static_cast<size_t>(H[1]);
      if (Length > kInlineLimit) return std::nullopt;
      return std::string(reinterpret_cast<const char *>(H.data() + 8), Length);
    }

    if (H[0] != std::byte{2}) return std::nullopt;
    auto It = Long.find(std::string(reinterpret_cast<const char *>(H.data() + 8), sizeof(Digest)));
    if (It == Long.end()) return std::nullopt;
    return It->second;
  }

private:
  mutable std::mutex Lock;
  std::unordered_map<std::string, std::string> Long;
};

Handles &sharedHandles() {
  static Handles Only;
  return Only;
}

void writeAttrs(XdrWriter &W, const proto::FileAttrs &A, const std::string &Path) {
  W.u32(A.Directory ? kTypeDirectory : kTypeRegular);
  W.u32(A.Mode);
  W.u32(A.Directory ? 2 : 1);
  W.u32(::getuid());
  W.u32(::getgid());
  W.u64(A.Size);
  W.u64((A.Size + 4095) & ~uint64_t{4095});
  W.u32(0);
  W.u32(0);
  W.u64(kFsId);
  W.u64(fileIdOf(Path));
  for (int I = 0; I < 3; I++) {
    W.u32(static_cast<uint32_t>(A.Mtime));
    W.u32(0);
  }
}

void writePostAttrs(XdrWriter &W, const proto::FileAttrs &A, const std::string &Path) {
  W.boolean(true);
  writeAttrs(W, A, Path);
}

void writeNoAttrs(XdrWriter &W) { W.boolean(false); }

// wcc_data: what the object looked like before and after. Both optional, and
// this export sends neither, so a client that caches attributes re-asks.
void writeNoWcc(XdrWriter &W) {
  W.boolean(false);
  W.boolean(false);
}

// sattr3. Only the size is acted on - the wire carries no ownership and the
// mode a client sets on create is not what the peer's umask would give it.
struct Wanted {
  bool HasSize = false;
  uint64_t Size = 0;
  bool HasMode = false;
  uint32_t Mode = 0;
};

Wanted readWanted(XdrReader &R) {
  Wanted Out;
  if (R.boolean()) {
    Out.HasMode = true;
    Out.Mode = R.u32();
  }
  if (R.boolean()) R.u32();
  if (R.boolean()) R.u32();
  if (R.boolean()) {
    Out.HasSize = true;
    Out.Size = R.u64();
  }
  for (int I = 0; I < 2; I++) {
    const uint32_t How = R.u32();
    if (How == 2) {
      R.u32();
      R.u32();
    }
  }
  return Out;
}

uint32_t failureFields(Proc Procedure) {
  switch (Procedure) {
  case Proc::Rename:
    return 4;
  case Proc::Link:
    return 3;
  default:
    return 2;
  }
}

class Session {
public:
  Session(const ExportOptions &Opts, Handles &Known) : Opts(Opts), Known(Known), Pool(Opts.Remote) {}

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  Coro<Result<void>> serveAndClose(Stream Conn) {
    auto Outcome = co_await serve(Conn);

    // Handlers hold the connection and the pool, so nothing may be destroyed
    // until every one of them has finished.
    for (auto &Task : Running)
      if (!Task.done()) [[maybe_unused]]
        auto Ended = co_await Task.join();
    Running.clear();

    Conn.close();
    co_await Pool.close();

    if (Served > 0)
      std::fprintf(stderr,
                   "railnfs: %llu bytes served, %llu bytes fetched, %.2f fetched per byte served\n",
                   static_cast<unsigned long long>(Served),
                   static_cast<unsigned long long>(Fetched),
                   static_cast<double>(Fetched) / static_cast<double>(Served));
    co_return Outcome;
  }

private:
  // One file's read window, and where that file was last read to: a detector
  // shared by the whole connection cannot tell two readers apart, since each
  // one's offsets break the other's run of sequential reads.
  struct Buffer {
    vfs::Window Have;
    size_t Sending = 0;
    std::deque<std::coroutine_handle<>> Idle;

    void sent() {
      if (--Sending > 0) return;
      auto Woken = std::move(Idle);
      Idle.clear();
      for (auto H : Woken)
        if (H && !H.done()) Loop::get().schedule(H);
    }

    void forget() {
      Have.Bytes = 0;
      Have.Path.clear();
    }
  };

  struct Drained {
    Buffer &B;
    std::coroutine_handle<> Queued{};

    bool await_ready() const noexcept { return B.Sending == 0; }
    void await_suspend(std::coroutine_handle<> H) {
      Queued = H;
      B.Idle.push_back(H);
    }
    void await_resume() noexcept { Queued = {}; }
    ~Drained() {
      if (Queued) std::erase(B.Idle, Queued);
    }
  };

  struct Cached {
    std::string Path;
    uint64_t End = 0;
    uint64_t Used = 0;
    size_t Users = 0;
    vfs::Gate Filling;
    std::unique_ptr<Buffer> Front = std::make_unique<Buffer>();
    std::unique_ptr<Buffer> Back = std::make_unique<Buffer>();

    vfs::Window &have() { return Front->Have; }

    void forget() {
      Front->forget();
      Back->forget();
    }
  };

  struct Hold {
    Cached &Entry;

    explicit Hold(Cached &Entry) : Entry(Entry) { Entry.Users++; }
    Hold(const Hold &) = delete;
    Hold &operator=(const Hold &) = delete;
    ~Hold() { Entry.Users--; }
  };

  uint64_t Served = 0;
  uint64_t Fetched = 0;

  Coro<Result<void>> serve(Stream &Conn) {
    for (;;) {
      auto C = co_await receiveCall(Conn);
      if (!C) co_return Result<void>{};

      for (size_t I = Running.size(); I-- > 0;)
        if (Running[I].done()) {
          if (auto R = Running[I].result(); !R) complain(R.error());
          Running.erase(Running.begin() + static_cast<long>(I));
        }

      while (Running.size() >= kMaxInFlight) {
        if (auto R = co_await Running.front().join(); !R) complain(R.error());
        Running.erase(Running.begin());
      }

      Running.push_back(dispatch(Conn, std::move(*C)));
      Running.back().start();
    }
  }

  Coro<Result<void>> dispatch(Stream &Conn, Call C) {
    if (C.Program == kMountProgram) co_return co_await onMount(Conn, C);
    if (C.Program == kNfsProgram) co_return co_await onNfs(Conn, C);
    co_return co_await sendGated(Conn, C.Xid, AcceptStatus::ProgramUnavailable, {});
  }

  Coro<Result<void>> sendGated(Stream &Conn, uint32_t Xid, AcceptStatus Status, const XdrPayload &Results) {
    co_await Writing.take();
    auto Sent = co_await sendReply(Conn, Xid, Status, Results);
    Writing.give();
    co_return Sent;
  }

  Coro<Result<void>> replyBroken(Stream &Conn, const Call &C, uint32_t Fields, const Error &Why) {
    complain(Why);
    co_return co_await replyStatus(Conn, C, kErrIo, Fields);
  }

  Coro<Result<void>> replyBroken(Stream &Conn, const Call &C, uint32_t Fields, const Error &Why, const vfs::Remotes::Lease &Held) {
    if (!Held.client().alive()) Held.discard();
    co_return co_await replyBroken(Conn, C, Fields, Why);
  }

  void complain(const Error &Why) {
    if (std::exchange(Complained, true)) return;
    std::fprintf(stderr, "railnfs: %s\n", Why.message().c_str());
  }

  Coro<Result<void>> reply(Stream &Conn, uint32_t Xid, const XdrWriter &W) {
    co_return co_await sendGated(Conn, Xid, AcceptStatus::Success, W.payload());
  }

  Coro<Result<void>> replyStatus(Stream &Conn, const Call &C, uint32_t Status, uint32_t Fields) {
    XdrWriter W;
    W.u32(Status);
    for (uint32_t I = 0; I < Fields; I++) W.boolean(false);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onMount(Stream &Conn, const Call &C) {
    if (C.Version != kProgramVersion) {
      XdrWriter W;
      W.u32(kProgramVersion);
      W.u32(kProgramVersion);
      co_return co_await sendGated(Conn, C.Xid, AcceptStatus::ProgramMismatch, W.payload());
    }

    XdrWriter W;
    switch (static_cast<MountProc>(C.Procedure)) {
    case MountProc::Null:
      break;
    case MountProc::Mnt: {
      XdrReader R(C.Args);
      const std::string Wanted = R.text(kPathLimit);
      if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});

      auto Held = co_await Pool.take();
      if (!Held) {
        complain(Held.error());
        W.u32(kMountIo);
        break;
      }

      auto Info = co_await Held->client().stat(Opts.Remote.Root);
      if (!Info) {
        complain(Info.error());
        if (!Held->client().alive()) Held->discard();
        W.u32(kMountIo);
        break;
      }
      if (!Info->Found || !Info->Attrs.Directory) {
        W.u32(kMountNoEnt);
        break;
      }

      std::fprintf(stderr, "railnfs: mounted %s as %s\n", Opts.Remote.Root.c_str(), Wanted.c_str());
      W.u32(kMountOk);
      W.opaque(Known.encode(Opts.Remote.Root));
      W.u32(1);
      W.u32(1);
      break;
    }
    case MountProc::Dump:
      W.boolean(false);
      break;
    case MountProc::Umnt:
    case MountProc::UmntAll:
      break;
    case MountProc::Export:
      W.boolean(true);
      W.text("/");
      W.boolean(false);
      W.boolean(false);
      break;
    default:
      co_return co_await sendGated(Conn, C.Xid, AcceptStatus::ProcedureUnavailable, {});
    }

    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onNfs(Stream &Conn, const Call &C) {
    if (!std::exchange(Warmed, true))
      if (auto R = co_await Pool.warm(); !R) complain(R.error());

    if (C.Version != kProgramVersion) {
      XdrWriter W;
      W.u32(kProgramVersion);
      W.u32(kProgramVersion);
      co_return co_await sendGated(Conn, C.Xid, AcceptStatus::ProgramMismatch, W.payload());
    }

    switch (static_cast<Proc>(C.Procedure)) {
    case Proc::Null:
      co_return co_await sendGated(Conn, C.Xid, AcceptStatus::Success, {});
    case Proc::GetAttr:
      co_return co_await onGetAttr(Conn, C);
    case Proc::Lookup:
      co_return co_await onLookup(Conn, C);
    case Proc::Access:
      co_return co_await onAccess(Conn, C);
    case Proc::ReadLink:
      co_return co_await onReadLink(Conn, C);
    case Proc::Read:
      co_return co_await onRead(Conn, C);
    case Proc::ReadDir:
      co_return co_await onReadDir(Conn, C, false);
    case Proc::ReadDirPlus:
      co_return co_await onReadDir(Conn, C, true);
    case Proc::FsStat:
      co_return co_await onFsStat(Conn, C);
    case Proc::FsInfo:
      co_return co_await onFsInfo(Conn, C);
    case Proc::PathConf:
      co_return co_await onPathConf(Conn, C);
    case Proc::SetAttr:
      co_return co_await onSetAttr(Conn, C);
    case Proc::Write:
      co_return co_await onWrite(Conn, C);
    case Proc::Create:
      co_return co_await onCreate(Conn, C);
    case Proc::Commit:
      co_return co_await onCommit(Conn, C);
    case Proc::Remove:
      co_return co_await onRemove(Conn, C);
    case Proc::MakeDirectory:
      co_return co_await onMakeDirectory(Conn, C);
    case Proc::RemoveDirectory:
      co_return co_await onRemoveDirectory(Conn, C);
    case Proc::Symlink:
      co_return co_await onSymlink(Conn, C);
    case Proc::Rename:
      co_return co_await onRename(Conn, C);
    case Proc::Link:
      co_return co_await onLink(Conn, C);
    // A device node is not something the export can carry.
    case Proc::MakeNode:
      co_return co_await replyStatus(Conn, C, kErrRoFs, failureFields(static_cast<Proc>(C.Procedure)));
    default:
      co_return co_await sendGated(Conn, C.Xid, AcceptStatus::ProcedureUnavailable, {});
    }
  }

  std::optional<std::string> pathOf(XdrReader &R) {
    auto Raw = R.opaque(kHandleSize);
    if (!R.ok()) return std::nullopt;

    auto Path = Known.decode(Raw);
    if (Path && !insideExport(*Path, Opts.Remote.Root)) return std::nullopt;
    return Path;
  }

  Coro<Result<void>> onGetAttr(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 0);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 0, Held.error());

    auto Info = co_await Held->client().stat(*Path);
    if (!Info) co_return co_await replyBroken(Conn, C, 0, Info.error(), *Held);
    if (!Info->Found) co_return co_await replyStatus(Conn, C, kErrNoEnt, 0);

    XdrWriter W;
    W.u32(kOk);
    writeAttrs(W, Info->Attrs, *Path);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onLookup(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, 1);
    if (Name.size() > kNameLimit) co_return co_await replyStatus(Conn, C, kErrNameTooLong, 1);

    std::string Target;
    if (Name == ".") Target = *Directory;
    else if (Name == "..") Target = parentPath(*Directory, Opts.Remote.Root);
    else if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrNoEnt, 1);
    else Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

    auto Info = co_await Held->client().stat(Target);
    if (!Info) co_return co_await replyBroken(Conn, C, 1, Info.error(), *Held);
    if (!Info->Found) co_return co_await replyStatus(Conn, C, kErrNoEnt, 1);

    XdrWriter W;
    W.u32(kOk);
    W.opaque(Known.encode(Target));
    writePostAttrs(W, Info->Attrs, Target);
    writeNoAttrs(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onAccess(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    const uint32_t Wanted = R.u32();
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

    auto Info = co_await Held->client().stat(*Path);
    if (!Info) co_return co_await replyBroken(Conn, C, 1, Info.error(), *Held);
    if (!Info->Found) co_return co_await replyStatus(Conn, C, kErrNoEnt, 1);

    const uint32_t OnDirectory = kAccessRead | kAccessLookup | kAccessModify | kAccessExtend | kAccessDelete;
    const uint32_t OnFile = kAccessRead | kAccessModify | kAccessExtend | kAccessExecute;
    const uint32_t Allowed = Info->Attrs.Directory ? OnDirectory : OnFile;

    XdrWriter W;
    W.u32(kOk);
    writePostAttrs(W, Info->Attrs, *Path);
    W.u32(Wanted & Allowed);
    co_return co_await reply(Conn, C.Xid, W);
  }

  // where + attributes, answered with a handle for what was made. The mode is
  // taken; ownership is not, because the wire does not carry it.
  Coro<Result<void>> onMakeDirectory(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    const Wanted Ask = readWanted(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::MakeDirectory));
    if (Name.size() > kNameLimit) co_return co_await replyStatus(Conn, C, kErrNameTooLong, failureFields(Proc::MakeDirectory));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::MakeDirectory));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::MakeDirectory), Held.error());

    auto Seen = co_await Held->client().stat(Target);
    if (!Seen) co_return co_await replyBroken(Conn, C, failureFields(Proc::MakeDirectory), Seen.error(), *Held);
    if (Seen->Found) co_return co_await replyStatus(Conn, C, kErrExist, failureFields(Proc::MakeDirectory));

    auto Made = co_await Held->client().makeDirectory(Target, Ask.HasMode ? Ask.Mode : 0755);
    if (!Made) co_return co_await replyBroken(Conn, C, failureFields(Proc::MakeDirectory), Made.error(), *Held);

    forget(Target);

    auto Info = co_await Held->client().stat(Target);
    if (!Info) co_return co_await replyBroken(Conn, C, failureFields(Proc::MakeDirectory), Info.error(), *Held);

    XdrWriter W;
    W.u32(kOk);
    W.boolean(true);
    W.opaque(Known.encode(Target));
    writePostAttrs(W, Info->Attrs, Target);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onRemoveDirectory(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::RemoveDirectory));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrNoEnt, failureFields(Proc::RemoveDirectory));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::RemoveDirectory), Held.error());

    auto Gone = co_await Held->client().removeDirectory(Target);
    if (!Gone) co_return co_await replyBroken(Conn, C, failureFields(Proc::RemoveDirectory), Gone.error(), *Held);

    forget(Target);

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onRename(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto FromDir = pathOf(R);
    const std::string FromName = R.text(kPathLimit);
    auto ToDir = pathOf(R);
    const std::string ToName = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!FromDir || !ToDir) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Rename));
    if (!namedSafely(FromName) || !namedSafely(ToName)) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::Rename));

    const std::string From = joinPath(*FromDir, FromName);
    const std::string To = joinPath(*ToDir, ToName);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Rename), Held.error());

    auto Moved = co_await Held->client().rename(From, To);
    if (!Moved) co_return co_await replyBroken(Conn, C, failureFields(Proc::Rename), Moved.error(), *Held);

    forget(From);
    forget(To);

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onSymlink(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    readWanted(R);
    const std::string Points = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Symlink));
    if (Name.size() > kNameLimit) co_return co_await replyStatus(Conn, C, kErrNameTooLong, failureFields(Proc::Symlink));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::Symlink));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Symlink), Held.error());

    auto Made = co_await Held->client().makeLink(Target, Points);
    if (!Made) co_return co_await replyBroken(Conn, C, failureFields(Proc::Symlink), Made.error(), *Held);

    forget(Target);

    auto Info = co_await Held->client().stat(Target);
    if (!Info) co_return co_await replyBroken(Conn, C, failureFields(Proc::Symlink), Info.error(), *Held);

    XdrWriter W;
    W.u32(kOk);
    W.boolean(true);
    W.opaque(Known.encode(Target));
    writePostAttrs(W, Info->Attrs, Target);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  // The argument order is the opposite of what the name suggests: the existing
  // file first, then the directory and name the new link takes.
  Coro<Result<void>> onLink(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Existing = pathOf(R);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Existing || !Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Link));
    if (Name.size() > kNameLimit) co_return co_await replyStatus(Conn, C, kErrNameTooLong, failureFields(Proc::Link));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::Link));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Link), Held.error());

    auto Made = co_await Held->client().hardLink(*Existing, Target);
    if (!Made) co_return co_await replyBroken(Conn, C, failureFields(Proc::Link), Made.error(), *Held);

    forget(Target);

    auto Info = co_await Held->client().stat(*Existing);
    if (!Info) co_return co_await replyBroken(Conn, C, failureFields(Proc::Link), Info.error(), *Held);

    XdrWriter W;
    W.u32(kOk);
    writePostAttrs(W, Info->Attrs, *Existing);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onReadLink(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::ReadLink));

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::ReadLink), Held.error());

    auto Points = co_await Held->client().readLink(*Path);
    if (!Points) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::ReadLink));

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.text(*Points);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onWrite(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    const uint64_t Offset = R.u64();
    const uint32_t Count = R.u32();
    R.u32();
    auto Data = R.opaque(kWriteLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Write));
    if (Data.size() != Count) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::Write));

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Write), Held.error());

    auto Put = co_await Held->client().write(*Path, Offset, Data);
    if (!Put) co_return co_await replyBroken(Conn, C, failureFields(Proc::Write), Put.error(), *Held);

    forget(*Path);

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    W.u32(Count);
    W.u32(kUnstable);
    W.fixed(Verifier);
    co_return co_await reply(Conn, C.Xid, W);
  }

  // Writes are answered UNSTABLE, so this is where they reach the disk.
  Coro<Result<void>> onCommit(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    R.u64();
    R.u32();
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Commit));

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Commit), Held.error());

    auto Synced = co_await Held->client().fsync(*Path);
    if (!Synced) co_return co_await replyBroken(Conn, C, failureFields(Proc::Commit), Synced.error(), *Held);

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    W.fixed(Verifier);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onCreate(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    const uint32_t How = R.u32();
    Wanted Ask;
    if (How == 2) R.fixed(8);
    else Ask = readWanted(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Create));
    if (Name.size() > kNameLimit) co_return co_await replyStatus(Conn, C, kErrNameTooLong, failureFields(Proc::Create));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrInval, failureFields(Proc::Create));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Create), Held.error());

    // GUARDED refuses a name that is already there; the other two modes take
    // it over, which is what O_CREAT without O_EXCL asks for.
    auto Seen = co_await Held->client().stat(Target);
    if (!Seen) co_return co_await replyBroken(Conn, C, failureFields(Proc::Create), Seen.error(), *Held);
    if (Seen->Found && How == 1) co_return co_await replyStatus(Conn, C, kErrExist, failureFields(Proc::Create));

    auto Made = co_await Held->client().write(Target, 0, {}, true);
    if (!Made) co_return co_await replyBroken(Conn, C, failureFields(Proc::Create), Made.error(), *Held);

    if (Ask.HasMode) {
      auto Set = co_await Held->client().setMode(Target, Ask.Mode);
      if (!Set) co_return co_await replyBroken(Conn, C, failureFields(Proc::Create), Set.error(), *Held);
    }

    forget(Target);

    auto Info = co_await Held->client().stat(Target);
    if (!Info) co_return co_await replyBroken(Conn, C, failureFields(Proc::Create), Info.error(), *Held);

    XdrWriter W;
    W.u32(kOk);
    W.boolean(true);
    W.opaque(Known.encode(Target));
    writePostAttrs(W, Info->Attrs, Target);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onSetAttr(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    const Wanted Ask = readWanted(R);
    if (R.boolean()) {
      R.u32();
      R.u32();
    }
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::SetAttr));

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::SetAttr), Held.error());

    if (Ask.HasSize) {
      auto Cut = co_await Held->client().truncate(*Path, Ask.Size);
      if (!Cut) co_return co_await replyBroken(Conn, C, failureFields(Proc::SetAttr), Cut.error(), *Held);
      forget(*Path);
    }
    if (Ask.HasMode) {
      auto Set = co_await Held->client().setMode(*Path, Ask.Mode);
      if (!Set) co_return co_await replyBroken(Conn, C, failureFields(Proc::SetAttr), Set.error(), *Held);
    }

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onRemove(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Directory = pathOf(R);
    const std::string Name = R.text(kPathLimit);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Directory) co_return co_await replyStatus(Conn, C, kErrStale, failureFields(Proc::Remove));
    if (!namedSafely(Name)) co_return co_await replyStatus(Conn, C, kErrNoEnt, failureFields(Proc::Remove));

    const std::string Target = joinPath(*Directory, Name);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, failureFields(Proc::Remove), Held.error());

    auto Gone = co_await Held->client().removeFile(Target);
    if (!Gone) co_return co_await replyBroken(Conn, C, failureFields(Proc::Remove), Gone.error(), *Held);

    forget(Target);

    XdrWriter W;
    W.u32(kOk);
    writeNoWcc(W);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onRead(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    const uint64_t Offset = R.u64();
    const uint32_t Count = R.u32();
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    Cached *Seen = windowFor(*Path);
    const bool Forward = Seen && Offset == Seen->End;
    if (Seen) Seen->End = Offset + Count;
    if (Forward && Opts.Remote.Readahead > Count) co_return co_await onWindowedRead(Conn, C, *Seen, *Path, Offset, Count);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

    const size_t Want = std::min<size_t>(Count, readCeiling(Held->client()));
    if (Want == 0) {
      XdrWriter W;
      W.u32(kOk);
      writeNoAttrs(W);
      W.u32(0);
      W.boolean(false);
      W.opaque({});
      co_return co_await reply(Conn, C.Xid, W);
    }

    auto &Scratch = Held->scratch();
    Page *Landing = Scratch.page(Want);
    if (!Landing && Scratch.Data.size() < Want) Scratch.Data.resize(Want);
    const std::span<std::byte> Data(Landing ? Landing->bytes() : Scratch.Data.data(), Want);

    auto Got = Landing ? co_await Held->client().read(*Path, Offset, *Landing) : co_await Held->client().read(*Path, Offset, Data);
    if (!Got) co_return co_await replyBroken(Conn, C, 1, Got.error(), *Held);

    Fetched += Got->Bytes;
    Served += Got->Bytes;

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.u32(static_cast<uint32_t>(Got->Bytes));
    W.boolean(Offset + Got->Bytes >= Got->FileSize);
    W.opaqueTail(Data.first(Got->Bytes));
    co_return co_await reply(Conn, C.Xid, W);
  }

  // A reader walking forward is served out of one large fetch. The window
  // belongs to the file rather than to a session, so any free session can fill
  // it: readers of different files never evict each other, however few sessions
  // the export was given, and one file's reads still spread over all of them.
  Coro<Result<void>> onWindowedRead(Stream &Conn, const Call &C, Cached &Entry, const std::string &Path, uint64_t Offset, uint32_t Count) {
    const Hold Busy(Entry);

    if (!Entry.have().covers(Path, Offset, Count)) {
      co_await Entry.Filling.take();

      Result<void> Filled{};
      if (!Entry.have().covers(Path, Offset, Count)) Filled = co_await fillWindow(Entry, Path, Offset);

      Entry.Filling.give();
      if (!Filled) co_return co_await replyBroken(Conn, C, 1, Filled.error());
    }

    Buffer &From = *Entry.Front;
    const uint64_t Into = Offset - From.Have.Start;
    const size_t Mine = Into < From.Have.Bytes ? std::min<size_t>(Count, From.Have.Bytes - Into) : 0;
    Served += Mine;

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.u32(static_cast<uint32_t>(Mine));
    W.boolean(Offset + Mine >= From.Have.FileSize);
    const std::byte *Bytes = From.Have.bytes().data();
    W.opaqueTail({Mine > 0 ? Bytes + Into : Bytes, Mine});
    From.Sending++;
    auto Sent = co_await reply(Conn, C.Xid, W);
    From.sent();
    co_return Sent;
  }

  // Aligned to what one call can actually carry rather than to the readahead
  // it was asked for: a window wider than a page would leave its own tail
  // uncovered, and every read landing there would be answered with nothing.
  Coro<Result<void>> fillWindow(Cached &Entry, const std::string &Path, uint64_t Offset) {
    auto Held = co_await Pool.take();
    if (!Held) co_return std::unexpected(Held.error());

    const uint64_t Room = std::min<uint64_t>(Opts.Remote.Readahead, Held->client().maxTransfer());
    const uint64_t Start = Room > 0 ? (Offset / Room) * Room : Offset;

    Buffer &Into = *Entry.Back;
    while (Into.Sending > 0) co_await Drained{Into};
    Into.forget();

    Page *Landing = Into.Have.page(Room);
    auto Got = Landing ? co_await Held->client().read(Path, Start, *Landing) : co_await Held->client().read(Path, Start, Into.Have.room(Room));
    if (!Got) {
      if (!Held->client().alive()) Held->discard();
      co_return std::unexpected(Got.error());
    }

    Fetched += Got->Bytes;
    Into.Have.Path = Path;
    Into.Have.Start = Start;
    Into.Have.FileSize = Got->FileSize;
    Into.Have.Bytes = Got->Bytes;
    std::swap(Entry.Front, Entry.Back);
    co_return Result<void>{};
  }

  Coro<Result<void>> onReadDir(Stream &Conn, const Call &C, bool WithAttrs) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    const uint64_t Cookie = R.u64();
    R.fixed(8);
    const uint32_t First = R.u32();
    const uint32_t Second = WithAttrs ? R.u32() : First;
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    if (Cookie == 0 || Listing.Path != *Path) {
      auto Held = co_await Pool.take();
      if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

      auto Listed = co_await Held->client().list(*Path);
      if (!Listed) co_return co_await replyBroken(Conn, C, 1, Listed.error(), *Held);
      if (!Listed->Found) co_return co_await replyStatus(Conn, C, kErrNotDir, 1);

      Listing.Path = *Path;
      Listing.Entries = std::move(Listed->Entries);
      std::ranges::sort(Listing.Entries, [](const proto::ListEntry &A, const proto::ListEntry &B) { return A.Name < B.Name; });
    }
    const std::vector<proto::ListEntry> &Entries = Listing.Entries;

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    for (int I = 0; I < 2; I++) W.u32(0);

    const size_t Ceiling = Second > kDirentSlack ? Second - kDirentSlack : Second;
    const size_t Total = Entries.size() + 2;
    const size_t Start = std::min<size_t>(Cookie, Total);
    size_t Index = Start;
    bool Full = false;

    for (; Index < Total && !Full; Index++) {
      const bool Dot = Index < 2;
      const std::string Name = Index == 0 ? "." : Index == 1 ? ".." : Entries[Index - 2].Name;
      const std::string Target = Index == 0 ? *Path : Index == 1 ? parentPath(*Path, Opts.Remote.Root) : joinPath(*Path, Name);

      const size_t Mark = W.size();
      W.boolean(true);
      W.u64(fileIdOf(Target));
      W.text(Name);
      W.u64(Index + 1);
      if (WithAttrs) {
        if (Dot) writeNoAttrs(W);
        else writePostAttrs(W, Entries[Index - 2].Attrs, Target);
        W.boolean(true);
        W.opaque(Known.encode(Target));
      }

      if (W.size() > Ceiling && Index > Start) {
        W.truncate(Mark);
        Full = true;
        Index--;
      }
    }

    W.boolean(false);
    W.boolean(Index >= Total);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onFsStat(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

    auto Info = co_await Held->client().statFs(*Path);
    if (!Info) co_return co_await replyBroken(Conn, C, 1, Info.error(), *Held);
    if (!Info->Ok) co_return co_await replyStatus(Conn, C, kErrIo, 1);

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.u64(Info->Blocks * Info->BlockSize);
    W.u64(Info->BlocksFree * Info->BlockSize);
    W.u64(Info->BlocksFree * Info->BlockSize);
    W.u64(Info->Files);
    W.u64(Info->FilesFree);
    W.u64(Info->FilesFree);
    W.u32(0);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onFsInfo(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    auto Held = co_await Pool.take();
    if (!Held) co_return co_await replyBroken(Conn, C, 1, Held.error());

    const uint32_t Ceiling = static_cast<uint32_t>(readCeiling(Held->client()));

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.u32(Ceiling);
    W.u32(Ceiling);
    W.u32(4096);
    W.u32(Ceiling);
    W.u32(Ceiling);
    W.u32(4096);
    W.u32(64u << 10);
    W.u64(UINT64_MAX >> 1);
    W.u32(1);
    W.u32(0);
    W.u32(0x8);
    co_return co_await reply(Conn, C.Xid, W);
  }

  Coro<Result<void>> onPathConf(Stream &Conn, const Call &C) {
    XdrReader R(C.Args);
    auto Path = pathOf(R);
    if (!R.ok()) co_return co_await sendGated(Conn, C.Xid, AcceptStatus::GarbageArguments, {});
    if (!Path) co_return co_await replyStatus(Conn, C, kErrStale, 1);

    XdrWriter W;
    W.u32(kOk);
    writeNoAttrs(W);
    W.u32(1);
    W.u32(kNameLimit);
    W.boolean(true);
    W.boolean(true);
    W.boolean(false);
    W.boolean(true);
    co_return co_await reply(Conn, C.Xid, W);
  }

  static size_t readCeiling(const FileClient &Client) { return std::min<size_t>(kReadLimit, Client.maxTransfer()); }

  // A written file's cached window is stale, and serving a read out of it
  // would hand back what the file said before the write.
  void forget(const std::string &Path) {
    for (auto &W : Windows) {
      if (W.Path != Path) continue;
      W.End = 0;
      W.forget();
    }
  }

  // The window a file is being read through, or nothing when every one of them
  // is in use - in which case the read falls back to a plain fetch rather than
  // taking a window out from under the reader who has it.
  Cached *windowFor(const std::string &Path) {
    Cached *Cold = nullptr;
    for (auto &W : Windows) {
      if (W.Path == Path) {
        W.Used = ++Ticks;
        return &W;
      }
      if (W.Users > 0) continue;
      if (!Cold || W.Used < Cold->Used) Cold = &W;
    }

    if (Cold) {
      Cold->Path = Path;
      Cold->End = 0;
      Cold->Used = ++Ticks;
      Cold->forget();
    }
    return Cold;
  }

  static constexpr size_t kMaxInFlight = 64;
  static constexpr size_t kWindows = 8;

  struct SortedListing {
    std::string Path;
    std::vector<proto::ListEntry> Entries;
  };
  SortedListing Listing;

  bool Warmed = false;

  const ExportOptions &Opts;
  Handles &Known;
  // Constant for the life of this server: it never loses an uncommitted write,
  // because every write is answered as already synced.
  std::array<std::byte, 8> Verifier{};
  vfs::Remotes Pool;
  std::vector<Cached> Windows = std::vector<Cached>(std::max<size_t>(Pool.count(), kWindows));
  uint64_t Ticks = 0;
  vfs::Gate Writing;
  std::vector<Coro<Result<void>>> Running;
  bool Complained = false;
};

} // namespace

Coro<Result<void>> serveExport(const ExportOptions &Opts) {
  auto Listener = Stream::listenOn(Opts.Port, true, Opts.Threads > 1);
  if (!Listener) co_return std::unexpected(Listener.error());

  constexpr size_t kMaxConnections = 8;

  Handles &Known = sharedHandles();
  struct Live {
    std::unique_ptr<Session> Owner;
    Coro<Result<void>> Task;
  };
  std::vector<Live> Running;

  for (;;) {
    for (size_t I = Running.size(); I-- > 0;)
      if (Running[I].Task.done()) {
        if (auto R = Running[I].Task.result(); !R) std::fprintf(stderr, "railnfs: %s\n", R.error().message().c_str());
        Running.erase(Running.begin() + static_cast<long>(I));
      }

    auto Conn = co_await Listener->accept();
    if (!Conn) co_return std::unexpected(Conn.error());

    if (Running.size() >= kMaxConnections) {
      std::fprintf(stderr, "railnfs: refusing a client, %zu connections already open\n", Running.size());
      continue;
    }

    Live Slot;
    Slot.Owner = std::make_unique<Session>(Opts, Known);
    Slot.Task = Slot.Owner->serveAndClose(std::move(*Conn));
    Slot.Task.start();
    Running.push_back(std::move(Slot));
  }
}

// One loop per thread: Loop::get() is thread_local and a Session belongs to
// the thread that accepted it, so the threads share nothing but the listening
// port and the handle table.
Result<void> serveExportThreaded(const ExportOptions &Opts) {
  const size_t Wanted = std::max<size_t>(1, Opts.Threads);

  if (Wanted == 1) {
    return run(serveExport(Opts));
  }

  std::mutex Telling;
  Result<void> First{};
  std::vector<std::thread> Answering;

  for (size_t I = 0; I < Wanted; I++) {
    Answering.emplace_back([&] {
      auto Outcome = run(serveExport(Opts));
      if (Outcome) {
        return;
      }

      std::fprintf(stderr, "mount.railnfs: a serving thread stopped: %s\n", Outcome.error().message().c_str());

      const std::lock_guard<std::mutex> Held(Telling);
      if (First) {
        First = std::unexpected(Outcome.error());
      }
    });
  }

  for (auto &One : Answering) {
    One.join();
  }
  return First;
}

} // namespace rail::nfs
