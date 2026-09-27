// Seeded parse failure: a string continued by a line splice, not closed on the next line.
#include <doctest/doctest.h>

namespace {
const char* const kSpliced = "a \
b;
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace
