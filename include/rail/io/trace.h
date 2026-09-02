#pragma once

#include <chrono>

namespace rail {

// Stage wall-clock, off unless RAIL_TRACE is set. Telling a transport limit
// from a limit in our own path is guesswork without it.
class Trace {
public:
  static bool on();
  static void add(const char *Name, std::chrono::nanoseconds Elapsed);
  static void dump(const char *Who);
};

// Times its scope into a named stage.
class Scoped {
public:
  explicit Scoped(const char *Name) : Name(Name), Start(Trace::on() ? Clock::now() : Clock::time_point{}) {}

  ~Scoped() {
    if (Trace::on()) Trace::add(Name, Clock::now() - Start);
  }

  Scoped(const Scoped &) = delete;
  Scoped &operator=(const Scoped &) = delete;

private:
  using Clock = std::chrono::steady_clock;

  const char *Name;
  Clock::time_point Start;
};

} // namespace rail
