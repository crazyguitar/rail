#pragma once

#include "harness.h"
#include "remote-host.h"

#include "rail/io/runner.h"
#include "rail/transport/data-channel.h"

#include <memory>
#include <span>
#include <optional>
#include <string>
#include <vector>

namespace rail::bench {
inline size_t poolPages(size_t Depth) { return Depth + 1; }

inline std::string toHex(const std::string &Blob) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Text;
  Text.reserve(Blob.size() * 2);
  for (const unsigned char C : Blob) {
    Text.push_back(Digits[C >> 4]);
    Text.push_back(Digits[C & 0xF]);
  }
  return Text;
}

inline std::string fromHex(const std::string &Text) {
  if (Text.empty() || Text.size() % 2 != 0) return {};

  std::string Blob;
  Blob.reserve(Text.size() / 2);
  for (size_t I = 0; I < Text.size(); I += 2) {
    const auto Nibble = [](char C) -> int {
      if (C >= '0' && C <= '9') return C - '0';
      if (C >= 'a' && C <= 'f') return C - 'a' + 10;
      return -1;
    };
    const int Hi = Nibble(Text[I]);
    const int Lo = Nibble(Text[I + 1]);
    if (Hi < 0 || Lo < 0) return {};
    Blob.push_back(static_cast<char>(Hi * 16 + Lo));
  }
  return Blob;
}

template <class Post> Coro<Result<void>> burst(std::span<Page> Bufs, Post Start) {
  std::vector<Coro<Result<void>>> Ops;
  Ops.reserve(Bufs.size());
  for (auto &Buf : Bufs) {
    Ops.push_back(Start(Buf));
    Ops.back().start();
  }

  Result<void> First;
  for (auto &Op : Ops)
    if (auto R = co_await Op.join(); !R && First) First = std::unexpected(R.error());
  co_return First;
}

// Posting and joining are separate so a receiver can keep one half of its
// buffers on the queue while it drains the other. Draining all of them first
// leaves a window where the sender's write finds nothing posted, and the rnr
// backoff that follows costs about a second.
class Burst {
public:
  template <class Post> void post(std::span<Page> Bufs, Post Start) {
    Ops.clear();
    Ops.reserve(Bufs.size());
    for (auto &Buf : Bufs) {
      Ops.push_back(Start(Buf));
      Ops.back().start();
    }
  }

  Coro<Result<void>> join() {
    Result<void> First;
    for (auto &Op : Ops)
      if (auto R = co_await Op.join(); !R && First) First = std::unexpected(R.error());
    co_return First;
  }

private:
  std::vector<Coro<Result<void>>> Ops;
};

inline Coro<Result<void>> receiveBurst(DataChannel &Channel, std::span<Page> Bufs, size_t MessageSize) {
  return burst(Bufs, [&Channel, MessageSize](Page &Buf) { return Channel.recv(Buf, 0, MessageSize); });
}

inline Coro<Result<void>> sendBurst(DataChannel &Channel, std::span<Page> Bufs) {
  return burst(Bufs, [&Channel](Page &Buf) { return Channel.send(Buf, 0); });
}

class Fabric {
public:
  Fabric(size_t MessageSize, size_t Depth) { start(MessageSize, Depth); }

  bool ready() const { return Channel != nullptr; }
  DataChannel &channel() { return *Channel; }

private:
  void start(size_t MessageSize, size_t Depth) {
    auto Opened = RemoteHost::open(peerHost());
    if (!Opened) return;
    Host.emplace(std::move(*Opened));

    Channel = makeDataChannel("rdma", poolPages(Depth), MessageSize, peerHost());
    if (!Channel) return;
    if (auto R = Channel->prepare(); !R) {
      Channel.reset();
      return;
    }

    auto Mine = run(Channel->localEndpoint());
    if (!Mine) {
      Channel.reset();
      return;
    }

    auto Started = Host->run({selfPath().string(), "--serve-buffers", std::to_string(MessageSize), std::to_string(Depth), toHex(*Mine)});
    if (!Started) {
      Channel.reset();
      return;
    }
    Serving.emplace(std::move(*Started));

    auto Endpoint = Serving->readLine();
    if (!Endpoint) {
      Channel.reset();
      return;
    }

    const std::string Blob = fromHex(*Endpoint);
    if (Blob.empty() || !run(Channel->connect(Blob))) Channel.reset();
  }

  std::optional<RemoteHost> Host;
  std::optional<RemoteProcess> Serving;
  std::unique_ptr<DataChannel> Channel;
};

} // namespace rail::bench
