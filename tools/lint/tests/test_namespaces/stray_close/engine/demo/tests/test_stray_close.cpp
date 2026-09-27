// Seeded parse failure: a '}' that closes nothing.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: stray") {
    CHECK(true);
}
}
} // namespace
