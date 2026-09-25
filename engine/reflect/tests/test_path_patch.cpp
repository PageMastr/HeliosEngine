// Property paths, diff and patch.

#include <doctest/doctest.h>

#include "helios/core/random.h"
#include "test_types.h"

using namespace helios;
using namespace helios::refl;
using namespace rtest;

namespace {
std::string parsed(std::string_view text) {
    auto p = PropertyPath::parse(text);
    REQUIRE_MESSAGE(p, text);
    return p->toString();
}

std::string keyHex(const KeyedList<Mount>& list, usize i) { return keyedKeyText(list.keyAt(i)); }

void checkDiffApply(const Everything& before, const Everything& after) {
    const TypeInfo& t = typeOf<Everything>();
    const Patch patch = diff(t, &before, &after);
    Everything applied = before;
    auto res = apply(t, &applied, patch);
    REQUIRE_MESSAGE(res, (res ? std::string() : res.error().message + "\n" + patch.toJson()));
    CHECK_MESSAGE(equals(t, &applied, &after), patch.toJson());
    // The patch survives a JSON round trip.
    auto reparsed = Patch::fromJson(patch.toJson());
    REQUIRE(reparsed);
    CHECK(*reparsed == patch);
    CHECK(diff(t, &after, &after).empty());
}
} // namespace

TEST_CASE("path: parse and canonical formatting") {
    CHECK(parsed("Transform/position") == "Transform/position");
    CHECK(parsed("components.Health.max") == "components/Health/max");
    CHECK(parsed("entries[#b21c]/weight") == "entries[#b21c]/weight");
    CHECK(parsed("baseAttrs[Ship.MaxLinearSpeed]") == "baseAttrs[Ship.MaxLinearSpeed]");
    CHECK(parsed("a[3][4]/b") == "a[3][4]/b");
    CHECK(parsed("m[a\\]b\\\\c]") == "m[a\\]b\\\\c]");
    CHECK(parsed("") == "");
    auto p = PropertyPath::parse("m[a\\]b]");
    REQUIRE(p);
    CHECK(p->segments()[1].kind == PathSegment::Kind::Key);
    CHECK(p->segments()[1].text == "a]b");
    auto idx = PropertyPath::parse("list[12]");
    REQUIRE(idx);
    CHECK(idx->back().kind == PathSegment::Kind::Index);
    CHECK(idx->back().index == 12);
    CHECK(idx->parent().toString() == "list");
    PropertyPath built;
    built.field("a").index(2).key("k").keyed("ff").field("b");
    CHECK(built.toString() == "a[2][k][#ff]/b");

    CHECK(parsed("a[]") == "a[]"); // the empty map key
    CHECK(parsed("m[\\#x]") == "m[\\#x]"); // a map key starting with '#'
    for (const char* bad : {"a//b", "a/", "/a", "a[", "a[#]", "a]b", "a/[1]", "a.", ".a", "a[1]b"}) {
        auto r = PropertyPath::parse(bad);
        CHECK_MESSAGE(!r, bad);
        if (!r) CHECK(r.error().code == ErrorCode::ParseError);
    }
}

TEST_CASE("path: resolve and read values") {
    const Everything e = makeFull();
    const TypeInfo& t = typeOf<Everything>();
    CHECK(*getJson(t, &e, "inner/a") == "7");
    CHECK(*getJson(t, &e, "inners[0]/s") == "\"x\"");
    CHECK(*getJson(t, &e, "attrs[Ship.Speed]") == "12.5");
    CHECK(*getJson(t, &e, "byId[10]/s") == "\"ten\"");
    CHECK(*getJson(t, &e, "byTint[Green][1]") == "2");
    CHECK(*getJson(t, &e, "v3/y") == "-2.5");
    CHECK(*getJson(t, &e, "choice/AltValue/label") == "\"nine\"");
    CHECK(*getJson(t, &e, "optInner/f") == "1.5");
    CHECK(*getJson(t, &e, "namedMounts[#right]/force") == "2");
    CHECK(*getJson(t, &e, "mounts[#" + keyHex(e.mounts, 1) + "]/bone") == "\"bone_b\"");
    // Short key prefixes resolve when unique.
    CHECK(*getJson(t, &e, "mounts[#" + keyHex(e.mounts, 0).substr(0, 20) + "]/force") == "5");
    CHECK(*getJson(t, &e, "mounts") == toJson(typeOf<KeyedList<Mount>>(), &e.mounts, JsonStyle::Compact));

    Everything m = e;
    auto speed = getAs<f32>(t, &m, "attrs[Ship.Speed]");
    REQUIRE(speed);
    **speed = 99.0f;
    CHECK(m.attrs[Name("Ship.Speed")] == 99.0f);
    CHECK(getAs<i32>(t, &m, "attrs[Ship.Speed]").errorCode() == ErrorCode::InvalidArgument);

    EntityDoc doc;
    doc.components.Health = HealthComp{50.0f, 150.0f};
    CHECK(*getJson(typeOf<EntityDoc>(), &doc, "components.Health.max") == "150");
}

