// Seeded violation: waiver regions do not nest.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace

// helios-lint: outside-anon-namespace begin (outer)
// helios-lint: outside-anon-namespace begin (inner)
int leaked = 0;
// helios-lint: outside-anon-namespace end
