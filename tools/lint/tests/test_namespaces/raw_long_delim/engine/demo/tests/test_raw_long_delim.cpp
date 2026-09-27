// Seeded parse failure: a raw string delimiter longer than the 16 characters C++ allows.
#include <doctest/doctest.h>

namespace {
const char* const kRaw = R"abcdefghijklmnopq(x)abcdefghijklmnopq";
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace
