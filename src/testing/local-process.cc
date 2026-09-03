#include "local-process.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace rail {

namespace {

// Starts the program with its stdout and stderr joined onto one pipe. Returns
// the child pid and hands the read end to the caller.
Result<pid_t> spawn(const std::vector<std::string> &Argv, int &ReadFd, const std::string &Cwd) {
  if (Argv.empty()) return failMessage("empty argument vector");

  int Pipe[2];
  if (::pipe(Pipe) != 0) return failErrno("pipe");

  posix_spawn_file_actions_t Actions;
  posix_spawn_file_actions_init(&Actions);
  if (!Cwd.empty()) posix_spawn_file_actions_addchdir_np(&Actions, Cwd.c_str());
  posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&Actions, Pipe[0]);
  posix_spawn_file_actions_addclose(&Actions, Pipe[1]);

  std::vector<char *> Raw;
  Raw.reserve(Argv.size() + 1);
  for (const auto &A : Argv) Raw.push_back(const_cast<char *>(A.c_str()));
  Raw.push_back(nullptr);

  pid_t Pid = -1;
  const int Failed = ::posix_spawnp(&Pid, Raw[0], &Actions, nullptr, Raw.data(), environ);
  posix_spawn_file_actions_destroy(&Actions);
  ::close(Pipe[1]);
  if (Failed != 0) {
    ::close(Pipe[0]);
    errno = Failed;
    return failErrno("posix_spawn");
  }

  ReadFd = Pipe[0];
  return Pid;
}

// Drains the pipe to end of file, then reaps the child. A killed process
// leaves its own ssh child holding the write end, so the wait is bounded:
// without that, one lingering grandchild hangs the suite with no diagnostic.
Result<ProcessResult> collect(pid_t Pid, int ReadFd, std::chrono::milliseconds Patience) {
  ProcessResult R;
  std::array<char, 4096> Buf{};
  const auto Deadline = std::chrono::steady_clock::now() + Patience;
  for (;;) {
    pollfd Poll{ReadFd, POLLIN, 0};
    const auto Left = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - std::chrono::steady_clock::now());
    if (Left.count() <= 0) break;
    if (::poll(&Poll, 1, static_cast<int>(Left.count())) <= 0) break;

    const ssize_t N = ::read(ReadFd, Buf.data(), Buf.size());
    if (N <= 0) break;
    R.Output.append(Buf.data(), static_cast<size_t>(N));
  }
  ::close(ReadFd);

  // The patience above bounds the reading, not the child, and reading ends
  // early on EOF or a signal as well as on the deadline. Poll for the exit so
  // the deadline bounds the child however the reading ended.
  // wait4 reports the child's peak RSS directly, so a memory assertion needs
  // no external tool.
  int Status = 0;
  rusage Usage{};
  for (;;) {
    const pid_t Done = ::wait4(Pid, &Status, WNOHANG, &Usage);
    if (Done < 0) return failErrno("wait4");
    if (Done > 0) break;

    if (std::chrono::steady_clock::now() >= Deadline) {
      ::kill(Pid, SIGKILL);
      if (::wait4(Pid, &Status, 0, &Usage) < 0) return failErrno("wait4");
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  R.ExitStatus = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
  R.MaxRssKb = static_cast<uint64_t>(Usage.ru_maxrss);
  return R;
}

} // namespace

Result<ProcessResult> runLocal(const std::vector<std::string> &Argv, const std::string &Cwd) {
  int ReadFd = -1;
  auto Pid = spawn(Argv, ReadFd, Cwd);
  if (!Pid) return std::unexpected(Pid.error());
  return collect(*Pid, ReadFd, std::chrono::minutes(10));
}

Result<BackgroundProcess> BackgroundProcess::start(const std::vector<std::string> &Argv) {
  int ReadFd = -1;
  auto Pid = spawn(Argv, ReadFd, {});
  if (!Pid) return std::unexpected(Pid.error());
  return BackgroundProcess(*Pid, ReadFd);
}

BackgroundProcess::BackgroundProcess(BackgroundProcess &&Other) noexcept
    : Pid(std::exchange(Other.Pid, -1)), ReadFd(std::exchange(Other.ReadFd, -1)) {}

BackgroundProcess::~BackgroundProcess() {
  if (Pid < 0) return;
  kill();
  [[maybe_unused]] auto Ignored = wait();
}

void BackgroundProcess::kill() {
  if (Pid >= 0) ::kill(Pid, SIGKILL);
}

Result<ProcessResult> BackgroundProcess::wait() {
  if (Pid < 0) return failMessage("process already reaped");

  // Draining only ends once every writer is gone, and a killed sender leaves
  // its ssh child holding the pipe, so the wait is bounded.
  auto R = collect(std::exchange(Pid, -1), std::exchange(ReadFd, -1), std::chrono::seconds(30));
  return R;
}

} // namespace rail
