// Semantic analysis: name resolution, imports, attribute validation, security lints, defaults,
// component splitting and the error messages for invalid schemas.

#include <algorithm>

#include "test_util.h"

using namespace schemac_test;

namespace {

// Stub imports for the normative example of 02 §3.1 (types it references from other files).
const std::map<std::string, std::string> kSpecStubs = {
    {"schemas/helios/world/frames.hschema", R"(package helios.world;
/// A reference frame entity (planet, station, ship interior).
component ReferenceFrame { radius: f64 }
)"},
};

// The example from docs/plan/02-engine-runtime.md §3.1 plus the declarations it refers to.
const std::string kSpecExampleWithDeps = R"(package game.ship;
import "helios/world/frames.hschema";

enum ShipSize : u8 { Small; Medium; Large; Capital }
struct ThrusterMount {
  bone:     Name
  dir:      vec3f        @normalized
  maxForce: f32 = 50000  @unit(N) @range(0, 1e8) @editor(category="Thrust")
}
component ShipMotion replicate(all) lod(core) {        // header sugar for @replicate(all) @lod(core)
  pos:    WorldPos @quant(frame_cell, cell=4096m, res=1/256m) @predicted
  rot:    quatf    @quant(smallest3, bits=10)          @predicted @interp(slerp)
  vel:    vec3f    @quant(range=4096, bits=16)         @predicted
  server { lastInputTick: u32 }                        // → ShipMotion::Server, never replicated
}
relation InFrame @exclusive @acyclic @target(helios.world.ReferenceFrame)

/// A hull type; instances live in content/records/hull/*.hrec.
record ShipHullDef @table("hull") {                    // also declares ShipHullRef
  name:      LocString
  size:      ShipSize
  mass:      f32 @unit(kg) @range(100, 1e9)
  thrusters: list<ThrusterMount> @keyed @max(64)       // elements carry stable "$key" GUIDs
  handling:  { pitchRate: f32 @unit(deg/s); yawRate: f32; rollRate: f32 }
  client { prefab: AssetRef<Prefab>; icon: AssetRef<Texture> }
  server { lootTable: LootTableRef?; aiHints: map<Name, f32> }
}
struct  HullDamage @store(checkpoint) @version(2) { hp: f32; breaches: list<u8> @was("holes") }
struct  Ammo @store(ledger) @ledger_policy(batched_consume) @lifecycle(decay=30d) { rounds: u32 }
event   ShipDestroyed @audience(relevant) { ship: EntityId; killer: EntityId? }
rpc     RequestDock(target: NetHandle, bay: u8) client->server reliable @ratelimit(2/s) @intent(interact);
service Ledger @scope(shard) {
  rpc Execute(tx: LedgerTx) -> LedgerResult @idempotent @timeout(500ms) @reason_required;
}
formula ThrustToWeight(ship) = attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81);
viewmodel ShipHud @client { speed: f32; throttle: f32; target: TargetVm? }
scriptlib Physics @realm(server, client) {                // C++ in engine/physics/script/, glue generated
  fn raycast(from: WorldPos, dir: vec3f, maxDist: f32 @unit(m), channel: QueryChannel) -> RayHit?
     @script(cost=24) @pure;
  fn overlapSphere(at: WorldPos, radius: f32 @unit(m), filter: TagQuery) -> list<EntityId> @max(256)
     @script(cost=30, each=2, of=result) @pure;
}
scriptlib World @realm(server) {
  fn spawn(prefab: PrefabRef, at: WorldPos) -> EntityId @script(cost=60, each=8, of=prefab);
}

// --- declarations the example refers to ---
record LootTableDef @server_only { rolls: u8 = 1 }
record ReasonCodeDef { code: Name }
struct LedgerTx { reason: ReasonCodeRef; amount: i64 }
struct LedgerResult { ok: bool }
viewmodel TargetVm @client { name: string }
struct RayHit { at: WorldPos; normal: vec3f; entity: EntityId }
enum QueryChannel : u8 { World, Ships, Projectiles }
record PrefabDef { name: Name }
)";

/// Diagnostics for `body` (inside "package test;"), expected to contain `expected`.
struct Case {
    const char* body;
    const char* expected;
};

void checkCases(std::initializer_list<Case> cases) {
    for (const Case& c : cases) {
        const std::string got = diagnosticsOf(c.body);
        CHECK_MESSAGE(got.find(c.expected) != std::string::npos,
                      "source:\n" << std::string(c.body) << "\nexpected:\n" << std::string(c.expected) << "\ngot:\n" << got);
    }
}

} // namespace

