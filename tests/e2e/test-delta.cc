#include "harness.h"

#include <gtest/gtest.h>

namespace rail::e2e {

// These assert on transferred-byte counters, not just file equality. Without
// that, every one of them passes against a dumb whole-file copy and the delta
// engine is never actually exercised.
class Delta : public ::testing::TestWithParam<std::string> {
protected:
  // Big enough for a few thousand blocks, small enough that seeding it over
  // sftp does not dominate the suite. Thresholds below are fractions of it so
  // the tests keep their meaning if it changes.
  static constexpr uint64_t kSize = 8u << 20;

  PushOptions alwaysDelta() const { return {GetParam(), {"--no-whole-file"}}; }
};

TEST_P(Delta, IdenticalDestinationSendsNoLiterals) {
  const auto Local = makeFile("same.bin", kSize, 7);
  const auto Remote = remotePath("same.bin");
  seedRemote(Local, Remote);

  const Report R = push(Local, Remote, alwaysDelta());

  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
  EXPECT_EQ(R.LiteralBytes, 0u);
  EXPECT_EQ(R.MatchedBytes, kSize);
}

TEST_P(Delta, SingleEditedBlockSendsOnlyThatBlock) {
  const auto Local = makeFile("edit.bin", kSize, 11);
  const auto Remote = remotePath("edit.bin");
  seedRemote(Local, Remote);

  overwriteLocal(Local, kSize / 2, 64);

  const Report R = push(Local, Remote, alwaysDelta());

  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
  EXPECT_GT(R.MatchedBytes, kSize - kSize / 16);
  EXPECT_LT(R.LiteralBytes, kSize / 16);
}

// Only a rolling checksum can still match blocks whose byte offsets have all
// shifted by one. A fixed-block differ would resend the entire file here.
TEST_P(Delta, InsertedByteStillMatchesMostBlocks) {
  const auto Local = makeFile("shift.bin", kSize, 13);
  const auto Remote = remotePath("shift.bin");
  seedRemote(Local, Remote);

  prependByteLocal(Local, std::byte{0x5a});

  const Report R = push(Local, Remote, alwaysDelta());

  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
  EXPECT_GT(R.MatchedBytes, kSize - kSize / 8);
}

INSTANTIATE_TEST_SUITE_P(Backends, Delta, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
