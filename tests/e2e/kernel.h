#pragma once

#include "harness.h"
#include "local-process.h"
#include "privileged.h"
#include "remote-host.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace rail::e2e {

inline constexpr uint16_t kPort = 18719;

inline std::filesystem::path repoRoot() { return serviceBinary().parent_path().parent_path().parent_path().parent_path(); }

inline std::filesystem::path modulePath() {
  if (const char *Given = ::getenv("RAIL_KO")) return Given;
  return repoRoot() / "build" / "src" / "linux" / "railfs.ko";
}

class Kernel : public ::testing::Test {
protected:
  void SetUp() override {
    if (!::getenv("RAIL_KERNEL_TESTS")) GTEST_SKIP() << "set RAIL_KERNEL_TESTS=1 to run these";
    if (!std::filesystem::exists(modulePath())) GTEST_SKIP() << "no module at " << modulePath() << "; run make in src/linux";
    ASSERT_TRUE(runningAsRoot()) << "the privileged suites need the binary run under sudo";

    Export = remoteDir() + "/kernel-export";
    ASSERT_TRUE(resetRemoteRoot(Export, {{seedOnce("a.txt", "hello-from-daemon\n"), "a.txt"}, {seedOnce("b.txt", "second\n"), "b.txt"}}));

    auto Address = peer().address();
    ASSERT_TRUE(Address) << "could not resolve the peer address";
    Host = *Address;

    ASSERT_NO_FATAL_FAILURE(restartDaemonOn("tcp"));

    unmountAndUnload();
    ASSERT_TRUE(loadModule(modulePath())) << "loading the module failed";
    Loaded = true;

    Mountpoint = (localDir() / "kernel-mnt").string();
    std::filesystem::create_directories(Mountpoint);
  }

  void TearDown() override {
    if (Loaded) unmountAndUnload();
    stopDaemon();
  }

  void seed(const std::string &Name, const std::string &Body) {
    const auto Local = localDir() / Name;
    std::ofstream(Local, std::ios::binary | std::ios::trunc) << Body;
    seedRemote(Local, Export + "/" + Name);
  }

  std::string seedOnce(const std::string &Name, const std::string &Body) {
    const auto Local = localDir() / ("kernel-seed-" + Name);
    std::ofstream(Local, std::ios::binary | std::ios::trunc) << Body;
    const std::string Remote = remoteDir() + "/seed/kernel-" + Name;
    seedRemoteOnce(Local, Remote);
    return Remote;
  }

  ::testing::AssertionResult mountIt(const std::string &Options) {
    auto Mounted = mountFilesystem("railfs", "none", Mountpoint, Options);
    if (Mounted) {
      return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure() << "mount -o " << Options << ": " << Mounted.error().message();
  }

  bool mounted() { return isMountpoint(Mountpoint); }

  std::string defaultOptions() const { return "host=" + Host + ",export=/,port=" + std::to_string(kPort); }

  void unmountAndUnload() {
    if (!Mountpoint.empty()) {
      [[maybe_unused]] auto U = unmountFilesystem(Mountpoint);
    }

    [[maybe_unused]] auto R = unloadModule("railfs");
  }

  std::string kernelLog() {
    auto Log = readKernelLog();
    return Log ? *Log : std::string{};
  }

  // Match the peer's md5sum.
  std::string digestOf(const std::filesystem::path &P) { return digestFile(P); }

  std::string digestThrough(const std::string &Name) { return digestFile(Mountpoint + "/" + Name); }

  std::string firstLineFromPeer(const std::vector<std::string> &Argv) {
    auto OnPeer = peer().run(Argv);
    if (!OnPeer) return {};
    auto Line = OnPeer->readLine();
    return Line ? *Line : std::string{};
  }

  std::string listing(const std::string &Of) {
    std::string Joined;
    for (const auto &Name : listNames(Of)) {
      Joined += Name + "\n";
    }

    return Joined;
  }

  // Use the module counter; timing-based concurrency checks are flaky.
  int busiestConnections() {
    const std::string Stats = readWholeFile("/sys/kernel/debug/railfs/stats");
    const auto At = Stats.find("conns busy now ");
    if (At == std::string::npos) return -1;
    const auto Most = Stats.find("most ", At);
    if (Most == std::string::npos) return -1;
    return std::atoi(Stats.c_str() + Most + 5);
  }

  long cachedMiB() {
    const std::string Meminfo = readWholeFile("/proc/meminfo");
    const auto Line = Meminfo.find("\nCached:");
    const std::string Info = Line == std::string::npos ? std::string{} : Meminfo.substr(Line + 1, 64);
    const auto At = Info.find_first_of("0123456789");
    return At == std::string::npos ? -1 : std::atol(Info.c_str() + At) / 1024;
  }

  void forgetCounters() { [[maybe_unused]] auto R = writeWholeFile("/sys/kernel/debug/railfs/stats", "reset"); }

  double timeOf(const std::string &Script) {
    const auto Began = std::chrono::steady_clock::now();
    [[maybe_unused]] auto Ran = runLocal({"sh", "-c", Script});
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - Began).count();
  }

  void restartDaemonOnRdma() { restartDaemonOn("rdma"); }

  void restartDaemonOn(const std::string &Backend) {
    stopDaemon();
    auto Started = peer().run({serviceBinary().string(), "--serve", Export, "--port", std::to_string(kPort), "--backend", Backend});
    ASSERT_TRUE(Started) << "could not start raild on " << Backend;
    Daemon.emplace(std::move(*Started));
    ASSERT_TRUE(waitForListener(Host, kPort, std::chrono::seconds(10))) << "the daemon never started listening";
  }

  void stopDaemon() {
    Daemon.reset();
    stopPeerProcess("raild");
  }

  std::string Export;
  std::string Host;
  std::string Mountpoint;
  bool Loaded = false;
  std::optional<RemoteProcess> Daemon;
};

} // namespace rail::e2e
