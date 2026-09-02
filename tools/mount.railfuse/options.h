#pragma once

#include "rail/fuse/mount.h"

#include <string>

namespace rail::fuse {

// mount(8) execs `/sbin/mount.railfuse SPEC DIR [-sfnv] [-o OPTIONS]` when asked
// for `-t railfuse`. The names inside -o are the kernel mount's, so one option
// string describes either way of reaching the same export.
Result<void> applyMountOptions(const std::string &Options, MountOptions &Opts);

// HOST, or HOST:EXPORT, the spelling both the kernel helper and rail use.
Result<void> applySpec(const std::string &Spec, MountOptions &Opts);

} // namespace rail::fuse
