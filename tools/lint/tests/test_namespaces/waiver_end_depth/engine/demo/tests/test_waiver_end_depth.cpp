// Seeded violation: a waiver region ends at the brace depth it begins at.
#include <doctest/doctest.h>

// helios-lint: outside-anon-namespace begin (opened at file scope)
int leaked = 0;
namespace {
// helios-lint: outside-anon-namespace end
TEST_CASE("demo: plain") {
    CHECK(leaked == 0);
}
} // namespace
