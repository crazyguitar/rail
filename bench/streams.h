#pragma once

#include "fixtures.h"
#include "p2p.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>

namespace rail::bench {
inline constexpr uint64_t kCompareEach = 1ull << 30;
inline constexpr size_t kCompareWindow = 16;
inline constexpr size_t kP2pDepth = 256;

enum class Via { P2p, Rail, Nvme, Tmpfs, Nfs, Fuse, Railfs };

// Every write case runs against a drive the previous ones have just filled, and
// a drive that has taken a hundred gigabytes back to back reports its sustained
// rate rather than its ceiling. Without this the column ranks backends by the
// order they run in: the peer takes writes from four of them, so whichever goes
// last measures the most exhausted disk.
inline constexpr const char *kSettle = "sync; sleep 20";

inline void settleLocal() { [[maybe_unused]] auto Flushed = runLocal({"sh", "-c", kSettle}); }

inline void settlePeer() {
  auto Opened = RemoteHost::open(peerHost());
  if (!Opened) {
    return;
  }

  auto Ran = Opened->run({"sh", "-c", kSettle});
  if (!Ran) {
    return;
  }
  while (Ran->readLine()) {}
}

inline void settleFor(Via How) {
  switch (How) {
  case Via::P2p:
  case Via::Tmpfs:
    return;
  case Via::Nvme:
    settleLocal();
    return;
  default:
    settlePeer();
    return;
  }
}

// Without this a second case is answered from the local page cache.
inline void coldLocal() { [[maybe_unused]] auto Dropped = runLocal({"sh", "-c", forgetHere(3)}); }

inline bool reachable(benchmark::State &State, Via How) {
  switch (How) {
  case Via::P2p:
  case Via::Rail:
    return exportReady(State);
  case Via::Nvme:
    return localReady(State);
  case Via::Tmpfs:
    return localTmpfsReady(State);
  case Via::Nfs:
    return nfsMountReady(State);
  case Via::Fuse:
    return mountReady(State);
  case Via::Railfs:
    return kernelMountReady(State);
  }
  return false;
}

inline std::string pathFor(Via How, const std::string &Name) {
  switch (How) {
  case Via::Nvme:
    return onLocal(Name);
  case Via::Tmpfs:
    return onLocalTmpfs(Name);
  case Via::Nfs:
    return onNfsMount(Name);
  case Via::Fuse:
    return onMount(Name);
  case Via::Railfs:
    return onKernelMount(Name);
  case Via::P2p:
  case Via::Rail:
    break;
  }
  return Name;
}

inline std::string readFile(size_t I) { return parallelFile(I % kParallelFiles); }

inline std::string writeFile(size_t I) { return "bench-w" + std::to_string(I) + ".bin"; }

inline Result<void> readThroughMount(const std::string &Path, uint64_t Each) {
  OpenFd Fd(Path, O_RDONLY);
  if (!Fd.valid()) return failErrno("open " + Path);

  std::vector<std::byte> Landing(kBlockBytes);
  return readWholeFile(Fd.get(), 0, Each, Landing);
}

// Direct for the local disk, so the column says what the drive does rather than
// what its page cache does. The backends it is compared against write to the
// peer with O_DIRECT, and buffered here would read as a network filesystem
// beating the disk underneath it.
inline Result<void> writeThroughMount(const std::string &Path, uint64_t Each, bool Direct = false) {
  OpenFd Fd(Path, O_WRONLY | O_CREAT | O_TRUNC | (Direct ? O_DIRECT : 0), 0644);
  if (!Fd.valid()) return failErrno("create " + Path);

  AlignedPages Page(1, kBlockBytes);
  if (!Page.valid()) return failMessage("could not allocate a write buffer");
  std::memset(Page[0].data(), 'w', kBlockBytes);

  for (uint64_t Done = 0; Done < Each; Done += kBlockBytes) {
    const size_t Want = static_cast<size_t>(std::min<uint64_t>(kBlockBytes, Each - Done));
    if (::pwrite(Fd.get(), Page[0].data(), Want, static_cast<off_t>(Done)) != static_cast<ssize_t>(Want)) return failErrno("write " + Path);
  }

  if (::fsync(Fd.get()) != 0) return failErrno("fsync " + Path);
  return Fd.release();
}

class ReadWindow {
public:
  ReadWindow(FileClient &Client, std::string Name, uint64_t Each, size_t Depth)
      : Client(Client), Name(std::move(Name)), Each(Each), Depth(Depth), Landing(Depth, kBlockBytes) {}

  bool valid() const { return Landing.valid(); }
  bool complete() const { return Done >= Each; }

