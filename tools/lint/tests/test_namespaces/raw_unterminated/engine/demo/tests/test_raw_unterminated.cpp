// Seeded parse failure: a raw string literal that never ends.
#include <doctest/doctest.h>

namespace {
const char* const kRaw = R"x(never closed )" nor here;
TEST_CASE("demo: raw") {
    CHECK(true);
}
} // namespace
