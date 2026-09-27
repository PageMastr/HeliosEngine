// Regression probe for test lambdas merged across files on MSVC (see tu_isolation.h). Keep this file
// the same as test_tu_isolation_b.cpp line for line down to the end of the probe test.
#include <doctest/doctest.h>

#include "tu_isolation.h"

namespace {

constexpr int kProbeCounter = __COUNTER__;
constexpr int kProbeLine = __LINE__ + 1;
TEST_CASE("tu isolation: file A's probe runs file A's lambdas") {
    CHECK(helios::tu_isolation::runKeyed([] { return 'A'; }) == 'A');
    char ran = 0;
    helios::tu_isolation::runAsJob([&ran] { ran = 'A'; });
    CHECK(ran == 'A');
}

} // namespace

helios::tu_isolation::Site helios::tu_isolation::siteA() {
    return {kProbeCounter, kProbeLine};
}
