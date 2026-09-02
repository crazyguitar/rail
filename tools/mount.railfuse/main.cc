#include "options.h"

#include "rail/fuse/mount.h"
#include "rail/io/runner.h"
#include "rail/version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

namespace {

void usage() {
  std::fputs("usage:\n"
             "  railfs HOST[:SUBDIR] MOUNTPOINT [--port N] [--backend tcp|rdma]\n"
             "                                [--sessions N] [--readahead MiB]\n"
             "                                [--pages N] [--page-size MiB]\n"
             "                                [--attr-timeout S] [--entry-timeout S] [--consistent]\n"
             "                                [--buffered] [--max-read BYTES] [-o OPT] [--no-checksum]\n"
             "                                [--stream-after MiB] [--stream-chunk MiB]\n"
             "                                [--write-chunk MiB] [--threads N] [-v]\n"
             "\n"
             "  mount.railfuse --null BYTES MOUNTPOINT [--max-background N] [--max-read BYTES]\n"
             "                               [--no-splice] [--direct-io] [-o OPT]\n"
             "                               [--attr-timeout S] [--entry-timeout S] [--consistent]\n"
             "           serve one synthetic file from memory, for measuring the\n"
             "           cost of the fuse boundary with no network in the path\n",
             stderr);
}

} // namespace

int main(int Argc, char **Argv) {
  std::signal(SIGPIPE, SIG_IGN);
  ::mallopt(M_MMAP_THRESHOLD, 64 << 20);
  ::mallopt(M_TRIM_THRESHOLD, 64 << 20);

  if (Argc < 2) {
    usage();
    return 2;
  }
  if (std::strcmp(Argv[1], "--version") == 0) {
    std::printf("mount.railfuse %s\n", rail::version());
    return 0;
  }

  rail::fuse::MountOptions Opts;
  std::vector<std::string> Positional;
  bool GeometryGiven = false;
  bool NullGiven = false;

  for (int I = 1; I < Argc; I++) {
    if (std::strcmp(Argv[I], "--null") == 0 && I + 1 < Argc) {
      NullGiven = true;
      Opts.NullSize = std::strtoull(Argv[++I], nullptr, 10);
    } else if (std::strcmp(Argv[I], "--attr-timeout") == 0 && I + 1 < Argc) Opts.AttrTimeout = std::strtod(Argv[++I], nullptr);
    else if (std::strcmp(Argv[I], "--entry-timeout") == 0 && I + 1 < Argc) Opts.EntryTimeout = std::strtod(Argv[++I], nullptr);
    else if (std::strcmp(Argv[I], "--consistent") == 0) Opts.AttrTimeout = Opts.EntryTimeout = 0;
    else if (std::strcmp(Argv[I], "--max-background") == 0 && I + 1 < Argc)
      Opts.MaxBackground = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--no-splice") == 0) Opts.Splice = false;
    else if (std::strcmp(Argv[I], "--max-read") == 0 && I + 1 < Argc) Opts.MaxRead = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--direct-io") == 0) Opts.DirectIo = true;
    else if (std::strcmp(Argv[I], "--buffered") == 0) Opts.DirectIo = false;
    else if (std::strcmp(Argv[I], "--flip-one-bit") == 0) Opts.Remote.Service.FlipOneBit = true;
    else if (std::strcmp(Argv[I], "--no-checksum") == 0) Opts.Remote.Service.Verify = false;
    else if (std::strcmp(Argv[I], "--threads") == 0 && I + 1 < Argc) Opts.Threads = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--verbose") == 0 || std::strcmp(Argv[I], "-v") == 0) Opts.Verbose = true;
    else if (std::strcmp(Argv[I], "--stream-after") == 0 && I + 1 < Argc) Opts.StreamAfter = std::strtoull(Argv[++I], nullptr, 10) << 20;
    else if (std::strcmp(Argv[I], "--stream-chunk") == 0 && I + 1 < Argc) Opts.StreamChunk = std::strtoull(Argv[++I], nullptr, 10) << 20;
    else if (std::strcmp(Argv[I], "--write-chunk") == 0 && I + 1 < Argc) Opts.WriteChunk = std::strtoull(Argv[++I], nullptr, 10) << 20;
    else if (std::strcmp(Argv[I], "--port") == 0 && I + 1 < Argc)
      Opts.Remote.Service.Port = static_cast<uint16_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (std::strcmp(Argv[I], "--backend") == 0 && I + 1 < Argc) Opts.Remote.Service.Backend = Argv[++I];
    else if (std::strcmp(Argv[I], "--sessions") == 0 && I + 1 < Argc) Opts.Remote.Sessions = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--readahead") == 0 && I + 1 < Argc) Opts.Remote.Readahead = std::strtoull(Argv[++I], nullptr, 10) << 20;
    else if (std::strcmp(Argv[I], "--pages") == 0 && I + 1 < Argc) Opts.Remote.Service.PageCount = std::strtoul(Argv[++I], nullptr, 10);
    else if (std::strcmp(Argv[I], "--page-size") == 0 && I + 1 < Argc) {
      GeometryGiven = true;
      Opts.Remote.Service.PageSize = std::strtoull(Argv[++I], nullptr, 10) << 20;
    } else if (std::strcmp(Argv[I], "-o") == 0 && I + 1 < Argc) {
      if (auto R = rail::fuse::applyMountOptions(Argv[++I], Opts); !R) {
        std::fprintf(stderr, "mount.railfuse: %s\n", R.error().message().c_str());
        return 2;
      }
    }
    // mount(8) passes these and expects them ignored rather than refused.
    else if (std::strcmp(Argv[I], "-s") == 0 || std::strcmp(Argv[I], "-f") == 0 || std::strcmp(Argv[I], "-n") == 0) continue;
    else if (Argv[I][0] != '-') Positional.emplace_back(Argv[I]);
  }

  if (Positional.empty() || (Positional.size() < 2 && !NullGiven)) {
    usage();
    return 2;
  }

  if (Opts.Threads > 255) {
    std::fprintf(stderr, "mount.railfuse: --threads %zu is more than the 255 a file handle can name\n", Opts.Threads);
    return 2;
  }

  Opts.MountPoint = Positional.back();

  // The spec loses to -o host=, so a mount unit may say either.
  if (Positional.size() >= 2 && Opts.Remote.Host.empty()) {
    if (auto Applied = rail::fuse::applySpec(Positional.front(), Opts); !Applied) {
      std::fprintf(stderr, "mount.railfuse: %s\n", Applied.error().message().c_str());
      return 1;
    }
  }

  if (!GeometryGiven && Opts.Remote.Readahead > Opts.Remote.Service.PageSize) {
    Opts.Remote.Service.PageSize = Opts.Remote.Readahead;
    Opts.Remote.Service.PageCount = 4;
  }

  auto R = rail::run(rail::fuse::serveMount(Opts));
  if (!R) {
    std::fprintf(stderr, "railfs: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}
