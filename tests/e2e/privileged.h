#pragma once

#include "rail/result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rail::e2e {

// Mounting, loading a module and reading the kernel log are privileged, and
// the suite does them itself rather than shelling out to sudo. Run the whole
// binary under sudo: the environment below refuses to start otherwise.
bool runningAsRoot();

// Waits for something to accept on Host:Port. The daemon takes a moment to
// start listening, and a fixed sleep either flakes or wastes the difference.
bool waitForListener(const std::string &Host, uint16_t Port, std::chrono::milliseconds Patience = std::chrono::seconds(15));

Result<void> mountFilesystem(const std::string &Type, const std::string &Source, const std::string &Target, const std::string &Options);

// Lazy detaches a mount whose peer has gone, which a plain unmount cannot.
Result<void> unmountFilesystem(const std::string &Target, bool Lazy = false);

Result<void> loadModule(const std::filesystem::path &Object, const std::string &Parameters = "");
Result<void> unloadModule(const std::string &Name);

Result<void> dropCaches();

// Plain filesystem work, done directly rather than through a child process.
// The suite is root, so these reach the mount the same way a program would.
bool createEmpty(const std::filesystem::path &At);
bool renameTo(const std::filesystem::path &From, const std::filesystem::path &To);
bool resizeTo(const std::filesystem::path &At, uint64_t Length);
bool setMode(const std::filesystem::path &At, unsigned int Mode);
bool setModifiedTime(const std::filesystem::path &At, int64_t Seconds);
bool makeSymlink(const std::string &Target, const std::filesystem::path &At);
bool makeHardLink(const std::filesystem::path &Existing, const std::filesystem::path &At);
bool syncFilesystem(const std::filesystem::path &At);

// Copies through the mount, so the filesystem sees ordinary writes. Direct
// bypasses the page cache the way dd oflag=direct would.
bool copyInto(const std::filesystem::path &From, const std::filesystem::path &To, bool Direct = false);

std::string readWholeFile(const std::filesystem::path &At);
std::string readRange(const std::filesystem::path &At, uint64_t Offset, uint64_t Length);

// md5, to compare against what the peer's md5sum prints. The project's own
// hash is xxhash and would never match it.
std::string digestBytes(const std::string &Body);
std::string digestFile(const std::filesystem::path &At);
std::string digestMapped(const std::filesystem::path &At);

// Reads with O_DIRECT, so the page cache is out of the path and the answer
// comes from the filesystem itself.
std::string digestDirect(const std::filesystem::path &At, size_t Block, uint64_t SkipBlocks = 0);

// What readdir said, not what a later stat says: the two disagreed when
// readdir hashed the bare name and lookup hashed the whole path.
std::vector<std::string> listNames(const std::filesystem::path &At);
uint64_t inodeFromListing(const std::filesystem::path &Directory, const std::string &Name);
std::vector<std::string> filesUnder(const std::filesystem::path &At);

bool isMountpoint(const std::filesystem::path &At);
int64_t fileSize(const std::filesystem::path &At);
uint64_t inodeOf(const std::filesystem::path &At);
int64_t modifiedTime(const std::filesystem::path &At);
uint64_t freeBlocks(const std::filesystem::path &At);
std::string linkTarget(const std::filesystem::path &At);
bool writeWholeFile(const std::filesystem::path &At, const std::string &Body);

Result<void> clearKernelLog();
Result<std::string> readKernelLog();

} // namespace rail::e2e
