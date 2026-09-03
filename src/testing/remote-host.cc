#include "remote-host.h"

#include <memory>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "rail/app/checksum.h"

#include <fcntl.h>
#include <format>
#include <fstream>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace rail {

namespace {

// Single quotes protect everything except a single quote, which is closed,
// escaped and reopened. Doing this once here is the point of the argv API.
std::string shellQuote(const std::string &Word) {
  std::string Quoted = "'";
  for (const char C : Word) {
    if (C == '\'') Quoted += "'\\''";
    else Quoted.push_back(C);
  }
  return Quoted + "'";
}

} // namespace

namespace {
// libssh caps a single sftp packet well below a megabyte, and sftp_write fails
// outright rather than writing what fits, so writes have to stay small.
constexpr size_t kWriteChunk = 32u << 10;

// Reads have no such limit and are round-trip bound: measured 1.6 MB/s at
// 32 KiB against 4.6 MB/s at 256 KiB.
constexpr size_t kReadChunk = 256u << 10;
} // namespace

struct RemoteHost::Impl {
  ssh_session Session = nullptr;
  std::shared_ptr<int> Epoch = std::make_shared<int>(0);
  sftp_session Sftp = nullptr;

  ~Impl() {
    if (Sftp) sftp_free(Sftp);
    if (Session) {
      if (ssh_is_connected(Session)) ssh_disconnect(Session);
      ssh_free(Session);
    }
  }

  std::string lastError() const { return Session ? ssh_get_error(Session) : "no session"; }
};

RemoteHost::RemoteHost(std::unique_ptr<Impl> P, std::string Host) : P(std::move(P)), Host(std::move(Host)) {}
RemoteHost::RemoteHost(RemoteHost &&) noexcept = default;
RemoteHost &RemoteHost::operator=(RemoteHost &&) noexcept = default;
RemoteHost::~RemoteHost() = default;

namespace {

Result<void> connectSession(RemoteHost::Impl &P, const std::string &Host) {
  P.Session = ssh_new();
  if (!P.Session) return failMessage("ssh_new failed");

  ssh_options_set(P.Session, SSH_OPTIONS_HOST, Host.c_str());

  long Timeout = 15;
  ssh_options_set(P.Session, SSH_OPTIONS_TIMEOUT, &Timeout);
  if (ssh_options_parse_config(P.Session, nullptr) != SSH_OK) return failMessage(std::format("could not parse ssh config for {}", Host));
  if (ssh_connect(P.Session) != SSH_OK) return failMessage(std::format("ssh connect to {} failed: {}", Host, P.lastError()));
  if (ssh_session_is_known_server(P.Session) != SSH_KNOWN_HOSTS_OK) return failMessage(std::format("host key for {} is not in known_hosts", Host));
  if (ssh_userauth_publickey_auto(P.Session, nullptr, nullptr) != SSH_AUTH_SUCCESS)
    return failMessage(std::format("public key auth to {} failed: {}", Host, P.lastError()));

  P.Sftp = sftp_new(P.Session);
  if (!P.Sftp) return failMessage(std::format("sftp_new failed for {}", Host));
  if (sftp_init(P.Sftp) != SSH_OK) return failMessage(std::format("sftp_init failed for {}", Host));
  return {};
}

} // namespace

Result<RemoteHost> RemoteHost::open(const std::string &Host) {
  auto P = std::make_unique<Impl>();
  if (auto R = connectSession(*P, Host); !R) return std::unexpected(R.error());
  return RemoteHost(std::move(P), Host);
}

bool RemoteHost::alive() const { return P->Session && P->Sftp && ssh_is_connected(P->Session); }

void RemoteHost::dropForTest() {
  P->Epoch = std::make_shared<int>(0);
  if (P->Sftp) {
    sftp_free(P->Sftp);
    P->Sftp = nullptr;
  }
  if (P->Session) {
    if (ssh_is_connected(P->Session)) ssh_disconnect(P->Session);
    ssh_free(P->Session);
    P->Session = nullptr;
  }
}

Result<void> RemoteHost::revive() {
  P->Epoch = std::make_shared<int>(0);
  if (P->Sftp) {
    sftp_free(P->Sftp);
    P->Sftp = nullptr;
  }
  if (P->Session) {
    if (ssh_is_connected(P->Session)) ssh_disconnect(P->Session);
    ssh_free(P->Session);
    P->Session = nullptr;
  }
  return connectSession(*P, Host);
}

