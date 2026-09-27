// Regression probe for test lambdas merged across files on MSVC (see tu_isolation.h). Keep this file
// the same as test_tu_isolation_a.cpp line for line down to the end of the probe test.
#include <doctest/doctest.h>

#include "tu_isolation.h"

constexpr int kProbeCounter = __COUNTER__;
constexpr int kProbeLine = __LINE__ + 1;
TEST_CASE("tu isolation: file B's probe runs file B's lambda") {
    CHECK(helios::tu_isolation::runKeyed([] { return 'B'; }) == 'B');
}

TEST_CASE("tu isolation: both probe tests have the same doctest number") {
    // Otherwise the probes no longer collide on MSVC without their unnamed namespaces, and pass
    // whether or not the hazard is back.
    const helios::tu_isolation::Site a = helios::tu_isolation::siteA();
    const helios::tu_isolation::Site b = helios::tu_isolation::siteB();
    CHECK(a.counter == b.counter);
    CHECK(a.line == b.line);
}

helios::tu_isolation::Site helios::tu_isolation::siteB() {
    return {kProbeCounter, kProbeLine};
}
