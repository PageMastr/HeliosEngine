// Seeded violation: a directive may follow a comment on its line, so each #else below ends its
// #if 0 group and the test case after it is global.
#include <doctest/doctest.h>

#if 0
/* a comment */ #else
TEST_CASE("demo: after a one-line comment") { CHECK(true); }
#endif

#if 0
/* a comment
   over two lines */ #else
TEST_CASE("demo: after a two-line comment") { CHECK(true); }
#endif
