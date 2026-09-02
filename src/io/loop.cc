#include "rail/io/loop.h"

#include <algorithm>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

namespace rail {

namespace {

// With a transport in hand the loop never sleeps long: an event it is waiting
// for may only appear once the transport is asked.
constexpr int kDrivenPollMs = 1;

// How many idle iterations before the loop stops polling a transport hard. The
// armed descriptor still wakes it for real events; this only bounds how long a
// transport can sit on work its own event never announces.
constexpr int kQuietRounds = 256;

} // namespace

namespace {
constexpr int kIdleTimeoutMs = 50;
} // namespace

namespace {
thread_local bool LoopLive = false;
}

Loop &Loop::get() {
  thread_local Loop L;
  return L;
}

void forgetCoroutine(std::coroutine_handle<> H) noexcept {
  if (LoopLive) Loop::get().cancel(H);
}

Loop::Loop() : EpollFd(::epoll_create1(EPOLL_CLOEXEC)) { LoopLive = true; }

Loop::~Loop() {
  LoopLive = false;
  if (EpollFd >= 0) ::close(EpollFd);
}

void Loop::schedule(std::coroutine_handle<> H) {
  if (H && !H.done()) Ready.push_back({H, -1});
}

void Loop::wait(int Fd, uint32_t Events, std::coroutine_handle<> H) {
  auto &OnFd = Waiters[Fd];
  OnFd.push_back({H, Events});
  if (Shared.contains(Fd)) return;

  uint32_t Wanted = 0;
  for (const auto &W : OnFd) Wanted |= W.Events;

  // Level triggered, and deliberately not EPOLLONESHOT. Oneshot disables the
  // descriptor as it fires, and re-arming it raced with the wakeup: a
  // completion channel that already held an event was reported to nobody and
  // the waiter slept until its timer. Level triggered cannot lose one, and
  // step() takes an idle descriptor back out so it cannot spin either.
  epoll_event Ev{};
  Ev.events = Wanted;
  Ev.data.fd = Fd;
  if (::epoll_ctl(EpollFd, EPOLL_CTL_MOD, Fd, &Ev) == 0) return;
  if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, Fd, &Ev) == 0) {
    Registered++;
    return;
  }

  // Neither call armed it, so nothing will ever report this descriptor and the
  // waiter would sleep on a wakeup that cannot come. Resuming it now turns that
  // into a retry the caller can act on.
  std::erase_if(OnFd, [H](const Parked &W) { return W.H == H; });
  if (OnFd.empty()) Waiters.erase(Fd);
  schedule(H);
}

void Loop::waitFor(int Fd, uint32_t Events, int TimeoutMs, std::coroutine_handle<> H) {
  wait(Fd, Events, H);
  Timed.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs), Fd, H});
}

void Loop::cancel(std::coroutine_handle<> H) {
  std::erase_if(Ready, [H](const Wake &W) { return W.H == H; });
  std::erase_if(Timed, [H](const Timer &T) { return T.H == H; });

  for (auto It = Waiters.begin(); It != Waiters.end();) {
    std::erase_if(It->second, [H](const Parked &W) { return W.H == H; });
    if (It->second.empty()) It = Waiters.erase(It);
    else ++It;
  }
}

void Loop::wake(int Fd) {
  auto It = Waiters.find(Fd);
  if (It == Waiters.end()) return;

  for (const auto &W : It->second) {
    std::erase_if(Timed, [&W](const Timer &T) { return T.H == W.H; });
    if (W.H && !W.H.done()) Ready.push_back({W.H, Fd});
  }
  Waiters.erase(It);
}

void Loop::share(int Fd, uint32_t Events) {
  epoll_event Ev{};
  Ev.events = Events | EPOLLEXCLUSIVE;
  Ev.data.fd = Fd;
  if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, Fd, &Ev) != 0) return;

  Registered++;
  Shared.insert(Fd);
}

void Loop::forget(int Fd) {
  // Whoever is parked here is waiting for an event that can no longer arrive.
  // Dropping them strands the coroutine and everything that joins it, so resume
  // them instead and let the closed descriptor become an error they can report.
  wake(Fd);
  Shared.erase(Fd);
  std::erase_if(Timed, [Fd](const Timer &T) { return T.Fd == Fd; });
  if (::epoll_ctl(EpollFd, EPOLL_CTL_DEL, Fd, nullptr) == 0) Registered--;
}

bool Loop::hasWork() const { return !Ready.empty() || !Timed.empty() || Registered > 0; }

