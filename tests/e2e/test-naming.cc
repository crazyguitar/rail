#include "harness.h"

#include <gtest/gtest.h>

namespace rail::e2e {

namespace {

const std::vector<std::string> &awkwardNames() {
  static const std::vector<std::string> Names{
      "plain.bin",
      "with space.bin",
      "it's.bin",
      "dollar$var.bin",
      "semi;colon.bin",
      "amp&sand.bin",
      "back\\slash.bin",
      "héllo-ünïcode.bin",
  };
  return Names;
}

} // namespace

class Naming : public BackendTest {};

TEST_P(Naming, OddFileNamesWork) {
  const std::string Name = "naming-files";
  const auto Local = freshLocal(Name);
  uint32_t Seed = 0;
  for (const auto &Leaf : awkwardNames()) {
    makeFile(Name + "/" + Leaf, 8192 + Seed, Seed);
    Seed++;
  }

  const auto Remote = remotePath("dest-naming");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, awkwardNames().size());
  for (const auto &Leaf : awkwardNames()) expectFileMatches(Local / Leaf, Remote + "/" + Leaf);
}

TEST_P(Naming, ADestinationWithASpaceArrives) {
  const auto Local = makeFile("naming-space.bin", 64u << 10, 71);
  const std::string Remote = remotePath("dest dir with space") + "/naming-space.bin";
  removeRemoteRecursive(remotePath("dest dir with space"));
  ASSERT_TRUE(peer().makeDirectory(remotePath("dest dir with space")));

  push(Local, Remote, options());

  expectFileMatches(Local, Remote);
}

TEST_P(Naming, ADestinationWithAQuoteArrives) {
  const auto Local = makeFile("naming-quote.bin", 64u << 10, 72);
  const std::string Dir = remotePath("dest'quote");
  const std::string Remote = Dir + "/naming-quote.bin";
  removeRemoteRecursive(Dir);
  ASSERT_TRUE(peer().makeDirectory(Dir));

  push(Local, Remote, options());

  expectFileMatches(Local, Remote);
}

TEST_P(Naming, DryRunWritesNothing) {
  const auto Local = makeFile("naming-dry.bin", 128u << 10, 73);
  const std::string Remote = remotePath("naming-dry.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, options({"--dry-run"}));

  EXPECT_EQ(R.Files, 1u) << "a dry run should still report what it would send";
  EXPECT_FALSE(peer().exists(Remote).value_or(false)) << "a dry run wrote to the destination";
  EXPECT_FALSE(remoteHasTempFiles());
}

INSTANTIATE_TEST_SUITE_P(Backends, Naming, ::testing::ValuesIn(kBackends), [](const auto &I) { return I.param; });

} // namespace rail::e2e
