// `--emit repl` (02 §3.5; 04 §4.1, §4.5, §4.6): helios_schema(REPL) compiles the descriptors and
// full-state codecs of the golden fixture and the sample schemas into this binary. The tests check
// the descriptors against the schema and the TypeInfo, round-trip full state within each quantizer's
// precision, feed truncated and random input to the readers, and cover the rpc and event tables, the
// protocol hash and the emitter's diagnostics.

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <type_traits>

#include "golden.repl.gen.h"
#include "helios/core/random.h"
#include "helios/reflect/reflect.h"
#include "repl_edges.repl.gen.h"
#include "sample/common.repl.gen.h"
#include "sample/ship.repl.gen.h"
#include "test_util.h"

using namespace schemac_test;

namespace {

namespace rp = helios::refl::repl;
using golden::all::Motion;
using golden::all::Status;

template <class C>
std::vector<u8> fullState(const C& c, usize* bits = nullptr) {
    rp::BitWriter w;
    rp::RepOf<C>::writeFullState(w, c);
    if (bits) *bits = w.bitCount();
    CHECK(w.bitCount() <= rp::RepOf<C>::desc().maxFullStateBits);
    return w.bytes();
}

template <class C>
helios::Result<void> readState(const std::vector<u8>& bytes, usize bits, C& out) {
    rp::BitReader r(bytes, bits);
    return rp::RepOf<C>::readFullState(r, out);
}

TEST_CASE("repl: generated descriptors carry audience, LOD, change-mask indices and quantizers") {
    const rp::ComponentRepDesc& d = rp::RepOf<Motion>::desc();
    const helios::refl::TypeInfo& t = helios::refl::typeOf<Motion>();
    CHECK(d.name == "golden.all.Motion");
    CHECK(d.typeId == t.id);
    CHECK(d.audience == helios::refl::Audience::Owner);
    CHECK(d.lod == rp::Lod::Core);
    REQUIRE(d.fields.size() == 3); // `server { lastTick }` is never replicated
    CHECK(d.fields[0].name == "pos");
    CHECK(d.fields[0].quant.kind == rp::Quant::FrameCell);
    CHECK(d.fields[0].quant.cell == 4096.0);
    CHECK(d.fields[0].quant.res == 1.0 / 256);
    CHECK(d.fields[0].quant.bits == 20);
    CHECK(d.fields[0].predicted);
    CHECK(d.fields[1].quant.kind == rp::Quant::Range);
    CHECK(d.fields[1].quant.min == -64.0);
    CHECK(d.fields[1].quant.max == 64.0);
    CHECK(d.fields[1].quant.bits == 12);
    CHECK(d.fields[1].maxBits == 36);
    CHECK(d.fields[2].name == "hp");
    CHECK(d.fields[2].quant.kind == rp::Quant::Raw);
    CHECK(d.fields[2].maxBits == 32);
    CHECK(d.maxFullStateBits == 3 * (80 + 20) + 36 + 32);
    // Ids, offsets and change-mask indices agree with the TypeInfo (and so with Mut<C>'s dirty bits).
    for (const rp::FieldRep& f : d.fields) {
        INFO(f.name);
        const auto it = std::find_if(t.fields.begin(), t.fields.end(), [&](const helios::refl::FieldInfo& fi) { return fi.name == f.name; });
        REQUIRE(it != t.fields.end());
        CHECK(f.fieldId == it->id);
        CHECK(f.offset == it->offset);
        CHECK(f.repIndex == it->repIndex);
    }
    const rp::ComponentRepDesc& ship = rp::RepOf<sample::ship::ShipMotion>::desc();
    CHECK(ship.audience == helios::refl::Audience::All);
    REQUIRE(ship.fields.size() == 4);
    CHECK(ship.fields[1].quant.kind == rp::Quant::Smallest3);
    CHECK(ship.fields[1].interp == rp::Interp::Slerp);
    CHECK(ship.fields[3].name == "angvel");
    CHECK(ship.fields[3].lod == rp::Lod::Near); // lod(near) on the field; the component is core
    CHECK(rp::RepOf<sample::common::Health>::desc().fields.size() == 3);
}

TEST_CASE("repl: full state round-trips within each quantizer's precision") {
    helios::SplitMix64 rng(0x4e71);
    auto unit = [&] { return static_cast<f64>(rng.next() >> 11) / 9007199254740992.0; };
    for (int i = 0; i < 500; ++i) {
        Motion m;
        m.pos.local = helios::DVec3((unit() - 0.5) * 2e7, (unit() - 0.5) * 1e5, (unit() - 0.5) * 1e13);
        m.vel = helios::Vec3(static_cast<f32>((unit() - 0.5) * 120), static_cast<f32>((unit() - 0.5) * 200), 0.0f); // y is clamped to ±64
        m.hp = static_cast<f32>(unit() * 1000);
        usize bits = 0;
        const std::vector<u8> bytes = fullState(m, &bits);
        Motion back;
        REQUIRE(readState(bytes, bits, back));
        CHECK(std::fabs(back.pos.local.x - m.pos.local.x) <= 1.0 / 512);
        CHECK(std::fabs(back.pos.local.z - m.pos.local.z) <= 1.0 / 512);
        CHECK(std::fabs(back.vel.x - m.vel.x) <= 128.0 / 4095 / 2 + 1e-5);
        CHECK(std::fabs(back.vel.y - std::clamp(m.vel.y, -64.0f, 64.0f)) <= 128.0 / 4095 / 2 + 1e-5);
        CHECK(back.hp == m.hp); // raw
        // Quantizing is idempotent: a decoded state re-encodes to the same bits (both sides quantize).
        usize again = 0;
        CHECK(fullState(back, &again) == bytes);
        CHECK(again == bits);
    }
    // Every raw form and a range-quantized f64.
    Status s;
    s.mode = golden::all::Color::Blue;
    s.alive = false;
    s.level = -12345;
    s.perm = golden::all::Perm::Read | golden::all::Perm::Exec;
    s.who = helios::refl::EntityId(0x7fff'ffff'ffff'fff1ull);
    s.ref = golden::all::ThingRef(0x7000'0000'0000'0001ull);
    s.at = 123456789;
    s.wait = helios::refl::Duration::fromMillis(-1500);
    s.handle = helios::refl::NetHandle(0xABCDEF);
    s.rot = helios::Quat(0, 0.6f, 0, 0.8f);
    s.flat = helios::Vec2(-1.5f, 2.5f);
    s.tint = helios::Color(0.1f, 0.2f, 0.3f, 0.4f);
    s.far = helios::DVec3(1e13, -2e13, 3.25);
    s.where.local = helios::DVec3(1.25, -2.5, 1e12);
    s.score = 123.456;
    usize bits = 0;
    const std::vector<u8> bytes = fullState(s, &bits);
    Status back;
    REQUIRE(readState(bytes, bits, back));
    CHECK(back.mode == s.mode);
    CHECK(back.alive == s.alive);
    CHECK(back.level == s.level);
    CHECK(back.perm == s.perm);
    CHECK(back.who == s.who);
    CHECK(back.ref == s.ref);
    CHECK(back.at == s.at);
    CHECK(back.wait == s.wait);
    CHECK(back.handle == s.handle);
    CHECK(back.rot == s.rot);
    CHECK(back.flat == s.flat);
    CHECK(back.tint == s.tint);
    CHECK(back.far == s.far);
    CHECK(back.where == s.where);
    CHECK(std::fabs(back.score - s.score) <= 2000.0 / ((1u << 20) - 1) / 2 + 1e-9);
}

TEST_CASE("repl: truncated, corrupt and random full state fails without overreading") {
    Status s;
    s.where.local = helios::DVec3(1e9, 2, 3);
    usize bits = 0;
    const std::vector<u8> bytes = fullState(s, &bits);
    for (usize n = 0; n < bits; ++n) {
        Status out;
        CHECK_FALSE(readState(bytes, n, out)); // every proper prefix is truncated
    }
    // An undeclared enum value is rejected (Color has 0, 5, 6).
    std::vector<u8> bad = bytes;
    bad[0] = 3;
    Status out;
    auto r = readState(bad, bits, out);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("Status.mode: not a Color value") != std::string::npos);
    // Undeclared flag bits are rejected the same way (Perm declares 1, 2 and 4).
    Status flags;
    flags.perm = static_cast<golden::all::Perm>(0x80);
    usize flagBits = 0;
    const std::vector<u8> flagBytes = fullState(flags, &flagBits);
    auto f = readState(flagBytes, flagBits, out);
    REQUIRE_FALSE(f);
    CHECK(f.error().message.find("Status.perm: undeclared Perm bits") != std::string::npos);
    // Random input: every outcome is a clean result.
    helios::SplitMix64 rng(0xf022);
    for (int i = 0; i < 5000; ++i) {
        std::vector<u8> junk(rng.next() % 96);
        for (u8& b : junk) b = static_cast<u8>(rng.next());
        Motion m;
        (void)readState(junk, junk.size() * 8, m);
        sample::ship::ShipMotion sm;
        (void)readState(junk, junk.size() * 8, sm);
        Status st;
        (void)readState(junk, junk.size() * 8, st);
    }
}

TEST_CASE("repl: rpc and event tables and the protocol hash") {
    const rp::FileRepTables& g = golden::all::goldenReplication();
    REQUIRE(g.components.size() == 2); // Motion and Status (Plain is not replicated)
    CHECK(g.components[0] == &rp::RepOf<Motion>::desc());
    REQUIRE(g.rpcs.size() == 1);
    CHECK(g.rpcs[0].name == "golden.all.Fire");
    CHECK(g.rpcs[0].direction == rp::RpcDirection::ClientToServer);
    CHECK_FALSE(g.rpcs[0].reliable);
    CHECK(g.rpcs[0].ratePerSecond == 20.0);
    CHECK(g.rpcs[0].intent == "combat");
    CHECK(g.rpcs[0].argsTypeId == helios::refl::typeOf<golden::all::Fire>().id);
    REQUIRE(g.events.size() == 1);
    CHECK(g.events[0].name == "golden.all.Exploded");
    CHECK(g.events[0].audience == rp::EventAudience::Relevant);
    CHECK(g.events[0].reliable);
    const rp::FileRepTables& ship = sample::ship::shipReplication();
    CHECK(ship.rpcs.size() == 1);
    CHECK(ship.rpcs[0].ratePerSecond == 2.0);
    CHECK(ship.events.size() == 1);
    CHECK(g.protocolHash != ship.protocolHash);
    const u64 files[] = {g.protocolHash, ship.protocolHash};
    const u64 swapped[] = {ship.protocolHash, g.protocolHash};
    CHECK(rp::protocolHash(files) == rp::protocolHash(swapped));

    // The hash covers the wire contract: a quantizer's bits change it, a comment does not.
    auto hashOf = [](const std::string& schema) {
        CompileOptions options;
        options.emitRepl = true;
        options.cppOut = "cpp";
        auto c = compileText(schema, options);
        REQUIRE_MESSAGE(c->ok(), c->messages);
        const std::string* src = c->output("cpp/test/t.repl.gen.cpp");
        REQUIRE(src);
        const usize at = src->rfind("0x");
        return src->substr(at, 18);
    };
    const std::string base = "package test;\ncomponent C replicate(all) { v: vec3f @quant(range=±8, bits=10) }\n";
    CHECK(hashOf(base) == hashOf("// note\n" + base));
    CHECK(hashOf(base) != hashOf("package test;\ncomponent C replicate(all) { v: vec3f @quant(range=±8, bits=11) }\n"));
    CHECK(hashOf(base) != hashOf("package test;\ncomponent C replicate(owner) { v: vec3f @quant(range=±8, bits=10) }\n"));
    // range=x is ±x (02 §3.1 writes range=4096, 04 §4.1 range=±4096).
    CHECK(hashOf(base) == hashOf("package test;\ncomponent C replicate(all) { v: vec3f @quant(range=8, bits=10) }\n"));
    // A replicated enum's or flags' values are part of the contract (readers reject undeclared ones).
    const std::string flags = "package test;\nflags F : u8 { A; B }\ncomponent C replicate(all) { f: F }\n";
    CHECK(hashOf(flags) != hashOf("package test;\nflags F : u8 { A; B; X }\ncomponent C replicate(all) { f: F }\n"));

    // Rpc arguments and event fields are payload: their types, ids, names, defaults and every type
    // they reach change the hash; the round-1 reproducers had identical hashes.
    const std::string rpc = "package test;\nrpc R(x: u8) client->server reliable @ratelimit(1/s) @intent(combat);\n";
    for (const std::string& changed : {
             std::string("package test;\nrpc R(x: u16) client->server reliable @ratelimit(1/s) @intent(combat);\n"),
             std::string("package test;\nrpc R(x: u8, y: string) client->server reliable @ratelimit(1/s) @intent(combat);\n"),
             std::string("package test;\nrpc R(y: u8) client->server reliable @ratelimit(1/s) @intent(combat);\n")}) {
        INFO(changed);
        CHECK(hashOf(rpc) != hashOf(changed));
    }
    CHECK(hashOf(rpc) == hashOf("// note\n" + rpc));
    const std::string event = "package test;\nevent E @audience(relevant) { a: u8 }\n";
    CHECK(hashOf(event) != hashOf("package test;\nevent E @audience(relevant) { a: f64; b: string }\n"));
    CHECK(hashOf(event) != hashOf("package test;\nevent E @audience(relevant) { a: u8 = 3 }\n")); // readers fill omitted fields
    // A struct reached from an rpc, through a container: a field change inside it changes the hash.
    const std::string nested = "package test;\nstruct P { a: u8 }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n";
    CHECK(hashOf(nested) != hashOf("package test;\nstruct P { a: u16 }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n"));
    CHECK(hashOf(nested) != hashOf("package test;\nstruct P { a: u8 = 1 }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n"));
    const std::string withEnum = "package test;\nenum K : u8 { A }\nstruct P { a: u8; k: K? }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n";
    CHECK(hashOf(nested) != hashOf(withEnum));
    CHECK(hashOf(withEnum) != hashOf("package test;\nenum K : u8 { A; B }\nstruct P { a: u8; k: K? }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n"));
    CHECK(hashOf(nested) == hashOf("package test;\nstruct Unused { z: u8 }\nstruct P { a: u8 }\nrpc R(ps: list<P> @max(4)) server->client reliable;\n"));

    // Round 2: an enum's or flags' underlying type is its raw wire width, which the lock lets widen.
    CHECK(hashOf("package test;\nenum M : u8 { A }\ncomponent C replicate(all) { m: M }\n") !=
          hashOf("package test;\nenum M : u16 { A }\ncomponent C replicate(all) { m: M }\n"));
    CHECK(hashOf("package test;\nflags F : u8 { A }\ncomponent C replicate(all) { f: F }\n") !=
          hashOf("package test;\nflags F : u32 { A }\ncomponent C replicate(all) { f: F }\n"));
    CHECK(hashOf("package test;\nenum K : u8 { A }\nrpc R(k: K) server->client reliable;\n") !=
          hashOf("package test;\nenum K : u16 { A }\nrpc R(k: K) server->client reliable;\n"));
    // A top-level rpc's result, a payload @max (a decoder rejects more) and an event's reliability.
    const std::string noResult = "package test;\nrpc R(x: u8) server->client reliable;\n";
    CHECK(hashOf(noResult) != hashOf("package test;\nrpc R(x: u8) -> u8 server->client reliable;\n"));
    CHECK(hashOf("package test;\nrpc R(x: u8) -> u8 server->client reliable;\n") !=
          hashOf("package test;\nrpc R(x: u8) -> string server->client reliable;\n"));
    CHECK(hashOf("package test;\nstruct P { a: u8 }\nrpc R(x: u8) -> P server->client reliable;\n") !=
          hashOf("package test;\nstruct P { a: u16 }\nrpc R(x: u8) -> P server->client reliable;\n"));
    CHECK(hashOf("package test;\nrpc R(x: list<u8> @max(4)) server->client reliable;\n") !=
          hashOf("package test;\nrpc R(x: list<u8> @max(4096)) server->client reliable;\n"));
    CHECK(hashOf("package test;\nrpc R(x: list<u8> @max(16)) server->client reliable;\n") ==
          hashOf("package test;\nrpc R(x: list<u8> @max(0x10)) server->client reliable;\n"));
    CHECK(hashOf("package test;\nstruct P { s: string @max(8) }\nevent E { p: P }\n") !=
          hashOf("package test;\nstruct P { s: string @max(9) }\nevent E { p: P }\n"));
    CHECK(hashOf("package test;\nevent E { a: u8 }\n") != hashOf("package test;\nevent E @unreliable { a: u8 }\n"));
    {
        // An enum value of INT64_MIN: -9223372036854775808ll does not fit long long (GCC and Clang warn),
        // so the reader's validity check spells it as gen_cpp does.
        CompileOptions options;
        options.emitRepl = true;
        options.cppOut = "cpp";
        auto c = compileText("package test;\nenum K : i64 { Low = -9223372036854775808; High = 9223372036854775807 }\n"
                             "component C replicate(all) { k: K }\n",
                             options);
        REQUIRE_MESSAGE(c->ok(), c->messages);
        const std::string* src = c->output("cpp/test/t.repl.gen.cpp");
        REQUIRE(src);
        CHECK(src->find("v != (-9223372036854775807ll - 1) && v != 9223372036854775807ll") != std::string::npos);
        CHECK(src->find("9223372036854775808") == std::string::npos);
    }
    {
        CompileOptions options;
        options.emitRepl = true;
        options.cppOut = "cpp";
        auto c = compileText("package test;\nevent E @unreliable { a: u8 }\n", options);
        REQUIRE_MESSAGE(c->ok(), c->messages);
        const std::string* src = c->output("cpp/test/t.repl.gen.cpp");
        CHECK((src && src->find("{\"test.E\", ") != std::string::npos && src->find("EventAudience::Relevant, false}") != std::string::npos));
    }
}

TEST_CASE("repl: invalid @quant and fields the full-state codec cannot carry are errors") {
    CompileOptions options;
    options.emitRepl = true;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"v: vec3f @quant(range=±8)", "needs range=±x and bits=n"},
        {"v: vec3f @quant(range=-8, bits=10)", "range= needs a positive bound"},
        {"v: vec3f @quant(range=±1e39, bits=8)", "range=±1e+39 does not fit an f32 component"},
        {"v: f32 @quant(range=±1e-50, bits=8)", "range=±1e-50 rounds to 0 as an f32"},
        {"v: f64 @quant(range=±1e308, bits=8)", "the range's width is not a finite f64"},
        {"p: WorldPos @quant(frame_cell, cell=4km, res=1m)", "cell=4km: unit 'km' is not supported"},
        {"v: vec3f @quant(range=±8ft, bits=10)", "unit 'ft' is not supported"},
        {"v: vec3f @quant(range=±8, bits=40)", "bits= needs 1 to 32"},
        {"v: vec3f @quant(range=±8, bits=0)", "bits= needs 1 to 32"},
        {"v: vec3f @quant(smallest3, bits=10)", "smallest3 needs a quatf"},
        {"q: quatf @quant(smallest3)", "smallest3 needs bits=n"},
        {"q: quatf @quant(smallest3, bits=2)", "smallest3 needs bits=3 to 32"},
        {"q: quatf @quant(smallest3, bits=1)", "smallest3 needs bits=3 to 32"},
        {"p: WorldPos @quant(frame_cell, cell=4096m, res=1/256m, bits=8)", "bits= is not an argument of the frame_cell form"},
        {"q: quatf @quant(smallest3, bits=10, range=±1)", "range= is not an argument of the smallest3 form"},
        {"v: vec3f @quant(range=±8, bits=10, cell=4m)", "cell= is not an argument of the range form"},
        {"v: vec3f @quant(frame_cell, cell=4096m, res=1/256m)", "frame_cell needs a WorldPos"},
        {"p: WorldPos @quant(frame_cell, cell=10m, res=3m)", "cell must be a whole multiple"},
        {"p: WorldPos @quant(frame_cell, res=1m)", "frame_cell needs cell=<size> and res=<resolution>"},
        {"n: u8 @quant(range=±8, bits=4)", "range quantization needs a float scalar or vector"},
        {"v: vec3f @quant(cubic, bits=4)", "unknown form 'cubic'"},
        {"v: vec3f @quant(range=±8, bits=4, spin=2)", "unknown argument 'spin'"},
        // Round 3: a repeated argument or a second form left all but the last one ignored.
        {"v: vec3f @quant(range=±8, bits=10, bits=12)", "bits= is given twice"},
        {"v: vec3f @quant(range=±8, range=±16, bits=10)", "range= is given twice"},
        {"p: WorldPos @quant(frame_cell, cell=4096m, cell=8192m, res=1/256m)", "cell= is given twice"},
        {"p: WorldPos @quant(smallest3, frame_cell, cell=4096m, res=1/256m)", "a second form 'frame_cell' after 'smallest3'"},
        {"s: string", "which the Phase 0 full-state codec does not carry"},
        {"l: list<u8>", "which the Phase 0 full-state codec does not carry"},
        {"n: Name", "which the Phase 0 full-state codec does not carry"},
        {"v: vec3f @interp(cubic)", "expected linear or slerp"},
        {"v: vec3f lod(far)", "expected core or near"},
    };
    for (const auto& [field, expected] : cases) {
        INFO(field);
        const std::string messages = diagnosticsOf("component C replicate(all) { " + field + " }\n", options);
        CHECK_MESSAGE(messages.find(expected) != std::string::npos, messages);
    }
    CHECK(diagnosticsOf("event E @audience(everyone) { x: u8 }\n", options).find("expected owner, relevant or party") != std::string::npos);
    // Units apply to lengths only: bits= takes any schema integer, hex included, and a hex length has no unit.
    CHECK(diagnosticsOf("component C replicate(all) { v: vec3f @quant(range=±8, bits=0xA) }\n", options).empty());
    CHECK(diagnosticsOf("component C replicate(all) { v: vec3f @quant(range=0xA, bits=10) }\n", options).empty());
    CHECK(diagnosticsOf("component C replicate(all) { p: WorldPos @quant(frame_cell, cell=0x1000, res=1/0x100) }\n", options).empty());
    CHECK(diagnosticsOf("component C replicate(all) { q: quatf @quant(smallest3, bits=3) }\n", options).empty());
    // Without --emit repl the same schemas compile.
    CHECK(diagnosticsOf("component C replicate(all) { s: string }\n").empty());
}

