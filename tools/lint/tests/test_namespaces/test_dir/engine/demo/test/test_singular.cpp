// Seeded violation (review finding (f)): a test/ directory is scanned like tests/.
#include <doctest/doctest.h>

TEST_CASE("demo: in test/") {
    CHECK(true);
}
