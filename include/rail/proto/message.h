#pragma once

#include "rail/app/checksum.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rail::proto {

inline constexpr uint32_t kMagic = 0x5241494c; // "RAIL"
// A peer built before a change misreads the frame rather than reporting a
// mismatch, so this moves with the wire, every time.
inline constexpr uint16_t kVersion = 1;
inline constexpr uint32_t kMaxFrame = 64u << 20;

enum class Type : uint16_t {
  Hello = 1,
  HelloAck = 2,
  FileHeader = 3,
  Signature = 4,
  Copy = 5,
  Literal = 6,
  Done = 8,
  Receipt = 9,
  End = 11,
  Stat = 12,
  StatReply = 13,
  List = 14,
  ListReply = 15,
  Read = 16,
  Write = 17,
  TransferReply = 18,
  PeerEndpoint = 19,
  Fetch = 20,
  Store = 21,
  StreamReply = 22,
  StreamDigest = 23,
  Meta = 24,
  MetaReply = 25,
  StatFs = 26,
  StatFsReply = 27,
  Open = 28,
  OpenReply = 29,
  Close = 30,
};

struct BlockSum {
  uint32_t Weak = 0;
  std::array<std::byte, 16> Strong{};
};

struct Hello {
  uint16_t Version = kVersion;
  std::string Backend;
  uint32_t BlockSize = 0;
  // The receiver builds its pool from these. A one-sided RDMA write lands at
  // an address derived from page geometry, so the two sides must match.
  uint64_t PageCount = 0;
  uint64_t PageSize = 0;
  // The receiver keeps as many receives posted as the sender has sends in
  // flight. Tagged sends that have no matching receive simply wait, so a
  // sender window alone buys nothing.
  uint64_t WindowPages = 0;
  // A recursive transfer carries relative paths and resolves every one under
  // the destination, rather than treating the destination as a single file.
  bool Recursive = false;
  bool Verify = true;
  // Which hash the session verifies with. A client that cannot compute the
  // default says so here rather than turning verification off altogether.
  // Declared in wire order: the codec writes Recursive, Verify, then Sum.
  uint8_t Sum = static_cast<uint8_t>(rail::Sum::XxH3);
};

struct HelloAck {
  std::string Backend;
  std::string ChannelEndpoint;
};

struct FileHeader {
  // Path relative to the destination root. Never absolute and never containing
  // a parent reference: the receiver rejects both rather than let a peer name
  // a path outside the directory it was given.
  std::string Name;
  uint64_t Size = 0;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
  // A directory entry carries no payload; it exists so empty directories and
  // their modes survive the transfer.
  bool Directory = false;
  // Nor does a symbolic link: what it points at is the whole of it, and that
  // target is copied as written rather than resolved, since it means whatever
  // it means on the machine that ends up holding it.
  bool Link = false;
  std::string Target;
  // False when the sender has already decided against a delta. Building a
  // signature costs a full read of the destination, and the sender would only
  // throw it away.
  bool WantSignature = true;
  // The payload is the whole file in page order, with no instruction per page.
  // Each page is keyed by its offset, so the receiver derives the layout from
  // Size and the agreed page size and never waits on the control channel,
  // which crosses a slow link and costs a round trip per page.
  bool Streamed = false;
};

struct Signature {
  uint32_t BlockLength = 0;
  uint32_t StrongLength = 0;
  uint64_t FileLength = 0;
  std::vector<BlockSum> Sums;
};

struct Copy {
  uint32_t BlockIndex = 0;
  uint32_t Count = 1;
  uint64_t DstOffset = 0;
};

struct Literal {
  uint64_t Offset = 0;
  uint32_t Length = 0;
};

struct Done {
  std::array<std::byte, 16> WholeFileHash{};
};

struct Receipt {
  uint64_t LiteralBytes = 0;
  uint64_t MatchedBytes = 0;
  uint64_t HashHits = 0;
  uint64_t FalseAlarms = 0;
};

// Closes the file stream. Without it the receiver cannot tell the last file
// from a peer that died between files.
struct End {};

struct FileAttrs {
  uint64_t Size = 0;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
  bool Directory = false;
  // Told apart from what it points at. The daemon stats without following, so
  // a link to a directory is a link here and a dangling one still exists -
  // following it is the client's business, and its target may well name a path
  // that only means something there.
  bool Link = false;
  uint32_t Links = 1;
};

struct StatRequest {
  uint64_t Id = 0;
  std::string Path;
};

struct StatReply {
  uint64_t Id = 0;
  bool Found = false;
  FileAttrs Attrs;
};

