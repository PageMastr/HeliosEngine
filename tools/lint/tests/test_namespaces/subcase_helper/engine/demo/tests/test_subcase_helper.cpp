// Seeded violation: a helper with subcases outside the unnamed namespace (its lambdas are exposed
// in the same way as a test case's).
#include <doctest/doctest.h>

static void checkBoth(int v) {
    SUBCASE("positive") {
        CHECK(v > 0);
    }
}

namespace {
TEST_CASE("demo: calls a helper") {
    checkBoth(1);
}
} // namespace