TEST_CASE("path: resolution errors") {
    Everything e = makeFull();
    e.optInner.reset();
    const TypeInfo& t = typeOf<Everything>();
    CHECK(getJson(t, &e, "nope").errorCode() == ErrorCode::NotFound);
    CHECK(getJson(t, &e, "inners[5]").errorCode() == ErrorCode::OutOfRange);
    CHECK(getJson(t, &e, "attrs[Missing]").errorCode() == ErrorCode::NotFound);
    CHECK(getJson(t, &e, "choice/AltEmpty").errorCode() == ErrorCode::NotFound);
    CHECK(getJson(t, &e, "optInner/a").errorCode() == ErrorCode::NotFound);
    CHECK(getJson(t, &e, "byId[x]").errorCode() == ErrorCode::InvalidArgument);
    CHECK(getJson(t, &e, "i32v/x").errorCode() == ErrorCode::InvalidArgument);
    CHECK(getJson(t, &e, "ints[#1]").errorCode() == ErrorCode::InvalidArgument);
    CHECK(getJson(t, &e, "mounts[#ffff]").errorCode() == ErrorCode::NotFound);
    CHECK(getJson(t, &e, "mounts[#0000]").errorCode() == ErrorCode::InvalidArgument); // ambiguous prefix
    auto err = getJson(t, &e, "inner/zz");
    REQUIRE_FALSE(err);
    CHECK(err.error().message.find("rtest.Inner has no field 'zz'") != std::string::npos);
}

TEST_CASE("path: set values") {
    const TypeInfo& t = typeOf<Everything>();
    Everything e = makeFull();
    REQUIRE(setJson(t, &e, "inner/a", "5"));
    CHECK(e.inner.a == 5);
    REQUIRE(setJson(t, &e, "attrs[New.Attr]", "3"));
    CHECK(e.attrs[Name("New.Attr")] == 3.0f);
    REQUIRE(setJson(t, &e, "optInt", "null"));
    CHECK_FALSE(e.optInt.has_value());
    REQUIRE(setJson(t, &e, "optInner/a", "4")); // write mode keeps/engages the optional
    CHECK(e.optInner->a == 4);
    REQUIRE(setJson(t, &e, "choice/AltEmpty", "{}"));
    CHECK(e.choice.index() == 0);
    const std::string key = "0123456789abcdef0123456789abcdef";
    REQUIRE(setJson(t, &e, "mounts[#" + key + "]", "{\"bone\": \"new\", \"force\": 1}"));
    REQUIRE(e.mounts.size() == 3);
    CHECK(keyedKeyText(e.mounts.keyAt(2)) == key);
    CHECK(e.mounts[2].bone == Name("new"));
    REQUIRE(setJson(t, &e, "namedMounts[#center]", "{\"force\": 3}"));
    REQUIRE(e.namedMounts.size() == 3);
    CHECK(e.namedMounts[2].bone == Name("center")); // key field comes from the path
    CHECK(e.namedMounts[2].force == 3.0f);
    REQUIRE(setJson(t, &e, "v3/x", "8"));
    CHECK(e.v3.x == 8.0f);
    // A malformed value changes nothing (no half-inserted element).
    const usize before = e.mounts.size();
    CHECK_FALSE(setJson(t, &e, "mounts[#ffffffffffffffffffffffffffffffff]", "{\"force\": \"x\"}"));
    CHECK(e.mounts.size() == before);
    CHECK_FALSE(setJson(t, &e, "inner/a", "{broken"));
    // A short prefix cannot create an element.
    CHECK(setJson(t, &e, "mounts[#abcd]", "{}").errorCode() == ErrorCode::NotFound);

    EntityDoc doc;
    REQUIRE(setJson(typeOf<EntityDoc>(), &doc, "components.Health.max", "250"));
    REQUIRE(doc.components.Health.has_value());
    CHECK(doc.components.Health->max == 250.0f);
    CHECK(doc.components.Health->current == 100.0f);
}

