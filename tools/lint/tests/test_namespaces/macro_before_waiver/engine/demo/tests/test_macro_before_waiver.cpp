// Seeded violation (review round 2, B1): a macro call without a ';' just before a waiver region is
// not part of the region.
#include <doctest/doctest.h>

#define HELPER static int runOne() { return 1; }

HELPER
// helios-lint: outside-anon-namespace begin (a waived declaration)
int waived = 0;
// helios-lint: outside-anon-namespace end

namespace {
TEST_CASE("demo: calls the helper") {
    CHECK(runOne() + waived == 1);
}
} // namespace
