#include "harness.h"

#include <gtest/gtest.h>

namespace rail::e2e {

class Directory : public BackendTest {
protected:
  void expectFileMatches(const std::filesystem::path &Local, const std::string &Remote) {
    EXPECT_TRUE(peer().exists(Remote).value_or(false)) << Remote << " is missing";
    EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local)) << Remote << " differs";
  }

  void removeRemoteTree(const std::string &Remote) {
    for (const auto &Leaf : {"/sub/nested.bin", "/top.bin"}) peer().removeFile(Remote + Leaf);
  }
};

// Without a trailing slash the directory itself lands under the destination,
// exactly as rsync does it.
TEST_P(Directory, RecursiveCopiesWholeTree) {
  const auto Local = makeTree("tree-plain");
  const auto Remote = remotePath("dest-plain");
  removeRemoteTree(Remote + "/tree-plain");

  const Report R = push(Local, Remote, options({"--recursive"}));

  EXPECT_EQ(R.Files, 2u) << "expected the two regular files";
  expectFileMatches(Local / "top.bin", Remote + "/tree-plain/top.bin");
  expectFileMatches(Local / "sub/nested.bin", Remote + "/tree-plain/sub/nested.bin");
  EXPECT_TRUE(peer().exists(Remote + "/tree-plain/empty").value_or(false)) << "an empty directory was dropped";
  EXPECT_FALSE(remoteHasTempFiles());
}

// A trailing slash copies the contents instead, so no extra level appears.
TEST_P(Directory, TrailingSlashCopiesContents) {
  const auto Local = makeTree("tree-slash");
  const auto Remote = remotePath("dest-slash");
  removeRemoteTree(Remote);

  const Report R = push(Local.string() + "/", Remote, options({"-r"}));

  EXPECT_EQ(R.Files, 2u);
  expectFileMatches(Local / "top.bin", Remote + "/top.bin");
  expectFileMatches(Local / "sub/nested.bin", Remote + "/sub/nested.bin");
  EXPECT_FALSE(peer().exists(Remote + "/tree-slash").value_or(false)) << "the source directory should not appear";
}

// rsync skips directories unless asked to recurse, and says so.
TEST_P(Directory, DirectoryWithoutRecursiveIsSkipped) {
  const auto Local = makeTree("tree-skip");
  const auto Remote = remotePath("dest-skip");

  const Report R = push(Local, Remote, options());

  EXPECT_EQ(R.Files, 0u);
  EXPECT_FALSE(peer().exists(Remote + "/tree-skip/top.bin").value_or(false));
}

// The second pass has an identical basis for every file, so the delta should
// carry instructions and no payload.
TEST_P(Directory, SecondPassMatchesEveryFile) {
  const auto Local = makeTree("tree-delta");
  const auto Remote = remotePath("dest-delta");
  removeRemoteTree(Remote);

  push(Local.string() + "/", Remote, options({"-r"}));
  const Report Again = push(Local.string() + "/", Remote, options({"-r", "--no-whole-file"}));

  EXPECT_EQ(Again.LiteralBytes, 0u) << "nothing changed, so nothing should have been sent";
  EXPECT_EQ(Again.MatchedBytes, (3u << 20) + (5u << 20));
  expectFileMatches(Local / "sub/nested.bin", Remote + "/sub/nested.bin");
}

// A bare relative name has no parent component. Relativising against an empty
// path produced empty names, so the walk silently sent nothing and still
// exited zero.
TEST_P(Directory, RelativeSourceWithoutParentStillWalks) {
  const auto Local = makeTree("tree-relative");
  const auto Remote = remotePath("dest-relative");
  removeRemoteTree(Remote + "/tree-relative");

  PushOptions Opts = options({"-r"});
  Opts.Cwd = localDir().string();
  const Report R = push("tree-relative", Remote, Opts);

  EXPECT_EQ(R.Files, 2u) << "the walk found nothing";
  expectFileMatches(Local / "top.bin", Remote + "/tree-relative/top.bin");
  expectFileMatches(Local / "sub/nested.bin", Remote + "/tree-relative/sub/nested.bin");
}

INSTANTIATE_TEST_SUITE_P(Backends, Directory, ::testing::ValuesIn(kBackends), [](const auto &Info) { return Info.param; });

} // namespace rail::e2e
