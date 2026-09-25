// Lexer and parser: valid grammar (including the normative example of 02 §3.1) and diagnostics
// with file:line:col for malformed input.

#include "lexer.h"
#include "parser.h"
#include "test_util.h"

using namespace schemac_test;

namespace {

// The example from docs/plan/02-engine-runtime.md §3.1, verbatim.
constexpr std::string_view kSpecExample = R"(package game.ship;
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
)";

FileAst parse(std::string_view text, DiagnosticEngine& diags) {
    const u32 file = diags.addFile("t.hschema", std::string(text));
    return parseFile(diags.fileText(file), file, diags);
}

std::string parseErrors(std::string_view text) {
    DiagnosticEngine diags;
    parse(text, diags);
    return formatDiags(diags);
}

} // namespace

TEST_CASE("lexer: tokens, doc comments, strings and locations") {
    DiagnosticEngine diags;
    const std::string text = "/// doc line\nstruct A { x: f32 = -1.5e3 } // tail\n/* block\n comment */ \"s\\\"q\" 0x1F 42 ±";
    const u32 file = diags.addFile("t", text);
    const auto toks = tokenize(diags.fileText(file), file, diags);
    CHECK_FALSE(diags.hasErrors());
    REQUIRE(toks.size() >= 14);
    CHECK(toks[0].kind == Tok::Doc);
    CHECK(toks[0].text == "doc line");
    CHECK(toks[1].isIdent("struct"));
    CHECK(toks[1].loc.line == 2);
    CHECK(toks[1].loc.col == 1);
    CHECK(toks[1].newlineBefore);
    CHECK(toks[3].isPunct("{"));
    CHECK(toks[7].isPunct("="));
    CHECK(toks[8].isPunct("-"));
    CHECK(toks[9].kind == Tok::Float);
    CHECK(toks[9].text == "1.5e3");
    const Token& str = toks[11];
    CHECK(str.kind == Tok::String);
    CHECK(str.loc.line == 4);
    std::string decoded;
    CHECK(decodeString(str.text, decoded));
    CHECK(decoded == "s\"q");
    CHECK(toks[12].kind == Tok::Int);
    CHECK(toks[12].text == "0x1F");
    CHECK(toks[13].text == "42");
    CHECK(toks[14].isPunct("±"));
    CHECK(toks.back().kind == Tok::End);

    std::string out;
    CHECK(decodeString("\"a\\u00e9\\n\"", out));
    CHECK(out == "a\xC3\xA9\n");
    CHECK_FALSE(decodeString("\"\\q\"", out));
    CHECK_FALSE(decodeString("\"\\ud800\"", out));
}

TEST_CASE("lexer: unterminated strings and comments are reported") {
    DiagnosticEngine d1;
    tokenize("x \"abc\ny", 1, d1);
    REQUIRE(d1.errorCount() == 1);
    CHECK(d1.diagnostics()[0].message == "unterminated string literal");
    CHECK(d1.diagnostics()[0].loc.col == 3);
    DiagnosticEngine d2;
    tokenize("a /* never closed\n\n", 1, d2);
    REQUIRE(d2.errorCount() == 1);
    CHECK(d2.diagnostics()[0].message == "unterminated block comment");
}

TEST_CASE("lexer: sources must be UTF-8 (comments and strings are copied into C++ and Go)") {
    CHECK(parseErrors("package p;\n/// caf\xC3\xA9\nstruct A { s: string = \"\xE2\x9C\x93\" }\n").empty());
    const std::string bad = parseErrors("package p;\nstruct A {}\n/// caf\xE9 (Latin-1)\nstruct B {}\n");
    CHECK_MESSAGE(bad.find("t.hschema:3:1: error: invalid UTF-8 (schema files must be UTF-8 encoded)") != std::string::npos, bad);
}

