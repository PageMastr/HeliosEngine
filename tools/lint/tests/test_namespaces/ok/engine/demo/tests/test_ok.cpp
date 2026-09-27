// Seeded file for lint_test_namespaces_fixture_ok: every test macro is inside an unnamed namespace,
// behind constructs a naive brace counter gets wrong. TEST_CASE("in a comment") { namespace {
/* A block comment with } braces }, TEST_SUITE("x") and namespace { */
#include <doctest/doctest.h>

#include <string>

#define HELIOS_DEMO_BLOCK(x) \
    do {                     \
        (void)(x);           \
    } while (false)
#define HELIOS_DEMO_OPEN {

namespace demo {
template <class T>
struct Traits {};
} // namespace demo

// An explicit specialization cannot live in an unnamed namespace; outside it is fine.
template <>
struct demo::Traits<int> {
    static constexpr int kValue = 1'000'000;
};

namespace {

const std::string kJson = R"json({"a": "}", "b": [1, 2]})json";
const char* const kRaw = u8R"(namespace { TEST_CASE("x") )";
const char kOpen = '{';
const char kClose = '}';
const char kQuote = '\'';
const char kDouble = '"';
const std::string kEscaped = "\"}\" and \\";
const std::string kFooBar = "FOOBAR"; // a string that ends in R"
constexpr long long kBig = 0xFF'FF'FF'FFLL;
constexpr int kSep = 10'000; const char kBrace = '}';
int MY_TEST_CASE_HELPER = 0;
namespace namespace_like = demo;

TEST_CASE("demo: a plain test") {
    HELIOS_DEMO_BLOCK(kOpen);
    CHECK(kClose == '}');
    SUBCASE("a subcase") { CHECK(kQuote != kDouble); }
}

TEST_SUITE("demo suite") {
    TEST_CASE("demo: inside a suite") { CHECK(kJson.size() > 0); }
}

DOCTEST_TEST_CASE("demo: the long form") { CHECK(kBig > 0); }

} // namespace

namespace demo {
namespace {
TEST_CASE("demo: an unnamed namespace inside a named one") { CHECK(kRaw != nullptr); }
} // namespace
} // namespace demo
