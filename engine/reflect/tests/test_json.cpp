// Canonical JSON writer, float formatting and the JSONC reader.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "helios/core/random.h"
#include "helios/reflect/json.h"

using namespace helios;
using namespace helios::refl;

namespace {
std::string f64s(f64 v) {
    char buf[48];
    return std::string(buf, formatJsonF64(v, buf));
}
std::string f32s(f32 v) {
    char buf[48];
    return std::string(buf, formatJsonF32(v, buf));
}
JsonDocument parse(std::string_view text) {
    auto d = JsonDocument::parse(text);
    REQUIRE_MESSAGE(d, (d ? std::string() : d.error().message));
    return std::move(*d);
}
} // namespace

TEST_CASE("json: shortest round-trip float formatting in JavaScript notation") {
    CHECK(f64s(0.0) == "0");
    CHECK(f64s(-0.0) == "-0.0");
    CHECK(f64s(1.0) == "1");
    CHECK(f64s(-1.5) == "-1.5");
    CHECK(f64s(0.1) == "0.1");
    CHECK(f64s(1.0 / 3.0) == "0.3333333333333333");
    CHECK(f64s(50000.0) == "50000");
    CHECK(f64s(1e8) == "100000000");
    CHECK(f64s(1e20) == "100000000000000000000");
    CHECK(f64s(1e21) == "1e+21");
    CHECK(f64s(1.5e300) == "1.5e+300");
    CHECK(f64s(1e-6) == "0.000001");
    CHECK(f64s(1.5e-7) == "1.5e-7");
    CHECK(f64s(5e-324) == "5e-324");
    CHECK(f64s(1.7976931348623157e308) == "1.7976931348623157e+308");
    CHECK(f64s(123456.789) == "123456.789");
    CHECK(f64s(std::numeric_limits<f64>::quiet_NaN()) == "nan");
    CHECK(f64s(std::numeric_limits<f64>::infinity()) == "inf");
    CHECK(f64s(-std::numeric_limits<f64>::infinity()) == "-inf");

    CHECK(f32s(0.1f) == "0.1");
    CHECK(f32s(1.5f) == "1.5");
    CHECK(f32s(16777216.0f) == "16777216");
    CHECK(f32s(3.4028235e38f) == "3.4028235e+38");
    CHECK(f32s(1e-7f) == "1e-7");
    CHECK(f32s(0.70710677f) == "0.70710677");
    CHECK(f32s(-0.0f) == "-0.0");
}

TEST_CASE("json: formatted floats parse back to identical bits") {
    Random rng(42);
    auto roundTrip64 = [](f64 v) {
        char buf[48];
        const std::string text(buf, formatJsonF64(v, buf));
        const JsonDocument d = parse("[" + (std::isfinite(v) ? text : "\"" + text + "\"") + "]");
        f64 back = 0;
        REQUIRE((*d.root().elements().begin()).getF64(back));
        if (std::isnan(v)) {
            CHECK(std::isnan(back));
        } else {
            CHECK(std::bit_cast<u64>(back) == std::bit_cast<u64>(v));
        }
    };
    auto roundTrip32 = [](f32 v) {
        char buf[48];
        const std::string text(buf, formatJsonF32(v, buf));
        const JsonDocument d = parse("[" + text + "]");
        f32 back = 0;
        REQUIRE((*d.root().elements().begin()).getF32(back));
        CHECK(std::bit_cast<u32>(back) == std::bit_cast<u32>(v));
    };
    for (int i = 0; i < 5000; ++i) {
        const f64 d = std::bit_cast<f64>(rng.nextU64());
        if (std::isfinite(d)) roundTrip64(d);
        const f32 f = std::bit_cast<f32>(rng.nextU32());
        if (std::isfinite(f)) roundTrip32(f);
        roundTrip64(rng.range(-1e6, 1e6));
        roundTrip32(rng.range(-1e3f, 1e3f));
    }
    roundTrip64(std::numeric_limits<f64>::denorm_min());
    roundTrip64(std::numeric_limits<f64>::max());
    roundTrip64(-0.0);
    roundTrip32(std::numeric_limits<f32>::denorm_min());
    roundTrip32(std::numeric_limits<f32>::max());
    roundTrip64(std::numeric_limits<f64>::quiet_NaN());
}

TEST_CASE("json: canonical pretty layout") {
    JsonWriter w;
    w.beginObject();
    w.key("a");
    w.integer(1);
    w.key("list");
    w.beginArray(true);
    w.number(0.5);
    w.integer(-2);
    w.string("x");
    w.endArray();
    w.key("objs");
    w.beginArray();
    w.beginObject();
    w.key("k");
    w.boolean(true);
    w.endObject();
    w.beginObject();
    w.endObject();
    w.endArray();
    w.key("empty");
    w.beginArray();
    w.endArray();
    w.key("nested");
    w.beginObject();
    w.key("n");
    w.null();
    w.endObject();
    w.endObject();
    CHECK(w.take() ==
          "{\n"
          "  \"a\": 1,\n"
          "  \"list\": [0.5, -2, \"x\"],\n"
          "  \"objs\": [\n"
          "    {\n"
          "      \"k\": true\n"
          "    },\n"
          "    {}\n"
          "  ],\n"
          "  \"empty\": [],\n"
          "  \"nested\": {\n"
          "    \"n\": null\n"
          "  }\n"
          "}\n");
}

