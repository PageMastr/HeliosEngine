// Compiled codecs (Codec<T>) for scalars, vocabulary types and containers.

#include <doctest/doctest.h>

#include "test_types.h"

using namespace helios;
using namespace helios::refl;
using namespace rtest;

namespace {
std::vector<u8> bytes(std::initializer_list<int> b) {
    std::vector<u8> out;
    for (int v : b) out.push_back(static_cast<u8>(v));
    return out;
}

template <class T>
std::vector<u8> fieldBytes(u32 id, const T& v) {
    std::vector<u8> out;
    TaggedWriter w(out);
    writeField(w, id, v);
    return out;
}

/// Reads every occurrence of field `id` back into a fresh T.
template <class T>
T readBack(const std::vector<u8>& data, u32 id) {
    T v{};
    TaggedReader r(data);
    while (!r.atEnd()) {
        auto tag = r.readTag();
        REQUIRE(tag);
        if (tag->id == id) {
            auto res = readField(r, tag->wire, v);
            REQUIRE_MESSAGE(res, (res ? std::string() : res.error().message));
        } else {
            REQUIRE(r.skip(tag->wire));
        }
    }
    return v;
}

template <class T>
void checkRoundTrip(const T& v) {
    const auto data = fieldBytes(3, v);
    const T back = readBack<T>(data, 3);
    CHECK(valuesEqual(back, v));
    ReadCtx ctx;
    T fromText{};
    const std::string text = toJson(v);
    auto res = fromJson(text, fromText, ctx);
    REQUIRE_MESSAGE(res, text);
    CHECK_MESSAGE(valuesEqual(fromText, v), text);
    CHECK(toJson(fromText) == text);
}
} // namespace

TEST_CASE("codec: scalar encodings") {
    CHECK(fieldBytes<bool>(1, true) == bytes({0x08, 0x01}));
    CHECK(fieldBytes<i32>(1, -1) == bytes({0x08, 0x01}));
    CHECK(fieldBytes<i32>(1, 1) == bytes({0x08, 0x02}));
    CHECK(fieldBytes<u32>(1, 300) == bytes({0x08, 0xac, 0x02}));
    CHECK(fieldBytes<f32>(2, 1.0f) == bytes({0x15, 0x00, 0x00, 0x80, 0x3f}));
    CHECK(fieldBytes<f64>(2, 1.0) == bytes({0x11, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f}));
    CHECK(fieldBytes<std::string>(1, "hi") == bytes({0x0a, 0x02, 'h', 'i'}));
    CHECK(fieldBytes<Name>(1, Name("hi")) == bytes({0x0a, 0x02, 'h', 'i'}));
    checkRoundTrip<i8>(-128);
    checkRoundTrip<u64>(~0ull);
    checkRoundTrip<i64>(std::numeric_limits<i64>::min());
    checkRoundTrip<f32>(-0.0f);
    checkRoundTrip<f64>(6.02214076e23);
    checkRoundTrip<std::string>("\xE2\x9C\x93 ok");
}

TEST_CASE("codec: integer range and wire type checks") {
    const auto big = fieldBytes<u32>(1, 300);
    TaggedReader r(big);
    auto tag = r.readTag();
    u8 small = 0;
    CHECK(Codec<u8>::readValue(r, tag->wire, small).errorCode() == ErrorCode::OutOfRange);

    const auto neg = fieldBytes<i32>(1, -200);
    TaggedReader r2(neg);
    tag = r2.readTag();
    i8 s8 = 0;
    CHECK(Codec<i8>::readValue(r2, tag->wire, s8).errorCode() == ErrorCode::OutOfRange);

    const auto str = fieldBytes<std::string>(1, "x");
    TaggedReader r3(str);
    tag = r3.readTag();
    i32 i = 0;
    CHECK(Codec<i32>::readValue(r3, tag->wire, i).errorCode() == ErrorCode::Corrupt);

    // f32 -> f64 widening is accepted (02 §3.4); the reverse narrowing is not.
    const auto f = fieldBytes<f32>(1, 0.25f);
    CHECK(readBack<f64>(f, 1) == 0.25);
    const auto d = fieldBytes<f64>(1, 0.25);
    TaggedReader r4(d);
    tag = r4.readTag();
    f32 narrow = 0;
    CHECK(Codec<f32>::readValue(r4, tag->wire, narrow).errorCode() == ErrorCode::Corrupt);

    // i32 -> i64 and u8 -> u32 widenings share the varint encoding.
    CHECK(readBack<i64>(fieldBytes<i32>(1, -5), 1) == -5);
    CHECK(readBack<u32>(fieldBytes<u8>(1, 250), 1) == 250u);
}

