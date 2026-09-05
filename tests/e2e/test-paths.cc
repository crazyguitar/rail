#include "rail/fs/safe-path.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

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

class SafePathTree : public testing::Test {
protected:
  void SetUp() override {
    char Pattern[] = "/tmp/rail-paths-XXXXXX";
    const auto Made = ::mkdtemp(Pattern);
    ASSERT_NE(Made, nullptr);
    Base = Made;
    Root = Base / "root";
    std::filesystem::create_directories(Root / "dir");
    std::ofstream(Root / "file") << "inside";
    std::ofstream(Base / "outside") << "outside";
  }
  void TearDown() override {
    if (!Base.empty()) std::filesystem::remove_all(Base);
  }
  std::filesystem::path Base, Root;
};

TEST_F(SafePathTree, DirectChildrenWorkThroughASymlinkedRoot) {
  const auto Alias = Base / "alias";
  std::filesystem::create_directory_symlink(Root, Alias);
  for (const auto *Name : {"file", "dir", ".", "missing", "dir/missing"}) {
    auto Under = underRoot(Alias, Name);
    ASSERT_TRUE(Under) << Name;
    EXPECT_EQ(*Under, Alias / Name);
  }
}

TEST_F(SafePathTree, LinksStillRequireContainmentChecks) {
  std::filesystem::create_symlink(Root / "file", Root / "inside");
  std::filesystem::create_symlink(Base / "outside", Root / "outside");
  std::filesystem::create_symlink(Base, Root / "dir" / "escape");
  std::filesystem::create_symlink(Root / "missing", Root / "dangling");
  EXPECT_TRUE(underRoot(Root, "inside"));
  EXPECT_TRUE(underRoot(Root, "dangling"));
  EXPECT_FALSE(underRoot(Root, "outside"));
  EXPECT_FALSE(underRoot(Root, "dir/escape/outside"));
}

TEST_F(SafePathTree, ReplacingAChildWithAnOutsideLinkIsRefused) {
  ASSERT_TRUE(underRoot(Root, "file"));
  std::filesystem::remove(Root / "file");
  std::filesystem::create_symlink(Base / "outside", Root / "file");
  EXPECT_FALSE(underRoot(Root, "file"));
}
