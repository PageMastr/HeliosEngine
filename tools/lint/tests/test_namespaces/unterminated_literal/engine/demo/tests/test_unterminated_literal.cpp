// Seeded parse failure: a string literal without its closing quote.
#include <doctest/doctest.h>

namespace {
const char* const kOpen = "never closed;
TEST_CASE("demo: literal") {
    CHECK(true);
}
} // namespace
