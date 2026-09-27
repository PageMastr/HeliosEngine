// Seeded violation: a named namespace is not unique per file.
#include <doctest/doctest.h>

namespace helios::demo {

TEST_CASE("demo: in a named namespace") {
    CHECK(true);
}

} // namespace helios::demo
