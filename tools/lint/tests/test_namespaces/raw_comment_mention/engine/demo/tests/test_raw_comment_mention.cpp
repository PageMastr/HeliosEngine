// Seeded clean file (review item 2): a comment that only mentions R"( is not an unterminated raw
// string.
#include <doctest/doctest.h>

namespace {
// the lexer accepts R"( as a raw string
TEST_CASE("demo: after the comment") {
    CHECK(true);
}
} // namespace
