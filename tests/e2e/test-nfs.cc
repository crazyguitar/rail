#include "harness.h"

#include "local-process.h"
#include "rail/nfs/rpc.h"
#include "rail/nfs/xdr.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace rail::e2e {

namespace {

uint16_t servicePortFor(const std::string &Backend) { return Backend == "tcp" ? 18721 : 18722; }
uint16_t exportPortFor(const std::string &Backend) { return Backend == "tcp" ? 18731 : 18732; }

constexpr uint32_t kMountMnt = 1;
constexpr uint32_t kNfsGetAttr = 1;
constexpr uint32_t kNfsLookup = 3;
constexpr uint32_t kNfsAccess = 4;
constexpr uint32_t kNfsRead = 6;
constexpr uint32_t kNfsWrite = 7;
constexpr uint32_t kNfsSetAttr = 2;
constexpr uint32_t kNfsCreate = 8;
constexpr uint32_t kNfsMakeDirectory = 9;
constexpr uint32_t kNfsSymlink = 10;
constexpr uint32_t kNfsReadLink = 5;
constexpr uint32_t kNfsRemove = 12;
constexpr uint32_t kNfsRemoveDirectory = 13;
constexpr uint32_t kNfsRename = 14;
constexpr uint32_t kNfsLink = 15;
constexpr uint32_t kNfsReadDir = 16;
constexpr uint32_t kNfsReadDirPlus = 17;
constexpr uint32_t kNfsFsInfo = 19;
constexpr uint32_t kNfsCommit = 21;

struct Reply {
  uint32_t Accept = 0;
  std::vector<std::byte> Body;
};

class RpcProbe {
public:
  RpcProbe() = default;
  RpcProbe(const RpcProbe &) = delete;
  RpcProbe &operator=(const RpcProbe &) = delete;
  ~RpcProbe() { close(); }

  Result<void> open(uint16_t Port) {
    close();
    Fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (Fd < 0) return failErrno("socket");

    const int One = 1;
    ::setsockopt(Fd, IPPROTO_TCP, TCP_NODELAY, &One, sizeof(One));

    sockaddr_in Addr{};
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(Port);
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(Fd, reinterpret_cast<sockaddr *>(&Addr), sizeof(Addr)) < 0) {
      close();
      return failErrno("connect");
    }
    return Result<void>{};
  }

  void close() {
    if (Fd >= 0) ::close(Fd);
    Fd = -1;
  }

  Result<Reply> call(uint32_t Program, uint32_t Procedure, std::span<const std::byte> Args) {
    nfs::XdrWriter Header;
    Header.u32(++Xid);
    Header.u32(0);
    Header.u32(2);
    Header.u32(Program);
    Header.u32(nfs::kProgramVersion);
    Header.u32(Procedure);
    Header.u32(0);
    Header.u32(0);
    Header.u32(0);
    Header.u32(0);

    nfs::XdrWriter Marker;
    Marker.u32(static_cast<uint32_t>(Header.size() + Args.size()) | 0x80000000);

    if (auto R = writeAll(Marker.bytes()); !R) return std::unexpected(R.error());
    if (auto R = writeAll(Header.bytes()); !R) return std::unexpected(R.error());
    if (auto R = writeAll(Args); !R) return std::unexpected(R.error());

    std::vector<std::byte> Record;
    for (;;) {
      std::byte Head[4];
      if (auto R = readExact(Head); !R) return std::unexpected(R.error());

      uint32_t Fragment = 0;
      for (std::byte B : Head) Fragment = (Fragment << 8) | static_cast<uint32_t>(B);

      const size_t At = Record.size();
      Record.resize(At + (Fragment & ~0x80000000u));
      if (auto R = readExact(std::span(Record).subspan(At)); !R) return std::unexpected(R.error());
      if (Fragment & 0x80000000u) break;
    }

    nfs::XdrReader R(Record);
    if (R.u32() != Xid) return failMessage("reply for another call");
    if (R.u32() != 1) return failMessage("not an rpc reply");
    if (R.u32() != 0) return failMessage("the call was denied");
    R.u32();
    R.opaque(400);

    Reply Out;
    Out.Accept = R.u32();
    if (!R.ok()) return failMessage("malformed reply header");
    Out.Body.assign(Record.end() - static_cast<long>(R.left()), Record.end());
    return Out;
  }

private:
  Result<void> writeAll(std::span<const std::byte> Src) {
    while (!Src.empty()) {
      const ssize_t N = ::write(Fd, Src.data(), Src.size());
      if (N <= 0) return failErrno("write");
      Src = Src.subspan(static_cast<size_t>(N));
    }
    return Result<void>{};
  }

