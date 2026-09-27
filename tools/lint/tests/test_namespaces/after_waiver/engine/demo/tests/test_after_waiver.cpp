// Seeded violation: code right after a waiver region is checked again.
#include <doctest/doctest.h>

// helios-lint: outside-anon-namespace begin (a waived helper)
static int waived() {
    return 1;
}
// helios-lint: outside-anon-namespace end
static int leaked() { return 2; }

namespace {
TEST_CASE("demo: calls both") {
    CHECK(waived() + leaked() == 3);
}
} // namespace
