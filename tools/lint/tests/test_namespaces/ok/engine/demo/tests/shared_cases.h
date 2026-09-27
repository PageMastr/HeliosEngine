#pragma once
// Part of the ok fixture: headers are held to the test-macro rule only, so a shared type may sit at
// global scope next to test cases in an unnamed namespace.
#include <doctest/doctest.h>

struct SharedFixture {
    int value = 1;
};

namespace {
TEST_CASE("demo: from a header, in an unnamed namespace") {
    CHECK(SharedFixture{}.value == 1);
}
} // namespace
