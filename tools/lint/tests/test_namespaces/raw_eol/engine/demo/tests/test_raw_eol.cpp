// Seeded violation (review round 3, B2): a raw string that closes at the end of its line, in code
// and in a #define. Its body holds a '/*' that is not a comment, so both test cases are global.
#include <doctest/doctest.h>

namespace {
TEST_CASE("demo: real") { CHECK(true); }
[[maybe_unused]] const char* const kRaw = R"(/*)"
    ;
} // namespace
TEST_CASE("demo: global after a raw string") { CHECK(true); }
namespace { // */
#define HELIOS_DEMO_RAW R"(/*)"
} // namespace
TEST_CASE("demo: global after a raw string in a directive") { CHECK(true); }
namespace { // */
} // namespace
