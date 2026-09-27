// Seeded violation: the file's only test case has its name on the next line. It is still a test
// source, so the helper outside the unnamed namespace is reported.
#include <doctest/doctest.h>

static int helper() { return 1; }

namespace {
TEST_CASE(
    "demo: split") {
    CHECK(helper() == 1);
}
} // namespace
