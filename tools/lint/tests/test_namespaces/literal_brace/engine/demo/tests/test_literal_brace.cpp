// Seeded violation: every '{' before the test case is in a literal or comment, so the unnamed
// namespace is closed when the test case starts. A lint that counted those braces would pass it.
#include <doctest/doctest.h>

#include <string>

namespace {
const char kOpen = '{';
constexpr int kThousand = 1'000; const char kAlsoOpen = '{';
const std::string kRaw = R"x({ )" {)x";
const std::string kPlain = "{ \" {";
// {
/* { */
} // namespace

TEST_CASE("demo: after literal braces") {
    CHECK(kOpen == kAlsoOpen);
}