TEST_CASE("patch: diff emits minimal property-path operations") {
    const TypeInfo& t = typeOf<Everything>();
    Everything a = makeFull();
    Everything b = a;
    b.inner.a = 8;
    b.attrs.erase(Name("Ship.Armor"));
    b.attrs[Name("Ship.Shield")] = 4.0f;
    b.mounts[1].force = 7.0f;
    b.optInts[1] = 3;
    const Patch p = diff(t, &a, &b);
    std::vector<std::string> ops;
    for (const PatchOp& op : p.ops) ops.push_back((op.kind == PatchOp::Kind::Set ? "set " : "remove ") + op.path + " " + op.value);
    CHECK(ops == std::vector<std::string>{
                     "set inner/a 8",
                     "set optInts[1] 3",
                     "remove attrs[Ship.Armor] ",
                     "set attrs[Ship.Shield] 4",
                     "set mounts[#" + keyHex(a.mounts, 1) + "]/force 7",
                 });
    checkDiffApply(a, b);
}

TEST_CASE("patch: keyed lists never depend on indices") {
    const TypeInfo& t = typeOf<Everything>();
    Everything a = makeFull();
    Everything b = a;
    const std::string removed = keyHex(a.mounts, 0);
    b.mounts.erase(usize{0});
    b.mounts.add(Guid(0xaaaa, 0xbbbb), Mount{Name("added"), 1.0f});
    b.namedMounts.erase(b.namedMounts.begin());
    b.namedMounts.push_back(Mount{Name("back"), 9.0f});
    const Patch p = diff(t, &a, &b);
    std::vector<std::string> paths;
    for (const PatchOp& op : p.ops) paths.push_back(op.path);
    CHECK(paths == std::vector<std::string>{"mounts[#" + removed + "]", "mounts[#000000000000aaaa000000000000bbbb]",
                                            "namedMounts[#left]", "namedMounts[#back]"});
    checkDiffApply(a, b);

    // Reordering falls back to replacing the list.
    Everything c = a;
    std::swap(c.namedMounts[0], c.namedMounts[1]);
    const Patch r = diff(t, &a, &c);
    REQUIRE(r.ops.size() == 1);
    CHECK(r.ops[0].path == "namedMounts");
    checkDiffApply(a, c);
}

TEST_CASE("patch: optionals, variants and lists of different length") {
    const TypeInfo& t = typeOf<Everything>();
    Everything a = makeFull();
    Everything b = a;
    b.optInner.reset();
    b.choice = AltValue{9, "renamed"};
    b.ints.push_back(4);
    const Patch p = diff(t, &a, &b);
    REQUIRE(p.ops.size() == 3);
    CHECK(p.ops[0].path == "optInner");
    CHECK(p.ops[0].value == "null");
    CHECK(p.ops[1].path == "ints");
    CHECK(p.ops[2].path == "choice/AltValue/label");
    checkDiffApply(a, b);
    Everything c = a;
    c.choice = AltEmpty{};
    const Patch q = diff(t, &a, &c);
    REQUIRE(q.ops.size() == 1);
    CHECK(q.ops[0].path == "choice");
    CHECK(q.ops[0].value == "\"AltEmpty\"");
    checkDiffApply(a, c);
    checkDiffApply(Everything{}, a);
    checkDiffApply(a, Everything{});
}

