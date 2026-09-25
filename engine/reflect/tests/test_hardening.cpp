// Hostile and edge-case input (review regressions): JSON nesting limits, recursive types, invalid
// UTF-8 in tagged data, map keys that need escaping in property paths, and canonical key lookup in
// @keyed(field) lists.

#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "helios/reflect/reflect.h"

namespace htest {
using namespace helios;
using namespace helios::refl;

/// Recursive through a list (like golden.all.Late): JSON nesting is bounded only by the input.
struct Tree {
    u8 n = 0;
    std::vector<Tree> kids;
};

struct Slot {
    u32 id = 0;
    f32 v = 0;
};

struct Holder {
    std::vector<Slot> slots; // @keyed(id)
    std::map<std::string, i32> byName;
    std::string text;
    Name name;
    TagSet tags;
    i32 count = 0; // @was("oldCount")
};
} // namespace htest

HELIOS_REFLECT_TYPE(htest::Tree);
HELIOS_REFLECT_TYPE(htest::Slot);
HELIOS_REFLECT_TYPE(htest::Holder);

using namespace helios;
using namespace helios::refl;
using namespace htest;

const TypeInfo& helios::refl::TypeOf<Tree>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Tree>("htest.Tree").field("n", &Tree::n).field("kids", &Tree::kids).build();
    return *info;
}

const TypeInfo& helios::refl::TypeOf<Slot>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Slot>("htest.Slot").field("id", &Slot::id).field("v", &Slot::v).build();
    return *info;
}

const TypeInfo& helios::refl::TypeOf<Holder>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Holder>("htest.Holder")
                                      .field("slots", &Holder::slots, {.keyedBy = "id"})
                                      .field("byName", &Holder::byName)
                                      .field("text", &Holder::text)
                                      .field("name", &Holder::name)
                                      .field("tags", &Holder::tags)
                                      .field("count", &Holder::count, {.was = {"oldCount"}})
                                      .build();
    return *info;
}

namespace {

std::string nestedArrays(usize depth) { return std::string(depth, '[') + std::string(depth, ']'); }

/// {"n":1,"kids":[{"n":1,"kids":[ ... ]}]} nested `depth` trees deep.
std::string nestedTrees(usize depth) {
    std::string out;
    for (usize i = 0; i < depth; ++i) out += "{\"n\":1,\"kids\":[";
    for (usize i = 0; i < depth; ++i) out += "]}";
    return out;
}

} // namespace

