// Seeded violation: a waiver region covers declarations, never test macros.
#include <doctest/doctest.h>

// helios-lint: outside-anon-namespace begin (tries to waive a test case)
TEST_CASE("demo: waived?") {
    CHECK(true);
}
// helios-lint: outside-anon-namespace end
