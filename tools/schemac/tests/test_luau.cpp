// `--emit luau` end to end (02 §3.5, §7.4). helios_schema(LUAU_OUT) compiles the scriptlib glue of the
// golden fixture and of the sample schemas into this binary; the tests register it on a real
// engine/script VM and call it from Luau (conversions, fuel charges, realms, hostile arguments),
// type-check the generated schema.d.luau with Luau.Analysis, and check the lock's binding ids and
// the emitter's diagnostics.

#include <format>
#include <fstream>
#include <limits>
#include <sstream>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Frontend.h"
#include "golden.luau.gen.h"
#include "helios/core/time.h"
#include "helios/script/script.h"
#include "sample/ship.luau.gen.h"
#include "test_util.h"
#include "text.h"

using namespace schemac_test;

namespace {

namespace hs = helios::script;
using golden::all::Item;
using golden::all::Late;
using golden::all::ThingRef;
using helios::FramePos;
using helios::refl::EntityId;

constexpr u64 kBigId = 0x7fff'ffff'ffff'fff1ull; // beyond 2^53: ids must survive the trip exactly
constexpr u64 kRefId = 0x7000'0000'0000'0001ull;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct FakeQueries final : golden::all::Queries {
    usize inRadiusCount = 3;
    u64 spawnItems = 0;
    FramePos lastAt;
    f32 lastRadius = 0;
    ThingRef lastThing;
    std::string logged;
    struct Converted {
        bool flag = false;
        i8 small = 0;
        i64 big = 0;
        u16 count = 0;
        f64 ratio = 0;
        std::string name, text, label, query;
        helios::Vec3 dir;
        helios::refl::Duration delay;
        u64 at = 0;
        golden::all::Color color = golden::all::Color::Red;
        Item item;
        std::vector<Item> items;
        std::set<helios::Name> names;
        std::array<f32, 3> trio{};
        std::optional<u32> maybe;
    } converted;

    std::optional<ThingRef> nearest(lua_State*, const FramePos& at, f32 radius) override {
        lastAt = at;
        lastRadius = radius;
        return ThingRef(kRefId);
    }
    std::vector<EntityId> inRadius(lua_State*, const FramePos&, f32) override {
        std::vector<EntityId> out;
        for (usize i = 0; i < inRadiusCount; ++i) out.push_back(EntityId(kBigId - i));
        return out;
    }
    EntityId spawn(lua_State*, ThingRef thing, const FramePos& at) override {
        lastThing = thing;
        lastAt = at;
        return EntityId(kBigId);
    }
    u64 spawnItemCount(lua_State*, ThingRef) override { return spawnItems; }
    Item convert(lua_State*, bool flag, i8 small, i64 big, u16 count, f64 ratio, helios::Name name, const std::string& text,
                 helios::Vec3 dir, helios::refl::Duration delay, u64 at, golden::all::Color color, const Item& item,
                 const std::vector<Item>& items, const std::set<helios::Name>& names, const std::array<f32, 3>& trio,
                 const std::optional<u32>& maybe, const helios::refl::LocString& label, const helios::refl::TagQuery& query) override {
        converted = Converted{flag, small, big, count, ratio, std::string(name.view()), text, label.key, query.text, dir, delay, at, color,
                              item, items, names, trio, maybe};
        Item out = item;
        out.count = static_cast<u16>(items.size() + 5);
        return out;
    }
    Late echo(lua_State*, const Late& tree) override { return tree; }
    void log(lua_State*, const std::string& text) override { logged = text; }
};

struct FakeShips final : sample::ship::ShipQueries {
    std::optional<sample::ship::ShipHullRef> hullOf(lua_State*, EntityId ship) override {
        if (ship.value != kBigId) return std::nullopt;
        return sample::ship::ShipHullRef(kRefId);
    }
    std::vector<EntityId> dockedShips(lua_State*, EntityId station) override { return {station, station}; }
};