TEST_CASE("sema: the normative example of 02 §3.1 compiles without diagnostics") {
    std::map<std::string, std::string> files = kSpecStubs;
    files.emplace("schemas/game/ship.hschema", kSpecExampleWithDeps);
    CompileOptions options;
    options.files = {"schemas/game/ship.hschema"};
    options.emitCpp = true;
    options.emitGo = true;
    options.emitJson = true;
    auto c = compileFiles(files, options);
    CHECK_MESSAGE(c->ok(), c->messages);
    CHECK_MESSAGE(c->diags.diagnostics().empty(), c->messages);

    const Decl* motion = c->decl("game.ship.ShipMotion");
    REQUIRE(motion);
    CHECK(motion->kind == DeclKind::Component);
    CHECK(motion->replicate == RepAudience::All);
    CHECK(motion->lod == "core");
    REQUIRE(motion->fields.size() == 3); // lastInputTick moved to ShipMotion.Server
    for (const Field& f : motion->fields) {
        CHECK(f.replicated);
        CHECK(f.predicted);
    }
    CHECK(motion->fields[2].repIndex == 2);
    REQUIRE(motion->serverPart);
    CHECK(motion->serverPart->kind == DeclKind::ComponentPart);
    CHECK(motion->serverPart->qualifiedName == "game.ship.ShipMotion.Server");
    REQUIRE(motion->serverPart->fields.size() == 1);
    CHECK(motion->serverPart->fields[0].name == "lastInputTick");
    CHECK_FALSE(motion->serverPart->fields[0].replicated);

    const Decl* hull = c->decl("game.ship.ShipHullDef");
    REQUIRE(hull);
    CHECK(hull->kind == DeclKind::Record);
    CHECK(hull->doc == "A hull type; instances live in content/records/hull/*.hrec.");
    CHECK(c->decl("game.ship.ShipHullDef.Handling"));
    const Field* thrusters = nullptr;
    const Field* lootTable = nullptr;
    const Field* icon = nullptr;
    for (const Field& f : hull->fields) {
        if (f.name == "thrusters") thrusters = &f;
        if (f.name == "lootTable") lootTable = &f;
        if (f.name == "icon") icon = &f;
    }
    REQUIRE(thrusters);
    CHECK(thrusters->type->kind == TypeKind::KeyedList);
    CHECK(thrusters->type->signature == "keyed<game.ship.ThrusterMount>");
    REQUIRE(lootTable);
    CHECK(lootTable->serverOnly);
    CHECK(lootTable->type->signature == "Ref<game.ship.LootTableDef>?");
    REQUIRE(icon);
    CHECK(icon->clientOnly);
    CHECK(icon->type->kind == TypeKind::AssetRef);
    CHECK(icon->type->assetKind == "Texture");

    const Decl* mount = c->decl("game.ship.ThrusterMount");
    REQUIRE(mount);
    REQUIRE(mount->fields[2].defaultValue);
    CHECK(mount->fields[2].defaultValue->json == "50000");

    const Decl* damage = c->decl("game.ship.HullDamage");
    REQUIRE(damage);
    CHECK(damage->version == 2);
    CHECK(damage->fields[1].was == std::vector<std::string>{"holes"});

    const Decl* dock = c->decl("game.ship.RequestDock");
    REQUIRE(dock);
    CHECK(dock->kind == DeclKind::Rpc);
    CHECK(dock->direction == "client->server");
    CHECK(dock->fields.size() == 2);
    const Decl* execute = c->decl("game.ship.LedgerExecuteRequest");
    REQUIRE(execute);
    CHECK(execute->service == c->decl("game.ship.Ledger"));
    CHECK(execute->rpcName == "Execute");
    REQUIRE(execute->result);
    CHECK(execute->result->decl == c->decl("game.ship.LedgerResult"));

    const Decl* formula = c->decl("game.ship.ThrustToWeight");
    REQUIRE(formula);
    CHECK(formula->kind == DeclKind::Formula);
    CHECK(formula->formulaBody == "attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81)");

    const Decl* physics = c->decl("game.ship.Physics");
    REQUIRE(physics);
    CHECK(physics->kind == DeclKind::ScriptLib);
    REQUIRE(physics->methods.size() == 2);
    const Decl* raycast = physics->methods[0];
    CHECK(raycast->kind == DeclKind::ScriptFn);
    CHECK(raycast->qualifiedName == "game.ship.Physics.raycast");
    REQUIRE(raycast->fields.size() == 4);
    CHECK(raycast->fields[3].type->decl == c->decl("game.ship.QueryChannel"));
    REQUIRE(raycast->result);
    CHECK(raycast->result->signature == "game.ship.RayHit?");
    CHECK(raycast->cost.base == 24);
    CHECK(raycast->cost.each == 0);
    CHECK(raycast->cost.pure);
    CHECK(raycast->cost.realms == std::vector<std::string>{"server", "client"});
    const Decl* overlap = physics->methods[1];
    CHECK(overlap->cost.base == 30);
    CHECK(overlap->cost.each == 2);
    CHECK(overlap->cost.of == "result");
    const Decl* spawn = c->decl("game.ship.World")->methods[0];
    CHECK(spawn->cost.of == "prefab");
    CHECK_FALSE(spawn->cost.pure);
    CHECK(spawn->cost.realms == std::vector<std::string>{"server"});
    CHECK(spawn->fields[0].type->kind == TypeKind::RecordRef);

    CHECK(c->output("game/ship.gen.h"));
    CHECK(c->output("game/ship.gen.cpp"));
    CHECK(c->output("ship.go"));
    const std::string* json = c->output("schema.json");
    REQUIRE(json);
    CHECK(json->find("\"name\": \"game.ship.Physics\"") != std::string::npos);
    CHECK(json->find("\"cost\": {\"base\": 30, \"each\": 2, \"of\": \"result\"}") != std::string::npos);
}

