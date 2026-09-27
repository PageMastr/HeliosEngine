// Seeded violation: a waiver region must end.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace

// helios-lint: outside-anon-namespace begin (never closed)
int leaked = 0;
