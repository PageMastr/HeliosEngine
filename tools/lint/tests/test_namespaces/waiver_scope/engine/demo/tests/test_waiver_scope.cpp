// Seeded violation: a waiver region cannot outlive the scope it begins in.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: plain") {
    CHECK(true);
}
// helios-lint: outside-anon-namespace begin (opened inside the unnamed namespace)
} // namespace
int leaked = 0;
// helios-lint: outside-anon-namespace end
