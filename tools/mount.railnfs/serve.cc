#include "rail/fs/safe-path.h"
#include "rail/io/runner.h"
#include "rail/nfs/client.h"
#include "rail/nfs/server.h"
#include "rail/version.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage() {
  std::fputs("usage:\n"
             "  mount.railnfs HOST[:EXPORT] MOUNTPOINT [-sfnv] [-o OPTIONS]\n"
             "\n"
             "  -o  daemon options: port= nfsport= backend= sessions= readahead=\n"
             "      anything else is passed to mount.nfs\n"
             "\n"
             "  mount.railnfs --serve HOST[:SUBDIR] [--nfs-port N] [--port N]\n"
             "              [--backend tcp|rdma] [--sessions N] [--threads N]\n"
             "              [--readahead MiB] [--no-checksum]\n"
             "              [--pages N] [--page-size MiB]\n"
             "\n"
             "  mount.railnfs probe [--nfs-port N] [--repeat N]\n",
             stderr);
}

double secondsSince(std::chrono::steady_clock::time_point Start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
}

constexpr size_t kOpCap = 400;
constexpr uint64_t kSpanCap = 256ull << 20;

size_t opsFor(uint64_t Span, size_t Block) { return std::clamp<size_t>(static_cast<size_t>(Span / Block), 1, kOpCap); }

struct Pass {
  size_t Ops = 0;
  uint64_t Bytes = 0;
  double Secs = 1;

  double rate() const { return Ops / Secs; }
};

// The control link this rides on is noisy enough that one pass says little, so
// every case is measured several times and the middle one reported.
Pass middle(std::vector<Pass> Passes) {
  std::ranges::sort(Passes, [](const Pass &A, const Pass &B) { return A.rate() < B.rate(); });
  return Passes[Passes.size() / 2];
}

int runProbe(uint16_t Port, size_t Repeat) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Made = rail::nfs::NfsClient::connect("127.0.0.1", Port);
    if (!Made) co_return std::unexpected(Made.error());
    auto &Client = *Made;

    auto Start = std::chrono::steady_clock::now();
    auto Root = co_await Client.mountRoot("/");
    if (!Root) co_return std::unexpected(Root.error());
    std::printf("  mount            %7.2f ms\n", secondsSince(Start) * 1000);

    Start = std::chrono::steady_clock::now();
    auto Listed = co_await Client.listDirectory(*Root);
    if (!Listed) co_return std::unexpected(Listed.error());
    double Secs = secondsSince(Start);
    std::printf("  readdirplus      %7.2f ms  %6zu entries\n\n", Secs * 1000, Listed->size());

    const rail::nfs::DirEntry *Biggest = nullptr;
    for (const auto &E : *Listed)
      if (!E.Info.Directory && (!Biggest || E.Info.Size > Biggest->Info.Size)) Biggest = &E;
    if (!Biggest) co_return rail::failMessage("no regular file in the export to read");

    const uint64_t Size = Biggest->Info.Size;
    std::printf("  target %s (%.2f GiB)\n\n", Biggest->Name.c_str(), double(Size) / double(1u << 30));

    constexpr size_t kCalls = 100;
    std::vector<Pass> Attrs;
    std::vector<Pass> Names;
    for (size_t Round = 0; Round < Repeat; Round++) {
      Start = std::chrono::steady_clock::now();
      for (size_t I = 0; I < kCalls; I++)
        if (auto A = co_await Client.getAttr(Biggest->File); !A) co_return std::unexpected(A.error());
      Attrs.push_back({kCalls, 0, secondsSince(Start)});

      Start = std::chrono::steady_clock::now();
      for (size_t I = 0; I < kCalls; I++)
        if (auto H = co_await Client.lookup(*Root, Biggest->Name); !H) co_return std::unexpected(H.error());
      Names.push_back({kCalls, 0, secondsSince(Start)});
    }

    const Pass Attr = middle(Attrs);
    const Pass Name = middle(Names);
    std::printf("  getattr          %6zu calls  %8.1f ops/s  %7.2f ms each\n", Attr.Ops, Attr.rate(), 1000 / Attr.rate());
    std::printf("  lookup           %6zu calls  %8.1f ops/s  %7.2f ms each\n\n", Name.Ops, Name.rate(), 1000 / Name.rate());

    for (const size_t Block : {size_t{4} << 10, size_t{128} << 10, size_t{1} << 20}) {
      const size_t Ops = opsFor(std::min(Size, kSpanCap), Block);
      std::vector<std::byte> Into(Block);

      std::vector<Pass> Passes;
      for (size_t Round = 0; Round < Repeat; Round++) {
        Start = std::chrono::steady_clock::now();
        uint64_t Done = 0;
        for (size_t I = 0; I < Ops; I++) {
          auto Got = co_await Client.read(Biggest->File, (I * Block) % Size, Into);
          if (!Got) co_return std::unexpected(Got.error());
          Done += Got->Bytes;
        }
        Passes.push_back({Ops, Done, secondsSince(Start)});
      }

      const Pass Got = middle(Passes);
      std::printf("  sequential %4zuK  %6zu reads  %8.1f ops/s  %7.2f ms each  %7.2f MB/s\n",
                  Block >> 10,
                  Got.Ops,
                  Got.rate(),
                  1000 / Got.rate(),
                  double(Got.Bytes) / 1e6 / Got.Secs);
    }
    std::printf("\n");

    for (const size_t Block : {size_t{128} << 10, size_t{1} << 20}) {
      for (const size_t Depth : {size_t{1}, size_t{4}, size_t{16}, size_t{32}}) {
        const size_t Ops = opsFor(std::min(Size, kSpanCap), Block);
        std::vector<std::vector<std::byte>> Landing(Depth, std::vector<std::byte>(Block));

        std::vector<size_t> Free(Depth);
        for (size_t I = 0; I < Depth; I++) Free[I] = I;

        Start = std::chrono::steady_clock::now();
        uint64_t Done = 0;
        size_t Sent = 0;
        size_t Taken = 0;
        while (Taken < Ops) {
          while (Sent < Ops && !Free.empty()) {
            const size_t Slot = Free.back();
            Free.pop_back();
            if (auto Ok = co_await Client.submitRead(Biggest->File, (Sent * Block) % Size, Landing[Slot]); !Ok) co_return std::unexpected(Ok.error());
            Sent++;
          }
          auto Got = co_await Client.collectRead();
          if (!Got) co_return std::unexpected(Got.error());
          Done += Got->Bytes;
          for (size_t I = 0; I < Depth; I++)
            if (Landing[I].data() == Got->Into.data()) Free.push_back(I);
          Taken++;
        }
        Secs = secondsSince(Start);
        std::printf("  pipelined %4zuK depth %2zu  %6zu reads  %8.1f ops/s  %7.2f MB/s\n",
                    Block >> 10,
                    Depth,
                    Ops,
                    Ops / Secs,
                    double(Done) / 1e6 / Secs);
      }
    }

    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "mount.railnfs: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

