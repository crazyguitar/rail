#include "rail/io/runner.h"
#include "rail/io/stream.h"
#include "rail/session.h"
#include "rail/version.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

void usage() {
  std::fprintf(stderr, R"(usage:
  rail [OPTION...] SRC HOST:DEST
  rail [OPTION...] HOST:SRC DEST
  rail --version

options:
  -v, --verbose          name each file as it is sent
  -n, --dry-run          list what would be sent
  -h, --human-readable   sizes with K, M and G suffixes
  -P, --progress         show progress during transfer
  -r, --recursive        descend into directories
  -a, --archive          same as -r
      --stats            print a summary
  -W, --whole-file       never compute a delta (default)
      --no-whole-file    compute a delta
  -B, --block-size N     delta block size
      --exclude P        skip files matching P
      --include P        keep files matching P, before a later --exclude
      --exclude-from F   read exclude patterns from F, one per line
      --include-from F   read include patterns from F, one per line
  -f, --filter RULE      one "+ pattern" or "- pattern" rule
      --max-size N       skip files larger than N
      --min-size N       skip files smaller than N
      --fsync            fsync on the peer
      --rail-path P    the peer's rail
      --backend tcp|rdma  data channel (default: tcp)
      --pages N          buffer pool pages
      --page-size N      bytes per page
      --window N         pages in flight
      --report-json      print the report as JSON
)");
}

size_t parseSize(const char *S) {
  char *End = nullptr;
  size_t V = std::strtoull(S, &End, 10);
  if (End && *End == 'M') V <<= 20;
  else if (End && *End == 'K') V <<= 10;
  else if (End && *End == 'G') V <<= 30;
  return V;
}

// Options that stand alone, with no value after them. Returns whether Key was
// one of them.
struct Output {
  bool Json = false;
  bool Verbose = false;
  bool Stats = false;
  bool Human = false;
  bool Progress = false;
};

// rsync prints nothing on a successful run unless asked, so neither do we.
bool applyFlag(const std::string &Key, rail::SyncOptions &Opts, Output &Show) {
  if (Key == "-r" || Key == "--recursive" || Key == "-a" || Key == "--archive") Opts.Recursive = true;
  else if (Key == "-v" || Key == "--verbose") Show.Verbose = true;
  else if (Key == "-n" || Key == "--dry-run") Opts.DryRun = true;
  else if (Key == "-h" || Key == "--human-readable") Show.Human = true;
  else if (Key == "--stats") Show.Stats = true;
  else if (Key == "-P" || Key == "--progress") Show.Progress = true;
  else if (Key == "-W" || Key == "--whole-file") Opts.Policy = rail::DeltaPolicy::Never;
  else if (Key == "--no-whole-file") Opts.Policy = rail::DeltaPolicy::Always;
  else if (Key == "--report-json") Show.Json = true;
  else if (Key == "--fsync") Opts.Durable = rail::Durability::Fsync;
  else if (Key == "--fault-inject-flip-literal") Opts.FlipOneLiteralBit = true;
  else return false;
  return true;
}

// Options that take a value. Returns false on an unknown key, having already
// reported it.
bool takesDashedValue(const std::string &Key) { return Key == "-f" || Key == "--filter" || Key == "--exclude" || Key == "--include"; }

bool applyOption(const std::string &Key, const std::string &Value, rail::SyncOptions &Opts) {
  if (Key == "--backend") Opts.Backend = Value;
  else if (Key == "-B" || Key == "--block-size") Opts.BlockSize = static_cast<uint32_t>(parseSize(Value.c_str()));
  else if (Key == "--pages") Opts.PageCount = parseSize(Value.c_str());
  else if (Key == "--window") Opts.WindowPages = parseSize(Value.c_str());
  else if (Key == "--page-size") Opts.PageSize = parseSize(Value.c_str());
  else if (Key == "--rail-path") Opts.RemoteCommand = Value;
  else if (Key == "--exclude") Opts.Filter.addExclude(Value);
  else if (Key == "--include") Opts.Filter.addInclude(Value);
  else if (Key == "--max-size") Opts.MaxSize = parseSize(Value.c_str());
  else if (Key == "--min-size") Opts.MinSize = parseSize(Value.c_str());
  else if (Key == "-f" || Key == "--filter") {
    if (auto R = Opts.Filter.addRule(Value); !R) {
      std::fprintf(stderr, "rail: %s\n", R.error().message().c_str());
      return false;
    }
  } else if (Key == "--exclude-from" || Key == "--include-from") {
    if (auto R = Opts.Filter.addFrom(Value, Key == "--include-from"); !R) {
      std::fprintf(stderr, "rail: %s\n", R.error().message().c_str());
      return false;
    }
  } else {
    std::fprintf(stderr, "rail: unknown option %s\n", Key.c_str());
    return false;
  }
  return true;
}

