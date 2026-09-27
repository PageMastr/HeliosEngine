// Seeded violation (review finding (e)): doctest's BDD subcases in a helper outside the namespace.
#include <doctest/doctest.h>

void steps() {
    GIVEN("a value") {
        WHEN("it is checked") {
            THEN("it holds") { CHECK(true); }
        }
    }
}

namespace {
SCENARIO("demo: runs the steps") {
    steps();
}
} // namespace
