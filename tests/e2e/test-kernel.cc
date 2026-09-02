#include "harness.h"
#include "local-process.h"
#include "privileged.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace rail::e2e {

namespace {

constexpr uint16_t kPort = 18719;

// The repo root, reached from a tool the harness already knows how to find.
std::filesystem::path repoRoot() { return serviceBinary().parent_path().parent_path().parent_path().parent_path(); }

std::filesystem::path helperPath() { return serviceBinary().parent_path().parent_path() / "mount.railfs" / "mount.railfs"; }

std::filesystem::path modulePath() {
  if (const char *Given = ::getenv("RAIL_KO")) return Given;
  return repoRoot() / "build" / "src" / "linux" / "railfs.ko";
}

bool ran(const std::vector<std::string> &Argv) {
  auto R = runLocal(Argv);
  return R && R->ExitStatus == 0;
}

std::string output(const std::vector<std::string> &Argv) {
  auto R = runLocal(Argv);
  return R ? R->Output : std::string{};
}

} // namespace

// Loading a module and mounting needs root, so these are opt-in: a suite that
// silently skipped would look the same as one that passed.
class Kernel : public ::testing::Test {
protected:
  void SetUp() override {
    if (!::getenv("RAIL_KERNEL_TESTS")) GTEST_SKIP() << "set RAIL_KERNEL_TESTS=1 to run these";
    if (!std::filesystem::exists(modulePath())) GTEST_SKIP() << "no module at " << modulePath() << "; run make in src/linux";
    ASSERT_TRUE(runningAsRoot()) << "the privileged suites need the binary run under sudo";

    Export = remoteDir() + "/kernel-export";
    removeRemoteRecursive(Export);
    ASSERT_TRUE(peer().makeDirectory(Export));

    auto Address = peer().address();
    ASSERT_TRUE(Address) << "could not resolve the peer address";
    Host = *Address;

    seed("a.txt", "hello-from-daemon\n");
    seed("b.txt", "second\n");

    stopDaemon();
    auto Started = peer().run({serviceBinary().string(), "--serve", Export, "--port", std::to_string(kPort), "--backend", "tcp"});
    ASSERT_TRUE(Started) << "could not start raild: " << Started.error().message();
    Daemon.emplace(std::move(*Started));
    ASSERT_TRUE(waitForListener(Host, kPort, std::chrono::seconds(10))) << "the daemon never started listening";

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

  // -i so mount never hands off to an installed /sbin/mount.railfs, which takes
  // a HOST:EXPORT spec and would make these tests depend on the host.
  ::testing::AssertionResult mountIt(const std::string &Options) {
    auto Mounted = mountFilesystem("railfs", "none", Mountpoint, Options);
    if (Mounted) {
      return ::testing::AssertionSuccess();
    }

    // The reason is the whole story when a mount fails, and a bare false makes
    // every one of these look alike.
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

  // md5, because the far side of the comparison is the peer's md5sum. The
  // project's own hash is xxhash and would never match it.
  std::string digestOf(const std::filesystem::path &P) { return digestFile(P); }

  std::string digestThrough(const std::string &Name) { return digestFile(Mountpoint + "/" + Name); }

  std::string listing(const std::string &Of) {
    std::string Joined;
    for (const auto &Name : listNames(Of)) {
      Joined += Name + "\n";
    }

    return Joined;
  }

  // The high-water mark of connections held at once, which the module counts
  // itself. Timing could only infer this, and inferring it from a hundred
  // milliseconds of wall clock is what made this test flaky.
  int busiestConnections() {
    const std::string Stats = readWholeFile("/sys/kernel/debug/railfs/stats");
    const auto At = Stats.find("conns busy now ");
    if (At == std::string::npos) return -1;
    const auto Most = Stats.find("most ", At);
    if (Most == std::string::npos) return -1;
    return std::atoi(Stats.c_str() + Most + 5);
  }

  // Cached in /proc/meminfo, in MiB, which is how a folio floor that ignores
  // the file size shows itself: the bytes are few and the folios are not.
  long cachedMiB() {
    const std::string Meminfo = readWholeFile("/proc/meminfo");
    const auto Line = Meminfo.find("\nCached:");
    const std::string Info = Line == std::string::npos ? std::string{} : Meminfo.substr(Line + 1, 64);
    const auto At = Info.find_first_of("0123456789");
    return At == std::string::npos ? -1 : std::atol(Info.c_str() + At) / 1024;
  }

  void forgetCounters() { [[maybe_unused]] auto R = writeWholeFile("/sys/kernel/debug/railfs/stats", "reset"); }

  // Wall time for a shell command, so a claim about parallelism rests on a
  // measurement rather than on the code looking concurrent.
  double timeOf(const std::string &Script) {
    const auto Began = std::chrono::steady_clock::now();
    [[maybe_unused]] auto Ran = runLocal({"sh", "-c", Script});
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - Began).count();
  }

  // The fabric tests need the daemon speaking rdma; the rest want tcp.
  void restartDaemonOnRdma() {
    stopDaemon();
    auto Started = peer().run({serviceBinary().string(), "--serve", Export, "--port", std::to_string(kPort), "--backend", "rdma"});
    ASSERT_TRUE(Started) << "could not start raild on rdma";
    Daemon.emplace(std::move(*Started));
    ASSERT_TRUE(waitForListener(Host, kPort, std::chrono::seconds(10))) << "the daemon never started listening";
  }

  void stopDaemon() {
    Daemon.reset();
    if (auto Killed = peer().run({"pkill", "-x", "raild"}); Killed) {
      [[maybe_unused]] auto Line = Killed->readLine();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  std::string Export;
  std::string Host;
  std::string Mountpoint;
  bool Loaded = false;
  std::optional<RemoteProcess> Daemon;
};

TEST_F(Kernel, MountsAndUnmounts) {
  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_TRUE(mounted());
  EXPECT_TRUE(unmountFilesystem(Mountpoint).has_value());
  EXPECT_FALSE(mounted());
}

TEST_F(Kernel, ListsWhatTheDaemonExports) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Listing = listing(Mountpoint);
  EXPECT_NE(Listing.find("a.txt"), std::string::npos) << Listing;
  EXPECT_NE(Listing.find("b.txt"), std::string::npos) << Listing;
}

TEST_F(Kernel, ReportsTheSizeTheDaemonGave) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  EXPECT_EQ(fileSize(Mountpoint + "/a.txt"), 18);
}

TEST_F(Kernel, NamesTheMountInProcMounts) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Mounts = readWholeFile("/proc/mounts");
  EXPECT_NE(Mounts.find("host=" + Host), std::string::npos) << Mounts;
  EXPECT_NE(Mounts.find("export=."), std::string::npos) << Mounts;
}

