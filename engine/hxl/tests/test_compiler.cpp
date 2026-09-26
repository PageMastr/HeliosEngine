// HXL compiler: grammar, precedence, typing, diagnostics (status + position) and limits.
#include <doctest/doctest.h>

#include <bit>
#include <format>
#include <string>

#include "helios/hxl/hxl.h"

using namespace helios;
using namespace helios::hxl;

namespace {

f64 evalNum(std::string_view src, const CompileOptions& opts = {}) {
    auto p = compile(src, opts);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    MapEnv env;
    env.bind(*p);
    Value v;
    REQUIRE(eval(*p, env, v) == Status::Ok);
    REQUIRE(v.type == Type::Number);
    return v.number;
}

bool evalBool(std::string_view src) {
    auto p = compile(src);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    MapEnv env;
    env.bind(*p);
    Value v;
    REQUIRE(eval(*p, env, v) == Status::Ok);
    REQUIRE(v.type == Type::Bool);
    return v.asBool();
}

std::string errorOf(std::string_view src, const CompileOptions& opts = {}) {
    Diagnostic d;
    auto p = compile(src, opts, &d);
    if (p) return "OK";
    return std::format("{} {}:{}", statusName(d.status), d.line, d.column);
}

} // namespace

TEST_CASE("hxl compiler: arithmetic precedence and associativity") {
    CHECK(evalNum("1 + 2 * 3") == 7.0);
    CHECK(evalNum("(1 + 2) * 3") == 9.0);
    CHECK(evalNum("10 - 4 - 3") == 3.0);
    CHECK(evalNum("24 / 4 / 3") == 2.0);
    CHECK(evalNum("-2 ^ 2") == -4.0);   // unary minus binds looser than ^
    CHECK(evalNum("2 ^ 3 ^ 2") == 512.0); // right associative
    CHECK(evalNum("2 ^ -1") == 0.5);
    CHECK(evalNum("--3") == 3.0);
    CHECK(evalNum("2 * -3") == -6.0);
    CHECK(evalNum(".5 + 1.") == 1.5);
    CHECK(evalNum("1e3 + 2.5E-1") == 1000.25);
    CHECK(evalNum("1 + 2; // trailing comment") == 3.0);
    CHECK(evalNum("// leading comment\n  4") == 4.0);
}

TEST_CASE("hxl compiler: booleans, comparisons and logic") {
    CHECK(evalBool("1 < 2"));
    CHECK_FALSE(evalBool("2 <= 1"));
    CHECK(evalBool("3 >= 3 && 4 > 3"));
    CHECK(evalBool("1 == 1 == true")); // (1 == 1) == true
    CHECK(evalBool("!(1 != 1)"));
    CHECK(evalBool("false || 1 < 2 && true"));  // && binds tighter than ||
    CHECK_FALSE(evalBool("true && false || false"));
    CHECK(evalBool("select(1 > 2, false, true)"));
}

TEST_CASE("hxl compiler: formula declarations") {
    auto p = compile("formula ThrustToWeight(ship) = attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81);");
    REQUIRE(p.ok());
    CHECK(p->name() == "ThrustToWeight");
    REQUIRE(p->params().size() == 1);
    CHECK(p->params()[0] == "ship");
    CHECK(p->attrSymbols() == std::vector<std::string>{"MaxThrust", "Mass"});
    MapEnv env;
    env.attrs["ship"]["MaxThrust"] = 981.0;
    env.attrs["ship"]["Mass"] = 10.0;
    env.bind(*p);
    Value v;
    REQUIRE(eval(*p, env, v) == Status::Ok);
    CHECK(v.number == 981.0 / (10.0 * 9.81));

    // Declared parameters replace the options' parameters.
    CompileOptions opts;
    opts.params = {"x"};
    auto q = compile("formula F() = 1", opts);
    REQUIRE(q.ok());
    CHECK(q->params().empty());
}

TEST_CASE("hxl compiler: context fields, attributes, tags and symbols") {
    CompileOptions opts;
    opts.params = {"src", "tgt", "ctx"};
    auto p = compile("attr(src, Tracking.Speed) * ctx.distance + attr(tgt, Sig) - attr(src, Tracking.Speed)", opts);
    REQUIRE(p.ok());
    // Deduplicated in first-use order; params index the options' list.
    CHECK(p->attrSymbols() == std::vector<std::string>{"Tracking.Speed", "Sig"});
    CHECK(p->fieldSymbols() == std::vector<std::string>{"distance"});
    const auto refs = p->references();
    REQUIRE(refs.size() == 4);
    CHECK(refs[0].param == 0);
    CHECK(refs[1].op == Op::Field);
    CHECK(refs[1].param == 2);
    CHECK(refs[2].param == 1);

    // Nested curves register in code order (the inner curve's op runs first).
    auto c = compile("curve(Outer, curve(Inner, 1))");
    REQUIRE(c.ok());
    CHECK(c->curveSymbols() == std::vector<std::string>{"Inner", "Outer"});
}

