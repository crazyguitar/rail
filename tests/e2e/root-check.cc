#include "privileged.h"

#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>

namespace rail::e2e {
namespace {

// The privileged suites mount, load a module and read the kernel log. Asking
// for them without the privilege to run them is a mistake worth naming: a
// suite that skipped instead would read exactly like one that passed.
class RequireRoot : public ::testing::Environment {
public:
  void SetUp() override {
    if (!::getenv("RAIL_KERNEL_TESTS")) return;
    if (runningAsRoot()) return;

    // Exits rather than fails: a failure here is reported as a skip, which
    // reads like a pass to anyone looking at the summary.
    std::fputs("rail-e2e: RAIL_KERNEL_TESTS is set, but this is not running as root.\n"
               "          These suites mount, load a module and read the kernel log.\n"
               "          Run the whole binary under sudo rather than each command:\n"
               "            sudo -E RAIL_KERNEL_TESTS=1 RAIL_PEER=<peer> ./build/tests/e2e/rail-e2e\n",
               stderr);
    std::exit(2);
  }
};

const auto *Registered = ::testing::AddGlobalTestEnvironment(new RequireRoot);

} // namespace
} // namespace rail::e2e
