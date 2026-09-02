// SPDX-License-Identifier: GPL-2.0
//
// The -o option string, spelled the way the kernel mount spells it.

#include "options.h"

#include "rail/fs/safe-path.h"

#include <cstdlib>
#include <string>

namespace rail::fuse {
namespace {

// Bytes on the wire like the kernel's, not the mebibytes the long flags take:
// the point of the shared spelling is that one string works on either mount.
uint64_t bytes(const std::string &Value) { return std::strtoull(Value.c_str(), nullptr, 10); }

Result<void> one(const std::string &Key, const std::string &Value, bool HasValue, MountOptions &Opts) {
  if (Key == "rdma") {
    Opts.Remote.Service.Backend = "rdma";
  } else if (Key == "tcp") {
    Opts.Remote.Service.Backend = "tcp";
  } else if (Key == "noverify") {
    Opts.Remote.Service.Verify = false;
  } else if (Key == "buffered") {
    Opts.DirectIo = false;
  } else if (Key == "consistent") {
    Opts.AttrTimeout = Opts.EntryTimeout = 0;
  } else if (!HasValue) {
    // Left for libfuse, which owns ro, rw, allow_other and the rest of them.
    Opts.Extra.emplace_back(Key);
  } else if (Key == "host") {
    Opts.Remote.Host = Value;
  } else if (Key == "export") {
    auto Root = exportRoot(Value);
    if (!Root) return std::unexpected(Root.error());

    Opts.Remote.Root = *Root;
  } else if (Key == "port") {
    Opts.Remote.Service.Port = static_cast<uint16_t>(bytes(Value));
  } else if (Key == "conns") {
    Opts.Remote.Sessions = static_cast<size_t>(bytes(Value));
  } else if (Key == "readahead") {
    Opts.Remote.Readahead = bytes(Value);
  } else if (Key == "fetch") {
    Opts.Remote.Service.PageSize = bytes(Value);
  } else if (Key == "actimeo") {
    Opts.AttrTimeout = Opts.EntryTimeout = static_cast<double>(bytes(Value));
  } else if (Key == "threads") {
    Opts.Threads = static_cast<size_t>(bytes(Value));
  } else if (Key == "stream_after") {
    Opts.StreamAfter = bytes(Value);
  } else if (Key == "stream_chunk") {
    Opts.StreamChunk = bytes(Value);
  } else if (Key == "write_chunk") {
    Opts.WriteChunk = bytes(Value);
  } else if (Key == "max_read") {
    Opts.MaxRead = static_cast<uint32_t>(bytes(Value));
  } else {
    Opts.Extra.emplace_back(Key + "=" + Value);
  }

  return Result<void>{};
}

} // namespace

Result<void> applyMountOptions(const std::string &Options, MountOptions &Opts) {
  size_t At = 0;

  while (At <= Options.size()) {
    const size_t Comma = Options.find(',', At);
    const std::string Item = Options.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At);

    if (!Item.empty()) {
      const size_t Equals = Item.find('=');
      const bool HasValue = Equals != std::string::npos;

      if (auto R = one(HasValue ? Item.substr(0, Equals) : Item, HasValue ? Item.substr(Equals + 1) : std::string(), HasValue, Opts); !R) {
        return R;
      }
    }

    if (Comma == std::string::npos) break;
    At = Comma + 1;
  }

  return Result<void>{};
}

Result<void> applySpec(const std::string &Spec, MountOptions &Opts) {
  const auto Cut = Spec.find(':');

  Opts.Remote.Host = Cut == std::string::npos ? Spec : Spec.substr(0, Cut);
  if (Cut == std::string::npos) {
    return {};
  }

  auto Root = exportRoot(Spec.substr(Cut + 1));
  if (!Root) return std::unexpected(Root.error());

  Opts.Remote.Root = *Root;
  return {};
}

} // namespace rail::fuse