TEST_CASE("hxl compiler: no constant folding; constants deduplicated by bits") {
    auto p = compile("1 + 2 + 1");
    REQUIRE(p.ok());
    CHECK(p->constants() == std::vector<f64>{1.0, 2.0});
    // Const 0, Const 1, Add, Const 0, Add
    const std::vector<u8> expected = {0x01, 0, 0, 0x01, 1, 0, 0x11, 0x01, 0, 0, 0x11};
    CHECK(p->code() == expected);
    CHECK(p->cost() == 5);
    CHECK(p->maxStack() == 2);
}

TEST_CASE("hxl compiler: diagnostics carry stable status codes and positions") {
    CHECK(errorOf("1 + @") == "E_LEX 1:5");
    CHECK(errorOf("1 & 2") == "E_LEX 1:3");
    CHECK(errorOf("1 | 2") == "E_LEX 1:3");
    CHECK(errorOf("\xc3\xa9") == "E_LEX 1:1");
    CHECK(errorOf("1e") == "E_NUMBER 1:1");
    CHECK(errorOf("12abc") == "E_NUMBER 1:1");
    CHECK(errorOf("1.2.3") == "E_NUMBER 1:1");
    CHECK(errorOf("1e400") == "E_NUMBER 1:1");
    CHECK(errorOf("1e-400") == "E_NUMBER 1:1");
    CHECK(errorOf("1e-310") == "E_NUMBER 1:1"); // subnormal literal
    CHECK(errorOf("0.0e-400") == "OK");       // zero is fine
    CHECK(errorOf("1 +") == "E_SYNTAX 1:4");
    CHECK(errorOf("(1 + 2") == "E_SYNTAX 1:7");
    CHECK(errorOf("1 2") == "E_SYNTAX 1:3");
    CHECK(errorOf("a.") == "E_SYNTAX 1:3");
    CHECK(errorOf("min(1,") == "E_SYNTAX 1:7");
    CHECK(errorOf("formula (x) = 1") == "E_SYNTAX 1:9");
    CHECK(errorOf("formula F(x, x) = 1") == "E_DUPLICATE_PARAM 1:14");
    CHECK(errorOf("formula F(x) 1") == "E_SYNTAX 1:14");
    CHECK(errorOf("x = 1") == "E_SYNTAX 1:3");
    CHECK(errorOf("\n\n  foo") == "E_UNKNOWN_NAME 3:3");
    CHECK(errorOf("self") == "E_TYPE 1:1");
    CHECK(errorOf("other.x") == "E_UNKNOWN_NAME 1:1");
    CHECK(errorOf("self.a.b") == "E_SYNTAX 1:1");
    CHECK(errorOf("foo(1)") == "E_UNKNOWN_FUNCTION 1:1");
    CHECK(errorOf("1 + min(1)") == "E_ARITY 1:5");
    CHECK(errorOf("sqrt(1, 2)") == "E_ARITY 1:1");
    CHECK(errorOf("stacks(1)") == "E_ARITY 1:1");
    CHECK(errorOf("1 + true") == "E_TYPE 1:3");
    CHECK(errorOf("-true") == "E_TYPE 1:1");
    CHECK(errorOf("!1") == "E_TYPE 1:1");
    CHECK(errorOf("1 && true") == "E_TYPE 1:3");
    CHECK(errorOf("true || 1") == "E_TYPE 1:6");
    CHECK(errorOf("1 == true") == "E_TYPE 1:3");
    CHECK(errorOf("select(1, 2, 3)") == "E_TYPE 1:8");
    CHECK(errorOf("select(true, 2, false)") == "E_TYPE 1:17");
    CHECK(errorOf("min(1, true)") == "E_TYPE 1:8");
    CHECK(errorOf("sqrt((1 < 2))") == "E_TYPE 1:6");
    CHECK(errorOf("attr(1, Hp)") == "E_ENTITY_ARG 1:6");
    CHECK(errorOf("attr(self.x, Hp)") == "E_ENTITY_ARG 1:6");
    CHECK(errorOf("attr(nobody, Hp)") == "E_UNKNOWN_NAME 1:6");
    CHECK(errorOf("attr(self, 1)") == "E_SYMBOL_ARG 1:12");
    CHECK(errorOf("tag(self, 1 + 2)") == "E_SYMBOL_ARG 1:11");
    CHECK(errorOf("curve(1, 2)") == "E_SYMBOL_ARG 1:7");
    CHECK(errorOf("curve(C, true)") == "E_TYPE 1:10");
    CompileOptions boolExpected;
    boolExpected.expectedType = Type::Bool;
    CHECK(errorOf("  1 + 2", boolExpected) == "E_RESULT_TYPE 1:3");
    CHECK(errorOf("(1 < 2)", boolExpected) == "OK");
    CompileOptions dup;
    dup.params = {"a", "a"};
    CHECK(errorOf("1", dup) == "E_DUPLICATE_PARAM 1:1");
}