TEST_CASE("sema: scriptlib lints (fuel costs, purity, realms, signatures)") {
    checkCases({
        {"scriptlib L @realm(server) { fn go(x: u8); }", "error: fn 'go' needs @script(cost=n): every script-callable function charges fuel (02 §7.4)"},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(each=2, of=x); }", "error: fn 'go' needs @script(cost=n) (02 §7.4)"},
        {"scriptlib L @realm(server) { fn go(x: u8) -> list<u8> @script(cost=1, each=2, of=result); }",
         "error: fn 'go' charges per result element (of=result) and must be @pure (02 §3.2)"},
        {"scriptlib L @realm(server) { fn go(x: u8) -> u8 @script(cost=1, each=2, of=result) @pure; }",
         "error: of=result needs a list, set or map result, fn 'go' returns u8"},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(cost=1, each=2, of=y); }",
         "error: @script(of=y): fn 'go' has no parameter 'y' (use a parameter name or 'result')"},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(cost=1, each=2); }", "error: @script on fn 'go': each= and of= go together"},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(cost=-1); }", "error: @script(cost=) needs a non-negative integer, got '-1'"},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(24); }", "error: @script on a fn takes named arguments: cost=, each=, of="},
        {"scriptlib L @realm(server) { fn go(x: u8) @script(cost=1, price=2); }", "error: unknown @script argument 'price' (cost, each, of)"},
        {"scriptlib L @realm(moon) { fn go() @script(cost=1); }", "error: invalid @realm(moon): expected server, client or editor"},
        {"scriptlib L @realm(server, server) { fn go() @script(cost=1); }", "error: @realm lists 'server' twice"},
        {"scriptlib L { fn go() @script(cost=1); }", "warning: fn 'go' has no @realm (on it or its scriptlib): no script realm can call it"},
        {"scriptlib L @realm(server) { fn go() @script(cost=1); fn go() @script(cost=2); }", "error: duplicate fn 'go' in scriptlib 'L'"},
        {"scriptlib L @realm(server) { fn go(p: { a: u8 }) @script(cost=1); }",
         "error: inline types are not allowed in the signature of fn 'go'; declare a named type"},
        {"scriptlib L @realm(server) { fn go() -> list<enum { A, B }> @script(cost=1); }",
         "error: inline types are not allowed in the signature of fn 'go'; declare a named type"},
        {"scriptlib L @realm(server) { fn go(x: u8 @predicted) @script(cost=1); }", "error: @predicted is not valid on a fn parameter"},
        {"scriptlib L @pure { fn go() @script(cost=1); }", "error: @pure is not valid on a type"},
        {"scriptlib L @realm(server) { fn go(x: Nope) @script(cost=1); }", "error: unknown type 'Nope'"},
        {"scriptlib L @realm(server) { fn go() -> u8 @max(4) @script(cost=1); }", "error: @max on fn 'go' needs a list, set, map or string result"},
        {"scriptlib L @realm(server) { fn go() @script(cost=1) @replicate(all); }", "error: @replicate is not valid on a scriptlib fn"},
        {"scriptlib L @table(\"x\") { fn go() @script(cost=1); }", "error: @table is not valid on a scriptlib (use @realm, @doc or @deprecated)"},
        {"scriptlib L @realm(server) { fn go() @script(cost=1); }\nstruct A { l: L }", "error: 'test.L' is a scriptlib, not a type"},
        {"struct A @realm(server) { x: u8 }", "error: @realm is only valid on scriptlibs and their fns"},
        {"struct A { x: u8 @pure }", "error: @pure is not valid on a field"},
        {"struct A { x: u8 @script(read, write) }", "error: @script on a field takes one argument: read, write or none"},
    });
    // One diagnostic per mistake (no follow-up errors from the generic field checks).
    CHECK(diagnosticsOf("scriptlib L @realm(server) { fn go(x: u8 @predicted) @script(cost=1); }") ==
          "schemas/test/t.hschema:2:43: error: @predicted is not valid on a fn parameter\n");
    CHECK(diagnosticsOf("scriptlib L @realm(server) @pure { fn go() @script(cost=1); }") ==
          "schemas/test/t.hschema:2:29: error: @pure is not valid on a type\n");
    // A fn's own @realm overrides the scriptlib's; fn and parameter doc comments are kept.
    auto c = compileText(R"(package test;
scriptlib L @realm(server, client) {
  /// Editor-only helper.
  fn edit(path: string) -> u8 @realm(editor) @script(cost=1);
}
)");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK_MESSAGE(c->diags.diagnostics().empty(), c->messages);
    const Decl* edit = c->decl("test.L")->methods[0];
    CHECK(edit->cost.realms == std::vector<std::string>{"editor"});
    CHECK(edit->doc == "Editor-only helper.");
}

TEST_CASE("sema: names resolve across packages, nested scopes and aliases") {
    auto c = compileFiles({
        {"schemas/a/main.hschema", R"(package game.main;
import "a/common.hschema";
import "b/other.hschema";
alias Money = i64;
alias Pos = common.Point;
struct Outer {
  inner: { v: Inner2 }
  money: Money = 5
  p:     Pos
  q:     game.common.Point
  kind:  enum { A, B } = B
  r:     Inner2
}
struct Inner2 { x: u8 }
)"},
        {"schemas/a/common.hschema", "package game.common;\nstruct Point { x: f32; y: f32 }\n"},
        {"schemas/b/other.hschema", "package other;\nstruct Unused { x: u8 }\n"},
    }, CompileOptions{.files = {"schemas/a/main.hschema"}});
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const Decl* outer = c->decl("game.main.Outer");
    REQUIRE(outer);
    CHECK(outer->fields[0].type->decl == c->decl("game.main.Outer.Inner"));
    CHECK(c->decl("game.main.Outer.Inner")->fields[0].type->decl == c->decl("game.main.Inner2"));
    CHECK(outer->fields[1].type->prim == Prim::I64); // aliases resolve to their target
    CHECK(outer->fields[1].defaultValue->json == "5");
    CHECK(outer->fields[2].type->decl == c->decl("game.common.Point"));
    CHECK(outer->fields[3].type == outer->fields[2].type); // types are interned
    CHECK(outer->fields[4].type->decl == c->decl("game.main.Outer.Kind"));
    CHECK(outer->fields[4].defaultValue->json == "\"B\"");
    // Imported declarations are resolved but not generated.
    CHECK(c->decl("game.main.Outer")->emitted);
    CHECK_FALSE(c->decl("game.common.Point")->emitted);
    CHECK_FALSE(c->decl("other.Unused")->emitted);
}

