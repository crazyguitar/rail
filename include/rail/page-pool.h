#pragma once

#include "rail/io/coro.h"
#include "rail/memory.h"

#include <cstddef>
#include <deque>
#include <vector>

namespace rail {

// Fixed footprint, taken page by page from shared registered memory so no
// pool ever asks for more contiguous bytes than one page. acquire() suspends
// when the pool is empty, which is what bounds memory under backpressure.
class PagePool {
public:
  PagePool(size_t PageCount, size_t PageSize);
  PagePool(const PagePool &) = delete;
  PagePool &operator=(const PagePool &) = delete;
  virtual ~PagePool();

  Coro<Page> acquire();

  size_t pageSize() const { return PageSize; }

  size_t pageCount() const { return PageCount; }
  size_t footprint() const { return PageCount * PageSize; }
  bool backed() const { return Slots.size() == PageCount; }

private:
  static void handBack(void *Owner, size_t Index);

  struct PageAwaiter {
    PagePool *Pool;
    std::coroutine_handle<> Queued{};

    bool await_ready() const noexcept { return !Pool->FreeList.empty(); }
    void await_suspend(std::coroutine_handle<> H) {
      Queued = H;
      Pool->Waiters.push_back(H);
    }
    void await_resume() noexcept { Queued = {}; }

    // A coroutine abandoned while it waits for a page takes its handle with
    // it, rather than leaving one for the next release to resume into.
    ~PageAwaiter() {
      if (Queued) std::erase(Pool->Waiters, Queued);
    }
  };

  void releaseIndex(size_t Index);

  std::vector<Run> Slots;
  size_t PageCount = 0;
  size_t PageSize = 0;
  std::vector<size_t> FreeList;
  std::deque<std::coroutine_handle<>> Waiters;
};

} // namespace rail
