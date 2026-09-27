// Seeded parse failure: an unclosed brace, so the lint cannot tell which scope anything is in.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: unbalanced") {
    CHECK(true);
}
