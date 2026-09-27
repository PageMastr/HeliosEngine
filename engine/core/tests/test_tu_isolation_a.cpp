// TEMPORARY experiment variant (not in an unnamed namespace on purpose). Keep this file the same as
// test_tu_isolation_b.cpp line for line down to the end of the probe test.
#include <doctest/doctest.h>

#include "tu_isolation.h"

constexpr int kProbeCounter = __COUNTER__;
constexpr int kProbeLine = __LINE__ + 1;
TEST_CASE("tu isolation: file A's probe runs file A's lambda") {
    CHECK(helios::tu_isolation::runKeyedMember([] { return 'A'; }) == 'A');
    CHECK(helios::tu_isolation::runKeyed([] { return 'A'; }) == 'A');
    char ran = 0;
    helios::tu_isolation::runJob([&ran] { ran = 'A'; });
    CHECK(ran == 'A');
}

helios::tu_isolation::Site helios::tu_isolation::siteA() {
    return {kProbeCounter, kProbeLine};
}
