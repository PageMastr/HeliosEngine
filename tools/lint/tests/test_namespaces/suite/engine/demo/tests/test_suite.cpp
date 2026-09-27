// Seeded violation: a test suite (a named namespace of its own) at global scope.
#include <doctest/doctest.h>

TEST_SUITE("demo") {
    TEST_CASE("demo: in a suite") {
        CHECK(true);
    }
}
