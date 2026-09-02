#include "harness.h"

#include "rail/fuse/inodes.h"

#include "local-process.h"
#include "privileged.h"
#include "remote-host.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rail::e2e {

namespace {

bool mounted(const std::filesystem::path &At) {
  std::ifstream In("/proc/self/mountinfo");
  for (std::string Line; std::getline(In, Line);)
    if (Line.find(" " + At.string() + " ") != std::string::npos) return true;
  return false;
}

bool waitForMount(const std::filesystem::path &At, std::chrono::seconds Limit = std::chrono::seconds(30)) {
  const auto Deadline = std::chrono::steady_clock::now() + Limit;
  while (std::chrono::steady_clock::now() < Deadline) {
    if (mounted(At)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void abortFuseConnection(dev_t Device) {
  const std::string Abort = "/sys/fs/fuse/connections/" + std::to_string(::minor(Device)) + "/abort";
  [[maybe_unused]] const bool Ended = writeWholeFile(Abort, "1");
}

// A lazy unmount detaches the mount but leaves the connection alive while
// anything still holds it, and the daemon then never sees its session end.
void unmount(const std::filesystem::path &At) {
  if (!mounted(At)) return;

  struct stat Info{};
  const bool Known = ::stat(At.c_str(), &Info) == 0;

  [[maybe_unused]] auto Ignored = runLocal({"fusermount3", "-u", "-z", At.string()});

  if (Known) abortFuseConnection(Info.st_dev);
}

class Mounted {
public:
  Mounted(std::vector<std::string> Argv, std::filesystem::path At) : At(std::move(At)) {
    std::filesystem::create_directories(this->At);
    auto Started = BackgroundProcess::start(Argv);
    if (!Started) {
      ADD_FAILURE() << "cannot start railfs: " << Started.error().message();
      return;
    }
    Child.emplace(std::move(*Started));
    Ready = waitForMount(this->At);
  }

  Mounted(const Mounted &) = delete;
  Mounted &operator=(const Mounted &) = delete;

  ~Mounted() {
    [[maybe_unused]] const std::string Ignored = drain();
    std::error_code Ec;
    std::filesystem::remove_all(At, Ec);
  }

  // The mount prints its tally as it leaves, after every shard has been joined,
  // so a fixed grace is a race the daemon loses once it has shards enough.
  // Unmounting ends it; wait() is what bounds the wait.
  std::string drain() {
    unmount(At);
    if (!Child) return {};
    auto Ended = Child->wait();
    Child.reset();
    return Ended ? Ended->Output : std::string{};
  }

  bool ready() const { return Ready; }
  const std::filesystem::path &at() const { return At; }

private:
  std::filesystem::path At;
  std::optional<BackgroundProcess> Child;
  bool Ready = false;
};

std::filesystem::path mountPointFor(const std::string &Name) { return localDir() / ("mnt-" + Name); }

// A run that was killed leaves its mount attached, and the next run then waits
// out the whole mount timeout against a directory nothing is serving. Detach
// anything left under the test directory before trusting a fresh mount.
// Starting the daemon does not mean it is listening yet, and the mount only
// reconnects on the read that needs it, so a read straight after a restart can
// still fail. Give it the same grace the nfs suite gives its probe.
bool digestRecovers(const std::filesystem::path &Through, const std::string &Want) {
  std::string Last;
  for (int Attempt = 0; Attempt < 40; Attempt++) {
    Last = localDigest(Through);
    if (Last == Want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  ADD_FAILURE() << "never matched: want " << Want << " got " << Last << " mounted=" << mounted(Through.parent_path())
                << " exists=" << std::filesystem::exists(Through);
  return false;
}

void sweepStaleMounts() {
  std::ifstream In("/proc/self/mountinfo");
  const std::string Under = localDir().string() + "/mnt-";
  for (std::string Line; std::getline(In, Line);) {
    const auto At = Line.find(" " + Under);
    if (At == std::string::npos) continue;
    const auto From = At + 1;
    const auto To = Line.find(' ', From);
    if (To == std::string::npos) continue;
    [[maybe_unused]] auto Ignored = runLocal({"fusermount3", "-u", "-z", Line.substr(From, To - From)});
  }
}

constexpr size_t kPatternSize = 1u << 20;

std::vector<std::byte> nullPattern() {
  std::vector<std::byte> V(kPatternSize);
  for (size_t I = 0; I < V.size(); I++) V[I] = static_cast<std::byte>((I * 31 + 7) & 0xff);
  return V;
}

} // namespace

TEST(Inodes, ReleasesOnlyWhatNobodyLookedUp) {
  fuse::Inodes Known(".");

  const fuse::Ino Fresh = Known.reserve(fuse::kRootIno, "a.bin");
  ASSERT_TRUE(Known.known(Fresh));
  Known.release(fuse::kRootIno, "a.bin");
  EXPECT_FALSE(Known.known(Fresh)) << "a reservation nobody took was kept";

  const fuse::Ino Held = Known.insert(fuse::kRootIno, "b.bin");
  Known.release(fuse::kRootIno, "b.bin");
  EXPECT_TRUE(Known.known(Held)) << "release dropped an inode the kernel is holding";

  const fuse::Ino Reserved = Known.reserve(fuse::kRootIno, "c.bin");
  const fuse::Ino Inserted = Known.insert(fuse::kRootIno, "c.bin");
  EXPECT_EQ(Reserved, Inserted) << "create routed to one inode and then made another";
  Known.release(fuse::kRootIno, "c.bin");
  EXPECT_TRUE(Known.known(Inserted)) << "release dropped an inode a create had taken";
}

TEST(Inodes, ListedNamesDoNotGrowWithoutBound) {
  fuse::Inodes Known(".");

  for (int I = 0; I < 200000; I++) Known.reserve(fuse::kRootIno, "f" + std::to_string(I));

  EXPECT_LE(Known.size(), 100000u) << "a listing kept every name it ever saw: " << Known.size() << " inodes";
}

TEST(Inodes, AListingDoesNotEvictAnAncestor) {
  fuse::Inodes Known(".");

  const fuse::Ino Dir = Known.reserve(fuse::kRootIno, "dir");
  const fuse::Ino Child = Known.insert(Dir, "child.bin");
  const std::string Want = Known.path(Child);
  ASSERT_FALSE(Want.empty());

  for (int I = 0; I < 200000; I++) Known.reserve(fuse::kRootIno, "f" + std::to_string(I));

  EXPECT_TRUE(Known.known(Dir)) << "the parent of a live name was evicted";
  EXPECT_EQ(Known.path(Child), Want) << "a live name lost its path when its parent was evicted";
}

// One rule, checked on every tool that takes a spec: mount.railfs enforces it in
// the kernel, and these two through rail::exportRoot. A tool that stopped
// refusing would be naming the whole export while looking like a subtree.
TEST(ExportRoot, TheFuseMountRefusesAnAbsoluteExport) {
  auto Ran = runLocal({mountBinary().string(), "10.0.0.1:/somewhere", "/nonexistent/mountpoint", "--port", "1"});
  ASSERT_TRUE(Ran);
  EXPECT_NE(Ran->Output.find("export is relative"), std::string::npos) << Ran->Output;
}

TEST(ExportRoot, TheNfsMountRefusesAnAbsoluteExport) {
  auto Ran = runLocal({exportBinary().string(), "--serve", "10.0.0.1:/somewhere", "--port", "1"});
  ASSERT_TRUE(Ran);
  EXPECT_NE(Ran->Output.find("export is relative"), std::string::npos) << Ran->Output;
}

// A mountpoint that cannot exist, so the spec is judged and nothing reaches
// the stage where a mount would be made or a peer dialled.
TEST(ExportRoot, ASubtreeIsStillAccepted) {
  auto Ran = runLocal({mountBinary().string(), "10.0.0.1:models", "/nonexistent/mountpoint", "--port", "1"});
  ASSERT_TRUE(Ran);
  EXPECT_EQ(Ran->Output.find("export is relative"), std::string::npos) << Ran->Output;
}

TEST(Fuse, MountsAndUnmounts) {
  const auto At = mountPointFor("empty");
  Mounted M({mountBinary().string(), "--null", "0", At.string()}, At);
  ASSERT_TRUE(M.ready()) << "railfs never mounted at " << At;

  struct ::stat S{};
  ASSERT_EQ(::stat(At.c_str(), &S), 0) << "stat on the mount point failed";
  EXPECT_TRUE(S_ISDIR(S.st_mode));
}

TEST(Fuse, NullMountReportsTheFileSize) {
  const uint64_t Size = 64u << 20;
  const auto At = mountPointFor("null-size");
  Mounted M({mountBinary().string(), "--null", std::to_string(Size), At.string()}, At);
  ASSERT_TRUE(M.ready());

  struct ::stat S{};
  ASSERT_EQ(::stat((At / "data.bin").c_str(), &S), 0);
  EXPECT_EQ(static_cast<uint64_t>(S.st_size), Size);
  EXPECT_TRUE(S_ISREG(S.st_mode));
}

TEST(Fuse, NullMountListsTheFile) {
  const auto At = mountPointFor("null-list");
  Mounted M({mountBinary().string(), "--null", std::to_string(4096), At.string()}, At);
  ASSERT_TRUE(M.ready());

  bool Found = false;
  for (const auto &E : std::filesystem::directory_iterator(At))
    if (E.path().filename() == "data.bin") Found = true;
  EXPECT_TRUE(Found);
}

TEST(Fuse, NullMountServesThePattern) {
  const uint64_t Size = 64u << 20;
  const auto At = mountPointFor("null-read");
  Mounted M({mountBinary().string(), "--null", std::to_string(Size), At.string()}, At);
  ASSERT_TRUE(M.ready());

  std::ifstream In(At / "data.bin", std::ios::binary);
  ASSERT_TRUE(In.good());

  const auto Want = nullPattern();
  std::vector<std::byte> Got(kPatternSize);
  uint64_t Read = 0;
  while (Read < Size) {
    In.read(reinterpret_cast<char *>(Got.data()), static_cast<std::streamsize>(Got.size()));
    const auto N = static_cast<size_t>(In.gcount());
    ASSERT_GT(N, 0u) << "short read at offset " << Read;
    for (size_t I = 0; I < N; I++) ASSERT_EQ(Got[I], Want[(Read + I) % kPatternSize]) << "byte " << (Read + I) << " differs";
    Read += N;
  }
  EXPECT_EQ(Read, Size);
}

namespace {

uint16_t fuseServicePortFor(const std::string &Backend) { return Backend == "tcp" ? 18741 : 18742; }

} // namespace

class Mount : public BackendTest {
protected:
  void SetUp() override {
    Root = remoteDir() + "/fuse-root";
    removeRemoteRecursive(Root);
    ASSERT_TRUE(peer().makeDirectory(Root));
    ASSERT_TRUE(peer().makeDirectory(Root + "/sub"));

    Alpha = makeFile("fuse-alpha.bin", 8u << 20, 21);
    seedRemote(Alpha, Root + "/alpha.bin");
    Nested = makeFile("fuse-nested.bin", 2048, 22);
    seedRemote(Nested, Root + "/sub/nested.bin");
    Big = makeFile("fuse-big.bin", 6u << 20, 23);
    seedRemote(Big, Root + "/big.bin");

    auto Address = peer().address();
    ASSERT_TRUE(Address) << "could not resolve the peer address";
    Host = *Address;

    At = mountPointFor(GetParam());
    stopEverything();
    startDaemon();
    mountWith({});
  }

  void TearDown() override { stopEverything(); }

  void startDaemon() {
    auto Served =
        peer().run({serviceBinary().string(), "--serve", Root, "--port", std::to_string(fuseServicePortFor(GetParam())), "--backend", GetParam()});
    ASSERT_TRUE(Served) << "could not start raild on the peer: " << Served.error().message();
    Daemon.emplace(std::move(*Served));
    ASSERT_TRUE(waitForListener(Host, fuseServicePortFor(GetParam()))) << "raild never started listening on the peer";
  }

  void killDaemon() {
    if (auto Killed = peer().run({"pkill", "-9", "-x", "raild"}); Killed) {
      [[maybe_unused]] auto Line = Killed->readLine();
    }
    Daemon.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  void stopDaemon() {
    Daemon.reset();
    if (auto Killed = peer().run({"pkill", "-x", "raild"}); Killed) {
      [[maybe_unused]] auto Line = Killed->readLine();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  void stopEverything() {
    Live.reset();
    sweepStaleMounts();
    [[maybe_unused]] auto Killed = runLocal({"pkill", "-x", "mount.railfuse"});
    stopDaemon();
  }

  void mountWith(std::vector<std::string> Extra, std::vector<std::string> Env = {}) {
    std::vector<std::string> Argv;
    if (!Env.empty()) {
      Argv.push_back("env");
      Argv.insert(Argv.end(), Env.begin(), Env.end());
    }
    Argv.push_back(mountBinary().string());
    for (const std::string &One :
         {Host, At.string(), std::string("--port"), std::to_string(fuseServicePortFor(GetParam())), std::string("--backend"), GetParam()})
      Argv.push_back(One);
    Argv.insert(Argv.end(), Extra.begin(), Extra.end());
    Live.emplace(std::move(Argv), At);
    ASSERT_TRUE(Live->ready()) << "railfs never mounted at " << At;
  }

  void remountWith(std::vector<std::string> Extra, std::vector<std::string> Env = {}) {
    Live.reset();
    [[maybe_unused]] auto Killed = runLocal({"pkill", "-x", "mount.railfuse"});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    mountWith(std::move(Extra), std::move(Env));
  }

  // "railfs: N pages landed direct, M copied"
  static std::pair<uint64_t, uint64_t> pagesFrom(const std::string &Log) {
    const size_t At = Log.rfind("pages landed direct");
    if (At == std::string::npos) return {0, 0};
    const size_t Line = Log.rfind("railfs: ", At);
    if (Line == std::string::npos) return {0, 0};
    unsigned long long Direct = 0;
    unsigned long long Copied = 0;
    if (std::sscanf(Log.c_str() + Line, "railfs: %llu pages landed direct, %llu copied", &Direct, &Copied) != 2) return {0, 0};
    return {Direct, Copied};
  }

  // One writer per GPU on a node, which is the shape a checkpoint arrives in.
  static constexpr int kRanks = 16;

  // A second mount of the same export, so the daemon sees two independent
  // clients rather than one. Two nodes checkpointing at once look like this to
  // the server, without needing a third machine.
  std::unique_ptr<Mounted> secondMount(std::vector<std::string> Extra) {
    const auto Where = mountPointFor(GetParam() + "-second");
    std::vector<std::string>
        Argv{mountBinary().string(), Host, Where.string(), "--port", std::to_string(fuseServicePortFor(GetParam())), "--backend", GetParam()};
    Argv.insert(Argv.end(), Extra.begin(), Extra.end());
    return std::make_unique<Mounted>(std::move(Argv), Where);
  }

  static std::string shardName(int Rank) { return "shard-" + std::to_string(Rank) + ".bin"; }

  // Every rank starts before any of them finishes, so the contention is real
  // rather than whatever order the threads happened to be created in.
  template <class Save> void eachRank(Save One) {
    std::vector<std::thread> Ranks;
    for (int Rank = 0; Rank < kRanks; Rank++) Ranks.emplace_back([&One, Rank] { One(Rank); });
    for (auto &Running : Ranks) Running.join();
  }

  std::vector<std::string> streamArgs(std::vector<std::string> More = {}) {
    std::vector<std::string> Argv{"--page-size", "1", "--pages", "12", "--stream-after", "1", "--stream-chunk", "1", "-v"};
    Argv.insert(Argv.end(), More.begin(), More.end());
    return Argv;
  }

  std::string Root;
  std::string Host;
  std::filesystem::path At;
  std::filesystem::path Alpha;
  std::filesystem::path Nested;
  std::filesystem::path Big;
  std::optional<RemoteProcess> Daemon;
  std::optional<Mounted> Live;
};

TEST_P(Mount, ReadsAFileThroughTheMount) { EXPECT_EQ(localDigest(At / "alpha.bin"), localDigest(Alpha)); }

TEST_P(Mount, StatMatchesTheSource) {
  struct ::stat S{};
  ASSERT_EQ(::stat((At / "alpha.bin").c_str(), &S), 0);
  EXPECT_EQ(static_cast<uint64_t>(S.st_size), std::filesystem::file_size(Alpha));
  EXPECT_TRUE(S_ISREG(S.st_mode));
}

TEST_P(Mount, ListsTheDirectory) {
  std::vector<std::string> Names;
  for (const auto &E : std::filesystem::directory_iterator(At)) Names.push_back(E.path().filename().string());
  std::ranges::sort(Names);
  EXPECT_EQ(Names, (std::vector<std::string>{"alpha.bin", "big.bin", "sub"}));
}

TEST_P(Mount, WalksIntoASubdirectory) {
  EXPECT_TRUE(std::filesystem::is_directory(At / "sub"));
  EXPECT_EQ(localDigest(At / "sub" / "nested.bin"), localDigest(Nested));
}

TEST_P(Mount, ReadsFromAnOffsetPastTheWindow) {
  const uint64_t Offset = 6u << 20;
  std::ifstream Through(At / "alpha.bin", std::ios::binary);
  std::ifstream Direct(Alpha, std::ios::binary);
  Through.seekg(static_cast<std::streamoff>(Offset));
  Direct.seekg(static_cast<std::streamoff>(Offset));

  std::vector<char> A(1u << 20), B(1u << 20);
  Through.read(A.data(), static_cast<std::streamsize>(A.size()));
  Direct.read(B.data(), static_cast<std::streamsize>(B.size()));
  ASSERT_EQ(Through.gcount(), Direct.gcount());
  EXPECT_EQ(A, B);
}

TEST_P(Mount, MissingFileIsEnoent) {
  struct ::stat S{};
  EXPECT_NE(::stat((At / "no-such-file.bin").c_str(), &S), 0);
  EXPECT_EQ(errno, ENOENT);
}

TEST_P(Mount, ReportsDiskSpace) {
  struct ::statvfs S{};
  ASSERT_EQ(::statvfs(At.c_str(), &S), 0);
  EXPECT_GT(S.f_blocks, 0u);
}

TEST_P(Mount, PromotesASequentialReadToAStream) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: streaming"), std::string::npos) << "a full sequential read never promoted:\n" << Log;
}

TEST_P(Mount, StreamedReadsLandDirect) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));

  const std::string Log = Live->drain();
  const auto [Direct, Copied] = pagesFrom(Log);
  EXPECT_GT(Direct, 0u) << "no page landed directly; zero-copy is inert:\n" << Log;
  EXPECT_EQ(Copied, 0u) << "a streamed read fell back to the pooled path:\n" << Log;
}

TEST_P(Mount, ReadsSurviveExhaustion) {
  // Forty-eight megabytes is four shards' worth of pools and nothing over, so
  // the squeeze lands on the stream windows rather than starving a channel of
  // the pages it cannot serve without. Pin the count the figure was cut for.
  remountWith(streamArgs({"--threads", "4"}), {"RAIL_MEMORY_REGIONS=1", "RAIL_MEMORY_REGION_BYTES=50331648"});
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big)) << "the heap path must serve the same bytes";

  const std::string Log = Live->drain();
  const auto [Direct, Copied] = pagesFrom(Log);
  EXPECT_GT(Copied, 0u) << "the squeeze did not force the fallback, so it proves nothing:\n" << Log;
  (void)Direct;
}

TEST_P(Mount, ReadsAcrossPageEdges) {
  // Four 1 MiB pages to a chunk, so a read can straddle two of them.
  remountWith(streamArgs({"--stream-chunk", "4"}));

  const int Mine = ::open((At / "big.bin").c_str(), O_RDONLY);
  ASSERT_GE(Mine, 0);
  const int Theirs = ::open(Big.c_str(), O_RDONLY);
  ASSERT_GE(Theirs, 0);

  constexpr uint64_t kPage = 1u << 20;
  const std::vector<uint64_t> Offsets{0, 1, 4095, 4096, kPage - 4096, kPage - 1, kPage, kPage + 1, 2 * kPage - 7, 3 * kPage, 4 * kPage - 3};
  const std::vector<size_t> Sizes{1, 4096, 4097, 65536, (1u << 20) - 1};

  std::vector<char> A(1u << 21);
  std::vector<char> B(1u << 21);
  for (uint64_t Off : Offsets)
    for (size_t Size : Sizes) {
      SCOPED_TRACE("offset " + std::to_string(Off) + " size " + std::to_string(Size));
      const ssize_t GotMine = ::pread(Mine, A.data(), Size, static_cast<off_t>(Off));
      const ssize_t GotTheirs = ::pread(Theirs, B.data(), Size, static_cast<off_t>(Off));
      ASSERT_EQ(GotMine, GotTheirs);
      if (GotMine > 0) EXPECT_EQ(std::memcmp(A.data(), B.data(), static_cast<size_t>(GotMine)), 0);
    }

  ::close(Mine);
  ::close(Theirs);
}

TEST_P(Mount, PrefetchRunsAheadOfTheReader) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: prefetching"), std::string::npos) << "no chunk was ever fetched ahead:\n" << Log;
}

TEST_P(Mount, StreamCrossesChunkBoundaries) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));

  const std::string Log = Live->drain();
  size_t Chunks = 0;
  for (const std::string &What : {std::string{"railfs: streaming"}, std::string{"railfs: prefetching"}}) {
    for (size_t At2 = Log.find(What); At2 != std::string::npos; At2 = Log.find(What, At2 + What.size())) {
      Chunks++;
    }
  }
  EXPECT_GE(Chunks, 4u) << "a 24 MiB file in 4 MiB chunks should have taken several:\n" << Log;
}

