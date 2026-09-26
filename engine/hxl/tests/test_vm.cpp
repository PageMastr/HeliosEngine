// HXL VM semantics: IEEE edge cases, min/max/clamp/lerp, curves, lazy select/&&/||, missing inputs,
// NaN canonicalization and the det:: built-ins.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "helios/hxl/hxl.h"
#include "helios/math/scalar.h"

using namespace helios;
using namespace helios::hxl;

namespace {

u64 bits(f64 v) { return std::bit_cast<u64>(v); }

struct Evaluated {
    Status status = Status::Ok;
    Value value;
};

Evaluated run(std::string_view src, MapEnv& env, std::vector<std::string> params = {"self"}) {
    CompileOptions opts;
    opts.params = std::move(params);
    auto p = compile(src, opts);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    env.bind(*p);
    Evaluated e;
    e.status = eval(*p, env, e.value);
    return e;
}

f64 num(std::string_view src, MapEnv& env) {
    Evaluated e = run(src, env);
    REQUIRE(e.status == Status::Ok);
    return e.value.number;
}

} // namespace

TEST_CASE("hxl vm: min/max propagate NaN and order -0 < +0") {
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    CHECK(bits(hxlMin(-0.0, 0.0)) == bits(-0.0));
    CHECK(bits(hxlMin(0.0, -0.0)) == bits(-0.0));
    CHECK(bits(hxlMax(-0.0, 0.0)) == bits(0.0));
    CHECK(bits(hxlMax(0.0, -0.0)) == bits(0.0));
    CHECK(bits(hxlMin(nan, 1.0)) == kCanonicalNaNBits);
    CHECK(bits(hxlMax(1.0, -nan)) == kCanonicalNaNBits);
    CHECK(hxlMin(1.0, 2.0) == 1.0);
    CHECK(hxlMax(1.0, 2.0) == 2.0);
    MapEnv env;
    CHECK(num("min(3, 1, 2)", env) == 1.0);
    CHECK(num("max(3, 1, 2, 7, -1)", env) == 7.0);
    CHECK(num("clamp(5, 0, 1)", env) == 1.0);
    CHECK(num("clamp(-5, 0, 1)", env) == 0.0);
    CHECK(num("clamp(0.5, 0, 1)", env) == 0.5);
    CHECK(num("clamp(0.5, 2, 1)", env) == 1.0); // lo > hi: hi wins
    CHECK(bits(num("clamp(0/0, 0, 1)", env)) == kCanonicalNaNBits);
}

TEST_CASE("hxl vm: IEEE results, signed zero and canonical NaN") {
    MapEnv env;
    CHECK(num("1 / 0", env) == std::numeric_limits<f64>::infinity());
    CHECK(num("-1 / 0", env) == -std::numeric_limits<f64>::infinity());
    CHECK(bits(num("0 / 0", env)) == kCanonicalNaNBits);
    CHECK(bits(num("-(0 / 0)", env)) == kCanonicalNaNBits); // sign of NaN is not observable
    CHECK(bits(num("sqrt(-1)", env)) == kCanonicalNaNBits);
    CHECK(bits(num("-0", env)) == bits(-0.0));
    CHECK(bits(num("0 * -1", env)) == bits(-0.0));
    CHECK(bits(num("-0 + 0", env)) == bits(0.0));
    CHECK(bits(num("sqrt(-0)", env)) == bits(-0.0));
    CHECK(bits(num("floor(-0.5)", env)) == bits(-1.0));
    CHECK(bits(num("ceil(-0.5)", env)) == bits(-0.0));
    CHECK(bits(num("abs(-0)", env)) == bits(0.0));
    CHECK(num("floor(2.5) + ceil(2.5)", env) == 5.0);
    CHECK(num("0.1 + 0.2", env) == 0.1 + 0.2);
    // Comparisons with NaN.
    CHECK(run("0/0 == 0/0", env).value.number == 0.0);
    CHECK(run("0/0 != 0/0", env).value.number == 1.0);
    CHECK(run("0/0 < 1 || 0/0 >= 1", env).value.number == 0.0);
    CHECK(run("-0 == 0", env).value.number == 1.0);
}

