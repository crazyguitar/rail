#include "p2p.h"
#include "harness.h"
#include "local-process.h"
#include "remote-host.h"

#include "rail/io/runner.h"
#include "rail/io/trace.h"
#include "rail/transport/data-channel.h"
#include "rail/transport/rdma-devices.h"

#include <benchmark/benchmark.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace rail;
using namespace rail::bench;

int serveBuffers(size_t MessageSize, size_t Depth, const std::string &Dialler) {
  // The endpoint travels on stdout, so keep a private copy and point fd 1 at
  // stderr or a stray write lands mid-frame.
  const int Out = ::dup(STDOUT_FILENO);
  if (Out < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 1;

  auto Channel = makeDataChannel("rdma", poolPages(Depth), MessageSize, "");
  if (!Channel) return 1;

  Channel->watch(STDIN_FILENO);

  if (!Channel->prepare()) return 1;
  if (!Dialler.empty() && !run(Channel->attachPeer(fromHex(Dialler)))) return 1;

  auto Endpoint = run(Channel->listen());
  if (!Endpoint) {
    std::fprintf(stderr, "listen: %s\n", Endpoint.error().message().c_str());
    return 1;
  }

  const std::string Line = toHex(*Endpoint) + "\n";
  if (::write(Out, Line.data(), Line.size()) != static_cast<ssize_t>(Line.size())) return 1;

  if (!::freopen("/dev/null", "w", stdout)) return 1;
  if (!::freopen("/dev/null", "w", stderr)) return 1;

  if (auto R = run(Channel->acceptPeer()); !R) return 1;

  std::vector<Page> Bufs;
  for (size_t I = 0; I < Depth; I++) {
    Bufs.push_back(run(Channel->pool().acquire()));
    if (!Bufs.back().valid()) return 1;
  }

  const size_t Half = Depth / 2;
  std::span<Page> Front{Bufs.data(), Half};
  std::span<Page> Back{Bufs.data() + Half, Depth - Half};

  auto take = [&](Page &Buf) { return Channel->recv(Buf, 0, MessageSize); };

  Burst Draining;
  Burst Waiting;
  Draining.post(Front, take);
  Waiting.post(Back, take);

  for (;;) {
    if (auto R = run(Draining.join()); !R) return 0;
    Draining.post(Front, take);
    std::swap(Draining, Waiting);
    std::swap(Front, Back);
  }
}

void BM_P2pSend(benchmark::State &State) {
  const size_t Size = static_cast<size_t>(State.range(0));
  const size_t Depth = static_cast<size_t>(State.range(1));

  Fabric F(Size, Depth);
  if (!available(State, F.ready(), "could not reach the peer over rdma")) return;

  std::vector<Page> Bufs;
  for (size_t I = 0; I < Depth; I++) {
    Bufs.push_back(run(F.channel().pool().acquire()));
    if (!Bufs.back().valid()) {
      State.SkipWithError("no registered memory for the transfer pages");
      return;
    }
    Bufs.back().resize(Size);
  }

  if (auto R = run(sendBurst(F.channel(), Bufs)); !R) {
    State.SkipWithError(R.error().message().c_str());
    return;
  }

  measure(State, Size * Depth, [&] { return run(sendBurst(F.channel(), Bufs)); });

  State.counters["rails"] = static_cast<double>(activeRdmaPorts().size());
}

BENCHMARK(BM_P2pSend)
    ->Args({1 << 20, 4})
    ->Args({16 << 20, 4})
    ->Args({64 << 20, 4})
    ->Args({256 << 20, 4})
    ->Args({1024 << 20, 4})
    ->Args({64 << 20, 1})
    ->Args({64 << 20, 16})
    ->Args({256 << 20, 16})
    ->Apply(steadyState);

} // namespace

int main(int Argc, char **Argv) {
  for (int I = 1; I < Argc; I++)
    if (std::string(Argv[I]) == "--serve-buffers" && I + 2 < Argc)
      return serveBuffers(std::strtoull(Argv[I + 1], nullptr, 10), std::strtoull(Argv[I + 2], nullptr, 10), I + 3 < Argc ? Argv[I + 3] : "");

  benchmark::Initialize(&Argc, Argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
