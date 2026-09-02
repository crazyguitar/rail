#include "rail/file-service.h"
#include "rail/io/runner.h"
#include "rail/version.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {

constexpr size_t kRequestedWindow = 16;

void usage() {
  std::fputs("usage:\n"
             "  raild --serve DIR [--port N] [--backend tcp|rdma] [--max-sessions N] [--threads N]\n"
             "  raild ls    HOST PATH [--port N] [--backend tcp|rdma]\n"
             "  raild stat  HOST PATH [--port N] [--backend tcp|rdma]\n"
             "  raild get   HOST PATH LOCAL [--port N] [--backend tcp|rdma] [--pages N] [--page-size MiB]\n"
             "  raild probe HOST DIR  [--port N] [--backend tcp|rdma]\n"
             "  raild put   HOST LOCAL PATH [--port N] [--backend tcp|rdma] [--pages N] [--page-size MiB]\n",
             stderr);
}

rail::ServiceOptions parseOptions(int Argc, char **Argv) {
  rail::ServiceOptions Opts;
  for (int I = 1; I < Argc; I++) {
    if (std::strcmp(Argv[I], "--port") == 0 && I + 1 < Argc) Opts.Port = static_cast<uint16_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--backend") == 0 && I + 1 < Argc) Opts.Backend = Argv[++I];
    else if (std::strcmp(Argv[I], "--flip-one-bit") == 0) Opts.FlipOneBit = true;
    else if (std::strcmp(Argv[I], "--pages") == 0 && I + 1 < Argc) Opts.PageCount = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--max-sessions") == 0 && I + 1 < Argc) Opts.MaxSessions = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--threads") == 0 && I + 1 < Argc) Opts.Threads = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--page-size") == 0 && I + 1 < Argc) Opts.PageSize = std::strtoull(Argv[++I], nullptr, 10) << 20;
  }
  return Opts;
}

void raiseDescriptorLimit() {
  ::rlimit Limit{};
  if (::getrlimit(RLIMIT_NOFILE, &Limit) != 0 || Limit.rlim_cur >= Limit.rlim_max) return;

  const ::rlim_t Was = Limit.rlim_cur;
  Limit.rlim_cur = Limit.rlim_max;
  if (::setrlimit(RLIMIT_NOFILE, &Limit) != 0) return;

  std::fprintf(stderr, "raild: descriptors %llu -> %llu\n", static_cast<unsigned long long>(Was), static_cast<unsigned long long>(Limit.rlim_cur));
}

