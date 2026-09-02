#include "rail/fuse/files.h"

#include <utility>

namespace rail::fuse {

uint64_t Files::open(File F) {
  const uint64_t Handle = Next++;
  Open.emplace(Handle, std::move(F));
  return Handle;
}

File *Files::get(uint64_t Handle) {
  auto It = Open.find(Handle);
  return It == Open.end() ? nullptr : &It->second;
}

void Files::close(uint64_t Handle) { Open.erase(Handle); }

uint64_t Files::nextPendingOn(const std::string &Path, uint64_t After) const {
  uint64_t Soonest = 0;
  for (const auto &[Handle, F] : Open) {
    if (Handle <= After || F.Path != Path || (F.PendingLen == 0 && !F.Flushing)) continue;
    if (Soonest == 0 || Handle < Soonest) Soonest = Handle;
  }
  return Soonest;
}

} // namespace rail::fuse