TEST_CASE("repl: a schema file name that is not an identifier is an error, not code") {
    // The round-5 review: <stem>Replication() (and --emit cpp's register<Stem>Types()) pasted the file's
    // stem into C++. `ship-motion` and `2d` gave code that does not compile; the last name below closed
    // the package namespace and defined a global function.
    CompileOptions options;
    options.emitCpp = true;
    options.emitRepl = true;
    const std::string body = "package t;\ncomponent C replicate(all) { v: f32 }\n";
    for (const char* path : {"schemas/t/ship-motion.hschema", "schemas/t/2d.hschema", "schemas/t/ship motion.hschema",
                             "schemas/t/_ship.hschema", "schemas/t/.hschema", "schemas/t/schiff\xC3\xA4.hschema",
                             "schemas/t/x() noexcept;} int injected(){return 42;} namespace t{y.hschema"}) {
        INFO(path);
        auto c = compileFiles({{path, body}}, options);
        CHECK_FALSE(c->ok());
        CHECK(c->result.outputs.empty());
        CHECK_MESSAGE(c->messages.find("which is not an identifier") != std::string::npos, c->messages);
    }
    // An import is checked the same way.
    auto imported = compileFiles({{"schemas/t/a.hschema", "package t;\nimport \"2d.hschema\";\n"}, {"schemas/t/2d.hschema", "package t;\n"}},
                                 options);
    CHECK_FALSE(imported->ok());
    CHECK_MESSAGE(imported->messages.find("the schema file 't/2d.hschema' is named '2d'") != std::string::npos, imported->messages);
    // A letter, then letters, digits and '_'; whatever follows the first '.' is not part of the stem.
    for (const char* path : {"schemas/t/ship_motion.hschema", "schemas/t/Ship2.hschema", "schemas/t/ship.v2-beta.hschema"}) {
        INFO(path);
        auto c = compileFiles({{path, body}}, options);
        CHECK_MESSAGE(c->ok(), c->messages);
    }

    // Stems that differ only by '_' or the first letter's case name the same functions in one package,
    // whether both are inputs or one imports the other; in two packages they do not clash.
    CompileOptions both = options;
    both.files = {"schemas/t/ship_motion.hschema", "schemas/t/shipMotion.hschema"};
    auto clash = compileFiles({{"schemas/t/ship_motion.hschema", body}, {"schemas/t/shipMotion.hschema", "package t;\n"}}, both);
    CHECK_FALSE(clash->ok());
    CHECK_MESSAGE(clash->messages.find("are both in package 't' and would both define registerShipMotionTypes()") != std::string::npos,
                  clash->messages);
    auto viaImport = compileFiles(
        {{"schemas/t/ship_motion.hschema", "package t;\nimport \"ShipMotion.hschema\";\n"}, {"schemas/t/ShipMotion.hschema", "package t;\n"}},
        [&] {
            CompileOptions o = options;
            o.files = {"schemas/t/ship_motion.hschema"};
            return o;
        }());
    CHECK_FALSE(viaImport->ok());
    CHECK_MESSAGE(viaImport->messages.find("would both define registerShipMotionTypes()") != std::string::npos, viaImport->messages);
    CompileOptions twoPackages = options;
    twoPackages.files = {"schemas/t/ship_motion.hschema", "schemas/u/shipMotion.hschema"};
    auto apart = compileFiles({{"schemas/t/ship_motion.hschema", body}, {"schemas/u/shipMotion.hschema", "package u;\n"}}, twoPackages);
    CHECK_MESSAGE(apart->ok(), apart->messages);
}