TEST_CASE("sema: imports — missing files, cycles and ambiguous names") {
    auto missing = compileFiles({{"schemas/t.hschema", "package p;\nimport \"nope/x.hschema\";\n"}});
    CHECK_FALSE(missing->ok());
    CHECK(missing->messages.find("schemas/t.hschema:2:8: error: cannot find imported schema 'nope/x.hschema'") != std::string::npos);

    auto cycle = compileFiles({
        {"schemas/a.hschema", "package a;\nimport \"b.hschema\";\n"},
        {"schemas/b.hschema", "package b;\nimport \"a.hschema\";\n"},
    });
    CHECK_FALSE(cycle->ok());
    CHECK_MESSAGE(cycle->messages.find("error: import cycle: schemas/a.hschema -> schemas/b.hschema -> schemas/a.hschema") != std::string::npos,
                  cycle->messages);

    auto ambiguous = compileFiles({
        {"schemas/m.hschema", "package m;\nimport \"x.hschema\";\nimport \"y.hschema\";\nstruct S { p: Point }\n"},
        {"schemas/x.hschema", "package x;\nstruct Point { v: u8 }\n"},
        {"schemas/y.hschema", "package y;\nstruct Point { v: u8 }\n"},
    });
    CHECK_FALSE(ambiguous->ok());
    CHECK_MESSAGE(ambiguous->messages.find("error: 'Point' is ambiguous: it is declared in packages 'x' and 'y' (qualify the name)") !=
                      std::string::npos,
                  ambiguous->messages);

    // The local package wins over imports; a qualified name picks one explicitly.
    auto local = compileFiles({
        {"schemas/m.hschema", "package m;\nimport \"x.hschema\";\nstruct Point { v: u16 }\nstruct S { p: Point; q: x.Point }\n"},
        {"schemas/x.hschema", "package x;\nstruct Point { v: u8 }\n"},
    });
    REQUIRE_MESSAGE(local->ok(), local->messages);
    CHECK(local->decl("m.S")->fields[0].type->decl == local->decl("m.Point"));
    CHECK(local->decl("m.S")->fields[1].type->decl == local->decl("x.Point"));

    // Types of files that are not imported are not visible, even when compiled together.
    auto invisible = compileFiles({
        {"schemas/m.hschema", "package m;\nstruct S { p: x.Point; q: Other }\nstruct Mine { v: u8 }\n"},
        {"schemas/x.hschema", "package x;\nstruct Point { v: u8 }\n"},
        {"schemas/m2.hschema", "package m;\nstruct Other { v: u8 }\n"},
    }, CompileOptions{.files = {"schemas/m.hschema", "schemas/x.hschema", "schemas/m2.hschema"}});
    CHECK_FALSE(invisible->ok());
    CHECK_MESSAGE(invisible->messages.find("schemas/m.hschema:2:15: error: 'x.Point' is declared in 'schemas/x.hschema', which is not imported by "
                                           "'schemas/m.hschema' (add import \"x.hschema\";)") != std::string::npos,
                  invisible->messages);
    CHECK_MESSAGE(invisible->messages.find("error: 'Other' is declared in 'schemas/m2.hschema', which is not imported by") != std::string::npos,
                  invisible->messages);
    CHECK(invisible->messages.find("unknown type") == std::string::npos);
}

