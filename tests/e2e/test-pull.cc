#include "harness.h"

#include <gtest/gtest.h>

namespace rail::e2e {

class Pull : public BackendTest {};

TEST_P(Pull, BringsAWholeFileBack) {
  const auto Seed = makeFile("pulled.bin", 8u << 20, 7);
  const auto Remote = remotePath("pulled.bin");
  seedRemote(Seed, Remote);

  const auto Landing = localDir() / "came-back.bin";
  std::filesystem::remove(Landing);

  const Report R = pull(Remote, Landing, options());

  ASSERT_TRUE(std::filesystem::exists(Landing));
  EXPECT_EQ(localDigest(Landing), localDigest(Seed));
  EXPECT_EQ(R.Files, 1u);
  EXPECT_EQ(R.LiteralBytes, 8u << 20);
  EXPECT_EQ(R.MatchedBytes, 0u);
}

TEST_P(Pull, ReportsWhatItReceived) {
  const auto Seed = makeFile("counted.bin", 4u << 20, 8);
  const auto Remote = remotePath("counted.bin");
  seedRemote(Seed, Remote);

  const auto Landing = localDir() / "counted-back.bin";
  std::filesystem::remove(Landing);

  const Report R = pull(Remote, Landing, options());

  // A pull's report comes from the receiving side, so a zero here means the
  // tally was never wired through rather than that nothing moved.
  EXPECT_EQ(R.FileSize, 4u << 20);
  EXPECT_EQ(R.Backend, GetParam());
}

TEST_P(Pull, EmptyFileArrivesEmpty) {
  const auto Seed = makeFile("nothing.bin", 0, 9);
  const auto Remote = remotePath("nothing.bin");
  seedRemote(Seed, Remote);

  const auto Landing = localDir() / "nothing-back.bin";
  std::filesystem::remove(Landing);

  pull(Remote, Landing, options());

  ASSERT_TRUE(std::filesystem::exists(Landing));
  EXPECT_EQ(std::filesystem::file_size(Landing), 0u);
}

TEST_P(Pull, OverwritesWhatIsAlreadyThere) {
  const auto Seed = makeFile("replace.bin", 2u << 20, 10);
  const auto Remote = remotePath("replace.bin");
  seedRemote(Seed, Remote);

  const auto Landing = makeFile("replace-target.bin", 5u << 20, 11);
  ASSERT_NE(localDigest(Landing), localDigest(Seed));

  pull(Remote, Landing, options());

  EXPECT_EQ(localDigest(Landing), localDigest(Seed));
}

TEST_P(Pull, RefusesWhenBothSidesAreRemote) {
  // pushExpectingFailure puts host: on the destination, so a source that
  // already carries one makes both sides remote.
  const auto Failed = pushExpectingFailure(peerHost() + ":/tmp/a", "/tmp/b", options());
  EXPECT_NE(Failed.ExitStatus, 0);
  EXPECT_NE(Failed.Output.find("one side must be local"), std::string::npos) << Failed.Output;
}

TEST_P(Pull, RefusesADryRun) {
  // Only the far side knows what a pull would move, so a dry run that reported
  // anything would have had to transfer it first.
  const auto Failed = pullExpectingFailure(remotePath("whatever.bin"), localDir() / "whatever.bin", options({"-n"}));
  EXPECT_NE(Failed.ExitStatus, 0);
  EXPECT_NE(Failed.Output.find("--dry-run is not supported when pulling"), std::string::npos) << Failed.Output;
}

TEST_P(Pull, RefusesARemoteSpecWithNoPath) {
  // "host:" is a remote path with the name left off, not a local file called
  // "host:" - deciding direction on a failed parse used to make it the latter.
  const auto Failed = pullExpectingFailure("", localDir() / "nowhere.bin", options());
  EXPECT_NE(Failed.ExitStatus, 0);
  EXPECT_NE(Failed.Output.find("empty remote path"), std::string::npos) << Failed.Output;
}

INSTANTIATE_TEST_SUITE_P(Backends, Pull, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
