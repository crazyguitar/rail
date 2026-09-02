#include "rail/nfs/client.h"

#include "rail/nfs/rpc.h"
#include "rail/nfs/xdr.h"

#include <cstring>
#include <format>

namespace rail::nfs {

namespace {

constexpr uint32_t kMountMnt = 1;
constexpr uint32_t kGetAttr = 1;
constexpr uint32_t kLookup = 3;
constexpr uint32_t kRead = 6;
constexpr uint32_t kReadDirPlus = 17;
constexpr size_t kHandleLimit = 64;
constexpr size_t kNameLimit = 255;

Attrs readAttrs(XdrReader &R) {
  Attrs A;
  A.Directory = R.u32() == 2;
  for (int I = 0; I < 4; I++) R.u32();
  A.Size = R.u64();
  R.u64();
  R.u32();
  R.u32();
  R.u64();
  A.FileId = R.u64();
  for (int I = 0; I < 6; I++) R.u32();
  return A;
}

Result<void> statusOf(XdrReader &R, const char *What) {
  const uint32_t Status = R.u32();
  if (Status != 0) return failMessage(std::format("{} failed with NFS3ERR {}", What, Status));
  return {};
}

} // namespace

Result<NfsClient> NfsClient::connect(const std::string &Host, uint16_t Port) {
  auto S = Stream::connect(Host, Port);
  if (!S) return std::unexpected(S.error());
  return NfsClient(std::move(*S));
}

Coro<Result<std::vector<std::byte>>> NfsClient::callNfs(uint32_t Procedure, std::span<const std::byte> Args) {
  const uint32_t Mine = ++Xid;
  if (auto R = co_await sendCall(S, Mine, kNfsProgram, Procedure, Args); !R) co_return std::unexpected(R.error());

  auto Reply = co_await receiveReply(S);
  if (!Reply) co_return std::unexpected(Reply.error());
  if (Reply->Xid != Mine) co_return failMessage("reply for another call");
  if (Reply->Status != AcceptStatus::Success)
    co_return failMessage(std::format("the server rejected the call: {}", static_cast<uint32_t>(Reply->Status)));
  co_return std::move(Reply->Results);
}

Coro<Result<Handle>> NfsClient::mountRoot(const std::string &Path) {
  XdrWriter Args;
  Args.text(Path);

  const uint32_t Mine = ++Xid;
  if (auto R = co_await sendCall(S, Mine, kMountProgram, kMountMnt, Args.bytes()); !R) co_return std::unexpected(R.error());

  auto Reply = co_await receiveReply(S);
  if (!Reply) co_return std::unexpected(Reply.error());

  XdrReader R(Reply->Results);
  if (auto Ok = statusOf(R, "mount"); !Ok) co_return std::unexpected(Ok.error());

  auto Root = R.opaque(kHandleLimit);
  if (!R.ok()) co_return failMessage("malformed mount reply");
  co_return Handle(Root.begin(), Root.end());
}

Coro<Result<Attrs>> NfsClient::getAttr(const Handle &File) {
  XdrWriter Args;
  Args.opaque(File);

  auto Body = co_await callNfs(kGetAttr, Args.bytes());
  if (!Body) co_return std::unexpected(Body.error());

  XdrReader R(*Body);
  if (auto Ok = statusOf(R, "getattr"); !Ok) co_return std::unexpected(Ok.error());

  Attrs A = readAttrs(R);
  if (!R.ok()) co_return failMessage("malformed getattr reply");
  co_return A;
}

Coro<Result<Handle>> NfsClient::lookup(const Handle &Directory, const std::string &Name) {
  XdrWriter Args;
  Args.opaque(Directory);
  Args.text(Name);

  auto Body = co_await callNfs(kLookup, Args.bytes());
  if (!Body) co_return std::unexpected(Body.error());

  XdrReader R(*Body);
  if (auto Ok = statusOf(R, "lookup"); !Ok) co_return std::unexpected(Ok.error());

  auto Found = R.opaque(kHandleLimit);
  if (!R.ok()) co_return failMessage("malformed lookup reply");
  co_return Handle(Found.begin(), Found.end());
}

Coro<Result<std::vector<DirEntry>>> NfsClient::listDirectory(const Handle &Directory) {
  std::vector<DirEntry> Entries;
  uint64_t Cookie = 0;

  for (;;) {
    XdrWriter Args;
    Args.opaque(Directory);
    Args.u64(Cookie);
    for (int I = 0; I < 2; I++) Args.u32(0);
    Args.u32(32u << 10);
    Args.u32(128u << 10);

    auto Body = co_await callNfs(kReadDirPlus, Args.bytes());
    if (!Body) co_return std::unexpected(Body.error());

    XdrReader R(*Body);
    if (auto Ok = statusOf(R, "readdirplus"); !Ok) co_return std::unexpected(Ok.error());
    if (R.boolean()) readAttrs(R);
    R.fixed(8);

    while (R.boolean()) {
      DirEntry E;
      E.Info.FileId = R.u64();
      E.Name = R.text(kNameLimit);
      Cookie = R.u64();
      if (R.boolean()) E.Info = readAttrs(R);
      if (R.boolean()) {
        auto File = R.opaque(kHandleLimit);
        E.File.assign(File.begin(), File.end());
      }
      if (!R.ok()) co_return failMessage("malformed readdirplus entry");
      if (E.Name != "." && E.Name != "..") Entries.push_back(std::move(E));
    }

    const bool Eof = R.boolean();
    if (!R.ok()) co_return failMessage("malformed readdirplus reply");
    if (Eof) co_return Entries;
  }
}

Coro<Result<void>> NfsClient::submitRead(const Handle &File, uint64_t Offset, std::span<std::byte> Into) {
  XdrWriter Args;
  Args.opaque(File);
  Args.u64(Offset);
  Args.u32(static_cast<uint32_t>(Into.size()));

  const uint32_t Mine = ++Xid;
  if (auto R = co_await sendCall(S, Mine, kNfsProgram, kRead, Args.bytes()); !R) co_return std::unexpected(R.error());

  Pending.emplace(Mine, Asked{Offset, Into});
  co_return Result<void>{};
}

Coro<Result<Chunk>> NfsClient::collectRead() {
  if (Pending.empty()) co_return failMessage("no read in flight");

  auto Reply = co_await receiveReply(S);
  if (!Reply) co_return std::unexpected(Reply.error());

  auto It = Pending.find(Reply->Xid);
  if (It == Pending.end()) co_return failMessage("a reply for a read nobody sent");
  const Asked Mine = It->second;
  Pending.erase(It);

  XdrReader R(Reply->Results);
  if (auto Ok = statusOf(R, "read"); !Ok) co_return std::unexpected(Ok.error());
  if (R.boolean()) readAttrs(R);

  Chunk Got;
  Got.Offset = Mine.Offset;
  Got.Into = Mine.Into;
  Got.Bytes = R.u32();
  Got.Eof = R.boolean();

  auto Data = R.opaque(Mine.Into.size());
  if (!R.ok()) co_return failMessage("malformed read reply");
  if (Data.size() != Got.Bytes) co_return failMessage("the read reply disagrees with itself about its length");
  std::memcpy(Mine.Into.data(), Data.data(), Data.size());
  co_return Got;
}

Coro<Result<Chunk>> NfsClient::read(const Handle &File, uint64_t Offset, std::span<std::byte> Into) {
  if (auto R = co_await submitRead(File, Offset, Into); !R) co_return std::unexpected(R.error());
  co_return co_await collectRead();
}

} // namespace rail::nfs
