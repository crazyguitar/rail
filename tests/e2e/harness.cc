#include "harness.h"

#include "local-process.h"
#include "rail/app/checksum.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <thread>

namespace rail::e2e {

const std::vector<std::string> kBackends = {"tcp", "rdma"};

namespace {

constexpr size_t kChunk = 1u << 20;

std::string envOr(const char *Name, const char *Fallback) {
  const char *V = ::getenv(Name);
  return V && *V ? V : Fallback;
}

std::filesystem::path binary() { return envOr("RAIL_BINARY", (std::filesystem::current_path() / "build/tools/rail/rail").c_str()); }

std::vector<std::string> pushArgv(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts) {
  std::vector<std::string> Argv{
      binary().string(),
      Local.string(),
      peerHost() + ":" + Remote,
      "--backend",
      Opts.Backend,
      "--rail-path",
      binary().string(),
      "--report-json",
  };
  Argv.insert(Argv.end(), Opts.Extra.begin(), Opts.Extra.end());
  return Argv;
}

std::vector<std::string> pullArgv(const std::string &Remote, const std::filesystem::path &Local, const PushOptions &Opts) {
  std::vector<std::string> Argv{
      binary().string(),
      peerHost() + ":" + Remote,
      Local.string(),
      "--backend",
      Opts.Backend,
      "--rail-path",
      binary().string(),
      "--report-json",
  };
  Argv.insert(Argv.end(), Opts.Extra.begin(), Opts.Extra.end());
  return Argv;
}

uint64_t jsonNumber(const std::string &Json, const std::string &Key) {
  const std::string Needle = "\"" + Key + "\":";
  const size_t At = Json.find(Needle);
  if (At == std::string::npos) return 0;
  return std::strtoull(Json.c_str() + At + Needle.size(), nullptr, 10);
}

std::string jsonString(const std::string &Json, const std::string &Key) {
  const std::string Needle = "\"" + Key + "\":\"";
  const size_t At = Json.find(Needle);
  if (At == std::string::npos) return {};
  const size_t Start = At + Needle.size();
  return Json.substr(Start, Json.find('"', Start) - Start);
}

} // namespace

std::string peerHost() { return envOr("RAIL_PEER", ""); }

RemoteHost &peer() {
  // Deliberately never freed: the session is shared by every test and tearing
  // it down during static destruction races with gtest's own teardown.
  static RemoteHost *Host = [] {
    if (peerHost().empty()) {
      ADD_FAILURE() << "RAIL_PEER is not set: name the host that serves, as ssh reaches it";
      std::abort();
    }

    auto Opened = RemoteHost::open(peerHost());
    if (!Opened) {
      ADD_FAILURE() << "cannot reach peer " << peerHost() << ": " << Opened.error().message();
      std::abort();
    }
    auto *H = new RemoteHost(std::move(*Opened));

    // Clear temp files left by an earlier run that died without unwinding, so
    // one crashed run does not fail every later one. Anything a test creates
    // after this point is still a real leak.
    H->makeDirectory(remoteDir());
    if (auto Names = H->listDirectory(remoteDir())) {
      for (const auto &N : *Names)
        if (N.rfind(".rail.tmp.", 0) == 0) H->removeFile(remoteDir() + "/" + N);
    }
    return H;
  }();
  return *Host;
}

std::filesystem::path localDir() {
  auto D = std::filesystem::path(envOr("RAIL_DIR", "/tmp/rail-e2e"));
  std::filesystem::create_directories(D);
  return D;
}

std::string remoteDir() { return envOr("RAIL_DIR", "/tmp/rail-e2e"); }

std::string remotePath(const std::string &Name) { return remoteDir() + "/" + Name; }

std::filesystem::path serviceBinary() { return binary().parent_path().parent_path() / "raild" / "raild"; }

std::filesystem::path exportBinary() { return binary().parent_path().parent_path() / "mount.railnfs" / "mount.railnfs"; }

std::filesystem::path mountBinary() { return binary().parent_path().parent_path() / "mount.railfuse" / "mount.railfuse"; }

std::filesystem::path makeFile(const std::string &Name, uint64_t Size, uint32_t Seed) {
  const auto P = localDir() / Name;
  std::ofstream Out(P, std::ios::binary | std::ios::trunc);

  std::mt19937_64 Rng(Seed);
  std::vector<char> Chunk(kChunk);
  uint64_t Written = 0;
  while (Written < Size) {
    const size_t N = static_cast<size_t>(std::min<uint64_t>(Chunk.size(), Size - Written));
    for (size_t I = 0; I < N; I += 8) {
      const uint64_t V = Rng();
      std::memcpy(Chunk.data() + I, &V, std::min<size_t>(8, N - I));
    }
    Out.write(Chunk.data(), static_cast<std::streamsize>(N));
    Written += N;
  }
  return P;
}

void overwriteLocal(const std::filesystem::path &P, uint64_t Offset, size_t Length) {
  std::fstream F(P, std::ios::binary | std::ios::in | std::ios::out);
  F.seekp(static_cast<std::streamoff>(Offset));
  const std::vector<char> Junk(Length, '\x5a');
  F.write(Junk.data(), static_cast<std::streamsize>(Length));
}

void prependByteLocal(const std::filesystem::path &P, std::byte B) {
  std::ifstream In(P, std::ios::binary);
  const std::string Content((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
  In.close();

  std::ofstream Out(P, std::ios::binary | std::ios::trunc);
  Out.put(static_cast<char>(B));
  Out.write(Content.data(), static_cast<std::streamsize>(Content.size()));
}

std::string localDigest(const std::filesystem::path &P) {
  std::ifstream In(P, std::ios::binary);
  if (!In) return {};

  Hasher H;
  std::vector<char> Buf(kChunk);
  while (In) {
    In.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
    const std::streamsize N = In.gcount();
    if (N <= 0) break;
    H.update({reinterpret_cast<const std::byte *>(Buf.data()), static_cast<size_t>(N)});
  }
  return toHex(H.digest());
}

bool seedRemote(const std::filesystem::path &Local, const std::string &Remote) {
  if (auto R = peer().makeDirectory(remoteDir()); !R) {
    ADD_FAILURE() << R.error().message();
    return false;
  }
  if (auto R = peer().upload(Local, Remote); !R) {
    ADD_FAILURE() << R.error().message();
    return false;
  }

  // Check the size rather than the digest: sftp reads are slow enough that
  // streaming every fixture back doubles the suite's runtime, and a truncated
  // or failed upload is what this is guarding against.
  auto Stat = peer().stat(Remote);
  if (!Stat) {
    ADD_FAILURE() << Stat.error().message();
    return false;
  }
  const auto Expected = std::filesystem::file_size(Local);
  if (Stat->Size == Expected) return true;
  ADD_FAILURE() << std::format("seeded {} is {} bytes, expected {}", Remote, Stat->Size, Expected);
  return false;
}

void expectFileMatches(const std::filesystem::path &Local, const std::string &Remote) {
  EXPECT_TRUE(peer().exists(Remote).value_or(false)) << Remote << " is missing";
  EXPECT_EQ(peer().digest(Remote).value_or(""), localDigest(Local)) << Remote << " differs";
}

std::filesystem::path freshLocal(const std::string &Name) {
  const auto Root = localDir() / Name;
  std::filesystem::remove_all(Root);
  std::filesystem::create_directories(Root);
  return Root;
}

void removeRemoteRecursive(const std::string &Remote) {
  auto Entries = peer().listDirectory(Remote);
  if (!Entries) {
    [[maybe_unused]] auto Removed = peer().removeFile(Remote);
    return;
  }
  for (const auto &Entry : *Entries) {
    if (Entry == "." || Entry == "..") continue;
    removeRemoteRecursive(Remote + "/" + Entry);
  }
  [[maybe_unused]] auto Removed = peer().removeDirectory(Remote);
}

void seedRemoteOnce(const std::filesystem::path &Local, const std::string &Remote) {
  static std::set<std::string> Seeded;
  if (Seeded.contains(Remote)) return;
  if (auto R = peer().makeDirectory(Remote.substr(0, Remote.rfind('/'))); !R) {
    ADD_FAILURE() << R.error().message();
    return;
  }
  if (seedRemote(Local, Remote)) Seeded.insert(Remote);
}

namespace {

void runOnPeerToCompletion(const std::vector<std::string> &Argv) {
  auto Ran = peer().run(Argv);
  if (!Ran) return;
  while (Ran->readLine()) {}
}

bool peerHasProcess(const std::string &Name) {
  auto Ran = peer().run({"pgrep", "-x", "--", Name});
  return Ran && Ran->readLine().has_value();
}

void endPeerProcess(const std::string &Name, const std::string &Signal) {
  runOnPeerToCompletion({"pkill", "-" + Signal, "-x", "--", Name});
  for (int Attempt = 0; Attempt < 100; Attempt++) {
    if (!peerHasProcess(Name)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ADD_FAILURE() << Name << " is still running on the peer";
}

bool makeRemoteParents(const std::string &Root, const std::string &Relative) {
  for (size_t Slash = Relative.find('/'); Slash != std::string::npos; Slash = Relative.find('/', Slash + 1)) {
    if (!peer().makeDirectory(Root + "/" + Relative.substr(0, Slash))) return false;
  }
  return true;
}

} // namespace

bool resetRemoteRoot(const std::string &Root, const std::vector<RemoteCopy> &Copies) {
  runOnPeerToCompletion({"rm", "-rf", "--", Root});
  if (peer().exists(Root).value_or(true)) return false;
  if (!peer().makeDirectory(Root)) return false;

  for (const auto &Copy : Copies) {
    const std::string Target = Root + "/" + Copy.To;
    if (!makeRemoteParents(Root, Copy.To)) return false;
    runOnPeerToCompletion({"cp", "--", Copy.From, Target});
    if (!peer().exists(Target).value_or(false)) return false;
  }
  return true;
}

void stopPeerProcess(const std::string &Name) { endPeerProcess(Name, "TERM"); }

void killPeerProcess(const std::string &Name) { endPeerProcess(Name, "KILL"); }

void endLocalProcess(const std::vector<std::string> &Match) {
  std::vector<std::string> Kill{"pkill"};
  Kill.insert(Kill.end(), Match.begin(), Match.end());
  [[maybe_unused]] auto Killed = runLocal(Kill);

  std::vector<std::string> Find{"pgrep"};
  Find.insert(Find.end(), Match.begin(), Match.end());
  for (int Attempt = 0; Attempt < 250; Attempt++) {
    auto Found = runLocal(Find);
    if (!Found || Found->ExitStatus != 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ADD_FAILURE() << "a local process matching " << Match.back() << " is still running";
}

bool remoteHasTempFiles() {
  auto Names = peer().listDirectory(remoteDir());
  if (!Names) return false;
  for (const auto &N : *Names)
    if (N.rfind(".rail.tmp.", 0) == 0) return true;
  return false;
}

std::filesystem::path makeEmptyFile(const std::string &Name) {
  const auto Path = localDir() / Name;
  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  return Path;
}

std::filesystem::path makeRepeatingFile(const std::string &Name, uint32_t BlockLength, uint32_t Count) {
  const auto Path = localDir() / Name;
  std::vector<char> Block(BlockLength, 'A');

  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  for (uint32_t I = 0; I < Count; I++) Out.write(Block.data(), Block.size());
  return Path;
}

std::filesystem::path makeTree(const std::string &Name) {
  const auto Root = localDir() / Name;
  std::filesystem::remove_all(Root);
  std::filesystem::create_directories(Root / "sub");
  std::filesystem::create_directories(Root / "empty");

  makeFile(Name + "/top.bin", 3u << 20, 41);
  makeFile(Name + "/sub/nested.bin", 5u << 20, 42);
  return Root;
}

void setLocalMode(const std::filesystem::path &P, uint32_t Mode) {
  std::filesystem::permissions(P, std::filesystem::perms(Mode), std::filesystem::perm_options::replace);
}

FailedPush pushExpectingFailure(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts) {

  auto Ran = runLocal(pushArgv(Local, Remote, Opts), Opts.Cwd);
  if (!Ran) {
    ADD_FAILURE() << "could not run rail: " << Ran.error().message();
    return {};
  }
  if (Ran->ExitStatus == 0) ADD_FAILURE() << "push was expected to fail but succeeded\n  " << Ran->Output;
  return {Ran->ExitStatus, Ran->Output};
}

uint64_t remoteTempBytes() {
  auto Names = peer().listDirectory(remoteDir());
  if (!Names) return 0;
  for (const auto &N : *Names) {
    if (N.rfind(".rail.tmp.", 0) != 0) continue;
    if (auto S = peer().stat(remoteDir() + "/" + N)) return S->Size;
  }
  return 0;
}

bool pushThenKillOnceWriting(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts) {

  auto Running = BackgroundProcess::start(pushArgv(Local, Remote, Opts));
  if (!Running) {
    ADD_FAILURE() << "could not run rail: " << Running.error().message();
    return false;
  }

  // Connection setup alone takes seconds, so killing after a fixed interval
  // would interrupt the handshake and prove nothing about a half-written file.
  //
  // The pace matters both ways. Each probe is an sftp round trip, so flat out
  // would load the peer; but the transfer itself only lasts a few hundred
  // milliseconds, because the destination lands in the peer's page cache, and
  // a probe every tenth of a second missed it about a third of the time - the
  // file arrived whole and the test read that as a partial write.
  const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool Writing = false;
  while (std::chrono::steady_clock::now() < Deadline) {
    if (remoteTempBytes() > 0) {
      Writing = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  Running->kill();
  [[maybe_unused]] auto Ignored = Running->wait();
  return Writing;
}

Report reportIn(const std::string &Out) {
  Report R;
  R.Files = jsonNumber(Out, "files");
  R.FileSize = jsonNumber(Out, "file_size");
  R.LiteralBytes = jsonNumber(Out, "literal_bytes");
  R.MatchedBytes = jsonNumber(Out, "matched_bytes");
  R.HashHits = jsonNumber(Out, "hash_hits");
  R.FalseAlarms = jsonNumber(Out, "false_alarms");
  R.ScanTime = std::chrono::nanoseconds(jsonNumber(Out, "scan_ns"));
  R.TransferTime = std::chrono::nanoseconds(jsonNumber(Out, "transfer_ns"));
  R.Backend = jsonString(Out, "backend");
  R.Rails = jsonString(Out, "rails");
  R.DeltaUsed = Out.find("\"delta_used\":true") != std::string::npos;
  return R;
}

Report push(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts) {
  const auto Argv = pushArgv(Local, Remote, Opts);

  Report R;
  auto Ran = runLocal(Argv, Opts.Cwd);
  if (!Ran) {
    ADD_FAILURE() << "could not run rail: " << Ran.error().message();
    return R;
  }
  if (Ran->ExitStatus != 0) {
    ADD_FAILURE() << "push failed (exit " << Ran->ExitStatus << ")\n  " << Ran->Output;
    return R;
  }

  return reportIn(Ran->Output);
}

Report pull(const std::string &Remote, const std::filesystem::path &Local, const PushOptions &Opts) {
  const auto Argv = pullArgv(Remote, Local, Opts);

  Report R;
  auto Ran = runLocal(Argv, Opts.Cwd);
  if (!Ran) {
    ADD_FAILURE() << "could not run rail: " << Ran.error().message();
    return R;
  }
  if (Ran->ExitStatus != 0) {
    ADD_FAILURE() << "pull failed (exit " << Ran->ExitStatus << ")\n  " << Ran->Output;
    return R;
  }

  return reportIn(Ran->Output);
}

FailedPush pullExpectingFailure(const std::string &Remote, const std::filesystem::path &Local, const PushOptions &Opts) {
  FailedPush Out;
  auto Ran = runLocal(pullArgv(Remote, Local, Opts), Opts.Cwd);
  if (!Ran) {
    ADD_FAILURE() << "could not run rail: " << Ran.error().message();
    return Out;
  }
  Out.ExitStatus = Ran->ExitStatus;
  Out.Output = Ran->Output;
  return Out;
}

uint64_t pushMeasuringPeakRss(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts) {
  auto Ran = runLocal(pushArgv(Local, Remote, Opts));
  if (!Ran) {
    ADD_FAILURE() << "could not run rail: " << Ran.error().message();
    return 0;
  }
  if (Ran->ExitStatus != 0) {
    ADD_FAILURE() << std::format("push failed (exit {})\n  {}", Ran->ExitStatus, Ran->Output);
    return 0;
  }
  return Ran->MaxRssKb;
}

} // namespace rail::e2e
