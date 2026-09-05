#include "harness.h"

#include "rail/file-service.h"
#include "rail/io/runner.h"
#include "rail/proto/message.h"

#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace rail::e2e {

namespace {

uint16_t portFor(const std::string &Backend) { return Backend == "tcp" ? 18711 : 18712; }

} // namespace

class Service : public BackendTest {
protected:
  void SetUp() override {
    Root = remoteDir() + "/service-root";
    removeRemoteRecursive(Root);
    ASSERT_TRUE(peer().makeDirectory(Root));

    Opts.Backend = GetParam();
    Opts.Port = portFor(GetParam());
    Opts.PageSize = 1u << 20;
    Opts.PageCount = 4;

    auto Address = peer().address();
    ASSERT_TRUE(Address) << "could not resolve the peer address";
    Host = *Address;

    ASSERT_NO_FATAL_FAILURE(restartDaemonWith({}));
  }

  void TearDown() override { stopDaemon(); }

  // Closing the ssh channel leaves the daemon running on the peer, so a later
  // start would find the port taken and every test would quietly share the
  // first daemon whatever options it was given.
  void stopDaemon() {
    Daemon.reset();
    stopPeerProcess("raild");
  }

  void restartDaemonWith(std::vector<std::string> Extra) {
    stopDaemon();
    std::vector<std::string> Argv{serviceBinary().string(), "--serve", Root, "--port", std::to_string(Opts.Port), "--backend", Opts.Backend};
    Argv.insert(Argv.end(), Extra.begin(), Extra.end());
    auto Started = peer().run(Argv);
    ASSERT_TRUE(Started) << "could not restart raild";
    Daemon.emplace(std::move(*Started));
  }

  // The daemon needs a moment to bind, and a refused connection says nothing
  // about the protocol, so connecting retries before a test blames the server.
  Result<std::unique_ptr<FileClient>> client() { return clientWith(Opts); }