TEST_CASE("parser: the normative example of 02 §3.1 parses") {
    DiagnosticEngine diags;
    const FileAst f = parse(kSpecExample, diags);
    CHECK_MESSAGE(!diags.hasErrors(), formatDiags(diags));
    CHECK(f.package == "game.ship");
    REQUIRE(f.imports.size() == 1);
    CHECK(f.imports[0].path == "helios/world/frames.hschema");
    REQUIRE(f.decls.size() == 14);
    const DeclAst& motion = f.decls[2];
    CHECK(motion.kind == DeclKindAst::Component);
    REQUIRE(motion.attrs.size() == 2);
    CHECK(motion.attrs[0].name == "replicate");
    CHECK(motion.attrs[0].args[0].value == "all");
    CHECK(motion.attrs[1].name == "lod");
    REQUIRE(motion.members.fields.size() == 4);
    const FieldAst& pos = motion.members.fields[0];
    REQUIRE(pos.attrs.size() == 2);
    CHECK(pos.attrs[0].name == "quant");
    REQUIRE(pos.attrs[0].args.size() == 3);
    CHECK(pos.attrs[0].args[0].value == "frame_cell");
    CHECK(pos.attrs[0].args[1].key == "cell");
    CHECK(pos.attrs[0].args[1].value == "4096m");
    CHECK(pos.attrs[0].args[2].value == "1/256m");
    CHECK(motion.members.fields[3].block == Block::Server);
    CHECK(motion.members.fields[3].name == "lastInputTick");
    const DeclAst& rel = f.decls[3];
    CHECK(rel.kind == DeclKindAst::Relation);
    CHECK_FALSE(rel.hasBody);
    CHECK(rel.attrs[2].args[0].value == "helios.world.ReferenceFrame");
    const DeclAst& hull = f.decls[4];
    CHECK(hull.doc == "A hull type; instances live in content/records/hull/*.hrec.");
    CHECK(hull.attrs[0].args[0].isString);
    CHECK(hull.attrs[0].args[0].value == "hull");
    const FieldAst& handling = hull.members.fields[4];
    CHECK(handling.type->kind == TypeExpr::Kind::InlineStruct);
    CHECK(handling.type->members->fields.size() == 3);
    CHECK(handling.type->members->fields[0].attrs[0].args[0].value == "deg/s");
    CHECK(hull.members.fields[5].block == Block::Client);
    CHECK(hull.members.fields[7].type->kind == TypeExpr::Kind::Optional);
    CHECK(hull.members.fields[8].type->kind == TypeExpr::Kind::Generic);
    const DeclAst& rpc = f.decls[8];
    CHECK(rpc.kind == DeclKindAst::Rpc);
    CHECK(rpc.direction == "client->server");
    REQUIRE(rpc.params.size() == 2);
    CHECK(rpc.attrs[0].name == "reliable");
    CHECK(rpc.attrs[1].args[0].value == "2/s");
    const DeclAst& svc = f.decls[9];
    REQUIRE(svc.members.rpcs.size() == 1);
    CHECK(svc.members.rpcs[0].result->name == "LedgerResult");
    CHECK(svc.members.rpcs[0].attrs[1].args[0].value == "500ms");
    const DeclAst& formula = f.decls[10];
    CHECK(formula.formulaParams == std::vector<std::string>{"ship"});
    CHECK(formula.exprText == "attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81)");
    CHECK(f.decls[11].attrs[0].name == "client");
    const DeclAst& physics = f.decls[12];
    CHECK(physics.kind == DeclKindAst::ScriptLib);
    CHECK(physics.name == "Physics");
    REQUIRE(physics.attrs.size() == 1);
    CHECK(physics.attrs[0].name == "realm");
    REQUIRE(physics.attrs[0].args.size() == 2);
    CHECK(physics.attrs[0].args[1].value == "client");
    REQUIRE(physics.members.fns.size() == 2);
    const FnAst& raycast = physics.members.fns[0];
    CHECK(raycast.name == "raycast");
    REQUIRE(raycast.params.size() == 4);
    CHECK(raycast.params[2].name == "maxDist");
    CHECK(raycast.params[2].attrs[0].name == "unit");
    CHECK(raycast.result->kind == TypeExpr::Kind::Optional);
    REQUIRE(raycast.attrs.size() == 2); // attributes on the continuation line
    CHECK(raycast.attrs[0].name == "script");
    CHECK(raycast.attrs[0].args[0].key == "cost");
    CHECK(raycast.attrs[1].name == "pure");
    const FnAst& overlap = physics.members.fns[1];
    CHECK(overlap.result->name == "list");
    REQUIRE(overlap.attrs.size() == 3);
    CHECK(overlap.attrs[0].name == "max");
    CHECK(overlap.attrs[1].args[2].key == "of");
    CHECK(overlap.attrs[1].args[2].value == "result");
    const DeclAst& world = f.decls[13];
    REQUIRE(world.members.fns.size() == 1);
    CHECK(world.members.fns[0].attrs[0].args[2].value == "prefab");
}

