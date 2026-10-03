#include <doctest/doctest.h>

namespace {
TEST_CASE("conformance/holder_rule: a cell keeps its zones" * doctest::skip()) {}
TEST_CASE("conformance/holder_rule: after another decorator" * doctest::timeout(120) * doctest::skip()) {}
} // namespace