  Result<std::unique_ptr<FileClient>> clientWith(const ServiceOptions &With) {
    for (int Attempt = 0; Attempt < 40; Attempt++) {
      auto C = run(FileClient::connect(Host, With));
      if (C) return C;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return failMessage("raild never accepted a connection");
  }

  std::string Root;
  std::string Host;
  ServiceOptions Opts;
  std::optional<RemoteProcess> Daemon;
};

TEST_P(Service, StatReportsSize) {
  const auto Local = makeFile("service-stat.bin", 4096, 11);
  seedRemote(Local, Root + "/stat.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Reply = run((*C)->stat("stat.bin"));
  ASSERT_TRUE(Reply) << Reply.error().message();
  EXPECT_TRUE(Reply->Found);
  EXPECT_EQ(Reply->Attrs.Size, 4096u);
  EXPECT_FALSE(Reply->Attrs.Directory);

  auto Missing = run((*C)->stat("absent.bin"));
  ASSERT_TRUE(Missing);
  EXPECT_FALSE(Missing->Found);

  run((*C)->close());
}

TEST_P(Service, ListsEveryEntry) {
  const auto Local = makeFile("service-list.bin", 2048, 12);
  seedRemote(Local, Root + "/one.bin");
  seedRemote(Local, Root + "/two.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Reply = run((*C)->list("."));
  ASSERT_TRUE(Reply) << Reply.error().message();
  EXPECT_TRUE(Reply->Found);

  std::vector<std::string> Names;
  for (const auto &E : Reply->Entries) Names.push_back(E.Name);
  std::sort(Names.begin(), Names.end());
  EXPECT_EQ(Names, (std::vector<std::string>{"one.bin", "two.bin"}));

  run((*C)->close());
}

TEST_P(Service, ReadsAtAnyOffset) {
  const auto Local = makeFile("service-read.bin", 700 * 1024, 13);
  seedRemote(Local, Root + "/read.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Whole(700 * 1024);
  auto Got = run((*C)->read("read.bin", 0, Whole));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(Got->Bytes, Whole.size());

  std::vector<std::byte> Tail(1024);
  auto Middle = run((*C)->read("read.bin", 400 * 1024, Tail));
  ASSERT_TRUE(Middle) << Middle.error().message();
  EXPECT_EQ(Middle->Bytes, Tail.size());
  EXPECT_TRUE(std::equal(Tail.begin(), Tail.end(), Whole.begin() + 400 * 1024)) << "an offset read returned the wrong bytes";

  std::vector<std::byte> Past(64);
  auto End = run((*C)->read("read.bin", 700 * 1024, Past));
  ASSERT_TRUE(End) << End.error().message();
  EXPECT_EQ(End->Bytes, 0u) << "reading past the end should report nothing, not fail";

  run((*C)->close());
}

TEST_P(Service, ReadsFromUnalignedOffsets) {
  const auto Local = makeFile("service-unaligned.bin", 300 * 1024, 31);
  seedRemote(Local, Root + "/unaligned.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Whole(300 * 1024);
  auto All = run((*C)->read("unaligned.bin", 0, Whole));
  ASSERT_TRUE(All) << All.error().message();
  ASSERT_EQ(All->Bytes, Whole.size());

  for (const uint64_t At : {uint64_t{1}, uint64_t{4095}, uint64_t{4097}, uint64_t{100000}}) {
    std::vector<std::byte> Part(1234);
    auto Got = run((*C)->read("unaligned.bin", At, Part));
    ASSERT_TRUE(Got) << Got.error().message();
    ASSERT_EQ(Got->Bytes, Part.size()) << "short read at offset " << At;
    EXPECT_TRUE(std::equal(Part.begin(), Part.end(), Whole.begin() + static_cast<long>(At))) << "offset " << At << " returned the wrong bytes";
  }

  std::vector<std::byte> Tail(8192);
  auto End = run((*C)->read("unaligned.bin", 300 * 1024 - 100, Tail));
  ASSERT_TRUE(End) << End.error().message();
  EXPECT_EQ(End->Bytes, 100u) << "a read running past the end should stop at it";
  EXPECT_TRUE(std::equal(Tail.begin(), Tail.begin() + 100, Whole.end() - 100));

  run((*C)->close());
}

TEST_P(Service, ReadsAWholePageFromAnUnalignedOffset) {
  const auto Local = makeFile("service-page.bin", 2u << 20, 33);
  seedRemote(Local, Root + "/page.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  // The largest read the client may post, from an offset that is not aligned:
  // the peer must send exactly what was asked for or the receive never fills.
  std::vector<std::byte> Page((*C)->maxTransfer());
  auto Got = run((*C)->read("page.bin", 1, Page));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(Got->Bytes, Page.size());

  std::ifstream Source(Local, std::ios::binary);
  Source.seekg(1);
  std::vector<std::byte> Expected(Page.size());
  Source.read(reinterpret_cast<char *>(Expected.data()), static_cast<std::streamsize>(Expected.size()));
  EXPECT_EQ(Page, Expected) << "a full page read at offset 1 returned the wrong bytes";

  run((*C)->close());
}

TEST_P(Service, WriteLandsOnPeer) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Payload(64 * 1024);
  for (size_t I = 0; I < Payload.size(); I++) Payload[I] = static_cast<std::byte>(I * 7);

  ASSERT_TRUE(run((*C)->write("written.bin", 0, Payload)));

  std::vector<std::byte> Back(Payload.size());
  auto Got = run((*C)->read("written.bin", 0, Back));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(Got->Bytes, Payload.size());
  EXPECT_EQ(Back, Payload) << "what came back is not what went out";

  EXPECT_TRUE(peer().exists(Root + "/written.bin").value_or(false));
  run((*C)->close());
}

TEST_P(Service, RefusesPathOutsideRoot) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Escape = run((*C)->stat("../escape.bin"));
  ASSERT_TRUE(Escape);
  EXPECT_FALSE(Escape->Found) << "a parent reference escaped the served directory";

  auto Absolute = run((*C)->stat("/etc/passwd"));
  ASSERT_TRUE(Absolute);
  EXPECT_FALSE(Absolute->Found) << "an absolute path escaped the served directory";

  run((*C)->close());
}

TEST_P(Service, RefusesSymlinkOutsideRoot) {
  {
    auto Linked = peer().run({"ln", "-s", "/etc/hostname", Root + "/escape.txt"});
    ASSERT_TRUE(Linked) << "could not create the symlink fixture";
    [[maybe_unused]] auto Line = Linked->readLine();
  }

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Escape = run((*C)->stat("escape.txt"));
  ASSERT_TRUE(Escape);
  EXPECT_FALSE(Escape->Found) << "a symlink read a file outside the served directory";

  std::vector<std::byte> Into(64);
  auto Got = run((*C)->read("escape.txt", 0, Into));
  EXPECT_FALSE(Got) << "a symlink served bytes from outside the root";

  run((*C)->close());
}

TEST_P(Service, TruncatingWriteShrinksFile) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Long(8192, std::byte{0xAB});
  auto Wide = run((*C)->write("replace.bin", 0, Long, true));
  ASSERT_TRUE(Wide) << Wide.error().message();

  std::vector<std::byte> Short(512, std::byte{0xCD});
  auto Narrow = run((*C)->write("replace.bin", 0, Short, true));
  ASSERT_TRUE(Narrow) << Narrow.error().message();

  auto After = run((*C)->stat("replace.bin"));
  ASSERT_TRUE(After);
  EXPECT_EQ(After->Attrs.Size, Short.size()) << "the tail of the longer file survived";

  run((*C)->close());
}

TEST_P(Service, FetchMatchesSource) {
  const auto Local = makeFile("service-fetch.bin", 6u << 20, 21);
  seedRemote(Local, Root + "/fetch.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  const auto Landed = localDir() / "service-fetched.bin";
  std::filesystem::remove(Landed);

  auto Got = run((*C)->fetch("fetch.bin", Landed));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(*Got, std::filesystem::file_size(Local));
  EXPECT_EQ(localDigest(Landed), localDigest(Local)) << "a streamed fetch did not reproduce the file";

  run((*C)->close());
}

TEST_P(Service, StoreLandsOnPeer) {
  const auto Local = makeFile("service-store.bin", 5u << 20, 22);

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Put = run((*C)->store(Local, "stored.bin"));
  ASSERT_TRUE(Put) << Put.error().message();
  EXPECT_EQ(*Put, std::filesystem::file_size(Local));
  EXPECT_EQ(peer().digest(Root + "/stored.bin").value_or(""), localDigest(Local)) << "a streamed store did not reproduce the file";

  run((*C)->close());
}

TEST_P(Service, FetchCatchesBadData) {
  const auto Local = makeFile("service-corrupt.bin", 4u << 20, 31);
  seedRemote(Local, Root + "/corrupt.bin");

  restartDaemonWith({"--flip-one-bit"});

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  const auto Landed = localDir() / "service-corrupt-landed.bin";
  std::filesystem::remove(Landed);

  auto Got = run((*C)->fetch("corrupt.bin", Landed));
  EXPECT_FALSE(Got) << "a flipped bit went unnoticed";
  if (!Got) EXPECT_NE(Got.error().message().find("hash mismatch"), std::string::npos) << Got.error().message();
  EXPECT_FALSE(std::filesystem::exists(Landed)) << "a corrupt transfer still left a file behind";
}

TEST_P(Service, StoreCatchesBadData) {
  const auto Local = makeFile("service-badstore.bin", 4u << 20, 41);

  ServiceOptions Bad = Opts;
  Bad.FlipOneBit = true;
  auto C = clientWith(Bad);
  ASSERT_TRUE(C) << C.error().message();

  auto Put = run((*C)->store(Local, "badstore.bin"));
  EXPECT_FALSE(Put) << "the peer accepted a corrupted store";
  if (!Put) EXPECT_NE(Put.error().message().find("hash mismatch"), std::string::npos) << Put.error().message();
  EXPECT_FALSE(peer().exists(Root + "/badstore.bin").value_or(false)) << "a corrupted store still landed a file";

  run((*C)->close());
}

TEST_P(Service, FetchIntoMemoryMatchesSource) {
  const uint64_t Size = 6u << 20;
  const auto Local = makeFile("service-fetch-into.bin", Size, 61);
  seedRemote(Local, Root + "/fetch-into.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Into(Size);
  auto Got = run((*C)->fetchInto("fetch-into.bin", 0, Into));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(*Got, Size);

  std::ifstream In(Local, std::ios::binary);
  std::vector<std::byte> Want(Size);
  In.read(reinterpret_cast<char *>(Want.data()), static_cast<std::streamsize>(Size));
  EXPECT_TRUE(Into == Want) << "a streamed fetch into memory did not reproduce the file";

  run((*C)->close());
}

TEST_P(Service, FetchIntoMemoryHonoursAnOffset) {
  const uint64_t Size = 6u << 20;
  const uint64_t At = 2u << 20;
  const auto Local = makeFile("service-fetch-range.bin", Size, 62);
  seedRemote(Local, Root + "/fetch-range.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Into(static_cast<size_t>(Size - At));
  auto Got = run((*C)->fetchInto("fetch-range.bin", At, Into));
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(*Got, Size - At);

  std::ifstream In(Local, std::ios::binary);
  In.seekg(static_cast<std::streamoff>(At));
  std::vector<std::byte> Want(static_cast<size_t>(Size - At));
  In.read(reinterpret_cast<char *>(Want.data()), static_cast<std::streamsize>(Want.size()));
  EXPECT_TRUE(Into == Want) << "a ranged streamed fetch did not reproduce the tail";

  run((*C)->close());
}

TEST_P(Service, FetchIntoRefusesAnUnalignedOffset) {
  const auto Local = makeFile("service-fetch-odd.bin", 4u << 20, 63);
  seedRemote(Local, Root + "/fetch-odd.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Into(4096);
  auto Got = run((*C)->fetchInto("fetch-odd.bin", 1234, Into));
  ASSERT_FALSE(Got) << "an unaligned streamed fetch was accepted";
  EXPECT_NE(Got.error().message().find("boundary"), std::string::npos)
      << "an unaligned fetch failed, but not with a message naming the alignment: " << Got.error().message();

  run((*C)->close());
}

TEST_P(Service, StoreFromMemoryLandsOnPeer) {
  const uint64_t Size = 5u << 20;
  const auto Local = makeFile("service-store-from.bin", Size, 64);

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::ifstream In(Local, std::ios::binary);
  std::vector<std::byte> From(Size);
  In.read(reinterpret_cast<char *>(From.data()), static_cast<std::streamsize>(Size));

  auto Put = run((*C)->storeFrom(From, "stored-from.bin", 0, true));
  ASSERT_TRUE(Put) << Put.error().message();
  EXPECT_EQ(*Put, Size);
  EXPECT_EQ(peer().digest(Root + "/stored-from.bin").value_or(""), localDigest(Local)) << "a streamed store from memory did not reproduce the file";

  run((*C)->close());
}

TEST_P(Service, StoreFromMemoryAtAnOffset) {
  const uint64_t Size = 4u << 20;
  const uint64_t At = 1u << 20;
  const auto Local = makeFile("service-store-at.bin", Size, 81);
  seedRemote(Local, Root + "/store-at.bin");

  const std::vector<std::byte> Patch(static_cast<size_t>(Size - At), std::byte{0x5c});

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Put = run((*C)->storeFrom(Patch, "store-at.bin", At, false));
  ASSERT_TRUE(Put) << Put.error().message();
  EXPECT_EQ(*Put, Patch.size());

  std::fstream Merge(Local, std::ios::binary | std::ios::in | std::ios::out);
  Merge.seekp(static_cast<std::streamoff>(At));
  Merge.write(reinterpret_cast<const char *>(Patch.data()), static_cast<std::streamsize>(Patch.size()));
  Merge.close();

  EXPECT_EQ(peer().digest(Root + "/store-at.bin").value_or(""), localDigest(Local)) << "a streamed store at an offset did not land correctly";

  run((*C)->close());
}

TEST_P(Service, StoreFromMemoryAtZeroKeepsTheTail) {
  const uint64_t Size = 4u << 20;
  const auto Local = makeFile("service-store-head.bin", Size, 82);
  seedRemote(Local, Root + "/store-head.bin");

  const std::vector<std::byte> Head(1u << 20, std::byte{0x3d});

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Put = run((*C)->storeFrom(Head, "store-head.bin", 0, false));
  ASSERT_TRUE(Put) << Put.error().message();
  EXPECT_EQ(*Put, Head.size());

  std::fstream Merge(Local, std::ios::binary | std::ios::in | std::ios::out);
  Merge.write(reinterpret_cast<const char *>(Head.data()), static_cast<std::streamsize>(Head.size()));
  Merge.close();

  EXPECT_EQ(peer().digest(Root + "/store-head.bin").value_or(""), localDigest(Local)) << "a non-truncating store at offset 0 replaced the whole file";

  run((*C)->close());
}

TEST_P(Service, TruncatingStoreAtAnOffsetIsRefused) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  const std::vector<std::byte> Body(4096, std::byte{1});
  auto Put = run((*C)->storeFrom(Body, "bad-truncate.bin", 4096, true));
  EXPECT_FALSE(Put) << "a truncating store at a non-zero offset was accepted";

  run((*C)->close());
}

TEST_P(Service, RefusesAMismatchedWireVersion) {
  ServiceOptions Old = Opts;
  Old.PretendVersion = proto::kVersion + 1;

  auto C = clientWith(Old);
  EXPECT_FALSE(C) << "the daemon accepted a client claiming a different wire version";
}

TEST_P(Service, FetchIntoCatchesBadData) {
  const auto Local = makeFile("service-fetch-corrupt.bin", 4u << 20, 65);
  seedRemote(Local, Root + "/fetch-corrupt.bin");

  restartDaemonWith({"--flip-one-bit"});

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Into(4u << 20);
  auto Got = run((*C)->fetchInto("fetch-corrupt.bin", 0, Into));
  EXPECT_FALSE(Got) << "a flipped bit went unnoticed on a fetch into memory";
  if (!Got) EXPECT_NE(Got.error().message().find("hash mismatch"), std::string::npos) << Got.error().message();
}

TEST_P(Service, FsyncReachesThePeer) {
  const auto Local = makeFile("service-fsync.bin", 64u << 10, 71);

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::ifstream In(Local, std::ios::binary);
  std::vector<std::byte> Body(static_cast<size_t>(std::filesystem::file_size(Local)));
  In.read(reinterpret_cast<char *>(Body.data()), static_cast<std::streamsize>(Body.size()));

  auto Wrote = run((*C)->write("synced.bin", 0, Body, true));
  ASSERT_TRUE(Wrote) << Wrote.error().message();

  auto Synced = run((*C)->fsync("synced.bin"));
  ASSERT_TRUE(Synced) << Synced.error().message();

  EXPECT_EQ(peer().digest(Root + "/synced.bin").value_or(""), localDigest(Local));

  run((*C)->close());
}

TEST_P(Service, FsyncOnAMissingPathFails) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Synced = run((*C)->fsync("no-such-file.bin"));
  EXPECT_FALSE(Synced) << "fsync on a missing path reported success, so it is not reaching the file at all";

  run((*C)->close());
}

TEST_P(Service, FsyncStaysInRoot) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Synced = run((*C)->fsync("../escape.bin"));
  EXPECT_FALSE(Synced) << "fsync resolved a path outside the served directory";

  run((*C)->close());
}

TEST_P(Service, ZeroLengthWriteCreatesAnEmptyFile) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Wrote = run((*C)->write("empty.bin", 0, {}, true));
  ASSERT_TRUE(Wrote) << Wrote.error().message();

  EXPECT_TRUE(peer().exists(Root + "/empty.bin").value_or(false)) << "a zero length write did not create the file";
  auto Info = peer().stat(Root + "/empty.bin");
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Size, 0u);

  run((*C)->close());
}

TEST_P(Service, PipelinedReadsMatch) {
  const auto Local = makeFile("service-pipe.bin", 512u << 10, 51);
  seedRemote(Local, Root + "/pipe.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  constexpr size_t kBlock = 64 << 10;
  const size_t Blocks = (512u << 10) / kBlock;
  const size_t Window = std::min<size_t>(Blocks, (*C)->maxOutstanding());

  std::vector<std::vector<std::byte>> Landing(Blocks, std::vector<std::byte>(kBlock));
  size_t Asked = 0;
  size_t Got = 0;
  while (Got < Blocks) {
    while (Asked < Blocks && Asked - Got < Window) {
      ASSERT_TRUE(run((*C)->submitRead("pipe.bin", Asked * kBlock, Landing[Asked])));
      Asked++;
    }
    auto Read = run((*C)->collectRead());
    ASSERT_TRUE(Read) << Read.error().message();
    EXPECT_EQ(Read->Bytes, kBlock);
    Got++;
  }

  std::vector<std::byte> Whole;
  for (const auto &Block : Landing) Whole.insert(Whole.end(), Block.begin(), Block.end());

  std::ifstream Source(Local, std::ios::binary);
  std::vector<std::byte> Expected(512u << 10);
  Source.read(reinterpret_cast<char *>(Expected.data()), static_cast<std::streamsize>(Expected.size()));

  EXPECT_EQ(Whole, Expected) << "pipelined reads did not reassemble the file";
  run((*C)->close());
}

TEST_P(Service, MoreReadsThanReceiveSlots) {
  constexpr size_t kBlock = 4096;
  constexpr size_t kBlocks = 64;
  constexpr size_t kReads = 1100;

  const auto Local = makeFile("service-slots.bin", kBlock * kBlocks, 53);
  seedRemote(Local, Root + "/slots.bin");
  const auto Expected = localBytes(Local);

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Block(kBlock);
  for (size_t I = 0; I < kReads; I++) {
    const size_t At = (I % kBlocks) * kBlock;
    auto Got = run((*C)->read("slots.bin", At, Block));
    ASSERT_TRUE(Got) << "read " << I << ": " << Got.error().message();
    ASSERT_EQ(Got->Bytes, kBlock);
    EXPECT_TRUE(std::equal(Block.begin(), Block.end(), Expected.begin() + static_cast<long>(At))) << "read " << I << " returned the wrong bytes";
  }

  run((*C)->close());
}

TEST_P(Service, MakesAndRemovesDirs) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  ASSERT_TRUE(run((*C)->makeDirectory("newdir", 0755)));
  auto Made = run((*C)->stat("newdir"));
  ASSERT_TRUE(Made);
  EXPECT_TRUE(Made->Found);
  EXPECT_TRUE(Made->Attrs.Directory);

  ASSERT_TRUE(run((*C)->removeDirectory("newdir")));
  auto Gone = run((*C)->stat("newdir"));
  ASSERT_TRUE(Gone);
  EXPECT_FALSE(Gone->Found);

  run((*C)->close());
}

TEST_P(Service, RemovesFile) {
  const auto Local = makeFile("service-unlink.bin", 4096, 61);
  seedRemote(Local, Root + "/unlink.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  ASSERT_TRUE(run((*C)->removeFile("unlink.bin")));
  EXPECT_FALSE(peer().exists(Root + "/unlink.bin").value_or(true));

  auto Again = run((*C)->removeFile("unlink.bin"));
  EXPECT_FALSE(Again) << "removing a missing file should fail";

  run((*C)->close());
}

TEST_P(Service, RenamesFile) {
  const auto Local = makeFile("service-rename.bin", 8192, 62);
  seedRemote(Local, Root + "/before.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  ASSERT_TRUE(run((*C)->rename("before.bin", "after.bin")));
  EXPECT_FALSE(peer().exists(Root + "/before.bin").value_or(true));
  EXPECT_EQ(peer().digest(Root + "/after.bin").value_or(""), localDigest(Local));

  auto Escape = run((*C)->rename("after.bin", "../escaped.bin"));
  EXPECT_FALSE(Escape) << "rename let a file out of the served directory";

  run((*C)->close());
}

TEST_P(Service, TruncatesFile) {
  const auto Local = makeFile("service-trunc.bin", 16384, 63);
  seedRemote(Local, Root + "/trunc.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  ASSERT_TRUE(run((*C)->truncate("trunc.bin", 4096)));
  auto Info = run((*C)->stat("trunc.bin"));
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Attrs.Size, 4096u);

  ASSERT_TRUE(run((*C)->truncate("trunc.bin", 20000)));
  Info = run((*C)->stat("trunc.bin"));
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Attrs.Size, 20000u) << "growing truncate should extend the file";

  run((*C)->close());
}

TEST_P(Service, SetsModeAndTime) {
  const auto Local = makeFile("service-attrs.bin", 2048, 64);
  seedRemote(Local, Root + "/attrs.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  ASSERT_TRUE(run((*C)->setMode("attrs.bin", 0640)));
  ASSERT_TRUE(run((*C)->setMtime("attrs.bin", 1000000000)));

  auto Info = run((*C)->stat("attrs.bin"));
  ASSERT_TRUE(Info);
  EXPECT_EQ(Info->Attrs.Mode, 0640u);
  EXPECT_EQ(Info->Attrs.Mtime, 1000000000);

  run((*C)->close());
}

TEST_P(Service, ReportsDiskSpace) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  auto Fs = run((*C)->statFs("."));
  ASSERT_TRUE(Fs) << Fs.error().message();
  EXPECT_GT(Fs->BlockSize, 0u);
  EXPECT_GT(Fs->Blocks, 0u);
  EXPECT_LE(Fs->BlocksFree, Fs->Blocks);

  run((*C)->close());
}

TEST_P(Service, MetadataStaysInRoot) {
  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  EXPECT_FALSE(run((*C)->makeDirectory("../outside", 0755))) << "mkdir escaped the root";
  EXPECT_FALSE(run((*C)->removeFile("/etc/hostname"))) << "unlink escaped the root";
  EXPECT_FALSE(run((*C)->truncate("../outside.bin", 0))) << "truncate escaped the root";

  run((*C)->close());
}

TEST_P(Service, MixesReadsWithOtherCalls) {
  const auto Local = makeFile("service-mix.bin", 128 << 10, 71);
  seedRemote(Local, Root + "/mix.bin");

  auto C = client();
  ASSERT_TRUE(C) << C.error().message();

  std::vector<std::byte> Into(64 << 10);
  ASSERT_TRUE(run((*C)->submitRead("mix.bin", 0, Into)));

  auto Info = run((*C)->stat("mix.bin"));
  ASSERT_TRUE(Info) << "stat should answer while a read is in flight";
  EXPECT_EQ(Info->Attrs.Size, 128u << 10);

  ASSERT_TRUE(run((*C)->makeDirectory("mixdir"))) << "mkdir should answer while a read is in flight";

  auto Got = run((*C)->collectRead());
  ASSERT_TRUE(Got) << Got.error().message();
  EXPECT_EQ(Got->Bytes, Into.size());

  std::ifstream Source(Local, std::ios::binary);
  std::vector<std::byte> Expected(Into.size());
  Source.read(reinterpret_cast<char *>(Expected.data()), static_cast<std::streamsize>(Expected.size()));
  EXPECT_EQ(Into, Expected) << "the read returned the wrong bytes once other calls interleaved";

  run((*C)->close());
}

TEST_P(Service, HarnessSurvivesADroppedSession) {
  const auto Local = makeFile("service-drop.bin", 4096, 81);
  seedRemote(Local, Root + "/drop.bin");
  ASSERT_TRUE(peer().exists(Root + "/drop.bin").value_or(false));

  peer().dropForTest();

  EXPECT_TRUE(peer().exists(Root + "/drop.bin").value_or(false)) << "the harness did not recover from a dropped session";
  EXPECT_EQ(peer().digest(Root + "/drop.bin").value_or(""), localDigest(Local));

  peer().dropForTest();
  auto Info = peer().stat(Root + "/drop.bin");
  EXPECT_TRUE(Info) << "stat did not recover from a dropped session";
  EXPECT_EQ(Info.value_or(RemoteStat{}).Size, 4096u);
}

INSTANTIATE_TEST_SUITE_P(Backends, Service, ::testing::ValuesIn(kBackends), [](const auto &I) { return I.param; });

} // namespace rail::e2e