  Coro<Result<void>> fill() {
    while (Sent < Each && (Sent - Done) / kBlockBytes < Depth) {
      if (auto R = co_await Client.submitRead(Name, Sent, Landing[slot(Sent)]); !R) co_return std::unexpected(R.error());
      Sent += kBlockBytes;
    }
    co_return Result<void>{};
  }

  Coro<Result<void>> take() {
    auto Got = co_await Client.collectRead();
    if (!Got) co_return std::unexpected(Got.error());
    if (Got->Bytes != kBlockBytes) co_return failMessage("short read, the case would be measured on stale bytes");
    Done += kBlockBytes;
    co_return Result<void>{};
  }

private:
  size_t slot(uint64_t At) const { return static_cast<size_t>((At / kBlockBytes) % Depth); }

  FileClient &Client;
  std::string Name;
  uint64_t Each;
  size_t Depth;
  AlignedPages Landing;
  uint64_t Sent = 0;
  uint64_t Done = 0;
};

inline Coro<Result<void>> pull(ReadWindow &Window) {
  while (!Window.complete()) {
    if (auto R = co_await Window.fill(); !R) co_return R;
    if (auto R = co_await Window.take(); !R) co_return R;
  }
  co_return Result<void>{};
}

inline Coro<Result<void>> push(FileClient &Client, const std::string &Name, uint64_t Each, AlignedPages &Page) {
  for (uint64_t Done = 0; Done < Each; Done += kBlockBytes) {
    const size_t Want = static_cast<size_t>(std::min<uint64_t>(kBlockBytes, Each - Done));
    if (auto R = co_await Client.write(Name, Done, Page[0].first(Want), Done == 0); !R) co_return std::unexpected(R.error());
  }
  co_return co_await Client.fsync(Name);
}

class Stream {
public:
  virtual ~Stream() = default;
  virtual Result<void> move(uint64_t Each) = 0;
};

class MountStream : public Stream {
public:
  MountStream(std::string Path, bool Writing, bool Direct) : Path(std::move(Path)), Writing(Writing), Direct(Direct) {}

  Result<void> move(uint64_t Each) override { return Writing ? writeThroughMount(Path, Each, Direct) : readThroughMount(Path, Each); }

private:
  std::string Path;
  bool Writing;
  bool Direct;
};

class ServiceStream : public Stream {
public:
  static Result<std::unique_ptr<Stream>> open(std::string Name, bool Writing) {
    auto Client = openService(kBlockBytes, kCompareWindow + 4);
    if (!Client) return std::unexpected(Client.error());
    return std::make_unique<ServiceStream>(std::move(*Client), std::move(Name), Writing);
  }

  ServiceStream(std::unique_ptr<FileClient> Client, std::string Name, bool Writing)
      : Client(std::move(Client)), Name(std::move(Name)), Writing(Writing) {}

  ~ServiceStream() override { run(Client->close()); }

  Result<void> move(uint64_t Each) override { return Writing ? write(Each) : read(Each); }

private:
  Result<void> read(uint64_t Each) {
    ReadWindow Window(*Client, Name, Each, std::min(kCompareWindow, Client->maxOutstanding()));
    if (!Window.valid()) return failMessage("could not allocate the landing window");
    return run(pull(Window));
  }

  Result<void> write(uint64_t Each) {
    AlignedPages Page(1, kBlockBytes);
    if (!Page.valid()) return failMessage("could not allocate a write buffer");
    std::memset(Page[0].data(), 'w', kBlockBytes);
    return run(push(*Client, Name, Each, Page));
  }

  std::unique_ptr<FileClient> Client;
  std::string Name;
  bool Writing;
};

class P2pStream : public Stream {
public:
  static Result<std::unique_ptr<Stream>> open(size_t Depth) {
    auto Mine = std::make_unique<P2pStream>();
    if (auto R = Mine->start(Depth); !R) return std::unexpected(R.error());
    return Mine;
  }

  Result<void> move(uint64_t Each) override {
    const uint64_t PerBurst = static_cast<uint64_t>(kBlockBytes) * Bufs.size();
    for (uint64_t Done = 0; Done < Each; Done += PerBurst) {
      if (auto R = run(sendBurst(Link->channel(), Bufs)); !R) return R;
    }
    return {};
  }

private:
  // The first burst pins the buffers with the nic, which would otherwise be
  // timed as bandwidth.
  Result<void> start(size_t Depth) {
    Link = std::make_unique<Fabric>(kBlockBytes, Depth);
    if (!Link->ready()) return failMessage("could not reach the peer over rdma");

    for (size_t I = 0; I < Depth; I++) {
      Bufs.push_back(run(Link->channel().pool().acquire()));
      if (!Bufs.back().valid()) return failMessage("no registered memory for the transfer pages");
      Bufs.back().resize(kBlockBytes);
    }
    return run(sendBurst(Link->channel(), Bufs));
  }

