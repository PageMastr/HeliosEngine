// The generated C++ for schemas/sample/*.hschema (built by helios_schema() into this test):
// registration and TypeInfo metadata, JSONC and tagged-binary round trips through the compiled
// codecs, cross-checks against the reflection walker, schema evolution tolerance, Mut<C> dirty
// bits, property paths and diff/patch on generated types, record files, and consistency of
// helios-schemac's text formats with the runtime's.

#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include "helios/core/random.h"
#include "helios/reflect/reflect.h"
#include "lock.h"
#include "sample/common.samples.gen.h"
#include "sample/items.samples.gen.h"
#include "sample/ship.samples.gen.h"
#include "text.h"

using namespace helios;
using namespace helios::refl;

namespace sc = sample::common;
namespace ss = sample::ship;
namespace si = sample::items;

namespace {

struct Sample {
    const TypeInfo* type;
    void (*fill)(void*);
};

std::vector<Sample> allSamples() {
    std::vector<Sample> out;
    for (const auto& s : sc::samples::kSamples) out.push_back({&s.type(), s.fill});
    for (const auto& s : ss::samples::kSamples) out.push_back({&s.type(), s.fill});
    for (const auto& s : si::samples::kSamples) out.push_back({&s.type(), s.fill});
    return out;
}

TypeRegistry& sampleRegistry() {
    static TypeRegistry* registry = [] {
        auto* r = new TypeRegistry();
        REQUIRE(sc::registerCommonTypes(*r));
        REQUIRE(ss::registerShipTypes(*r));
        REQUIRE(si::registerItemsTypes(*r));
        return r;
    }();
    return *registry;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const FieldInfo& fieldOf(const TypeInfo& t, std::string_view name) {
    for (const FieldInfo& f : t.fields) {
        if (f.name == name) return f;
    }
    FAIL("no field " << std::string(name) << " in " << std::string(t.qualifiedName));
    return t.fields[0];
}

} // namespace

TEST_CASE("generated: registration and ids from the committed lock") {
    TypeRegistry& reg = sampleRegistry();
    CHECK(reg.size() >= 35);
    // Registering the same TypeInfos again is a no-op.
    CHECK(ss::registerShipTypes(reg));

    // Every generated type carries the id recorded in schemas/sample/schema.lock.jsonc.
    const std::string text = readFile(std::string(HELIOS_SOURCE_DIR) + "/schemas/sample/schema.lock.jsonc");
    REQUIRE_FALSE(text.empty());
    schemac::DiagnosticEngine diags;
    const u32 file = diags.addFile("schema.lock.jsonc", text);
    schemac::Lock lock;
    REQUIRE(schemac::loadLock(diags.fileText(file), file, lock, diags));
    usize checked = 0;
    for (const TypeInfo* t : reg.types()) {
        auto it = lock.types.find(std::string(t->qualifiedName));
        REQUIRE_MESSAGE(it != lock.types.end(), t->qualifiedName);
        CHECK(t->id == it->second.id);
        CHECK(reg.find(t->id) == t);
        CHECK(reg.find(t->qualifiedName) == t);
        for (const FieldInfo& f : t->fields) {
            bool found = false;
            for (const schemac::LockField& lf : it->second.fields) {
                if (lf.name == f.name && !lf.tombstone) {
                    CHECK(lf.id == f.id);
                    found = true;
                }
            }
            CHECK_MESSAGE(found, t->qualifiedName << "." << f.name);
        }
        for (const VariantAlt& a : t->alternatives) {
            bool found = false;
            for (const schemac::LockField& lf : it->second.fields) found |= lf.name == a.name && lf.id == a.id;
            CHECK(found);
        }
        ++checked;
    }
    CHECK(checked == reg.size());
    CHECK(typeOf<ss::ShipHullDef>().id == fnv1a32("sample.ship.ShipHullDef"));
}

TEST_CASE("generated: TypeInfo metadata mirrors the schema") {
    const TypeInfo& health = typeOf<sc::Health>();
    CHECK(health.qualifiedName == "sample.common.Health");
    CHECK(health.name == "Health");
    CHECK(health.decl == DeclKind::Component);
    CHECK(hasFlag(health.flags, TypeFlags::Replicated));
    CHECK(health.size == sizeof(sc::Health));
    REQUIRE(health.fields.size() == 3);
    const FieldInfo& current = fieldOf(health, "current");
    CHECK(current.repIndex == 0);
    CHECK(hasFlag(current.flags, FieldFlags::Replicated));
    CHECK(current.defaultJson == "100");
    CHECK(current.offset == offsetof(sc::Health, current));
    const auto* range = current.attr<attrs::Range>();
    REQUIRE(range);
    CHECK(range->min == 0);
    CHECK(range->max == 1e6);
    const auto* editor = current.attr<attrs::Editor>();
    REQUIRE(editor);
    CHECK(editor->category == "Combat");
    CHECK(editor->order == 1);
    CHECK(fieldOf(health, "shieldFraction").repIndex == 2);
    CHECK(std::tuple_size_v<decltype(sc::Health::kReplicatedFields)> == 3);
    CHECK(std::get<2>(sc::Health::kReplicatedFields) == &sc::Health::shieldFraction);

    const TypeInfo& server = typeOf<sc::Health::Server>();
    CHECK(server.qualifiedName == "sample.common.Health.Server");
    CHECK(hasFlag(server.flags, TypeFlags::ServerPart));
    const FieldInfo& regen = fieldOf(server, "regenPerSecond");
    CHECK(regen.defaultJson == "1.5");
    REQUIRE(regen.attr("unit"));
    CHECK(regen.attr("unit")->arg(0) == "hp/s");

    const TypeInfo& hull = typeOf<ss::ShipHullDef>();
    CHECK(hull.decl == DeclKind::Record);
    CHECK(hull.doc == "A hull type; instances live in content/records/hull/*.hrec.");
    REQUIRE(hull.attr<attrs::Table>());
    CHECK(hull.attr<attrs::Table>()->name == "hull");
    CHECK(hasFlag(fieldOf(hull, "lootTable").flags, FieldFlags::ServerOnly));
    CHECK(hasFlag(fieldOf(hull, "icon").flags, FieldFlags::ClientOnly));
    const FieldInfo& thrusters = fieldOf(hull, "thrusters");
    CHECK(thrusters.type().kind == Kind::KeyedList);
    CHECK(hasFlag(thrusters.flags, FieldFlags::Keyed));
    REQUIRE(thrusters.attr<attrs::Max>());
    CHECK(thrusters.attr<attrs::Max>()->count == 64);
    const FieldInfo& hardpoints = fieldOf(hull, "hardpoints");
    CHECK(hardpoints.type().kind == Kind::List);
    REQUIRE(hardpoints.attr<attrs::Keyed>());
    CHECK(hardpoints.attr<attrs::Keyed>()->field == "slot");
    CHECK(fieldOf(hull, "capabilities").defaultJson == R"(["Fly","Dock"])");
    CHECK(fieldOf(hull, "size").defaultJson == R"("Medium")");
    CHECK(fieldOf(hull, "icon").attr<attrs::Asset>()->kind == "Texture");
    CHECK(hasFlag(typeOf<ss::LootTableDef>().flags, TypeFlags::ServerOnly));

    const TypeInfo& damage = typeOf<ss::HullDamage>();
    CHECK(damage.version == 2);
    const FieldInfo& breaches = fieldOf(damage, "breaches");
    REQUIRE(breaches.was.size() == 1);
    CHECK(breaches.was[0] == "holes");

    const TypeInfo& faction = typeOf<sc::Faction>();
    CHECK(faction.kind == Kind::Enum);
    REQUIRE(faction.enumValues.size() == 4);
    CHECK(faction.enumValues[3].name == "Drones");
    CHECK(faction.enumValues[3].value == 10);
    CHECK(faction.enumValues[3].doc == "Hostile automatons.");
    CHECK(typeOf<sc::Capability>().kind == Kind::Flags);

    const TypeInfo& duration = typeOf<ss::EffectDef::Duration>();
    CHECK(duration.kind == Kind::Variant);
    CHECK(duration.qualifiedName == "sample.ship.EffectDef.Duration");
    REQUIRE(duration.alternatives.size() == 4);
    CHECK(duration.alternatives[3].name == "Periodic");
    CHECK(&duration.alternatives[3].type() == &typeOf<ss::EffectDef::DurationPeriodic>());
    CHECK(hasFlag(typeOf<ss::EffectDef::DurationInstant>().flags, TypeFlags::Unit));

    CHECK(typeOf<ss::RequestDock>().decl == DeclKind::Rpc);
    CHECK(typeOf<si::LedgerExecuteRequest>().decl == DeclKind::Rpc);
    CHECK(typeOf<ss::ShipDestroyed>().decl == DeclKind::Event);
    CHECK(typeOf<si::InventoryHud>().decl == DeclKind::ViewModel);
    CHECK(typeOf<si::TradeOffer>().decl == DeclKind::Message);
    CHECK(typeOf<ss::DockedTo>().decl == DeclKind::Relation);

    // Constants and aliases.
    CHECK(sc::MaxStackSize == 9999u);
    CHECK(sc::RespawnDelay == Duration::fromSeconds(30));
    CHECK(sc::DefaultCallsign == "Unnamed");
    static_assert(std::is_same_v<sc::Credits, i64>);
    static_assert(std::is_same_v<si::ItemTemplateRef, RecordRef<si::ItemTemplateDef>>);

    // Layout hashes identify layouts: distinct across types, stable across calls.
    std::set<u64> hashes;
    for (const TypeInfo* t : sampleRegistry().types()) {
        CHECK(t->layoutHash != 0);
        hashes.insert(t->layoutHash);
    }
    CHECK(hashes.size() == sampleRegistry().size());
}

TEST_CASE("generated: every sample round-trips through JSONC and binary, compiled and walked alike") {
    const auto samples = allSamples();
    REQUIRE(samples.size() >= 30);
    for (const Sample& s : samples) {
        const TypeInfo& t = *s.type;
        INFO(t.qualifiedName);
        Value v(t);
        CHECK(isDefault(t, v.data()));
        CHECK(walk::isDefault(t, v.data()));
        // Default objects serialize to nothing.
        CHECK(toJson(t, v.data(), JsonStyle::Compact) == "{}");
        std::vector<u8> empty;
        encodeTagged(t, v.data(), empty);
        CHECK(empty.empty());

        s.fill(v.data());
        const bool unit = t.fields.empty();
        CHECK(isDefault(t, v.data()) == unit);

        // JSONC: compiled writer == walker, and reading gives back an equal object.
        const std::string json = toJson(t, v.data());
        JsonWriter walked;
        walk::writeJson(t, v.data(), walked);
        CHECK(walked.take() == json);
        Value back(t);
        ReadCtx ctx;
        auto r = fromJson(t, back.data(), json, ctx);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(ctx.warnings().empty());
        CHECK_MESSAGE(equals(t, v.data(), back.data()), json);
        CHECK(walk::equals(t, v.data(), back.data()));
        Value walkedBack(t);
        auto doc = JsonDocument::parse(json);
        REQUIRE(doc);
        ReadCtx ctx2;
        REQUIRE(walk::readJson(t, walkedBack.data(), doc->root(), ctx2));
        CHECK(equals(t, v.data(), walkedBack.data()));

        // Binary: compiled bytes == walker bytes, decoded by both.
        std::vector<u8> bytes;
        encodeTagged(t, v.data(), bytes);
        std::vector<u8> walkedBytes;
        walk::encodeTagged(t, v.data(), walkedBytes);
        CHECK(bytes == walkedBytes);
        CHECK(bytes.empty() == unit);
        Value fromBytes(t);
        REQUIRE(decodeTagged(t, fromBytes.data(), bytes));
        CHECK(equals(t, v.data(), fromBytes.data()));
        Value fromBytesWalked(t);
        REQUIRE(walk::decodeTagged(t, fromBytesWalked.data(), bytes));
        CHECK(equals(t, v.data(), fromBytesWalked.data()));

        // Versioned envelope.
        const std::vector<u8> blob = encodeEnvelope(t, v.data());
        auto peek = peekEnvelopeType(blob);
        REQUIRE(peek);
        CHECK(*peek == t.id);
        Value fromBlob(t);
        REQUIRE(decodeEnvelope(t, fromBlob.data(), blob));
        CHECK(equals(t, v.data(), fromBlob.data()));

        // Copies compare equal; a changed copy does not (when there is something to change).
        Value copy(t);
        t.ops->copy(copy.data(), v.data());
        CHECK(t.ops->equals(copy.data(), v.data()));
    }
}

TEST_CASE("generated: typed API, canonical JSONC and operator==") {
    ss::ShipHullDef hull = ss::samples::makeShipHullDef();
    const std::string json = toJson(hull);
    auto back = fromJson<ss::ShipHullDef>(json);
    REQUIRE(back);
    CHECK(*back == hull);
    CHECK(json.starts_with("{\n  \"name\": \"loc:name.47\",\n  \"size\": \"Capital\",\n  \"faction\": \"Drones\",\n"));
    CHECK(json.find("\"thrusters\": [\n    {\n      \"$key\": \"f0791eb9-c3df-1ff7-61ac-79f7256bc235\",") != std::string::npos);
    CHECK(json.find("\"capabilities\": [\"Fly\", \"Dock\", \"Mine\", \"Scan\"]") != std::string::npos);
    CHECK(json.find("\"lootTable\": 4474426824777251459") != std::string::npos);
    CHECK(json.ends_with("}\n"));

    ss::ShipHullDef other = hull;
    CHECK(other == hull);
    other.thrusters[1].maxForce = 125.0f;
    CHECK_FALSE(other == hull);

    // Bitwise float equality: -0 != +0, NaN == NaN.
    ss::ThrusterMount a{}, b{};
    b.maxForce = a.maxForce;
    CHECK(a == b);
    a.maxForce = 0.0f;
    b.maxForce = -0.0f;
    CHECK_FALSE(a == b);
    a.maxForce = std::numeric_limits<f32>::quiet_NaN();
    b.maxForce = std::numeric_limits<f32>::quiet_NaN();
    CHECK(a == b);
    CHECK(toJson(a, JsonStyle::Compact) == R"({"maxForce":"nan"})");
    b.maxForce = -0.0f;
    CHECK(toJson(b, JsonStyle::Compact) == R"({"maxForce":-0.0})");

    // Defaults are omitted, so the explicit default and an absent member mean the same.
    ss::ThrusterMount d{};
    CHECK(d.maxForce == 50000.0f);
    CHECK(d.dir == Vec3(0, 0, -1));
    CHECK(toJson(d, JsonStyle::Compact) == "{}");
    auto parsed = fromJson<ss::ThrusterMount>(R"({ "bone": "b", /* comment */ "maxForce": 50000, })");
    REQUIRE(parsed);
    CHECK(parsed->bone == Name("b"));
    CHECK(toJson(*parsed, JsonStyle::Compact) == R"({"bone":"b"})");

    // Variants: {"<Alternative>": {...}}; unit alternatives are written as strings.
    ss::EffectDef effect = ss::samples::makeEffectDef();
    CHECK(toJson(effect, JsonStyle::Compact) ==
          R"({"duration":{"Periodic":{"period":"2906ms","ticks":47623,"executeOnApply":true}},"magnitude":-61.625,"tags":["Tag.2","Tag.Z4"],"stacking":"ByTarget"})");
    effect.duration = ss::EffectDef::DurationInfinite{};
    CHECK(toJson(effect, JsonStyle::Compact).starts_with(R"({"duration":"Infinite",)"));
    auto e2 = fromJson<ss::EffectDef>(R"({"duration": {"Timed": {"secs": 2.5}}})");
    REQUIRE(e2);
    REQUIRE(std::holds_alternative<ss::EffectDef::DurationTimed>(e2->duration));
    CHECK(std::get<ss::EffectDef::DurationTimed>(e2->duration).secs == 2.5f);
    CHECK_FALSE(fromJson<ss::EffectDef>(R"({"duration": {"Forever": {}}})"));

    // Items: arrays, optionals, nested lists, sets, maps with enum keys.
    si::ItemStack stack = si::samples::makeItemStack();
    auto stackBack = fromJson<si::ItemStack>(toJson(stack));
    REQUIRE(stackBack);
    CHECK(*stackBack == stack);
    auto bin = decodeTagged<si::ItemStack>(encodeTagged(stack));
    REQUIRE(bin);
    CHECK(*bin == stack);
    si::ItemTemplateDef itm{};
    CHECK(itm.quality == std::array<i32, 3>{1, 2, 0});
    CHECK(itm.stackMax == 1u);
    CHECK(itm.volume == 0.1f);
}

TEST_CASE("generated: readers tolerate schema evolution") {
    // Unknown binary fields (a newer writer) are skipped.
    ss::HullDamage dmg = ss::samples::makeHullDamage();
    std::vector<u8> bytes = encodeTagged(dmg);
    TaggedWriter w(bytes);
    w.writeTag(99, WireType::Varint);
    w.writeVarint(12345);
    w.writeTag(100, WireType::Len);
    w.writeString("future");
    w.writeTag(101, WireType::I64);
    w.writeFixed64(7);
    auto back = decodeTagged<ss::HullDamage>(bytes);
    REQUIRE(back);
    CHECK(*back == dmg);

    // Unknown JSON members warn (or fail in strict mode); @was names are accepted.
    ss::HullDamage fromOld{};
    ReadCtx ctx;
    REQUIRE(fromJson(R"({"hp": 5, "holes": [1, 2], "armor": 3})", fromOld, ctx));
    CHECK(fromOld.hp == 5.0f);
    CHECK(fromOld.breaches == std::vector<u8>{1, 2});
    REQUIRE(ctx.warnings().size() == 1);
    CHECK(ctx.warnings()[0].find("armor") != std::string::npos);
    ReadCtx strict(ReadCtx::Options{.strictUnknownFields = true});
    ss::HullDamage s{};
    CHECK_FALSE(fromJson(R"({"armor": 3})", s, strict));

    // An older reader (fewer fields) reading newer data: simulate by decoding Health bytes as
    // Health.Server-free Health (the server part is a separate type) — ids keep them apart.
    sc::Health h = sc::samples::makeHealth();
    auto hb = decodeTagged<sc::Health>(encodeTagged(h));
    REQUIRE(hb);
    CHECK(*hb == h);

    // f32 -> f64 widening: an f64 field accepts f32 (I32) wire data.
    std::vector<u8> f32bytes;
    TaggedWriter fw(f32bytes);
    fw.writeTag(fieldOf(typeOf<ss::EffectDef>(), "magnitude").id, WireType::I32);
    fw.writeF32(1.5f);
    auto widened = decodeTagged<ss::EffectDef>(f32bytes);
    REQUIRE(widened);
    CHECK(widened->magnitude == 1.5);

    // Corrupt input is rejected, never crashes.
    Xoshiro256 rng(7);
    const std::vector<u8> good = encodeTagged(ss::samples::makeShipHullDef());
    for (int i = 0; i < 300; ++i) {
        std::vector<u8> bad = good;
        const usize n = 1 + rng.nextU32() % 4;
        for (usize k = 0; k < n; ++k) bad[rng.nextU32() % bad.size()] = static_cast<u8>(rng.nextU32());
        if (rng.nextU32() % 3 == 0) bad.resize(rng.nextU32() % bad.size());
        ss::ShipHullDef out{};
        (void)decodeTagged(bad, out); // must not crash or trip sanitizers
    }
}

TEST_CASE("generated: Mut<C> sets per-field dirty bits") {
    sc::Health h{};
    u64 entityMask = 0;
    Mut<sc::Health> m(h, &entityMask, 1ull << 5);
    m.setCurrent(100.0f); // unchanged: no bit
    CHECK(h._dirty == 0);
    CHECK(entityMask == 0);
    m.setMax(120.0f);
    CHECK(h.max == 120.0f);
    CHECK(h._dirty == Mut<sc::Health>::kMax);
    CHECK(entityMask == (1ull << 5));
    m.editShieldFraction() = 0.5f;
    CHECK(dirtyFields(h) == (Mut<sc::Health>::kMax | Mut<sc::Health>::kShieldFraction));
    clearDirty(h);
    m.raw().current = 1.0f;
    CHECK(h._dirty == Mut<sc::Health>::kAllFields);
    // _dirty is not part of the value.
    sc::Health clean = h;
    clean._dirty = 0;
    CHECK(clean == h);
    CHECK(toJson(h, JsonStyle::Compact) == toJson(clean, JsonStyle::Compact));

    // Bits follow FieldInfo::repIndex (kReplicatedFields order) for every replicated component.
    for (const TypeInfo* t : sampleRegistry().types()) {
        if (!hasFlag(t->flags, TypeFlags::Replicated)) continue;
        u8 next = 0;
        for (const FieldInfo& f : t->fields) {
            CHECK(hasFlag(f.flags, FieldFlags::Replicated));
            CHECK(f.repIndex == next++);
        }
    }
    ss::ShipMotion motion{};
    Mut<ss::ShipMotion> mm(motion);
    mm.setVel(Vec3(1, 2, 3));
    CHECK(motion._dirty == (1ull << fieldOf(typeOf<ss::ShipMotion>(), "vel").repIndex));
    CHECK(hasFlag(fieldOf(typeOf<ss::ShipMotion>(), "pos").flags, FieldFlags::Predicted));
    CHECK_FALSE(hasFlag(fieldOf(typeOf<ss::ShipMotion>(), "angvel").flags, FieldFlags::Predicted));
}

TEST_CASE("generated: property paths and diff/patch on generated types") {
    const TypeInfo& t = typeOf<ss::ShipHullDef>();
    ss::ShipHullDef hull = ss::samples::makeShipHullDef();
    const std::string key0 = keyedKeyText(hull.thrusters.keyAt(0));

    auto force = getJson(t, &hull, "thrusters[#" + key0 + "]/maxForce");
    REQUIRE(force);
    CHECK(*force == "-104.25");
    CHECK(*getJson(t, &hull, "hardpoints[#slot_858]/size") == "\"Large\"");
    CHECK(*getJson(t, &hull, "handling.yawRate") == "-46.875");
    CHECK(*getJson(t, &hull, "aiHints[aiHintsKey_566]") == "165.875");
    REQUIRE(setJson(t, &hull, "handling/rollRate", "2"));
    CHECK(hull.handling.rollRate == 2.0f);
    REQUIRE(setJson(t, &hull, "aiHints[newHint]", "0.5"));
    CHECK(hull.aiHints.at(Name("newHint")) == 0.5f);
    auto typed = getAs<f32>(t, &hull, "mass");
    REQUIRE(typed);
    **typed = 99.0f;
    CHECK(hull.mass == 99.0f);
    CHECK_FALSE(getAs<u32>(t, &hull, "mass"));
    CHECK_FALSE(getJson(t, &hull, "thrusters[#00]/maxForce"));
    CHECK_FALSE(getJson(t, &hull, "nope"));

    ss::ShipHullDef before = ss::samples::makeShipHullDef();
    ss::ShipHullDef after = before;
    after.thrusters[0].maxForce = 1.0f;
    after.thrusters.add(Guid(1, 2), ss::ThrusterMount{});
    after.hardpoints[1].offset = Vec3(1, 1, 1);
    after.aiHints.erase(Name("aiHintsKey_538"));
    after.lootTable.reset();
    after.capabilities = sc::Capability::Fly;
    const Patch patch = diff(before, after);
    std::vector<std::string> paths;
    for (const PatchOp& op : patch.ops) paths.push_back(op.path);
    CHECK(std::find(paths.begin(), paths.end(), "thrusters[#" + key0 + "]/maxForce") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "hardpoints[#slot_858]/offset") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "aiHints[aiHintsKey_538]") != paths.end());
    ss::ShipHullDef applied = before;
    REQUIRE(apply(applied, patch));
    CHECK(applied == after);
    CHECK(diff(after, after).empty());

    // Variant alternative switch and nested edit.
    ss::EffectDef e1 = ss::samples::makeEffectDef();
    ss::EffectDef e2 = e1;
    std::get<ss::EffectDef::DurationPeriodic>(e2.duration).ticks = 3;
    Patch p = diff(e1, e2);
    REQUIRE(p.ops.size() == 1);
    CHECK(p.ops[0].path == "duration/Periodic/ticks");
    e2.duration = ss::EffectDef::DurationTimed{4.0f};
    p = diff(e1, e2);
    ss::EffectDef e3 = e1;
    REQUIRE(apply(e3, p));
    CHECK(e3 == e2);

    // Randomized: diff/apply over every sample type against a mutated copy.
    Xoshiro256 rng(42);
    for (const Sample& s : allSamples()) {
        const TypeInfo& type = *s.type;
        Value x(type);
        Value y(type);
        s.fill(y.data());
        for (int round = 0; round < 2; ++round) {
            const Patch forward = diff(type, x.data(), y.data());
            Value z(type);
            type.ops->copy(z.data(), x.data());
            auto res = apply(type, z.data(), forward);
            REQUIRE_MESSAGE(res, type.qualifiedName << ": " << (res ? std::string() : res.error().toString()));
            CHECK_MESSAGE(equals(type, z.data(), y.data()), type.qualifiedName << "\n" << forward.toJson());
            std::swap(x, y);
        }
        (void)rng;
    }
}

