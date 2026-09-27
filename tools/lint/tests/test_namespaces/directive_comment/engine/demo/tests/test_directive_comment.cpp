// Seeded violation: a block comment opened on a directive line hides the next line's opener.
#include <doctest/doctest.h> /* the comment goes on
namespace { */

TEST_CASE("demo: global after preprocessing") {
    CHECK(true);
}

#define HELIOS_DEMO_END /*
} // namespace */