TEST_CASE("json: compact layout, escaping and raw values") {
    JsonWriter w(JsonStyle::Compact);
    w.beginObject();
    w.key("s");
    w.string("q\"b\\n\n\t\x01\x7f\xC3\xA9");
    w.key("r");
    w.raw("{\"x\":[1,2]}");
    w.key("u");
    w.unsignedInteger(18446744073709551615ull);
    w.key("i");
    w.integer(std::numeric_limits<i64>::min());
    w.key("nan");
    w.number(std::numeric_limits<f64>::quiet_NaN());
    w.endObject();
    CHECK(w.take() ==
          "{\"s\":\"q\\\"b\\\\n\\n\\t\\u0001\x7f\xC3\xA9\",\"r\":{\"x\":[1,2]},\"u\":18446744073709551615,"
          "\"i\":-9223372036854775808,\"nan\":\"nan\"}");
}

TEST_CASE("json: JSONC reader accepts comments, trailing commas and a BOM") {
    const JsonDocument d = parse(
        "\xEF\xBB\xBF// header\n"
        "{\n"
        "  /* block */ \"a\": 1, // trailing\n"
        "  \"b\": [1, 2,],\n"
        "  \"c\": {\"d\": \"e\",},\n"
        "}\n");
    const JsonValue root = d.root();
    REQUIRE(root.isObject());
    CHECK(root.size() == 3);
    u64 a = 0;
    CHECK(root.get("a").getU64(a));
    CHECK(a == 1);
    CHECK(root.get("b").size() == 2);
    CHECK(root.get("c").get("d").asString() == "e");
    CHECK_FALSE(root.get("missing").isValid());
    std::vector<std::string_view> keys;
    for (const JsonMember m : root.members()) keys.push_back(m.key);
    CHECK(keys == std::vector<std::string_view>{"a", "b", "c"});
}

TEST_CASE("json: parse errors carry line and column") {
    auto d = JsonDocument::parse("{\n  \"a\": 1,\n  \"b\": ]\n}", "ship.hrec");
    REQUIRE_FALSE(d);
    CHECK(d.error().code == ErrorCode::ParseError);
    CHECK(d.error().message.starts_with("ship.hrec:3:"));
}

TEST_CASE("json: exact integer conversions") {
    const JsonDocument d = parse("[18446744073709551615, -9223372036854775808, 9223372036854775808, 1.0, 1e3, -1, 0.5]");
    std::vector<JsonValue> v;
    for (JsonValue e : d.root().elements()) v.push_back(e);
    u64 u = 0;
    i64 i = 0;
    CHECK(v[0].getU64(u));
    CHECK(u == std::numeric_limits<u64>::max());
    CHECK_FALSE(v[0].getI64(i));
    CHECK(v[1].getI64(i));
    CHECK(i == std::numeric_limits<i64>::min());
    CHECK_FALSE(v[2].getI64(i));
    CHECK_FALSE(v[3].getU64(u)); // "1.0" is not an integer literal
    CHECK_FALSE(v[4].getU64(u));
    CHECK_FALSE(v[5].getU64(u));
    CHECK(v[5].getI64(i));
    f64 f = 0;
    CHECK(v[6].getF64(f));
    CHECK(f == 0.5);
    CHECK(v[4].getF64(f));
    CHECK(f == 1000.0);
}

TEST_CASE("json: non-finite floats as strings and literals") {
    const JsonDocument d = parse("[\"nan\", \"inf\", \"-inf\", NaN, -Infinity, \"other\"]");
    std::vector<JsonValue> v;
    for (JsonValue e : d.root().elements()) v.push_back(e);
    f64 x = 0;
    CHECK(v[0].getF64(x));
    CHECK(std::isnan(x));
    CHECK(v[1].getF64(x));
    CHECK(x == std::numeric_limits<f64>::infinity());
    CHECK(v[2].getF64(x));
    CHECK(x == -std::numeric_limits<f64>::infinity());
    CHECK(v[3].getF64(x));
    CHECK(std::isnan(x));
    CHECK(v[4].getF64(x));
    CHECK(x == -std::numeric_limits<f64>::infinity());
    CHECK_FALSE(v[5].getF64(x));
}

TEST_CASE("json: copy re-renders parsed values exactly") {
    const JsonDocument d = parse("{\"a\": [1.50, -0, 1e+3], \"b\": {\"c\": null, \"d\": [true, {\"e\": \"f\"}]}}");
    JsonWriter w(JsonStyle::Compact);
    w.copy(d.root());
    CHECK(w.take() == "{\"a\":[1.50,-0,1e+3],\"b\":{\"c\":null,\"d\":[true,{\"e\":\"f\"}]}}");
}

TEST_CASE("json: ReadCtx paths, warnings and strict mode") {
    ReadCtx ctx;
    {
        ReadCtx::Scope a(ctx, "thrusters");
        ReadCtx::Scope b(ctx, usize{2});
        ReadCtx::Scope c(ctx, "maxForce");
        CHECK(ctx.path() == "thrusters[2].maxForce");
        const Error e = ctx.error("bad");
        CHECK(e.code == ErrorCode::ParseError);
        CHECK(e.message == "thrusters[2].maxForce: bad");
        CHECK(ctx.unknownField("x"));
    }
    CHECK(ctx.path().empty());
    REQUIRE(ctx.warnings().size() == 1);
    CHECK(ctx.warnings()[0] == "thrusters[2].maxForce: unknown field ignored");

    ReadCtx strict(ReadCtx::Options{.strictUnknownFields = true});
    ReadCtx::Scope s(strict, "zzz");
    CHECK_FALSE(strict.unknownField("zzz"));
}
