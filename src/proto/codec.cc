#include "rail/proto/codec.h"

#include <format>

namespace rail::proto {

uint64_t idOf(const Message &M) {
  return std::visit(
      [](const auto &V) -> uint64_t {
        using T = std::decay_t<decltype(V)>;
        if constexpr (requires { V.Id; }) return V.Id;
        else return 0;
      },
      M);
}

Type typeOf(const Message &M) {
  return std::visit(
      [](const auto &V) -> Type {
        using T = std::decay_t<decltype(V)>;
        if constexpr (std::is_same_v<T, Hello>) return Type::Hello;
        else if constexpr (std::is_same_v<T, HelloAck>) return Type::HelloAck;
        else if constexpr (std::is_same_v<T, FileHeader>) return Type::FileHeader;
        else if constexpr (std::is_same_v<T, Signature>) return Type::Signature;
        else if constexpr (std::is_same_v<T, Copy>) return Type::Copy;
        else if constexpr (std::is_same_v<T, Literal>) return Type::Literal;
        else if constexpr (std::is_same_v<T, Done>) return Type::Done;
        else if constexpr (std::is_same_v<T, End>) return Type::End;
        else if constexpr (std::is_same_v<T, Receipt>) return Type::Receipt;
        else if constexpr (std::is_same_v<T, StatRequest>) return Type::Stat;
        else if constexpr (std::is_same_v<T, StatReply>) return Type::StatReply;
        else if constexpr (std::is_same_v<T, ListRequest>) return Type::List;
        else if constexpr (std::is_same_v<T, ListReply>) return Type::ListReply;
        else if constexpr (std::is_same_v<T, ReadRequest>) return Type::Read;
        else if constexpr (std::is_same_v<T, WriteRequest>) return Type::Write;
        else if constexpr (std::is_same_v<T, PeerEndpoint>) return Type::PeerEndpoint;
        else if constexpr (std::is_same_v<T, FetchRequest>) return Type::Fetch;
        else if constexpr (std::is_same_v<T, StoreRequest>) return Type::Store;
        else if constexpr (std::is_same_v<T, StreamReply>) return Type::StreamReply;
        else if constexpr (std::is_same_v<T, StreamDigest>) return Type::StreamDigest;
        else if constexpr (std::is_same_v<T, MetaRequest>) return Type::Meta;
        else if constexpr (std::is_same_v<T, MetaReply>) return Type::MetaReply;
        else if constexpr (std::is_same_v<T, StatFsRequest>) return Type::StatFs;
        else if constexpr (std::is_same_v<T, StatFsReply>) return Type::StatFsReply;
        else if constexpr (std::is_same_v<T, OpenRequest>) return Type::Open;
        else if constexpr (std::is_same_v<T, OpenReply>) return Type::OpenReply;
        else if constexpr (std::is_same_v<T, CloseRequest>) return Type::Close;
        else return Type::TransferReply;
      },
      M);
}

const char *typeName(Type T) {
  switch (T) {
  case Type::Hello:
    return "Hello";
  case Type::HelloAck:
    return "HelloAck";
  case Type::FileHeader:
    return "FileHeader";
  case Type::Signature:
    return "Signature";
  case Type::Copy:
    return "Copy";
  case Type::Literal:
    return "Literal";
  case Type::Done:
    return "Done";
  case Type::End:
    return "End";
  case Type::Receipt:
    return "Receipt";
  case Type::Stat:
    return "Stat";
  case Type::StatReply:
    return "StatReply";
  case Type::List:
    return "List";
  case Type::ListReply:
    return "ListReply";
  case Type::Read:
    return "Read";
  case Type::Write:
    return "Write";
  case Type::TransferReply:
    return "TransferReply";
  case Type::PeerEndpoint:
    return "PeerEndpoint";
  case Type::Fetch:
    return "Fetch";
  case Type::Store:
    return "Store";
  case Type::StreamReply:
    return "StreamReply";
  case Type::StreamDigest:
    return "StreamDigest";
  case Type::Meta:
    return "Meta";
  case Type::MetaReply:
    return "MetaReply";
  case Type::Open:
    return "Open";
  case Type::OpenReply:
    return "OpenReply";
  case Type::Close:
    return "Close";
  case Type::StatFs:
    return "StatFs";
  case Type::StatFsReply:
    return "StatFsReply";
  }
  return "Unknown";
}

void writeAttrs(Writer &W, const FileAttrs &A) {
  W.u64(A.Size);
  W.u32(A.Mode);
  W.i64(A.Mtime);
  W.u8(A.Directory ? 1 : 0);
  W.u8(A.Link ? 1 : 0);
  W.u32(A.Links);
}

FileAttrs readAttrs(Reader &R) {
  FileAttrs A;
  A.Size = R.u64();
  A.Mode = R.u32();
  A.Mtime = R.i64();
  A.Directory = R.u8() != 0;
  A.Link = R.u8() != 0;
  A.Links = R.u32();
  return A;
}

void encode(const Message &M, std::vector<std::byte> &Payload) {
  Writer W(Payload);
  std::visit(
      [&](const auto &V) {
        using T = std::decay_t<decltype(V)>;
        if constexpr (std::is_same_v<T, Hello>) {
          W.u16(V.Version);
          W.str(V.Backend);
          W.u32(V.BlockSize);
          W.u64(V.PageCount);
          W.u64(V.PageSize);
          W.u64(V.WindowPages);
          W.u8(V.Recursive ? 1 : 0);
          W.u8(V.Verify ? 1 : 0);
          W.u8(V.Sum);
        } else if constexpr (std::is_same_v<T, HelloAck>) {
          W.str(V.Backend);
          W.str(V.ChannelEndpoint);
        } else if constexpr (std::is_same_v<T, End>) {
          // no fields
        } else if constexpr (std::is_same_v<T, FileHeader>) {
          W.str(V.Name);
          W.u64(V.Size);
          W.u32(V.Mode);
          W.i64(V.Mtime);
          W.u8(V.Directory ? 1 : 0);
          W.u8(V.WantSignature ? 1 : 0);
          W.u8(V.Streamed ? 1 : 0);
          W.u8(V.Link ? 1 : 0);
          W.str(V.Target);
        } else if constexpr (std::is_same_v<T, Signature>) {
          W.u32(V.BlockLength);
          W.u32(V.StrongLength);
          W.u64(V.FileLength);
          W.u32(static_cast<uint32_t>(V.Sums.size()));
          for (const auto &S : V.Sums) {
            W.u32(S.Weak);
            W.hash(S.Strong);
          }
        } else if constexpr (std::is_same_v<T, Copy>) {
          W.u32(V.BlockIndex);
          W.u32(V.Count);
          W.u64(V.DstOffset);
        } else if constexpr (std::is_same_v<T, Literal>) {
          W.u64(V.Offset);
          W.u32(V.Length);
        } else if constexpr (std::is_same_v<T, Done>) {
          W.hash(V.WholeFileHash);
        } else if constexpr (std::is_same_v<T, Receipt>) {
          W.u64(V.LiteralBytes);
          W.u64(V.MatchedBytes);
          W.u64(V.HashHits);
          W.u64(V.FalseAlarms);
        } else if constexpr (std::is_same_v<T, StatRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
        } else if constexpr (std::is_same_v<T, StatReply>) {
          W.u64(V.Id);
          W.u8(V.Found ? 1 : 0);
          writeAttrs(W, V.Attrs);
        } else if constexpr (std::is_same_v<T, ListRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
        } else if constexpr (std::is_same_v<T, ListReply>) {
          W.u64(V.Id);
          W.u8(V.Found ? 1 : 0);
          W.u32(static_cast<uint32_t>(V.Entries.size()));
          for (const auto &E : V.Entries) {
            W.str(E.Name);
            writeAttrs(W, E.Attrs);
          }
        } else if constexpr (std::is_same_v<T, ReadRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
          W.u64(V.Offset);
          W.u32(V.Length);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, WriteRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
          W.u64(V.Offset);
          W.u32(V.Length);
          W.u8(V.Truncate ? 1 : 0);
          W.hash(V.Payload);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, PeerEndpoint>) {
          W.str(V.Blob);
        } else if constexpr (std::is_same_v<T, FetchRequest>) {
          W.u64(V.Id);
          W.u64(V.TagBase);
          W.str(V.Path);
          W.u64(V.Offset);
          W.u64(V.Length);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, StoreRequest>) {
          W.u64(V.Id);
          W.u64(V.TagBase);
          W.str(V.Path);
          W.u64(V.Offset);
          W.u64(V.Length);
          W.u8(V.Truncate ? 1 : 0);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, StreamReply>) {
          W.u64(V.Id);
          W.u64(V.Length);
          W.u8(V.Ok ? 1 : 0);
          W.str(V.Error);
        } else if constexpr (std::is_same_v<T, StreamDigest>) {
          W.u64(V.Id);
          W.hash(V.Whole);
          W.u8(V.Ok ? 1 : 0);
          W.str(V.Error);
        } else if constexpr (std::is_same_v<T, MetaRequest>) {
          W.u64(V.Id);
          W.u16(static_cast<uint16_t>(V.Op));
          W.str(V.Path);
          W.str(V.Target);
          W.u64(V.Size);
          W.u32(V.Mode);
          W.i64(V.Mtime);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, OpenRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
          W.u8(V.Writable ? 1 : 0);
        } else if constexpr (std::is_same_v<T, OpenReply>) {
          W.u64(V.Id);
          W.u8(V.Ok ? 1 : 0);
          W.u8(V.Found ? 1 : 0);
          W.u64(V.Handle);
          writeAttrs(W, V.Attrs);
          W.str(V.Error);
          W.u32(V.Errno);
        } else if constexpr (std::is_same_v<T, CloseRequest>) {
          W.u64(V.Id);
          W.u64(V.Handle);
        } else if constexpr (std::is_same_v<T, MetaReply>) {
          W.u64(V.Id);
          W.u8(V.Ok ? 1 : 0);
          W.str(V.Error);
          W.str(V.Target);
          W.u32(V.Errno);
        } else if constexpr (std::is_same_v<T, StatFsRequest>) {
          W.u64(V.Id);
          W.str(V.Path);
        } else if constexpr (std::is_same_v<T, StatFsReply>) {
          W.u64(V.Id);
          W.u8(V.Ok ? 1 : 0);
          W.u64(V.BlockSize);
          W.u64(V.Blocks);
          W.u64(V.BlocksFree);
          W.u64(V.Files);
          W.u64(V.FilesFree);
          W.str(V.Error);
        } else {
          W.u64(V.Id);
          W.u32(V.Length);
          W.u64(V.FileSize);
          W.u8(V.Ok ? 1 : 0);
          W.hash(V.Payload);
          W.str(V.Error);
        }
      },
      M);
}

Result<Message> decode(Type T, std::span<const std::byte> Payload) {
  Reader R(Payload);
  Message M;

  switch (T) {
  case Type::Hello: {
    Hello V;
    V.Version = R.u16();
    V.Backend = R.str();
    V.BlockSize = R.u32();
    V.PageCount = R.u64();
    V.PageSize = R.u64();
    V.WindowPages = R.u64();
    V.Recursive = R.u8() != 0;
    V.Verify = R.u8() != 0;
    V.Sum = R.u8();
    M = V;
    break;
  }
  case Type::HelloAck: {
    HelloAck V;
    V.Backend = R.str();
    V.ChannelEndpoint = R.str();
    M = V;
    break;
  }
  case Type::FileHeader: {
    FileHeader V;
    V.Name = R.str();
    V.Size = R.u64();
    V.Mode = R.u32();
    V.Mtime = R.i64();
    V.Directory = R.u8() != 0;
    V.WantSignature = R.u8() != 0;
    V.Streamed = R.u8() != 0;
    V.Link = R.u8() != 0;
    V.Target = R.str();
    M = V;
    break;
  }
  case Type::Signature: {
    Signature V;
    V.BlockLength = R.u32();
    V.StrongLength = R.u32();
    V.FileLength = R.u64();
    const uint32_t N = R.u32();
    if (!R.ok() || static_cast<uint64_t>(N) * 20 > R.remaining() + 20) return failMessage("signature length out of range");
    V.Sums.reserve(N);
    for (uint32_t I = 0; I < N && R.ok(); I++) {
      BlockSum S;
      S.Weak = R.u32();
      S.Strong = R.hash();
      V.Sums.push_back(S);
    }
    M = V;
    break;
  }
  case Type::Copy: {
    Copy V;
    V.BlockIndex = R.u32();
    V.Count = R.u32();
    V.DstOffset = R.u64();
    M = V;
    break;
  }
  case Type::Literal: {
    Literal V;
    V.Offset = R.u64();
    V.Length = R.u32();
    M = V;
    break;
  }
  case Type::End:
    M = End{};
    break;
  case Type::Done: {
    Done V;
    V.WholeFileHash = R.hash();
    M = V;
    break;
  }
  case Type::Receipt: {
    Receipt V;
    V.LiteralBytes = R.u64();
    V.MatchedBytes = R.u64();
    V.HashHits = R.u64();
    V.FalseAlarms = R.u64();
    M = V;
    break;
  }
  case Type::Stat: {
    StatRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    M = V;
    break;
  }
  case Type::StatReply: {
    StatReply V;
    V.Id = R.u64();
    V.Found = R.u8() != 0;
    V.Attrs = readAttrs(R);
    M = V;
    break;
  }
  case Type::List: {
    ListRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    M = V;
    break;
  }
  case Type::ListReply: {
    ListReply V;
    V.Id = R.u64();
    V.Found = R.u8() != 0;
    const uint32_t Count = R.u32();
    V.Entries.reserve(Count);
    for (uint32_t I = 0; I < Count; I++) {
      ListEntry E;
      E.Name = R.str();
      E.Attrs = readAttrs(R);
      V.Entries.push_back(std::move(E));
    }
    M = V;
    break;
  }
  case Type::Read: {
    ReadRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    V.Offset = R.u64();
    V.Length = R.u32();
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::Write: {
    WriteRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    V.Offset = R.u64();
    V.Length = R.u32();
    V.Truncate = R.u8() != 0;
    V.Payload = R.hash();
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::Fetch: {
    FetchRequest V;
    V.Id = R.u64();
    V.TagBase = R.u64();
    V.Path = R.str();
    V.Offset = R.u64();
    V.Length = R.u64();
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::Store: {
    StoreRequest V;
    V.Id = R.u64();
    V.TagBase = R.u64();
    V.Path = R.str();
    V.Offset = R.u64();
    V.Length = R.u64();
    V.Truncate = R.u8() != 0;
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::Open: {
    OpenRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    V.Writable = R.u8() != 0;
    M = V;
    break;
  }
  case Type::OpenReply: {
    OpenReply V;
    V.Id = R.u64();
    V.Ok = R.u8() != 0;
    V.Found = R.u8() != 0;
    V.Handle = R.u64();
    V.Attrs = readAttrs(R);
    V.Error = R.str();
    V.Errno = R.u32();
    M = V;
    break;
  }
  case Type::Close: {
    CloseRequest V;
    V.Id = R.u64();
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::Meta: {
    MetaRequest V;
    V.Id = R.u64();
    V.Op = static_cast<MetaOp>(R.u16());
    V.Path = R.str();
    V.Target = R.str();
    V.Size = R.u64();
    V.Mode = R.u32();
    V.Mtime = R.i64();
    V.Handle = R.u64();
    M = V;
    break;
  }
  case Type::MetaReply: {
    MetaReply V;
    V.Id = R.u64();
    V.Ok = R.u8() != 0;
    V.Error = R.str();
    V.Target = R.str();
    V.Errno = R.u32();
    M = V;
    break;
  }
  case Type::StatFs: {
    StatFsRequest V;
    V.Id = R.u64();
    V.Path = R.str();
    M = V;
    break;
  }
  case Type::StatFsReply: {
    StatFsReply V;
    V.Id = R.u64();
    V.Ok = R.u8() != 0;
    V.BlockSize = R.u64();
    V.Blocks = R.u64();
    V.BlocksFree = R.u64();
    V.Files = R.u64();
    V.FilesFree = R.u64();
    V.Error = R.str();
    M = V;
    break;
  }
  case Type::StreamDigest: {
    StreamDigest V;
    V.Id = R.u64();
    V.Whole = R.hash();
    V.Ok = R.u8() != 0;
    V.Error = R.str();
    M = V;
    break;
  }
  case Type::StreamReply: {
    StreamReply V;
    V.Id = R.u64();
    V.Length = R.u64();
    V.Ok = R.u8() != 0;
    V.Error = R.str();
    M = V;
    break;
  }
  case Type::PeerEndpoint: {
    PeerEndpoint V;
    V.Blob = R.str();
    M = V;
    break;
  }
  case Type::TransferReply: {
    TransferReply V;
    V.Id = R.u64();
    V.Length = R.u32();
    V.FileSize = R.u64();
    V.Ok = R.u8() != 0;
    V.Payload = R.hash();
    V.Error = R.str();
    M = V;
    break;
  }
  default:
    return failMessage("unknown message type");
  }

  if (!R.ok()) return failMessage(std::format("truncated {}", typeName(T)));
  return M;
}

} // namespace rail::proto
