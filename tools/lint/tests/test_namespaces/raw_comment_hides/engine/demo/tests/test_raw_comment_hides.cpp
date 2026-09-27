// Seeded violation (review item 2): an R"( inside a comment must not open a raw string that
// swallows the code up to the next )", here a test case at global scope.
#include <doctest/doctest.h>
namespace {
// Raw strings start with R"( and
} // namespace
TEST_CASE("really at global scope") { CHECK(true); }
namespace {
// end with )" in C++.
} // namespace
