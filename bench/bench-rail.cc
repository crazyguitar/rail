
#include "fixtures.h"

#include <cstring>

namespace {
using namespace rail;
using namespace rail::bench;

void BM_RailRead(benchmark::State &State) {
  const size_t Block = static_cast<size_t>(State.range(0));
  const size_t Asked = static_cast<size_t>(State.range(1));

  if (!exportReady(State)) return;

  auto Opened = openService(Block, Asked + 4);
  if (!orSkip(State, Opened)) return;
  auto &Client = **Opened;

  const size_t Depth = std::min(Asked, Client.maxOutstanding());
  std::vector<std::vector<std::byte>> Landing(Depth, std::vector<std::byte>(Block));
  const std::string Source = targetFor(targetSize());
  uint64_t Offset = 0;

  measure(State, Depth * Block, [&] { return run(readRound(Client, Source, Offset, Block, Landing)); });

  run(Client.close());
  State.counters["depth"] = static_cast<double>(Depth);
}

} // namespace

BENCHMARK(BM_RailRead)
    ->Args({128 << 10, 1})
    ->Args({128 << 10, 16})
    ->Args({1 << 20, 1})
    ->Args({1 << 20, 4})
    ->Args({1 << 20, 16})
    ->Args({8 << 20, 4})
    ->Args({16 << 20, 4})
    ->Args({32 << 20, 4})
    ->Args({64 << 20, 4})
    ->Apply(steadyState);
