// Seeded violation (review round 3, B3): a '#' on a line that a splice continues is not a
// directive, so the #if 0 and #endif inside DROP(...) pair with nothing and the test case is global.
#include <doctest/doctest.h>

#define DROP(...)

namespace {
TEST_CASE("demo: real") { CHECK(true); }
#if 1
DROP( \
#if 0
)
} // namespace
TEST_CASE("demo: global after preprocessing") { CHECK(true); }
namespace {
#endif
DROP( \
#endif
)
} // namespace