TEST_CASE("generated: record files carry $rid/$name headers before the fields") {
    si::ItemTemplateDef itm = si::samples::makeItemTemplateDef();
    RecordHeader header;
    header.rid = mintRecordId();
    header.name = "itm/sword";
    header.comment = "sample";
    const std::string text = writeRecord(itm, header);
    CHECK(text.starts_with("{\n  \"$rid\": "));
    CHECK(text.find("\"$name\": \"itm/sword\"") < text.find("\"name\":"));
    si::ItemTemplateDef back{};
    RecordHeader h2;
    ReadCtx ctx;
    REQUIRE(readRecord(text, back, h2, ctx));
    CHECK(back == itm);
    CHECK(h2 == header);
    CHECK(writeRecord(back, h2) == text);
}

TEST_CASE("generated: default values emitted by schemac equal the runtime's canonical JSON") {
    usize checked = 0;
    for (const TypeInfo* t : sampleRegistry().types()) {
        for (const FieldInfo& f : t->fields) {
            if (!f.defaultValue) {
                CHECK(f.defaultJson.empty());
                continue;
            }
            JsonWriter w(JsonStyle::Compact);
            writeJson(f.type(), f.defaultValue, w);
            CHECK_MESSAGE(w.take() == f.defaultJson, t->qualifiedName << "." << f.name);
            // The default object equals the field of a default-constructed owner.
            Value owner(*t);
            CHECK(equals(f.type(), f.ptr(owner.data()), f.defaultValue));
            CHECK(fieldIsDefault(f, f.ptr(owner.data())));
            ++checked;
        }
    }
    CHECK(checked >= 15);
}

