#include "rail/io/trace.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace rail {

namespace {

// Per thread, because the daemon serves on several and one shared vector would
// be a data race on every add. Each thread dumps its own stages, so a trace of
// a multi-threaded run prints one block per thread.
std::vector<std::pair<const char *, std::chrono::nanoseconds>> &stages() {
  thread_local std::vector<std::pair<const char *, std::chrono::nanoseconds>> Stages;
  return Stages;
}

} // namespace

bool Trace::on() {
  static const bool On = ::getenv("RAIL_TRACE") != nullptr;
  return On;
}

void Trace::add(const char *Name, std::chrono::nanoseconds Elapsed) {
  for (auto &Stage : stages())
    if (Stage.first == Name) {
      Stage.second += Elapsed;
      return;
    }
  stages().emplace_back(Name, Elapsed);
}

void Trace::dump(const char *Who) {
  if (!on()) return;
  for (const auto &Stage : stages())
    std::fprintf(stderr, "trace %s %-12s %8.1f ms\n", Who, Stage.first, std::chrono::duration<double, std::milli>(Stage.second).count());
  stages().clear();
}

} // namespace rail
