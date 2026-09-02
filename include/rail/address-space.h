#pragma once

#include "rail/memory.h"

#include <cstdint>
#include <vector>

namespace rail {

class AddressSpace {
public:
  struct Mapping {
    Page *Where = nullptr;
    size_t Offset = 0;
    size_t Length = 0;
  };

  AddressSpace() = default;
  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;
  AddressSpace(AddressSpace &&) = default;
  AddressSpace &operator=(AddressSpace &&) = default;

  // Always succeeds: registered pages when the memory is there, heap when it
  // is not, and only registered pages are ever offered to the fabric.
  void claim(Memory &From, size_t PageBytes, size_t Count);
  void release();

  void rebase(uint64_t Start) { Base = Start; }
  uint64_t base() const { return Base; }

  bool backed() const { return !Views.empty(); }
  bool registered() const { return Heap.empty() && !Views.empty(); }
  size_t pageSize() const { return PageBytes; }
  size_t capacity() const { return Views.size() * PageBytes; }

  // Never spans two pages: the caller gets what is left of the one the offset
  // falls in, which is what keeps an rdma transfer inside a single page.
  Mapping at(uint64_t Offset, size_t Length);

private:
  std::vector<Run> Held;
  std::vector<std::byte> Heap;
  std::vector<Page> Views;
  size_t PageBytes = 0;
  uint64_t Base = 0;
};

} // namespace rail
