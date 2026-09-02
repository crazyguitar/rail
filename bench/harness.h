#pragma once

#include "rail/result.h"
#include "local-process.h"
#include "remote-host.h"

#include <benchmark/benchmark.h>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <string>

namespace rail::bench {

inline std::string envOr(const char *Name, const std::string &Fallback) {
  const char *Value = ::getenv(Name);
  return Value ? Value : Fallback;
}

inline std::string peerHost() { return envOr("RAIL_PEER", ""); }

// The address railnfs and the file client dial, which decides how long a request
// waits. RAIL_FABRIC names the peer on the fast network; without it the
// benchmark reports whatever the ssh route happens to be.
inline std::string fabricHost() { return envOr("RAIL_FABRIC", peerHost()); }

inline std::filesystem::path selfPath() {
  std::error_code EC;
  const auto Self = std::filesystem::read_symlink("/proc/self/exe", EC);
  return EC ? std::filesystem::path("rail-bench") : Self;
}

inline std::string toolPath(const std::string &Name) { return (selfPath().parent_path().parent_path() / "tools" / Name / Name).string(); }

// Dropping the caches needs root. In a container the suite already is root and
// there is no sudo to call; on the peer it is an ordinary account that has it.
// A drop that quietly fails leaves the next read served from memory.
inline std::string forgetHere(int Level) {
  const std::string Write = "echo " + std::to_string(Level) + " > /proc/sys/vm/drop_caches";
  if (::geteuid() == 0) return "sync; " + Write;

  return "sync; sudo -n sh -c '" + Write + "' 2>/dev/null || true";
}

inline std::string forgetOnPeer(int Level) {
  return "sync; sudo -n sh -c 'echo " + std::to_string(Level) + " > /proc/sys/vm/drop_caches' 2>/dev/null || true";
}

// A fixture the peer has in RAM is read at memory speed, and every case after
// the first one found it there. Dropping both caches before a whole-file case
// is what makes its number a transfer rather than a memcpy.
inline void coldData(const std::string &Peer) {
  [[maybe_unused]] auto Here = runLocal({"sh", "-c", forgetHere(1)});

  auto Opened = RemoteHost::open(Peer);
  if (!Opened) return;
  auto Ran = Opened->run({"sh", "-c", forgetOnPeer(1)});
  if (!Ran) return;
  while (Ran->readLine()) {}
}

inline bool available(benchmark::State &State, bool Ready, const char *Missing) {
  if (Ready) return true;
  State.SkipWithError(Missing);
  return false;
}

template <class T> T *orSkip(benchmark::State &State, Result<T> &Maybe) {
  if (Maybe) return &*Maybe;
  State.SkipWithError(Maybe.error().message().c_str());
  return nullptr;
}

inline void measure(benchmark::State &State, uint64_t BytesEach, auto Round) {
  for (auto _ : State) {
    auto Done = Round();
    if (Done) continue;
    State.SkipWithError(Done.error().message().c_str());
    break;
  }
  State.SetBytesProcessed(static_cast<int64_t>(State.iterations()) * static_cast<int64_t>(BytesEach));
}

inline void wholeFile(benchmark::internal::Benchmark *Case) { Case->UseRealTime()->Unit(benchmark::kMillisecond)->Iterations(1); }

inline void steadyState(benchmark::internal::Benchmark *Case) { Case->MinTime(2.0)->UseRealTime()->Unit(benchmark::kMillisecond); }

} // namespace rail::bench