struct ListRequest {
  uint64_t Id = 0;
  std::string Path;
};

struct ListEntry {
  std::string Name;
  FileAttrs Attrs;
};

struct ListReply {
  uint64_t Id = 0;
  bool Found = false;
  std::vector<ListEntry> Entries;
};

struct OpenRequest {
  uint64_t Id = 0;
  std::string Path;
  bool Writable = false;
};

struct OpenReply {
  uint64_t Id = 0;
  bool Ok = false;
  bool Found = false;
  uint64_t Handle = 0;
  FileAttrs Attrs;
  std::string Error;
  // What the mount should report to its caller. Without it every refusal
  // arrives as a sentence and reaches the program that asked as EIO, so a file
  // it simply lacks permission for looks like failing hardware.
  uint32_t Errno = 0;
};

struct CloseRequest {
  uint64_t Id = 0;
  uint64_t Handle = 0;
};

struct ReadRequest {
  uint64_t Id = 0;
  std::string Path;
  uint64_t Offset = 0;
  uint32_t Length = 0;
  uint64_t Handle = 0;
};

struct WriteRequest {
  uint64_t Id = 0;
  std::string Path;
  uint64_t Offset = 0;
  uint32_t Length = 0;
  bool Truncate = false;
  std::array<std::byte, 16> Payload{};
  uint64_t Handle = 0;
};

struct FetchRequest {
  uint64_t Id = 0;
  uint64_t TagBase = 0;
  std::string Path;
  uint64_t Offset = 0;
  uint64_t Length = 0;
  uint64_t Handle = 0;
};

struct StoreRequest {
  uint64_t Id = 0;
  uint64_t TagBase = 0;
  std::string Path;
  uint64_t Offset = 0;
  uint64_t Length = 0;
  bool Truncate = false;
  uint64_t Handle = 0;
};

struct StreamReply {
  uint64_t Id = 0;
  uint64_t Length = 0;
  bool Ok = false;
  std::string Error;
};

enum class MetaOp : uint16_t {
  MakeDirectory = 1,
  RemoveFile = 2,
  RemoveDirectory = 3,
  Rename = 4,
  Truncate = 5,
  SetMode = 6,
  SetMtime = 7,
  Fsync = 8,
  Symlink = 9,
  ReadLink = 10,
  HardLink = 11,
};

struct MetaRequest {
  uint64_t Id = 0;
  MetaOp Op = MetaOp::MakeDirectory;
  std::string Path;
  std::string Target;
  uint64_t Size = 0;
  uint32_t Mode = 0;
  int64_t Mtime = 0;
  uint64_t Handle = 0;
};

struct MetaReply {
  uint64_t Id = 0;
  bool Ok = false;
  std::string Error;
  // What a link points at, for ReadLink. Empty for every other operation.
  std::string Target;
  uint32_t Errno = 0;
};

struct StatFsRequest {
  uint64_t Id = 0;
  std::string Path;
};

struct StatFsReply {
  uint64_t Id = 0;
  bool Ok = false;
  uint64_t BlockSize = 0;
  uint64_t Blocks = 0;
  uint64_t BlocksFree = 0;
  uint64_t Files = 0;
  uint64_t FilesFree = 0;
  std::string Error;
};

struct StreamDigest {
  uint64_t Id = 0;
  std::array<std::byte, 16> Whole{};
  bool Ok = false;
  std::string Error;
};

struct PeerEndpoint {
  std::string Blob;
};

struct TransferReply {
  uint64_t Id = 0;
  uint32_t Length = 0;
  uint64_t FileSize = 0;
  bool Ok = false;
  std::array<std::byte, 16> Payload{};
  std::string Error;
};

using Message = std::variant<Hello,
                             HelloAck,
                             FileHeader,
                             Signature,
                             Copy,
                             Literal,
                             Done,
                             Receipt,
                             End,
                             StatRequest,
                             StatReply,
                             ListRequest,
                             ListReply,
                             ReadRequest,
                             WriteRequest,
                             TransferReply,
                             PeerEndpoint,
                             FetchRequest,
                             StoreRequest,
                             StreamReply,
                             StreamDigest,
                             MetaRequest,
                             MetaReply,
                             StatFsRequest,
                             StatFsReply,
                             OpenRequest,
                             OpenReply,
                             CloseRequest>;

inline uint64_t tagSpan(uint64_t Size, uint64_t PageSize) { return ((Size + PageSize - 1) / PageSize + 1) * PageSize; }

uint64_t idOf(const Message &M);

Type typeOf(const Message &M);
const char *typeName(Type T);

} // namespace rail::proto
