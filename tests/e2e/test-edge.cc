#include "harness.h"

#include "rail/app/signature.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

namespace rail::e2e {

namespace {

// The sizes below are chosen against the block length the sender will actually
// pick, so "exactly one block" stays true if the sizing rule changes.
uint32_t blockLengthFor(uint64_t FileSize) { return chooseBlockLength(FileSize, 1u << 17); }

} // namespace

class Edge : public BackendTest {
protected:
  // Pushes and asserts the destination is byte-identical to the source.
  void expectIdentical(const std::filesystem::path &Local, const std::string &Remote) {
    EXPECT_TRUE(peer().exists(Remote).value_or(false));
    EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
    EXPECT_EQ(peer().stat(Remote).transform([](const RemoteStat &S) { return S.Size; }).value_or(~0ull), std::filesystem::file_size(Local));
    EXPECT_FALSE(remoteHasTempFiles());
  }
};

TEST_P(Edge, EmptyFile) {
  const auto Local = makeEmptyFile("edge-empty.bin");
  const auto Remote = remotePath("edge-empty.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, options());

  EXPECT_EQ(R.FileSize, 0u);
  EXPECT_EQ(R.LiteralBytes, 0u);
  expectIdentical(Local, Remote);
}

TEST_P(Edge, SmallerThanOneBlock) {
  const auto Local = makeFile("edge-tiny.bin", 100, 7);
  const auto Remote = remotePath("edge-tiny.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, options());

  EXPECT_EQ(R.LiteralBytes, 100u);
  expectIdentical(Local, Remote);
}

TEST_P(Edge, ExactlyOneBlock) {
  const uint32_t Block = blockLengthFor(700 * 700);
  const auto Local = makeFile("edge-oneblock.bin", Block, 8);
  const auto Remote = remotePath("edge-oneblock.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  push(Local, Remote, options());
  expectIdentical(Local, Remote);
}

TEST_P(Edge, ExactlyNBlocks) {
  const uint32_t Block = blockLengthFor(64u << 20);
  const auto Local = makeFile("edge-nblocks.bin", uint64_t(Block) * 64, 9);
  const auto Remote = remotePath("edge-nblocks.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  push(Local, Remote, options());
  expectIdentical(Local, Remote);

  // Sending it again must match every block, including the last: a file that
  // is an exact multiple of the block length has no short trailing block. The
  // delta is asked for, since it is not the default.
  const Report Again = push(Local, Remote, options({"--no-whole-file"}));
  EXPECT_EQ(Again.LiteralBytes, 0u);
  EXPECT_EQ(Again.MatchedBytes, uint64_t(Block) * 64);
}

TEST_P(Edge, DestinationLongerThanSource) {
  const auto Seed = makeFile("edge-long-seed.bin", 32u << 20, 11);
  const auto Remote = remotePath("edge-shrink.bin");
  seedRemote(Seed, Remote);

  // Auto declines a delta when the two sizes differ by more than 2x, so the
  // policy is forced here: this case is about the truncation, not the policy.
  const auto Local = makeFile("edge-shrink.bin", 8u << 20, 11);
  push(Local, Remote, options({"--no-whole-file"}));

  // The destination must be truncated to the source length, not left long.
  expectIdentical(Local, Remote);
}

TEST_P(Edge, DestinationShorterThanSource) {
  const auto Seed = makeFile("edge-short-seed.bin", 8u << 20, 12);
  const auto Remote = remotePath("edge-grow.bin");
  seedRemote(Seed, Remote);

  const auto Local = makeFile("edge-grow.bin", 32u << 20, 12);
  const Report R = push(Local, Remote, options({"--no-whole-file"}));

  EXPECT_GT(R.MatchedBytes, 0u) << "the shared prefix should have matched";
  expectIdentical(Local, Remote);
}

TEST_P(Edge, PreservesModeAndMtime) {
  const auto Local = makeFile("edge-mode.bin", 1u << 20, 13);
  setLocalMode(Local, 0640);
  const auto Remote = remotePath("edge-mode.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  push(Local, Remote, options());

  auto Stat = peer().stat(Remote);
  ASSERT_TRUE(Stat.has_value()) << "destination is missing";
  EXPECT_EQ(Stat->Mode & 0777u, 0640u);

  // std::filesystem::last_write_time counts from an unspecified epoch, so the
  // comparison comes from stat, which is what the wire actually carries.
  struct ::stat Source{};
  ASSERT_EQ(::stat(Local.c_str(), &Source), 0) << "cannot stat the source";
  EXPECT_NE(Stat->Mtime, 0) << "mtime was not carried over";
  EXPECT_LT(std::abs(Stat->Mtime - Source.st_mtime), 3) << "mtime drifted by more than rounding";
}

// Durability is opt-in, and the flag has to survive the trip through the ssh
// command line into the receiver. It silently did not once, when a move
// constructor dropped the member.
TEST_P(Edge, FsyncStillTransfersCorrectly) {
  const auto Local = makeFile("edge-fsync.bin", 4u << 20, 14);
  const auto Remote = remotePath("edge-fsync.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, options({"--fsync"}));

  EXPECT_EQ(R.LiteralBytes, 4u << 20);
  expectIdentical(Local, Remote);
}

INSTANTIATE_TEST_SUITE_P(Backends, Edge, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
