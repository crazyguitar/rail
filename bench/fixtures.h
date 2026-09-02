#pragma once

#include "harness.h"
#include "local-process.h"
#include "remote-host.h"

#include "rail/file-service.h"
#include "rail/fs/writer.h"
#include "rail/io/runner.h"
#include "rail/nfs/client.h"

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rail::bench {
inline constexpr uint16_t kServicePort = 18690;

inline constexpr uint16_t kNfsPort = 20490;

inline constexpr uint64_t kTargetSize = 32ull << 30;

inline constexpr size_t kParallelFiles = 16;

inline constexpr size_t kBlockBytes = 1 << 20;
inline constexpr uint64_t kLocalEach = 1ull << 30;
inline constexpr size_t kLocalReadahead = 1u << 20;

inline std::string targetFor(uint64_t Size) { return "bench-" + std::to_string(Size >> 30) + "g.bin"; }

inline std::string parallelFile(size_t I) { return "bench-par-" + std::to_string(I) + ".bin"; }

inline std::string exportDir() { return envOr("RAIL_DIR", "/tmp/rail-bench-export"); }

inline std::string mountDir() { return envOr("RAIL_MOUNT", "/tmp/rail-bench-mnt"); }

inline std::string kernelMountDir() { return envOr("RAIL_KERNEL_MOUNT", "/tmp/rail-bench-kmnt"); }

// A directory on this machine's own nvme, seeded with the same fixtures. It is
// the disk's ceiling with no network under it at all, which is what says
// whether a row is bound by the fabric or by a drive.
inline std::string localDir() { return envOr("RAIL_LOCAL", "/tmp/rail-bench-local"); }

inline std::string nfsMountDir() { return envOr("RAIL_NFS_MOUNT", "/tmp/rail-bench-nmnt"); }

inline std::string modulePath() {
  if (const char *Given = ::getenv("RAIL_KO")) return Given;

  const std::filesystem::path BuildDir = selfPath().parent_path().parent_path();
  return (BuildDir / "src" / "linux" / "railfs.ko").string();
}

inline std::string sessions() { return envOr("RAIL_SESSIONS", "4"); }

inline std::string chunk() {
  const std::string Want = envOr("RAIL_WRITE_CHUNK", "");
  return Want.empty() ? "" : " --write-chunk " + Want;
}

inline std::string threads() {
  const std::string Want = envOr("RAIL_THREADS", "");
  return Want.empty() ? "" : " --threads " + Want;
}

class Export {
public:
  static Export &get() {
    static Export Only;
    return Only;
  }

  Export(const Export &) = delete;
  Export &operator=(const Export &) = delete;

  ~Export() {
    [[maybe_unused]] auto Local = runLocal({"pkill", "-f", "mount.railnfs --serve"});
    onPeer("pkill -x raild");
  }

  bool ready() const { return Ready; }

private:
  Export() { Ready = start(); }

  bool start() {
    [[maybe_unused]] auto Local = runLocal({"pkill", "-f", "mount.railnfs --serve"});
    if (!onPeer("pkill -x raild")) return false;
    if (!onPeer("mkdir -p " + exportDir())) return false;
    if (!onPeer("chmod 0777 " + exportDir())) return false;
    if (!seedFixtures()) return false;
    if (!onPeer(forgetOnPeer(3))) return false;
    dropLocalCache();
    if (!startDaemon()) return false;
    if (!startExport()) return false;

    std::this_thread::sleep_for(std::chrono::seconds(3));
    return true;
  }

  static void dropLocalCache() {
    [[maybe_unused]] auto Dropped = runLocal({"sh", "-c", forgetHere(3)});
  }

  bool seedFixtures() {
    for (const uint64_t Size : {uint64_t{1} << 30, uint64_t{8} << 30, kTargetSize}) {
      const std::string Target = exportDir() + "/" + targetFor(Size);
      if (!onPeer("[ -s " + Target + " ] || dd if=/dev/urandom of=" + Target + " bs=1M count=" + std::to_string(Size >> 20) + " status=none"))
        return false;
    }

    const std::string Smallest = exportDir() + "/" + targetFor(uint64_t{1} << 30);
    for (size_t I = 0; I < kParallelFiles; I++) {
      const std::string Target = exportDir() + "/" + parallelFile(I);
      if (!onPeer("[ -s " + Target + " ] || cp " + Smallest + " " + Target)) return false;
    }
    return true;
  }

  bool startDaemon() {
    return onPeer("cd / && setsid " + toolPath("raild") + " --serve " + exportDir() + " --port " + std::to_string(kServicePort) +
                  " --backend rdma --threads " + envOr("RAIL_DAEMON_THREADS", "8") + " > /tmp/raild-bench.log 2>&1 < /dev/null &");
  }

  bool startExport() {
    const std::string Command = "exec " + toolPath("mount.railnfs") + " --serve " + fabricHost() + " --nfs-port " + std::to_string(kNfsPort) +
                                " --port " + std::to_string(kServicePort) + " --sessions " + sessions() +
                                " --backend rdma > /tmp/railnfs-bench.log 2>&1";
    auto Started = BackgroundProcess::start({"sh", "-c", Command});
    if (!Started) return false;
    Serving.emplace(std::move(*Started));
    return true;
  }

  static bool onPeer(const std::string &Command) {
    auto Opened = RemoteHost::open(peerHost());
    if (!Opened) return false;

    auto Ran = Opened->run({"sh", "-c", Command});
    if (!Ran) return false;
    while (Ran->readLine()) {}
    return true;
  }

  std::optional<BackgroundProcess> Serving;
  bool Ready = false;
};

namespace {

std::vector<std::string> asRoot(std::vector<std::string> Argv) {
  if (::geteuid() == 0) return Argv;

  Argv.insert(Argv.begin(), "-n");
  Argv.insert(Argv.begin(), "sudo");
  return Argv;
}

} // namespace

class KernelMount {
public:
  static KernelMount &get() {
    static KernelMount Only;
    return Only;
  }

  KernelMount(const KernelMount &) = delete;
  KernelMount &operator=(const KernelMount &) = delete;

  ~KernelMount() { clear(); }

  bool ready() const { return Ready; }

  const char *why() const { return Why; }

private:
  KernelMount() { Ready = start(); }

  static bool ran(const std::vector<std::string> &Argv) {
    auto R = runLocal(Argv);
    return R && R->ExitStatus == 0;
  }

  bool refuse(const char *Reason) {
    Why = Reason;
    clear();
    return false;
  }

  bool start() {
    if (!Export::get().ready()) return refuse("the export is not serving");
    if (::geteuid() != 0 && !ran({"sudo", "-n", "true"})) return refuse("these need root, or passwordless sudo");
    if (!std::filesystem::exists(modulePath())) return refuse("no railfs.ko; build the tree or set RAIL_KO");

    clear();

    std::error_code EC;
    std::filesystem::create_directories(kernelMountDir(), EC);

    const std::string Params = envOr("RAIL_KO_PARAMS", "");
    std::vector<std::string> Load = asRoot({"insmod", modulePath()});
    if (!Params.empty()) Load.push_back(Params);
    if (!ran(Load)) return refuse("insmod failed; is a stale railfs still loaded");
    Loaded = true;

    // The daemon serves exportDir(), and an export is named relative to it, so
    // / is the whole of what it serves.
    const std::string Options = "host=" + fabricHost() + ",export=/,port=" + std::to_string(kServicePort) +
                                ",rdma,uid=" + std::to_string(::getuid()) + ",gid=" + std::to_string(::getgid()) + envOr("RAIL_KERNEL_OPTS", "");
    // -i so mount never hands off to an installed /sbin/mount.railfs. uid and gid
    // because the wire carries no ownership, and without them every inode
    // belongs to root and the benchmark cannot write.
    if (!ran(asRoot({"mount", "-i", "-t", "railfs", "-o", Options, "none", kernelMountDir()}))) return refuse("mount -t railfs failed; see dmesg");

    Mounted = true;
    if (!std::filesystem::exists(std::filesystem::path(kernelMountDir()) / targetFor(kTargetSize)))
      return refuse("the fixture is not visible through the kernel mount");

    return true;
  }

  void clear() {
    if (Mounted) ran(asRoot({"umount", kernelMountDir()}));
    if (Loaded) ran(asRoot({"rmmod", "railfs"}));
    Mounted = false;
    Loaded = false;
  }

  bool Loaded = false;
  bool Mounted = false;
  bool Ready = false;
  const char *Why = "the kernel mount is not up";
};

class Local {
public:
  static Local &get() {
    static Local Only;
    return Only;
  }

  Local(const Local &) = delete;
  Local &operator=(const Local &) = delete;

  ~Local() {
    if (!Knob.empty() && !Restore.empty()) {
      const std::string Tee = ::geteuid() == 0 ? std::string("tee ") : std::string("sudo -n tee ");
      [[maybe_unused]] auto Put = runLocal({"sh", "-c", "echo " + Restore + " | " + Tee + Knob + " > /dev/null"});
    }
  }

  bool ready() const { return Ready; }
  const char *why() const { return Why; }

private:
  Local() { Ready = seed() && widen(); }

  bool seed() {
    std::error_code EC;
    std::filesystem::create_directories(localDir(), EC);
    if (EC) {
      Why = "could not make the local fixture directory";
      return false;
    }

    for (size_t I = 0; I < kParallelFiles; I++) {
      const std::filesystem::path One = std::filesystem::path(localDir()) / parallelFile(I);
      if (std::filesystem::exists(One, EC) && std::filesystem::file_size(One, EC) == kLocalEach) {
        continue;
      }

      const std::string Make = "dd if=/dev/urandom of=" + One.string() + " bs=1M count=" + std::to_string(kLocalEach >> 20) + " status=none";
      auto Done = runLocal({"sh", "-c", Make});
      if (!Done || Done->ExitStatus != 0) {
        Why = "could not seed the local fixtures";
        return false;
      }
    }

    auto Synced = runLocal({"sync"});
    return Synced && Synced->ExitStatus == 0;
  }

  // 128 KiB of readahead is what a stock disk gives a sequential reader, and it
  // holds four of them to 2.0 GiB/s where the same drive does 9.9. The mounts
  // being compared against do their own readahead by the hundred megabyte, so
  // without this the column measures a default rather than the disk.
  bool widen() {
    Why = "could not widen the local readahead; the column would report a default";

    auto Named = runLocal({"sh", "-c", "findmnt -no SOURCE --nofsroot --target " + localDir() + " | sed 's|/dev/||; s|p\\?[0-9]*$||'"});
    if (!Named || Named->ExitStatus != 0) {
      return false;
    }

    Knob = "/sys/block/" + trimmed(Named->Output) + "/queue/read_ahead_kb";
    auto Was = runLocal({"sh", "-c", "cat " + Knob});
    if (!Was || Was->ExitStatus != 0) {
      return false;
    }

    Restore = trimmed(Was->Output);
    const std::string Tee = ::geteuid() == 0 ? std::string("tee ") : std::string("sudo -n tee ");
    auto Set = runLocal({"sh", "-c", "echo " + std::to_string(kLocalReadahead >> 10) + " | " + Tee + Knob + " > /dev/null"});
    return Set && Set->ExitStatus == 0;
  }

  static std::string trimmed(const std::string &Text) {
    const size_t End = Text.find_last_not_of(" \t\r\n");
    return End == std::string::npos ? std::string{} : Text.substr(0, End + 1);
  }

  std::string Knob;
  std::string Restore;
  bool Ready = false;
  const char *Why = "the local fixtures are not there";
};

class NfsMount {
public:
  static NfsMount &get() {
    static NfsMount Only;
    return Only;
  }

  NfsMount(const NfsMount &) = delete;
  NfsMount &operator=(const NfsMount &) = delete;

  ~NfsMount() { clear(); }

  bool ready() const { return Ready; }
  const char *why() const { return Why; }

private:
  NfsMount() { Ready = start(); }

  static bool ran(const std::vector<std::string> &Argv) {
    auto Done = runLocal(Argv);
    return Done && Done->ExitStatus == 0;
  }

  bool refuse(const char *Reason) {
    Why = Reason;
    clear();
    return false;
  }

  bool start() {
    if (!Export::get().ready()) return refuse("the export is not serving");
    if (::geteuid() != 0 && !ran({"sudo", "-n", "true"})) return refuse("these need root, or passwordless sudo");

    clear();

    std::error_code EC;
    std::filesystem::create_directories(nfsMountDir(), EC);

    // soft, because a hard mount against a server that stops answering wedges
    // every process that touches the directory.
    const std::string Options = "vers=3,proto=tcp,port=" + std::to_string(kNfsPort) + ",mountport=" + std::to_string(kNfsPort) +
                                ",mountvers=3,nolock,soft,timeo=100,retrans=2,nconnect=8" + envOr("RAIL_NFS_OPTS", "");
    if (!ran(asRoot({"mount", "-t", "nfs", "-o", Options, "127.0.0.1:/", nfsMountDir()}))) return refuse("mount -t nfs failed");

    Mounted = true;
    if (!std::filesystem::exists(std::filesystem::path(nfsMountDir()) / targetFor(kTargetSize)))
      return refuse("the fixture is not visible through the nfs mount");

    return true;
  }

  void clear() {
    if (Mounted) ran(asRoot({"umount", "-f", nfsMountDir()}));
    Mounted = false;
  }

  bool Mounted = false;
  bool Ready = false;
  const char *Why = "the nfs mount is not up";
};

class Mount {
public:
  static Mount &get() {
    static Mount Only;
    return Only;
  }

  Mount(const Mount &) = delete;
  Mount &operator=(const Mount &) = delete;

  ~Mount() { clear(); }

  bool ready() const { return Ready; }

  bool use(bool Verify) {
    if (Ready && Verifying == Verify) return true;
    clear();
    Verifying = Verify;
    Ready = start();
    return Ready;
  }

private:
  Mount() { Ready = start(); }

  bool start() {
    if (!Export::get().ready()) return false;
    clear();

    std::error_code EC;
    std::filesystem::create_directories(mountDir(), EC);

    const std::string Command = "exec " + toolPath("mount.railfuse") + " " + fabricHost() + " " + mountDir() + " --port " + std::to_string(kServicePort) +
                                " --sessions " + sessions() + threads() + chunk() + " --backend rdma" +
                                std::string(Verifying ? "" : " --no-checksum") + " > /tmp/railfs-bench.log 2>&1";
    auto Started = BackgroundProcess::start({"sh", "-c", Command});
    if (!Started) return false;
    Serving.emplace(std::move(*Started));

    return appears(std::filesystem::path(mountDir()) / targetFor(kTargetSize));
  }

  static bool appears(const std::filesystem::path &Fixture) {
    std::error_code EC;
    for (int I = 0; I < 150; I++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (std::filesystem::exists(Fixture, EC)) return true;
    }
    return false;
  }

  static void clear() {
    [[maybe_unused]] auto Unmounted = runLocal({"fusermount3", "-u", "-z", mountDir()});
    [[maybe_unused]] auto Killed = runLocal({"pkill", "-x", "mount.railfuse"});
  }

  std::optional<BackgroundProcess> Serving;
  bool Ready = false;
  bool Verifying = true;
};

inline bool exportReady(benchmark::State &State) { return available(State, Export::get().ready(), "could not start the export"); }

inline bool mountReady(benchmark::State &State) { return available(State, Mount::get().use(true), "could not start the mount"); }

inline bool kernelMountReady(benchmark::State &State) { return available(State, KernelMount::get().ready(), KernelMount::get().why()); }

inline std::string onKernelMount(const std::string &Name) { return (std::filesystem::path(kernelMountDir()) / Name).string(); }

inline bool mountReadyUnverified(benchmark::State &State) {
  return available(State, Mount::get().use(false), "could not start the mount without checksums");
}

inline std::string onMount(const std::string &Name) { return (std::filesystem::path(mountDir()) / Name).string(); }

inline bool nfsMountReady(benchmark::State &State) { return available(State, NfsMount::get().ready(), NfsMount::get().why()); }

inline bool localReady(benchmark::State &State) { return available(State, Local::get().ready(), Local::get().why()); }

inline std::string onLocal(const std::string &Name) { return (std::filesystem::path(localDir()) / Name).string(); }

inline std::string onNfsMount(const std::string &Name) { return (std::filesystem::path(nfsMountDir()) / Name).string(); }

class AlignedPages {
public:
  AlignedPages(size_t Count, size_t Each) : Each(Each), Memory(static_cast<std::byte *>(std::aligned_alloc(kDirectAlignment, Count * Each))) {}
  AlignedPages(const AlignedPages &) = delete;
  AlignedPages &operator=(const AlignedPages &) = delete;
  ~AlignedPages() { std::free(Memory); }

  bool valid() const { return Memory != nullptr; }
  std::span<std::byte> operator[](size_t I) { return {Memory + I * Each, Each}; }
  size_t indexOf(std::span<std::byte> Page) const { return static_cast<size_t>(Page.data() - Memory) / Each; }

private:
  size_t Each;
  std::byte *Memory;
};

inline bool allocated(benchmark::State &State, const AlignedPages &Pages) {
  return available(State, Pages.valid(), "could not allocate aligned pages");
}

inline Result<std::unique_ptr<FileClient>> openService(size_t PageSize = 0, size_t PageCount = 0) {
  ServiceOptions Opts;
  Opts.Backend = "rdma";
  Opts.Port = kServicePort;
  if (PageSize > 0) Opts.PageSize = PageSize;
  if (PageCount > 0) Opts.PageCount = PageCount;
  return run(FileClient::connect(fabricHost(), Opts));
}

struct OverNfs {
  nfs::NfsClient Client;
  nfs::Handle File;
};

inline Result<OverNfs> openOverNfs(const std::string &Name) {
  auto Client = nfs::NfsClient::connect("127.0.0.1", kNfsPort);
  if (!Client) return std::unexpected(Client.error());

  auto Root = run(Client->mountRoot("/"));
  if (!Root) return std::unexpected(Root.error());

  auto File = run(Client->lookup(*Root, Name));
  if (!File) return std::unexpected(File.error());

  return OverNfs{std::move(*Client), std::move(*File)};
}

template <class Client, class File>
Coro<Result<void>> readRound(Client &Reader, const File &Which, uint64_t &Offset, size_t Block, std::vector<std::vector<std::byte>> &Landing) {
  const size_t Depth = Landing.size();

  for (size_t I = 0; I < Depth; I++)
    if (auto R = co_await Reader.submitRead(Which, Offset + I * Block, Landing[I]); !R) co_return std::unexpected(R.error());

  for (size_t I = 0; I < Depth; I++)
    if (auto Got = co_await Reader.collectRead(); !Got) co_return std::unexpected(Got.error());

  Offset = (Offset + Depth * Block) % (kTargetSize - Depth * Block);
  co_return Result<void>{};
}

inline Result<void> readWholeFile(int Fd, uint64_t From, uint64_t Size, std::span<std::byte> Landing) {
  for (uint64_t Done = 0; Done < Size;) {
    const size_t Want = static_cast<size_t>(std::min<uint64_t>(Landing.size(), Size - Done));
    const ssize_t Got = ::pread(Fd, Landing.data(), Want, static_cast<off_t>(From + Done));
    if (Got < 0) return failErrno("pread through the mount");
    if (static_cast<size_t>(Got) != Want) return failMessage("short read, the mount would be measured on stale bytes");
    Done += Want;
  }
  return {};
}

class OpenFd {
public:
  OpenFd(const std::string &Path, int Flags, ::mode_t Mode = 0) : Fd(::open(Path.c_str(), Flags, Mode)) {}
  OpenFd(const OpenFd &) = delete;
  OpenFd &operator=(const OpenFd &) = delete;
  ~OpenFd() {
    if (Fd >= 0) ::close(Fd);
  }

  bool valid() const { return Fd >= 0; }
  int get() const { return Fd; }

  Result<void> release() {
    const int Closing = Fd;
    Fd = -1;
    if (Closing >= 0 && ::close(Closing) != 0) return failErrno("close through the mount");
    return {};
  }

private:
  int Fd;
};

} // namespace rail::bench