TEST_CASE("patch: remove operations and errors") {
    const TypeInfo& t = typeOf<Everything>();
    Everything e = makeFull();
    REQUIRE(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "optInt", {}}));
    CHECK_FALSE(e.optInt.has_value());
    REQUIRE(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "ints[0]", {}}));
    CHECK(e.ints.front() == -2);
    REQUIRE(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "byId[10]", {}}));
    CHECK(e.byId.size() == 1);
    CHECK(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "byId[10]", {}}).errorCode() == ErrorCode::NotFound);
    CHECK(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "inner/a", {}}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(apply(t, &e, PatchOp{PatchOp::Kind::Remove, "", {}}).errorCode() == ErrorCode::InvalidArgument);
    CHECK_FALSE(Patch::fromJson("[{\"op\": \"move\", \"path\": \"a\"}]"));
    CHECK_FALSE(Patch::fromJson("[{\"op\": \"set\", \"path\": \"a\"}]"));
    CHECK_FALSE(Patch::fromJson("{}"));
}

TEST_CASE("patch: property-based diff/apply over random edits") {
    const TypeInfo& t = typeOf<Everything>();
    Random rng(20260925);
    auto mutate = [&](Everything& e) {
        switch (rng.range(0, 19)) {
        case 0: e.i32v = rng.range(-1000, 1000); break;
        case 1: e.str = "s" + std::to_string(rng.range(0, 9)); break;
        case 2: e.inner.f = rng.range(-2.0f, 2.0f); break;
        case 3: e.ints.push_back(rng.range(-5, 5)); break;
        case 4:
            if (!e.ints.empty()) e.ints.pop_back();
            break;
        case 5: e.attrs[Name("k" + std::to_string(rng.range(0, 4)))] = rng.range(0.0f, 1.0f); break;
        case 6:
            if (!e.attrs.empty()) e.attrs.erase(e.attrs.begin());
            break;
        case 7: e.mounts.add(Mount{Name("m" + std::to_string(rng.range(0, 99))), rng.range(0.0f, 9.0f)}); break;
        case 8:
            if (!e.mounts.empty()) e.mounts.erase(rng.index(e.mounts.size()));
            break;
        case 9:
            if (!e.mounts.empty()) e.mounts[rng.index(e.mounts.size())].force = rng.range(0.0f, 9.0f);
            break;
        case 10:
            if (e.optInner) {
                e.optInner.reset();
            } else {
                e.optInner = Inner{rng.range(0, 3), "o", 1.5f};
            }
            break;
        case 11: e.choice = rng.chance(0.5) ? Choice{AltEmpty{}} : Choice{AltValue{rng.range(0, 3), "v"}}; break;
        case 12: e.byId[static_cast<u32>(rng.range(0, 5))].a = rng.range(0, 9); break;
        case 13: e.nameSet.insert(Name("n" + std::to_string(rng.range(0, 5)))); break;
        case 14: {
            const std::string bone = "nb" + std::to_string(rng.range(0, 6));
            bool exists = false;
            for (const Mount& m : e.namedMounts) exists = exists || m.bone == Name(bone);
            if (!exists) e.namedMounts.push_back(Mount{Name(bone), 1.0f});
            break;
        }
        case 15:
            if (!e.namedMounts.empty()) e.namedMounts.erase(e.namedMounts.begin() + static_cast<isize>(rng.index(e.namedMounts.size())));
            break;
        case 16: e.byTint[static_cast<Tint>(rng.range(1, 3))].push_back(rng.range(0, 3)); break;
        case 17: e.nested.push_back({static_cast<u8>(rng.range(0, 255))}); break;
        case 18: e.arrInner[rng.index(2)].s = "arr" + std::to_string(rng.range(0, 3)); break;
        default: e.tags.add(Name("T." + std::to_string(rng.range(0, 3)))); break;
        }
    };
    for (int iter = 0; iter < 300; ++iter) {
        Everything a = iter % 2 ? makeFull() : Everything{};
        for (int k = rng.range(0, 3); k > 0; --k) mutate(a);
        Everything b = a;
        for (int k = rng.range(1, 6); k > 0; --k) mutate(b);
        checkDiffApply(a, b);
        checkDiffApply(b, a);
    }
    (void)t;
}