  Result<void> readExact(std::span<std::byte> Dst) {
    while (!Dst.empty()) {
      const ssize_t N = ::read(Fd, Dst.data(), Dst.size());
      if (N <= 0) return failErrno("read");
      Dst = Dst.subspan(static_cast<size_t>(N));
    }
    return Result<void>{};
  }

  int Fd = -1;
  uint32_t Xid = 0;
};

std::vector<std::byte> localBytes(const std::filesystem::path &P) {
  std::ifstream In(P, std::ios::binary);
  std::vector<std::byte> Out;
  char Chunk[64 << 10];
  while (In.read(Chunk, sizeof(Chunk)) || In.gcount() > 0) {
    const size_t N = static_cast<size_t>(In.gcount());
    const auto *Start = reinterpret_cast<const std::byte *>(Chunk);
    Out.insert(Out.end(), Start, Start + N);
    if (!In) break;
  }
  return Out;
}

} // namespace

class Nfs : public BackendTest {
protected:
  void SetUp() override {
    Root = remoteDir() + "/nfs-root";
    removeRemoteRecursive(Root);
    ASSERT_TRUE(peer().makeDirectory(Root));
    ASSERT_TRUE(peer().makeDirectory(Root + "/sub"));

    Alpha = makeFile("nfs-alpha.bin", 4096, 21);
    seedRemote(Alpha, Root + "/alpha.bin");
    seedRemote(makeFile("nfs-nested.bin", 2048, 22), Root + "/sub/nested.bin");

    auto Address = peer().address();
    ASSERT_TRUE(Address) << "could not resolve the peer address";
    Host = *Address;

    stopEverything();

    startDaemon();

    auto Exported = BackgroundProcess::start({exportBinary().string(),
                                              "--serve",
                                              Host,
                                              "--nfs-port",
                                              std::to_string(exportPortFor(GetParam())),
                                              "--port",
                                              std::to_string(servicePortFor(GetParam())),
                                              "--backend",
                                              GetParam()});
    ASSERT_TRUE(Exported) << "could not start the export: " << Exported.error().message();
    Export.emplace(std::move(*Exported));
  }

  void TearDown() override { stopEverything(); }

  void startDaemon() {
    auto Served =
        peer().run({serviceBinary().string(), "--serve", Root, "--port", std::to_string(servicePortFor(GetParam())), "--backend", GetParam()});
    ASSERT_TRUE(Served) << "could not start raild on the peer: " << Served.error().message();
    Daemon.emplace(std::move(*Served));
  }

