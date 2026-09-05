
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

enum class MetadataOp { Stat, OpenClose, SetMode, StatFs, Truncate };

Coro<Result<void>> metadataRound(FileClient &Client, const std::string &Name, size_t Times, MetadataOp Op, uint64_t Size) {
  for (size_t I = 0; I < Times; I++) {
    switch (Op) {
    case MetadataOp::Stat: {
      auto Seen = co_await Client.stat(Name);
      if (!Seen) co_return std::unexpected(Seen.error());
      if (!Seen->Found) co_return failMessage("metadata benchmark target was not found");
      break;
    }
    case MetadataOp::OpenClose: {
      auto Opened = co_await Client.openFile(Name, false);
      if (!Opened) co_return std::unexpected(Opened.error());
      if (!Opened->Ok) co_return failMessage(Opened->Error);
      if (auto Closed = co_await Client.closeFile(Opened->Handle); !Closed) co_return Closed;
      break;
    }
    case MetadataOp::SetMode:
      if (auto Changed = co_await Client.setMode(Name, 0644); !Changed) co_return Changed;
      break;
    case MetadataOp::Truncate:
      // Keep the shared data fixture's size and contents intact.
      if (auto Changed = co_await Client.truncate(Name, Size); !Changed) co_return Changed;
      break;
    case MetadataOp::StatFs: {
      auto Seen = co_await Client.statFs(Name);
      if (!Seen) co_return std::unexpected(Seen.error());
      if (!Seen->Ok) co_return failMessage(Seen->Error);
      break;
    }
    }
  }
  co_return Result<void>{};
}

void metadata(benchmark::State &State, MetadataOp Op) {
  const size_t Batch = static_cast<size_t>(State.range(0));

  if (!exportReady(State)) return;

  auto Opened = openService();
  if (!orSkip(State, Opened)) return;
  auto &Client = **Opened;
  const std::string Target = targetFor(targetSize());
  uint64_t Size = 0;
  if (Op == MetadataOp::Truncate) {
    auto Seen = run(Client.stat(Target));
    if (!Seen || !Seen->Found) {
      State.SkipWithError("truncate benchmark target was not found");
      run(Client.close());
      return;
    }
    Size = Seen->Attrs.Size;
  }

  // Check success before timing as well as within every measured batch.
  auto Warm = run(metadataRound(Client, Target, Batch, Op, Size));
  if (!Warm) State.SkipWithError(Warm.error().message().c_str());
  else {
    for (auto _ : State) {
      auto Done = run(metadataRound(Client, Target, Batch, Op, Size));
      if (Done) continue;
      State.SkipWithError(Done.error().message().c_str());
      break;
    }
    State.SetItemsProcessed(static_cast<int64_t>(State.iterations()) * static_cast<int64_t>(Batch));
  }
  run(Client.close());
}

void BM_RailStat(benchmark::State &State) { metadata(State, MetadataOp::Stat); }
void BM_RailOpenClose(benchmark::State &State) { metadata(State, MetadataOp::OpenClose); }
void BM_RailSetMode(benchmark::State &State) { metadata(State, MetadataOp::SetMode); }
void BM_RailStatFs(benchmark::State &State) { metadata(State, MetadataOp::StatFs); }
void BM_RailTruncate(benchmark::State &State) { metadata(State, MetadataOp::Truncate); }

} // namespace

BENCHMARK(BM_RailStat)->Arg(100)->Arg(1000)->UseRealTime();
BENCHMARK(BM_RailOpenClose)->Arg(100)->UseRealTime();
BENCHMARK(BM_RailSetMode)->Arg(100)->UseRealTime();
BENCHMARK(BM_RailStatFs)->Arg(100)->UseRealTime();
BENCHMARK(BM_RailTruncate)->Arg(100)->UseRealTime();

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
