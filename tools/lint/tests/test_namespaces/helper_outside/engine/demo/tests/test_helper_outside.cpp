// Seeded violation (review finding (a)): a helper at file scope, before the unnamed namespace. Two
// files of one executable with a same-named helper collide on MSVC as test bodies did.
#include <doctest/doctest.h>

static int runOne() {
    int r = 0;
    auto set = [&r] { r = 1; };
    set();
    return r;
}

namespace {
TEST_CASE("demo: uses a helper at global scope") {
    CHECK(runOne() == 1);
}
} // namespace