  void stopDaemon() {
    Daemon.reset();
    if (auto Killed = peer().run({"pkill", "-x", "raild"}); Killed) {
      [[maybe_unused]] auto Line = Killed->readLine();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  void stopEverything() {
    Export.reset();
    [[maybe_unused]] auto Local = runLocal({"pkill", "-f", "mount.railnfs --serve"});
    stopDaemon();
  }

  void restartExportAs(const std::string &Target) {
    Export.reset();
    [[maybe_unused]] auto Killed = runLocal({"pkill", "-f", "mount.railnfs --serve"});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto Started = BackgroundProcess::start({exportBinary().string(),
                                             "--serve",
                                             Target,
                                             "--nfs-port",
                                             std::to_string(exportPortFor(GetParam())),
                                             "--port",
                                             std::to_string(servicePortFor(GetParam())),
                                             "--backend",
                                             GetParam()});
    ASSERT_TRUE(Started) << "could not restart the export";
    Export.emplace(std::move(*Started));
  }

  std::vector<std::byte> forged(const std::string &Path) {
    std::vector<std::byte> H(64, std::byte{0});
    H[0] = std::byte{1};
    H[1] = static_cast<std::byte>(Path.size());
    std::memcpy(H.data() + 8, Path.data(), Path.size());
    return H;
  }

  uint32_t statusOfGetAttr(RpcProbe &Probe, std::span<const std::byte> Handle) {
    nfs::XdrWriter Args;
    Args.opaque(Handle);

    auto R = Probe.call(nfs::kNfsProgram, kNfsGetAttr, Args.bytes());
    EXPECT_TRUE(R) << (R ? "" : R.error().message());
    if (!R) return 0;

    nfs::XdrReader Body(R->Body);
    return Body.u32();
  }

  Result<void> connectProbe(RpcProbe &Probe) {
    for (int Attempt = 0; Attempt < 40; Attempt++) {
      if (auto R = Probe.open(exportPortFor(GetParam())); R) return R;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return failMessage("the export never accepted a connection");
  }

  std::vector<std::byte> mountRoot(RpcProbe &Probe) {
    nfs::XdrWriter Args;
    Args.text("/");

    auto R = Probe.call(nfs::kMountProgram, kMountMnt, Args.bytes());
    EXPECT_TRUE(R) << (R ? "" : R.error().message());
    if (!R) return {};
    EXPECT_EQ(R->Accept, 0u);

    nfs::XdrReader Body(R->Body);
    EXPECT_EQ(Body.u32(), 0u) << "the export refused the mount";
    auto Handle = Body.opaque(64);
    EXPECT_TRUE(Body.ok());
    return std::vector<std::byte>(Handle.begin(), Handle.end());
  }

  std::vector<std::byte> lookup(RpcProbe &Probe, std::span<const std::byte> Directory, const std::string &Name, uint32_t &Status) {
    nfs::XdrWriter Args;
    Args.opaque(Directory);
    Args.text(Name);

    auto R = Probe.call(nfs::kNfsProgram, kNfsLookup, Args.bytes());
    EXPECT_TRUE(R) << (R ? "" : R.error().message());
    if (!R) return {};

    nfs::XdrReader Body(R->Body);
    Status = Body.u32();
    if (Status != 0) return {};
    auto Handle = Body.opaque(64);
    EXPECT_TRUE(Body.ok());
    return std::vector<std::byte>(Handle.begin(), Handle.end());
  }

  std::string Root;
  std::string Host;
  std::filesystem::path Alpha;
  std::optional<RemoteProcess> Daemon;
  std::optional<BackgroundProcess> Export;
};

TEST_P(Nfs, MountReturnsARootHandle) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));

  const auto Handle = mountRoot(Probe);
  EXPECT_EQ(Handle.size(), 64u);
}

TEST_P(Nfs, GetAttrReportsTheDirectory) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);

  auto R = Probe.call(nfs::kNfsProgram, kNfsGetAttr, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  EXPECT_EQ(Body.u32(), 0u);
  EXPECT_EQ(Body.u32(), 2u) << "the export root is not a directory";
}

TEST_P(Nfs, LookupWalksIntoASubdirectory) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto Sub = lookup(Probe, RootHandle, "sub", Status);
  ASSERT_EQ(Status, 0u);
  ASSERT_EQ(Sub.size(), 64u);

  const auto Nested = lookup(Probe, Sub, "nested.bin", Status);
  ASSERT_EQ(Status, 0u);
  EXPECT_EQ(Nested.size(), 64u);

  lookup(Probe, RootHandle, "absent.bin", Status);
  EXPECT_EQ(Status, 2u) << "a missing name should be NFS3ERR_NOENT";
}