// rsync takes its options anywhere around the two paths, so the positionals
// are collected rather than fixed at argv[2] and argv[3]. Returns false on a
// bad option, having already reported it.
bool parseCommandLine(int Argc, char **Argv, rail::SyncOptions &Opts, Output &Show, std::vector<std::string> &Paths) {
  for (int I = 1; I < Argc; I++) {
    std::string Key = Argv[I];
    if (Key.empty()) continue;
    if (Key[0] != '-') {
      Paths.push_back(std::move(Key));
      continue;
    }

    std::string Value;
    bool HasInline = false;
    if (const size_t Eq = Key.find('='); Eq != std::string::npos) {
      Value = Key.substr(Eq + 1);
      Key = Key.substr(0, Eq);
      HasInline = true;
    }

    if (applyFlag(Key, Opts, Show)) continue;

    // Short flags bundle, as in rsync: -rvh is -r -v -h. None of them take a
    // value, so splitting cannot swallow one.
    if (!HasInline && Key.size() > 2 && Key[0] == '-' && Key[1] != '-') {
      bool AllFlags = true;
      for (size_t C = 1; C < Key.size() && AllFlags; C++) AllFlags = applyFlag(std::string("-") + Key[C], Opts, Show);
      if (AllFlags) continue;
      std::fprintf(stderr, "rail: unknown option in %s\n", Key.c_str());
      return false;
    }

    // Only take the next argument as a value when it is not itself an option,
    // so "--delta --report-json" reports a missing value rather than quietly
    // treating the flag as the value and dropping it. A filter rule is the
    // exception: "- *.o" opens with a dash and is still a value.
    if (!HasInline && I + 1 < Argc && (Argv[I + 1][0] != '-' || takesDashedValue(Key))) Value = Argv[++I];
    if (Value.empty()) {
      std::fprintf(stderr, "rail: %s needs a value\n", Key.c_str());
      return false;
    }
    if (!applyOption(Key, Value, Opts)) return false;
  }
  return true;
}

// Sizes the way rsync's -h writes them.
std::string readable(uint64_t Bytes, bool Human) {
  if (!Human) return std::to_string(Bytes);

  static const char *Suffix[] = {"", "K", "M", "G", "T"};
  double Value = static_cast<double>(Bytes);
  size_t Step = 0;
  while (Value >= 1024.0 && Step + 1 < std::size(Suffix)) {
    Value /= 1024.0;
    Step++;
  }

  char Text[32];
  std::snprintf(Text, sizeof(Text), Step ? "%.2f%s" : "%.0f%s", Value, Suffix[Step]);
  return Text;
}

// Installs the callbacks behind -v and --progress. Formatting, rates and
// terminal control belong here rather than in the library, which should never
// write to a stream it does not own.
void attachReporting(rail::SyncOptions &Opts, const Output &Show, const std::function<void(const std::string &)> &say) {
  if (Show.Verbose)
    Opts.OnFile = [&Show, &say](const std::string &Name, uint64_t Bytes) { say(std::format("{} ({} bytes)\n", Name, readable(Bytes, Show.Human))); };

  if (!Show.Progress) return;

  auto Started = std::chrono::steady_clock::now();
  std::string Current;
  size_t Widest = 0;

  // Rewrites one line per file, as rsync does, and leaves the finished line in
  // place when the next file starts.
  Opts.OnProgress = [=, &say](const std::string &Name, uint64_t Done, uint64_t Total) mutable {
    if (Name != Current) {
      if (!Current.empty()) say("\n");
      Current = Name;
      Started = std::chrono::steady_clock::now();
    }

    const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - Started).count();
    const uint64_t Percent = Total ? Done * 100 / Total : 100;

    // A file that finishes in microseconds divides into a rate of hundreds of
    // gigabytes a second, which is arithmetic rather than measurement.
    const std::string Rate = Secs < 0.05 ? "-" : readable(static_cast<uint64_t>(double(Done) / Secs), true) + "/s";

    // Rewriting in place leaves the tail of a longer previous line behind.
    std::string Line = std::format("\r{} {} {}% {} ", Name, readable(Done, true), Percent, Rate);
    if (Line.size() < Widest) Line.append(Widest - Line.size(), ' ');
    Widest = std::max(Widest, Line.size());
    say(Line);
  };
}

