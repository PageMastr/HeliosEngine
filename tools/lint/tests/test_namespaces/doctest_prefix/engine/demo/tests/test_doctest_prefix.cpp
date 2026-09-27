// Seeded violation: the DOCTEST_-prefixed form of a test macro at global scope.
#include <doctest/doctest.h>

namespace {
int value() {
    return 2;
}
} // namespace

DOCTEST_TEST_CASE_FIXTURE(int, "demo: fixture outside") {
    CHECK(value() == 2);
}
