// Seeded violation: only a plain `#if 0` group is skipped. `#if 0 || defined(...)` is lexed, so
// the test case in it is reported as global.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: plain") { CHECK(true); }
} // namespace

#if 0 || defined(HELIOS_DEMO)
TEST_CASE("demo: global when HELIOS_DEMO is defined") { CHECK(true); }
#endif