TEST_CASE("parser: the 04 replication snippet parses (bare field attributes, ±, rate)") {
    DiagnosticEngine diags;
    parse(R"(package net;
component ShipMotion replicate(all) lod(core) {
  pos    : WorldPos @quant(frame_cell, cell=4096m, res=1/256m)   // §4.5
  rot    : quatf    @quant(smallest3, bits=10)                  // 32 bits
  vel    : vec3f    @quant(range=±4096, bits=16)
  angvel : vec3f    @quant(range=±8, bits=10)        lod(near)
}
rpc ActivateModule(slot: u8, target: NetHandle) client->server reliable rate(10/s)
)"
          "@intent(use);\n",
          diags);
    CHECK_MESSAGE(!diags.hasErrors(), formatDiags(diags));
}

TEST_CASE("parser: types, literals and enum bodies") {
    DiagnosticEngine diags;
    const FileAst f = parse(R"(package p;
struct S {
  a: map<Name, list<u8>>
  b: u8[4]?
  c: list<{ x: i32 }>
  d: variant { A; B{ v: f32 }, C }
  e: enum { X, Y = 5 } = Y
  f: vec3f = [0, 0, -1]
  g: Duration = 1.5s
  h: string = "q\"s"
  i: i64 = -9223372036854775808
  j: u32 = 0xFF_FF
}
flags F : u16 {
  /// first
  A
  B = 4, C
}
const Pi: f64 = 3.25;
alias Money = i64;
alias Point = { x: f32; y: f32 };
)",
                          diags);
    CHECK_MESSAGE(!diags.hasErrors(), formatDiags(diags));
    const auto& fields = f.decls[0].members.fields;
    REQUIRE(fields.size() == 10);
    CHECK(fields[0].type->args[1]->name == "list");
    CHECK(fields[1].type->kind == TypeExpr::Kind::Optional);
    CHECK(fields[1].type->args[0]->kind == TypeExpr::Kind::Array);
    CHECK(fields[1].type->args[0]->arraySize == 4);
    CHECK(fields[3].type->alternatives.size() == 3);
    CHECK(fields[3].type->alternatives[1].members->fields.size() == 1);
    CHECK(fields[4].type->enumValues[1].value->text == "5");
    CHECK(fields[4].defaultValue->kind == LiteralAst::Kind::Ident);
    CHECK(fields[5].defaultValue->items.size() == 3);
    CHECK(fields[5].defaultValue->items[2].negative);
    CHECK(fields[6].defaultValue->kind == LiteralAst::Kind::Unit);
    CHECK(fields[6].defaultValue->text == "1.5s");
    CHECK(fields[7].defaultValue->text == "q\"s");
    CHECK(fields[9].defaultValue->text == "0xFFFF");
    CHECK(f.decls[1].enumValues.size() == 3);
    CHECK(f.decls[1].enumValues[0].doc == "first");
    CHECK(f.decls[2].constValue->text == "3.25");
    CHECK(f.decls[4].aliasTarget->kind == TypeExpr::Kind::InlineStruct);
}

