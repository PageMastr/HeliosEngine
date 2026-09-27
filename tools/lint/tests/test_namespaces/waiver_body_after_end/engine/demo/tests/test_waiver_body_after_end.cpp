// Seeded violation (review round 3, B1): a waived head whose body comes after the waiver end. The
// region covers the head only, so each body is a declaration outside an unnamed namespace.
#include <doctest/doctest.h>

#define HEAD(n) static int n()

namespace helpers {
// helios-lint: outside-anon-namespace begin (a waived head in a named namespace)
static int inNamed()
// helios-lint: outside-anon-namespace end
{ return 1; }
} // namespace helpers

// helios-lint: outside-anon-namespace begin (a waived struct head)
struct Helper
// helios-lint: outside-anon-namespace end
{ int x = 1; };

// helios-lint: outside-anon-namespace begin (a waived macro head)
HEAD(fromMacro)
// helios-lint: outside-anon-namespace end
{ return 1; }

namespace {
TEST_CASE("demo: calls the helpers") {
    CHECK(helpers::inNamed() + Helper{}.x + fromMacro() == 3);
}
} // namespace