TEST_CASE("sema: declaration and type errors") {
    checkCases({
        {"struct A {}\nstruct A {}", "t.hschema:3:8: error: duplicate declaration 'test.A'"},
        {"struct vec3f {}", "t.hschema:2:8: error: 'vec3f' is a built-in type name and cannot be declared"},
        {"record ItemDef {}\nstruct ItemRef {}", "t.hschema:3:8: error: 'test.ItemRef' collides with the reference type declared by record 'test.ItemDef'"},
        {"struct A { x: f33 }", "t.hschema:2:15: error: unknown type 'f33'; did you mean 'f32'?"},
        {"struct A { x: list }", "t.hschema:2:15: error: 'list' needs type arguments, e.g. list<T>"},
        {"struct A { x: map<u8> }", "t.hschema:2:15: error: 'map' takes 2 type arguments, got 1"},
        {"struct A { x: list<bool> }", "t.hschema:2:15: error: list<bool> is not supported; use flags or list<u8>"},
        {"struct A { x: set<f32> }", "t.hschema:2:19: error: set elements must be integers, enums, strings, Names, Guids, EntityIds or record refs, not 'f32'"},
        {"struct A { x: map<vec3f, u8> }", "t.hschema:2:19: error: map keys must be integers, enums, strings, Names, Guids, EntityIds or record refs, not 'vec3f'"},
        {"struct B {}\nstruct A { x: Ref<B> }", "t.hschema:3:19: error: Ref<T> needs a record type, 'test.B' is not a record"},
        {"struct A { x: u8?? }", "error: nested optionals (T? ?) are not supported"},
        {"struct A { x: list<u8>? }", "error: optional containers ('list<u8>?') are not supported: an empty container already means 'none'"},
        {"const C: u8 = 1;\nstruct A { x: C }", "t.hschema:3:15: error: 'test.C' is a const, not a type"},
        {"struct A { x: variant { B; B } }", "error: duplicate alternative 'B'"},
        {"alias X = X;", "error: alias 'test.X' refers to itself"},
        {"enum E : f32 { A }", "t.hschema:2:10: error: the underlying type of enum 'E' must be an integer type (u8, i16, ...)"},
        {"flags F : i8 { A }", "t.hschema:2:11: error: flags need an unsigned underlying type"},
        {"enum E : u8 {}", "error: enum 'E' needs at least one value"},
        {"enum E : u8 { A = 256 }", "t.hschema:2:19: error: value 256 of 'A' does not fit u8"},
        {"enum E : u8 { A = -1 }", "error: value -1 of 'A' does not fit u8"},
        {"enum E : i8 { A = -129 }", "error: value -129 of 'A' does not fit i8"},
        {"enum E : u8 { A = 255, B }", "error: implicit value of 'B' does not fit u8"},
        {"enum E { A, A }", "t.hschema:2:13: error: duplicate value name 'A'"},
        {"enum E { A = 1, B = 1 }", "error: 'B' has the same value (1) as 'A'"},
        {"enum E { A = 1.5 }", "error: the value of 'A' must be an integer"},
        {"struct A { x: u8; x: u16 }", "t.hschema:2:19: error: duplicate field 'x' in 'test.A'"},
        {"struct A { x: list<u8> @keyed }", "error: @keyed needs a list of structs, 'x' is 'list<u8>'"},
        {"struct S {}\nconst C: S = 1;", "error: const 'C' must have a scalar, string or Duration type"},
        {"service S { x: u8 }", "error: a service contains only rpc declarations"},
        {"rpc Go(x: u8);", "error: rpc 'Go' needs a direction (client->server, server->client or server->server)"},
        {"struct B {}\nstruct A : B {}", "error: base types (': Base') are only allowed on enum and flags; embed 'B' as a field instead"},
        {"component C { editor { x: u8 } }", "error: components have no editor {} block; put editor-only data in an @authoring component"},
        {"struct A { b: B }\nstruct B { a: A }", "error: 'test.A' contains itself by value (test.A -> test.B -> test.A); use list<T>, map<K, T> or Ref<T> to break the cycle"},
        {"struct A { a: A? }", "error: 'test.A' contains itself by value"},
        {"struct A { v: variant { X { a: A } } }", "error: 'test.A' contains itself by value"},
        // Names that would not compile as generated C++.
        {"struct int { x: u8 }", "t.hschema:2:8: error: 'int' is a C++ keyword and cannot name a struct"},
        {"enum E { default, other }", "error: 'default' is a C++ keyword and cannot name an enum value"},
        {"struct A { A: u8 }", "error: field 'A' has the name of its type 'A' (not valid in C++); rename it"},
        {"struct A { Inner: { x: u8 } }", "error: field 'Inner' has the name of the nested type 'test.A.Inner' (not valid in C++); rename it"},
        {"component C replicate(all) { allFields: u8 }", "error: field 'allFields' clashes with Mut<C>::kAllFields; rename it"},
    });
    CHECK(compileText("package game.new;\nstruct A { x: u8 }\n")->messages.find(
              "t.hschema:1:9: error: package component 'new' is a C++ keyword (packages become C++ namespaces)") != std::string::npos);
    // Pathological but syntactically fine input is a diagnostic, not a stack overflow.
    std::string aliases;
    for (int i = 0; i < 5000; ++i) aliases += "alias A" + std::to_string(i) + " = A" + std::to_string(i + 1) + ";\n";
    aliases += "alias A5000 = u8;\nstruct S { x: A0 }\n";
    CHECK(diagnosticsOf(aliases).find("is longer than 64 aliases") != std::string::npos);
    std::string chain;
    for (int i = 0; i < 5000; ++i) chain += "struct S" + std::to_string(i) + " { x: S" + std::to_string(i + 1) + " }\n";
    chain += "struct S5000 { x: u8 }\n";
    const std::string chainDiags = diagnosticsOf(chain);
    CHECK(chainDiags.find("is nested more than 64 levels deep by value") != std::string::npos);
    CHECK(std::count(chainDiags.begin(), chainDiags.end(), '\n') == 1); // reported once
    std::map<std::string, std::string> files;
    for (int i = 0; i < 100; ++i)
        files["schemas/c/f" + std::to_string(i) + ".hschema"] = "package c;\nimport \"c/f" + std::to_string(i + 1) + ".hschema\";\n";
    files["schemas/c/f100.hschema"] = "package c;\n";
    CompileOptions deepImports;
    deepImports.files = {"schemas/c/f0.hschema"};
    CHECK(compileFiles(files, deepImports)->messages.find("imports nest more than 64 files deep") != std::string::npos);
}

TEST_CASE("sema: recursion through containers and references is allowed") {
    const std::string d = diagnosticsOf("record Node { kids: list<Node>; next: Ref<Node>?; byName: map<Name, Node> }\n");
    CHECK_MESSAGE(d.empty(), d);
}