  std::unique_ptr<Fabric> Link;
  std::vector<Page> Bufs;
};

// One channel, not q of them: each would want its own helper on the peer, and
// sshd allows ten sessions. Its window is fixed, so the row is a flat ceiling.
inline size_t workersFor(Via How, size_t Q) { return How == Via::P2p ? 1 : Q; }

inline Result<std::unique_ptr<Stream>> openStream(Via How, bool Writing, size_t I, size_t Q) {
  if (How == Via::P2p) return P2pStream::open(kP2pDepth);

  const std::string Name = Writing ? writeFile(I) : readFile(I);
  if (How == Via::Rail) return ServiceStream::open(Name, Writing);
  return std::make_unique<MountStream>(pathFor(How, Name), Writing, How == Via::Nvme);
}

// Workers outlive the timed region: a mount's session is up before it is
// measured, so a client's has to be too.
class Fleet {
public:
  Fleet(size_t Q, Via How, bool Writing) {
    const size_t Workers = workersFor(How, Q);

    Pending = Workers;
    for (size_t I = 0; I < Workers; I++) {
      Threads.emplace_back([this, I, How, Writing, Q] { serve(I, How, Writing, Q); });
    }
    Opened = waitForRound();
  }

  Fleet(const Fleet &) = delete;
  Fleet &operator=(const Fleet &) = delete;

  ~Fleet() {
    {
      const std::lock_guard<std::mutex> Held(Guard);
      Stopping = true;
      Generation++;
    }
    Wake.notify_all();
    for (auto &One : Threads) One.join();
  }

  bool ready() const { return Opened.has_value(); }

  Result<void> round(uint64_t Total) {
    if (!Opened) return std::unexpected(Opened.error());

    {
      const std::lock_guard<std::mutex> Held(Guard);
      Each = Total / Threads.size();
      Pending = Threads.size();
      Generation++;
    }
    Wake.notify_all();
    return waitForRound();
  }

private:
  void serve(size_t I, Via How, bool Writing, size_t Q) {
    auto Opening = openStream(How, Writing, I, Q);
    std::unique_ptr<Stream> Mine;
    if (Opening) {
      Mine = std::move(*Opening);
    } else {
      record(Opening.error());
    }

    unsigned Seen = finish();
    while (true) {
      std::unique_lock<std::mutex> Held(Guard);
      Wake.wait(Held, [&] { return Generation != Seen; });
      Seen = Generation;
      const bool Stop = Stopping;
      const uint64_t Bytes = Each;
      Held.unlock();

      if (Stop) return;
      if (Mine) {
        if (auto Moved = Mine->move(Bytes); !Moved) record(Moved.error());
      }
      finish();
    }
  }

  void record(const Error &What) {
    const std::lock_guard<std::mutex> Held(Guard);
    if (!First) First = What;
  }

  unsigned finish() {
    unsigned Seen;
    {
      const std::lock_guard<std::mutex> Held(Guard);
      Pending--;
      Seen = Generation;
    }
    Done.notify_all();
    return Seen;
  }

  Result<void> waitForRound() {
    std::unique_lock<std::mutex> Held(Guard);
    Done.wait(Held, [&] { return Pending == 0; });
    if (First) return std::unexpected(*First);
    return Result<void>{};
  }

  std::vector<std::thread> Threads;
  std::mutex Guard;
  std::condition_variable Wake;
  std::condition_variable Done;
  std::optional<Error> First;
  Result<void> Opened;
  uint64_t Each = 0;
  size_t Pending = 0;
  unsigned Generation = 0;
  bool Stopping = false;
};

// A client registers the rdma memory it transfers through the first time it
// transfers through it, and for a mount that is five gigabytes of ibv_reg_mr.
// Whichever repetition runs first pays, and it came back at a third of the
// speed of the rest. So read a round here and throw it away: same backend,
// same width, same bytes, just not on the clock. Once is enough - the second
// round through finds the memory already registered, which is the state every
// timed repetition should start from.
inline void warmOnce(Via How, size_t Q) {
  static std::set<Via> Warmed;
  if (!Warmed.insert(How).second) return;

  Fleet Streams(Q, How, false);
  if (!Streams.ready()) return;

  [[maybe_unused]] const auto Ignored = Streams.round(kCompareEach * Q);
}

} // namespace rail::bench