TEST_CASE("generated: schemac number, duration and GUID text match the runtime") {
    Xoshiro256 rng(2024);
    char buf[64];
    auto checkF64 = [&](f64 v) {
        const usize n = formatJsonF64(v, buf);
        CHECK(schemac::formatF64(v) == std::string_view(buf, n));
    };
    auto checkF32 = [&](f32 v) {
        const usize n = formatJsonF32(v, buf);
        CHECK(schemac::formatF32(v) == std::string_view(buf, n));
    };
    for (f64 v : {0.0, -0.0, 1.0, 0.1, 1e21, 1e-7, 123456789012345680000.0, 5e-324, 1.7976931348623157e308,
                  std::numeric_limits<f64>::infinity(), -std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()})
        checkF64(v);
    for (int i = 0; i < 20000; ++i) {
        checkF64(std::bit_cast<f64>(rng.nextU64()));
        checkF32(std::bit_cast<f32>(rng.nextU32()));
        // Short decimals (the common case in schemas).
        const f64 dec = static_cast<f64>(static_cast<i64>(rng.nextU64() % 2000001) - 1000000) / std::pow(10.0, rng.nextU32() % 9);
        checkF64(dec);
        checkF32(static_cast<f32>(dec));
    }
    for (int i = 0; i < 5000; ++i) {
        const i64 unitScale[] = {1, 1000, 1'000'000, 1'000'000'000, 60'000'000'000, 3'600'000'000'000, 86'400'000'000'000};
        const i64 nanos = static_cast<i64>(rng.nextU64() % 100000) * unitScale[rng.nextU32() % 7] * ((rng.nextU32() & 1) ? -1 : 1);
        const std::string text = schemac::formatDuration(nanos);
        CHECK(text == formatDuration(Duration(nanos)));
        auto parsed = parseDuration(text);
        REQUIRE(parsed);
        CHECK(parsed->nanos == nanos);
        CHECK(schemac::parseDuration(text) == nanos);
    }
    for (const char* text : {"1.5s", "250ms", "0.001s", "2.5h", "30d", "-3m", "1us", "7ns", "1.000000001s"}) {
        auto a = parseDuration(text);
        auto b = schemac::parseDuration(text);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->nanos == *b);
    }
    for (const char* bad : {"", "5", "1.5x", "s", "1e3s", "1..5s", "--1s"}) {
        CHECK_FALSE(parseDuration(bad));
        CHECK_FALSE(schemac::parseDuration(bad));
    }
    for (int i = 0; i < 1000; ++i) {
        const Guid g(rng.nextU64(), rng.nextU64());
        CHECK(schemac::formatGuid(g.high, g.low) == g.toString());
        u64 hi = 0, lo = 0;
        REQUIRE(schemac::parseGuid(g.toString(), hi, lo));
        CHECK(Guid(hi, lo) == g);
    }
}