TEST_CASE("sema: attribute validation") {
    checkCases({
        {"struct A { x: f32 @rnage(0, 1) }", "t.hschema:2:20: warning: unknown attribute '@rnage'; did you mean '@range'?"},
        {"struct A @predicted { x: f32 }", "t.hschema:2:11: error: @predicted is not valid on a type"},
        {"struct A { x: f32 @replicate(all) }", "error: @replicate is not valid on a field"},
        {"struct A { x: f32 @range(1) }", "error: @range takes 2 arguments, got 1"},
        {"struct A { x: f32 @was }", "error: @was takes at least 1 argument, got 0"},
        {"struct A { x: f32 @keyed(a, b) }", "error: @keyed takes 0 to 1 arguments, got 2"},
        {"struct A { x: f32 @unit(m) @unit(m) }", "error: duplicate attribute @unit"},
        {"struct A @store(disk) { x: u8 }", "t.hschema:2:17: error: invalid @store(disk): expected one of"},
        {"component C @replicate(everyone) { x: u8 }", "error: invalid @replicate(everyone): expected one of"},
        {"struct A @replicate(all) { x: u8 }", "error: @replicate is only valid on components"},
        {"struct A @exclusive { x: u8 }", "error: @exclusive is only valid on relations"},
        {"struct A @table(\"a\") { x: u8 }", "error: @table is only valid on records"},
        {"struct A @client { x: u8 }", "error: @client is only valid on viewmodels (use a client {} block for fields)"},
        {"struct A @store(ledger) @persist { x: u8 }", "error: 'A' is @store(ledger): ledger data is never @persist write-behind (ADR-008)"},
        {"struct A @ledger_policy(batched_consume) { x: u8 }", "error: @ledger_policy requires @store(ledger)"},
        {"struct A @version(0) { x: u8 }", "error: @version needs a positive integer"},
        {"struct A { x: f32 @quant(bits=8) }", "error: @quant is only valid on fields of replicated components"},
        {"struct A { x: f32 @predicted }", "error: @predicted is only valid on fields of replicated components"},
        {"struct A { x: f32 @lod(near) }", "error: @lod is only valid on replicated components and their fields"},
        {"struct A { x: f32 @range(a, 1) }", "error: @range needs two numbers: @range(min, max)"},
        {"struct A { x: f32 @range(2, 1) }", "error: @range min 2 is greater than max 1"},
        {"struct A { x: string @range(0, 1) }", "error: @range needs a numeric field, 'x' is 'string'"},
        {"struct A { x: f32 @step(0) }", "error: @step needs a positive number"},
        {"struct A { x: list<u8> @max(0) }", "error: @max needs a positive integer"},
        {"struct A { x: u8 @max(4) }", "error: @max needs a list, set, map or string field, 'x' is 'u8'"},
        {"struct A { x: u8 @editor(Thrust) }", "error: @editor takes named arguments: category=, widget=, order="},
        {"struct A { x: u8 @editor(colour=\"red\") }", "error: unknown @editor argument 'colour' (category, widget, order, customizer)"},
        {"struct A { x: u8 @editor(order=1.5) }", "error: @editor(order=) needs a 32-bit integer"},
        {"struct A { x: u8 @was(\"x\") }", "error: @was(\"x\") names the field itself"},
        {"struct A { x: u8 @was(\"y\"); y: u8 }", "error: @was(\"y\"): 'y' is still a field of 'A'"},
        {"struct E { id: u8 }\nstruct A { x: list<E> @keyed(nope) }", "error: @keyed(nope): 'test.E' has no field 'nope'"},
        {"struct E { id: f32 }\nstruct A { x: list<E> @keyed(id) }", "error: @keyed(id): key field type 'f32' is not a valid key"},
        {"struct A { x: list<u8> @merge(prepend) }", "error: @merge supports only 'append'"},
        {"struct A { x: u8 @merge(append) }", "error: @merge(append) is only valid on lists"},
        {"struct A { x: f32 @normalized }", "error: @normalized needs a vector or quaternion field"},
        {"struct A { x: string @asset(Texture) }", "error: @asset needs an AssetRef field"},
        {"struct A { x: u8 @opaque }", "warning: @opaque on 'x' has no effect (it references no server-only data)"},
        {"struct A { x: u8 = 7 @range(0, 5) }", "error: default 7 of 'x' is outside @range(0, 5)"},
        // Non-finite numbers would be pasted into generated C++ ("inf.0").
        {"struct A { x: f32 @range(0, inf) }", "error: @range needs two numbers: @range(min, max)"},
        {"struct A { x: f32 @range(nan, 1) }", "error: @range needs two numbers: @range(min, max)"},
        {"struct A { x: f32 @range(0, infinity) }", "error: @range needs two numbers: @range(min, max)"},
        {"struct A { x: f32 @step(inf) }", "error: @step needs a positive number"},
        {"struct A { x: u8 @editor(order=inf) }", "error: @editor(order=) needs a 32-bit integer"},
        {"struct A { x: u8 @editor(order=3e9) }", "error: @editor(order=) needs a 32-bit integer"},
        {"struct A { x: u8 @was(\"\") }", "error: @was(\"\") must name the old field (an identifier)"},
        {"struct A { x: u8 @was(\"a b\") }", "error: @was(\"a b\") must name the old field (an identifier)"},
        {"struct A @was(\"\") { x: u8 }", "error: @was(\"\") must name the old type (a name or qualified name)"},
        {"struct A @was(\"p..B\") { x: u8 }", "error: @was(\"p..B\") must name the old type (a name or qualified name)"},
    });
}