TEST_CASE("hxl vm: transcendental built-ins are helios::det") {
    MapEnv env;
    CHECK(bits(num("exp(1.5)", env)) == bits(det::exp(1.5)));
    CHECK(bits(num("ln(10)", env)) == bits(det::ln(10.0)));
    CHECK(bits(num("pow(1.1, 7.5)", env)) == bits(det::pow(1.1, 7.5)));
    CHECK(bits(num("1.1 ^ 7.5", env)) == bits(det::pow(1.1, 7.5)));
    CHECK(bits(num("asinh(-3.25)", env)) == bits(det::asinh(-3.25)));
    CHECK(bits(num("sqrt(2)", env)) == bits(std::sqrt(2.0)));
    CHECK(num("pow(0, 0)", env) == 1.0);
    CHECK(bits(num("ln(-1)", env)) == kCanonicalNaNBits);
    CHECK(num("ln(0)", env) == -std::numeric_limits<f64>::infinity());
    CHECK(num("exp(1000)", env) == std::numeric_limits<f64>::infinity());
    CHECK(bits(num("pow(-8, 1/3)", env)) == kCanonicalNaNBits);
}

TEST_CASE("hxl vm: lerp is a + (b - a) * t with one rounding per op") {
    MapEnv env;
    const f64 a = 0.1, b = 0.7, t = 0.3;
    const f64 d = b - a;
    const f64 p = d * t;
    CHECK(bits(num("lerp(0.1, 0.7, 0.3)", env)) == bits(a + p));
    CHECK(num("lerp(2, 4, 0)", env) == 2.0);
    CHECK(num("lerp(2, 4, 1)", env) == 4.0);
}

TEST_CASE("hxl vm: inputs, hierarchical tags and missing inputs") {
    MapEnv env;
    env.attrs["self"]["Shield.Max"] = 200.0;
    env.fields["self"]["distance"] = 3.0;
    env.tags["self"] = {"State.Debuff.Stun", "Ship.Class.Frigate"};
    env.hasStacks = true;
    env.stacksValue = 2.0;
    CHECK(num("attr(self, Shield.Max) * self.distance + stacks()", env) == 602.0);
    CHECK(run("tag(self, State.Debuff)", env).value.asBool());
    CHECK(run("tag(self, State.Debuff.Stun)", env).value.asBool());
    CHECK_FALSE(run("tag(self, State.Debuff.Stun.Long)", env).value.asBool());
    CHECK_FALSE(run("tag(self, State.Deb)", env).value.asBool()); // prefix of a segment is no match
    CHECK_FALSE(run("tag(self, Ship.Class.Cruiser)", env).value.asBool());
    CHECK(run("attr(self, Hull)", env).status == Status::MissingInput);
    CHECK(run("self.speed", env).status == Status::MissingInput);
    CHECK(run("level()", env).status == Status::MissingInput);
    CHECK(run("curve(Nope, 1)", env).status == Status::MissingInput);
    auto missing = [&] {
        auto p = compile("attr(self, Hull)");
        REQUIRE(p.ok());
        env.bind(*p);
        return evaluate(*p, env);
    }();
    CHECK(missing.errorCode() == ErrorCode::NotFound);
}

TEST_CASE("hxl vm: select, && and || are lazy") {
    MapEnv env; // no inputs at all: evaluating attr() would fail
    CHECK(num("select(1 < 2, 5, attr(self, Missing))", env) == 5.0);
    CHECK(num("select(1 > 2, attr(self, Missing), 6)", env) == 6.0);
    CHECK(run("false && attr(self, Missing) > 0", env).value.number == 0.0);
    CHECK(run("true || attr(self, Missing) > 0", env).value.number == 1.0);
    CHECK(run("true && attr(self, Missing) > 0", env).status == Status::MissingInput);
    CHECK(run("select(false, true, false) || select(true, false, true)", env).value.number == 0.0);
}