TEST_CASE("repl: a declaration named like a generated function is an error") {
    // Round 6, nit 2: `component shipReplication` in ship.hschema made RepOf<::t::shipReplication> name
    // the function <stem>Replication(), so the generated code did not compile (register<Stem>Types() alike).
    CompileOptions options;
    options.emitCpp = true;
    options.emitRepl = true;
    options.namingLints = false;
    for (const char* decl : {"component shipReplication replicate(all) { v: f32 }", "struct registerShipTypes { v: f32 }",
                             "enum shipReplication : u8 { A }", "const registerShipTypes: u32 = 1;"}) {
        INFO(decl);
        auto c = compileFiles({{"schemas/t/ship.hschema", std::string("package t;\n") + decl + "\n"}}, options);
        CHECK_FALSE(c->ok());
        CHECK(c->result.outputs.empty());
        CHECK_MESSAGE(c->messages.find("that generated C++ for 'schemas/t/ship.hschema' declares in package 't'") != std::string::npos,
                      c->messages);
    }
    // Another file of the package, imported, declares the same namespace's names.
    auto viaImport = compileFiles({{"schemas/t/other.hschema", "package t;\nimport \"ship.hschema\";\nstruct shipReplication { v: f32 }\n"},
                                   {"schemas/t/ship.hschema", "package t;\n"}},
                                  [&] {
                                      CompileOptions o = options;
                                      o.files = {"schemas/t/other.hschema"};
                                      return o;
                                  }());
    CHECK_FALSE(viaImport->ok());
    CHECK_MESSAGE(viaImport->messages.find("'t.shipReplication' has the name of the function shipReplication()") != std::string::npos,
                  viaImport->messages);
    // Another package, a field and its inline (nested) type are other scopes; another case is another name.
    CompileOptions two = options;
    two.files = {"schemas/t/ship.hschema", "schemas/u/other.hschema"};
    auto apart = compileFiles({{"schemas/t/ship.hschema", "package t;\n"}, {"schemas/u/other.hschema", "package u;\nstruct shipReplication { v: f32 }\n"}},
                              two);
    CHECK_MESSAGE(apart->ok(), apart->messages);
    for (const char* text : {"package t;\nstruct Outer { shipReplication: { v: f32 } }\n", "package t;\nstruct ShipReplication { v: f32 }\n",
                             "package t;\nstruct RegisterShipTypes { v: f32 }\n"}) {
        INFO(text);
        auto c = compileFiles({{"schemas/t/ship.hschema", text}}, options);
        CHECK_MESSAGE(c->ok(), c->messages);
    }
}