TEST_CASE("sema: security and persistence lints (AAA-SEC-1, AAA-SEC-4, ledger)") {
    checkCases({
        {"rpc Go(x: u8) client->server;", "t.hschema:2:5: error: client->server rpc 'Go' needs @ratelimit(n/s) and @intent(...) (AAA-SEC-1)"},
        {"rpc Go(x: u8) client->server @ratelimit(1/s);", "error: client->server rpc 'Go' needs @intent(...) (AAA-SEC-1)"},
        {"rpc Go(x: u8) client->server @intent(use);", "error: client->server rpc 'Go' needs @ratelimit(n/s) (AAA-SEC-1)"},
        {"rpc Go(x: u8) client->server @intent(use) @ratelimit(2/fortnight);", "error: invalid @ratelimit(2/fortnight): expected a rate like 2/s"},
        {"service S { rpc Go(x: u8) -> u8 @timeout(5parsecs); }", "error: invalid @timeout(5parsecs): expected a duration like 500ms"},
        // AAA-SEC-1 covers client->server rpcs of services too.
        {"service S { rpc Go(x: u8) -> u8 client->server; }", "error: client->server rpc 'S.Go' needs @ratelimit(n/s) and @intent(...) (AAA-SEC-1)"},
        {"struct Tx { amount: i64 }\nservice S { rpc Go(tx: Tx) -> u8 @reason_required; }",
         "error: rpc 'Go' is @reason_required but carries no ReasonCodeRef (directly or in a parameter struct, 06 §4)"},
        {"struct Secret @server_only { seed: u64 }\ncomponent C { s: Secret }",
         "t.hschema:3:15: error: shared field 's' references server-only type 'test.Secret'; mark it @opaque or move it into a server {} block (AAA-SEC-4)"},
        {"record LootDef @server_only { n: u8 }\nstruct Hull { loot: list<LootDef> }",
         "error: shared field 'loot' references server-only type 'test.LootDef'"},
        {"record LootDef @server_only { n: u8 }\nstruct Hull { loot: LootRef? }", "error: shared field 'loot' references server-only type 'test.LootDef'"},
    });
    // Accepted forms: rate limited + intent, server block, @opaque, server-only containers.
    const std::string ok = diagnosticsOf(R"(rpc Go(x: u8) client->server @ratelimit(2/s) @intent(use);
rpc Push(x: u8) server->client;
record LootDef @server_only { n: u8 }
struct Hull { server { loot: LootRef? }; ref: LootRef? @opaque }
struct Server @server_only { loot: LootRef }
record ReasonCodeDef { n: Name }
struct Tx { reason: ReasonCodeRef }
service S { rpc Go(tx: Tx) -> u8 @reason_required; rpc Direct(r: ReasonCodeRef?) -> u8 @reason_required; }
)");
    CHECK_MESSAGE(ok.empty(), ok);
}

TEST_CASE("sema: replicated components — replicated field mask limit and header sugar") {
    std::string many = "component Big replicate(all) {\n";
    for (int i = 0; i < 65; ++i) many += "  f" + std::to_string(i) + ": u8\n";
    many += "}\n";
    CHECK(diagnosticsOf(many).find(
              "error: component 'Big' has 65 replicated fields; at most 64 fit the dirty mask (move some into a server {} block or split the component)") !=
          std::string::npos);
    // 64 replicated plus server-only fields are fine.
    std::string ok = "component Big replicate(owner) {\n";
    for (int i = 0; i < 64; ++i) ok += "  f" + std::to_string(i) + ": u8\n";
    ok += "  server { a: u8; b: u8 }\n}\n";
    auto c = compileText("package test;\n" + ok);
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const Decl* big = c->decl("test.Big");
    CHECK(big->replicate == RepAudience::Owner);
    CHECK(big->fields.size() == 64);
    CHECK(big->serverPart->fields.size() == 2);
    CHECK(big->fields[63].repIndex == 63);

    // Non-replicated component: client block becomes a part too, no replication flags.
    auto plain = compileText("package test;\ncomponent Health { hp: f32 = 100; client { bar: color } }\n");
    REQUIRE_MESSAGE(plain->ok(), plain->messages);
    const Decl* h = plain->decl("test.Health");
    CHECK_FALSE(h->isReplicatedComponent());
    CHECK_FALSE(h->fields[0].replicated);
    REQUIRE(h->clientPart);
    CHECK(h->clientPart->qualifiedName == "test.Health.Client");
    CHECK(h->clientPart->fields[0].name == "bar");
}

