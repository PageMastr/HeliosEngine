// Asserts are forced on for this translation unit so the test is meaningful in every build config.
#undef HELIOS_ENABLE_ASSERTS
#define HELIOS_ENABLE_ASSERTS 1

#include <doctest/doctest.h>

#include <atomic>
#include <string>

#include "helios/core/assert.h"
#include "helios/core/log.h"

using namespace helios;

namespace {

struct Seen {
    std::string kind;
    std::string expression;
    std::string message;
    u32 line = 0;
    int count = 0;
};
Seen g_seen;

AssertAction recordingHandler(const AssertInfo& info) {
    g_seen.kind = info.kind;
    g_seen.expression = info.expression;
    g_seen.message = std::string(info.message);
    g_seen.line = info.line;
    ++g_seen.count;
    return AssertAction::Continue;
}

struct HandlerScope {
    AssertHandler previous;
    HandlerScope() : previous(setAssertHandler(&recordingHandler)) { g_seen = Seen{}; }
    ~HandlerScope() { setAssertHandler(previous); }
};

[[maybe_unused]] int unreachableAfterSwitch(int v) {
    switch (v) {
    case 0: return 1;
    default: break;
    }
    HELIOS_UNREACHABLE("value {}", v); // compiles as a no-return statement
}

} // namespace

TEST_CASE("assert: passing assertions do not call the handler and evaluate once") {
    HandlerScope scope;
    int evaluations = 0;
    HELIOS_ASSERT(++evaluations == 1);
    HELIOS_ASSERT(true, "with message {}", 1);
    CHECK(evaluations == 1);
    CHECK(g_seen.count == 0);
}

TEST_CASE("assert: failures reach the handler with expression, message and location") {
    HandlerScope scope;
    const u64 before = assertFailureCount();
    const int index = 7;
    const u32 line = __LINE__ + 1;
    HELIOS_ASSERT(index < 5, "index {} out of range", index);
    CHECK(g_seen.count == 1);
    CHECK(g_seen.kind == "ASSERT");
    CHECK(g_seen.expression == "index < 5");
    CHECK(g_seen.message == "index 7 out of range");
    CHECK(g_seen.line == line);
    CHECK(assertFailureCount() == before + 1);

    HELIOS_ASSERT(index == 0);
    CHECK(g_seen.count == 2);
    CHECK(g_seen.message.empty());

    // Messages without arguments are formatted too, so brace escapes print as braces.
    HELIOS_ASSERT(index == 1, "expected {{0, 1}}");
    CHECK(g_seen.message == "expected {0, 1}");
}

TEST_CASE("assert: HELIOS_VERIFY always evaluates and checks") {
    HandlerScope scope;
    int sideEffect = 0;
    HELIOS_VERIFY(++sideEffect == 1);
    CHECK(sideEffect == 1);
    CHECK(g_seen.count == 0);
    HELIOS_VERIFY(sideEffect == 2, "verify {}", "failed");
    CHECK(g_seen.count == 1);
    CHECK(g_seen.kind == "VERIFY");
    CHECK(g_seen.message == "verify failed");
}

TEST_CASE("assert: handler installation returns the previous handler") {
    const AssertHandler original = assertHandler();
    CHECK(original == &defaultAssertHandler);
    const AssertHandler previous = setAssertHandler(&recordingHandler);
    CHECK(previous == &defaultAssertHandler);
    CHECK(assertHandler() == &recordingHandler);
    setAssertHandler(nullptr); // restores the default
    CHECK(assertHandler() == &defaultAssertHandler);
    CHECK(unreachableAfterSwitch(0) == 1);
}
