#pragma once

namespace rail {

// Whether a completed file waits for its data to reach the device. rsync
// leaves that to the page cache by default and so do we; a caller writing a
// checkpoint it cannot lose asks for Fsync.
enum class Durability { PageCache, Fsync };

} // namespace rail
