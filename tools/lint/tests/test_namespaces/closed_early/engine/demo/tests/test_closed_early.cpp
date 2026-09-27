// Seeded violation: the helpers are in an unnamed namespace, the tests after it are not (the
// shape of test_async_io.cpp before #15).
#include <doctest/doctest.h>

namespace {
int helper() {
    return 1;
}
} // namespace

TEST_CASE("demo: after the unnamed namespace") {
    CHECK(helper() == 1);
}