TEST_CASE("repl: two outputs at one path are an error") {
    // Round 6, nit 1: t/ship.hschema's --emit repl header is t/ship.repl.gen.h, which is also --emit cpp's
    // header of t/ship.repl.hschema; in two packages no stem check caught it, and one overwrote the other.
    CompileOptions options;
    options.emitCpp = true;
    options.emitRepl = true;
    options.cppOut = "cpp";
    options.files = {"schemas/t/ship.hschema", "schemas/t/ship.repl.hschema"};
    auto c = compileFiles({{"schemas/t/ship.hschema", "package a;\n"}, {"schemas/t/ship.repl.hschema", "package b;\n"}}, options);
    CHECK_FALSE(c->ok());
    CHECK_MESSAGE(c->messages.find("two outputs would be written to 'cpp/t/ship.repl.gen.h'") != std::string::npos, c->messages);
    // --emit luau's glue likewise, and names that differ only in case (one file on Windows and macOS).
    CompileOptions luau = options;
    luau.emitRepl = false;
    luau.emitLuau = true;
    luau.luauOut = "luau";
    luau.files = {"schemas/t/ship.hschema", "schemas/t/ship.luau.hschema"};
    auto l = compileFiles({{"schemas/t/ship.hschema", "package a;\n"}, {"schemas/t/ship.luau.hschema", "package b;\n"}}, luau);
    CHECK_FALSE(l->ok());
    CHECK_MESSAGE(l->messages.find("two outputs would be written to 'cpp/t/ship.luau.gen.h'") != std::string::npos, l->messages);
    options.files = {"schemas/t/ship.hschema", "schemas/t/Ship.hschema"};
    auto cased = compileFiles({{"schemas/t/ship.hschema", "package a;\n"}, {"schemas/t/Ship.hschema", "package b;\n"}}, options);
    CHECK_FALSE(cased->ok());
    CHECK_MESSAGE(cased->messages.find("two outputs would be written to 'cpp/t/") != std::string::npos, cased->messages);
    // Distinct names compile.
    options.files = {"schemas/t/ship.hschema", "schemas/t/hull.hschema"};
    auto apart = compileFiles({{"schemas/t/ship.hschema", "package a;\n"}, {"schemas/t/hull.hschema", "package b;\n"}}, options);
    CHECK_MESSAGE(apart->ok(), apart->messages);
}