Loop::Driver::Driver(Driver &&Other) noexcept : Id(std::exchange(Other.Id, 0)) {}

Loop::Driver &Loop::Driver::operator=(Driver &&Other) noexcept {
  if (this != &Other) {
    stop();
    Id = std::exchange(Other.Id, 0);
  }
  return *this;
}

Loop::Driver::~Driver() { stop(); }

void Loop::Driver::stop() {
  if (const uint64_t Ending = std::exchange(Id, 0)) Loop::get().undrive(Ending);
}

Loop::Driver Loop::drive(ProgressFn Fn) {
  const uint64_t Id = NextDriverId++;
  Driven.emplace_back(Id, std::move(Fn));
  return Driver(Id);
}

void Loop::undrive(uint64_t Id) {
  std::erase_if(Driven, [Id](const auto &Entry) { return Entry.first == Id; });
}

bool Loop::step(std::coroutine_handle<> Until) {
  // Drive registered transports first: a completion found here can make a
  // coroutine runnable in this same iteration.
  // Indexed rather than a range-for: a transport that fails inside its own
  // progress call may end its registration, which would erase from under an
  // iterator.
  bool Advanced = false;
  for (size_t I = 0; I < Driven.size(); I++)
    if (Driven[I].second()) Advanced = true;

  // Resume everything already runnable. Snapshot the count so coroutines
  // scheduled by this batch run on the next iteration rather than starving
  // the epoll wait.
  const bool Woke = !Ready.empty();
  for (size_t I = 0, N = Ready.size(); I < N && !Ready.empty(); I++) {
    auto W = Ready.front();
    Ready.pop_front();
    if (W.H && !W.H.done()) W.H.resume();
  }

  if (Until && Until.done()) return true;
  if (!hasWork()) return false;

  // Resume anything whose timer expired, so a silent fd cannot pin a coroutine.
  const auto Now = std::chrono::steady_clock::now();
  for (size_t I = 0; I < Timed.size();) {
    if (Timed[I].When <= Now) {
      auto Entry = Timed[I];
      Timed.erase(Timed.begin() + static_cast<long>(I));

      // Drop just this waiter; others on the same fd keep waiting.
      if (auto It = Waiters.find(Entry.Fd); It != Waiters.end()) {
        std::erase_if(It->second, [&Entry](const Parked &W) { return W.H == Entry.H; });
        if (It->second.empty()) Waiters.erase(It);
      }
      if (Entry.H && !Entry.H.done()) Ready.push_back({Entry.H, Entry.Fd});
      continue;
    }
    I++;
  }

  // Blocking while a transport still has work would stall it until a timer,
  // which is the whole reason progress is driven from here. Once it goes quiet
  // the loop stops polling hard, so an idle server does not sit at a thousand
  // wakeups a second waiting for nothing.
  //
  // Quiet counts steps, so a loop busy with anything else crosses the limit in
  // milliseconds and stops polling the transport while a transfer is still in
  // flight. Any work at all counts as not quiet.
  Quiet = (Advanced || Woke) ? 0 : Quiet + 1;

  int Timeout = (!Ready.empty() || Advanced) ? 0 : (Timed.empty() ? kIdleTimeoutMs : 10);
  if (!Driven.empty() && Quiet < kQuietRounds && Timeout > kDrivenPollMs) Timeout = kDrivenPollMs;
  epoll_event Events[64];
  const int N = ::epoll_wait(EpollFd, Events, 64, Timeout);
  // Wake every coroutine parked on this descriptor, not just the newest.
  for (int I = 0; I < N; I++) {
    const int Fd = Events[I].data.fd;
    auto It = Waiters.find(Fd);
    if (It == Waiters.end()) {
      // Nobody is parked here any more. A level triggered descriptor that stays
      // readable would be reported on every turn, so it comes out until someone
      // waits on it again. Shared ones are registered for their whole life.
      if (!Shared.contains(Fd) && ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, Fd, nullptr) == 0) Registered--;
      continue;
    }

    for (const auto &W : It->second) {
      std::erase_if(Timed, [&W](const Timer &T) { return T.H == W.H; });
      if (W.H && !W.H.done()) Ready.push_back({W.H, Fd});
    }
    Waiters.erase(It);
  }
  return true;
}

void Loop::runUntil(std::coroutine_handle<> H) {
  while (H && !H.done()) {
    if (!step(H)) break;
  }
}

} // namespace rail
