#pragma once

#include "rail/io/coro.h"
#include "rail/result.h"
#include "rail/vfs/remotes.h"

#include <cstdint>

namespace rail::nfs {

struct ExportOptions {
  vfs::RemoteOptions Remote;
  uint16_t Port = 2049;
  // One event loop per thread, each with its own listener on the same port.
  // One thread saturates a core at about 2.2 GiB/s; eight reach 3.3. The
  // client has to open as many connections for them to be reached at all.
  size_t Threads = 8;
};

Coro<Result<void>> serveExport(const ExportOptions &Opts);

// Runs serveExport on Opts.Threads threads and does not return until they do.
Result<void> serveExportThreaded(const ExportOptions &Opts);

} // namespace rail::nfs