int runSend(int Argc, char **Argv) {
  rail::SyncOptions Opts;
  Output Show;
  std::vector<std::string> Paths;
  if (!parseCommandLine(Argc, Argv, Opts, Show, Paths)) return 2;

  if (Paths.size() != 2) {
    usage();
    return 2;
  }

  // The trailing slash has to be read before the path is normalised, because
  // std::filesystem drops it.
  Opts.SourceContentsOnly = Paths[0].size() > 1 && Paths[0].back() == '/';

  // rsync decides direction the same way: whichever side carries host: is the
  // remote one. Deciding on a failed parse instead would make "host:" - a
  // remote path with the name left off - look like a local file.
  const auto NamesAHost = [](const std::string &Spec) {
    const auto Colon = Spec.find(':');
    return Colon != std::string::npos && Colon != 0;
  };
  const bool SourceRemote = NamesAHost(Paths[0]);
  const bool DestRemote = NamesAHost(Paths[1]);
  if (SourceRemote && DestRemote) {
    std::fprintf(stderr, "rail: one side must be local\n");
    return 2;
  }
  if (!SourceRemote && !DestRemote) {
    std::fprintf(stderr, "rail: one side must be remote, as HOST:PATH\n");
    return 2;
  }

  const bool Pulling = SourceRemote;
  auto Remote = rail::RemotePath::parse(Pulling ? Paths[0] : Paths[1]);
  if (!Remote) {
    std::fprintf(stderr, "rail: %s\n", Remote.error().message().c_str());
    return 2;
  }

  // Take a private copy of stdout before any library can write to it. UCX logs
  // to stdout by default, which otherwise lands in the middle of the output and
  // makes --report-json unparseable. Everything this tool prints goes to the
  // copy, so it is still the caller's stdout.
  const int Out = ::dup(STDOUT_FILENO);
  if (Out < 0) return 1;
  ::dup2(STDERR_FILENO, STDOUT_FILENO);

  // The report is for a program to read, so anything meant for a person goes
  // to stderr when it is asked for. Sharing one stream leaves the JSON with
  // progress lines in front of it, and unparseable.
  const int Chatter = Show.Json ? STDERR_FILENO : Out;

  const auto emit = [](int Fd, const std::string &Line) { [[maybe_unused]] auto Ignored = ::write(Fd, Line.data(), Line.size()); };
  const auto say = [&](const std::string &Line) { emit(Chatter, Line); };

  attachReporting(Opts, Show, say);

  // Only the far side knows what a pull would move, and it has no way to say
  // so without sending it. Refusing beats reporting a transfer that happened.
  if (Pulling && Opts.DryRun) {
    std::fprintf(stderr, "rail: --dry-run is not supported when pulling\n");
    return 2;
  }

  auto R = Pulling ? rail::run(rail::pullPath(*Remote, Paths[1], Opts)) : rail::run(rail::pushPath(Paths[0], *Remote, Opts));
  if (!R) {
    std::fprintf(stderr, "rail: %s\n", R.error().message().c_str());
    return 1;
  }

  if (Show.Progress) say("\n");

  if (Show.Json) emit(Out, R->toJson() + "\n");
  else if (Show.Verbose || Show.Stats || Show.Progress)
    say(std::format("{}{} files, {} bytes ({} literal, {} matched) over {}\n",
                    Opts.DryRun ? "would send " : (Pulling ? "received " : "sent "),
                    R->Files,
                    readable(R->FileSize, Show.Human),
                    readable(R->LiteralBytes, Show.Human),
                    readable(R->MatchedBytes, Show.Human),
                    R->Backend));
  return 0;
}

int runServer(int Argc, char **Argv) {
  // rsync's shape: `--server [--sender] OPTIONS PATH`. Receiving is the
  // default and --sender inverts it, so the far side of a pull is the sender.
  const char *Path = nullptr;
  bool Sending = false;
  auto Durable = rail::Durability::PageCache;
  rail::SyncOptions Opts;
  for (int I = 2; I < Argc; I++) {
    const std::string_view Arg(Argv[I]);
    if (Arg == "--sender") Sending = true;
    else if (Arg == "--fsync") Durable = rail::Durability::Fsync;
    else if (Arg == "--recursive") Opts.Recursive = true;
    else if (Arg == "--no-whole-file") Opts.Policy = rail::DeltaPolicy::Always;
    else if (Arg == "--block-size" && I + 1 < Argc) Opts.BlockSize = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 10));
    else if (Arg == "--backend" && I + 1 < Argc) Opts.Backend = Argv[++I];
    else if (!Arg.starts_with("--")) Path = Argv[I];
  }

  if (!Path) {
    std::fprintf(stderr, "rail: --server requires a path\n");
    return 2;
  }

  if (Sending) {
    auto R = rail::run(rail::servePush(Path, Opts));
    if (!R) {
      std::fprintf(stderr, "rail server: %s\n", R.error().message().c_str());
      return 1;
    }
    return 0;
  }

  auto R = rail::run(rail::serveReceive(Path, Durable));
  if (!R) {
    std::fprintf(stderr, "rail server: %s\n", R.error().message().c_str());
    return 1;
  }
  return 0;
}

} // namespace

int main(int Argc, char **Argv) {
  std::signal(SIGPIPE, SIG_IGN);

  if (Argc >= 2 && std::strcmp(Argv[1], "--version") == 0) {
    std::printf("rail %s\n", rail::version());
    return 0;
  }
  if (Argc >= 2 && std::strcmp(Argv[1], "--server") == 0) return runServer(Argc, Argv);
  if (Argc >= 3) return runSend(Argc, Argv);

  usage();
  return 2;
}