TEST_P(Nfs, ReadReturnsTheFileBytes) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.u64(0);
  Args.u32(4096);

  auto R = Probe.call(nfs::kNfsProgram, kNfsRead, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  EXPECT_FALSE(Body.boolean());
  EXPECT_EQ(Body.u32(), 4096u);
  EXPECT_TRUE(Body.boolean()) << "reading the whole file should report eof";

  auto Data = Body.opaque(1u << 20);
  ASSERT_TRUE(Body.ok());
  const auto Expected = localBytes(Alpha);
  ASSERT_EQ(Data.size(), Expected.size());
  EXPECT_TRUE(std::equal(Data.begin(), Data.end(), Expected.begin()));

  nfs::XdrWriter Head;
  Head.opaque(File);
  Head.u64(0);
  Head.u32(1024);

  auto Partial = Probe.call(nfs::kNfsProgram, kNfsRead, Head.bytes());
  ASSERT_TRUE(Partial) << Partial.error().message();

  nfs::XdrReader Short(Partial->Body);
  ASSERT_EQ(Short.u32(), 0u);
  Short.boolean();
  EXPECT_EQ(Short.u32(), 1024u);
  EXPECT_FALSE(Short.boolean()) << "a read that stops short of the end is not eof";
}

TEST_P(Nfs, ReadDirPlusListsDotAndEveryEntry) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.u64(0);
  for (int I = 0; I < 2; I++) Args.u32(0);
  Args.u32(8192);
  Args.u32(32768);

  auto R = Probe.call(nfs::kNfsProgram, kNfsReadDirPlus, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  Body.boolean();
  Body.fixed(8);

  std::vector<std::string> Names;
  while (Body.boolean()) {
    Body.u64();
    Names.push_back(Body.text(255));
    Body.u64();
    if (Body.boolean()) {
      Body.u32();
      Body.u32();
      Body.u32();
      Body.u32();
      Body.u32();
      Body.u64();
      Body.u64();
      Body.u32();
      Body.u32();
      Body.u64();
      Body.u64();
      for (int I = 0; I < 6; I++) Body.u32();
    }
    if (Body.boolean()) Body.opaque(64);
    ASSERT_TRUE(Body.ok());
  }
  EXPECT_TRUE(Body.boolean()) << "one call should have returned the whole directory";

  std::ranges::sort(Names);
  EXPECT_EQ(Names, (std::vector<std::string>{".", "..", "alpha.bin", "sub"}));
}

TEST_P(Nfs, ReadDirListsEveryEntryWithoutAttributes) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.u64(0);
  for (int I = 0; I < 2; I++) Args.u32(0);
  Args.u32(32768);

  auto R = Probe.call(nfs::kNfsProgram, kNfsReadDir, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  Body.boolean();
  Body.fixed(8);

  std::vector<std::string> Names;
  uint64_t Last = 0;
  while (Body.boolean()) {
    EXPECT_NE(Body.u64(), 0u) << "every entry needs a fileid";
    Names.push_back(Body.text(255));
    Last = Body.u64();
    ASSERT_TRUE(Body.ok());
  }
  EXPECT_TRUE(Body.boolean());
  EXPECT_EQ(Last, Names.size()) << "the last cookie should name the next entry";

  std::ranges::sort(Names);
  EXPECT_EQ(Names, (std::vector<std::string>{".", "..", "alpha.bin", "sub"}));
}

TEST_P(Nfs, ReadDirResumesFromACookie) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.u64(3);
  for (int I = 0; I < 2; I++) Args.u32(0);
  Args.u32(32768);

  auto R = Probe.call(nfs::kNfsProgram, kNfsReadDir, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  Body.boolean();
  Body.fixed(8);

  std::vector<std::string> Names;
  while (Body.boolean()) {
    Body.u64();
    Names.push_back(Body.text(255));
    Body.u64();
    ASSERT_TRUE(Body.ok());
  }
  EXPECT_TRUE(Body.boolean());
  EXPECT_EQ(Names, (std::vector<std::string>{"sub"})) << "a cookie should skip what was already returned";
}

TEST_P(Nfs, FsInfoAnnouncesAReadSize) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);

  auto R = Probe.call(nfs::kNfsProgram, kNfsFsInfo, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  Body.boolean();
  EXPECT_GE(Body.u32(), 64u << 10) << "rtmax should let a client read in large chunks";
}

