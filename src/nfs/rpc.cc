#include "rail/nfs/rpc.h"
#include "rail/nfs/xdr.h"

#include <algorithm>

namespace rail::nfs {

namespace {

constexpr uint32_t kCall = 0;
constexpr uint32_t kReply = 1;
constexpr uint32_t kRpcVersion = 2;
constexpr uint32_t kMessageAccepted = 0;
constexpr uint32_t kAuthNone = 0;
constexpr uint32_t kLastFragment = 0x80000000;

Coro<Result<std::vector<std::byte>>> receiveRecord(Stream &S) {
  std::vector<std::byte> Record;
  for (;;) {
    std::byte Marker[4];
    if (auto R = co_await S.readExact(Marker); !R) co_return std::unexpected(R.error());

    uint32_t Header = 0;
    for (std::byte B : Marker) Header = (Header << 8) | static_cast<uint32_t>(B);

    const bool Last = (Header & kLastFragment) != 0;
    const size_t Length = Header & ~kLastFragment;
    if (Record.size() + Length > kMaxRecord) co_return failMessage("rpc record too large");

    const size_t At = Record.size();
    Record.resize(At + Length);
    if (Length > 0) {
      if (auto R = co_await S.readExact(std::span(Record).subspan(At, Length)); !R) co_return std::unexpected(R.error());
    }
    if (Last) co_return Record;
  }
}

Coro<Result<void>> sendRecord(Stream &S, std::span<const std::byte> Header, const XdrPayload &Body) {
  XdrWriter Out;
  Out.u32(static_cast<uint32_t>(Header.size() + Body.size()) | kLastFragment);
  Out.fixed(Header);

  if (auto R = co_await S.writeAll(Out.bytes()); !R) co_return std::unexpected(R.error());
  if (!Body.Body.empty())
    if (auto R = co_await S.writeAll(Body.Body); !R) co_return std::unexpected(R.error());
  if (!Body.Tail.empty())
    if (auto R = co_await S.writeAll(Body.Tail); !R) co_return std::unexpected(R.error());
  if (Body.Pad == 0) co_return Result<void>{};

  static constexpr std::byte Zero[XdrPayload::kMaxPad]{};
  co_return co_await S.writeAll(std::span<const std::byte>(Zero, std::min(Body.Pad, XdrPayload::kMaxPad)));
}

} // namespace

Coro<Result<Call>> receiveCall(Stream &S) {
  auto Record = co_await receiveRecord(S);
  if (!Record) co_return std::unexpected(Record.error());

  XdrReader R(*Record);
  Call C;
  C.Xid = R.u32();
  if (R.u32() != kCall) co_return failMessage("not an rpc call");
  if (R.u32() != kRpcVersion) co_return failMessage("unsupported rpc version");
  C.Program = R.u32();
  C.Version = R.u32();
  C.Procedure = R.u32();

  R.u32();
  R.opaque(400);
  R.u32();
  R.opaque(400);
  if (!R.ok()) co_return failMessage("malformed rpc call header");

  C.Args.assign(Record->end() - static_cast<long>(R.left()), Record->end());
  co_return C;
}

Coro<Result<void>> sendReply(Stream &S, uint32_t Xid, AcceptStatus Status, const XdrPayload &Results) {
  XdrWriter Header;
  Header.u32(Xid);
  Header.u32(kReply);
  Header.u32(kMessageAccepted);
  Header.u32(kAuthNone);
  Header.u32(0);
  Header.u32(static_cast<uint32_t>(Status));

  co_return co_await sendRecord(S, Header.bytes(), Results);
}

Coro<Result<void>> sendCall(Stream &S, uint32_t Xid, uint32_t Program, uint32_t Procedure, std::span<const std::byte> Args) {
  XdrWriter Header;
  Header.u32(Xid);
  Header.u32(kCall);
  Header.u32(kRpcVersion);
  Header.u32(Program);
  Header.u32(kProgramVersion);
  Header.u32(Procedure);
  Header.u32(kAuthNone);
  Header.u32(0);
  Header.u32(kAuthNone);
  Header.u32(0);

  co_return co_await sendRecord(S, Header.bytes(), Args);
}

Coro<Result<ReplyMessage>> receiveReply(Stream &S) {
  auto Record = co_await receiveRecord(S);
  if (!Record) co_return std::unexpected(Record.error());

  XdrReader R(*Record);
  ReplyMessage M;
  M.Xid = R.u32();
  if (R.u32() != kReply) co_return failMessage("not an rpc reply");
  if (R.u32() != kMessageAccepted) co_return failMessage("the call was denied");
  R.u32();
  R.opaque(400);
  M.Status = static_cast<AcceptStatus>(R.u32());
  if (!R.ok()) co_return failMessage("malformed rpc reply header");

  M.Results.assign(Record->end() - static_cast<long>(R.left()), Record->end());
  co_return M;
}

} // namespace rail::nfs
