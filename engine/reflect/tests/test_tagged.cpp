// Tagged wire format primitives.

#include <doctest/doctest.h>

#include "helios/core/random.h"
#include "helios/reflect/tagged.h"

using namespace helios;
using namespace helios::refl;

namespace {
std::vector<u8> bytes(std::initializer_list<int> b) {
    std::vector<u8> out;
    for (int v : b) out.push_back(static_cast<u8>(v));
    return out;
}
} // namespace

TEST_CASE("tagged: varints and zigzag match the protobuf encoding") {
    std::vector<u8> out;
    TaggedWriter w(out);
    w.writeVarint(0);
    w.writeVarint(1);
    w.writeVarint(127);
    w.writeVarint(128);
    w.writeVarint(300);
    w.writeVarint(~0ull);
    CHECK(out == bytes({0x00, 0x01, 0x7f, 0x80, 0x01, 0xac, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));

    CHECK(zigzagEncode(0) == 0);
    CHECK(zigzagEncode(-1) == 1);
    CHECK(zigzagEncode(1) == 2);
    CHECK(zigzagEncode(-2) == 3);
    CHECK(zigzagEncode(std::numeric_limits<i64>::max()) == 0xfffffffffffffffeull);
    CHECK(zigzagEncode(std::numeric_limits<i64>::min()) == 0xffffffffffffffffull);
    for (i64 v : std::initializer_list<i64>{0, 1, -1, 12345, -987654321, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max()})
        CHECK(zigzagDecode(zigzagEncode(v)) == v);

    TaggedReader r(out);
    for (u64 expect : {0ull, 1ull, 127ull, 128ull, 300ull, ~0ull}) {
        auto v = r.readVarint();
        REQUIRE(v);
        CHECK(*v == expect);
    }
    CHECK(r.atEnd());
}

TEST_CASE("tagged: tags, fixed values and length-delimited payloads") {
    std::vector<u8> out;
    TaggedWriter w(out);
    w.writeTag(1, WireType::Varint);
    w.writeVarint(150);
    w.writeTag(2, WireType::Len);
    w.writeString("testing");
    w.writeTag(3, WireType::I32);
    w.writeF32(1.0f);
    w.writeTag(4, WireType::I64);
    w.writeF64(-2.0);
    // Well-known protobuf examples: field 1 = 150 -> 08 96 01; field 2 = "testing" -> 12 07 ...
    CHECK(std::vector<u8>(out.begin(), out.begin() + 3) == bytes({0x08, 0x96, 0x01}));
    CHECK(out[3] == 0x12);
    CHECK(out[4] == 7);
    CHECK(out[12] == 0x1d); // (3 << 3) | 5
    CHECK(out[17] == 0x21); // (4 << 3) | 1

    TaggedReader r(out);
    auto t = r.readTag();
    REQUIRE(t);
    CHECK(t->id == 1);
    CHECK(t->wire == WireType::Varint);
    CHECK(*r.readVarint() == 150);
    t = r.readTag();
    CHECK(t->id == 2);
    auto s = r.readLen();
    REQUIRE(s);
    CHECK(std::string_view(reinterpret_cast<const char*>(s->data()), s->size()) == "testing");
    t = r.readTag();
    CHECK(t->wire == WireType::I32);
    CHECK(*r.readF32() == 1.0f);
    t = r.readTag();
    CHECK(t->wire == WireType::I64);
    CHECK(*r.readF64() == -2.0);
    CHECK(r.atEnd());
}

TEST_CASE("tagged: beginLen/endLen patch minimal length prefixes") {
    for (usize payload : {usize{0}, usize{1}, usize{127}, usize{128}, usize{16383}, usize{16384}, usize{300000}}) {
        std::vector<u8> out;
        TaggedWriter w(out);
        w.writeVarint(0xAB); // prefix byte(s) to make sure the marker is relative
        const usize m = w.beginLen();
        for (usize i = 0; i < payload; ++i) out.push_back(static_cast<u8>(i));
        w.endLen(m);
        std::vector<u8> expect;
        TaggedWriter e(expect);
        e.writeVarint(0xAB);
        std::vector<u8> body(payload);
        for (usize i = 0; i < payload; ++i) body[i] = static_cast<u8>(i);
        e.writeLenBytes(body.data(), body.size());
        CHECK(out == expect);
    }
}

TEST_CASE("tagged: nested beginLen regions") {
    std::vector<u8> out;
    TaggedWriter w(out);
    const usize outer = w.beginLen();
    w.writeTag(1, WireType::Len);
    const usize inner = w.beginLen();
    for (int i = 0; i < 200; ++i) w.writeVarint(1);
    w.endLen(inner);
    w.endLen(outer);
    TaggedReader r(out);
    auto msg = r.readMessage();
    REQUIRE(msg);
    auto tag = msg->readTag();
    REQUIRE(tag);
    auto body = msg->readLen();
    REQUIRE(body);
    CHECK(body->size() == 200);
    CHECK(msg->atEnd());
    CHECK(r.atEnd());
}

TEST_CASE("tagged: skip unknown fields of every wire type") {
    std::vector<u8> out;
    TaggedWriter w(out);
    w.writeTag(7, WireType::Varint);
    w.writeVarint(1ull << 40);
    w.writeTag(8, WireType::I64);
    w.writeFixed64(5);
    w.writeTag(9, WireType::Len);
    w.writeString("skip me");
    w.writeTag(10, WireType::I32);
    w.writeFixed32(9);
    w.writeTag(1, WireType::Varint);
    w.writeVarint(42);
    TaggedReader r(out);
    for (int i = 0; i < 4; ++i) {
        auto t = r.readTag();
        REQUIRE(t);
        REQUIRE(r.skip(t->wire));
    }
    auto t = r.readTag();
    REQUIRE(t);
    CHECK(t->id == 1);
    CHECK(*r.readVarint() == 42);
}

TEST_CASE("tagged: malformed input fails cleanly") {
    SUBCASE("truncated varint") {
        const auto b = bytes({0x80, 0x80});
        TaggedReader r(b);
        CHECK(r.readVarint().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("varint longer than 64 bits") {
        const auto b = bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02});
        TaggedReader r(b);
        CHECK(r.readVarint().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("eleven-byte varint") {
        const auto b = bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01});
        TaggedReader r(b);
        CHECK(r.readVarint().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("field number zero") {
        const auto b = bytes({0x00});
        TaggedReader r(b);
        CHECK(r.readTag().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("group wire types are rejected") {
        const auto b = bytes({0x0b}); // field 1, wire type 3 (SGROUP)
        TaggedReader r(b);
        CHECK(r.readTag().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("length beyond the buffer") {
        const auto b = bytes({0x05, 'a', 'b'});
        TaggedReader r(b);
        CHECK(r.readLen().errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("truncated fixed values") {
        const auto b = bytes({1, 2, 3});
        TaggedReader r(b);
        CHECK(r.readFixed32().errorCode() == ErrorCode::Corrupt);
        CHECK(r.readFixed64().errorCode() == ErrorCode::Corrupt);
        CHECK(r.skip(WireType::I32).errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("nesting limit") {
        std::vector<u8> out;
        TaggedWriter w(out);
        std::vector<usize> marks;
        for (u32 i = 0; i < kMaxTaggedDepth + 2; ++i) marks.push_back(w.beginLen());
        for (auto it = marks.rbegin(); it != marks.rend(); ++it) w.endLen(*it);
        TaggedReader r(out);
        Result<void> status;
        for (u32 i = 0; i < kMaxTaggedDepth + 2; ++i) {
            auto m = r.readMessage();
            if (!m) {
                status = Error(m.error());
                break;
            }
            r = *m;
        }
        CHECK(status.errorCode() == ErrorCode::LimitExceeded);
    }
}

TEST_CASE("tagged: random bytes never crash the reader") {
    Random rng(1234);
    for (int iter = 0; iter < 2000; ++iter) {
        std::vector<u8> data(rng.range<usize>(0, 64));
        for (u8& b : data) b = static_cast<u8>(rng.nextU32());
        TaggedReader r(data);
        for (int step = 0; step < 64 && !r.atEnd(); ++step) {
            auto t = r.readTag();
            if (!t) break;
            if (!r.skip(t->wire)) break;
        }
        CHECK(r.position() <= data.size());
    }
}
