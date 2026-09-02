#include "harness.h"

#include "rail/app/signature.h"

#include <chrono>
#include <gtest/gtest.h>

namespace rail::e2e {

class Failure : public BackendTest {};

TEST_P(Failure, MissingDestinationDirectoryFails) {
  const auto Local = makeFile("fail-nodir.bin", 1u << 20, 21);
  const std::string Missing = remoteDir() + "/nope";
  const std::string Remote = Missing + "/fail-nodir.bin";

  // The directory has to be absent for this to mean anything, and an earlier
  // run may have created it.
  ASSERT_TRUE(peer().removeFile(Remote));
  ASSERT_TRUE(peer().removeDirectory(Missing));

  const FailedPush R = pushExpectingFailure(Local, Remote, options());

  EXPECT_NE(R.ExitStatus, 0);
  EXPECT_FALSE(peer().exists(Remote).value_or(false));
  EXPECT_FALSE(remoteHasTempFiles()) << "a failed push left a temp file behind";
}

TEST_P(Failure, CorruptedPayloadIsDetected) {
  const auto Seed = makeFile("fail-corrupt-seed.bin", 4u << 20, 22);
  const auto Remote = remotePath("fail-corrupt.bin");
  seedRemote(Seed, Remote);
  const std::string Before = peer().digest(Remote).value_or("");
  ASSERT_FALSE(Before.empty());

  // Delta is off so the whole file travels as literals, guaranteeing there is
  // a literal to corrupt.
  const auto Local = makeFile("fail-corrupt.bin", 4u << 20, 23);
  const FailedPush R = pushExpectingFailure(Local, Remote, options({"-W", "--fault-inject-flip-literal"}));

  EXPECT_NE(R.ExitStatus, 0);
  EXPECT_NE(R.Output.find("hash mismatch"), std::string::npos) << "expected the whole-file check to catch it:\n" << R.Output;
  EXPECT_EQ(peer().digest(Remote).value_or(""), Before) << "a rejected transfer must leave the destination untouched";
  EXPECT_FALSE(remoteHasTempFiles());
}

TEST_P(Failure, KilledSenderLeavesNoPartialFile) {
  const auto Seed = makeFile("fail-killed-seed.bin", 4u << 20, 24);
  const auto Remote = remotePath("fail-killed.bin");
  seedRemote(Seed, Remote);
  const std::string Before = peer().digest(Remote).value_or("");
  ASSERT_FALSE(Before.empty());

  const auto Local = makeFile("fail-killed.bin", 512u << 20, 25);

  // Small pages and a shallow pool on purpose. The destination lands in the
  // peer's page cache, so this crosses in a few hundred milliseconds however
  // large it is made - catching it halfway is a question of how quickly the
  // watcher notices, not of how much there is to send.
  ASSERT_TRUE(pushThenKillOnceWriting(Local, Remote, options({"-W", "--pages=2", "--page-size=1M"})))
      << "the peer never began writing, so nothing was interrupted";

  // The destination is only renamed into place after the digest matches, so a
  // sender that dies mid-transfer must leave the original exactly as it was.
  EXPECT_EQ(peer().digest(Remote).value_or(""), Before);
  EXPECT_FALSE(remoteHasTempFiles()) << "a killed sender left a temp file behind";
}

TEST_P(Failure, PathologicalHashBucketStillTransfersCorrectly) {
  // Every block is identical, so all 4096 weak checksums collide into one
  // bucket. Without the MaxChainLen bound the scan degrades to quadratic.
  const uint32_t Block = chooseBlockLength(4u << 20, 1u << 17);
  const auto Seed = makeRepeatingFile("fail-collide-seed.bin", Block, 4096);
  const auto Remote = remotePath("fail-collide.bin");
  seedRemote(Seed, Remote);

  const auto Local = makeRepeatingFile("fail-collide.bin", Block, 4096);
  overwriteLocal(Local, uint64_t(Block) * 2048, 64);

  const auto Started = std::chrono::steady_clock::now();
  push(Local, Remote, options({"--no-whole-file"}));
  const auto Elapsed = std::chrono::steady_clock::now() - Started;

  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(Elapsed).count(), 60) << "the hash chain bound is not holding";
  EXPECT_FALSE(remoteHasTempFiles());
}

INSTANTIATE_TEST_SUITE_P(Backends, Failure, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
