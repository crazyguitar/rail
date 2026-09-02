#include "harness.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

namespace rail::e2e {

class Tree : public BackendTest {
protected:
  void expectFileMatches(const std::filesystem::path &Local, const std::string &Remote) {
    EXPECT_TRUE(peer().exists(Remote).value_or(false)) << Remote << " is missing";
    EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local)) << Remote << " differs";
  }

  std::filesystem::path freshLocal(const std::string &Name) {
    const auto Root = localDir() / Name;
    std::filesystem::remove_all(Root);
    std::filesystem::create_directories(Root);
    return Root;
  }
};

TEST_P(Tree, ManyFilesArrive) {
  const std::string Name = "tree-many";
  const auto Local = freshLocal(Name);
  constexpr uint32_t Count = 120;
  for (uint32_t I = 0; I < Count; I++) makeFile(Name + "/f" + std::to_string(I) + ".bin", 4096 + I, I);

  const auto Remote = remotePath("dest-many");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, Count);
  EXPECT_EQ(peer().listDirectory(Remote).transform([](const std::vector<std::string> &E) { return E.size(); }).value_or(0), Count);
  for (uint32_t I = 0; I < Count; I += 17) {
    const std::string Leaf = "f" + std::to_string(I) + ".bin";
    expectFileMatches(Local / Leaf, Remote + "/" + Leaf);
  }
  EXPECT_FALSE(remoteHasTempFiles());
}

TEST_P(Tree, EmptyFilesArrive) {
  const std::string Name = "tree-mixed";
  const auto Local = freshLocal(Name);
  constexpr uint32_t Count = 20;
  for (uint32_t I = 0; I < Count; I++) {
    const std::string Leaf = Name + "/m" + std::to_string(I) + ".bin";
    if (I % 2 == 0) makeEmptyFile(Leaf);
    else makeFile(Leaf, 64u << 10, I);
  }

  const auto Remote = remotePath("dest-mixed");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, Count);
  for (uint32_t I = 0; I < Count; I++) {
    const std::string Leaf = "m" + std::to_string(I) + ".bin";
    expectFileMatches(Local / Leaf, Remote + "/" + Leaf);
    EXPECT_EQ(peer().stat(Remote + "/" + Leaf).transform([](const RemoteStat &S) { return S.Size; }).value_or(~0ull),
              std::filesystem::file_size(Local / Leaf))
        << Leaf << " has the wrong size";
  }
}

TEST_P(Tree, MultiPageFilesArrive) {
  const std::string Name = "tree-pages";
  const auto Local = freshLocal(Name);
  for (uint32_t I = 0; I < 4; I++) makeFile(Name + "/p" + std::to_string(I) + ".bin", (5u << 20) + I, 100 + I);

  const auto Remote = remotePath("dest-pages");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--page-size=1M", "--pages=4"}));

  EXPECT_EQ(R.Files, 4u);
  for (uint32_t I = 0; I < 4; I++) {
    const std::string Leaf = "p" + std::to_string(I) + ".bin";
    expectFileMatches(Local / Leaf, Remote + "/" + Leaf);
  }
}

TEST_P(Tree, KeepsDeepNesting) {
  const std::string Name = "tree-deep";
  const auto Local = freshLocal(Name);
  std::string Relative = Name;
  std::string Leaf;
  for (int I = 0; I < 8; I++) {
    Relative += "/d" + std::to_string(I);
    Leaf += (Leaf.empty() ? "" : "/");
    Leaf += "d" + std::to_string(I);
  }
  std::filesystem::create_directories(localDir() / Relative);
  makeFile(Relative + "/deep.bin", 1u << 20, 55);

  const auto Remote = remotePath("dest-deep");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, 1u);
  expectFileMatches(localDir() / Relative / "deep.bin", Remote + "/" + Leaf + "/deep.bin");
}

TEST_P(Tree, CopiesSymlink) {
  const std::string Name = "tree-link";
  const auto Local = freshLocal(Name);
  makeFile(Name + "/real.bin", 32u << 10, 61);
  std::filesystem::create_symlink("real.bin", Local / "link.bin");

  const auto Remote = remotePath("dest-link");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, 1u) << "a link carries no payload and should not count as a file";
  expectFileMatches(Local / "real.bin", Remote + "/real.bin");

  // Arrives as a link, not as a copy of what it points at.
  auto Reading = peer().run({"readlink", Remote + "/link.bin"});
  ASSERT_TRUE(Reading) << "could not read the link on the peer";
  auto Target = Reading->readLine();
  ASSERT_TRUE(Target) << "the link did not arrive at all";
  EXPECT_EQ(*Target, "real.bin");
}

// A link to nothing is still a link, and a tree that holds one must arrive
// whole rather than stopping at it.
TEST_P(Tree, CopiesADanglingSymlink) {
  const std::string Name = "tree-dangling";
  const auto Local = freshLocal(Name);
  makeFile(Name + "/real.bin", 8u << 10, 63);
  std::filesystem::create_symlink("nowhere.bin", Local / "dangling.bin");

  const auto Remote = remotePath("dest-dangling");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, 1u);
  auto Reading = peer().run({"readlink", Remote + "/dangling.bin"});
  ASSERT_TRUE(Reading);
  auto Target = Reading->readLine();
  ASSERT_TRUE(Target) << "the dangling link did not arrive";
  EXPECT_EQ(*Target, "nowhere.bin");
}

TEST_P(Tree, SkipsFifo) {
  const std::string Name = "tree-fifo";
  const auto Local = freshLocal(Name);
  makeFile(Name + "/real.bin", 16u << 10, 62);
  const auto Fifo = Local / "pipe";
  std::filesystem::remove(Fifo);
  ASSERT_EQ(::mkfifo(Fifo.string().c_str(), 0600), 0) << "could not create the fifo fixture";

  const auto Remote = remotePath("dest-fifo");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, 1u) << "the fifo should not count as a file";
  expectFileMatches(Local / "real.bin", Remote + "/real.bin");
  EXPECT_FALSE(peer().exists(Remote + "/pipe").value_or(false)) << "a fifo was copied";
}

INSTANTIATE_TEST_SUITE_P(Backends, Tree, ::testing::ValuesIn(kBackends), [](const auto &I) { return I.param; });

} // namespace rail::e2e