TEST_CASE("parser: diagnostics carry file:line:col and recover at the next declaration") {
    struct Case {
        const char* source;
        const char* expected;
    };
    const Case cases[] = {
        {"struct A {}", "t.hschema:1:1: error: expected 'package <name>;' at the start of the file, got 'struct'"},
        {"package p", "t.hschema:1:10: error: expected ';' after the package name, got end of file"},
        {"package p;\nimport foo;", "t.hschema:2:8: error: expected an import path string, got 'foo'"},
        {"package p;\nstruct A {}\nimport \"x\";", "t.hschema:3:1: error: 'import' must come before all declarations"},
        {"package p;\nstructure A {}", "t.hschema:2:1: error: expected a declaration (enum, flags, struct, component, relation, record, event, rpc, message, service, viewmodel, formula, const, alias, scriptlib), got 'structure'"},
        {"package p;\nstruct A { fn go(); }", "t.hschema:2:12: error: fn declarations are only allowed inside a scriptlib"},
        {"package p;\nscriptlib L { x: u8 }", "t.hschema:2:15: error: a scriptlib contains only fn declarations, got 'x'"},
        {"package p;\nscriptlib L { rpc Go(x: u8); }", "t.hschema:2:15: error: a scriptlib contains only fn declarations, got 'rpc'"},
        {"package p;\nscriptlib L { fn go(x: u8) -> u8 @pure }", "t.hschema:2:40: error: expected ';' after fn 'go', got '}'"},
        {"package p;\nscriptlib L { fn (x: u8); }", "t.hschema:2:18: error: expected a function name, got '('"},
        {"package p;\nstruct {}", "t.hschema:2:8: error: expected a name after 'struct', got '{'"},
        {"package p;\nstruct A { x f32 }", "t.hschema:2:14: error: expected ':' after field name 'x', got 'f32'"},
        {"package p;\nstruct A { x: }", "t.hschema:2:15: error: expected a type, got '}'"},
        {"package p;\nstruct A { x: u8 y: u8 }", "t.hschema:2:18: error: expected ';' or a line break after field 'x', got 'y'"},
        {"package p;\nstruct A { x: list<u8 }", "t.hschema:2:23: error: expected '>' to close the type argument list, got '}'"},
        {"package p;\nstruct A { x: u8[0] }", "t.hschema:2:17: error: array size must be between 1 and 65536"},
        {"package p;\nstruct A { x: u8 = }", "t.hschema:2:20: error: expected a literal value, got '}'"},
        {"package p;\nstruct A { x: u8 @ }", "t.hschema:2:20: error: expected an attribute name after '@', got '}'"},
        {"package p;\nstruct A { x: u8 @range(0, )", "t.hschema:2:28: error: empty argument in @range(...)"},
        {"package p;\nstruct A { x: u8 @range(0, }", "t.hschema:2:28: error: expected ')' to close the arguments of @range, got '}'"},
        {"package p;\nstruct A { x: variant { B { server { z: u8 } } } }", "t.hschema:2:29: error: 'server { }' blocks are not allowed here"},
        {"package p;\nstruct A { x: u8 @range(0", "t.hschema:2:19: error: unterminated arguments of @range"},
        {"package p;\nstruct A {\n  x: u8\n", "t.hschema:4:1: error: unexpected end of file (missing '}')"},
        {"package p;\nenum E { A B }", "t.hschema:2:12: error: expected ';', ',' or a line break after enum value 'A', got 'B'"},
        {"package p;\nenum E { 1 }", "t.hschema:2:10: error: expected an enum value name, got '1'"},
        {"package p;\nstruct A { server { server { x: u8 } } }", "t.hschema:2:21: error: client/server/editor blocks cannot be nested"},
        {"package p;\nstruct A { rpc Go(x: u8); }", "t.hschema:2:12: error: rpc declarations are only allowed inside a service"},
        {"package p;\nrpc Go(x: u8 = 1) client->server;", "t.hschema:2:14: error: parameters cannot have default values"},
        {"package p;\nrpc Go() client->client;", "t.hschema:2:10: error: invalid direction 'client->client'"},
        {"package p;\nrpc Go() client->server server->client;", "t.hschema:2:25: error: rpc has more than one direction"},
        {"package p;\nstruct A client->server {}", "t.hschema:2:10: error: a message direction is only valid on an rpc"},
        {"package p;\nformula F(x) = 1 + 2", "t.hschema:2:16: error: unterminated formula body (missing ';')"},
        {"package p;\nconst X = 1;", "t.hschema:2:9: error: expected ':' and a type after const 'X', got '='"},
        {"package p;\nalias X u8;", "t.hschema:2:9: error: expected '=' and a type after alias 'X', got 'u8'"},
        {"package p;\nstruct A { x: variant { B C } }", "t.hschema:2:27: error: expected ';' or a line break after alternative 'B', got 'C'"},
        {"package p;\nstruct A { x: string = \"\\q\" }", "t.hschema:2:24: error: invalid escape sequence in string"},
        {"package p;\nstruct A { x: i8 = -true }", "t.hschema:2:21: error: expected a number after '-', got 'true'"},
        {"package p;\nstruct A x", "t.hschema:2:11: error: expected '{' or ';' after 'A', got end of file"},
        {"package p;\nstruct A\nstruct B {}", "t.hschema:3:1: error: expected '{' or ';' after 'A', got 'struct'"},
    };
    for (const Case& c : cases) {
        const std::string errors = parseErrors(c.source);
        CHECK_MESSAGE(errors.find(c.expected) != std::string::npos, "source:\n" << std::string(c.source) << "\nexpected:\n" << std::string(c.expected) << "\ngot:\n" << errors);
    }
}