int runServe(const std::string &Dir, const rail::ServiceOptions &Opts) {
  raiseDescriptorLimit();
  std::fprintf(stderr, "raild: serving %s on port %u over %s\n", Dir.c_str(), Opts.Port, Opts.Backend.c_str());
  auto R = rail::serveFilesThreaded(Dir, Opts);
  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

int runList(const std::string &Host, const std::string &Path, const rail::ServiceOptions &Opts) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Client = co_await rail::FileClient::connect(Host, Opts);
    if (!Client) co_return std::unexpected(Client.error());

    auto Reply = co_await (*Client)->list(Path);
    if (!Reply) co_return std::unexpected(Reply.error());
    if (!Reply->Found) co_return rail::failMessage("no such directory");

    for (const auto &E : Reply->Entries)
      std::printf("%s %8llu %s\n", E.Attrs.Directory ? "d" : "-", static_cast<unsigned long long>(E.Attrs.Size), E.Name.c_str());

    co_await (*Client)->close();
    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

int runStat(const std::string &Host, const std::string &Path, const rail::ServiceOptions &Opts) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Client = co_await rail::FileClient::connect(Host, Opts);
    if (!Client) co_return std::unexpected(Client.error());

    auto Reply = co_await (*Client)->stat(Path);
    if (!Reply) co_return std::unexpected(Reply.error());
    if (!Reply->Found) co_return rail::failMessage("no such file");

    std::printf("%s size=%llu mode=%o dir=%d\n",
                Path.c_str(),
                static_cast<unsigned long long>(Reply->Attrs.Size),
                Reply->Attrs.Mode,
                Reply->Attrs.Directory ? 1 : 0);

    co_await (*Client)->close();
    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

int runGet(const std::string &Host, const std::string &Path, const std::string &Local, const rail::ServiceOptions &Opts) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Client = co_await rail::FileClient::connect(Host, Opts);
    if (!Client) co_return std::unexpected(Client.error());

    auto Got = co_await (*Client)->fetch(Path, Local);
    if (!Got) co_return std::unexpected(Got.error());

    std::fprintf(stderr, "raild: got %llu bytes\n", static_cast<unsigned long long>(*Got));
    co_await (*Client)->close();
    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

int runPut(const std::string &Host, const std::string &Local, const std::string &Path, const rail::ServiceOptions &Opts) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Client = co_await rail::FileClient::connect(Host, Opts);
    if (!Client) co_return std::unexpected(Client.error());

    auto Put = co_await (*Client)->store(Local, Path);
    if (!Put) co_return std::unexpected(Put.error());

    std::fprintf(stderr, "raild: put %llu bytes\n", static_cast<unsigned long long>(*Put));
    co_await (*Client)->close();
    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

double secondsSince(std::chrono::steady_clock::time_point Start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
}

int runProbe(const std::string &Host, const std::string &Dir, const rail::ServiceOptions &Opts) {
  auto R = rail::run([&]() -> rail::Coro<rail::Result<void>> {
    auto Client = co_await rail::FileClient::connect(Host, Opts);
    if (!Client) co_return std::unexpected(Client.error());
    auto &C = **Client;

    auto Listed = co_await C.list(Dir);
    if (!Listed) co_return std::unexpected(Listed.error());
    if (!Listed->Found) co_return rail::failMessage("no such directory");

    std::printf("directory %s: %zu entries\n\n", Dir.c_str(), Listed->Entries.size());

    auto Start = std::chrono::steady_clock::now();
    size_t Stats = 0;
    for (const auto &E : Listed->Entries) {
      auto Info = co_await C.stat(Dir + "/" + E.Name);
      if (!Info) co_return std::unexpected(Info.error());
      Stats++;
    }
    double Secs = secondsSince(Start);
    std::printf("  metadata walk   %6zu stats  %8.1f ops/s  %7.2f ms each\n", Stats, Stats / Secs, Secs * 1000 / double(Stats ? Stats : 1));

    const rail::proto::ListEntry *Biggest = nullptr;
    for (const auto &E : Listed->Entries)
      if (!E.Attrs.Directory && (!Biggest || E.Attrs.Size > Biggest->Attrs.Size)) Biggest = &E;
    if (!Biggest) co_return rail::failMessage("no regular file to read");

    const std::string Target = Dir + "/" + Biggest->Name;
    const uint64_t Size = Biggest->Attrs.Size;
    std::printf("  target %s (%.2f GiB)\n\n", Biggest->Name.c_str(), double(Size) / double(1u << 30));

    for (const size_t Block : {size_t{4} << 10, size_t{128} << 10, size_t{1} << 20}) {
      if (Block > C.maxTransfer()) continue;
      const uint64_t Span = std::min<uint64_t>(Size, 256ull << 20);
      std::vector<std::byte> Into(Block);

      Start = std::chrono::steady_clock::now();
      uint64_t Done = 0;
      size_t Ops = 0;
      while (Done < Span) {
        auto Got = co_await C.read(Target, Done, Into);
        if (!Got) co_return std::unexpected(Got.error());
        if (Got->Bytes == 0) break;
        Done += Got->Bytes;
        Ops++;
      }
      Secs = secondsSince(Start);
      std::printf("  sequential %4zuK  %6zu reads  %8.1f ops/s  %7.2f ms each  %7.2f MB/s\n",
                  Block >> 10,
                  Ops,
                  Ops / Secs,
                  Secs * 1000 / double(Ops ? Ops : 1),
                  double(Done) / 1e6 / Secs);
    }

    for (const size_t Depth : {size_t{4}, size_t{8}, size_t{16}}) {
      const size_t Window = std::min(Depth, C.maxOutstanding());
      const size_t Block = 128 << 10;
      if (Block > C.maxTransfer()) continue;

      const uint64_t Span = std::min<uint64_t>(Size, 256ull << 20);
      std::vector<std::vector<std::byte>> Landing(Window, std::vector<std::byte>(Block));

      Start = std::chrono::steady_clock::now();
      uint64_t Asked = 0;
      uint64_t Done = 0;
      size_t Ops = 0;
      size_t Live = 0;
      size_t Slot = 0;
      bool Failed = false;
      while (Done < Span && !Failed) {
        while (Live < Window && Asked < Span) {
          if (auto R = co_await C.submitRead(Target, Asked, Landing[(Slot + Live) % Window]); !R) {
            Failed = true;
            break;
          }
          Asked += Block;
          Live++;
        }
        if (Failed) break;
        auto Got = co_await C.collectRead();
        if (!Got) co_return std::unexpected(Got.error());
        Done += Got->Bytes;
        Live--;
        Ops++;
        Slot = (Slot + 1) % Window;
      }
      Secs = secondsSince(Start);
      std::printf("  pipelined 128K depth %2zu  %6zu reads  %8.1f ops/s  %7.2f MB/s\n", Window, Ops, Ops / Secs, double(Done) / 1e6 / Secs);
    }

    std::mt19937_64 Rng(12345);
    for (const size_t Block : {size_t{4} << 10, size_t{128} << 10}) {
      if (Block > C.maxTransfer() || Size <= Block) continue;
      std::vector<std::byte> Into(Block);
      constexpr size_t kOps = 200;

      Start = std::chrono::steady_clock::now();
      for (size_t I = 0; I < kOps; I++) {
        const uint64_t At = (Rng() % (Size - Block)) & ~uint64_t{4095};
        auto Got = co_await C.read(Target, At, Into);
        if (!Got) co_return std::unexpected(Got.error());
      }
      Secs = secondsSince(Start);
      std::printf("  random     %4zuK  %6zu reads  %8.1f ops/s  %7.2f ms each  %7.2f MB/s\n",
                  Block >> 10,
                  kOps,
                  kOps / Secs,
                  Secs * 1000 / double(kOps),
                  double(kOps * Block) / 1e6 / Secs);
    }

    const auto Scratch = std::filesystem::temp_directory_path() / "raild-probe.bin";
    Start = std::chrono::steady_clock::now();
    auto Fetched = co_await C.fetch(Target, Scratch);
    if (!Fetched) co_return std::unexpected(Fetched.error());
    Secs = secondsSince(Start);
    std::printf("\n  streamed fetch  %.2f GiB  %7.2f MB/s  %6.2f Gbps  (for comparison)\n",
                double(*Fetched) / double(1u << 30),
                double(*Fetched) / 1e6 / Secs,
                double(*Fetched) * 8 / 1e9 / Secs);
    std::filesystem::remove(Scratch);

    co_await C.close();
    co_return rail::Result<void>{};
  }());

  if (!R) {
    std::fprintf(stderr, "raild: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

} // namespace

int main(int Argc, char **Argv) {
  std::signal(SIGPIPE, SIG_IGN);

  if (Argc >= 2 && std::strcmp(Argv[1], "--version") == 0) {
    std::printf("raild %s\n", rail::version());
    return 0;
  }

  const rail::ServiceOptions Opts = parseOptions(Argc, Argv);

  if (Argc >= 3 && std::strcmp(Argv[1], "--serve") == 0) return runServe(Argv[2], Opts);
  if (Argc >= 4 && std::strcmp(Argv[1], "ls") == 0) return runList(Argv[2], Argv[3], Opts);
  if (Argc >= 4 && std::strcmp(Argv[1], "stat") == 0) return runStat(Argv[2], Argv[3], Opts);
  if (Argc >= 4 && std::strcmp(Argv[1], "probe") == 0) return runProbe(Argv[2], Argv[3], Opts);
  if (Argc >= 5 && std::strcmp(Argv[1], "get") == 0) return runGet(Argv[2], Argv[3], Argv[4], Opts);
  if (Argc >= 5 && std::strcmp(Argv[1], "put") == 0) return runPut(Argv[2], Argv[3], Argv[4], Opts);

  usage();
  return 2;
}