TEST_CASE("codec: vocabulary types") {
    CHECK(fieldBytes<Vec3>(1, Vec3(1, 0, 0)) == bytes({0x0a, 12, 0, 0, 0x80, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(fieldBytes<EntityId>(1, EntityId(1)) == bytes({0x09, 1, 0, 0, 0, 0, 0, 0, 0}));
    const Guid g(0x0011223344556677ull, 0x8899aabbccddeeffull);
    CHECK(fieldBytes<Guid>(1, g) == bytes({0x0a, 16, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
                                           0xbb, 0xcc, 0xdd, 0xee, 0xff}));
    checkRoundTrip(g);
    checkRoundTrip(Vec2(1.5f, -2));
    checkRoundTrip(Vec4(1, 2, 3, 4));
    checkRoundTrip(DVec3(1e13, 2, 3));
    checkRoundTrip(Quat(0, 0, 0.70710677f, 0.70710677f));
    checkRoundTrip(DQuat(0, 1, 0, 0));
    checkRoundTrip(Color(1, 0.5f, 0.25f, 0));
    checkRoundTrip(WorldPos{DVec3(-4e12, 5, 6)});
    checkRoundTrip(EntityId(0xffffffffffffffffull));
    checkRoundTrip(NetHandle::make(0xffffff, 0xff));
    checkRoundTrip(Duration(-1));
    checkRoundTrip(RecordRef<Inner>(12345));
    checkRoundTrip(AssetRef{Guid(5, 6)});
    checkRoundTrip(LocString{"a.b"});
    checkRoundTrip(TagQuery{"A | B"});
    checkRoundTrip(HxlExpr{"1 + 2"});
    TagSet tags;
    tags.add("b.c");
    tags.add("a");
    tags.add("b.c");
    CHECK(tags.size() == 2);
    CHECK(toJson(tags, JsonStyle::Compact) == "[\"a\",\"b.c\"]");
    checkRoundTrip(tags);

    CHECK(toJson(Vec3(1, 0, -1), JsonStyle::Compact) == "[1,0,-1]");
    CHECK(toJson(g) == "\"00112233-4455-6677-8899-aabbccddeeff\"\n");
    Guid parsed;
    ReadCtx ctx;
    REQUIRE(fromJson("\"guid:00112233-4455-6677-8899-aabbccddeeff\"", parsed, ctx));
    CHECK(parsed == g);

    // References use the canonical JSONC prefixes of 02 §3.7; readers accept the bare forms too.
    CHECK(toJson(AssetRef{g}, JsonStyle::Compact) == "\"guid:00112233-4455-6677-8899-aabbccddeeff\"");
    CHECK(toJson(EntityId(42), JsonStyle::Compact) == "\"ent:42\"");
    CHECK(toJson(LocString{"ui.title"}, JsonStyle::Compact) == "\"loc:ui.title\"");
    CHECK(toJson(RecordRef<Inner>(7), JsonStyle::Compact) == "7");
    AssetRef asset;
    REQUIRE(fromJson("\"00112233-4455-6677-8899-aabbccddeeff\"", asset, ctx));
    CHECK(asset.guid == g);
    EntityId e;
    REQUIRE(fromJson("\"ent:18446744073709551615\"", e, ctx));
    CHECK(e.value == 0xffffffffffffffffull);
    REQUIRE(fromJson("17", e, ctx));
    CHECK(e.value == 17u);
    CHECK_FALSE(fromJson("\"ent:x\"", e, ctx));
    CHECK_FALSE(fromJson("\"ent:\"", e, ctx));
    CHECK_FALSE(fromJson("-1", e, ctx));
    LocString loc;
    REQUIRE(fromJson("\"menu.quit\"", loc, ctx));
    CHECK(loc.key == "menu.quit");
    REQUIRE(fromJson("\"loc:menu.quit\"", loc, ctx));
    CHECK(loc.key == "menu.quit");
    // Map keys stay plain text (they are object keys, not reference values).
    std::map<EntityId, u8> byEntity{{EntityId(5), 1}};
    CHECK(toJson(byEntity, JsonStyle::Compact) == "{\"5\":1}");
}

TEST_CASE("codec: durations use the largest exact unit") {
    CHECK(formatDuration(Duration()) == "0s");
    CHECK(formatDuration(Duration::fromSeconds(30 * 86400)) == "30d");
    CHECK(formatDuration(Duration::fromMillis(500)) == "500ms");
    CHECK(formatDuration(Duration::fromMillis(1500)) == "1500ms");
    CHECK(formatDuration(Duration::fromSeconds(90)) == "90s");
    CHECK(formatDuration(Duration::fromSeconds(7200)) == "2h");
    CHECK(formatDuration(Duration(-1500)) == "-1500ns");
    CHECK(formatDuration(Duration(2000)) == "2us");
    CHECK(parseDuration("1.5s")->nanos == 1'500'000'000);
    CHECK(parseDuration("30d")->nanos == 30ll * 86'400'000'000'000);
    CHECK(parseDuration("0.5h")->nanos == 1'800'000'000'000);
    CHECK(parseDuration("-250ms")->nanos == -250'000'000);
    CHECK(parseDuration("1.23456789012s")->nanos == 1'234'567'890);
    CHECK(parseDuration(".5ms")->nanos == 500'000);
    CHECK(parseDuration("0.6ns")->nanos == 1);
    CHECK_FALSE(parseDuration("5"));
    CHECK_FALSE(parseDuration("5 s"));
    CHECK_FALSE(parseDuration("1.2.3s"));
    CHECK_FALSE(parseDuration("99999999999999999999d"));
    Duration d;
    ReadCtx ctx;
    REQUIRE(fromJson("2.5", d, ctx)); // plain numbers are seconds
    CHECK(d.nanos == 2'500'000'000);
}

TEST_CASE("codec: enums and flags use names") {
    CHECK(toJson(Tint::Green, JsonStyle::Compact) == "\"Green\"");
    CHECK(toJson(static_cast<Tint>(9), JsonStyle::Compact) == "9");
    CHECK(toJson(static_cast<Caps>(1 | 4 | 64), JsonStyle::Compact) == "[\"Fly\",\"Dig\",64]");
    CHECK(Codec<Tint>::isDefault(Tint::Red)); // first declared value, not zero
    CHECK_FALSE(Codec<Tint>::isDefault(Tint::Green));
    CHECK(Codec<Delta>::isDefault(Delta::Level));
    CHECK(fieldBytes<Delta>(1, Delta::Down) == bytes({0x08, 0x01})); // signed underlying: zigzag
    checkRoundTrip(Tint::Blue);
    checkRoundTrip(static_cast<Caps>(2 | 4));
    checkRoundTrip(Delta::Down);
    Tint t{};
    ReadCtx ctx;
    CHECK_FALSE(fromJson("\"Purple\"", t, ctx));
    CHECK(fromJson("3", t, ctx));
    CHECK(t == Tint::Blue);
    Caps c{};
    CHECK(fromJson("[\"Swim\", 1]", c, ctx));
    CHECK(static_cast<u32>(c) == 3u);
}

TEST_CASE("codec: lists are packed or repeated") {
    // Packed varints: field 1, LEN 4: 01 02 ac 02 (protobuf's packed repeated example shape).
    CHECK(fieldBytes(1, std::vector<i32>{-1, 1, 150}) == bytes({0x0a, 0x04, 0x01, 0x02, 0xac, 0x02}));
    CHECK(fieldBytes(1, std::vector<u32>{}) == bytes({}));
    CHECK(fieldBytes(1, std::vector<std::string>{"a", ""}) == bytes({0x0a, 0x01, 'a', 0x0a, 0x00}));
    CHECK(fieldBytes(1, std::vector<std::vector<u8>>{{1}, {}}) ==
          bytes({0x0a, 0x03, 0x0a, 0x01, 0x01, 0x0a, 0x00})); // wrapped elements {1: packed}
    CHECK(fieldBytes(1, std::vector<std::optional<i32>>{2, std::nullopt}) ==
          bytes({0x0a, 0x02, 0x08, 0x04, 0x0a, 0x00}));
    checkRoundTrip(std::vector<i32>{1, -2, 3});
    checkRoundTrip(std::vector<f32>{0.5f, -1});
    checkRoundTrip(std::vector<std::vector<std::string>>{{"a", "b"}, {}, {"c"}});
    checkRoundTrip(std::vector<std::optional<f64>>{1.5, std::nullopt});
    checkRoundTrip(std::vector<Inner>{Inner{1, "x", 1.5f}, Inner{}});
    checkRoundTrip(std::vector<Vec3>{Vec3(1, 2, 3)});
    CHECK(toJson(std::vector<i32>{1, 2}, JsonStyle::Pretty) == "[1, 2]\n");
    CHECK(toJson(std::vector<Vec3>{Vec3(1, 2, 3), Vec3()}) == "[\n  [1, 2, 3],\n  [0, 0, 0]\n]\n");

    // An unpacked occurrence of a packable element is accepted (protobuf compatibility).
    std::vector<u8> unpacked;
    TaggedWriter w(unpacked);
    w.writeTag(1, WireType::Varint);
    w.writeVarint(zigzagEncode(7));
    w.writeTag(1, WireType::Varint);
    w.writeVarint(zigzagEncode(-7));
    CHECK(readBack<std::vector<i32>>(unpacked, 1) == std::vector<i32>{7, -7});
}

TEST_CASE("codec: arrays keep positions") {
    const std::array<i32, 3> a{0, 5, 0};
    CHECK(fieldBytes(1, a) == bytes({0x0a, 0x03, 0x00, 0x0a, 0x00}));
    checkRoundTrip(a);
    checkRoundTrip(std::array<std::string, 2>{"", "x"});
    checkRoundTrip(std::array<Inner, 2>{Inner{}, Inner{2, "b", 1.5f}});
    // Too many elements on the wire is corrupt input.
    const auto four = fieldBytes(1, std::vector<i32>{1, 2, 3, 4});
    std::array<i32, 3> small{};
    TaggedReader r(four);
    auto tag = r.readTag();
    CHECK(readField(r, tag->wire, small).errorCode() == ErrorCode::Corrupt);
    ReadCtx ctx;
    CHECK_FALSE(fromJson("[1,2,3,4]", small, ctx));
    CHECK(fromJson("[9]", small, ctx));
    CHECK(small == std::array<i32, 3>{9, 0, 0});
}

TEST_CASE("codec: sets and maps are written in canonical key order") {
    const std::set<Name> names{Name("zeta"), Name("alpha"), Name("mid")};
    CHECK(toJson(names, JsonStyle::Compact) == "[\"alpha\",\"mid\",\"zeta\"]");
    checkRoundTrip(names);
    checkRoundTrip(std::set<i32>{5, -3});

    const std::map<u32, i32> m{{10, 1}, {2, 0}};
    // entries {1: key, 2: value}, key order 2 then 10, zero values still written inside entries
    CHECK(fieldBytes(1, m) == bytes({0x0a, 0x04, 0x08, 0x02, 0x10, 0x00, 0x0a, 0x04, 0x08, 0x0a, 0x10, 0x02}));
    CHECK(toJson(m, JsonStyle::Compact) == "{\"2\":0,\"10\":1}");
    checkRoundTrip(m);
    std::map<Name, f32> attrs{{Name("b"), 1.0f}, {Name("a"), 2.0f}};
    CHECK(toJson(attrs, JsonStyle::Compact) == "{\"a\":2,\"b\":1}");
    checkRoundTrip(attrs);
    checkRoundTrip(std::map<Tint, std::vector<i32>>{{Tint::Blue, {1}}, {Tint::Red, {}}});
    checkRoundTrip(std::map<Guid, std::optional<Inner>>{{Guid(1, 1), Inner{}}, {Guid(0, 9), std::nullopt}});
    checkRoundTrip(std::map<std::string, std::map<i64, std::string>>{{"x", {{-1, "neg"}}}});
    std::map<u32, i32> bad;
    ReadCtx ctx;
    CHECK_FALSE(fromJson("{\"abc\": 1}", bad, ctx));
}

TEST_CASE("codec: optionals encode presence") {
    CHECK(fieldBytes(1, std::optional<i32>{}) == bytes({}));
    CHECK(fieldBytes(1, std::optional<i32>{0}) == bytes({0x08, 0x00}));
    CHECK(readBack<std::optional<i32>>(bytes({0x08, 0x00}), 1) == std::optional<i32>{0});
    CHECK_FALSE(readBack<std::optional<i32>>(bytes({}), 1).has_value());
    CHECK(toJson(std::optional<i32>{}, JsonStyle::Compact) == "null");
}

TEST_CASE("codec: keyed lists carry their keys") {
    KeyedList<Mount> mounts;
    mounts.add(Guid(0, 1), Mount{Name("a"), 100.0f});
    mounts.add(Guid(0, 2), Mount{Name("b"), 5.0f});
    const std::string json = toJson(mounts);
    CHECK(json ==
          "[\n"
          "  {\n"
          "    \"$key\": \"00000000-0000-0000-0000-000000000001\",\n"
          "    \"bone\": \"a\"\n"
          "  },\n"
          "  {\n"
          "    \"$key\": \"00000000-0000-0000-0000-000000000002\",\n"
          "    \"bone\": \"b\",\n"
          "    \"force\": 5\n"
          "  }\n"
          "]\n");
    checkRoundTrip(mounts);

    KeyedList<Mount> dup;
    ReadCtx ctx;
    CHECK_FALSE(fromJson(
        "[{\"$key\": \"00000000-0000-0000-0000-000000000001\"}, {\"$key\": \"00000000-0000-0000-0000-000000000001\"}]", dup,
        ctx));
    KeyedList<Mount> minted;
    ReadCtx ctx2;
    REQUIRE(fromJson("[{\"bone\": \"x\"}]", minted, ctx2));
    CHECK(minted.size() == 1);
    CHECK_FALSE(minted.keyAt(0).isNil());
    CHECK(ctx2.warnings().size() == 1);
    CHECK_FALSE(fromJson("[{\"$key\": \"not-a-guid\"}]", minted, ctx2));
}

TEST_CASE("codec: variants") {
    CHECK(toJson(Choice{AltEmpty{}}, JsonStyle::Compact) == "\"AltEmpty\"");
    CHECK(toJson(Choice{AltValue{3, "x"}}, JsonStyle::Compact) == "{\"AltValue\":{\"x\":3,\"label\":\"x\"}}");
    CHECK(Codec<Choice>::isDefault(Choice{}));
    CHECK_FALSE(Codec<Choice>::isDefault(Choice{AltValue{}}));
    // AltValue has id 7: message {7: LEN {1: 6}}
    CHECK(fieldBytes(1, Choice{AltValue{3, ""}}) == bytes({0x0a, 0x04, 0x3a, 0x02, 0x08, 0x06}));
    checkRoundTrip(Choice{AltEmpty{}});
    checkRoundTrip(Choice{AltValue{-4, "neg"}});
    Choice c;
    ReadCtx ctx;
    CHECK(fromJson("{\"AltValue\": {\"x\": 1}}", c, ctx));
    CHECK(std::get<AltValue>(c).x == 1);
    CHECK_FALSE(fromJson("\"Nope\"", c, ctx));
    CHECK_FALSE(fromJson("{\"AltValue\": {}, \"AltEmpty\": {}}", c, ctx));
    // An alternative id unknown to this reader is skipped (tolerant of newer writers).
    std::vector<u8> future;
    TaggedWriter w(future);
    w.writeTag(1, WireType::Len);
    const usize m = w.beginLen();
    w.writeTag(99, WireType::Varint);
    w.writeVarint(1);
    w.endLen(m);
    const Choice back = readBack<Choice>(future, 1);
    CHECK(back.index() == 0);
}

TEST_CASE("codec: TypeOf for containers") {
    const TypeInfo& list = typeOf<std::vector<Inner>>();
    CHECK(list.kind == Kind::List);
    CHECK(list.qualifiedName == "list<rtest.Inner>");
    CHECK(&list.element() == &typeOf<Inner>());
    CHECK(typeOf<std::map<Name, f32>>().qualifiedName == "map<Name,f32>");
    CHECK(&typeOf<std::map<Name, f32>>().key() == &typeOf<Name>());
    CHECK(typeOf<std::optional<i32>>().qualifiedName == "i32?");
    CHECK(typeOf<std::array<f32, 3>>().qualifiedName == "f32[3]");
    CHECK(typeOf<std::array<f32, 3>>().arraySize == 3);
    CHECK(typeOf<KeyedList<Mount>>().kind == Kind::KeyedList);
    CHECK(typeOf<RecordRef<Inner>>().qualifiedName == "Ref<rtest.Inner>");
    CHECK(typeOf<std::set<i32>>().kind == Kind::Set);
    CHECK(typeOf<Vec3>().fields.size() == 3);
    CHECK(typeOf<Vec3>().hasFlag(TypeFlags::Tuple));
    CHECK(typeOf<Guid>().kind == Kind::Builtin);
    CHECK(typeOf<i32>().id == fnv1a32("i32"));
}