TEST_P(Mount, DoesNotPromoteARandomReader) {
  remountWith(streamArgs());

  const int Fd = ::open((At / "big.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);

  std::ifstream Direct(Big, std::ios::binary);
  for (uint64_t Where : {uint64_t{5u << 20}, uint64_t{0}, uint64_t{3u << 20}, uint64_t{4096}, uint64_t{2u << 20}}) {
    std::vector<char> Got(4096), Want(4096);
    ASSERT_EQ(::pread(Fd, Got.data(), Got.size(), static_cast<off_t>(Where)), 4096) << "at " << Where;
    Direct.seekg(static_cast<std::streamoff>(Where));
    Direct.read(Want.data(), static_cast<std::streamsize>(Want.size()));
    ASSERT_EQ(Got, Want) << "differs at " << Where;
  }
  ::close(Fd);

  const std::string Log = Live->drain();
  EXPECT_EQ(Log.find("railfs: streaming"), std::string::npos) << "a seeking reader was promoted:\n" << Log;
}

TEST_P(Mount, DemotedReaderStillMatches) {
  remountWith(streamArgs());

  std::ifstream Through(At / "big.bin", std::ios::binary);
  std::ifstream Direct(Big, std::ios::binary);
  ASSERT_TRUE(Through.good());

  std::vector<char> A(3u << 20), B(3u << 20);
  Through.read(A.data(), static_cast<std::streamsize>(A.size()));
  Direct.read(B.data(), static_cast<std::streamsize>(B.size()));
  ASSERT_EQ(A, B) << "the forward part differs";

  Through.seekg(1024);
  Direct.seekg(1024);
  std::vector<char> C(1u << 20), D(1u << 20);
  Through.read(C.data(), static_cast<std::streamsize>(C.size()));
  Direct.read(D.data(), static_cast<std::streamsize>(D.size()));
  EXPECT_EQ(C, D) << "the part after a backward seek differs";
}

TEST_P(Mount, FirstReadAtAnOffsetStillStreams) {
  remountWith(streamArgs());

  const uint64_t Start = 2u << 20;
  const int Fd = ::open((At / "big.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  ASSERT_EQ(::lseek(Fd, static_cast<off_t>(Start), SEEK_SET), static_cast<off_t>(Start));

  std::vector<char> Got(1u << 20);
  uint64_t Read = 0;
  while (Read < (3u << 20)) {
    const auto N = ::read(Fd, Got.data(), Got.size());
    ASSERT_GT(N, 0) << "short read at " << Start + Read;
    Read += static_cast<uint64_t>(N);
  }
  ::close(Fd);

  std::ifstream Direct(Big, std::ios::binary);
  Direct.seekg(static_cast<std::streamoff>(Start + Read - Got.size()));
  std::vector<char> Want(Got.size());
  Direct.read(Want.data(), static_cast<std::streamsize>(Want.size()));
  EXPECT_EQ(Got, Want) << "the last block read mid-file differs";

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: streaming"), std::string::npos) << "a reader that opened mid-file never promoted:\n" << Log;
}

TEST_P(Mount, TwoOpenFilesStreamIndependently) {
  remountWith(streamArgs());

  std::ifstream One(At / "big.bin", std::ios::binary);
  std::ifstream Two(At / "alpha.bin", std::ios::binary);
  ASSERT_TRUE(One.good());
  ASSERT_TRUE(Two.good());

  std::vector<char> A(4u << 20), B(4u << 20);
  One.read(A.data(), static_cast<std::streamsize>(A.size()));
  Two.read(B.data(), static_cast<std::streamsize>(B.size()));
  EXPECT_EQ(One.gcount(), static_cast<std::streamsize>(A.size()));

  One.close();
  Two.close();
  EXPECT_EQ(localDigest(At / "alpha.bin"), localDigest(Alpha));
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));
}

TEST_P(Mount, SmallFileNeverPromotes) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "sub" / "nested.bin"), localDigest(Nested));

  const std::string Log = Live->drain();
  EXPECT_EQ(Log.find("nested.bin"), std::string::npos) << "a 2 KiB file promoted to a stream:\n" << Log;
}

