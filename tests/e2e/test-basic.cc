#include "harness.h"

#include <gtest/gtest.h>

namespace rail::e2e {

class Basic : public BackendTest {};

TEST_P(Basic, DestinationAbsentCopiesWholeFile) {
  const auto Local = makeFile("absent.bin", 8u << 20, 1);
  const auto Remote = remotePath("absent.bin");
  ASSERT_TRUE(peer().removeFile(Remote));

  const Report R = push(Local, Remote, options());

  EXPECT_TRUE(peer().exists(Remote).value_or(false));
  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local));
  EXPECT_EQ(R.MatchedBytes, 0u);
  EXPECT_EQ(R.LiteralBytes, 8u << 20);
  EXPECT_FALSE(remoteHasTempFiles());
}

INSTANTIATE_TEST_SUITE_P(Backends, Basic, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
