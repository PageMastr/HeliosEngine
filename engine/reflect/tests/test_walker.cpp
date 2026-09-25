// Builder-reflected types: TypeInfo contents, the reflection walker, envelopes and the registry.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "helios/core/random.h"
#include "test_types.h"

using namespace helios;
using namespace helios::refl;
using namespace rtest;

TEST_CASE("walker: builder TypeInfo describes fields, ids, offsets and defaults") {
    const TypeInfo& t = typeOf<Inner>();
    CHECK(t.qualifiedName == "rtest.Inner");
    CHECK(t.name == "Inner");
    CHECK(t.kind == Kind::Struct);
    CHECK(t.size == sizeof(Inner));
    CHECK(t.doc == "Nested struct");
    REQUIRE(t.fields.size() == 3);
    CHECK(t.fields[0].name == "a");
    CHECK(t.fields[0].id == 1);
    CHECK(t.fields[1].id == 5);
    CHECK(t.fields[2].id == 6); // continues after an explicit id
    const Inner probe;
    CHECK(t.fields[1].offset ==
          static_cast<u32>(reinterpret_cast<const char*>(&probe.s) - reinterpret_cast<const char*>(&probe)));
    CHECK(&t.fields[2].type() == &typeOf<f32>());
    CHECK(t.fieldOrAlias("text") == &t.fields[1]);
    CHECK(t.fieldById(6) == &t.fields[2]);
    CHECK(t.field("nope") == nullptr);
    REQUIRE(t.fields[2].defaultValue != nullptr);
    CHECK(*static_cast<const f32*>(t.fields[2].defaultValue) == 1.5f);

    const TypeInfo& m = typeOf<Mount>();
    REQUIRE(m.fields[1].attr("unit") != nullptr);
    CHECK(m.fields[1].attr("unit")->arg(0) == "N");

    const TypeInfo& e = typeOf<Everything>();
    CHECK(e.attr("table")->arg(0) == "everything");
    const FieldInfo* named = e.field("namedMounts");
    REQUIRE(named);
    REQUIRE(named->attr<attrs::Keyed>());
    CHECK(named->attr<attrs::Keyed>()->field == "bone");
    CHECK(hasFlag(named->flags, FieldFlags::Keyed));

    const TypeInfo& tint = typeOf<Tint>();
    CHECK(tint.kind == Kind::Enum);
    CHECK(tint.enumByName("Blue")->value == 3);
    CHECK(&tint.element() == &typeOf<u8>());
    CHECK(typeOf<Caps>().kind == Kind::Flags);
    const TypeInfo& choice = typeOf<Choice>();
    CHECK(choice.kind == Kind::Variant);
    CHECK(choice.alternatives.size() == 2);
    CHECK(choice.alternative("AltValue")->id == 7);
    CHECK(typeOf<AltEmpty>().hasFlag(TypeFlags::Unit));
}

TEST_CASE("walker: default objects serialize to nothing") {
    const Everything e;
    CHECK(isDefault(typeOf<Everything>(), &e));
    std::vector<u8> out;
    encodeTagged(typeOf<Everything>(), &e, out);
    CHECK(out.empty());
    CHECK(toJson(typeOf<Everything>(), &e) == "{}\n");
}

TEST_CASE("walker: full object round-trips through JSONC and tagged binary") {
    const TypeInfo& t = typeOf<Everything>();
    const Everything full = makeFull();
    CHECK_FALSE(isDefault(t, &full));

    const std::string json = toJson(t, &full);
    Everything fromText;
    ReadCtx ctx;
    auto res = fromJson(t, &fromText, json, ctx);
    REQUIRE_MESSAGE(res, (res ? std::string() : res.error().message));
    CHECK(ctx.warnings().empty());
    CHECK(equals(t, &fromText, &full));
    CHECK(toJson(t, &fromText) == json);

    std::vector<u8> bin;
    encodeTagged(t, &full, bin);
    Everything fromBin;
    auto res2 = decodeTagged(t, &fromBin, bin);
    REQUIRE_MESSAGE(res2, (res2 ? std::string() : res2.error().message));
    CHECK(equals(t, &fromBin, &full));
    std::vector<u8> again;
    encodeTagged(t, &fromBin, again);
    CHECK(again == bin);

    // Spot-check canonical text.
    CHECK(json.find("\"withDefault\": 0,") != std::string::npos);
    CHECK(json.find("\"f32v\": 0.1,") != std::string::npos);
    CHECK(json.find("\"nameSet\": [\"alpha\", \"mid\", \"zeta\"],") != std::string::npos);
    CHECK(json.find("\"caps\": [\"Fly\", \"Dig\"],") != std::string::npos);
    CHECK(json.find("\"dur\": \"1500ms\",") != std::string::npos);
    CHECK(json.find("\"optInt\": 0,") != std::string::npos);
    CHECK(json.find("\"optInner\": {},") != std::string::npos);
    CHECK(json.find("\"choice\": {\n    \"AltValue\": {") != std::string::npos);
}