TEST_CASE("hxl compiler: limits") {
    // Parse depth.
    std::string deep(limits::kMaxParseDepth + 1, '(');
    deep += "1";
    deep += std::string(limits::kMaxParseDepth + 1, ')');
    CHECK(errorOf(deep).starts_with("E_LIMIT"));
    std::string ok(limits::kMaxParseDepth - 1, '(');
    ok += "1";
    ok += std::string(limits::kMaxParseDepth - 1, ')');
    CHECK(errorOf(ok) == "OK");
    // Prefix operator chains count as nesting.
    CHECK(errorOf(std::string(limits::kMaxParseDepth + 1, '-') + "1").starts_with("E_LIMIT"));
    // Long operator chains exceed the tree depth without recursion.
    std::string chain = "1";
    for (u32 i = 0; i < limits::kMaxAstDepth; ++i) chain += "+1";
    CHECK(errorOf(chain).starts_with("E_LIMIT"));
    // Source size.
    CHECK(errorOf(std::string(limits::kMaxSourceBytes + 1, ' ')) == "E_LIMIT 1:1");
    // Parameters.
    std::string params = "formula F(";
    for (u32 i = 0; i <= limits::kMaxParams; ++i) params += std::format("{}p{}", i ? "," : "", i);
    params += ") = 1";
    CHECK(errorOf(params) == "E_LIMIT 1:65");
    // Cost budget.
    CompileOptions cheap;
    cheap.maxCost = 3;
    CHECK(errorOf("1 + 2", cheap) == "OK");
    CHECK(errorOf("1 + 2 + 3", cheap) == "E_LIMIT 1:1");
    CHECK(errorOf("exp(1)", cheap) == "E_LIMIT 1:1");
    // Op count: a flat min() with thousands of arguments.
    std::string wide = "min(1";
    for (int i = 0; i < 3000; ++i) wide += ",1";
    wide += ")";
    CHECK(errorOf(wide) == "E_LIMIT 1:1");
    // Distinct constants.
    std::string consts = "min(0";
    for (u32 i = 1; i <= limits::kMaxConstants; ++i) consts += std::format(",{}", i);
    consts += ")";
    CHECK(errorOf(consts).starts_with("E_LIMIT"));
    // Long names.
    CHECK(errorOf(std::string(limits::kMaxNameBytes + 1, 'a')) == "E_LIMIT 1:1");
}

// Review regression (WP-0.19): the deepest programs the limits admit, and one level more. Hostile
// sources recurse through the parser and the code generator once per level; before the parser used
// precedence climbing and formatted its diagnostics out of line, 127 nested parentheses took
// ~0.5 MiB of stack with GCC -O2 (engine/hxl/README.md has the measured bound). The Go twin is
// TestDeepestPrograms in services/pkg/hxl/review_test.go.
TEST_CASE("hxl compiler: the deepest programs within the limits compile and evaluate") {
    const u32 nest = limits::kMaxParseDepth - 1; // the top-level expression is nesting level 1
    auto repeat = [](std::string_view s, u32 n) {
        std::string out;
        for (u32 i = 0; i < n; ++i) out += s;
        return out;
    };
    // Parentheses, prefix operators and right-associative powers count as nesting.
    CHECK(evalNum(repeat("(", nest) + "1" + repeat(")", nest)) == 1.0);
    CHECK(evalNum(repeat("-", nest) + "1") == -1.0);
    CHECK(errorOf(repeat("-", nest + 1) + "1") == "E_LIMIT 1:128");
    CHECK(evalNum("1" + repeat("^1", nest)) == 1.0);
    CHECK(errorOf("1" + repeat("^1", nest + 1)) == "E_LIMIT 1:256");
    // Nested calls around a left-associative chain: tree depth exactly kMaxAstDepth (126 calls over
    // 129 operators over the chain's first leaf), then one level more (reported at the outermost
    // call).
    const u32 calls = 126;
    const u32 links = limits::kMaxAstDepth - calls - 1;
    auto nestedMin = [&](u32 n) { return repeat("min(1, ", calls) + "1" + repeat("+1", n) + repeat(")", calls); };
    CHECK(evalNum(nestedMin(links)) == 1.0);
    CHECK(errorOf(nestedMin(links + 1)) == "E_LIMIT 1:1");
    CHECK(evalNum(repeat("clamp(1, 0, ", calls) + "1" + repeat("+1", links) + repeat(")", calls)) == 1.0);
    CHECK(evalBool(repeat("select(true, ", calls) + "true" + repeat("||true", links) + repeat(", false)", calls)));
    // An operator of every precedence level in front of each nested parenthesis (the deepest
    // parser recursion per level): 7 tree levels each.
    const u32 levels = (limits::kMaxAstDepth - 1) / 7;
    const std::string everyLevel = repeat("select(true || true && true == 1 < 1 + 1 * (", levels) + "1" + repeat("), 1, 0)", levels);
    CHECK(evalNum(everyLevel) == 1.0);
}

TEST_CASE("hxl compiler: the static cost bounds every evaluation") {
    auto p = compile("select(1 < 2, exp(1) + ln(2), pow(2, 3))");
    REQUIRE(p.ok());
    // Const Const Lt JIF Const Exp Const Ln Add Jump Const Const Pow
    CHECK(p->cost() == 1 + 1 + 1 + 1 + 1 + 8 + 1 + 8 + 1 + 1 + 1 + 1 + 8);
}
