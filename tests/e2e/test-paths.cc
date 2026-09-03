#include "rail/fs/safe-path.h"

#include <gtest/gtest.h>

using namespace rail;

TEST(SafePath, ServingTheRootAcceptsWhatIsUnderIt) {
  auto Under = underRoot("/", "etc/hostname");
  ASSERT_TRUE(Under.has_value()) << Under.error().message();
  EXPECT_EQ(Under->string(), "/etc/hostname");
}

TEST(SafePath, ServingTheRootStillRefusesEscapes) {
  EXPECT_FALSE(underRoot("/", "../etc").has_value());
  EXPECT_FALSE(underRoot("/", "/etc").has_value());
}

TEST(SafePath, ADirectoryRootRefusesItsSiblings) {
  EXPECT_FALSE(underRoot("/tmp", "../etc").has_value());
  auto Under = underRoot("/tmp", "file");
  ASSERT_TRUE(Under.has_value()) << Under.error().message();
  EXPECT_EQ(Under->string(), "/tmp/file");
}
