#include "rail/fuse/inodes.h"

#include <vector>

namespace rail::fuse {

std::string Inodes::join(const std::string &Dir, const std::string &Name) {
  if (Dir.empty() || Dir == ".") return Name;
  return Dir + "/" + Name;
}

namespace {

constexpr size_t kListedLimit = 65536;

} // namespace

Ino Inodes::create(Ino Parent, const std::string &Name, uint64_t Refs) {
  const Ino Fresh = Next++;
  Nodes.emplace(Fresh, Node{Parent, Name, Refs, 0});
  ByName.emplace(keyOf(Parent, Name), Fresh);
  if (auto Up = Nodes.find(Parent); Up != Nodes.end()) Up->second.Kids++;
  return Fresh;
}

void Inodes::erase(Held It) {
  if (auto Up = Nodes.find(It->second.Parent); Up != Nodes.end()) Up->second.Kids--;
  ByName.erase(keyOf(It->second.Parent, It->second.Name));
  Nodes.erase(It);
}

void Inodes::trim() {
  while (Cold.size() > kListedLimit) {
    const Ino Oldest = Cold.front();
    Cold.pop_front();

    auto It = Nodes.find(Oldest);
    if (It == Nodes.end()) continue;
    if (It->second.Refs > 0 || It->second.Kids > 0) continue;
    erase(It);
  }
}

Ino Inodes::insert(Ino Parent, const std::string &Name) {
  const std::string Key = keyOf(Parent, Name);
  if (auto It = ByName.find(Key); It != ByName.end()) {
    Nodes[It->second].Refs++;
    return It->second;
  }

  return create(Parent, Name, 1);
}

Ino Inodes::reserve(Ino Parent, const std::string &Name) {
  const std::string Key = keyOf(Parent, Name);
  if (auto It = ByName.find(Key); It != ByName.end()) return It->second;

  const Ino Fresh = create(Parent, Name, 0);
  Cold.push_back(Fresh);
  trim();
  return Fresh;
}

void Inodes::release(Ino Parent, const std::string &Name) {
  auto It = ByName.find(keyOf(Parent, Name));
  if (It == ByName.end()) return;

  auto Node = Nodes.find(It->second);
  if (Node == Nodes.end() || Node->second.Refs > 0) return;
  erase(Node);
}

void Inodes::forget(Ino I, uint64_t Count) {
  auto It = Nodes.find(I);
  if (It == Nodes.end()) return;
  if (It->second.Refs > Count) {
    It->second.Refs -= Count;
    return;
  }
  erase(It);
}

std::string Inodes::path(Ino I) const {
  std::vector<const std::string *> Parts;
  for (Ino At = I; At != kRootIno;) {
    auto It = Nodes.find(At);
    if (It == Nodes.end()) return {};
    Parts.push_back(&It->second.Name);
    At = It->second.Parent;
  }

  std::string Out = Root;
  for (size_t N = Parts.size(); N-- > 0;) Out = join(Out, *Parts[N]);
  return Out;
}

void Inodes::reparent(Ino Parent, const std::string &Name, Ino NewParent, const std::string &NewName) {
  auto It = ByName.find(keyOf(Parent, Name));
  if (It == ByName.end()) return;

  const Ino Moving = It->second;
  ByName.erase(It);

  if (auto Clash = ByName.find(keyOf(NewParent, NewName)); Clash != ByName.end()) {
    if (auto Gone = Nodes.find(Clash->second); Gone != Nodes.end()) {
      if (auto Up = Nodes.find(Gone->second.Parent); Up != Nodes.end()) Up->second.Kids--;
      Nodes.erase(Gone);
    }
    ByName.erase(Clash);
  }

  if (auto Up = Nodes.find(Nodes[Moving].Parent); Up != Nodes.end()) Up->second.Kids--;
  Nodes[Moving].Parent = NewParent;
  Nodes[Moving].Name = NewName;
  if (auto Up = Nodes.find(NewParent); Up != Nodes.end()) Up->second.Kids++;
  ByName.emplace(keyOf(NewParent, NewName), Moving);
}

void Inodes::drop(Ino Parent, const std::string &Name) {
  auto It = ByName.find(keyOf(Parent, Name));
  if (It == ByName.end()) return;
  if (auto Node = Nodes.find(It->second); Node != Nodes.end()) erase(Node);
  else ByName.erase(It);
}

} // namespace rail::fuse
