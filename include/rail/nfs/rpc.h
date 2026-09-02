#pragma once

#include "rail/io/coro.h"
#include "rail/io/stream.h"
#include "rail/nfs/xdr.h"
#include "rail/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rail::nfs {

inline constexpr uint32_t kNfsProgram = 100003;
inline constexpr uint32_t kMountProgram = 100005;
inline constexpr uint32_t kProgramVersion = 3;
inline constexpr size_t kMaxRecord = 16u << 20;

enum class AcceptStatus : uint32_t {
  Success = 0,
  ProgramUnavailable = 1,
  ProgramMismatch = 2,
  ProcedureUnavailable = 3,
  GarbageArguments = 4,
};

struct Call {
  uint32_t Xid = 0;
  uint32_t Program = 0;
  uint32_t Version = 0;
  uint32_t Procedure = 0;
  std::vector<std::byte> Args;
};

struct ReplyMessage {
  uint32_t Xid = 0;
  AcceptStatus Status = AcceptStatus::Success;
  std::vector<std::byte> Results;
};

Coro<Result<Call>> receiveCall(Stream &S);
Coro<Result<void>> sendReply(Stream &S, uint32_t Xid, AcceptStatus Status, const XdrPayload &Results);

Coro<Result<void>> sendCall(Stream &S, uint32_t Xid, uint32_t Program, uint32_t Procedure, std::span<const std::byte> Args);
Coro<Result<ReplyMessage>> receiveReply(Stream &S);

} // namespace rail::nfs