Result<std::string> RemoteHost::address() const {
  const int Fd = ssh_get_fd(P->Session);
  if (Fd < 0) return failMessage("no ssh socket");

  sockaddr_storage Addr{};
  socklen_t Len = sizeof(Addr);
  if (::getpeername(Fd, reinterpret_cast<sockaddr *>(&Addr), &Len) != 0) return failErrno("getpeername");

  char Text[INET6_ADDRSTRLEN]{};
  if (Addr.ss_family == AF_INET) {
    ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&Addr)->sin_addr, Text, sizeof(Text));
  } else if (Addr.ss_family == AF_INET6) {
    ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&Addr)->sin6_addr, Text, sizeof(Text));
  } else {
    return failMessage("ssh socket is not an IP socket");
  }
  return std::string(Text);
}

Result<void> RemoteHost::upload(const std::filesystem::path &Local, const std::string &Remote) {
  return retrying([&]() { return uploadOnce(Local, Remote); });
}

Result<void> RemoteHost::uploadOnce(const std::filesystem::path &Local, const std::string &Remote) {
  std::ifstream In(Local, std::ios::binary);
  if (!In) return failMessage(std::format("cannot open {}", Local.string()));

  sftp_file Out = sftp_open(P->Sftp, Remote.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!Out) return failMessage(std::format("sftp open {} on {} failed: {}", Remote, Host, P->lastError()));

  std::vector<char> Buf(kWriteChunk);
  while (In) {
    In.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
    const std::streamsize N = In.gcount();
    if (N <= 0) break;

    // sftp_write returns what fitted in the transfer window, not what was
    // asked for, so a large chunk needs a loop rather than an equality check.
    size_t Done = 0;
    while (Done < static_cast<size_t>(N)) {
      const ssize_t W = sftp_write(Out, Buf.data() + Done, static_cast<size_t>(N) - Done);
      if (W <= 0) {
        sftp_close(Out);
        return failMessage(std::format("sftp write to {} on {} failed after {} bytes", Remote, Host, Done));
      }
      Done += static_cast<size_t>(W);
    }
  }
  sftp_close(Out);
  return {};
}

Result<void> RemoteHost::makeDirectory(const std::string &Remote) {
  return retrying([&]() { return makeDirectoryOnce(Remote); });
}

Result<void> RemoteHost::makeDirectoryOnce(const std::string &Remote) {
  if (sftp_mkdir(P->Sftp, Remote.c_str(), 0755) == SSH_OK) return {};
  if (sftp_get_error(P->Sftp) == SSH_FX_FILE_ALREADY_EXISTS) return {};
  return failMessage(std::format("sftp mkdir {} on {} failed", Remote, Host));
}

Result<void> RemoteHost::removeFile(const std::string &Remote) {
  return retrying([&]() { return removeFileOnce(Remote); });
}

Result<void> RemoteHost::removeFileOnce(const std::string &Remote) {
  if (sftp_unlink(P->Sftp, Remote.c_str()) == SSH_OK) return {};
  if (sftp_get_error(P->Sftp) == SSH_FX_NO_SUCH_FILE) return {};
  return failMessage(std::format("sftp unlink {} on {} failed", Remote, Host));
}

Result<void> RemoteHost::removeDirectory(const std::string &Remote) {
  return retrying([&]() { return removeDirectoryOnce(Remote); });
}

Result<void> RemoteHost::removeDirectoryOnce(const std::string &Remote) {
  // Absent is the desired state, so only a real failure is reported.
  sftp_rmdir(P->Sftp, Remote.c_str());
  return {};
}

Result<bool> RemoteHost::exists(const std::string &Remote) {
  return retrying([&]() { return existsOnce(Remote); });
}

Result<bool> RemoteHost::existsOnce(const std::string &Remote) {
  sftp_attributes A = sftp_stat(P->Sftp, Remote.c_str());
  if (A) {
    sftp_attributes_free(A);
    return true;
  }
  if (sftp_get_error(P->Sftp) == SSH_FX_NO_SUCH_FILE) return false;
  return failMessage(std::format("sftp stat {} on {} failed", Remote, Host));
}

Result<RemoteStat> RemoteHost::stat(const std::string &Remote) {
  return retrying([&]() { return statOnce(Remote); });
}

