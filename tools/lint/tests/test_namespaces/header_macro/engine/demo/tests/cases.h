#pragma once
// Seeded violation (review finding (b)): a test case in a header under tests/, at global scope.
#include <doctest/doctest.h>

TEST_CASE("demo: from a header") {
    CHECK(true);
}