TEST_F(Kernel, ReadsAFileThroughTheKernel) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Body = readWholeFile(Mountpoint + "/a.txt");
  EXPECT_EQ(Body, "hello-from-daemon\n") << Body;
}

TEST_F(Kernel, ReadsAFileLargerThanOnePage) {
  // Three pages, so the fetch has to be repeated and reassembled rather than
  // answered by one round trip.
  const auto Local = makeFile("kernel-multipage.bin", 9000, 21);
  seedRemote(Local, Export + "/multi.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Wanted = digestOf(Local);
  EXPECT_FALSE(Wanted.empty());
  EXPECT_EQ(digestThrough("multi.bin"), Wanted);
}

// A page is 4096 here, and every one of these sits on or beside that boundary,
// where a fetch that rounds the wrong way shows up as silent corruption.
TEST_F(Kernel, ReadsAnEmptyFile) {
  const auto Local = makeFile("kernel-empty.bin", 0, 30);
  seedRemote(Local, Export + "/empty.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  EXPECT_EQ(fileSize(Mountpoint + "/empty.bin"), 0);
  EXPECT_EQ(readWholeFile(Mountpoint + "/empty.bin"), "");
}

TEST_F(Kernel, ReadsExactlyOnePage) {
  const auto Local = makeFile("kernel-onepage.bin", 4096, 31);
  seedRemote(Local, Export + "/onepage.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_EQ(digestThrough("onepage.bin"), digestOf(Local));
}

TEST_F(Kernel, ReadsOneByteMoreThanAPage) {
  const auto Local = makeFile("kernel-overpage.bin", 4097, 32);
  seedRemote(Local, Export + "/overpage.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_EQ(digestThrough("overpage.bin"), digestOf(Local));
}

TEST_F(Kernel, ReadsOneByteLessThanAPage) {
  const auto Local = makeFile("kernel-underpage.bin", 4095, 33);
  seedRemote(Local, Export + "/underpage.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_EQ(digestThrough("underpage.bin"), digestOf(Local));
}

TEST_F(Kernel, ReadingPastTheEndReturnsNothing) {
  const auto Local = makeFile("kernel-short.bin", 100, 34);
  seedRemote(Local, Export + "/short.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  // A read past the end must come back empty rather than with a page of zeros.
  const std::string Out = readRange(Mountpoint + "/short.bin", 200, 10);
  EXPECT_TRUE(Out.empty()) << "got " << Out.size() << " bytes past the end";
}

TEST_F(Kernel, AMissingNameIsNotFound) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  auto Ran = runLocal({"cat", Mountpoint + "/nothing-here.bin"});
  ASSERT_TRUE(Ran);
  EXPECT_NE(Ran->ExitStatus, 0);
  EXPECT_NE(Ran->Output.find("No such file"), std::string::npos) << Ran->Output;
}

TEST_F(Kernel, RefusesToUnloadWhileMounted) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  // .owner on the ops is what makes this refusal happen. Without it the module
  // unloads under a live mount and the next call enters freed code.
  EXPECT_FALSE(unloadModule("railfs").has_value());
  EXPECT_TRUE(mounted());
}

TEST_F(Kernel, TwoReadersOnOneMountBothGetTheirOwnBytes) {
  const auto A = makeFile("kernel-two-a.bin", 40000, 35);
  const auto B = makeFile("kernel-two-b.bin", 40000, 36);
  seedRemote(A, Export + "/two-a.bin");
  seedRemote(B, Export + "/two-b.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  // One connection serves both, so a reader taking the other's data frame
  // would show up here as a digest that belongs to the wrong file.
  // Per-test paths: fixed names under /tmp collide between runs and a stale
  // one left by a crashed run would be read as this run's answer.
  const std::string OutA = (localDir() / "two-a.txt").string();
  const std::string OutB = (localDir() / "two-b.txt").string();
  std::string GotA;
  std::string GotB;
  std::thread ReaderA([&] { GotA = digestThrough("two-a.bin"); });
  std::thread ReaderB([&] { GotB = digestThrough("two-b.bin"); });
  ReaderA.join();
  ReaderB.join();

  EXPECT_EQ(GotA, digestOf(A));
  EXPECT_EQ(GotB, digestOf(B));
}

TEST_F(Kernel, ReadFailsRatherThanHangsWhenTheDaemonGoesAway) {
  const auto Proof = makeFile("kernel-proof.bin", 20000, 37);
  const auto Cold = makeFile("kernel-gone.bin", 20000, 38);
  seedRemote(Proof, Export + "/proof.bin");
  seedRemote(Cold, Export + "/gone.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  // Prove the mount works on one file, then kill the daemon and read a file
  // that was never touched - reading the first one again would be served from
  // the client's page cache and never reach the network at all.
  ASSERT_FALSE(digestThrough("proof.bin").empty());

  stopDaemon();

  // The socket deadline is what turns this into an error. Without it the read
  // sleeps forever holding the channel, and the unmount blocks behind it.
  auto Ran = runLocal({"cat", Mountpoint + "/gone.bin"});
  ASSERT_TRUE(Ran) << "cat never returned";
  EXPECT_NE(Ran->ExitStatus, 0) << "read succeeded with no daemon: " << Ran->Output.substr(0, 80);
}

// Every inode carries the path it was found at, not just its last component.
// Asking the daemon about "." from inside a subdirectory would list the root.
TEST_F(Kernel, ListsASubdirectory) {
  const auto Local = makeFile("kernel-nested.bin", 700, 40);
  ASSERT_TRUE(peer().makeDirectory(Export + "/sub"));
  seedRemote(Local, Export + "/sub/nested.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Listing = listing(Mountpoint + "/sub");
  EXPECT_NE(Listing.find("nested.bin"), std::string::npos) << Listing;
  EXPECT_EQ(Listing.find("a.txt"), std::string::npos) << "the root leaked into the subdirectory: " << Listing;
}

TEST_F(Kernel, ReadsAFileTwoDirectoriesDown) {
  const auto Local = makeFile("kernel-deep.bin", 5000, 41);
  ASSERT_TRUE(peer().makeDirectory(Export + "/sub"));
  ASSERT_TRUE(peer().makeDirectory(Export + "/sub/deeper"));
  seedRemote(Local, Export + "/sub/deeper/deep.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_EQ(digestThrough("sub/deeper/deep.bin"), digestOf(Local));
}

TEST_F(Kernel, FindWalksTheWholeTree) {
  const auto One = makeFile("kernel-walk-one.bin", 10, 42);
  const auto Two = makeFile("kernel-walk-two.bin", 10, 43);
  ASSERT_TRUE(peer().makeDirectory(Export + "/walk"));
  seedRemote(One, Export + "/walk/one.bin");
  seedRemote(Two, Export + "/walk/two.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  // Walking recurses through readdir and lookup together, which is where a
  // path built from the wrong parent shows up as missing or duplicated.
  std::string Walked;
  for (const auto &Path : filesUnder(Mountpoint + "/walk")) {
    Walked += Path + "\n";
  }
  EXPECT_NE(Walked.find("walk/one.bin"), std::string::npos) << Walked;
  EXPECT_NE(Walked.find("walk/two.bin"), std::string::npos) << Walked;
}

// O_DIRECT asks that the storage not be cached, and the storage is the peer's,
// so the daemon answers it and this side reads as it always does. The open has
// to be accepted for that to happen at all, which is what these cover.
TEST_F(Kernel, ReadsWithDirectIo) {
  const auto Local = makeFile("kernel-direct.bin", 9000, 120);
  seedRemote(Local, Export + "/direct.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Got = digestDirect(Mountpoint + "/direct.bin", 4096);
  EXPECT_EQ(Got, digestOf(Local)) << "a direct read served different bytes";
}

TEST_F(Kernel, DirectAndBufferedReadsAgree) {
  const auto Local = makeFile("kernel-direct-agree.bin", 300000, 121);
  seedRemote(Local, Export + "/direct-agree.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Direct = digestDirect(Mountpoint + "/direct-agree.bin", 1u << 20);
  EXPECT_EQ(Direct, digestThrough("direct-agree.bin")) << "the two paths disagree";
}

TEST_F(Kernel, DirectReadStartsAtAnOffset) {
  const auto Local = makeFile("kernel-direct-offset.bin", 40000, 122);
  seedRemote(Local, Export + "/direct-offset.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  const std::string Tail = digestDirect(Mountpoint + "/direct-offset.bin", 1000, 7);
  const std::string Wanted = digestBytes(readRange(Local, 7000, 40000));
  EXPECT_EQ(Tail, Wanted) << "a direct read at an offset served the wrong range";
}

TEST_F(Kernel, WritesWithDirectIo) {
  const auto Local = makeFile("kernel-direct-write.bin", 64, 123);
  seedRemote(Local, Export + "/direct-write.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/direct-write.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));

  const auto Source = makeFile("kernel-direct-source.bin", 300000, 124);
  ASSERT_TRUE(copyInto(Source, Mountpoint + "/direct-write.bin", true));

  const std::string OnPeer = output({"ssh", peerHost(), "md5sum", Export + "/direct-write.bin"}).substr(0, 32);
  EXPECT_EQ(OnPeer, digestOf(Source)) << "a direct write did not reach the peer intact";
}

TEST_F(Kernel, WritesReachThePeer) {
  const auto Local = makeFile("kernel-writable.bin", 64, 50);
  seedRemote(Local, Export + "/writable.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/writable.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(writeWholeFile(Mountpoint + "/writable.bin", "written-by-the-kernel\n"));

  EXPECT_EQ(readWholeFile(Mountpoint + "/writable.bin"), "written-by-the-kernel\n");
}

TEST_F(Kernel, WritesOverTheFabric) {
  const auto Seed = makeFile("kernel-fabric-write.bin", 64, 90);
  seedRemote(Seed, Export + "/fabric-write.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/fabric-write.bin"}));

  restartDaemonOnRdma();
  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));

  const auto Source = makeFile("kernel-fabric-source.bin", 9000, 91);
  ASSERT_TRUE(copyInto(Source, Mountpoint + "/fabric-write.bin"));

  // The peer's own copy is the proof the bytes crossed the fabric rather than
  // stopping in the page cache on this side.
  const std::string OnPeer = output({"ssh", peerHost(), "md5sum", Export + "/fabric-write.bin"}).substr(0, 32);
  EXPECT_EQ(OnPeer, digestOf(Source));
}

TEST_F(Kernel, AShorterOverwriteLeavesNoTail) {
  const auto Local = makeFile("kernel-shrink.bin", 0, 51);
  seedRemote(Local, Export + "/shrink.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/shrink.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(writeWholeFile(Mountpoint + "/shrink.bin", "aaaaaaaaaaaaaaaaaaaaaaaaa\n"));

  // Without truncate the old bytes survive past the end of the new content,
  // and the mount reports a size that does not match what is on the peer.
  ASSERT_TRUE(writeWholeFile(Mountpoint + "/shrink.bin", "bb\n"));

  EXPECT_EQ(readWholeFile(Mountpoint + "/shrink.bin"), "bb\n");
  EXPECT_EQ(fileSize(Mountpoint + "/shrink.bin"), 3);
}

TEST_F(Kernel, TruncateShrinksTheFileOnThePeer) {
  const auto Local = makeFile("kernel-trunc.bin", 4000, 52);
  seedRemote(Local, Export + "/trunc.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/trunc.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(resizeTo(Mountpoint + "/trunc.bin", 10));

  EXPECT_EQ(fileSize(Mountpoint + "/trunc.bin"), 10);

  // The peer is the only place that can confirm it, since the mount would
  // happily report a size it never sent anywhere.
  auto OnPeer = peer().run({"stat", "-c", "%s", Export + "/trunc.bin"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(Line->substr(0, 2), "10");
}

TEST_F(Kernel, CreatesAFileOnThePeer) {
  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(createEmpty(Mountpoint + "/made.txt"));

  // The peer is the only witness that matters: the dcache would happily show a
  // file that never reached the daemon.
  EXPECT_TRUE(peer().exists(Export + "/made.txt").value_or(false));
}

TEST_F(Kernel, MakesAndRemovesADirectory) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  ASSERT_TRUE(std::filesystem::create_directory(Mountpoint + "/adir"));
  EXPECT_TRUE(peer().exists(Export + "/adir").value_or(false));

  ASSERT_TRUE(std::filesystem::remove(Mountpoint + "/adir"));
  EXPECT_FALSE(peer().exists(Export + "/adir").value_or(false));
}

TEST_F(Kernel, UnlinkRemovesItFromThePeer) {
  const auto Local = makeFile("kernel-doomed.bin", 32, 60);
  seedRemote(Local, Export + "/doomed.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(std::filesystem::remove(Mountpoint + "/doomed.bin"));

  EXPECT_FALSE(peer().exists(Export + "/doomed.bin").value_or(false));
}

TEST_F(Kernel, ARefusalArrivesAsItsOwnErrno) {
  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(std::filesystem::create_directory(Mountpoint + "/full"));
  ASSERT_TRUE(createEmpty(Mountpoint + "/full/inside.txt"));

  // Without the errno from the reply every refusal reaches the caller as EIO,
  // and a directory that merely is not empty looks like failing hardware.
  std::error_code Failed;
  std::filesystem::remove(Mountpoint + "/full", Failed);
  EXPECT_EQ(Failed, std::errc::directory_not_empty) << Failed.message();
}

TEST_F(Kernel, NamesTheConnectionCountItWasGiven) {
  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=4"));
  EXPECT_NE(readWholeFile("/proc/mounts").find("conns=4"), std::string::npos);
}

TEST_F(Kernel, ParallelReadersSpreadAcrossTheConnectionPool) {
  // Every operation holds a connection for its whole exchange, so a mount with
  // one is serial by construction. This is the only check that the pool is
  // actually being spread across.
  for (int I = 0; I < 4; I++) {
    const auto Local = makeFile("kernel-par-" + std::to_string(I) + ".bin", 24u << 20, 70 + I);
    seedRemote(Local, Export + "/par" + std::to_string(I) + ".bin");
  }

  const std::string Readers = "for i in 0 1 2 3; do dd if=" + Mountpoint + "/par$i.bin of=/dev/null bs=1M 2>/dev/null & done; wait";
  const std::string Cold = "sync; echo 1 > /proc/sys/vm/drop_caches";

  // This used to compare wall time against a 0.75 ratio over about a hundred
  // milliseconds of work, which flaked. The module counts how many connections
  // were held at once, so the property can be asserted rather than timed.
  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=1"));
  ASSERT_TRUE(dropCaches().has_value());
  forgetCounters();
  [[maybe_unused]] auto Serial = runLocal({"sh", "-c", Readers});
  const int Alone = busiestConnections();
  ASSERT_TRUE(unmountFilesystem(Mountpoint).has_value());

  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=4"));
  ASSERT_TRUE(dropCaches().has_value());
  forgetCounters();
  [[maybe_unused]] auto Parallel = runLocal({"sh", "-c", Readers});
  const int Together = busiestConnections();

  ASSERT_GE(Alone, 0) << "the module did not report its counters; is debugfs mounted";
  EXPECT_EQ(Alone, 1) << "a one-connection mount held " << Alone << " at once";
  EXPECT_GT(Together, 1) << "a four-connection mount never held more than " << Together << " at once";
}

TEST_F(Kernel, JoinsItsQueuePairToTheDaemons) {
  restartDaemonOnRdma();
  ASSERT_TRUE(clearKernelLog().has_value());
  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));

  // Reaching ready against a peer queue pair number, not its own, is what
  // separates a rail that exists from one that is connected. Rails are
  // numbered because a mount builds one per active port, so the first is what
  // every machine has and the second is what this hardware happens to offer.
  const std::string Log = kernelLog();
  EXPECT_NE(Log.find("rdma rail 0 on"), std::string::npos) << Log;
  EXPECT_NE(Log.find("rdma rail 0 up, local qp"), std::string::npos) << Log;
  EXPECT_EQ(Log.find("no rdma rail"), std::string::npos) << Log;
}

TEST_F(Kernel, ListsOverTheControlChannelWhileOnRdma) {
  restartDaemonOnRdma();
  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));

  // Listing never touches the data path, so it works on a rail that carries
  // no payload yet.
  EXPECT_NE(listing(Mountpoint).find("a.txt"), std::string::npos);
}

TEST_F(Kernel, ReadsOverTheFabric) {
  const auto Local = makeFile("kernel-fabric.bin", 9000, 80);
  restartDaemonOnRdma();
  seedRemote(Local, Export + "/fabric.bin");

  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));

  // Three pages, written straight into kernel memory by the peer's nic and
  // checked against the digest the daemon sent for them.
  EXPECT_EQ(digestThrough("fabric.bin"), digestOf(Local));
  EXPECT_EQ(kernelLog().find("did not match its digest"), std::string::npos);
}

TEST_F(Kernel, FabricReadsAreNotWaitingOnATimer) {
  const auto Local = makeFile("kernel-fabric-big.bin", 300000, 81);
  restartDaemonOnRdma();
  seedRemote(Local, Export + "/fabric-big.bin");

  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));

  // A clear-to-send without immediate data lands silently, and the peer only
  // notices when its own backstop fires a second later. Seventy pages at a
  // second each is a minute; this bounds it well below that without being
  // tight enough to flake.
  const double Took = timeOf("dd if=" + Mountpoint + "/fabric-big.bin of=/dev/null bs=4096 2>/dev/null");
  EXPECT_LT(Took, 5.0) << "300 KB took " << Took << "s over the fabric";
}

TEST_F(Kernel, ReadsTheSameBytesOverEitherTransport) {
  const auto Local = makeFile("kernel-either.bin", 5000, 82);

  restartDaemonOnRdma();
  seedRemote(Local, Export + "/either.bin");
  ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,conns=1"));
  const std::string OverFabric = digestThrough("either.bin");
  ASSERT_TRUE(unmountFilesystem(Mountpoint).has_value());

  stopDaemon();
  auto Started = peer().run({serviceBinary().string(), "--serve", Export, "--port", std::to_string(kPort), "--backend", "tcp"});
  ASSERT_TRUE(Started);
  Daemon.emplace(std::move(*Started));
  std::this_thread::sleep_for(std::chrono::milliseconds(600));

  ASSERT_TRUE(mountIt(defaultOptions()));
  EXPECT_EQ(OverFabric, digestThrough("either.bin"));
  EXPECT_EQ(OverFabric, digestOf(Local));
}

TEST_F(Kernel, ExportNamesASubdirectoryOfWhatTheDaemonServes) {
  const auto Local = makeFile("kernel-nested.bin", 700, 95);
  ASSERT_TRUE(peer().makeDirectory(Export + "/nested"));
  seedRemote(Local, Export + "/nested/inside.bin");

  // Relative, so it means a subdirectory of the served root. The daemon
  // refuses an absolute path outright, which is why one can only mean the root.
  ASSERT_TRUE(mountIt("host=" + Host + ",export=nested,port=" + std::to_string(kPort) + ",conns=1"));

  const std::string Listing = listing(Mountpoint);
  EXPECT_NE(Listing.find("inside.bin"), std::string::npos) << Listing;
  EXPECT_EQ(Listing.find("a.txt"), std::string::npos) << "the root leaked into a subdirectory mount: " << Listing;
  EXPECT_EQ(digestThrough("inside.bin"), digestOf(Local));
}

TEST_F(Kernel, WalksADirectoryWithoutListingItPerName) {
  ASSERT_TRUE(peer().makeDirectory(Export + "/wide"));
  for (int I = 0; I < 40; I++) {
    const auto Local = makeFile("kernel-wide-" + std::to_string(I) + ".bin", 8, 120 + I);
    seedRemote(Local, Export + "/wide/w" + std::to_string(I) + ".bin");
  }

  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=1"));

  // Resolving one name used to fetch the whole parent listing, so walking a
  // directory of n names cost n listings. A name near the end has to resolve
  // just like one near the start.
  const std::string Listing = listing(Mountpoint + "/wide");
  EXPECT_NE(Listing.find("w39.bin"), std::string::npos) << Listing;
  EXPECT_NE(Listing.find("w0.bin"), std::string::npos) << Listing;

  EXPECT_EQ(fileSize(Mountpoint + "/wide/w39.bin"), 8);
  EXPECT_LT(fileSize(Mountpoint + "/wide/absent.bin"), 0);
}

TEST_F(Kernel, AListingAnswersTheStatsThatFollow) {
  ASSERT_TRUE(peer().makeDirectory(Export + "/listed"));
  for (int I = 0; I < 5; I++) {
    const auto Local = makeFile("kernel-listed-" + std::to_string(I) + ".bin", 8, 150 + I);
    seedRemote(Local, Export + "/listed/l" + std::to_string(I) + ".bin");
  }

  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=1"));
  ASSERT_FALSE(listNames(Mountpoint + "/listed").empty());

  // A listing carries every entry's attributes, so the stats after it owe the
  // peer nothing. With the daemon gone, anything that still asks cannot answer.
  stopDaemon();

  EXPECT_EQ(fileSize(Mountpoint + "/listed/l0.bin"), 8);
  EXPECT_EQ(fileSize(Mountpoint + "/listed/l4.bin"), 8);
}

TEST_F(Kernel, AListingAndAStatAgreeOnTheInode) {
  ASSERT_TRUE(peer().makeDirectory(Export + "/numbered"));
  const auto Local = makeFile("kernel-numbered.bin", 8, 160);
  seedRemote(Local, Export + "/numbered/n.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  // What readdir itself numbered the entry, not what a stat says afterwards:
  // readdir used to hash the bare name and lookup the whole path, so one file
  // had two inode numbers depending on which call asked.
  const uint64_t Listed = inodeFromListing(Mountpoint + "/numbered", "n.bin");
  const uint64_t Stated = inodeOf(Mountpoint + "/numbered/n.bin");

  ASSERT_NE(Listed, 0u);
  ASSERT_NE(Stated, 0u);
  EXPECT_EQ(Listed, Stated);
}

TEST_F(Kernel, RenamesReachThePeer) {
  const auto Local = makeFile("kernel-rename.bin", 400, 130);
  seedRemote(Local, Export + "/before.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "777", Export}));

  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=1"));
  ASSERT_TRUE(renameTo(Mountpoint + "/before.bin", Mountpoint + "/after.bin"));

  const std::string OnPeer = output({"ssh", peerHost(), "ls", Export});
  EXPECT_NE(OnPeer.find("after.bin"), std::string::npos) << OnPeer;
  EXPECT_EQ(OnPeer.find("before.bin"), std::string::npos) << OnPeer;

  // The inode remembers its own name, so a read after the move has to use the
  // new one rather than the name it was created with.
  EXPECT_EQ(digestThrough("after.bin"), digestOf(Local));
}

TEST_F(Kernel, RenameReplacesAnExistingName) {
  const auto Old = makeFile("kernel-replace-old.bin", 100, 131);
  const auto New = makeFile("kernel-replace-new.bin", 200, 132);
  seedRemote(Old, Export + "/target.bin");
  seedRemote(New, Export + "/source.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "777", Export}));

  ASSERT_TRUE(mountIt(defaultOptions() + ",conns=1"));
  ASSERT_TRUE(renameTo(Mountpoint + "/source.bin", Mountpoint + "/target.bin"));

  EXPECT_EQ(digestThrough("target.bin"), digestOf(New));
}

TEST_F(Kernel, ReportsThePeersFreeSpace) {
  ASSERT_TRUE(mountIt(defaultOptions()));

  // Blocks of zero is what a mount that never asked the peer reports, and df
  // then prints a filesystem with no room at all.
  EXPECT_GT(freeBlocks(Mountpoint), 0u);
}

TEST_F(Kernel, MountsWithoutARailWhenNotAsked) {
  ASSERT_TRUE(clearKernelLog().has_value());
  ASSERT_TRUE(mountIt(defaultOptions()));

  // The fabric costs a queue pair and a pinned ring, so a mount that did not
  // ask for one should not be paying for it.
  EXPECT_EQ(kernelLog().find("rdma rail on"), std::string::npos);
}

TEST_F(Kernel, RefusesAnOptionItDoesNotKnow) {
  EXPECT_FALSE(mountIt(defaultOptions() + ",bogus=1"));
  EXPECT_FALSE(mounted());
}

TEST_F(Kernel, RefusesToMountWhenNothingIsServing) {
  stopDaemon();
  EXPECT_FALSE(mountIt(defaultOptions()));
  EXPECT_FALSE(mounted());
}

// Relative, because an absolute export names the daemon's own root whatever
// follows the slash, and would pass this check by never being looked up.
TEST_F(Kernel, RefusesAnExportThePeerDoesNotHave) {
  EXPECT_FALSE(mountIt("host=" + Host + ",export=no-such-directory,port=" + std::to_string(kPort)));
  EXPECT_FALSE(mounted());
}

TEST_F(Kernel, RefusesAnExportThatIsNotADirectory) {
  const auto Local = makeFile("kernel-not-a-dir.bin", 64, 141);
  seedRemote(Local, Export + "/not-a-dir.bin");

  EXPECT_FALSE(mountIt("host=" + Host + ",export=not-a-dir.bin,port=" + std::to_string(kPort)));
  EXPECT_FALSE(mounted());
}

TEST_F(Kernel, ChmodReachesThePeer) {
  const auto Local = makeFile("kernel-mode.bin", 64, 140);
  seedRemote(Local, Export + "/mode.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(setMode(Mountpoint + "/mode.bin", 0640));

  // The mount would report a mode it kept to itself, so the peer is the only
  // witness that the chmod went anywhere.
  auto OnPeer = peer().run({"stat", "-c", "%a", Export + "/mode.bin"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(*Line, "640");
}

TEST_F(Kernel, TouchSetsTheTimeOnThePeer) {
  const auto Local = makeFile("kernel-mtime.bin", 64, 141);
  seedRemote(Local, Export + "/mtime.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(setModifiedTime(Mountpoint + "/mtime.bin", 1600000000));

  auto OnPeer = peer().run({"stat", "-c", "%Y", Export + "/mtime.bin"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(*Line, "1600000000");
}

TEST_F(Kernel, ReportsTheTimeThePeerHas) {
  const auto Local = makeFile("kernel-aged.bin", 64, 142);
  seedRemote(Local, Export + "/aged.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "touch", "-d", "@1500000000", Export + "/aged.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));

  // Without the mtime off the wire every file is dated to the moment the mount
  // first saw it, and a backup tool reads the whole tree as new.
  EXPECT_EQ(modifiedTime(Mountpoint + "/aged.bin"), 1500000000);
}

TEST_F(Kernel, SymlinkReachesThePeer) {
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "777", Export}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(makeSymlink("a.txt", Mountpoint + "/points.txt"));

  auto OnPeer = peer().run({"readlink", Export + "/points.txt"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(*Line, "a.txt");
}

TEST_F(Kernel, ReadsALinkThePeerHas) {
  ASSERT_TRUE(ran({"ssh", peerHost(), "ln", "-s", "a.txt", Export + "/seeded-link.txt"}));

  ASSERT_TRUE(mountIt(defaultOptions()));

  EXPECT_EQ(linkTarget(Mountpoint + "/seeded-link.txt"), "a.txt");

  // Reading the link is not the same as walking it: the second one puts the
  // target back through lookup and reads the file it names.
  EXPECT_EQ(readWholeFile(Mountpoint + "/seeded-link.txt"), "hello-from-daemon\n");
}

TEST_F(Kernel, HardLinkGivesOneFileASecondName) {
  const auto Local = makeFile("kernel-linked.bin", 4096, 143);
  seedRemote(Local, Export + "/linked.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "777", Export}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(makeHardLink(Mountpoint + "/linked.bin", Mountpoint + "/second.bin"));

  auto OnPeer = peer().run({"stat", "-c", "%h", Export + "/linked.bin"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(*Line, "2");

  // A second name for the same bytes, not a copy of them.
  EXPECT_EQ(digestThrough("second.bin"), digestOf(Local));
}

TEST_F(Kernel, ASecondNameOutlivesTheFirst) {
  const auto Local = makeFile("kernel-outlives.bin", 512, 146);
  seedRemote(Local, Export + "/first.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "777", Export}));

  ASSERT_TRUE(mountIt(defaultOptions()));
  ASSERT_TRUE(makeHardLink(Mountpoint + "/first.bin", Mountpoint + "/other.bin"));
  ASSERT_TRUE(std::filesystem::remove(Mountpoint + "/first.bin"));

  // Sharing one inode between both names made this EIO: an inode remembers the
  // one path its reads ask for, and that path had just been unlinked.
  EXPECT_EQ(digestThrough("other.bin"), digestOf(Local));

  // And zeroing that inode's link count made the surviving name look deleted.
  auto OnPeer = peer().run({"stat", "-c", "%h", Export + "/other.bin"});
  ASSERT_TRUE(OnPeer);
  auto Line = OnPeer->readLine();
  ASSERT_TRUE(Line);
  EXPECT_EQ(*Line, "1");
}

TEST_F(Kernel, MapsAFileIntoMemory) {
  const auto Local = makeFile("kernel-mapped.bin", 300000, 144);
  seedRemote(Local, Export + "/mapped.bin");

  ASSERT_TRUE(mountIt(defaultOptions()));

  EXPECT_EQ(digestMapped(Mountpoint + "/mapped.bin"), digestOf(Local));
}

// A symlink cannot be an export: the peer lstats it, and will not list through
// one either, so the mount could only ever be empty.
TEST_F(Kernel, RefusesAnAbsoluteExport) {
  EXPECT_FALSE(mountIt("host=" + Host + ",export=" + Export + ",port=" + std::to_string(kPort)));
  EXPECT_FALSE(mounted());
}

// Asserted on the syscall: glibc turns ENOENT from getdents64 into end of
// directory, so readdir, ls and every tool built on them read a lost export as
// an empty one however the filesystem answers.
TEST_F(Kernel, ReportsAnExportThePeerLost) {
  const std::string Gone = Export + "/losable";
  ASSERT_TRUE(peer().makeDirectory(Gone));
  ASSERT_TRUE(mountIt("host=" + Host + ",export=losable,port=" + std::to_string(kPort) + ",actimeo=1"));

  removeRemoteRecursive(Gone);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  const int Fd = ::open(Mountpoint.c_str(), O_RDONLY | O_DIRECTORY);
  ASSERT_GE(Fd, 0);

  char Buffer[32768];
  errno = 0;
  const long Got = ::syscall(SYS_getdents64, Fd, Buffer, sizeof(Buffer));
  const int Failed = errno;
  ::close(Fd);

  EXPECT_EQ(Got, -1) << "getdents64 returned " << Got << " for an export the peer lost";
  EXPECT_EQ(Failed, ENOENT) << "errno was " << Failed;
}

TEST_F(Kernel, RefusesAnExportThatIsASymlink) {
  ASSERT_TRUE(peer().makeDirectory(Export + "/real-dir"));
  ASSERT_TRUE(ran({"ssh", peerHost(), "ln", "-sfn", Export + "/real-dir", Export + "/link-dir"}));

  EXPECT_FALSE(mountIt("host=" + Host + ",export=link-dir,port=" + std::to_string(kPort)));
  EXPECT_FALSE(mounted());
}

class MountHelper : public Kernel {
protected:
  // The failures below are all decided before mount(2), so they need no module
  // and no daemon, but the fixture gives them a mountpoint and a peer.
  std::string helperSays(const std::vector<std::string> &Args) {
    std::vector<std::string> Argv{helperPath().string()};
    Argv.insert(Argv.end(), Args.begin(), Args.end());
    auto R = runLocal(Argv);
    return R ? R->Output : std::string{};
  }
};

TEST_F(MountHelper, RefusesASpecWithoutAColon) { EXPECT_NE(helperSays({Host, Mountpoint}).find("expected HOST:EXPORT"), std::string::npos); }

TEST_F(MountHelper, RefusesAnIpv6Spec) { EXPECT_NE(helperSays({"fe80::1:/models", Mountpoint}).find("IPv6"), std::string::npos); }

TEST_F(MountHelper, RefusesAHostItCannotResolve) {
  EXPECT_NE(helperSays({"no-such-host.invalid:models", Mountpoint}).find("cannot resolve"), std::string::npos);
}

TEST_F(MountHelper, RefusesAMountpointThatIsMissing) {
  EXPECT_NE(helperSays({Host + ":models", "/no/such/mountpoint"}).find("/no/such/mountpoint"), std::string::npos);
}

TEST_F(MountHelper, RefusesAMountpointThatIsNotADirectory) {
  EXPECT_NE(helperSays({Host + ":models", "/etc/hostname"}).find("is not a directory"), std::string::npos);
}

// The module has no resolver, so a name has to become an address here.
TEST_F(MountHelper, ResolvesANameBeforeHandingItToTheKernel) {
  EXPECT_TRUE(ran({helperPath().string(), "localhost:models", Mountpoint, "-f"}));
}

TEST_F(MountHelper, MountsThroughTheSpec) {
  ASSERT_TRUE(ran({helperPath().string(), Host + ":.", Mountpoint, "-o", "port=" + std::to_string(kPort)}));
  EXPECT_TRUE(mounted());
  EXPECT_NE(listing(Mountpoint).find("a.txt"), std::string::npos);
}

TEST_F(Kernel, DoesNotGiveASmallFileAFolioTheSizeOfTheFloor) {
  const std::string Many = Export + "/many";
  ASSERT_TRUE(peer().makeDirectory(Many));
  ASSERT_TRUE(ran({"ssh", peerHost(), "for i in $(seq 1 500); do head -c 1024 /dev/zero > " + Many + "/f$i; done"}));

  ASSERT_TRUE(mountIt("host=" + Host + ",export=many,port=" + std::to_string(kPort) + ",minfolio=262144"));
  ASSERT_TRUE(dropCaches().has_value());

  const long Before = cachedMiB();
  ASSERT_GE(Before, 0);
  [[maybe_unused]] auto Read = runLocal({"sh", "-c", "cat " + Mountpoint + "/* > /dev/null"});
  const long Grew = cachedMiB() - Before;

  // 500 KiB of data. Bounded it costs a few MiB; a floor per file would cost
  // 125, so the bar is set far from both.
  EXPECT_LT(Grew, 40) << "cache grew " << Grew << " MiB for 500 KiB of files";
}

TEST_F(Kernel, ClampsAFolioFloorLargerThanTheFetch) {
  ASSERT_TRUE(mountIt(defaultOptions() + ",fetch=262144,minfolio=1048576"));
  EXPECT_EQ(readWholeFile(Mountpoint + "/a.txt"), "hello-from-daemon\n");
}

// A write shorter than one folio leaves the rest of it holding nothing the file
// owns, so writeback has to stop at i_size rather than send the whole folio.
TEST_F(Kernel, WritesAFileFarBelowTheFolioFloor) {
  ASSERT_TRUE(mountIt(defaultOptions() + ",minfolio=262144"));

  const std::string Body = "written-through-a-large-folio\n";
  ASSERT_TRUE(writeWholeFile(createEmpty(Mountpoint + "/short.bin") ? Mountpoint + "/short.bin" : "", Body));
  ASSERT_TRUE(syncFilesystem(Mountpoint));

  EXPECT_EQ(output({"ssh", peerHost(), "cat", Export + "/short.bin"}), Body);
  EXPECT_EQ(output({"ssh", peerHost(), "stat", "-c", "%s", Export + "/short.bin"}).substr(0, 2), std::to_string(Body.size()).substr(0, 2));
}

TEST_F(Kernel, RefusesAMinfolioThatIsNotAPowerOfTwo) {
  EXPECT_FALSE(mountIt(defaultOptions() + ",minfolio=3000"));
  EXPECT_FALSE(mounted());
}

// The floor is bounded by the file, so a file far below it must still be given
// folios it can fill rather than one the size of the floor.
TEST_F(Kernel, ReadsAFileFarBelowTheFolioFloor) {
  const auto Local = makeFile("kernel-tiny.bin", 300, 146);
  seedRemote(Local, Export + "/tiny.bin");

  ASSERT_TRUE(mountIt(defaultOptions() + ",minfolio=262144"));
  EXPECT_EQ(digestOf(Mountpoint + "/tiny.bin"), digestOf(Local));
}

TEST_F(Kernel, MapsAFileFarBelowTheFolioFloor) {
  const auto Local = makeFile("kernel-tinymap.bin", 300, 147);
  seedRemote(Local, Export + "/tinymap.bin");

  ASSERT_TRUE(mountIt(defaultOptions() + ",minfolio=262144"));

  const int Fd = ::open((Mountpoint + "/tinymap.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0) << "open failed";

  struct stat St{};
  ASSERT_EQ(::fstat(Fd, &St), 0);
  ASSERT_EQ(St.st_size, 300);

  void *At = ::mmap(nullptr, (size_t)St.st_size, PROT_READ, MAP_PRIVATE, Fd, 0);
  ASSERT_NE(At, MAP_FAILED);

  std::ifstream In(Local, std::ios::binary);
  const std::string Wanted{std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()};
  EXPECT_EQ(std::string((const char *)At, (size_t)St.st_size), Wanted);

  ::munmap(At, (size_t)St.st_size);
  ::close(Fd);
}

TEST_F(Kernel, AMappedWriteReachesThePeer) {
  const auto Local = makeFile("kernel-mapwrite.bin", 8192, 145);
  seedRemote(Local, Export + "/mapwrite.bin");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/mapwrite.bin"}));

  ASSERT_TRUE(mountIt(defaultOptions()));

  // A store through a mapping dirties the folio without ever calling write, so
  // this fails unless writeback picks it up on msync.
  const int Fd = ::open((Mountpoint + "/mapwrite.bin").c_str(), O_RDWR);
  ASSERT_GE(Fd, 0);

  void *At = ::mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  ASSERT_NE(At, MAP_FAILED);

  std::memcpy(At, "MAPPD", 5);
  ASSERT_EQ(::msync(At, 8192, MS_SYNC), 0);
  ::munmap(At, 8192);
  ::close(Fd);

  // Not peer().run: readLine waits for a newline and five bytes carry none.
  EXPECT_EQ(output({"ssh", peerHost(), "head", "-c", "5", Export + "/mapwrite.bin"}), "MAPPD");
}

TEST_F(Kernel, LeavesNoKernelComplaintBehind) {
  ASSERT_TRUE(clearKernelLog().has_value());

  ASSERT_TRUE(mountIt(defaultOptions()));
  [[maybe_unused]] auto Listed = runLocal({"ls", Mountpoint});
  ASSERT_TRUE(unmountFilesystem(Mountpoint).has_value());

  const std::string Log = kernelLog();
  EXPECT_EQ(Log.find("Busy inodes"), std::string::npos) << Log;
  EXPECT_EQ(Log.find("WARNING"), std::string::npos) << Log;
  EXPECT_EQ(Log.find("BUG"), std::string::npos) << Log;
}

} // namespace rail::e2e