TEST_P(Nfs, AccessGrantsReadAndWrite) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.u32(0x3f);

  auto R = Probe.call(nfs::kNfsProgram, kNfsAccess, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  ASSERT_TRUE(Body.boolean());
  for (int I = 0; I < 5; I++) Body.u32();
  Body.u64();
  Body.u64();
  for (int I = 0; I < 2; I++) Body.u32();
  Body.u64();
  Body.u64();
  for (int I = 0; I < 6; I++) Body.u32();

  const uint32_t Granted = Body.u32();
  ASSERT_TRUE(Body.ok());
  EXPECT_TRUE(Granted & 0x0001) << "read";
  EXPECT_TRUE(Granted & 0x0004) << "modify";
  EXPECT_TRUE(Granted & 0x0008) << "extend";
}

// sattr3 with nothing set: four optional fields, then the two times as "do
// not change". What create and setattr carry when only the name matters.
void putNoAttrs(nfs::XdrWriter &Args) {
  for (int I = 0; I < 4; I++) Args.boolean(false);
  Args.u32(0);
  Args.u32(0);
}

TEST_P(Nfs, WriteLandsOnThePeer) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  const std::vector<std::byte> Payload(16, std::byte{0x5a});
  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.u64(0);
  Args.u32(static_cast<uint32_t>(Payload.size()));
  Args.u32(0);
  Args.opaque(Payload);

  auto R = Probe.call(nfs::kNfsProgram, kNfsWrite, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u) << "the export takes writes";

  // wcc_data: neither the before nor the after attributes are sent.
  ASSERT_FALSE(Body.boolean());
  ASSERT_FALSE(Body.boolean());

  EXPECT_EQ(Body.u32(), Payload.size()) << "every byte offered";
  EXPECT_EQ(Body.u32(), 0u) << "answered UNSTABLE, so the client must commit";
  ASSERT_TRUE(Body.ok());

  // The status only says the daemon took it; reading it back says it landed.
  nfs::XdrWriter Back;
  Back.opaque(File);
  Back.u64(0);
  Back.u32(static_cast<uint32_t>(Payload.size()));

  auto Reread = Probe.call(nfs::kNfsProgram, kNfsRead, Back.bytes());
  ASSERT_TRUE(Reread) << Reread.error().message();

  nfs::XdrReader Got(Reread->Body);
  ASSERT_EQ(Got.u32(), 0u);
  Got.boolean();
  ASSERT_EQ(Got.u32(), Payload.size());
  Got.boolean();

  auto Seen = Got.opaque(1u << 20);
  ASSERT_TRUE(Got.ok());
  EXPECT_TRUE(std::equal(Seen.begin(), Seen.end(), Payload.begin()));
}

TEST_P(Nfs, CreateMakesAFileThatLookupFinds) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("made-by-create.bin");
  Args.u32(0);
  putNoAttrs(Args);

  auto R = Probe.call(nfs::kNfsProgram, kNfsCreate, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  ASSERT_TRUE(Body.boolean()) << "a handle for what was made";
  ASSERT_EQ(Body.opaque(64).size(), 64u);

  uint32_t Status = 0;
  const auto Found = lookup(Probe, RootHandle, "made-by-create.bin", Status);
  EXPECT_EQ(Status, 0u) << "lookup should find what create made";
  EXPECT_EQ(Found.size(), 64u);
}

TEST_P(Nfs, CommitSucceeds) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.u64(0);
  Args.u32(0);

  auto R = Probe.call(nfs::kNfsProgram, kNfsCommit, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  EXPECT_EQ(Body.u32(), 0u) << "the commit is what makes a write durable";
}