TEST_CASE("hardening: JSON nesting is limited at parse time (no stack overflow)") {
    CHECK(JsonDocument::parse(nestedArrays(kMaxJsonDepth)));
    auto deep = JsonDocument::parse(nestedArrays(kMaxJsonDepth + 1));
    REQUIRE_FALSE(deep);
    CHECK(deep.error().code == ErrorCode::LimitExceeded);
    // Far deeper than any stack: rejected quickly and without recursion.
    auto huge = JsonDocument::parse(nestedArrays(1'000'000));
    REQUIRE_FALSE(huge);
    CHECK(huge.error().code == ErrorCode::LimitExceeded);
    // Objects count too; wide documents are fine.
    std::string wide = "[";
    for (int i = 0; i < 10000; ++i) wide += (i ? ",{\"a\":[1,{\"b\":2}]}" : "{\"a\":[1,{\"b\":2}]}");
    wide += "]";
    CHECK(JsonDocument::parse(wide));
    CHECK_FALSE(JsonDocument::parse(std::string(kMaxJsonDepth + 1, '[') + "{\"k\":1}" + std::string(kMaxJsonDepth + 1, ']')));
    // Patches copy their values recursively: a hostile patch file is rejected, not a crash.
    const std::string patch = "[{\"op\":\"set\",\"path\":\"a\",\"value\":" + nestedArrays(500'000) + "}]";
    CHECK(Patch::fromJson(patch).errorCode() == ErrorCode::LimitExceeded);
}

TEST_CASE("hardening: recursive types read deep but bounded JSON and reject deeper input") {
    const TypeInfo& t = typeOf<Tree>();
    Value v(t);
    ReadCtx ctx;
    REQUIRE(fromJson(t, v.data(), nestedTrees(40), ctx));
    const Tree& tree = *static_cast<const Tree*>(v.data());
    const Tree* cur = &tree;
    usize depth = 1;
    while (!cur->kids.empty()) {
        cur = &cur->kids[0];
        ++depth;
    }
    CHECK(depth == 40);
    // Round trip through both formats.
    const std::string text = toJson(t, v.data());
    Value back(t);
    REQUIRE(fromJson(t, back.data(), text, ctx));
    CHECK(equals(t, v.data(), back.data()));
    std::vector<u8> bytes;
    encodeTagged(t, v.data(), bytes);
    Value fromBytes(t);
    REQUIRE(decodeTagged(t, fromBytes.data(), bytes));
    CHECK(equals(t, v.data(), fromBytes.data()));

    Value hostile(t);
    CHECK(fromJson(t, hostile.data(), nestedTrees(200'000), ctx).errorCode() == ErrorCode::LimitExceeded);
    CHECK(fromJson(t, hostile.data(), nestedTrees(kMaxJsonDepth), ctx).errorCode() == ErrorCode::LimitExceeded);
}

TEST_CASE("hardening: tagged strings, Names and tags must be valid UTF-8") {
    const TypeInfo& t = typeOf<Holder>();
    Holder h;
    h.text = "ok \xC3\xA9 \xF0\x9F\x9A\x80";
    h.name = Name("n\xC3\xA9");
    h.tags.add("Item.Laser");
    std::vector<u8> good;
    encodeTagged(t, &h, good);
    Holder back;
    REQUIRE(decodeTagged(t, &back, good));
    CHECK(back.text == h.text);
    CHECK(back.name == h.name);

    const u32 textId = t.field("text")->id;
    const u32 nameId = t.field("name")->id;
    const u32 tagsId = t.field("tags")->id;
    for (const std::string& bad : {std::string("\xFF"), std::string("a\xC3"), std::string("\xED\xA0\x80"), std::string("\xC0\xAF")}) {
        INFO(bad.size());
        for (const u32 id : {textId, nameId}) {
            std::vector<u8> bytes;
            TaggedWriter w(bytes);
            w.writeTag(id, WireType::Len);
            w.writeString(bad);
            Holder out;
            CHECK(decodeTagged(t, &out, bytes).errorCode() == ErrorCode::Corrupt);
            // The compiled std::string / Name codecs agree with the walker.
            TaggedReader r(bytes);
            REQUIRE(r.readTag());
            std::string s;
            Name n;
            TaggedReader r2(bytes);
            REQUIRE(r2.readTag());
            if (id == textId) {
                CHECK(Codec<std::string>::readValue(r, WireType::Len, s).errorCode() == ErrorCode::Corrupt);
            } else {
                CHECK(Codec<Name>::readValue(r2, WireType::Len, n).errorCode() == ErrorCode::Corrupt);
            }
        }
        std::vector<u8> tagBytes;
        TaggedWriter w(tagBytes);
        w.writeTag(tagsId, WireType::Len);
        const usize m = w.beginLen();
        w.writeTag(1, WireType::Len);
        w.writeString(bad);
        w.endLen(m);
        Holder out;
        CHECK(decodeTagged(t, &out, tagBytes).errorCode() == ErrorCode::Corrupt);
    }
}

TEST_CASE("hardening: map keys that need escaping survive paths, diff and patch JSON") {
    const TypeInfo& t = typeOf<Holder>();
    for (const std::string key : {"#hash", "", "#", "a]b", "\\", "12", "#00ff", "x/y.z[0]"}) {
        INFO("key '" << key << "'");
        PropertyPath p;
        p.field("byName").key(key);
        auto reparsed = PropertyPath::parse(p.toString());
        REQUIRE_MESSAGE(reparsed, p.toString());
        const PathSegment& seg = reparsed->back();
        CHECK(seg.text == key);
        CHECK(seg.kind != PathSegment::Kind::Keyed);

        Holder before;
        before.byName["keep"] = 1;
        before.byName[key == "keep" ? "other" : key] = 5;
        Holder after = before;
        after.byName[key] = 6;
        after.byName["new"] = 2;
        for (const Holder* target : {&after}) {
            const Patch patch = diff(t, &before, target);
            auto viaJson = Patch::fromJson(patch.toJson());
            REQUIRE(viaJson);
            Holder applied = before;
            auto r = apply(t, &applied, *viaJson);
            REQUIRE_MESSAGE(r, (r ? std::string() : r.error().message + "\n" + patch.toJson()));
            CHECK(equals(t, &applied, target));
        }
        // Removal of the key.
        Holder removed = before;
        removed.byName.erase(key);
        const Patch rm = diff(t, &before, &removed);
        Holder applied = before;
        REQUIRE(apply(t, &applied, *Patch::fromJson(rm.toJson())));
        CHECK(equals(t, &applied, &removed));
    }
    // "[\#x]" is a map key, "[#x]" a keyed-list key.
    auto escaped = PropertyPath::parse("byName[\\#x]");
    REQUIRE(escaped);
    CHECK(escaped->back().kind == PathSegment::Kind::Key);
    CHECK(escaped->back().text == "#x");
    auto keyed = PropertyPath::parse("byName[#x]");
    REQUIRE(keyed);
    CHECK(keyed->back().kind == PathSegment::Kind::Keyed);
    auto empty = PropertyPath::parse("byName[]");
    REQUIRE(empty);
    CHECK(empty->back().kind == PathSegment::Kind::Key);
    CHECK(empty->back().text.empty());
}

TEST_CASE("hardening: @keyed(field) paths compare canonical key text") {
    const TypeInfo& t = typeOf<Holder>();
    Holder h;
    h.slots = {Slot{1, 0.5f}, Slot{20, 1.5f}};
    CHECK(*getJson(t, &h, "slots[#01]/v") == "0.5");
    // Writing through a non-canonical spelling updates the existing element (no duplicate key).
    REQUIRE(setJson(t, &h, "slots[#020]", "{\"v\": 7}"));
    REQUIRE(h.slots.size() == 2);
    CHECK(h.slots[1].id == 20);
    CHECK(h.slots[1].v == 7.0f);
    REQUIRE(setJson(t, &h, "slots[#3]/v", "9"));
    REQUIRE(h.slots.size() == 3);
    CHECK(h.slots[2].id == 3);
    CHECK(h.slots[2].v == 9.0f);
    CHECK(getJson(t, &h, "slots[#4]").errorCode() == ErrorCode::NotFound);
}

TEST_CASE("hardening: the current field name wins over an @was alias in any member order") {
    const TypeInfo& t = typeOf<Holder>();
    for (const char* text : {R"({"count": 9, "oldCount": 7})", R"({"oldCount": 7, "count": 9})"}) {
        INFO(text);
        Value v(t);
        ReadCtx ctx;
        REQUIRE(fromJson(t, v.data(), text, ctx));
        CHECK(static_cast<const Holder*>(v.data())->count == 9);
    }
    Value old(t);
    ReadCtx ctx;
    REQUIRE(fromJson(t, old.data(), R"({"oldCount": 7})", ctx));
    CHECK(static_cast<const Holder*>(old.data())->count == 7);
    CHECK(ctx.warnings().empty());
}

TEST_CASE("hardening: null reads as an empty container (Go writes nil slices and maps as null)") {
    const TypeInfo& t = typeOf<Holder>();
    Holder h;
    h.slots = {Slot{1, 2.0f}};
    h.byName["a"] = 1;
    h.tags.add("X.Y");
    ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
    REQUIRE(fromJson(t, &h, R"({"slots": null, "byName": null, "tags": null})", ctx));
    CHECK(h.slots.empty());
    CHECK(h.byName.empty());
    CHECK(h.tags.empty());
    // Compiled container codecs agree with the walker.
    std::vector<i32> list{1};
    std::map<std::string, i32> map{{"a", 1}};
    std::set<i32> set{1};
    CHECK(fromJson("null", list, ctx));
    CHECK(fromJson("null", map, ctx));
    CHECK(fromJson("null", set, ctx));
    CHECK(list.empty());
    CHECK(map.empty());
    CHECK(set.empty());
    // Scalars and structs still reject null.
    i32 n = 0;
    CHECK_FALSE(fromJson("null", n, ctx));
    CHECK_FALSE(fromJson(t, &h, "null", ctx));
}
