#pragma once

#include "rail/io/coro.h"
#include "rail/proto/message.h"
#include "rail/result.h"

#include <cstdint>

namespace rail::proto {

// A transport that carries control frames itself, such as the rdma rings.
// Messages are matched by content, never by arrival order: some fabrics reorder.
class ControlLink {
public:
  virtual ~ControlLink() = default;

  virtual Coro<Result<void>> send(const Message &M) = 0;
  virtual Coro<Result<Message>> receive() = 0;

  // Every reply for Id has been taken; the transport may reuse what it held.
  virtual void release(uint64_t Id) = 0;

  virtual void close() = 0;
};

} // namespace rail::proto
