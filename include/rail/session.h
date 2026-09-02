#pragma once

#include "rail/app/filter.h"
#include "rail/app/report.h"
#include "rail/fs/durability.h"
#include "rail/io/coro.h"
#include "rail/proto/control-channel.h"
#include "rail/result.h"

#include <filesystem>
#include <functional>
#include <string>

namespace rail {

enum class DeltaPolicy { Always, Never, Auto };

struct SyncOptions {
  std::string Backend = "tcp";
  // Off by default, unlike rsync, which assumes the network is the scarce
  // resource. Here it is not: on a 2 GiB file whose destination shares nothing,
  // a delta took 134 s against 0.9 s for sending it whole, and even when the
  // destination matched exactly it was 2.5x slower than not looking.
  DeltaPolicy Policy = DeltaPolicy::Never;
  uint32_t BlockSize = 0; // 0 derives from file size
  // Zero derives the geometry from the transfer size; see chooseGeometry.
  // Pool footprint is PageCount * PageSize, allocated up front, so the two
  // together are a memory budget.
  size_t PageCount = 0;
  // Pages the sender keeps in flight, derived with the rest of the geometry.
  // Not exposed: once the loop drives the fabric, one operation in flight
  // reaches 139 Gbps against 141 for sixteen, so there is nothing to tune.
  size_t WindowPages = 0;
  size_t PageSize = 0;
  std::string RemoteCommand = "rail";
  // Descend into directories. rsync skips them without this, and so do we.
  bool Recursive = false;
  FilterRules Filter;
  uint64_t MinSize = 0;
  uint64_t MaxSize = 0;
  // Report what would move without moving it, as rsync's -n does.
  bool DryRun = false;
  // Called once per file as it is sent, for -v. The tool decides where the
  // names go, so the library never writes to a stream it does not own.
  std::function<void(const std::string &Name, uint64_t Bytes)> OnFile;
  // Called as a file moves, for --progress. Done counts matched bytes too, so
  // it reaches Total on a delta that sends almost nothing.
  std::function<void(const std::string &Name, uint64_t Done, uint64_t Total)> OnProgress;
  // True when the source was written with a trailing slash, which in rsync
  // means "the contents of this directory" rather than the directory itself.
  bool SourceContentsOnly = false;
  // Test-only: corrupts one payload byte after it has been hashed, so the
  // receiver's whole-file check is what has to notice. Without it there is no
  // way to prove the check actually guards anything.
  bool FlipOneLiteralBit = false;
  // Forcing the data to the device is serialised at the end of the transfer,
  // so it is off unless asked for.
  Durability Durable = Durability::PageCache;
};

struct RemotePath {
  std::string Host;
  std::filesystem::path Path;

  static Result<RemotePath> parse(std::string_view Spec);
};

// Spawns `ssh <host> <remote-command> --server --receive <path>` and drives
// the push over its stdio pipes, exactly as rsync does.
Coro<Result<Report>> pushPath(const std::filesystem::path &Src, const RemotePath &Dst, const SyncOptions &Opts);

// Runs on the far side of the ssh pipe. Control channel is stdin/stdout, so
// nothing else may write to stdout in this mode.
Coro<Result<void>> serveReceive(const std::filesystem::path &Dst, Durability D);

// Receives over a control channel the caller already owns - the ssh pipes of a
// pull, where this side is local and the remote is the one sending.
Coro<Result<Report>> receiveOver(proto::ControlChannel C, const std::filesystem::path &Dst, Durability D);

// Spawns `ssh <host> <remote-command> --server --sender <path>` and receives what
// it sends. The mirror of pushPath.
Coro<Result<Report>> pullPath(const RemotePath &Src, const std::filesystem::path &Dst, const SyncOptions &Opts);

// Sends a local path over stdio, for the remote end of a pull.
Coro<Result<Report>> servePush(const std::filesystem::path &Src, const SyncOptions &Opts);

} // namespace rail