TEST_CASE("walker: typed container codecs and the walker agree") {
    // Containers of builder structs go through Codec<std::vector<...>> etc. (compiled) while the
    // walker drives them through TypeOps; both must produce identical bytes and text.
    const Everything full = makeFull();
    const TypeInfo& inners = typeOf<std::vector<Inner>>();
    JsonWriter a;
    writeJson(inners, &full.inners, a);
    JsonWriter b;
    walk::writeJson(inners, &full.inners, b);
    CHECK(a.take() == b.take());

    const TypeInfo& byTint = typeOf<std::map<Tint, std::vector<i32>>>();
    JsonWriter c;
    writeJson(byTint, &full.byTint, c);
    JsonWriter d;
    walk::writeJson(byTint, &full.byTint, d);
    CHECK(c.take() == d.take());
}

TEST_CASE("walker: readers tolerate unknown and missing fields") {
    const TypeInfo& t = typeOf<Inner>();
    Inner v;
    ReadCtx ctx;
    REQUIRE(fromJson(t, &v, "{\"a\": 3, \"future\": [1, 2], \"$comment\": \"ignored\", \"text\": \"old key\"}", ctx));
    CHECK(v.a == 3);
    CHECK(v.s == "old key"); // @was alias
    CHECK(v.f == 1.5f);      // missing -> default
    CHECK(ctx.warnings().size() == 1);

    ReadCtx strict(ReadCtx::Options{.strictUnknownFields = true});
    Inner w;
    auto res = fromJson(t, &w, "{\"future\": 1}", strict);
    REQUIRE_FALSE(res);
    CHECK(res.error().message.find("future") != std::string::npos);

    // Binary: unknown field ids of every wire type are skipped.
    std::vector<u8> bin;
    TaggedWriter tw(bin);
    tw.writeTag(99, WireType::Len);
    tw.writeString("new field");
    tw.writeTag(1, WireType::Varint);
    tw.writeZigZag(-9);
    tw.writeTag(98, WireType::I64);
    tw.writeFixed64(1);
    Inner x;
    REQUIRE(decodeTagged(t, &x, bin));
    CHECK(x.a == -9);
    CHECK(x.f == 1.5f);
}

TEST_CASE("walker: type errors report the JSON path") {
    Everything e;
    ReadCtx ctx;
    auto res = fromJson(typeOf<Everything>(), &e, "{\"inners\": [{}, {\"a\": \"nope\"}]}", ctx);
    REQUIRE_FALSE(res);
    CHECK(res.error().message == "inners[1].a: expected integer, got string");
    auto res2 = fromJson(typeOf<Everything>(), &e, "{\"u8v\": 256}", ctx);
    REQUIRE_FALSE(res2);
    CHECK(res2.error().message == "u8v: integer out of range");
}

TEST_CASE("walker: corrupt binary input fails without crashing") {
    const Everything full = makeFull();
    std::vector<u8> bin;
    encodeTagged(typeOf<Everything>(), &full, bin);
    Random rng(99);
    int failures = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<u8> mutated = bin;
        const usize flips = rng.range<usize>(1, 4);
        for (usize f = 0; f < flips; ++f) mutated[rng.index(mutated.size())] ^= static_cast<u8>(1u << rng.range(0, 7));
        if (rng.chance(0.3)) mutated.resize(rng.index(mutated.size()));
        Everything out;
        if (!decodeTagged(typeOf<Everything>(), &out, mutated)) ++failures;
    }
    CHECK(failures > 0); // most mutations are detected; none may crash (ASan/UBSan builds check that)
}

