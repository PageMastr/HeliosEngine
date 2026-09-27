// Seeded file for lint_test_namespaces_fixture_ok: every test macro and declaration is inside an
// unnamed namespace or a waiver, behind constructs a naive lexer gets wrong. TEST_CASE("in a comment")
/* A block comment with } braces }, TEST_SUITE("x"), namespace { and R"( */
// This comment mentions R"( as a raw-string opener and has no terminator.
#include <doctest/doctest.h>

#include <string>

#define HELIOS_DEMO_BLOCK(x) \
    do {                     \
        (void)(x);           \
    } while (false)
#define HELIOS_DEMO_OPEN {

// Using-directives, using-declarations and aliases may stay outside: they define nothing.
using std::string;
using namespace std::string_literals;
namespace str = std;

#if 0
TEST_CASE("demo: skipped with its group") {
namespace {
#endif

// helios-lint: outside-anon-namespace begin (explicit specializations cannot live in an unnamed namespace)
namespace demo {
template <class T>
struct Traits {};
} // namespace demo

template <>
struct demo::Traits<int> {
    static constexpr int kValue = 1'000'000;
};
// helios-lint: outside-anon-namespace end

namespace
{

const std::string kJson = R"json({"a": "}", "b": [1, 2]})json";
const auto* const kRaw = u8R"(namespace { TEST_CASE("x") )";
const auto* const kRawQuote = u8R"x(a"{)x";
const char kOpen = '{';
const char kClose = '}';
const char kQuote = '\'';
const char kDouble = '"';
const std::string kEscaped = "\"}\" and \\";
const std::string kFooBar = "FOOBAR"; // a string that ends in R"
constexpr long long kBig = 0xFF'FF'FF'FFLL;
constexpr int kSep = 10'000; const char kBrace = '}';
const char8_t kU8Open = u8'{';
const wchar_t kWideClose = L'}';
const char8_t kU8a = u8'a'; const char kOpen2 = '{';
const char kClose2 = '}';
int MY_TEST_CASE_HELPER = 0;
namespace namespace_like = demo;
inline namespace v1 {
struct Inner {};
} // namespace v1
const char* const kSpliced = "a { \
b";
// A spliced comment \
{ still the comment
/* A block comment in the unnamed scope
   } spanning lines {
*/
#if 0
int skipped( {
#else
namespace live_branch {
#endif
struct InLiveBranch {};
} // namespace live_branch
namespace after_raw { const char* const kR = R"(x)"; }

TEST_CASE("demo: a plain test") {
    HELIOS_DEMO_BLOCK(kOpen);
    CHECK(kClose == '}');
    SUBCASE("a subcase") { CHECK(kQuote != kDouble); }
}

TEST_SUITE("demo suite") {
    TEST_CASE("demo: inside a suite") { CHECK(kJson.size() > 0); }
}

SCENARIO("demo: a scenario") {
    GIVEN("a value") {
        WHEN("it is read") {
            THEN("it holds") { CHECK(kSep == 10'000); }
        }
    }
}

DOCTEST_TEST_CASE("demo: the long form") { CHECK(kBig > 0); }

} // namespace

namespace demo {
namespace {
TEST_CASE("demo: an unnamed namespace inside a named one") { CHECK(kRaw != nullptr); }
} // namespace
} // namespace demo

inline namespace v2 {
namespace {
TEST_CASE("demo: inside an inline namespace") { CHECK(true); }
} // namespace
} // namespace v2
