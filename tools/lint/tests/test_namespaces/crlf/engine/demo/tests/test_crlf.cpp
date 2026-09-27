// Seeded violation with CRLF line endings: the finding must name line 6.
#include <doctest/doctest.h>

namespace {
} // namespace
TEST_CASE("demo: crlf") {
    CHECK(true);
}
