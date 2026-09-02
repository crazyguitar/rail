#include "harness.h"

#include <fstream>
#include <gtest/gtest.h>

namespace rail::e2e {

class Filter : public BackendTest {
protected:
  std::filesystem::path buildSource(const std::string &Name) {
    const auto Root = localDir() / Name;
    std::filesystem::remove_all(Root);
    std::filesystem::create_directories(Root / "keep");
    std::filesystem::create_directories(Root / "skip");
    std::filesystem::create_directories(Root / "sub/deep");

    makeFile(Name + "/a.txt", 1024, 1);
    makeFile(Name + "/b.log", 2048, 2);
    makeFile(Name + "/keep/c.txt", 1024, 3);
    makeFile(Name + "/keep/d.log", 2048, 4);
    makeFile(Name + "/skip/e.txt", 1024, 5);
    makeFile(Name + "/sub/deep/f.log", 2048, 6);
    makeFile(Name + "/sub/deep/g.txt", 4096, 7);
    return Root;
  }

  void expectPresent(const std::string &Remote, const std::vector<std::string> &Leaves) {
    for (const auto &Leaf : Leaves) EXPECT_TRUE(peer().exists(Remote + "/" + Leaf).value_or(false)) << Leaf << " should have been sent";
  }

  void expectAbsent(const std::string &Remote, const std::vector<std::string> &Leaves) {
    for (const auto &Leaf : Leaves) EXPECT_FALSE(peer().exists(Remote + "/" + Leaf).value_or(false)) << Leaf << " should have been filtered out";
  }
};

TEST_P(Filter, ExcludeSkipsMatches) {
  const auto Local = buildSource("filter-exclude");
  const auto Remote = remotePath("dest-filter-exclude");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=*.log"}));

  EXPECT_EQ(R.Files, 4u);
  expectPresent(Remote, {"a.txt", "keep/c.txt", "skip/e.txt", "sub/deep/g.txt"});
  expectAbsent(Remote, {"b.log", "keep/d.log", "sub/deep/f.log"});
}

TEST_P(Filter, ExcludeDirPrunesSubtree) {
  const auto Local = buildSource("filter-prune");
  const auto Remote = remotePath("dest-filter-prune");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=sub/"}));

  EXPECT_EQ(R.Files, 5u);
  expectPresent(Remote, {"a.txt", "b.log", "keep/c.txt", "keep/d.log", "skip/e.txt"});
  expectAbsent(Remote, {"sub/deep/f.log", "sub/deep/g.txt", "sub"});
}

TEST_P(Filter, IncludeBeforeExcludeWins) {
  const auto Local = buildSource("filter-order");
  const auto Remote = remotePath("dest-filter-order");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--include=keep/**", "--exclude=*.log"}));

  EXPECT_EQ(R.Files, 5u);
  expectPresent(Remote, {"a.txt", "keep/c.txt", "keep/d.log", "skip/e.txt", "sub/deep/g.txt"});
  expectAbsent(Remote, {"b.log", "sub/deep/f.log"});
}

TEST_P(Filter, AnchoredMatchesRootOnly) {
  const auto Local = buildSource("filter-anchor");
  const auto Remote = remotePath("dest-filter-anchor");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=/a.txt"}));

  EXPECT_EQ(R.Files, 6u);
  expectAbsent(Remote, {"a.txt"});
  expectPresent(Remote, {"b.log", "keep/c.txt", "sub/deep/g.txt"});
}

TEST_P(Filter, IncludeThenExcludeAll) {
  const auto Local = buildSource("filter-only");
  const auto Remote = remotePath("dest-filter-only");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--include=*.txt", "--exclude=*"}));

  EXPECT_EQ(R.Files, 1u) << "only the root .txt survives, since the directories are excluded too";
  expectPresent(Remote, {"a.txt"});
  expectAbsent(Remote, {"b.log", "keep/c.txt"});
}

TEST_P(Filter, ExcludeFromFileWorks) {
  const auto Local = buildSource("filter-from");
  const auto Rules = localDir() / "filter-from.rules";
  {
    std::ofstream Out(Rules);
    Out << "# comment\n\n*.log\nskip/\n";
  }
  const auto Remote = remotePath("dest-filter-from");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude-from=" + Rules.string()}));

  EXPECT_EQ(R.Files, 3u);
  expectPresent(Remote, {"a.txt", "keep/c.txt", "sub/deep/g.txt"});
  expectAbsent(Remote, {"b.log", "keep/d.log", "skip/e.txt"});
}

TEST_P(Filter, FilterTakesPlusAndMinus) {
  const auto Local = buildSource("filter-rule");
  const auto Remote = remotePath("dest-filter-rule");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--filter=+ keep/**", "--filter=- *.log"}));

  EXPECT_EQ(R.Files, 5u);
  expectPresent(Remote, {"keep/d.log"});
  expectAbsent(Remote, {"b.log", "sub/deep/f.log"});
}

TEST_P(Filter, SizeLimitsSkipFiles) {
  const auto Local = buildSource("filter-size");
  const auto Remote = remotePath("dest-filter-size");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--min-size=2000", "--max-size=3000"}));

  EXPECT_EQ(R.Files, 3u);
  expectPresent(Remote, {"b.log", "keep/d.log", "sub/deep/f.log"});
  expectAbsent(Remote, {"a.txt", "keep/c.txt", "sub/deep/g.txt"});
}

TEST_P(Filter, CharClassMatches) {
  const auto Local = buildSource("filter-class");
  const auto Remote = remotePath("dest-filter-class");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=[ab]*"}));

  EXPECT_EQ(R.Files, 5u);
  expectAbsent(Remote, {"a.txt", "b.log"});
  expectPresent(Remote, {"keep/c.txt", "keep/d.log", "skip/e.txt", "sub/deep/f.log", "sub/deep/g.txt"});
}

TEST_P(Filter, NegatedCharClassMatches) {
  const auto Local = buildSource("filter-negclass");
  const auto Remote = remotePath("dest-filter-negclass");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=[!ab]*.txt"}));

  EXPECT_EQ(R.Files, 4u);
  expectPresent(Remote, {"a.txt", "b.log", "keep/d.log", "sub/deep/f.log"});
  expectAbsent(Remote, {"keep/c.txt", "skip/e.txt", "sub/deep/g.txt"});
}

TEST_P(Filter, IncludeFromFileWorks) {
  const auto Local = buildSource("filter-incfrom");
  const auto Rules = localDir() / "filter-include.rules";
  {
    std::ofstream Out(Rules);
    Out << "; comment\n*.txt\n";
  }
  const auto Remote = remotePath("dest-filter-incfrom");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--include-from=" + Rules.string(), "--exclude=*"}));

  EXPECT_EQ(R.Files, 1u);
  expectPresent(Remote, {"a.txt"});
  expectAbsent(Remote, {"b.log", "keep/c.txt"});
}

TEST_P(Filter, ExcludeAllSendsNothing) {
  const auto Local = buildSource("filter-none");
  const auto Remote = remotePath("dest-filter-none");
  removeRemoteRecursive(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r", "--exclude=*"}));

  EXPECT_EQ(R.Files, 0u);
  EXPECT_FALSE(remoteHasTempFiles());
}

INSTANTIATE_TEST_SUITE_P(Backends, Filter, ::testing::ValuesIn(kBackends), [](const auto &I) { return I.param; });

} // namespace rail::e2e