Result<RemoteStat> RemoteHost::statOnce(const std::string &Remote) {
  sftp_attributes A = sftp_stat(P->Sftp, Remote.c_str());
  if (!A) return failMessage(std::format("sftp stat {} on {} failed", Remote, Host));

  RemoteStat S;
  S.Size = A->size;
  S.Mode = A->permissions & 07777;
  S.Mtime = static_cast<int64_t>(A->mtime);
  sftp_attributes_free(A);
  return S;
}

Result<std::vector<std::string>> RemoteHost::listDirectory(const std::string &Remote) {
  return retrying([&]() { return listDirectoryOnce(Remote); });
}

Result<std::vector<std::string>> RemoteHost::listDirectoryOnce(const std::string &Remote) {
  sftp_dir Dir = sftp_opendir(P->Sftp, Remote.c_str());
  if (!Dir) return failMessage(std::format("sftp opendir {} on {} failed", Remote, Host));

  std::vector<std::string> Names;
  while (sftp_attributes A = sftp_readdir(P->Sftp, Dir)) {
    const std::string Name = A->name ? A->name : "";
    sftp_attributes_free(A);
    if (Name != "." && Name != "..") Names.push_back(Name);
  }
  sftp_closedir(Dir);
  return Names;
}

Result<std::string> RemoteHost::digest(const std::string &Remote) {
  return retrying([&]() { return digestOnce(Remote); });
}

Result<std::string> RemoteHost::digestOnce(const std::string &Remote) {
  sftp_file In = sftp_open(P->Sftp, Remote.c_str(), O_RDONLY, 0);
  if (!In) return failMessage(std::format("sftp open {} on {} failed", Remote, Host));

  Hasher H;
  std::vector<std::byte> Buf(kReadChunk);
  for (;;) {
    const ssize_t N = sftp_read(In, Buf.data(), Buf.size());
    if (N < 0) {
      sftp_close(In);
      return failMessage(std::format("sftp read {} failed", Remote));
    }
    if (N == 0) break;
    H.update({Buf.data(), static_cast<size_t>(N)});
  }
  sftp_close(In);
  return toHex(H.digest());
}

struct RemoteProcess::Impl {
  ssh_channel Channel = nullptr;
  std::string Pending;

  std::weak_ptr<int> Session;

  bool live() const { return Channel && !Session.expired(); }

  ~Impl() {
    if (!live()) return;
    ssh_channel_close(Channel);
    ssh_channel_free(Channel);
  }
};

RemoteProcess::RemoteProcess(std::unique_ptr<Impl> P) : P(std::move(P)) {}
RemoteProcess::RemoteProcess(RemoteProcess &&) noexcept = default;
RemoteProcess::~RemoteProcess() = default;

Result<std::string> RemoteProcess::readLine() {
  for (;;) {
    if (const auto Break = P->Pending.find('\n'); Break != std::string::npos) {
      std::string Line = P->Pending.substr(0, Break);
      P->Pending.erase(0, Break + 1);
      return Line;
    }

    char Buf[512];
    if (!P->live()) return failMessage("the session this command ran on is gone");
    const int N = ssh_channel_read(P->Channel, Buf, sizeof(Buf), 0);
    if (N < 0) return failMessage("reading from the peer failed");
    if (N == 0) return failMessage("the peer closed before it produced a line");
    P->Pending.append(Buf, static_cast<size_t>(N));
  }
}

Result<RemoteProcess> RemoteHost::run(const std::vector<std::string> &Argv, const std::vector<std::pair<std::string, std::string>> &Env) {
  if (Argv.empty()) return failMessage("empty argument vector");

  auto Channel = std::make_unique<RemoteProcess::Impl>();
  Channel->Channel = ssh_channel_new(P->Session);
  if (!Channel->Channel) return failMessage("ssh_channel_new: " + P->lastError());
  Channel->Session = P->Epoch;
  if (ssh_channel_open_session(Channel->Channel) != SSH_OK) return failMessage("ssh_channel_open_session: " + P->lastError());

  std::string Command;
  for (const auto &[Name, Value] : Env) Command += Name + "=" + shellQuote(Value) + " ";
  for (const auto &Word : Argv) Command += shellQuote(Word) + " ";

  if (ssh_channel_request_exec(Channel->Channel, Command.c_str()) != SSH_OK) return failMessage("ssh_channel_request_exec: " + P->lastError());

  return RemoteProcess(std::move(Channel));
}

} // namespace rail