TEST_P(Mount, PromotionOffMatchesPromotionOn) {
  remountWith(streamArgs({"--stream-after", "0"}));
  const std::string Ranged = localDigest(At / "big.bin");

  remountWith(streamArgs());
  const std::string Streamed = localDigest(At / "big.bin");

  EXPECT_EQ(Ranged, Streamed) << "the ranged and streamed paths disagree";
  EXPECT_EQ(Streamed, localDigest(Big));
}

TEST_P(Mount, StreamedReadStopsAtEof) {
  remountWith(streamArgs());

  const int Fd = ::open((At / "big.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  std::vector<char> Buf(1u << 20);
  ASSERT_EQ(::lseek(Fd, static_cast<off_t>(6u << 20), SEEK_SET), static_cast<off_t>(6u << 20));
  EXPECT_EQ(::read(Fd, Buf.data(), Buf.size()), 0) << "a read at end of file did not return zero";
  ::close(Fd);
}

TEST_P(Mount, DirectIoReadsAreLargerThanBuffered) {
  remountWith(streamArgs());
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));
  const std::string Direct = Live->drain();

  remountWith(streamArgs({"--buffered"}));
  EXPECT_EQ(localDigest(At / "big.bin"), localDigest(Big));
  const std::string Buffered = Live->drain();

  const auto Bytes = [](const std::string &Log) -> uint64_t {
    const auto At2 = Log.rfind(" bytes per read");
    if (At2 == std::string::npos) return 0;
    const auto From = Log.rfind(", ", At2);
    if (From == std::string::npos) return 0;
    return std::strtoull(Log.c_str() + From + 2, nullptr, 10);
  };

  EXPECT_GT(Bytes(Direct), Bytes(Buffered)) << "direct io did not produce larger reads\n--- direct ---\n"
                                            << Direct << "\n--- buffered ---\n"
                                            << Buffered;
}