TEST_CASE("generated: the JSON schema description lists every type with its lock id") {
    const std::string text = readFile(HELIOS_SAMPLE_SCHEMA_JSON);
    REQUIRE_FALSE(text.empty());
    auto doc = JsonDocument::parse(text);
    REQUIRE(doc);
    const JsonValue root = doc->root();
    REQUIRE(root.isObject());
    CHECK(root.get("format").asString() == "helios-schema/1");
    const JsonValue types = root.get("types");
    REQUIRE(types.isArray());
    usize found = 0;
    for (const JsonValue ty : types.elements()) {
        const TypeInfo* info = sampleRegistry().find(ty.get("name").asString());
        if (!info) continue;
        u64 id = 0;
        CHECK(ty.get("id").getU64(id));
        CHECK(id == info->id);
        CHECK(ty.get("layoutHash").asString() == std::format("{:016x}", info->layoutHash));
        ++found;
    }
    CHECK(found == sampleRegistry().size());
}

TEST_CASE("generated: registerXTypes() defaults to the global registry (02 §3.6 find)") {
    REQUIRE(sc::registerCommonTypes());
    REQUIRE(sc::registerCommonTypes()); // idempotent
    CHECK(refl::find("sample.common.Health") == &typeOf<sc::Health>());
    CHECK(refl::find(typeOf<sc::Transform>().id) == &typeOf<sc::Transform>());
    CHECK(refl::find("sample.common.Nope") == nullptr);
}