rail::Result<rail::nfs::ExportOptions> parse(int Argc, char **Argv) {
  rail::nfs::ExportOptions Opts;

  const std::string Target = Argv[1];
  const size_t Cut = Target.find(':');
  Opts.Remote.Host = Cut == std::string::npos ? Target : Target.substr(0, Cut);
  if (Cut != std::string::npos) {
    auto Root = rail::exportRoot(Target.substr(Cut + 1));
    if (!Root) return std::unexpected(Root.error());

    Opts.Remote.Root = *Root;
  }

  bool GeometryGiven = false;
  for (int I = 2; I < Argc; I++) {
    if (std::strcmp(Argv[I], "--nfs-port") == 0 && I + 1 < Argc) Opts.Port = static_cast<uint16_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--port") == 0 && I + 1 < Argc)
      Opts.Remote.Service.Port = static_cast<uint16_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--backend") == 0 && I + 1 < Argc) Opts.Remote.Service.Backend = Argv[++I];
    else if (std::strcmp(Argv[I], "--sessions") == 0 && I + 1 < Argc) Opts.Remote.Sessions = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--threads") == 0 && I + 1 < Argc) Opts.Threads = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--no-checksum") == 0) Opts.Remote.Service.Verify = false;
    else if (std::strcmp(Argv[I], "--readahead") == 0 && I + 1 < Argc) Opts.Remote.Readahead = std::strtoull(Argv[++I], nullptr, 10) << 20;
    else if (std::strcmp(Argv[I], "--pages") == 0 && I + 1 < Argc) Opts.Remote.Service.PageCount = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--page-size") == 0 && I + 1 < Argc) {
      Opts.Remote.Service.PageSize = std::strtoull(Argv[++I], nullptr, 10) << 20;
      GeometryGiven = true;
    }
  }

  // A window is fetched in one call, so it has to fit in one page.
  if (!GeometryGiven && Opts.Remote.Readahead > Opts.Remote.Service.PageSize) {
    Opts.Remote.Service.PageSize = Opts.Remote.Readahead;
    Opts.Remote.Service.PageCount = 4;
  }
  return Opts;
}

} // namespace

void railnfsUsage() { usage(); }

int railnfsServe(int Argc, char **Argv) {
  std::signal(SIGPIPE, SIG_IGN);

  if (Argc >= 2 && std::strcmp(Argv[1], "--version") == 0) {
    std::printf("mount.railnfs %s\n", rail::version());
    return 0;
  }
  if (Argc < 2) {
    usage();
    return 2;
  }

  if (std::strcmp(Argv[1], "probe") == 0) {
    uint16_t Port = 2049;
    size_t Repeat = 3;
    bool GeometryGiven = false;
    for (int I = 2; I < Argc; I++) {
      if (std::strcmp(Argv[I], "--nfs-port") == 0 && I + 1 < Argc) Port = static_cast<uint16_t>(std::strtoul(Argv[++I], nullptr, 10));
      else if (std::strcmp(Argv[I], "--repeat") == 0 && I + 1 < Argc) Repeat = std::max<size_t>(1, std::strtoul(Argv[++I], nullptr, 10));
    }
    return runProbe(Port, Repeat);
  }

  auto Parsed = parse(Argc, Argv);
  if (!Parsed) {
    std::fprintf(stderr, "mount.railnfs: %s\n", Parsed.error().message().c_str());
    return 1;
  }

  const rail::nfs::ExportOptions Opts = *Parsed;
  std::fprintf(stderr,
               "mount.railnfs: exporting %s:%s on port %u over %s\n",
               Opts.Remote.Host.c_str(),
               Opts.Remote.Root.c_str(),
               Opts.Port,
               Opts.Remote.Service.Backend.c_str());

  auto R = rail::nfs::serveExportThreaded(Opts);
  if (!R) {
    std::fprintf(stderr, "mount.railnfs: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}
