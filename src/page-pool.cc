#include "rail/page-pool.h"
#include "rail/io/loop.h"

#include <cstdio>

namespace rail {

PagePool::PagePool(size_t PageCount, size_t PageSize) : PageCount(PageCount), PageSize(PageSize) {
  Slots.reserve(PageCount);
  for (size_t I = 0; I < PageCount; I++) {
    Run One = Memory::get().alloc(PageSize);
    if (!One.valid()) {
      std::fprintf(stderr, "rail: no registered memory for a page pool of %zu x %zu bytes\n", PageCount, PageSize);
      Slots.clear();
      return;
    }
    Slots.push_back(std::move(One));
  }

  FreeList.reserve(PageCount);
  for (size_t I = PageCount; I > 0; I--) FreeList.push_back(I - 1);
}

PagePool::~PagePool() {
  // A page still out means someone holds one from this pool while its run
  // goes back to be handed to the next one. Saying so is the difference
  // between a bug report and a transfer quietly writing into another pool's
  // memory.
  if (backed() && FreeList.size() != PageCount)
    std::fprintf(stderr, "rail: page pool destroyed with %zu of %zu pages still out\n", PageCount - FreeList.size(), PageCount);
}

void PagePool::handBack(void *Owner, size_t Index) { static_cast<PagePool *>(Owner)->releaseIndex(Index); }

Coro<Page> PagePool::acquire() {
  if (!backed()) co_return Page{};

  while (FreeList.empty()) co_await PageAwaiter{this};

  const size_t Index = FreeList.back();
  FreeList.pop_back();
  co_return Page(Slots[Index].bytes(), PageSize, Slots[Index].region(), &handBack, this, Index);
}

void PagePool::releaseIndex(size_t Index) {
  FreeList.push_back(Index);
  if (!Waiters.empty()) {
    auto H = Waiters.front();
    Waiters.pop_front();
    Loop::get().schedule(H);
  }
}

} // namespace rail
