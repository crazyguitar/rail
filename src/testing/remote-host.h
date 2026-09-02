#pragma once

#include "rail/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rail {

struct RemoteStat {
  uint64_t Size = 0;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
};

// One ssh + sftp session to a host, over libssh.
//
// Tests used to shell out to ssh and scp with concatenated command strings,
// which meant every path was one space or quote away from silent breakage and
// failures surfaced as empty output rather than errors. Everything here is a
// typed operation instead, so a test never builds a command line.
//
// Host is resolved through ~/.ssh/config, so an alias works exactly
// as they do for the product.
// A command running on the peer, reachable over its own libssh channel.
class RemoteProcess {
public:
  RemoteProcess(const RemoteProcess &) = delete;
  RemoteProcess &operator=(const RemoteProcess &) = delete;
  RemoteProcess(RemoteProcess &&) noexcept;
  ~RemoteProcess();

  // One line of the command's stdout, without the newline.
  Result<std::string> readLine();

private:
  friend class RemoteHost;

  struct Impl;

  explicit RemoteProcess(std::unique_ptr<Impl> P);

  std::unique_ptr<Impl> P;
};

class RemoteHost {
public:
  static Result<RemoteHost> open(const std::string &Host);

  RemoteHost(const RemoteHost &) = delete;
  RemoteHost &operator=(const RemoteHost &) = delete;
  RemoteHost(RemoteHost &&) noexcept;
  RemoteHost &operator=(RemoteHost &&) noexcept;
  ~RemoteHost();

  const std::string &name() const { return Host; }

  Result<std::string> address() const;

  void dropForTest();

  Result<void> upload(const std::filesystem::path &Local, const std::string &Remote);
  Result<void> makeDirectory(const std::string &Remote);
  Result<void> removeFile(const std::string &Remote);
  Result<void> removeDirectory(const std::string &Remote);
  Result<bool> exists(const std::string &Remote);
  Result<RemoteStat> stat(const std::string &Remote);

  // Entry names only, not full paths. Uses sftp, so no shell globbing.
  Result<std::vector<std::string>> listDirectory(const std::string &Remote);

  // Streams the remote file over sftp and digests it locally, so the check
  // does not depend on which hashing tool the peer happens to have. Every
  // operation on this class is sftp: the harness runs no remote commands at
  // all, so there is no command string to get quoting wrong in.
  Result<std::string> digest(const std::string &Remote);

  // Starts a command on the peer. Arguments are passed as a vector and quoted
  // here, in one place, so no caller ever assembles a command line: a path
  // holding a space or a quote cannot change what runs. Env entries are
  // exported ahead of the command, which is how the peer finds libraries that
  // are not on its default loader path.
  Result<RemoteProcess> run(const std::vector<std::string> &Argv, const std::vector<std::pair<std::string, std::string>> &Env = {});

public:
  struct Impl;

private:
  Result<void> revive();

  Result<void> uploadOnce(const std::filesystem::path &Local, const std::string &Remote);
  Result<void> makeDirectoryOnce(const std::string &Remote);
  Result<void> removeFileOnce(const std::string &Remote);
  Result<void> removeDirectoryOnce(const std::string &Remote);
  Result<bool> existsOnce(const std::string &Remote);
  Result<RemoteStat> statOnce(const std::string &Remote);
  Result<std::vector<std::string>> listDirectoryOnce(const std::string &Remote);
  Result<std::string> digestOnce(const std::string &Remote);

  bool alive() const;

  template <typename Fn> auto retrying(Fn &&Op) -> decltype(Op()) {
    using Outcome = decltype(Op());

    if (!alive())
      if (auto Back = revive(); !Back) return Outcome(std::unexpected(Back.error()));

    auto First = Op();
    if (First) return First;
    if (auto Back = revive(); !Back) return First;
    return Op();
  }

  RemoteHost(std::unique_ptr<Impl> P, std::string Host);

  std::unique_ptr<Impl> P;
  std::string Host;
};

} // namespace rail
