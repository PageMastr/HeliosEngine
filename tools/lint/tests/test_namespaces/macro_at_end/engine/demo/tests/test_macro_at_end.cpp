// Seeded violation: a macro call at global scope that ends the file, without a ';'.
#include <doctest/doctest.h>

#define MAKE_TEST(name) TEST_CASE(name) { CHECK(true); }

namespace {
TEST_CASE("demo: a plain test") {
    CHECK(true);
}
} // namespace

MAKE_TEST("demo: generated at the end")