int nop(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

/// A cell-profile VM with the golden and sample glue bound for `realm`, plus Probe.nop (no charge),
/// the reference that fuel deltas are measured against.
struct Vm {
    helios::DilatableClock clock{helios::DilatableClock::Config{50'000'000, 100'000, 1'000'000}};
    FakeQueries queries;
    FakeShips ships;
    u32 boundQueries = 0;
    u32 boundShips = 0;
    std::unique_ptr<hs::ScriptVm> vm;

    explicit Vm(std::string_view realm = "server") {
        hs::VmConfig config;
        config.name = "schemac-luau";
        config.clock = &clock;
        config.budget.wallBackstopNanos = 0; // fuel only: deterministic
        auto created = hs::ScriptVm::create(config, [&](hs::Binder& b) {
            boundQueries = golden::all::bindQueries(b, queries, realm);
            boundShips = sample::ship::bindShipQueries(b, ships, realm);
            b.function("Probe", "nop", &nop, hs::FuelCost{0, 0, 0});
        });
        REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string() : created.error().toString()));
        vm = std::move(*created);
    }

    /// Runs `body` as the function `M.run` of a fresh module; returns "" or the error text.
    std::string run(const std::string& body, std::vector<double>* numbers = nullptr, int nresults = 0) {
        const std::string name = "m" + std::to_string(++m_modules);
        const std::string source = "local M = {}\nfunction M.run()\n" + body + "\nend\nreturn M\n";
        if (auto r = vm->loadModule(name, source); !r.ok()) return r.error().toString();
        auto read = [&](lua_State* L, int base, int count) {
            for (int i = 0; i < count; ++i) numbers->push_back(lua_tonumber(L, base + i));
        };
        auto r = numbers ? vm->callExport(name, "run", {}, read, nresults) : vm->callExport(name, "run");
        return r.ok() ? std::string() : r.error().toString();
    }

private:
    int m_modules = 0;
};

/// Fuel a call costs beyond the uncharged Probe.nop with the same arguments (`args` is Luau).
std::string fuelDelta(const std::string& call, const std::string& args) {
    return "local p = WorldPos.new(0, 0, 0)\nlocal a = task.fuel(); " + call + "(" + args + "); local b = task.fuel()\n" +
           "local c = task.fuel(); Probe.nop(" + args + "); local d = task.fuel()\nreturn (b - a) - (d - c)";
}

TEST_CASE("luau: the generated glue converts arguments and results through a script VM") {
    Vm v;
    CHECK(v.boundQueries == 5); // echo is editor-only
    CHECK(v.boundShips == 2);
    const std::string err = v.run(R"(
  local p = WorldPos.new(1.5, -2, 3, 7)
  local ref = Queries.nearest(p, 10)
  assert(ref ~= nil and typeof(ref) == "RecordRef")
  local e = Queries.spawn(ref, p)
  assert(typeof(e) == "EntityId")
  local near = Queries.inRadius(p, 5)
  assert(#near == 3 and near[1] == e and near[2] ~= e)
  local seen = {}
  seen[e] = true
  assert(seen[near[1]] and not seen[near[2]])
  local hull = ShipQueries.hullOf(e)
  assert(hull ~= nil and ShipQueries.hullOf(near[2]) == nil)
  local docked = ShipQueries.dockedShips(e)
  assert(#docked == 2 and docked[2] == e)
  local item = Queries.convert(true, -8, 2^53, 65535, 0.25, "a.b", "text", vector.create(1, 2, 3), 1.5, 42, "Blue",
    {slot = "s", count = 3}, {{slot = "x"}, {slot = "y", count = 9}}, {"n2", "n1", "n2"}, {1, 2}, nil, "loc.key", "A & B")
  assert(item.slot == "s" and item.count == 7)
  Queries.log("hello")
)");
    REQUIRE_MESSAGE(err.empty(), err);
    CHECK(v.queries.lastAt.frame.value == 7);
    CHECK(v.queries.lastAt.local.x == 1.5);
    CHECK(v.queries.lastAt.local.y == -2.0);
    CHECK(v.queries.lastRadius == 10.0f);
    CHECK(v.queries.lastThing.id == kRefId);
    const auto& c = v.queries.converted;
    CHECK(c.flag);
    CHECK(c.small == -8);
    CHECK(c.big == (i64(1) << 53));
    CHECK(c.count == 65535);
    CHECK(c.ratio == 0.25);
    CHECK(c.name == "a.b");
    CHECK(c.text == "text");
    CHECK(c.dir == helios::Vec3(1, 2, 3));
    CHECK(c.delay == helios::refl::Duration::fromMillis(1500));
    CHECK(c.at == 42);
    CHECK(c.color == golden::all::Color::Blue);
    CHECK(c.item.slot == helios::Name("s"));
    CHECK(c.item.count == 3);
    REQUIRE(c.items.size() == 2);
    CHECK(c.items[0].count == 1); // a missing field takes its schema default
    CHECK(c.items[1].count == 9);
    CHECK(c.names.size() == 2);
    CHECK(c.trio == std::array<f32, 3>{1, 2, 0});
    CHECK_FALSE(c.maybe.has_value());
    CHECK(c.label == "loc.key");
    CHECK(c.query == "A & B");
    CHECK(v.queries.logged == "hello");

    Vm editor("editor");
    CHECK(editor.boundQueries == 1);
    CHECK(editor.boundShips == 0);
    const std::string echoed = editor.run(R"(
  local t = Queries.echo({n = 1, tree = {{n = 2, tree = {}}, {n = 3}}})
  assert(t.n == 1 and #t.tree == 2 and t.tree[2].n == 3 and #t.tree[2].tree == 0)
  assert(Queries.nearest == nil)
)");
    CHECK_MESSAGE(echoed.empty(), echoed);
}

TEST_CASE("luau: the generated glue charges each fn's schema fuel") {
    Vm v;
    std::vector<double> fuel;
    auto delta = [&](const std::string& call, const std::string& args) {
        fuel.clear();
        const std::string err = v.run(fuelDelta(call, args), &fuel, 1);
        REQUIRE_MESSAGE(err.empty(), err);
        REQUIRE(fuel.size() == 1);
        return fuel[0];
    };
    CHECK(delta("Queries.nearest", "p, 1") == 12);
    v.queries.inRadiusCount = 0;
    CHECK(delta("Queries.inRadius", "p, 1") == 20);
    v.queries.inRadiusCount = 7; // `each` per result element, charged after the call
    CHECK(delta("Queries.inRadius", "p, 1") == 20 + 7);
    v.queries.spawnItems = 10; // `of` a record ref: the hand-written count, charged before the call
    CHECK(delta("Queries.spawn", "Queries.nearest(p, 1), p") == 50 + 4 * 10);
    CHECK(delta("Queries.log", "'hello'") == 2 + 5); // `of` a string: its length
    CHECK(delta("ShipQueries.dockedShips", "Queries.spawn(Queries.nearest(p, 1), p)") == 10 + 2);
    const std::string convert =
        "true, 0, 0, 0, 0, 'n', 't', vector.create(0, 0, 0), 0, 0, 'Red', {}, {{}, {}, {}}, {}, {}, nil, 'l', 'q'";
    CHECK(delta("Queries.convert", convert) == 3 + 2 * 3); // `of` a list: its length
}

TEST_CASE("luau: the generated glue rejects hostile arguments without running Luau code") {
    Vm v;
    struct Case {
        const char* body;
        const char* error;
    };
    const std::string args = "true, 0, 0, 0, 0, 'n', 't', vector.create(0, 0, 0), 0, 0, 'Red', {}, {}, {}, {}, nil, 'l', 'q'";
    auto convertWith = [&](int index, const std::string& value) {
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        for (const char ch : args) {
            if (ch == '(' || ch == '{') ++depth;
            if (ch == ')' || ch == '}') --depth;
            if (ch == ',' && depth == 0) {
                parts.push_back(cur);
                cur.clear();
                continue;
            }
            cur += ch;
        }
        parts.push_back(cur);
        parts[static_cast<usize>(index)] = value;
        std::string out;
        for (usize i = 0; i < parts.size(); ++i) out += (i ? "," : "") + parts[i];
        return "Queries.convert(" + out + ")";
    };
    const std::vector<std::pair<std::string, std::string>> cases = {
        {convertWith(0, "1"), "argument 'flag': expected boolean, got number"},
        {convertWith(1, "200"), "200 is not a i8"},
        {convertWith(2, "2^53 + 2"), "is not a i64"},
        {convertWith(3, "1.5"), "1.5 is not a u16"},
        {convertWith(3, "'7'"), "argument 'count': expected number, got string"},
        {convertWith(5, "{}"), "argument 'name': expected string, got table"},
        {convertWith(7, "{1, 2, 3}"), "argument 'dir': expected vector, got table"},
        {convertWith(8, "1e300"), "seconds is not a valid duration"},
        {convertWith(10, "'Purple'"), "'Purple' is not a Color (expected one of Red, Green, Blue)"},
        {convertWith(11, "5"), "argument 'item': expected Item, got number"},
        {convertWith(12, "{{}, {}, {}, {}, {}}"), "argument 'items': more than 4 elements"},
        {convertWith(14, "{1, 2, 3, 4}"), "4 elements, at most 3 allowed"},
        {"Queries.spawn(1, WorldPos.new(0, 0, 0))", "argument 'thing': expected ThingRef, got number"},
        {"local p = WorldPos.new(0, 0, 0); Queries.spawn(Queries.spawn(Queries.nearest(p, 1), p), p)", "expected ThingRef, got EntityId"},
        {"Queries.nearest(vector.create(0, 0, 0), 1)", "argument 'at': expected WorldPos, got vector"},
        {"ShipQueries.hullOf(nil)", "argument 'ship': expected EntityId, got nil"},
    };
    for (const auto& [body, expected] : cases) {
        INFO(body);
        const std::string err = v.run(body);
        CHECK_MESSAGE(err.find(expected) != std::string::npos, err);
    }
    // Struct fields and list elements are read raw: a metatable's __index never runs.
    const std::string raw = v.run(convertWith(11, "setmetatable({}, {__index = function() error('metamethod ran') end})"));
    CHECK_MESSAGE(raw.empty(), raw);
    // A result above the fn's @max fails the call instead of reaching the script.
    v.queries.inRadiusCount = 65;
    CHECK(v.run("Queries.inRadius(WorldPos.new(0, 0, 0), 1)").find("returned 65 elements; its @max is 64") != std::string::npos);

    // A cyclic table stops at the depth limit instead of overflowing the C++ stack; the VM stays usable.
    Vm editor("editor");
    const std::string cyclic = editor.run("local t = {n = 1}\nt.tree = {t}\nQueries.echo(t)");
    CHECK_MESSAGE(cyclic.find("nests more than 32 tables deep") != std::string::npos, cyclic);
    const std::string after = editor.run("assert(Queries.echo({n = 4}).n == 4)");
    CHECK_MESSAGE(after.empty(), after);
}

// --- schema.d.luau under Luau.Analysis ------------------------------------------------------------

struct Sources final : Luau::FileResolver {
    std::map<std::string, std::string> modules;
    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
        auto it = modules.find(name);
        if (it == modules.end()) return std::nullopt;
        return Luau::SourceCode{it->second, Luau::SourceCode::Module};
    }
};

/// Type errors of `script` (strict mode) against helios.d.luau plus the generated definitions `defs`.
std::vector<std::string> typeErrors(const std::string& defs, const std::string& script) {
    Sources sources;
    sources.modules["main"] = "--!strict\n" + script;
    Luau::NullConfigResolver config;
    Luau::Frontend frontend(&sources, &config);
    Luau::registerBuiltinGlobals(frontend, frontend.globals);
    std::vector<std::string> out;
    for (const auto& [name, text] : {std::pair{std::string("@helios"), readFile(HELIOS_SCRIPT_DEFS_PATH)}, std::pair{std::string("@schema"), defs}}) {
        const Luau::LoadDefinitionFileResult r = frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, text, name, false);
        if (!r.success) {
            for (const Luau::ParseError& e : r.parseResult.errors) out.push_back(name + ": " + e.what());
            if (r.module) {
                for (const Luau::TypeError& e : r.module->errors) out.push_back(name + ": " + Luau::toString(e));
            }
            out.push_back(name + ": definitions failed to load");
            return out;
        }
    }
    Luau::freeze(frontend.globals.globalTypes);
    const Luau::CheckResult result = frontend.check("main");
    for (const Luau::TypeError& e : result.errors) out.push_back(std::format("main:{}: {}", e.location.begin.line, Luau::toString(e)));
    return out;
}

TEST_CASE("luau: the generated schema.d.luau type-checks scripts with Luau.Analysis") {
    const std::string golden = readFile(std::string(HELIOS_GOLDEN_LUAU_DIR) + "/schema.d.luau");
    const std::string sample = readFile(std::string(HELIOS_SAMPLE_LUAU_DIR) + "/schema.d.luau");
    REQUIRE_FALSE(golden.empty());
    REQUIRE_FALSE(sample.empty());
    const std::string good = R"(
local p = WorldPos.new(0, 0, 0)
local ref: ThingRef? = Queries.nearest(p, 1)
if ref then
  local e: EntityId = Queries.spawn(ref, p)
  local near: {EntityId} = Queries.inRadius(p, 2)
  local item: Item = Queries.convert(true, 0, 0, 0, 0, "n", "t", vector.create(0, 0, 0), 0, 0, "Green",
    {slot = "s", count = 1}, {}, {"a"}, {1, 2, 3}, nil, "l", "q")
  local t: Late = Queries.echo({n = 1, tree = {}})
  Queries.log(item.slot .. tostring(#near) .. tostring(t.n) .. tostring(e == near[1]))
end
)";
    const std::vector<std::string> none = typeErrors(golden, good);
    CHECK_MESSAGE(none.empty(), helios::schemac::join(none, "\n"));
    const std::vector<std::string> ship =
        typeErrors(sample, "local ships: {EntityId} = ShipQueries.dockedShips(ShipQueries.dockedShips(nil :: any)[1])\n"
                           "local hull: ShipHullRef? = ShipQueries.hullOf(ships[1])\n");
    CHECK_MESSAGE(ship.empty(), helios::schemac::join(ship, "\n"));
    // Wrong argument types, an unknown enum value and a result used as the wrong type are all caught.
    for (const char* bad : {"Queries.nearest(1, 2)", "local x: number = Queries.inRadius(WorldPos.new(0, 0, 0), 1)",
                            "Queries.spawn(Queries.inRadius(WorldPos.new(0, 0, 0), 1)[1], WorldPos.new(0, 0, 0))",
                            "Queries.log(5)", "local c: Color = \"Purple\""}) {
        INFO(bad);
        CHECK_FALSE(typeErrors(golden, bad).empty());
    }
}

// --- lock and diagnostics -----------------------------------------------------------------------

TEST_CASE("luau: scriptlib fns get stable binding ids from the lock") {
    const std::string text = "package test;\nscriptlib Lib @realm(server) {\n  fn a(x: f32) -> f32 @script(cost=1) @pure;\n"
                             "  fn b() @script(cost=2);\n}\n";
    MemoryFileSystem fs;
    CompileOptions options;
    options.lockPath = "schemas/lock.jsonc";
    auto fnDecl = [](const Compiled& c, const std::string& name) -> const Decl* {
        for (const Decl* d : c.schema().decls) {
            if (d->kind == DeclKind::ScriptFn && d->qualifiedName == name) return d;
        }
        return nullptr;
    };
    auto first = compileFiles({{"schemas/test/t.hschema", text}}, options, &fs);
    REQUIRE_MESSAGE(first->ok(), first->messages);
    const Decl* a = fnDecl(*first, "test.Lib.a");
    REQUIRE(a);
    CHECK(a->typeId != 0);
    CHECK(first->result.lockText.find("\"test.Lib.a\": {\n      \"id\": " + std::to_string(a->typeId) + ",\n      \"kind\": \"fn\"\n    }") !=
          std::string::npos);
    const u32 id = a->typeId;
    // A later run keeps the id even when fns are added before it; a removed fn keeps its entry.
    fs.files["schemas/lock.jsonc"] = first->result.lockText;
    const std::string edited = "package test;\nscriptlib Lib @realm(server) {\n  fn z() @script(cost=1);\n"
                               "  fn a(x: f32) -> f32 @script(cost=1) @pure;\n}\n";
    auto second = compileFiles({{"schemas/test/t.hschema", edited}}, options, &fs);
    REQUIRE_MESSAGE(second->ok(), second->messages);
    REQUIRE(fnDecl(*second, "test.Lib.a"));
    CHECK(fnDecl(*second, "test.Lib.a")->typeId == id);
    CHECK(second->result.lockText.find("\"test.Lib.b\"") != std::string::npos);
    // A fn entry carries nothing but its id: fields in it are a hand edit.
    fs.files["schemas/lock.jsonc"] = "{\"format\": 1, \"types\": {\"test.Lib.a\": {\"id\": 5, \"kind\": \"fn\", \"fields\": []}}}";
    auto broken = compileFiles({{"schemas/test/t.hschema", text}}, options, &fs);
    CHECK_FALSE(broken->ok());
    CHECK(broken->messages.find("a fn entry has only 'id' and 'kind'") != std::string::npos);
}

TEST_CASE("luau: signatures that cannot cross the Luau boundary are errors") {
    CompileOptions options;
    options.emitLuau = true;
    const std::string pre = "struct S { p: WorldPos }\nstruct V { v: variant { A; B } }\nrecord R { x: u8 }\n";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"fn f(m: map<string, u8>) @script(cost=1);", "parameter 'm' of fn 'f' cannot cross the Luau boundary: 'map<string,u8>'"},
        {"fn f(g: Guid) @script(cost=1);", "'Guid' is not supported by --emit luau"},
        {"fn f() -> S @script(cost=1);", "a WorldPos inside a struct"},
        {"fn f(v: V) @script(cost=1);", "in field 'v' of 'test.V'"},
        {"fn end() @script(cost=1);", "fn name 'end' is a Luau keyword"},
        {"fn f(then: u8) @script(cost=1);", "parameter name 'then' is a Luau keyword"},
        {"fn f(L: u8) @script(cost=1);", "parameter name 'L' is reserved"},
        {"fn f(r: RRef) @script(cost=1, each=1, of=r);\n  fn fItemCount() @script(cost=1);", "needs the item-count hook 'fItemCount'"},
        {"fn f(t: string) @script(cost=1, each=5000000, of=t);", "exceeds the script host's range"},
        {"fn a_b() @script(cost=1);\n  fn aB() @script(cost=1);", "both get the C++ constant kABBinding"},
    };
    for (const auto& [fns, expected] : cases) {
        INFO(fns);
        const std::string messages = diagnosticsOf(pre + "scriptlib Lib @realm(server) {\n  " + fns + "\n}\n", options);
        CHECK_MESSAGE(messages.find(expected) != std::string::npos, messages);
    }
    CHECK(diagnosticsOf("scriptlib Task @realm(server) { fn f() @script(cost=1); }\n", options).find("would replace a Luau") !=
          std::string::npos);
    CHECK(diagnosticsOf("struct Task { x: u8 }\nscriptlib L2 @realm(server) { fn f() -> Task @script(cost=1); }\n", options)
              .find("which the script host already declares") != std::string::npos);
    // Luau's contextual keywords stay usable as names.
    CHECK(diagnosticsOf("struct K { type: u8 }\nscriptlib Lib @realm(server) { fn f(type: u8, k: K) @script(cost=1); }\n", options).empty());
    // Without --emit luau the same schema compiles (only the Luau glue cannot represent it).
    CHECK(diagnosticsOf("scriptlib Lib @realm(server) { fn f(g: Guid) @script(cost=1); }\n").empty());
}

} // namespace
