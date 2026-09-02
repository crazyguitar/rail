#pragma once

#include "rail/app/checksum.h"

#include "rail/address-space.h"
#include "rail/io/coro.h"
#include "rail/proto/message.h"
#include "rail/result.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rail {

class PageSink;
class PageSource;

struct ServiceOptions {
  std::string Backend = "rdma";
  uint16_t Port = 18600;
  size_t PageCount = 12;
  size_t PageSize = 8u << 20;
  bool FlipOneBit = false;
  // Hash every page and check it on arrival. Costs bandwidth on a fabric that
  // already checks its own CRC, so a mount that trusts the wire can turn it
  // off; the client asks and the daemon follows, so both ends always agree.
  bool Verify = true;
  // The hash both ends will use. A client that cannot compute the default
  // names another rather than switching verification off.
  Sum Checksum = Sum::XxH3;
  uint16_t PretendVersion = 0;
  // Clients served at once. Zero asks the machine what it can hold - see
  // sessionsAffordable() - which is what a cluster wants, since the answer
  // differs between one node and the next. A number here overrides it.
  size_t MaxSessions = 0;
  // How many threads answer. Each gets its own event loop, its own io_uring
  // and its own listener on the shared port, so a session never crosses
  // between them and nothing has to be locked on the request path. One thread
  // saturates at about 4 GB/s against a kernel mount, which is what this is
  // for.
  size_t Threads = 0;
};

Coro<Result<void>> serveFiles(const std::filesystem::path &Root, const ServiceOptions &Opts);

// Runs serveFiles on Opts.Threads threads and does not return until they do.
// Blocking, unlike serveFiles, because there is no one loop left to await on.
Result<void> serveFilesThreaded(const std::filesystem::path &Root, const ServiceOptions &Opts);

// What this machine can hold at once, from its descriptor limit, the memory it
// has left, and how many cores there are to answer with. Reports how it got
// there, because a refused client is otherwise a mystery.
size_t sessionsAffordable(const ServiceOptions &Opts, bool Explain = false);

struct ReadOutcome {
  size_t Bytes = 0;
  uint64_t FileSize = 0;
};

class FileClient {
public:
  static Coro<Result<std::unique_ptr<FileClient>>> connect(const std::string &Host, const ServiceOptions &Opts);

  FileClient(const FileClient &) = delete;
  FileClient &operator=(const FileClient &) = delete;
  ~FileClient();

  Coro<Result<proto::StatReply>> stat(const std::string &Path);
  Coro<Result<proto::ListReply>> list(const std::string &Path);
  Coro<Result<ReadOutcome>> read(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle = 0);
  Coro<Result<ReadOutcome>> read(const std::string &Path, uint64_t Offset, Page &Into, uint64_t Handle = 0);

  // Returns the id of the read it posted. Collecting by that id rather than in
  // submission order is what lets several callers share one connection.
  Coro<Result<uint64_t>> submitRead(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle = 0);
  Coro<Result<uint64_t>> submitRead(const std::string &Path, uint64_t Offset, Page &Into, uint64_t Handle = 0);
  Coro<Result<ReadOutcome>> collectRead(uint64_t Id);

  // The oldest read still outstanding, for a caller that pipelines on its own
  // and collects in the order it asked.
  Coro<Result<ReadOutcome>> collectRead();
  size_t maxOutstanding() const;
  Coro<Result<void>> write(const std::string &Path, uint64_t Offset, std::span<const std::byte> From, bool Truncate = false, uint64_t Handle = 0);

  Coro<Result<proto::OpenReply>> openFile(const std::string &Path, bool Writable);
  Coro<Result<void>> closeFile(uint64_t Handle);

  Coro<Result<void>> makeDirectory(const std::string &Path, uint32_t Mode = 0755);
  Coro<Result<void>> removeFile(const std::string &Path);
  Coro<Result<void>> removeDirectory(const std::string &Path);
  Coro<Result<void>> rename(const std::string &From, const std::string &To);
  Coro<Result<void>> truncate(const std::string &Path, uint64_t Size);
  Coro<Result<void>> setMode(const std::string &Path, uint32_t Mode);
  Coro<Result<void>> setMtime(const std::string &Path, int64_t Mtime);
  Coro<Result<void>> fsync(const std::string &Path, uint64_t Handle = 0);
  Coro<Result<void>> makeLink(const std::string &Path, const std::string &Target);
  Coro<Result<void>> hardLink(const std::string &Path, const std::string &Target);
  Coro<Result<std::string>> readLink(const std::string &Path);
  Coro<Result<proto::StatFsReply>> statFs(const std::string &Path);

  bool alive() const;

  Coro<Result<uint64_t>> fetch(const std::string &Path, const std::filesystem::path &Local);
  Coro<Result<uint64_t>> store(const std::filesystem::path &Local, const std::string &Path);
  Coro<Result<uint64_t>> fetchInto(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, uint64_t Handle = 0);
  Coro<Result<uint64_t>> fetchInto(const std::string &Path, uint64_t Offset, AddressSpace &Into, uint64_t Handle = 0);
  Coro<Result<uint64_t>> storeFrom(std::span<const std::byte> From, const std::string &Path, uint64_t Offset, bool Truncate, uint64_t Handle = 0);
  Coro<Result<uint64_t>> storeFrom(AddressSpace &From, size_t Length, const std::string &Path, uint64_t Offset, bool Truncate, uint64_t Handle = 0);
  Coro<void> close();

  size_t maxTransfer() const;

private:
  struct Impl;

  Coro<Result<void>> sendMeta(const proto::MetaRequest &Meta);
  Coro<Result<uint64_t>>
  submitPosted(const std::string &Path, uint64_t Offset, std::span<std::byte> Into, Page *Landing, size_t Want, uint64_t Handle);
  Coro<Result<uint64_t>> fetchThrough(const std::string &Path, uint64_t Offset, uint64_t Want, PageSink &Landing, uint64_t Handle);
  Coro<Result<uint64_t>>
  storeThrough(const std::string &Path, uint64_t Offset, uint64_t Length, bool Truncate, PageSource &Outgoing, uint64_t Handle);

  explicit FileClient(std::unique_ptr<Impl> P);

  std::unique_ptr<Impl> P;
};

} // namespace rail