TEST_CASE("parser: nesting is limited (hostile input is a diagnostic, not a stack overflow)") {
    auto nestedList = [](usize depth) {
        std::string t;
        for (usize i = 0; i < depth; ++i) t += "list<";
        t += "u8";
        for (usize i = 0; i < depth; ++i) t += ">";
        return t;
    };
    // 63 generics plus the element type: 64 levels, the limit.
    CHECK(parseErrors("package p;\nstruct A { x: " + nestedList(63) + " }\n").empty());
    const std::string tooDeep = parseErrors("package p;\nstruct A { x: " + nestedList(64) + " }\n");
    CHECK_MESSAGE(tooDeep.find("error: nesting too deep (more than 64 levels of types, bodies or list literals)") != std::string::npos, tooDeep);
    // Far beyond any stack: still one diagnostic, and the next declaration parses.
    DiagnosticEngine diags;
    const FileAst f = parse("package p;\nstruct A { x: " + nestedList(200'000) + " }\nstruct B { y: u8 }\n", diags);
    CHECK(diags.errorCount() == 1);
    REQUIRE(f.decls.size() == 1);
    CHECK(f.decls[0].name == "B");
    // Inline struct bodies, optionals/arrays and list literals count too.
    std::string inlineDeep = "package p;\nstruct A { x: ";
    for (int i = 0; i < 100; ++i) inlineDeep += "{ a: ";
    inlineDeep += "u8";
    for (int i = 0; i < 100; ++i) inlineDeep += " }";
    inlineDeep += " }\n";
    CHECK(parseErrors(inlineDeep).find("nesting too deep") != std::string::npos);
    CHECK(parseErrors("package p;\nconst X: u8 = " + std::string(100'000, '[') + ";\n").find("nesting too deep") != std::string::npos);
    CHECK(parseErrors("package p;\nstruct A { x: u8" + std::string(100'000, '?') + " }\n").find("error") != std::string::npos);
}

TEST_CASE("parser: one error per declaration, then recovery") {
    DiagnosticEngine diags;
    const FileAst f = parse("package p;\nstruct A { x f32; y f64 }\nstruct B { ok: u8 }\nenum C { }\nstruct D { z: }\n", diags);
    // A and D are broken (one error each); B and C still parse.
    CHECK(diags.errorCount() == 2);
    REQUIRE(f.decls.size() == 2);
    CHECK(f.decls[0].name == "B");
    CHECK(f.decls[1].name == "C");
}