TEST_CASE("walker: envelopes are versioned and typed") {
    const Inner v{4, "env", 2.0f};
    const std::vector<u8> blob = encodeEnvelope(typeOf<Inner>(), &v);
    CHECK(blob[0] == 'H');
    CHECK(blob[3] == '1');
    auto id = peekEnvelopeType(blob);
    REQUIRE(id);
    CHECK(*id == typeOf<Inner>().id);
    Inner back;
    REQUIRE(decodeEnvelope(typeOf<Inner>(), &back, blob));
    CHECK(back == v);
    Mount wrong;
    CHECK(decodeEnvelope(typeOf<Mount>(), &wrong, blob).errorCode() == ErrorCode::InvalidArgument);
    std::vector<u8> bad = blob;
    bad[0] = 'X';
    CHECK(decodeEnvelope(typeOf<Inner>(), &back, bad).errorCode() == ErrorCode::VersionMismatch);
}

TEST_CASE("walker: Value owns type-erased storage") {
    Value v(typeOf<Everything>());
    CHECK(v.type() == &typeOf<Everything>());
    CHECK(isDefault(*v.type(), v.data()));
    Value moved = std::move(v);
    CHECK_FALSE(v);
    CHECK(moved);
    static_cast<Everything*>(moved.data())->strs.push_back("heap");
    CHECK_FALSE(isDefault(*moved.type(), moved.data()));
}

TEST_CASE("registry: lookup by id and name, duplicates and hot-reload replacement") {
    TypeRegistry reg;
    const TypeInfo* types[] = {&typeOf<Inner>(), &typeOf<Mount>(), &typeOf<Everything>()};
    REQUIRE(reg.add(types));
    REQUIRE(reg.add(typeOf<Inner>())); // same object again: no-op
    CHECK(reg.size() == 3);
    CHECK(reg.find("rtest.Mount") == &typeOf<Mount>());
    CHECK(reg.find(typeOf<Everything>().id) == &typeOf<Everything>());
    CHECK(reg.find("rtest.Nope") == nullptr);
    CHECK(reg.find(TypeId{12345}) == nullptr);
    CHECK(reg.types()[0]->qualifiedName == "rtest.Everything"); // sorted by name

    TypeInfo impostor = typeOf<Inner>();
    CHECK(reg.add(impostor).errorCode() == ErrorCode::AlreadyExists);
    TypeInfo sameId = typeOf<Mount>();
    sameId.qualifiedName = "rtest.Other";
    CHECK(reg.add(sameId).errorCode() == ErrorCode::AlreadyExists);
    CHECK(reg.size() == 3);

    TypeInfo reloaded = typeOf<Inner>();
    const TypeInfo* repl[] = {&reloaded};
    REQUIRE(reg.replace(repl));
    CHECK(reg.find("rtest.Inner") == &reloaded);
    CHECK(reg.size() == 3);
}

TEST_CASE("registry: lookups are safe while another thread registers") {
    TypeRegistry reg;
    REQUIRE(reg.add(typeOf<Inner>()));
    std::vector<TypeInfo> extra(200, typeOf<Mount>());
    std::vector<std::string> names(extra.size());
    for (usize i = 0; i < extra.size(); ++i) {
        names[i] = "rtest.Generated" + std::to_string(i);
        extra[i].qualifiedName = names[i];
        extra[i].id = static_cast<TypeId>(1000 + i);
    }
    std::atomic<bool> done{false};
    std::atomic<u64> hits{0};
    std::atomic<bool> sawNull{false};
    std::thread reader([&] {
        // Keep reading until registration finished and at least 1000 lookups ran.
        for (u64 i = 0; !done.load(std::memory_order_acquire) || i < 1000; ++i) {
            if (reg.find("rtest.Inner") == &typeOf<Inner>()) hits.fetch_add(1, std::memory_order_relaxed);
            for (const TypeInfo* t : reg.types()) {
                if (!t) sawNull.store(true);
            }
        }
    });
    for (TypeInfo& t : extra) REQUIRE(reg.add(t));
    done.store(true, std::memory_order_release);
    reader.join();
    CHECK(reg.size() == 201);
    CHECK(reg.find(TypeId{1199})->qualifiedName == "rtest.Generated199");
    CHECK(hits.load() >= 1000);
    CHECK_FALSE(sawNull.load());
}
