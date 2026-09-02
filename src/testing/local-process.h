#pragma once

#include "rail/result.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rail {

struct ProcessResult {
  int ExitStatus = 0;
  std::string Output;    // stdout and stderr, interleaved
  uint64_t MaxRssKb = 0; // peak resident set of the child
};

// Runs a program with an explicit argument vector and no shell. Arguments
// reach the program exactly as written, so a path containing a space or a
// quote cannot change what runs.
Result<ProcessResult> runLocal(const std::vector<std::string> &Argv, const std::string &Cwd = {});

// A running program the caller can kill when it decides to. Killing on a
// sleep cannot tell setup from transfer, so a test that needs to interrupt a
// transfer waits for evidence the transfer began and kills on that.
class BackgroundProcess {
public:
  static Result<BackgroundProcess> start(const std::vector<std::string> &Argv);

  BackgroundProcess(const BackgroundProcess &) = delete;
  BackgroundProcess &operator=(const BackgroundProcess &) = delete;
  BackgroundProcess(BackgroundProcess &&Other) noexcept;
  ~BackgroundProcess();

  void kill();
  Result<ProcessResult> wait();

private:
  BackgroundProcess(int Pid, int ReadFd) : Pid(Pid), ReadFd(ReadFd) {}

  int Pid = -1;
  int ReadFd = -1;
};

} // namespace rail
