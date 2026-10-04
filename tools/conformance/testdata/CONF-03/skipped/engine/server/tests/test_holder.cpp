#include <doctest/doctest.h>

namespace {
TEST_CASE("conformance/holder_rule: a cell keeps its zones" * doctest::skip()) {}
TEST_CASE("conformance/holder_rule: after another decorator" * doctest::timeout(120) * doctest::skip()) {}
TEST_CASE("conformance/holder_rule: after a string with parentheses" * doctest::description("60 s (no responders)") *
          doctest::skip()) {}
TEST_CASE("conformance/holder_rule: nested" * doctest::timeout(kT.count()) * doctest::skip()) {}
TEST_CASE("conformance/holder_rule: deeper" * doctest::timeout(std::chrono::seconds(f(g(1))).count()) *
          doctest::may_fail(')' == 'x')) {}
TEST_CASE("conformance/holder_rule: kept" * doctest::description("doctest::skip() is only text")) {}
} // namespace
