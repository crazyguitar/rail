#include "rail/address-space.h"

#include <algorithm>

namespace rail {

void AddressSpace::claim(Memory &From, size_t Bytes, size_t Count) {
  release();
  if (Bytes == 0 || Count == 0) return;

  PageBytes = Bytes;
  Views.reserve(Count);

  std::vector<Run> Taken;
  Taken.reserve(Count);
  for (size_t I = 0; I < Count; I++) {
    Run One = From.alloc(Bytes);
    if (!One.valid()) {
      Taken.clear();
      break;
    }
    Taken.push_back(std::move(One));
  }

  if (Taken.size() == Count) {
    Held = std::move(Taken);
    for (const Run &One : Held) Views.push_back(One.borrow(0, Bytes));
    return;
  }

  Heap.assign(Bytes * Count, std::byte{0});
  for (size_t I = 0; I < Count; I++) Views.push_back(Page(Heap.data() + I * Bytes, Bytes, nullptr));
}

void AddressSpace::release() {
  Views.clear();
  Held.clear();
  Heap.clear();
  Heap.shrink_to_fit();
  PageBytes = 0;
}

AddressSpace::Mapping AddressSpace::at(uint64_t Offset, size_t Length) {
  if (Views.empty() || Offset < Base) return {};

  const uint64_t Into = Offset - Base;
  const size_t Index = static_cast<size_t>(Into / PageBytes);
  if (Index >= Views.size()) return {};

  const size_t Within = static_cast<size_t>(Into % PageBytes);
  return Mapping{&Views[Index], Within, std::min(Length, PageBytes - Within)};
}

} // namespace rail
