// Seeded violation (review round 2, B1): a macro call that ends a named namespace, without a ';',
// defines a lambda-keyed helper outside every unnamed namespace.
#include <doctest/doctest.h>

#include "helios/jobs/job.h"

#define DEFINE_HELPER(n) static int n() { helios::jobs::Job j([] {}); j.invoke(); return 1; }

namespace helpers {
DEFINE_HELPER(runOne)
} // namespace helpers

namespace {
TEST_CASE("demo: calls the helper") {
    CHECK(helpers::runOne() == 1);
}
} // namespace
