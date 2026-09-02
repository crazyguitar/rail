#include "rail/proto/control-channel.h"

#include "rail/proto/codec.h"
#include <unistd.h>

#include <cstring>

namespace rail::proto {

namespace {
constexpr size_t kHeaderSize = 10; // magic(4) + type(2) + length(4)
} // namespace

Coro<Result<void>> ControlChannel::send(const Message &M) {
  co_await Order.take();
  const Turn Mine(Order);
  co_return co_await sendClaimed(M);
}

Coro<Result<void>> ControlChannel::sendClaimed(const Message &M) {
  OutBuf.clear();
  OutBuf.reserve(kHeaderSize);
  OutBuf.resize(kHeaderSize);

  encode(M, OutBuf);

  const uint32_t Magic = kMagic;
  const uint16_t T = static_cast<uint16_t>(typeOf(M));
  const uint32_t Length = static_cast<uint32_t>(OutBuf.size() - kHeaderSize);

  std::memcpy(OutBuf.data(), &Magic, 4);
  std::memcpy(OutBuf.data() + 4, &T, 2);
  std::memcpy(OutBuf.data() + 6, &Length, 4);

  co_return co_await writer().writeAll(OutBuf);
}

Coro<Result<Message>> ControlChannel::receive() {
  std::byte Header[kHeaderSize];
  if (auto R = co_await S.readExact(Header); !R) co_return std::unexpected(R.error());

  uint32_t Magic = 0;
  uint16_t T = 0;
  uint32_t Length = 0;
  std::memcpy(&Magic, Header, 4);
  std::memcpy(&T, Header + 4, 2);
  std::memcpy(&Length, Header + 6, 4);

  if (Magic != kMagic) co_return failMessage("bad frame magic");
  if (Length > kMaxFrame) co_return failMessage("frame too large");

  InBuf.resize(Length);
  if (Length > 0) {
    if (auto R = co_await S.readExact(InBuf); !R) co_return std::unexpected(R.error());
  }

  co_return decode(static_cast<Type>(T), InBuf);
}

void ControlChannel::close() {
  S.close();
  if (Split) W.close();
}

Result<ControlChannel> ControlChannel::overStdio() {
  const int Protocol = ::dup(STDOUT_FILENO);
  if (Protocol < 0) return failErrno("dup(stdout)");

  // Every failure below has to give the copy back: this runs once per server
  // process, but a leaked descriptor here is one the caller cannot reach.
  const auto Undo = [Protocol](Error Why) {
    ::close(Protocol);
    return std::unexpected(std::move(Why));
  };

  if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return Undo(failErrno("dup2(stderr, stdout)").error());
  if (auto R = setNonBlocking(STDIN_FILENO); !R) return Undo(R.error());
  if (auto R = setNonBlocking(Protocol); !R) return Undo(R.error());
  return ControlChannel(Stream(STDIN_FILENO), Stream(Protocol));
}

} // namespace rail::proto
