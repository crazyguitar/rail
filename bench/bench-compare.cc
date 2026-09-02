
#include "streams.h"

namespace {
using namespace rail;
using namespace rail::bench;

void BM_Read(benchmark::State &State, Via How) {
  const size_t Q = static_cast<size_t>(State.range(0));

  if (!reachable(State, How)) return;
  warmOnce(How, Q);
  coldData(peerHost());
  coldLocal();

  Fleet Streams(Q, How, false);
  measure(State, kCompareEach * Q, [&] { return Streams.round(kCompareEach * Q); });
}

void BM_Write(benchmark::State &State, Via How) {
  const size_t Q = static_cast<size_t>(State.range(0));

  if (!reachable(State, How)) return;
  warmOnce(How, Q);
  coldData(peerHost());
  coldLocal();
  settleFor(How);

  {
    Fleet Streams(Q, How, true);
    measure(State, kCompareEach * Q, [&] { return Streams.round(kCompareEach * Q); });
  }

  if (How == Via::P2p) return;

  if (How == Via::Rail) {
    if (auto Opened = openService()) {
      for (size_t I = 0; I < Q; I++) run((*Opened)->removeFile(writeFile(I)));
      run((*Opened)->close());
    }
    return;
  }

  for (size_t I = 0; I < Q; I++) ::unlink(pathFor(How, writeFile(I)).c_str());
}

} // namespace

BENCHMARK_CAPTURE(BM_Read, p2p, Via::P2p)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Read, rail, Via::Rail)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Read, nvme, Via::Nvme)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Read, nfs, Via::Nfs)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Read, fuse, Via::Fuse)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Read, railfs, Via::Railfs)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);

BENCHMARK_CAPTURE(BM_Write, p2p, Via::P2p)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Write, rail, Via::Rail)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Write, nvme, Via::Nvme)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Write, nfs, Via::Nfs)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Write, fuse, Via::Fuse)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
BENCHMARK_CAPTURE(BM_Write, railfs, Via::Railfs)->Arg(4)->Arg(8)->Arg(16)->Apply(wholeFile);