TEST_CASE("sema: default values resolve to canonical JSON") {
    auto c = compileText(R"(package test;
enum Mode : u8 { Off, On, Auto = 7 }
flags Bits : u16 { A, B, C }
struct D {
  b:    bool = true
  i8v:  i8 = -128
  u64v: u64 = 18446744073709551615
  hex:  u32 = 0xFF_FF
  f:    f32 = 0.1
  d:    f64 = 1e300
  nz:   f32 = -0
  s:    string = "a\"b\n"
  n:    Name = "engine.main"
  m:    Mode = Auto
  fl:   Bits = [A, C]
  none: Bits = []
  v:    vec3f = [0, 0, -1]
  q:    quatf = [0, 0, 0, 1]
  col:  color = [1, 0.5, 0.25, 1]
  dur:  Duration = 1.5s
  ms:   Duration = 250ms
  g:    Guid = "0f8fad5b-d9cb-469f-a165-70867728950e"
  nul:  u8? = null
  arr:  u8[3] = [1, 2, 3]
  lst:  list<i32> = []
}
const Big: i64 = -9223372036854775808;
const Pi: f64 = 3.14159;
const Label: string = "x";
const Tick2: Duration = 2m;
)");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const Decl* d = c->decl("test.D");
    std::map<std::string, std::string> json;
    for (const Field& f : d->fields) json[f.name] = f.defaultValue ? f.defaultValue->json : "<none>";
    CHECK(json["b"] == "true");
    CHECK(json["i8v"] == "-128");
    CHECK(json["u64v"] == "18446744073709551615");
    CHECK(json["hex"] == "65535");
    CHECK(json["f"] == "0.1"); // shortest f32 representation
    CHECK(json["d"] == "1e+300");
    CHECK(json["nz"] == "-0.0");
    CHECK(json["s"] == "\"a\\\"b\\n\"");
    CHECK(json["n"] == "\"engine.main\"");
    CHECK(json["m"] == "\"Auto\"");
    CHECK(json["fl"] == "[\"A\",\"C\"]");
    CHECK(json["none"] == "[]");
    CHECK(json["v"] == "[0,0,-1]");
    CHECK(json["q"] == "[0,0,0,1]");
    CHECK(json["col"] == "[1,0.5,0.25,1]");
    CHECK(json["dur"] == "\"1500ms\""); // canonical: the largest unit that is exact
    CHECK(json["ms"] == "\"250ms\"");
    CHECK(json["g"] == "\"0f8fad5b-d9cb-469f-a165-70867728950e\"");
    CHECK(json["nul"] == "<none>"); // same as the implicit default
    CHECK(json["arr"] == "[1,2,3]");
    CHECK(json["lst"] == "<none>");
    CHECK(d->fields[4].defaultValue->f == static_cast<f64>(0.1f)); // rounded to f32
    CHECK(d->fields[10].defaultValue->u == 5);                    // A | C bits
    CHECK(c->decl("test.Big")->constValue.i == INT64_MIN);
    CHECK(c->decl("test.Pi")->constValue.f == 3.14159);
    CHECK(c->decl("test.Tick2")->constValue.i == 120'000'000'000);

    checkCases({
        {"struct A { x: u8 = 256 }", "t.hschema:2:20: error: invalid default for field 'x': 256 does not fit u8"},
        {"struct A { x: u8 = -1 }", "error: invalid default for field 'x': u8 is unsigned"},
        {"struct A { x: i64 = -9223372036854775809 }", "error: invalid default for field 'x': -9223372036854775809 does not fit i64"},
        {"struct A { x: u8 = 1.5 }", "error: invalid default for field 'x': expected an integer"},
        {"struct A { x: string = 5 }", "error: invalid default for field 'x': expected a string"},
        {"struct A { x: bool = 1 }", "error: invalid default for field 'x': expected true or false"},
        {"struct A { x: vec3f = [1, 2] }", "error: invalid default for field 'x': expected a list of 3 numbers, e.g. [0, 0, 1]"},
        {"enum E { A }\nstruct S { x: E = Nope }", "t.hschema:3:19: error: invalid default for field 'x': 'Nope' is not a value of E"},
        {"flags F { A }\nstruct S { x: F = [A, Z] }", "error: invalid default for field 'x': 'Z' is not a flag of F"},
        {"struct A { x: Duration = 5 }", "error: invalid default for field 'x': expected a duration like 500ms or 30d"},
        {"struct A { x: Guid = \"nope\" }", "error: invalid default for field 'x': expected a GUID"},
        {"struct A { x: u8 = null }", "error: invalid default for field 'x': expected an integer for u8"},
        {"struct A { x: u8[2] = [1, 2, 3] }", "error: invalid default for field 'x': expected a list of at most 2 values"},
        {"struct B { v: u8 }\nstruct A { x: B = 1 }", "error: invalid default for field 'x': default values are not supported for test.B"},
        {"const C: u8 = 300;", "error: invalid default for const 'C': 300 does not fit u8"},
        // Writers omit default values, so a present-but-empty optional or container would be
        // unencodable if the default were anything else.
        {"struct A { x: u8? = 4 }", "error: invalid default for field 'x': optional fields default to none; only 'null' is accepted"},
        {"struct A { x: list<u8> = [1] }", "error: invalid default for field 'x': container defaults must be empty ([])"},
    });
}

TEST_CASE("sema: naming lints are warnings (and errors with warnings-as-errors)") {
    const std::string text = "package test;\nstruct lower_case { Bad_Field: u8 }\nenum E { lowerValue }\n";
    auto c = compileText(text);
    CHECK(c->ok());
    CHECK(c->messages.find("t.hschema:2:8: warning: type name 'lower_case' should be PascalCase") != std::string::npos);
    CHECK(c->messages.find("warning: field name 'Bad_Field' should be camelCase") != std::string::npos);
    CHECK(c->messages.find("warning: enum value 'lowerValue' should be PascalCase") != std::string::npos);
    CompileOptions strict;
    strict.warningsAsErrors = true;
    auto s = compileText(text, strict);
    CHECK_FALSE(s->ok());
    CompileOptions quiet;
    quiet.namingLints = false;
    auto q = compileText(text, quiet);
    CHECK(q->ok());
    CHECK(q->diags.diagnostics().empty());
}

TEST_CASE("sema: inline types are materialized as nested declarations") {
    auto c = compileText(R"(package test;
record EffectDef {
  shape: variant { Sphere { radius: f32 }; Beam { length: f32; width: f32 }; Burst }
  mode:  enum { Once, Loop } = Loop
  cost:  { energy: f32; heat: f32 = 1 }
  steps: list<{ at: Duration; amount: f32 }>
}
struct Uses { e: EffectRef }
)");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const Decl* shape = c->decl("test.EffectDef.Shape");
    REQUIRE(shape);
    CHECK(shape->kind == DeclKind::Variant);
    REQUIRE(shape->alternatives.size() == 3);
    CHECK(shape->alternatives[0].type->qualifiedName == "test.EffectDef.Shape.Sphere");
    CHECK(shape->alternatives[2].type->fields.empty());
    CHECK(c->decl("test.EffectDef.Mode")->kind == DeclKind::Enum);
    CHECK(c->decl("test.EffectDef.Cost")->fields[1].defaultValue->json == "1");
    const Decl* steps = c->decl("test.EffectDef.Steps"); // inline types are named after the field
    REQUIRE(steps);
    CHECK(c->decl("test.EffectDef")->fields[3].type->element->decl == steps);
    CHECK(c->decl("test.EffectDef")->nested.size() == 4);
    const Decl* uses = c->decl("test.Uses");
    REQUIRE(uses);
    CHECK(uses->fields[0].type->kind == TypeKind::RecordRef); // record EffectDef declares EffectRef
    CHECK(uses->fields[0].type->decl == c->decl("test.EffectDef"));
}