TEST_P(Nfs, SetAttrTruncates) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Made;
  Made.opaque(RootHandle);
  Made.text("to-truncate.bin");
  Made.u32(0);
  putNoAttrs(Made);
  ASSERT_TRUE(Probe.call(nfs::kNfsProgram, kNfsCreate, Made.bytes()));

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "to-truncate.bin", Status);
  ASSERT_EQ(Status, 0u);

  const std::vector<std::byte> Payload(4096, std::byte{0x7e});
  nfs::XdrWriter Wrote;
  Wrote.opaque(File);
  Wrote.u64(0);
  Wrote.u32(static_cast<uint32_t>(Payload.size()));
  Wrote.u32(0);
  Wrote.opaque(Payload);
  ASSERT_TRUE(Probe.call(nfs::kNfsProgram, kNfsWrite, Wrote.bytes()));

  // Only the size is set, and the guard says not to check the ctime.
  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.boolean(false);
  Args.boolean(false);
  Args.boolean(false);
  Args.boolean(true);
  Args.u64(1024);
  Args.u32(0);
  Args.u32(0);
  Args.boolean(false);

  auto R = Probe.call(nfs::kNfsProgram, kNfsSetAttr, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);

  nfs::XdrWriter Ask;
  Ask.opaque(File);
  auto Stat = Probe.call(nfs::kNfsProgram, kNfsGetAttr, Ask.bytes());
  ASSERT_TRUE(Stat) << Stat.error().message();

  nfs::XdrReader Attrs(Stat->Body);
  ASSERT_EQ(Attrs.u32(), 0u);
  for (int I = 0; I < 5; I++) Attrs.u32();
  EXPECT_EQ(Attrs.u64(), 1024u) << "the size the truncate asked for";
}

TEST_P(Nfs, RemoveDeletesAFile) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Made;
  Made.opaque(RootHandle);
  Made.text("to-remove.bin");
  Made.u32(0);
  putNoAttrs(Made);
  ASSERT_TRUE(Probe.call(nfs::kNfsProgram, kNfsCreate, Made.bytes()));

  uint32_t Status = 0;
  lookup(Probe, RootHandle, "to-remove.bin", Status);
  ASSERT_EQ(Status, 0u) << "it should be there before it is removed";

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("to-remove.bin");

  auto R = Probe.call(nfs::kNfsProgram, kNfsRemove, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);

  lookup(Probe, RootHandle, "to-remove.bin", Status);
  EXPECT_EQ(Status, 2u) << "lookup should answer NFS3ERR_NOENT after a remove";
}

TEST_P(Nfs, MakeDirectoryAndRemoveIt) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("a-directory");
  putNoAttrs(Args);

  auto R = Probe.call(nfs::kNfsProgram, kNfsMakeDirectory, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  ASSERT_TRUE(Body.boolean());
  ASSERT_EQ(Body.opaque(64).size(), 64u);

  uint32_t Status = 0;
  lookup(Probe, RootHandle, "a-directory", Status);
  ASSERT_EQ(Status, 0u) << "lookup should find the new directory";

  nfs::XdrWriter Gone;
  Gone.opaque(RootHandle);
  Gone.text("a-directory");

  auto Removed = Probe.call(nfs::kNfsProgram, kNfsRemoveDirectory, Gone.bytes());
  ASSERT_TRUE(Removed) << Removed.error().message();

  nfs::XdrReader After(Removed->Body);
  ASSERT_EQ(After.u32(), 0u);

  lookup(Probe, RootHandle, "a-directory", Status);
  EXPECT_EQ(Status, 2u) << "and not find it afterwards";
}

TEST_P(Nfs, MakeDirectoryRefusesAnExistingName) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("alpha.bin");
  putNoAttrs(Args);

  auto R = Probe.call(nfs::kNfsProgram, kNfsMakeDirectory, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  EXPECT_EQ(Body.u32(), 17u) << "NFS3ERR_EXIST";

  // The failure reply carries dir_wcc and nothing else, so a reader that
  // expects exactly that finds the end of the message where it should be.
  EXPECT_FALSE(Body.boolean());
  EXPECT_FALSE(Body.boolean());
  EXPECT_TRUE(Body.ok());
  EXPECT_EQ(Body.left(), 0u) << "nothing left over";
}

