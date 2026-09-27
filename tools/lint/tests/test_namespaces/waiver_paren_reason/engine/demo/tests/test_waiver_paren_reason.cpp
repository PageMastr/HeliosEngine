// Seeded violation (review round 2, nit 5): a waiver reason needs a letter or digit.
#include <doctest/doctest.h>

// helios-lint: outside-anon-namespace begin ())
int leaked = 0;
// helios-lint: outside-anon-namespace end

namespace {
TEST_CASE("demo: plain") {
    CHECK(leaked == 0);
}
} // namespace
