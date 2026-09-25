#include <doctest/doctest.h>

#include <format>
#include <set>
#include <string>
#include <unordered_set>

#include "helios/core/guid.h"

using namespace helios;

TEST_CASE("guid: generation is random version 4") {
    std::unordered_set<Guid> seen;
    for (int i = 0; i < 10000; ++i) {
        const Guid g = Guid::generate();
        CHECK(!g.isNil());
        CHECK(g.version() == 4);
        CHECK(((g.low >> 62) & 0x3) == 0x2); // RFC variant bits 10
        seen.insert(g);
    }
    CHECK(seen.size() == 10000);
    u8 buf[64] = {};
    CHECK(secureRandomBytes(buf, sizeof(buf)));
    int zeros = 0;
    for (u8 b : buf) zeros += b == 0 ? 1 : 0;
    CHECK(zeros < 16);
}

TEST_CASE("guid: canonical text format and parsing") {
    const auto parsed = Guid::parse("123e4567-e89b-12d3-a456-426614174000");
    REQUIRE(parsed);
    CHECK(parsed->high == 0x123e4567e89b12d3ull);
    CHECK(parsed->low == 0xa456426614174000ull);
    CHECK(parsed->toString() == "123e4567-e89b-12d3-a456-426614174000");
    CHECK(Guid::parse("{123E4567-E89B-12D3-A456-426614174000}").value() == *parsed);
    CHECK(Guid::parse("123e4567e89b12d3a456426614174000").value() == *parsed);
    CHECK(std::format("{}", *parsed) == "123e4567-e89b-12d3-a456-426614174000");

    for (int i = 0; i < 100; ++i) {
        const Guid g = Guid::generate();
        const std::string text = g.toString();
        CHECK(text.size() == 36);
        CHECK(text[8] == '-');
        CHECK(text[14] == '4');
        CHECK(Guid::parse(text).value() == g);
    }

    CHECK(Guid::parse("").error().code == ErrorCode::ParseError);
    CHECK(!Guid::parse("123e4567-e89b-12d3-a456-42661417400"));  // too short
    CHECK(!Guid::parse("123e4567+e89b-12d3-a456-426614174000")); // bad separator
    CHECK(!Guid::parse("123e4567-e89b-12d3-a456-42661417400g")); // bad digit
    CHECK(Guid::nil().toString() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("guid: bytes, ordering and hashing") {
    std::array<u8, 16> bytes{};
    for (u8 i = 0; i < 16; ++i) bytes[i] = static_cast<u8>(i * 17);
    const Guid g = Guid::fromBytes(bytes);
    CHECK(g.toBytes() == bytes);
    CHECK(g.toString() == "00112233-4455-6677-8899-aabbccddeeff");

    // Ordering agrees with the canonical text ordering.
    std::set<Guid> byValue;
    std::set<std::string> byText;
    for (int i = 0; i < 200; ++i) {
        const Guid x = Guid::generate();
        byValue.insert(x);
        byText.insert(x.toString());
    }
    auto it = byText.begin();
    for (const Guid& x : byValue) CHECK(x.toString() == *it++);
    CHECK(Guid(1, 0) > Guid(0, ~0ull));
    CHECK(g.hash() == std::hash<Guid>{}(g));
    CHECK(Guid::generate() != Guid::generate());
}
