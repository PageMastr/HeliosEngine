// Seeded violation: a waiver end without a begin.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace
// helios-lint: outside-anon-namespace end
