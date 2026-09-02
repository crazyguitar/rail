#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace rail::fuse {

using Ino = uint64_t;

inline constexpr Ino kRootIno = 1;

class Inodes {
public:
  explicit Inodes(std::string Root) : Root(std::move(Root)) {}

  Inodes(const Inodes &) = delete;
  Inodes &operator=(const Inodes &) = delete;

  bool known(Ino I) const { return I == kRootIno || Nodes.contains(I); }
  size_t size() const { return Nodes.size(); }

  Ino insert(Ino Parent, const std::string &Name);
  Ino reserve(Ino Parent, const std::string &Name);
  void release(Ino Parent, const std::string &Name);
  void forget(Ino I, uint64_t Count);
  std::string path(Ino I) const;
  void reparent(Ino Parent, const std::string &Name, Ino NewParent, const std::string &NewName);
  void drop(Ino Parent, const std::string &Name);

  static std::string join(const std::string &Dir, const std::string &Name);

private:
  struct Node {
    Ino Parent = 0;
    std::string Name;
    uint64_t Refs = 0;
    uint64_t Kids = 0;
  };

  using Held = std::unordered_map<Ino, Node>::iterator;

  Ino create(Ino Parent, const std::string &Name, uint64_t Refs);
  void erase(Held It);
  void trim();

  static std::string keyOf(Ino Parent, const std::string &Name) { return std::to_string(Parent) + "/" + Name; }

  std::deque<Ino> Cold;

  std::unordered_map<Ino, Node> Nodes;
  std::unordered_map<std::string, Ino> ByName;
  std::string Root;
  Ino Next = kRootIno + 1;
};

} // namespace rail::fuse