TEST_CASE("repl: a package's types named like the runtime's do not break <stem>Replication()") {
    // Round 6, blocking 2: the function is in the package's namespace and used the runtime's names
    // unqualified, so repl_edges.hschema's FileRepTables, RpcDirection, ... hid them and its generated
    // code did not compile (this binary did not build).
    static_assert(!std::is_same_v<repl_edges::RpcDirection, rp::RpcDirection>);
    static_assert(!std::is_same_v<repl_edges::FileRepTables, rp::FileRepTables>);
    const rp::FileRepTables& t = repl_edges::replEdgesReplication();
    REQUIRE(t.components.size() == 1);
    CHECK(t.components[0] == &rp::RepOf<repl_edges::Saturating>::desc());
    REQUIRE(t.rpcs.size() == 1);
    CHECK(t.rpcs[0].name == "repl_edges.Hit");
    CHECK(t.rpcs[0].direction == rp::RpcDirection::ServerToClient);
    REQUIRE(t.events.size() == 1);
    CHECK(t.events[0].name == "repl_edges.Boom");
    CHECK(t.events[0].audience == rp::EventAudience::Owner);
    CHECK(t.protocolHash != 0);
}

TEST_CASE("repl: a saturated f32 range value re-encodes to the same bits") {
    // Round 6, blocking 1: an f32 reader stores static_cast<f32>(dequantizeRange(...)), so with the bound
    // as written (0.7, 0.9, 3.3) a saturated value or NaN decoded to the bound rounded to f32, inside the
    // range, and re-encoded to another step at 26 bits or more. f32 fields now quantize against the f32
    // bound; f64 fields keep the bound as written.
    using repl_edges::Saturating;
    const rp::ComponentRepDesc& d = rp::RepOf<Saturating>::desc();
    REQUIRE(d.fields.size() == 4);
    CHECK(d.fields[0].quant.max == static_cast<f64>(0.7f));
    CHECK(d.fields[0].quant.min == -static_cast<f64>(0.7f));
    CHECK(d.fields[1].quant.max == static_cast<f64>(0.9f));
    CHECK(d.fields[2].quant.max == static_cast<f64>(3.3f));
    CHECK(d.fields[3].quant.max == 0.7);
    constexpr f32 kInf = std::numeric_limits<f32>::infinity();
    std::vector<f32> values = {-1e9f, 1e9f, std::numeric_limits<f32>::quiet_NaN(), kInf, -kInf, 0.7f, -0.7f, 0.9f, -0.9f, 3.3f, -3.3f,
                               std::nextafter(0.7f, 0.0f), std::nextafter(3.3f, 0.0f), 0.0f, -0.0f, 0.25f, FLT_MAX};
    helios::SplitMix64 rng(0x5a70);
    for (int i = 0; i < 20000; ++i) values.push_back(static_cast<f32>((static_cast<f64>(rng.next() >> 11) / 9007199254740992.0 * 2 - 1) * 8));
    u32 differ = 0;
    std::string first; // the first few values that re-encoded to other bits
    for (const f32 v : values) {
        Saturating s;
        s.a = v;
        s.b = helios::Vec3(v, -v, v * 0.5f);
        s.c = v;
        s.d = v;
        usize bits = 0;
        const std::vector<u8> bytes = fullState(s, &bits);
        Saturating back;
        REQUIRE(readState(bytes, bits, back));
        usize again = 0;
        if ((fullState(back, &again) != bytes || again != bits) && differ++ < 4)
            first += std::format("v={} decoded a={} b.x={} c={}; ", v, back.a, back.b.x, back.c);
    }
    INFO(first);
    CHECK(differ == 0);

    // The emitted bound is the f32 value; an f64 field keeps 0.7.
    CompileOptions options;
    options.emitRepl = true;
    options.cppOut = "cpp";
    auto c = compileText("package test;\ncomponent C replicate(all) { v: f32 @quant(range=±0.7, bits=32); w: f64 @quant(range=±0.7, bits=32) }\n",
                         options);
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string* src = c->output("cpp/test/t.repl.gen.cpp");
    REQUIRE(src);
    CHECK(src->find("quantizeRange(c.v, -0.699999988079071, 0.699999988079071, 32)") != std::string::npos);
    CHECK(src->find("dequantizeRange(*x, -0.699999988079071, 0.699999988079071, 32)") != std::string::npos);
    CHECK(src->find("Quantizer{Quant::Range, 32, -0.699999988079071, 0.699999988079071,") != std::string::npos);
    CHECK(src->find("quantizeRange(c.w, -0.7, 0.7, 32)") != std::string::npos);
}

} // namespace