TEST_P(Nfs, RenameMovesAName) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Made;
  Made.opaque(RootHandle);
  Made.text("before-rename.bin");
  Made.u32(0);
  putNoAttrs(Made);
  ASSERT_TRUE(Probe.call(nfs::kNfsProgram, kNfsCreate, Made.bytes()));

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("before-rename.bin");
  Args.opaque(RootHandle);
  Args.text("after-rename.bin");

  auto R = Probe.call(nfs::kNfsProgram, kNfsRename, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);

  uint32_t Status = 0;
  lookup(Probe, RootHandle, "after-rename.bin", Status);
  EXPECT_EQ(Status, 0u) << "the new name is there";
  lookup(Probe, RootHandle, "before-rename.bin", Status);
  EXPECT_EQ(Status, 2u) << "the old name is not";
}

TEST_P(Nfs, SymlinkIsMadeAndRead) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  nfs::XdrWriter Args;
  Args.opaque(RootHandle);
  Args.text("points-at-alpha");
  putNoAttrs(Args);
  Args.text("alpha.bin");

  auto R = Probe.call(nfs::kNfsProgram, kNfsSymlink, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);
  ASSERT_TRUE(Body.boolean());
  const auto Made = Body.opaque(64);
  ASSERT_EQ(Made.size(), 64u);

  nfs::XdrWriter Ask;
  Ask.opaque(Made);
  auto Read = Probe.call(nfs::kNfsProgram, kNfsReadLink, Ask.bytes());
  ASSERT_TRUE(Read) << Read.error().message();

  nfs::XdrReader Points(Read->Body);
  ASSERT_EQ(Points.u32(), 0u);
  Points.boolean();
  EXPECT_EQ(Points.text(4096), "alpha.bin");
}

TEST_P(Nfs, HardLinkSharesTheFile) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  nfs::XdrWriter Args;
  Args.opaque(File);
  Args.opaque(RootHandle);
  Args.text("another-alpha.bin");

  auto R = Probe.call(nfs::kNfsProgram, kNfsLink, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  ASSERT_EQ(Body.u32(), 0u);

  const auto Linked = lookup(Probe, RootHandle, "another-alpha.bin", Status);
  ASSERT_EQ(Status, 0u) << "the second name is there";

  nfs::XdrWriter Ask;
  Ask.opaque(Linked);
  auto Stat = Probe.call(nfs::kNfsProgram, kNfsGetAttr, Ask.bytes());
  ASSERT_TRUE(Stat) << Stat.error().message();

  nfs::XdrReader Attrs(Stat->Body);
  ASSERT_EQ(Attrs.u32(), 0u);
  for (int I = 0; I < 5; I++) Attrs.u32();
  EXPECT_EQ(Attrs.u64(), localBytes(Alpha).size()) << "and it is the same file";
}

TEST_P(Nfs, StaleHandleIsRefused) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  ASSERT_EQ(mountRoot(Probe).size(), 64u);

  std::vector<std::byte> Bogus(64, std::byte{0});
  Bogus[0] = std::byte{2};
  Bogus[8] = std::byte{0xff};

  nfs::XdrWriter Args;
  Args.opaque(Bogus);

  auto R = Probe.call(nfs::kNfsProgram, kNfsGetAttr, Args.bytes());
  ASSERT_TRUE(R) << R.error().message();

  nfs::XdrReader Body(R->Body);
  EXPECT_EQ(Body.u32(), 70u) << "an unknown handle should be NFS3ERR_STALE";
}

TEST_P(Nfs, ALongPathsHandleResolvesOnEveryConnection) {
  const std::string Long(70, 'n');
  ASSERT_TRUE(peer().makeDirectory(Root + "/" + Long));

  RpcProbe Minting;
  ASSERT_TRUE(connectProbe(Minting));
  const auto RootHandle = mountRoot(Minting);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 1;
  const auto Handle = lookup(Minting, RootHandle, Long, Status);
  ASSERT_EQ(Status, 0u);
  ASSERT_EQ(Handle.size(), 64u);
  ASSERT_EQ(Handle[0], std::byte{2}) << "a path this long should not fit inline";

  for (int I = 0; I < 16; I++) {
    RpcProbe Other;
    ASSERT_TRUE(connectProbe(Other));
    EXPECT_EQ(statusOfGetAttr(Other, Handle), 0u) << "connection " << I << " did not know the handle";
  }
}