TEST_CASE("hxl vm: curves clamp at the ends and interpolate inside") {
    Curve c{{0.0, 10.0, 20.0}, {0.0, 100.0, 50.0}};
    REQUIRE(c.validate().ok());
    CHECK(c.sample(-5.0) == 0.0);
    CHECK(c.sample(0.0) == 0.0);
    CHECK(c.sample(5.0) == 50.0);
    CHECK(c.sample(10.0) == 100.0);
    CHECK(c.sample(15.0) == 75.0);
    CHECK(c.sample(25.0) == 50.0);
    CHECK(c.sample(std::numeric_limits<f64>::infinity()) == 50.0);
    CHECK(bits(c.sample(std::numeric_limits<f64>::quiet_NaN())) == kCanonicalNaNBits);
    Curve single{{3.0}, {7.0}};
    REQUIRE(single.validate().ok());
    CHECK(single.sample(-1e300) == 7.0);
    CHECK(single.sample(1e300) == 7.0);
    CHECK_FALSE(Curve{}.validate().ok());
    CHECK_FALSE((Curve{{1.0, 1.0}, {0.0, 1.0}}).validate().ok());
    CHECK_FALSE((Curve{{0.0, 1.0}, {0.0}}).validate().ok());
    CHECK_FALSE((Curve{{0.0, std::numeric_limits<f64>::infinity()}, {0.0, 1.0}}).validate().ok());

    MapEnv env;
    env.curves["Xp.Level"] = c;
    env.hasLevel = true;
    env.levelValue = 12.5;
    CHECK(num("curve(Xp.Level, level())", env) == c.sample(12.5));
}

TEST_CASE("hxl vm: EVE turret hit chance formula (06 §1.2)") {
    const char* src =
        "formula TurretHitChance(src, tgt, ctx) =\n"
        "  0.5 ^ ( (ctx.angularVelocity * 40000 / (attr(src,TrackingSpeed) * attr(tgt,SignatureRadius)))^2\n"
        "        + (max(0, ctx.distance - attr(src,OptimalRange)) / attr(src,FalloffRange))^2 )";
    auto p = compile(src);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    MapEnv env;
    env.attrs["src"]["TrackingSpeed"] = 0.05;
    env.attrs["src"]["OptimalRange"] = 10000.0;
    env.attrs["src"]["FalloffRange"] = 5000.0;
    env.attrs["tgt"]["SignatureRadius"] = 40.0;
    env.fields["ctx"]["angularVelocity"] = 0.01;
    env.fields["ctx"]["distance"] = 15000.0;
    env.bind(*p);
    Value v;
    REQUIRE(eval(*p, env, v) == Status::Ok);
    const f64 a = 0.01 * 40000.0 / (0.05 * 40.0);
    const f64 b = (15000.0 - 10000.0) / 5000.0;
    const f64 expected = det::pow(0.5, det::pow(a, 2.0) + det::pow(b, 2.0));
    CHECK(bits(v.number) == bits(expected));
    CHECK(v.number == doctest::Approx(std::pow(0.5, a * a + b * b)).epsilon(1e-15));
}

TEST_CASE("hxl vm: a default-constructed program is rejected, not evaluated (review regression)") {
    // eval() used to read the uninitialized result slot of an empty program (UB; Go returned 0).
    const Program empty;
    MapEnv env;
    Value v = Value::num(123.0);
    CHECK(eval(empty, env, v) == Status::Bytecode);
    CHECK(v.number == 123.0); // untouched
    auto r = evaluate(empty, env);
    REQUIRE_FALSE(r.ok());
    CHECK(r.errorCode() == ErrorCode::Corrupt);
}

TEST_CASE("hxl vm: MapEnv binds invalid curves as missing inputs (review regression)") {
    // Curve::sample() requires a valid curve; MapEnv used to hand out empty or ragged curves, which
    // sample() read out of bounds (Go panicked).
    auto p = compile("curve(C, 0.5)");
    REQUIRE(p.ok());
    for (const Curve& bad : {Curve{}, Curve{{0.0, 1.0}, {1.0}}, Curve{{1.0, 0.0}, {0.0, 1.0}},
                             Curve{{0.0, std::numeric_limits<f64>::infinity()}, {0.0, 1.0}}}) {
        MapEnv env;
        env.curves["C"] = bad;
        env.bind(*p);
        Value v;
        CHECK(eval(*p, env, v) == Status::MissingInput);
    }
    MapEnv good;
    good.curves["C"] = Curve{{0.0, 1.0}, {0.0, 4.0}};
    good.bind(*p);
    Value v;
    REQUIRE(eval(*p, good, v) == Status::Ok);
    CHECK(v.number == 2.0);
}
