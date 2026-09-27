// Seeded violation (review finding (c)): the unnamed namespace exists only inside #if 0 groups.
#include <doctest/doctest.h>

#if 0
namespace {
#endif

TEST_CASE("demo: not in a namespace after preprocessing") {
    CHECK(true);
}

#if 0
} // namespace
#endif