TEST_P(Nfs, TooLongNameIsRefused) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  lookup(Probe, RootHandle, std::string(300, 'x'), Status);
  EXPECT_EQ(Status, 63u) << "a name past the limit should be NFS3ERR_NAMETOOLONG";
}

TEST_P(Nfs, ForgedHandleStaysInTheExport) {
  restartExportAs(Host + ":sub");

  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  ASSERT_EQ(mountRoot(Probe).size(), 64u);

  EXPECT_EQ(statusOfGetAttr(Probe, forged("sub/nested.bin")), 0u) << "a handle inside the export should still work, or this proves nothing";
  EXPECT_EQ(statusOfGetAttr(Probe, forged("alpha.bin")), 70u) << "a handle above the exported directory must not resolve";
  EXPECT_EQ(statusOfGetAttr(Probe, forged("../alpha.bin")), 70u) << "a parent reference must not resolve";
}

TEST_P(Nfs, ParentOfTheExportRootIsTheExportRoot) {
  restartExportAs(Host + ":sub");

  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto Up = lookup(Probe, RootHandle, "..", Status);
  ASSERT_EQ(Status, 0u);
  EXPECT_EQ(Up, RootHandle) << "climbing out of the export should land back on it";
}

TEST_P(Nfs, SurvivesTheDaemonRestarting) {
  RpcProbe Probe;
  ASSERT_TRUE(connectProbe(Probe));
  const auto RootHandle = mountRoot(Probe);
  ASSERT_EQ(RootHandle.size(), 64u);

  uint32_t Status = 0;
  const auto File = lookup(Probe, RootHandle, "alpha.bin", Status);
  ASSERT_EQ(Status, 0u);

  stopDaemon();
  EXPECT_EQ(statusOfGetAttr(Probe, File), 5u) << "a dead daemon should answer NFS3ERR_IO, not drop the connection";

  startDaemon();

  uint32_t Recovered = 0;
  for (int Attempt = 0; Attempt < 40; Attempt++) {
    Recovered = statusOfGetAttr(Probe, File);
    if (Recovered == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  EXPECT_EQ(Recovered, 0u) << "the same connection should recover once the daemon is back";
}

INSTANTIATE_TEST_SUITE_P(Backends, Nfs, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e

namespace rail::nfs {
namespace {

std::vector<std::byte> flatten(const XdrPayload &P) {
  std::vector<std::byte> All(P.Body.begin(), P.Body.end());
  All.insert(All.end(), P.Tail.begin(), P.Tail.end());
  All.insert(All.end(), P.Pad, std::byte{0});
  return All;
}

std::vector<std::byte> counted(size_t N) {
  std::vector<std::byte> Data(N);
  for (size_t I = 0; I < N; I++) Data[I] = static_cast<std::byte>(I & 0xff);
  return Data;
}

} // namespace

TEST(Xdr, TailMatchesACopiedOpaqueAtEveryPadding) {
  for (size_t N : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 1023u, 1024u, 4096u}) {
    SCOPED_TRACE("payload " + std::to_string(N));
    const auto Data = counted(N);

    XdrWriter Copied;
    Copied.u32(0x2a);
    Copied.boolean(true);
    Copied.opaque(Data);

    XdrWriter Referenced;
    Referenced.u32(0x2a);
    Referenced.boolean(true);
    Referenced.opaqueTail(Data);

    EXPECT_EQ(Copied.size(), Referenced.size()) << "the record length must not change";
    EXPECT_EQ(flatten(Copied.payload()), flatten(Referenced.payload()));
  }
}

TEST(Xdr, TailIsNotCopiedIntoTheWriter) {
  const auto Data = counted(4096);

  XdrWriter W;
  W.opaqueTail(Data);

  EXPECT_EQ(W.bytes().size(), 4u) << "only the length prefix belongs in the writer";
  EXPECT_EQ(W.payload().Tail.data(), Data.data()) << "the payload must go on the wire from where it already is";
  EXPECT_EQ(W.size(), 4100u);
}

} // namespace rail::nfs
