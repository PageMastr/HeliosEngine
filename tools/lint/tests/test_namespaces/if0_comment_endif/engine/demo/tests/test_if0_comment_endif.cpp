// Seeded violation (review round 2, nit 1): an #endif inside a block comment in an #if 0 group does
// not end the group, so both halves of the unnamed namespace are skipped.
#include <doctest/doctest.h>

#if 0
/*
#endif
namespace { // */
#endif
TEST_CASE("demo: global after preprocessing") {
    CHECK(true);
}
#if 0
/*
#endif
} // */
#endif
