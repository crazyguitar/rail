#pragma once

#include "rail/app/report.h"
#include "remote-host.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace rail::e2e {

// Peer this suite pushes to, from RAIL_PEER. There is no default: a guessed
// hostname fails as a name that does not resolve, which reads like a network
// fault rather than a missing setting.
std::string peerHost();

// The connected peer, opened once for the whole suite.
RemoteHost &peer();

// Directory used on both sides. RAIL_DIR, default /tmp/rail-e2e.
std::filesystem::path localDir();
std::string remoteDir();
std::string remotePath(const std::string &Name);

std::filesystem::path serviceBinary();
std::filesystem::path exportBinary();
std::filesystem::path mountBinary();

// Local fixtures. Content is deterministic in Seed, so both sides can build
// identical or deliberately divergent files without shipping bytes around.
std::filesystem::path makeFile(const std::string &Name, uint64_t Size, uint32_t Seed);
std::filesystem::path makeEmptyFile(const std::string &Name);

// A file of Count identical blocks. Every block shares one weak checksum, so
// the whole signature collapses into a single hash bucket.
std::filesystem::path makeRepeatingFile(const std::string &Name, uint32_t BlockLength, uint32_t Count);

// Builds Name/{top.bin, sub/nested.bin, empty/} under the local directory and
// returns its root. Enough shape to tell a flat copy from a real walk.
std::filesystem::path makeTree(const std::string &Name);

void setLocalMode(const std::filesystem::path &P, uint32_t Mode);
void overwriteLocal(const std::filesystem::path &P, uint64_t Offset, size_t Length);
void prependByteLocal(const std::filesystem::path &P, std::byte B);
std::string localDigest(const std::filesystem::path &P);

// Places a fixture on the peer and verifies it arrived intact, so a failed
// copy surfaces here rather than later as "the delta matched nothing".
bool seedRemote(const std::filesystem::path &Local, const std::string &Remote);
void expectFileMatches(const std::filesystem::path &Local, const std::string &Remote);
std::filesystem::path freshLocal(const std::string &Name);

void removeRemoteRecursive(const std::string &Remote);

void seedRemoteOnce(const std::filesystem::path &Local, const std::string &Remote);

struct RemoteCopy {
  std::string From;
  std::string To;
};

bool resetRemoteRoot(const std::string &Root, const std::vector<RemoteCopy> &Copies);

void stopPeerProcess(const std::string &Name);
void killPeerProcess(const std::string &Name);

void endLocalProcess(const std::vector<std::string> &Match);

// True when a partial transfer left a temp file behind.
bool remoteHasTempFiles();

struct PushOptions {
  std::string Backend = "tcp";
  std::vector<std::string> Extra;
  // Directory to run the client from. A bare relative source only behaves
  // differently when the process actually sits somewhere else.
  std::string Cwd;
};

// Runs a push and returns the parsed report. Fails the test on non-zero exit.
Report push(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts);

// Runs a pull - HOST:SRC DEST - and returns the parsed report. The report comes
// from the receiving side, which on a pull is the local one.
Report pull(const std::string &Remote, const std::filesystem::path &Local, const PushOptions &Opts);

// Runs a push that is expected to fail and returns its exit status, so a test
// can assert on the failure instead of on a crash.
struct FailedPush {
  int ExitStatus = 0;
  std::string Output;
};
FailedPush pushExpectingFailure(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts);

// A pull expected to be refused, so a test can assert on the refusal.
FailedPush pullExpectingFailure(const std::string &Remote, const std::filesystem::path &Local, const PushOptions &Opts);

// Starts a push, waits until the peer has actually written payload, then kills
// the sender. Returns false if the transfer never got that far, so a test
// cannot quietly pass by interrupting the connection setup instead.
bool pushThenKillOnceWriting(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts);

// Bytes in the peer's temp file, or 0 if there is none.
uint64_t remoteTempBytes();

// Peak resident set of the push process, in KiB.
uint64_t pushMeasuringPeakRss(const std::filesystem::path &Local, const std::string &Remote, const PushOptions &Opts);

extern const std::vector<std::string> kBackends;

// Every suite runs once per backend and needs the same option builder.
class BackendTest : public ::testing::TestWithParam<std::string> {
protected:
  PushOptions options(std::vector<std::string> Extra = {}) const { return {GetParam(), std::move(Extra)}; }
};

} // namespace rail::e2e
