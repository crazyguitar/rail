
#include "fixtures.h"

#include <cstring>

namespace {
using namespace rail;
using namespace rail::bench;

void BM_NfsRead(benchmark::State &State) {
  const size_t Block = static_cast<size_t>(State.range(0));
  const size_t Depth = static_cast<size_t>(State.range(1));

  if (!exportReady(State)) return;

  auto Opened = openOverNfs(targetFor(kTargetSize));
  if (!orSkip(State, Opened)) return;

  std::vector<std::vector<std::byte>> Landing(Depth, std::vector<std::byte>(Block));
  uint64_t Offset = 0;

  measure(State, Depth * Block, [&] { return run(readRound(Opened->Client, Opened->File, Offset, Block, Landing)); });
}

} // namespace

BENCHMARK(BM_NfsRead)->Args({128 << 10, 1})->Args({128 << 10, 16})->Args({1 << 20, 1})->Args({1 << 20, 4})->Args({1 << 20, 16})->Apply(steadyState);