namespace {

constexpr size_t kHeldSize = 4u << 20;

void writeThrough(const std::filesystem::path &At, unsigned char Fill) {
  std::vector<char> Body(kHeldSize, static_cast<char>(Fill));
  std::ofstream Out(At, std::ios::binary | std::ios::trunc);
  Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  Out.close();
  ASSERT_TRUE(Out.good()) << "could not write " << At;
}

std::optional<unsigned char> readOneAt(int Fd, off_t Off) {
  unsigned char Byte = 0;
  if (::pread(Fd, &Byte, 1, Off) != 1) return std::nullopt;
  return Byte;
}

} // namespace

TEST_P(Mount, AnOpenFileSurvivesARename) {
  remountWith({"--readahead", "1"});
  writeThrough(At / "held.bin", 0xaa);

  const int Fd = ::open((At / "held.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  ASSERT_EQ(readOneAt(Fd, 0), 0xaa);

  ASSERT_EQ(::rename((At / "held.bin").c_str(), (At / "moved.bin").c_str()), 0);

  const auto After = readOneAt(Fd, 2u << 20);
  ::close(Fd);
  ASSERT_TRUE(After.has_value()) << "reading a renamed file through an open handle failed with " << std::strerror(errno);
  EXPECT_EQ(*After, 0xaa);
}

TEST_P(Mount, AnOpenFileDoesNotFollowItsName) {
  remountWith({"--readahead", "1"});
  writeThrough(At / "held.bin", 0xaa);

  const int Fd = ::open((At / "held.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  ASSERT_EQ(readOneAt(Fd, 0), 0xaa);

  ASSERT_EQ(::rename((At / "held.bin").c_str(), (At / "moved.bin").c_str()), 0);
  writeThrough(At / "held.bin", 0xbb);

  const auto After = readOneAt(Fd, 3u << 20);
  ::close(Fd);
  ASSERT_TRUE(After.has_value()) << "reading through an open handle failed with " << std::strerror(errno);
  EXPECT_EQ(*After, 0xaa) << "an open handle served the bytes of the file that took its name";
}

TEST_P(Mount, AnOpenFileSyncsAfterARename) {
  remountWith({"--readahead", "1"});

  const int Fd = ::open((At / "synced.bin").c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(Fd, 0);
  const std::string Body(64u << 10, 's');
  ASSERT_EQ(::write(Fd, Body.data(), Body.size()), static_cast<ssize_t>(Body.size()));

  ASSERT_EQ(::rename((At / "synced.bin").c_str(), (At / "renamed.bin").c_str()), 0);

  const int Synced = ::fsync(Fd);
  const int Why = errno;
  ::close(Fd);
  EXPECT_EQ(Synced, 0) << "fsync on a renamed but open file failed with " << std::strerror(Why);
}

TEST_P(Mount, AnOpenFileSurvivesUnlink) {
  remountWith({"--readahead", "1"});
  writeThrough(At / "held.bin", 0xcc);

  const int Fd = ::open((At / "held.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  ASSERT_EQ(readOneAt(Fd, 0), 0xcc);

  ASSERT_EQ(::unlink((At / "held.bin").c_str()), 0);

  const auto After = readOneAt(Fd, 2u << 20);
  ::close(Fd);
  ASSERT_TRUE(After.has_value()) << "reading an unlinked file through an open handle failed with " << std::strerror(errno);
  EXPECT_EQ(*After, 0xcc);
}

TEST_P(Mount, AReadOnlyFileRefusesToOpenForWriting) {
  writeThrough(At / "sealed.bin", 0xdd);

  auto Sealed = peer().run({"chmod", "444", Root + "/sealed.bin"});
  ASSERT_TRUE(Sealed) << "could not make the file read-only on the peer";
  [[maybe_unused]] auto Waited = Sealed->readLine();

  errno = 0;
  const int Fd = ::open((At / "sealed.bin").c_str(), O_WRONLY);
  const int Why = errno;
  if (Fd >= 0) ::close(Fd);

  ASSERT_LT(Fd, 0) << "a read-only file opened for writing";
  EXPECT_EQ(Why, EACCES) << "reported " << std::strerror(Why) << " instead";
}

TEST_P(Mount, AReadAcrossAWindowBoundaryIsWhole) {
  remountWith({"--readahead", "1"});

  const size_t Size = 1u << 20;
  const off_t At2 = static_cast<off_t>(512u << 10);

  const int Fd = ::open((At / "alpha.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  std::vector<char> Got(Size);
  const ssize_t Read = ::pread(Fd, Got.data(), Size, At2);
  ::close(Fd);

  ASSERT_EQ(Read, static_cast<ssize_t>(Size)) << "a read across the window boundary came back short";

  std::ifstream Source(Alpha, std::ios::binary);
  Source.seekg(At2);
  std::vector<char> Want(Size);
  Source.read(Want.data(), static_cast<std::streamsize>(Size));
  EXPECT_EQ(Got, Want) << "a read across the window boundary did not match the source";
}

TEST_P(Mount, AReadAcrossAChunkBoundaryIsWhole) {
  remountWith(streamArgs());

  const size_t Size = 1u << 20;
  const int Fd = ::open((At / "alpha.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);

  std::vector<char> Warm(64u << 10);
  for (off_t Off = 0; Off < static_cast<off_t>(2u << 20); Off += static_cast<off_t>(Warm.size()))
    ASSERT_EQ(::pread(Fd, Warm.data(), Warm.size(), Off), static_cast<ssize_t>(Warm.size()));

  const off_t At2 = static_cast<off_t>((3u << 20) + (512u << 10));
  std::vector<char> Got(Size);
  const ssize_t Read = ::pread(Fd, Got.data(), Size, At2);
  ::close(Fd);

  ASSERT_EQ(Read, static_cast<ssize_t>(Size)) << "a read across the chunk boundary came back short";

  std::ifstream Source(Alpha, std::ios::binary);
  Source.seekg(At2);
  std::vector<char> Want(Size);
  Source.read(Want.data(), static_cast<std::streamsize>(Size));
  EXPECT_EQ(Got, Want) << "a read across the chunk boundary did not match the source";
}

TEST_P(Mount, ARandomReaderDoesNotPullAWholeWindow) {
  const auto Scattered = makeFile("fuse-scattered.bin", 64u << 20, 71);
  seedRemote(Scattered, Root + "/scattered.bin");
  remountWith({});

  constexpr size_t kStep = 4096;
  constexpr size_t kReads = 64;

  const int Fd = ::open((At / "scattered.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);
  std::vector<char> Buf(kStep);
  for (size_t I = 0; I < kReads; I++) {
    const off_t Off = static_cast<off_t>(((I * 7u * (1u << 20)) + (13u << 10)) % ((64u << 20) - kStep)) & ~off_t{4095};
    ASSERT_EQ(::pread(Fd, Buf.data(), kStep, Off), static_cast<ssize_t>(kStep)) << "read " << I << " at " << Off;
  }
  ::close(Fd);

  const std::string Log = Live->drain();
  const auto count = [&Log](const char *Label) -> double {
    const auto To = Log.rfind(Label);
    if (To == std::string::npos) return 0;
    const auto From = Log.rfind(", ", To);
    if (From == std::string::npos) return 0;
    return static_cast<double>(std::strtoull(Log.c_str() + From + 2, nullptr, 10));
  };

  const double Requested = count(" bytes requested");
  ASSERT_GT(Requested, 0) << Log;
  const double Pulled = count(" bytes fetched") / Requested;
  EXPECT_LE(Pulled, 32) << "a reader that jumps pulled " << Pulled << " bytes for every byte it asked for\n" << Log;
}

TEST_P(Mount, InterleavedReadersKeepTheirOwnWindow) {
  constexpr size_t kStep = 4096;
  constexpr size_t kSpan = 1u << 20;

  const auto count = [](const std::string &Log, const char *Label) -> double {
    const auto To = Log.rfind(Label);
    if (To == std::string::npos) return 0;
    const auto From = Log.rfind(", ", To);
    if (From == std::string::npos) return 0;
    return static_cast<double>(std::strtoull(Log.c_str() + From + 2, nullptr, 10));
  };

  const auto amplification = [&](const std::string &Log) {
    const double Requested = count(Log, " bytes requested");
    return Requested > 0 ? count(Log, " bytes fetched") / Requested : 0;
  };

  const auto readInStep = [&](const std::vector<std::filesystem::path> &Paths) {
    std::vector<int> Fds;
    for (const auto &One : Paths) {
      const int Fd = ::open(One.c_str(), O_RDONLY);
      EXPECT_GE(Fd, 0) << "could not open " << One;
      if (Fd >= 0) Fds.push_back(Fd);
    }

    std::vector<char> Buf(kStep);
    for (size_t Off = 0; Off + kStep <= kSpan; Off += kStep)
      for (const int Fd : Fds) ASSERT_EQ(::pread(Fd, Buf.data(), kStep, static_cast<off_t>(Off)), static_cast<ssize_t>(kStep));

    for (const int Fd : Fds) ::close(Fd);
  };

  remountWith({"--threads", "1"});
  readInStep({At / "alpha.bin"});
  const std::string Alone = Live->drain();

  remountWith({"--threads", "1"});
  readInStep({At / "alpha.bin", At / "big.bin"});
  const std::string Together = Live->drain();

  const double One = amplification(Alone);
  const double Two = amplification(Together);
  ASSERT_GT(One, 0) << "no read summary from a mount reading one file\n" << Alone;
  EXPECT_LE(Two, 3 * One) << "reading a second file made the mount refetch its window: " << Two << " bytes fetched per byte served against " << One
                          << " alone\n--- alone ---\n"
                          << Alone << "\n--- together ---\n"
                          << Together;
}

TEST_P(Mount, CreatesAndWritesAFile) {
  const std::string Body(64u << 10, 'q');
  {
    std::ofstream Out(At / "made.bin", std::ios::binary);
    ASSERT_TRUE(Out.good());
    Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  }

  auto Info = peer().stat(Root + "/made.bin");
  ASSERT_TRUE(Info) << "the file never reached the peer";
  EXPECT_EQ(Info->Size, Body.size());
}

TEST_P(Mount, LargeSequentialWriteMatches) {
  const auto Source = makeFile("fuse-write-big.bin", 4u << 20, 31);
  std::filesystem::copy_file(Source, At / "copied.bin", std::filesystem::copy_options::overwrite_existing);
  EXPECT_EQ(peer().digest(Root + "/copied.bin").value_or(""), localDigest(Source));
}

TEST_P(Mount, OverwritesInTheMiddle) {
  const auto Source = makeFile("fuse-mid.bin", 1u << 20, 32);
  std::filesystem::copy_file(Source, At / "mid.bin", std::filesystem::copy_options::overwrite_existing);

  const int Fd = ::open((At / "mid.bin").c_str(), O_WRONLY);
  ASSERT_GE(Fd, 0);
  const std::string Patch(512, 'Z');
  ASSERT_EQ(::pwrite(Fd, Patch.data(), Patch.size(), 4096), static_cast<ssize_t>(Patch.size()));
  ASSERT_EQ(::close(Fd), 0);

  std::fstream Local(Source, std::ios::binary | std::ios::in | std::ios::out);
  Local.seekp(4096);
  Local.write(Patch.data(), static_cast<std::streamsize>(Patch.size()));
  Local.close();

  EXPECT_EQ(peer().digest(Root + "/mid.bin").value_or(""), localDigest(Source));
}

TEST_P(Mount, AppendsToAFile) {
  const auto Source = makeFile("fuse-append.bin", 4096, 33);
  std::filesystem::copy_file(Source, At / "append.bin", std::filesystem::copy_options::overwrite_existing);

  const std::string Tail(1024, 'A');
  {
    std::ofstream Out(At / "append.bin", std::ios::binary | std::ios::app);
    Out.write(Tail.data(), static_cast<std::streamsize>(Tail.size()));
  }

  auto Info = peer().stat(Root + "/append.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, 4096u + Tail.size());
}

TEST_P(Mount, MakesAndRemovesDirectories) {
  ASSERT_EQ(::mkdir((At / "made-dir").c_str(), 0755), 0) << std::strerror(errno);
  EXPECT_TRUE(peer().exists(Root + "/made-dir").value_or(false));
  ASSERT_EQ(::rmdir((At / "made-dir").c_str()), 0) << std::strerror(errno);
  EXPECT_FALSE(peer().exists(Root + "/made-dir").value_or(true));
}

TEST_P(Mount, RemovesAFile) {
  ASSERT_EQ(::unlink((At / "alpha.bin").c_str()), 0) << std::strerror(errno);
  EXPECT_FALSE(peer().exists(Root + "/alpha.bin").value_or(true));
}

TEST_P(Mount, RenamesAFile) {
  ASSERT_EQ(::rename((At / "alpha.bin").c_str(), (At / "renamed.bin").c_str()), 0) << std::strerror(errno);
  EXPECT_TRUE(peer().exists(Root + "/renamed.bin").value_or(false));
  EXPECT_FALSE(peer().exists(Root + "/alpha.bin").value_or(true));
}

TEST_P(Mount, RenamesADirectoryWithChildren) {
  struct ::stat Before{};
  ASSERT_EQ(::stat((At / "sub" / "nested.bin").c_str(), &Before), 0);

  ASSERT_EQ(::rename((At / "sub").c_str(), (At / "after").c_str()), 0) << std::strerror(errno);

  struct ::stat Moved{};
  EXPECT_EQ(::stat((At / "after" / "nested.bin").c_str(), &Moved), 0) << "the child did not follow its renamed directory";
  EXPECT_EQ(localDigest(At / "after" / "nested.bin"), localDigest(Nested));
  EXPECT_TRUE(peer().exists(Root + "/after/nested.bin").value_or(false));
}

TEST_P(Mount, TruncatesAFile) {
  ASSERT_EQ(::truncate((At / "alpha.bin").c_str(), 512), 0) << std::strerror(errno);
  auto Info = peer().stat(Root + "/alpha.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, 512u);
}

TEST_P(Mount, ChmodsAFile) {
  ASSERT_EQ(::chmod((At / "alpha.bin").c_str(), 0600), 0) << std::strerror(errno);
  auto Info = peer().stat(Root + "/alpha.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Mode & 07777, 0600u);
}

TEST_P(Mount, MakesAndReadsASymbolicLink) {
  ASSERT_EQ(::symlink("alpha.bin", (At / "points-at-alpha").c_str()), 0) << std::strerror(errno);

  struct ::stat Link{};
  ASSERT_EQ(::lstat((At / "points-at-alpha").c_str(), &Link), 0) << std::strerror(errno);
  EXPECT_TRUE(S_ISLNK(Link.st_mode)) << "the link came back as mode " << std::oct << Link.st_mode;

  char Target[256] = {};
  const ssize_t Read = ::readlink((At / "points-at-alpha").c_str(), Target, sizeof(Target) - 1);
  ASSERT_GT(Read, 0) << std::strerror(errno);
  EXPECT_EQ(std::string(Target, static_cast<size_t>(Read)), "alpha.bin");

  // Following it has to reach the file it names, which is the whole point.
  struct ::stat Followed{};
  ASSERT_EQ(::stat((At / "points-at-alpha").c_str(), &Followed), 0) << std::strerror(errno);
  EXPECT_TRUE(S_ISREG(Followed.st_mode));
  EXPECT_EQ(::unlink((At / "points-at-alpha").c_str()), 0);
}

TEST_P(Mount, MakesAHardLink) {
  ASSERT_EQ(::link((At / "alpha.bin").c_str(), (At / "second-name.bin").c_str()), 0) << std::strerror(errno);

  struct ::stat Linked{};
  ASSERT_EQ(::lstat((At / "second-name.bin").c_str(), &Linked), 0) << std::strerror(errno);
  EXPECT_TRUE(S_ISREG(Linked.st_mode)) << "the second name came back as mode " << std::oct << Linked.st_mode;
  EXPECT_EQ(Linked.st_nlink, 2u) << "the link count did not reach the mount";

  // The same bytes under both names, which is what makes it a link rather than
  // a copy the daemon happened to make.
  EXPECT_EQ(localDigest(At / "second-name.bin"), localDigest(Alpha));

  // Removing one name leaves the other, and the file, alone.
  ASSERT_EQ(::unlink((At / "second-name.bin").c_str()), 0) << std::strerror(errno);
  EXPECT_EQ(localDigest(At / "alpha.bin"), localDigest(Alpha));
}

TEST_P(Mount, AHardLinkOverAnExistingNameIsRefused) {
  errno = 0;
  EXPECT_NE(::link((At / "alpha.bin").c_str(), (At / "big.bin").c_str()), 0) << "a link wrote over a file that was already there";
  EXPECT_EQ(errno, EEXIST) << "the refusal arrived as " << std::strerror(errno);
}

TEST_P(Mount, AHardLinkToADirectoryIsRefused) {
  errno = 0;
  EXPECT_NE(::link((At / "sub").c_str(), (At / "sub-again").c_str()), 0) << "a directory was hard linked";
  EXPECT_TRUE(errno == EPERM || errno == EACCES) << "the refusal arrived as " << std::strerror(errno);
}

// A link to nowhere is still a link. Reporting what it points at rather than
// following it is what keeps it visible instead of a listing that names a file
// no lookup can find.
TEST_P(Mount, ADanglingLinkIsStillListed) {
  ASSERT_EQ(::symlink("no-such-file.bin", (At / "dangling").c_str()), 0) << std::strerror(errno);

  struct ::stat Link{};
  ASSERT_EQ(::lstat((At / "dangling").c_str(), &Link), 0) << std::strerror(errno);
  EXPECT_TRUE(S_ISLNK(Link.st_mode));

  struct ::stat Followed{};
  EXPECT_NE(::stat((At / "dangling").c_str(), &Followed), 0) << "a link to nothing resolved to something";
  EXPECT_EQ(errno, ENOENT);
  EXPECT_EQ(::unlink((At / "dangling").c_str()), 0);
}

TEST_P(Mount, SetsMtime) {
  const ::timespec Times[2] = {{0, UTIME_OMIT}, {1000000000, 0}};
  ASSERT_EQ(::utimensat(AT_FDCWD, (At / "alpha.bin").c_str(), Times, 0), 0) << std::strerror(errno);
  auto Info = peer().stat(Root + "/alpha.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Mtime, 1000000000);
}

TEST_P(Mount, FsyncReachesThePeer) {
  const int Fd = ::open((At / "synced.bin").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(Fd, 0) << std::strerror(errno);
  const std::string Body(8192, 'y');
  ASSERT_EQ(::write(Fd, Body.data(), Body.size()), static_cast<ssize_t>(Body.size()));
  EXPECT_EQ(::fsync(Fd), 0) << std::strerror(errno);
  ASSERT_EQ(::close(Fd), 0);

  auto Info = peer().stat(Root + "/synced.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, Body.size());
}

TEST_P(Mount, ChownIsRefused) {
  EXPECT_NE(::chown((At / "alpha.bin").c_str(), 0, 0), 0) << "chown was accepted on a mount that cannot carry ownership";
}

TEST_P(Mount, StreamsALargeSequentialWrite) {
  remountWith(streamArgs());

  const auto Source = makeFile("fuse-stream-write.bin", 6u << 20, 41);
  std::filesystem::copy_file(Source, At / "streamed.bin", std::filesystem::copy_options::overwrite_existing);

  EXPECT_EQ(peer().digest(Root + "/streamed.bin").value_or(""), localDigest(Source));

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: storing"), std::string::npos) << "a large sequential write never streamed:\n" << Log;
}

TEST_P(Mount, SmallWriteDoesNotStream) {
  remountWith(streamArgs());

  const std::string Body(4096, 's');
  {
    std::ofstream Out(At / "small.bin", std::ios::binary);
    Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  }

  auto Info = peer().stat(Root + "/small.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, Body.size());

  const std::string Log = Live->drain();
  EXPECT_EQ(Log.find("railfs: storing"), std::string::npos) << "a 4 KiB write was streamed:\n" << Log;
}

TEST_P(Mount, SeekingWriterStillMatches) {
  remountWith(streamArgs());

  const auto Source = makeFile("fuse-seek-write.bin", 4u << 20, 42);
  std::filesystem::copy_file(Source, At / "seeked.bin", std::filesystem::copy_options::overwrite_existing);

  const int Fd = ::open((At / "seeked.bin").c_str(), O_WRONLY);
  ASSERT_GE(Fd, 0);
  const std::string Patch(512, 'W');
  for (uint64_t Where : {uint64_t{3u << 20}, uint64_t{0}, uint64_t{2u << 20}}) {
    ASSERT_EQ(::pwrite(Fd, Patch.data(), Patch.size(), static_cast<off_t>(Where)), static_cast<ssize_t>(Patch.size()));
    std::fstream Local(Source, std::ios::binary | std::ios::in | std::ios::out);
    Local.seekp(static_cast<std::streamoff>(Where));
    Local.write(Patch.data(), static_cast<std::streamsize>(Patch.size()));
  }
  ASSERT_EQ(::close(Fd), 0);

  EXPECT_EQ(peer().digest(Root + "/seeked.bin").value_or(""), localDigest(Source)) << "a seeking writer did not land correctly";
}

TEST_P(Mount, OverwritingTheHeadKeepsTheTail) {
  remountWith(streamArgs());

  const auto Source = makeFile("fuse-head-write.bin", 8u << 20, 44);
  std::filesystem::copy_file(Source, At / "head.bin", std::filesystem::copy_options::overwrite_existing);

  const int Fd = ::open((At / "head.bin").c_str(), O_WRONLY);
  ASSERT_GE(Fd, 0);
  const std::vector<char> Head(4u << 20, 'W');
  ASSERT_EQ(::pwrite(Fd, Head.data(), Head.size(), 0), static_cast<ssize_t>(Head.size()));
  ASSERT_EQ(::close(Fd), 0);

  std::fstream Local(Source, std::ios::binary | std::ios::in | std::ios::out);
  Local.write(Head.data(), static_cast<std::streamsize>(Head.size()));
  Local.close();

  auto Info = peer().stat(Root + "/head.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, 8u << 20) << "a streamed write at offset 0 replaced the whole file";
  EXPECT_EQ(peer().digest(Root + "/head.bin").value_or(""), localDigest(Source));

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: storing"), std::string::npos) << "the overwrite never took the streamed path:\n" << Log;
}

TEST_P(Mount, ReadsItsOwnWrites) {
  const int Fd = ::open((At / "readback.bin").c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(Fd, 0);

  const std::string Wrote(4096, 'A');
  ASSERT_EQ(::pwrite(Fd, Wrote.data(), Wrote.size(), 0), static_cast<ssize_t>(Wrote.size()));

  std::string Back(Wrote.size(), '\0');
  EXPECT_EQ(::pread(Fd, Back.data(), Back.size(), 0), static_cast<ssize_t>(Back.size())) << "a read did not see a write on its own handle";
  EXPECT_EQ(Back, Wrote);
  ASSERT_EQ(::close(Fd), 0);
}

TEST_P(Mount, SizeFollowsABufferedWrite) {
  remountWith({"--consistent"});

  const int Fd = ::open((At / "sized.bin").c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(Fd, 0);

  const std::string Wrote(4096, 'S');
  ASSERT_EQ(::pwrite(Fd, Wrote.data(), Wrote.size(), 0), static_cast<ssize_t>(Wrote.size()));

  struct ::stat S{};
  ASSERT_EQ(::fstat(Fd, &S), 0);
  EXPECT_EQ(S.st_size, static_cast<off_t>(Wrote.size())) << "stat did not see a buffered write";
  ASSERT_EQ(::close(Fd), 0);
}

TEST_P(Mount, TruncatingAfterABufferedWriteWins) {
  const int Fd = ::open((At / "trunc-after.bin").c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(Fd, 0);

  const std::string Wrote(4096, 'T');
  ASSERT_EQ(::pwrite(Fd, Wrote.data(), Wrote.size(), 0), static_cast<ssize_t>(Wrote.size()));
  ASSERT_EQ(::ftruncate(Fd, 0), 0);
  ASSERT_EQ(::close(Fd), 0);

  auto Info = peer().stat(Root + "/trunc-after.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, 0u) << "a buffered write outlived the truncate that followed it";
}

TEST_P(Mount, BufferedMidFileReadStillStreams) {
  remountWith(streamArgs({"--stream-after", "4"}));

  const uint64_t Start = 1u << 20;
  std::ifstream Through(At / "big.bin", std::ios::binary);
  std::ifstream Direct(Big, std::ios::binary);
  ASSERT_TRUE(Through.good());
  Through.seekg(static_cast<std::streamoff>(Start));
  Direct.seekg(static_cast<std::streamoff>(Start));

  std::vector<char> A(5u << 20), B(5u << 20);
  Through.read(A.data(), static_cast<std::streamsize>(A.size()));
  Direct.read(B.data(), static_cast<std::streamsize>(B.size()));
  ASSERT_EQ(Through.gcount(), Direct.gcount());
  EXPECT_EQ(A, B);

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("railfs: streaming"), std::string::npos) << "an overlapping buffered read pattern never promoted:\n" << Log;
}

TEST_P(Mount, ReaddirReportsDistinctInodes) {
  DIR *D = ::opendir(At.c_str());
  ASSERT_NE(D, nullptr);

  std::map<std::string, ino_t> FromReaddir;
  while (const ::dirent *E = ::readdir(D)) {
    const std::string Name = E->d_name;
    if (Name == "." || Name == "..") continue;
    FromReaddir[Name] = E->d_ino;
  }
  ::closedir(D);

  ASSERT_GE(FromReaddir.size(), 2u);

  std::set<ino_t> Seen;
  for (const auto &[Name, Number] : FromReaddir) {
    EXPECT_NE(Number, 0u) << Name << " has no inode number";
    EXPECT_TRUE(Seen.insert(Number).second) << Name << " shares an inode number with another entry";

    struct ::stat S{};
    ASSERT_EQ(::stat((At / Name).c_str(), &S), 0);
    EXPECT_EQ(S.st_ino, Number) << Name << ": readdir and stat disagree about the inode";
  }
}

TEST_P(Mount, SeesAFileThatGrewOnThePeer) {
  remountWith(streamArgs());

  const int Fd = ::open((At / "alpha.bin").c_str(), O_RDONLY);
  ASSERT_GE(Fd, 0);

  const uint64_t Was = std::filesystem::file_size(Alpha);
  std::vector<char> Buf(1u << 20);
  for (uint64_t Done = 0; Done < Was;) {
    const auto N = ::pread(Fd, Buf.data(), Buf.size(), static_cast<off_t>(Done));
    ASSERT_GT(N, 0);
    Done += static_cast<uint64_t>(N);
  }

  auto Grew = peer().run({"sh", "-c", "dd if=/dev/zero bs=1M count=1 >> " + Root + "/alpha.bin 2>/dev/null"});
  ASSERT_TRUE(Grew);
  [[maybe_unused]] auto Line = Grew->readLine();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const auto N = ::pread(Fd, Buf.data(), Buf.size(), static_cast<off_t>(Was));
  ::close(Fd);

  const std::string Log = Live->drain();
  ASSERT_NE(Log.find("railfs: streaming"), std::string::npos) << "the reader never promoted, so this proves nothing:\n" << Log;
  EXPECT_EQ(N, static_cast<ssize_t>(Buf.size())) << "a streaming handle could not see bytes appended on the peer";
}

TEST_P(Mount, WarnsWhenWriteChunkCannotStream) {
  remountWith({"--write-chunk", "1"});
  const std::string Body(4096, 'w');
  {
    std::ofstream Out(At / "warned.bin", std::ios::binary);
    Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  }

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("stay ranged"), std::string::npos) << "a write-chunk too small to stream was not reported:\n" << Log;
}

TEST_P(Mount, ACorruptedStreamedWriteIsRepaired) {
  remountWith(streamArgs({"--flip-one-bit"}));

  const auto Source = makeFile("fuse-repair.bin", 6u << 20, 43);
  std::filesystem::copy_file(Source, At / "repaired.bin", std::filesystem::copy_options::overwrite_existing);

  EXPECT_EQ(peer().digest(Root + "/repaired.bin").value_or(""), localDigest(Source))
      << "a streamed store that failed its digest did not get repaired";

  const std::string Log = Live->drain();
  EXPECT_NE(Log.find("rewriting ranged"), std::string::npos) << "the corrupted store never fell back:\n" << Log;
}

// What a training job does at a checkpoint: every rank writes its own shard to
// the same mount at the same moment. Eight writers against four sessions means
// they queue for one, and every shard is large enough to stream, so they also
// share the tag space and the page pool - the two places a window wider than
// the pool has deadlocked before.
TEST_P(Mount, ShardsSaveAtOnce) {
  remountWith(streamArgs());

  std::vector<std::filesystem::path> Shards;
  for (int Rank = 0; Rank < kRanks; Rank++) Shards.push_back(makeFile("shard-" + std::to_string(Rank) + ".bin", 6u << 20, 60 + Rank));

  std::vector<std::error_code> Failed(kRanks);
  eachRank([&](int Rank) {
    std::filesystem::copy_file(Shards[Rank], At / shardName(Rank), std::filesystem::copy_options::overwrite_existing, Failed[Rank]);
  });

  const std::string Log = Live->drain();
  for (int Rank = 0; Rank < kRanks; Rank++) EXPECT_FALSE(Failed[Rank]) << shardName(Rank) << ": " << Failed[Rank].message() << "\n" << Log;

  // A pool that handed one rank another's page shows up here as a digest
  // belonging to the wrong rank, not as a corrupted one.
  for (int Rank = 0; Rank < kRanks; Rank++)
    EXPECT_EQ(peer().digest(Root + "/" + shardName(Rank)).value_or(""), localDigest(Shards[Rank]))
        << shardName(Rank) << " did not survive eight writers";
}

// Open once, write each tensor, close at the end, which is how a checkpoint is
// actually saved.
TEST_P(Mount, ShardsAppendAtOnce) {
  constexpr int kRounds = 4;
  remountWith(streamArgs());

  const size_t Each = 1u << 20;
  eachRank([&](int Rank) {
    const std::string Body(Each, static_cast<char>('a' + Rank));
    std::ofstream Out(At / shardName(Rank), std::ios::binary);
    for (int Round = 0; Round < kRounds; Round++) Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  });

  for (int Rank = 0; Rank < kRanks; Rank++) {
    auto Info = peer().stat(Root + "/" + shardName(Rank));
    ASSERT_TRUE(Info) << "rank " << Rank << " left no shard";
    EXPECT_EQ(Info->Size, Each * kRounds) << "rank " << Rank << " lost or duplicated a round";
  }
}

// The second checkpoint lands on top of the first. A shorter shard must not
// leave the tail of the older one behind.
TEST_P(Mount, ShardsOverwriteAtOnce) {
  remountWith(streamArgs());

  std::vector<std::filesystem::path> First, Second;
  for (int Rank = 0; Rank < kRanks; Rank++) {
    First.push_back(makeFile("first-" + std::to_string(Rank) + ".bin", 6u << 20, 80 + Rank));
    Second.push_back(makeFile("second-" + std::to_string(Rank) + ".bin", 2u << 20, 90 + Rank));
  }

  for (const auto *Round : {&First, &Second})
    eachRank([&](int Rank) { std::filesystem::copy_file((*Round)[Rank], At / shardName(Rank), std::filesystem::copy_options::overwrite_existing); });

  for (int Rank = 0; Rank < kRanks; Rank++)
    EXPECT_EQ(peer().digest(Root + "/" + shardName(Rank)).value_or(""), localDigest(Second[Rank]))
        << shardName(Rank) << " kept part of the older checkpoint";
}

// A rank that fsyncs before it reports success, which is what makes a
// checkpoint restartable.
TEST_P(Mount, ShardsSyncAtOnce) {
  remountWith(streamArgs());

  const size_t Each = 2u << 20;
  eachRank([&](int Rank) {
    const std::string Body(Each, static_cast<char>('A' + Rank));
    const int Fd = ::open((At / shardName(Rank)).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (Fd < 0) return;
    [[maybe_unused]] const ssize_t Wrote = ::write(Fd, Body.data(), Body.size());
    ::fsync(Fd);
    ::close(Fd);
  });

  for (int Rank = 0; Rank < kRanks; Rank++) {
    auto Info = peer().stat(Root + "/" + shardName(Rank));
    ASSERT_TRUE(Info) << "rank " << Rank << " left no shard";
    EXPECT_EQ(Info->Size, Each);
  }
}

// Optimiser state is many small files rather than one large one, which puts
// the load on lookup and create instead of the page pool.
TEST_P(Mount, SmallShardsSaveAtOnce) {
  constexpr int kEach = 8;
  remountWith(streamArgs());

  const std::string Body(4096, 'o');
  eachRank([&](int Rank) {
    for (int I = 0; I < kEach; I++) {
      std::ofstream Out(At / ("state-" + std::to_string(Rank) + "-" + std::to_string(I) + ".bin"), std::ios::binary);
      Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
    }
  });

  for (int Rank = 0; Rank < kRanks; Rank++)
    for (int I = 0; I < kEach; I++) {
      const std::string Name = "state-" + std::to_string(Rank) + "-" + std::to_string(I) + ".bin";
      auto Info = peer().stat(Root + "/" + Name);
      ASSERT_TRUE(Info) << Name << " never arrived";
      EXPECT_EQ(Info->Size, Body.size()) << Name << " is the wrong size";
    }
}

// The usual on-disk layout: one directory per rank, created by the rank that
// writes into it, so create and lookup race on the same parent.
TEST_P(Mount, ShardsSaveInOwnDirectories) {
  remountWith(streamArgs());

  const std::string Body(1u << 20, 'd');
  eachRank([&](int Rank) {
    const std::filesystem::path Dir = At / ("rank-" + std::to_string(Rank));
    std::error_code EC;
    std::filesystem::create_directory(Dir, EC);
    std::ofstream Out(Dir / "shard.bin", std::ios::binary);
    Out.write(Body.data(), static_cast<std::streamsize>(Body.size()));
  });

  for (int Rank = 0; Rank < kRanks; Rank++) {
    auto Info = peer().stat(Root + "/rank-" + std::to_string(Rank) + "/shard.bin");
    ASSERT_TRUE(Info) << "rank " << Rank << " left no shard in its directory";
    EXPECT_EQ(Info->Size, Body.size());
  }
}

// fio drives the write patterns a checkpoint library actually issues, which is
// more than a copy loop reaches: its own buffering, its own sync policy, and
// eight jobs started together rather than as fast as threads happen to spawn.
TEST_P(Mount, FioSavesShardsAtOnce) {
  if (runLocal({"sh", "-c", "command -v fio"}).value_or(ProcessResult{1, "", 0}).ExitStatus != 0) GTEST_SKIP() << "fio is not installed";
  remountWith(streamArgs());

  // Small blocks land as ranged writes and large ones stream, so the sweep
  // crosses the boundary rather than testing one side of it.
  for (const auto &[Block, Size] : {std::pair{"4k", 1u << 20}, std::pair{"64k", 4u << 20}, std::pair{"1m", 16u << 20}}) {
    auto Ran = runLocal({"fio",
                         "--name=ckpt",
                         "--directory=" + At.string(),
                         "--rw=write",
                         std::string("--bs=") + Block,
                         "--size=" + std::to_string(Size),
                         "--numjobs=" + std::to_string(kRanks),
                         "--end_fsync=1",
                         "--group_reporting",
                         "--minimal"});
    ASSERT_TRUE(Ran);
    EXPECT_EQ(Ran->ExitStatus, 0) << "bs=" << Block << ":\n" << Ran->Output << "\n--- mount said:\n" << Live->drain();

    for (int Rank = 0; Rank < kRanks; Rank++) {
      const std::string Name = "ckpt." + std::to_string(Rank) + ".0";
      auto Info = peer().stat(Root + "/" + Name);
      ASSERT_TRUE(Info) << Name << " never arrived at bs=" << Block << ":\n" << Ran->Output;
      EXPECT_EQ(Info->Size, Size) << Name << " is the wrong size at bs=" << Block;
    }
  }
}

// Two nodes checkpointing at once, simulated: two mounts of the same export,
// half the ranks on each. The daemon sees two clients with their own sessions
// and tag spaces writing into one directory.
TEST_P(Mount, TwoMountsSaveShardsAtOnce) {
  remountWith(streamArgs());
  auto Other = secondMount(streamArgs());
  ASSERT_TRUE(Other->ready()) << "the second mount never came up";

  std::vector<std::filesystem::path> Shards;
  for (int Rank = 0; Rank < kRanks; Rank++) Shards.push_back(makeFile("pair-" + std::to_string(Rank) + ".bin", 4u << 20, 120 + Rank));

  std::vector<std::error_code> Failed(kRanks);
  eachRank([&](int Rank) {
    const std::filesystem::path Where = (Rank % 2 == 0 ? At : Other->at()) / shardName(Rank);
    std::filesystem::copy_file(Shards[Rank], Where, std::filesystem::copy_options::overwrite_existing, Failed[Rank]);
  });

  for (int Rank = 0; Rank < kRanks; Rank++) EXPECT_FALSE(Failed[Rank]) << shardName(Rank) << ": " << Failed[Rank].message();
  for (int Rank = 0; Rank < kRanks; Rank++)
    EXPECT_EQ(peer().digest(Root + "/" + shardName(Rank)).value_or(""), localDigest(Shards[Rank]))
        << shardName(Rank) << " did not survive two mounts";
}

TEST_P(Mount, ReadsTwoFilesAtOnce) {
  const auto First = makeFile("fuse-par-a.bin", 6u << 20, 51);
  const auto Second = makeFile("fuse-par-b.bin", 6u << 20, 52);
  std::filesystem::copy_file(First, At / "par-a.bin", std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(Second, At / "par-b.bin", std::filesystem::copy_options::overwrite_existing);

  std::string Through[2];
  std::thread Readers[2];
  const std::filesystem::path Paths[2] = {At / "par-a.bin", At / "par-b.bin"};

  for (int I = 0; I < 2; I++) Readers[I] = std::thread([&, I] { Through[I] = localDigest(Paths[I]); });
  for (auto &One : Readers) One.join();

  EXPECT_EQ(Through[0], localDigest(First)) << "a file read beside another did not match";
  EXPECT_EQ(Through[1], localDigest(Second)) << "a file read beside another did not match";
}

TEST_P(Mount, UnmountsWhenKilled) {
  ASSERT_TRUE(mounted(At));
  [[maybe_unused]] auto Killed = runLocal({"pkill", "-x", "mount.railfuse"});

  bool Gone = false;
  for (int I = 0; I < 100 && !Gone; I++) {
    Gone = !mounted(At);
    if (!Gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_TRUE(Gone) << "a killed railfs left its mount attached";
  Live.reset();
}

TEST_P(Mount, SurvivesTheDaemonRestarting) {
  remountWith({"--consistent"});
  ASSERT_EQ(localDigest(At / "alpha.bin"), localDigest(Alpha));

  stopDaemon();
  {
    std::ifstream Through(At / "alpha.bin", std::ios::binary);
    std::vector<char> Buf(4096);
    Through.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
    EXPECT_TRUE(Through.fail()) << "a read against a dead daemon reported success";
  }
  EXPECT_TRUE(mounted(At)) << "the mount went away with the daemon";

  startDaemon();
  EXPECT_TRUE(digestRecovers(At / "alpha.bin", localDigest(Alpha))) << "the mount did not recover";
}

TEST_P(Mount, SurvivesABrokenPipe) {
  ASSERT_TRUE(mounted(At));

  [[maybe_unused]] auto Signalled = runLocal({"pkill", "-PIPE", "-x", "mount.railfuse"});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_TRUE(mounted(At)) << "a broken pipe took the mount down";
  EXPECT_EQ(localDigest(At / "alpha.bin"), localDigest(Alpha)) << "the mount stopped serving after a broken pipe";
}

TEST_P(Mount, SurvivesTheDaemonDyingMidRead) {
  const auto Wide = makeFile("fuse-wide.bin", 48u << 20, 61);
  seedRemote(Wide, Root + "/wide.bin");
  remountWith({"--consistent", "--readahead", "1"});

  std::atomic<bool> Reading{true};
  std::vector<std::thread> Readers;
  for (int I = 0; I < 4; I++)
    Readers.emplace_back([&] {
      std::vector<char> Buf(1u << 20);
      while (Reading) {
        const int Fd = ::open((At / "wide.bin").c_str(), O_RDONLY);
        if (Fd < 0) continue;
        for (off_t Off = 0; Off < static_cast<off_t>(48u << 20) && Reading; Off += static_cast<off_t>(Buf.size()))
          if (::pread(Fd, Buf.data(), Buf.size(), Off) <= 0) break;
        ::close(Fd);
      }
    });

  for (int Round = 0; Round < 3; Round++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    killDaemon();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    startDaemon();
  }

  Reading = false;
  for (auto &One : Readers) One.join();

  EXPECT_TRUE(mounted(At)) << "the mount went away when the daemon died under a read";
  EXPECT_TRUE(digestRecovers(At / "alpha.bin", localDigest(Alpha))) << "the mount did not recover";
}

TEST_P(Mount, RecoversFromRepeatedDaemonLoss) {
  remountWith({"--consistent"});

  for (int Round = 0; Round < 3; Round++) {
    stopDaemon();
    {
      std::ifstream Through(At / "alpha.bin", std::ios::binary);
      std::vector<char> Buf(4096);
      Through.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
    }
    startDaemon();
    if (!mounted(At)) {
      const std::string Log = Live->drain();
      FAIL() << "the mount died in round " << Round << ", railfs said:\n" << Log;
    }
    ASSERT_TRUE(digestRecovers(At / "alpha.bin", localDigest(Alpha))) << "round " << Round << " did not recover";
  }
}

INSTANTIATE_TEST_SUITE_P(Backends, Mount, ::testing::ValuesIn(kBackends), [](const auto &I) { return I.param; });

} // namespace rail::e2e
