// The append-only schema lock (02 §3.4): stable ids across reorders and renames, tombstones,
// widening rules, default-change protection, enum value stability, --check-lock and detection of
// hand edits.

#include <format>

#include "lock.h"
#include "test_util.h"
#include "text.h"

using namespace schemac_test;

namespace {

/// Compiles successive versions of one schema against a lock kept in an in-memory file system,
/// "committing" the lock after every successful run (as the build does).
struct LockFixture {
    MemoryFileSystem fs;
    std::string lockPath = "schemas/schema.lock.jsonc";

    std::unique_ptr<Compiled> run(const std::string& body, CompileOptions options = {}) {
        options.lockPath = lockPath;
        options.namingLints = false;
        auto c = compileFiles({{"schemas/test/t.hschema", "package test;\n" + body}}, options, &fs);
        if (c->ok() && c->result.lockChanged && !options.checkLock) fs.files[lockPath] = c->result.lockText;
        return c;
    }
    const std::string& lock() { return fs.files[lockPath]; }

    static u32 fieldId(const Compiled& c, const std::string& type, const std::string& field) {
        const Decl* d = c.decl(type);
        REQUIRE_MESSAGE(d, type);
        for (const Field& f : d->fields) {
            if (f.name == field) return f.id;
        }
        FAIL("no field " << field);
        return 0;
    }
};

bool hasChange(const Compiled& c, const std::string& text) {
    for (const std::string& s : c.result.lockChanges) {
        if (s == text) return true;
    }
    return false;
}

std::string changes(const Compiled& c) {
    std::string out;
    for (const std::string& s : c.result.lockChanges) out += s + "\n";
    return out;
}

} // namespace

TEST_CASE("lock: a new lock mints fnv1a32 type ids and sequential field ids") {
    LockFixture fx;
    auto c = fx.run("struct A { x: u8; y: string = \"hi\" }\nenum E : u8 { P, Q = 5 }\n");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->result.lockChanged);
    CHECK(c->decl("test.A")->typeId == fnv1a32("test.A"));
    CHECK(c->decl("test.E")->typeId == fnv1a32("test.E"));
    CHECK(LockFixture::fieldId(*c, "test.A", "x") == 1);
    CHECK(LockFixture::fieldId(*c, "test.A", "y") == 2);
    CHECK(hasChange(*c, std::format("new type test.A (id {})", fnv1a32("test.A"))));
    CHECK(hasChange(*c, "test.A: new field y (id 2)"));
    CHECK(hasChange(*c, "test.E: new value Q = 5"));
    CHECK(fx.lock().find(R"({"id": 2, "name": "y", "type": "string", "default": "\"hi\""})") != std::string::npos);
    CHECK(fx.lock().starts_with("// helios-schemac schema lock"));

    // Recompiling the same schema changes nothing.
    auto again = fx.run("struct A { x: u8; y: string = \"hi\" }\nenum E : u8 { P, Q = 5 }\n");
    REQUIRE(again->ok());
    CHECK_FALSE(again->result.lockChanged);
    CHECK(again->result.lockChanges.empty());
}

TEST_CASE("lock: ids survive reordering; new fields get fresh ids") {
    LockFixture fx;
    REQUIRE(fx.run("struct A { a: u8; b: u8; c: u8 }")->ok());
    auto c = fx.run("struct A { c: u8; n: f32; a: u8; b: u8 }");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(LockFixture::fieldId(*c, "test.A", "a") == 1);
    CHECK(LockFixture::fieldId(*c, "test.A", "b") == 2);
    CHECK(LockFixture::fieldId(*c, "test.A", "c") == 3);
    CHECK(LockFixture::fieldId(*c, "test.A", "n") == 4);
    CHECK(changes(*c) == "test.A: new field n (id 4)\n");
}

TEST_CASE("lock: @was renames keep the id (fields and types)") {
    LockFixture fx;
    REQUIRE(fx.run("struct Hull { holes: list<u8>; hp: f32 }\nstruct User { h: Hull }")->ok());
    const u32 hullId = fnv1a32("test.Hull");

    auto renamed = fx.run("struct Hull { hp: f32; breaches: list<u8> @was(\"holes\") }\nstruct User { h: Hull }");
    REQUIRE_MESSAGE(renamed->ok(), renamed->messages);
    CHECK(LockFixture::fieldId(*renamed, "test.Hull", "breaches") == 1);
    CHECK(hasChange(*renamed, "test.Hull: renamed field holes -> breaches (id 1)"));
    CHECK(fx.lock().find(R"({"id": 1, "name": "breaches", "type": "list<u8>", "was": ["holes"]})") != std::string::npos);

    // The @was attribute may be dropped later: the lock remembers the new name.
    auto dropped = fx.run("struct Hull { hp: f32; breaches: list<u8> }\nstruct User { h: Hull }");
    REQUIRE(dropped->ok());
    CHECK(LockFixture::fieldId(*dropped, "test.Hull", "breaches") == 1);
    CHECK_FALSE(dropped->result.lockChanged);

    // Type rename: the id moves with it and signatures that mention the old name follow.
    auto type = fx.run("struct HullState @was(\"Hull\") { hp: f32; breaches: list<u8> }\nstruct User { h: HullState }");
    REQUIRE_MESSAGE(type->ok(), type->messages);
    CHECK(type->decl("test.HullState")->typeId == hullId);
    CHECK(LockFixture::fieldId(*type, "test.HullState", "hp") == 2);
    CHECK(hasChange(*type, std::format("renamed type test.Hull -> test.HullState (id {})", hullId)));
    CHECK(fx.lock().find("\"test.Hull\":") == std::string::npos);
    CHECK(fx.lock().find(R"({"id": 1, "name": "h", "type": "test.HullState"})") != std::string::npos);
    CHECK(fx.lock().find(R"("was": ["test.Hull"])") != std::string::npos);
}

TEST_CASE("lock: removed fields become tombstones; their ids are never reused") {
    LockFixture fx;
    REQUIRE(fx.run("struct A { a: u8; b: u8 }")->ok());
    auto removed = fx.run("struct A { a: u8 }");
    REQUIRE(removed->ok());
    CHECK(changes(*removed) == "test.A: removed field b (id 2 is retired)\n");
    CHECK(fx.lock().find(R"({"id": 2, "name": "b", "type": "u8", "tombstone": true})") != std::string::npos);

    auto added = fx.run("struct A { a: u8; c: u8 }");
    REQUIRE(added->ok());
    CHECK(LockFixture::fieldId(*added, "test.A", "c") == 3);

    auto revived = fx.run("struct A { a: u8; b: u8; c: u8 }");
    REQUIRE(revived->ok());
    CHECK(LockFixture::fieldId(*revived, "test.A", "b") == 2);
    CHECK(hasChange(*revived, "test.A: revived field b (id 2)"));

    // A @was can also claim a tombstone.
    REQUIRE(fx.run("struct A { a: u8; c: u8 }")->ok());
    auto claimed = fx.run("struct A { a: u8; c: u8; bee: u8 @was(\"b\") }");
    REQUIRE(claimed->ok());
    CHECK(LockFixture::fieldId(*claimed, "test.A", "bee") == 2);
}

TEST_CASE("lock: field types may only widen") {
    CHECK(isWidening("u8", "u32"));
    CHECK(isWidening("i8", "i64"));
    CHECK(isWidening("f32", "f64"));
    CHECK(isWidening("u8", "u16?"));
    CHECK(isWidening("test.P", "test.P?"));
    CHECK(isWidening("i16?", "i32?"));
    CHECK_FALSE(isWidening("u32", "u8"));
    CHECK_FALSE(isWidening("i32", "u32"));
    CHECK_FALSE(isWidening("u32", "i64"));
    CHECK_FALSE(isWidening("f64", "f32"));
    CHECK_FALSE(isWidening("u8?", "u8"));
    CHECK_FALSE(isWidening("string", "Name"));
    CHECK_FALSE(isWidening("list<u8>", "list<u16>"));

    LockFixture fx;
    REQUIRE(fx.run("struct P { v: u8 }\nstruct A { n: u8; s: i32; f: f32; p: P; q: string }")->ok());
    auto widened = fx.run("struct P { v: u8 }\nstruct A { n: u64; s: i64; f: f64; p: P?; q: string }");
    REQUIRE_MESSAGE(widened->ok(), widened->messages);
    CHECK(hasChange(*widened, "test.A: field n type u8 -> u64"));
    CHECK(hasChange(*widened, "test.A: field p type test.P -> test.P?"));
    CHECK(fx.lock().find(R"({"id": 1, "name": "n", "type": "u64"})") != std::string::npos);

    auto narrowed = fx.run("struct P { v: u8 }\nstruct A { n: u8; s: i64; f: f64; p: P?; q: Name }");
    CHECK_FALSE(narrowed->ok());
    CHECK(narrowed->messages.find("schemas/test/t.hschema:3:12: error: the type of field 'n' changed from 'u64' to 'u8' (lock id 1); only "
                                  "widenings keep the id (i32→i64, u8→u32, f32→f64, T→T?) — add a new field instead") != std::string::npos);
    CHECK(narrowed->messages.find("error: the type of field 'q' changed from 'string' to 'Name' (lock id 5)") != std::string::npos);
    // The lock on disk is untouched by a failed run.
    CHECK(fx.lock().find(R"({"id": 1, "name": "n", "type": "u64"})") != std::string::npos);
}

TEST_CASE("lock: explicit defaults are part of the wire contract") {
    LockFixture fx;
    REQUIRE(fx.run("struct A { hp: f32 = 100; n: u8 }")->ok());
    auto changed = fx.run("struct A { hp: f32 = 150; n: u8 }");
    CHECK_FALSE(changed->ok());
    CHECK(changed->messages.find("error: the default of field 'hp' changed from 100 to 150 (lock id 1); writers omit default values") !=
          std::string::npos);
    auto added = fx.run("struct A { hp: f32 = 100; n: u8 = 1 }");
    CHECK_FALSE(added->ok());
    CHECK(added->messages.find("the default of field 'n' changed from <implicit> to 1 (lock id 2)") != std::string::npos);

    CompileOptions allow;
    allow.allowDefaultChange = true;
    auto allowed = fx.run("struct A { hp: f32 = 150; n: u8 }", allow);
    REQUIRE_MESSAGE(allowed->ok(), allowed->messages);
    CHECK(hasChange(*allowed, "test.A: field hp default 100 -> 150"));
    CHECK(fx.run("struct A { hp: f32 = 150; n: u8 }")->ok());
    // Spelling the same value differently is not a change (defaults are compared canonically).
    auto same = fx.run("struct A { hp: f32 = 1.5e2; n: u8 = 0x0 }");
    CHECK_FALSE(same->ok()); // n: explicit 0 differs from the implicit default in the lock
    auto spelled = fx.run("struct A { hp: f32 = 1.5e2; n: u8 }");
    CHECK_MESSAGE(spelled->ok(), spelled->messages);
    CHECK_FALSE(spelled->result.lockChanged);
}

TEST_CASE("lock: enum values are stable and never reused") {
    LockFixture fx;
    REQUIRE(fx.run("enum E : u8 { A, B, C }")->ok());
    auto appended = fx.run("enum E : u8 { A, B, C, D }");
    REQUIRE(appended->ok());
    CHECK(changes(*appended) == "test.E: new value D = 3\n");

    auto moved = fx.run("enum E : u8 { A, C, B, D }");
    CHECK_FALSE(moved->ok());
    CHECK(moved->messages.find("error: the value of 'test.E.C' changed from 2 to 1; enum values are stable (append new values instead)") !=
          std::string::npos);

    auto removed = fx.run("enum E : u8 { A, B = 1, D = 3 }");
    REQUIRE_MESSAGE(removed->ok(), removed->messages);
    CHECK(changes(*removed) == "test.E: removed value C (2 is retired)\n");
    auto reused = fx.run("enum E : u8 { A, B = 1, D = 3, X = 2 }");
    CHECK_FALSE(reused->ok());
    CHECK(reused->messages.find("error: value 2 of 'test.E.X' was used by 'C' (removed); pick a new value") != std::string::npos);
    auto revived = fx.run("enum E : u8 { A, B = 1, C = 2, D = 3 }");
    REQUIRE(revived->ok());
    CHECK(changes(*revived) == "test.E: revived value C\n");

    auto widened = fx.run("enum E : u16 { A, B = 1, C = 2, D = 3 }");
    REQUIRE(widened->ok());
    CHECK(changes(*widened) == "test.E: underlying type u8 -> u16\n");
    auto narrowed = fx.run("enum E : u8 { A, B = 1, C = 2, D = 3 }");
    CHECK_FALSE(narrowed->ok());
    CHECK(narrowed->messages.find("error: the underlying type of 'test.E' changed from u16 to u8; only widenings are allowed") !=
          std::string::npos);
}

TEST_CASE("lock: variant alternatives, kinds and versions") {
    LockFixture fx;
    REQUIRE(fx.run("struct V @version(2) { s: variant { A { x: u8 }; B; C } }")->ok());
    auto c = fx.run("struct V @version(3) { s: variant { C; A { x: u8 }; D } }");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const Decl* v = c->decl("test.V.S");
    REQUIRE(v);
    std::map<std::string, u32> ids;
    for (const Alternative& a : v->alternatives) ids[a.name] = a.id;
    CHECK(ids["A"] == 1);
    CHECK(ids["C"] == 3);
    CHECK(ids["D"] == 4);
    CHECK(hasChange(*c, "test.V.S: removed alternative B"));
    CHECK(hasChange(*c, "test.V: version 2 -> 3"));

    auto down = fx.run("struct V @version(1) { s: variant { C; A { x: u8 }; D } }");
    CHECK_FALSE(down->ok());
    CHECK(down->messages.find("error: @version of 'test.V' decreased from 3 to 1") != std::string::npos);

    auto kind = fx.run("enum V { A }");
    CHECK_FALSE(kind->ok());
    CHECK(kind->messages.find(std::format("error: 'test.V' was a struct (lock id {}); a type cannot change kind — declare a new type instead",
                                          fnv1a32("test.V"))) != std::string::npos);
}

TEST_CASE("lock: entries of types outside the compilation are kept") {
    LockFixture fx;
    REQUIRE(fx.run("struct A { a: u8 }")->ok());
    // A second schema set sharing the lock.
    CompileOptions other;
    other.lockPath = fx.lockPath;
    other.namingLints = false;
    auto b = compileFiles({{"schemas/other/o.hschema", "package other;\nstruct B { b: u8 }\n"}}, other, &fx.fs);
    REQUIRE_MESSAGE(b->ok(), b->messages);
    CHECK(b->result.lockText.find("\"test.A\"") != std::string::npos);
    CHECK(b->result.lockText.find("\"other.B\"") != std::string::npos);
    CHECK(b->result.lockChanges.size() == 2); // new type other.B + its field
}

TEST_CASE("lock: --check-lock fails on a stale or missing lock") {
    LockFixture fx;
    CompileOptions check;
    check.checkLock = true;
    auto missing = fx.run("struct A { a: u8 }", check);
    CHECK_FALSE(missing->ok());
    CHECK(missing->messages.find("error: schema lock 'schemas/schema.lock.jsonc' does not exist (run helios-schemac without --check-lock and commit it)") !=
          std::string::npos);

    REQUIRE(fx.run("struct A { a: u8 }")->ok());
    CHECK(fx.run("struct A { a: u8 }", check)->ok());
    auto stale = fx.run("struct A { a: u8; b: u8 }", check);
    CHECK_FALSE(stale->ok());
    CHECK(stale->messages.find("error: schema lock 'schemas/schema.lock.jsonc' is out of date; run helios-schemac without --check-lock and commit the lock") !=
          std::string::npos);
    CHECK(stale->messages.find("note: test.A: new field b (id 2)") != std::string::npos);
    CHECK(stale->result.outputs.empty());

    // Non-canonical formatting also counts as stale.
    fx.fs.files[fx.lockPath] += "\n";
    auto format = fx.run("struct A { a: u8 }", check);
    CHECK_FALSE(format->ok());
    CHECK(format->messages.find("note: the lock file is not in canonical form") != std::string::npos);
}

TEST_CASE("lock: hand edits and corruption are detected") {
    struct Case {
        const char* lock;
        const char* expected;
    };
    const Case cases[] = {
        {"{", "schemas/schema.lock.jsonc:1:2: error: schema lock is not valid JSONC"},
        {R"({"format": 2, "types": {}})", "error: invalid schema lock at format: unsupported lock format 2 (this helios-schemac writes 1)"},
        {R"({"format": 1})", "error: invalid schema lock at <root>: missing 'types' object"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "blob"}}})", "error: invalid schema lock at types.a.X.kind: unknown kind 'blob'"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "struct"}, "a.Y": {"id": 5, "kind": "struct"}}})",
         "error: invalid schema lock at types.a.Y: type id 5 is also used by 'a.X'"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "struct", "nextField": 3, "fields": [{"id": 1, "name": "a", "type": "u8"}]}}})",
         "error: invalid schema lock at types.a.X: entry with id 2 is missing; lock entries must never be deleted (restore it from version control)"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "struct", "nextField": 2, "fields": [{"id": 1, "name": "a"}, {"id": 1, "name": "b"}]}}})",
         "error: invalid schema lock at types.a.X: id 1 is used twice"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "struct", "nextField": 1, "fields": [{"id": 1, "name": "a"}]}}})",
         "error: invalid schema lock at types.a.X: id 1 of 'a' is not below nextField 1"},
        {R"({"format": 1, "types": {"a.X": {"id": 5, "kind": "struct", "nextField": 3, "fields": [{"id": 1, "name": "a"}, {"id": 2, "name": "a"}]}}})",
         "error: invalid schema lock at types.a.X: 'a' appears twice"},
        {R"({"format": 1, "types": {"a.E": {"id": 5, "kind": "enum", "values": [{"name": "A", "value": 1}, {"name": "B", "value": 1}]}}})",
         "error: invalid schema lock at types.a.E: enum value 1 is used twice"},
        {R"({"format": 1, "types": {"a.X": {"id": -1, "kind": "struct"}}})", "error: invalid schema lock at types.a.X.id: expected an unsigned 32-bit integer"},
        {R"({"format": 1, "types": {"a.X": {"id": 1, "kind": "struct", "fields": [{"id": 1}]}}})",
         "error: invalid schema lock at types.a.X.fields[0]: missing 'name'"},
    };
    for (const Case& c : cases) {
        LockFixture fx;
        fx.fs.files[fx.lockPath] = c.lock;
        auto r = fx.run("struct A { a: u8 }");
        CHECK_FALSE(r->ok());
        CHECK_MESSAGE(r->messages.find(c.expected) != std::string::npos,
                      "lock:\n" << std::string(c.lock) << "\nexpected:\n" << std::string(c.expected) << "\ngot:\n" << r->messages);
    }
}

TEST_CASE("lock: the lock text is canonical and round-trips") {
    LockFixture fx;
    REQUIRE(fx.run(R"(struct A @version(4) { a: u8 = 3; b: list<string>; c: variant { X; Y { v: f32 } } }
enum E : i16 { Neg = -5, Zero = 0, Big = 30000 }
flags F : u32 { A, B, C }
)")->ok());
    const std::string text = fx.lock();
    DiagnosticEngine diags;
    const u32 file = diags.addFile("lock", text);
    Lock lock;
    REQUIRE(loadLock(diags.fileText(file), file, lock, diags));
    CHECK(writeLock(lock) == text);
    CHECK(lock.types.at("test.E").values[0].value == -5);
    CHECK(lock.types.at("test.F").values[2].value == 4);
    CHECK(lock.types.at("test.A").version == 4);
    CHECK(lock.types.at("test.A.C").kind == "variant");
    CHECK(text.find("\"alternatives\": [") != std::string::npos);
}

TEST_CASE("lock: component parts and service requests are locked separately") {
    LockFixture fx;
    auto c = fx.run(R"(component Health replicate(all) { hp: f32; server { regen: f32 } }
struct Tx { n: u8 }
service Bank { rpc Pay(tx: Tx, note: string) -> u8; }
)");
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->decl("test.Health.Server")->typeId == fnv1a32("test.Health.Server"));
    CHECK(LockFixture::fieldId(*c, "test.Health.Server", "regen") == 1);
    CHECK(c->decl("test.BankPayRequest")->typeId == fnv1a32("test.BankPayRequest"));
    CHECK(LockFixture::fieldId(*c, "test.BankPayRequest", "note") == 2);
    // Moving a field between the replicated part and the server block keeps both ids stable per part.
    auto moved = fx.run(R"(component Health replicate(all) { hp: f32; server { regen: f32; shield: f32 } }
struct Tx { n: u8 }
service Bank { rpc Pay(tx: Tx, note: string) -> u8; }
)");
    REQUIRE(moved->ok());
    CHECK(changes(*moved) == "test.Health.Server: new field shield (id 2)\n");
}

TEST_CASE("lock: layout hashes are transitive and independent of declaration and file order") {
    auto hashes = [](const std::map<std::string, std::string>& files, std::vector<std::string> generate) {
        CompileOptions options;
        options.files = std::move(generate);
        options.lockPath = "schemas/schema.lock.jsonc"; // imported-only types get no lock ids
        auto c = compileFiles(files, options);
        REQUIRE_MESSAGE(c->ok(), c->messages);
        std::map<std::string, u64> out;
        for (const Decl* d : c->schema().decls) {
            if (d->isLockable()) out[d->qualifiedName] = d->layoutHash;
        }
        return out;
    };
    const std::string inner = "package a;\nenum Mode { Off, On }\nstruct Inner { a: u8; mode: Mode }\n";
    const std::string outer = "package b;\nimport \"a/inner.hschema\";\n"
                              "struct Outer { i: a.Inner; l: list<a.Inner> }\nstruct Node { n: u8; kids: list<Node>; o: Outer? }\n"
                              "struct ByRef { r: RRef? }\nrecord RDef { x: u8 }\n";
    const auto base = hashes({{"schemas/b/outer.hschema", outer}, {"schemas/a/inner.hschema", inner}}, {"schemas/b/outer.hschema"});
    // Compiling the imported file too (it then gets lock ids) changes nothing for the importer.
    const auto both =
        hashes({{"schemas/b/outer.hschema", outer}, {"schemas/a/inner.hschema", inner}}, {"schemas/a/inner.hschema", "schemas/b/outer.hschema"});
    CHECK(both.at("b.Outer") == base.at("b.Outer"));
    CHECK(both.at("b.Node") == base.at("b.Node"));
    // Reordering declarations changes nothing.
    const std::string reordered = "package b;\nimport \"a/inner.hschema\";\nrecord RDef { x: u8 }\nstruct ByRef { r: RRef? }\n"
                                  "struct Node { n: u8; kids: list<Node>; o: Outer? }\nstruct Outer { i: a.Inner; l: list<a.Inner> }\n";
    const auto moved = hashes({{"schemas/b/outer.hschema", reordered}, {"schemas/a/inner.hschema", inner}}, {"schemas/b/outer.hschema"});
    CHECK(moved.at("b.Outer") == base.at("b.Outer"));
    CHECK(moved.at("b.Node") == base.at("b.Node"));
    CHECK(moved.at("b.ByRef") == base.at("b.ByRef"));
    // A change two levels down (an enum value of a field of an element type) reaches every user...
    const std::string inner2 = "package a;\nenum Mode { Off, On, Auto }\nstruct Inner { a: u8; mode: Mode }\n";
    const auto changed = hashes({{"schemas/b/outer.hschema", outer}, {"schemas/a/inner.hschema", inner2}}, {"schemas/b/outer.hschema"});
    CHECK(changed.at("b.Outer") != base.at("b.Outer"));
    CHECK(changed.at("b.Node") != base.at("b.Node"));
    // ...but a record reference is just an id: the record's layout is not part of the referrer's.
    const std::string outer2 = outer.substr(0, outer.find("record RDef")) + "record RDef { x: u8; y: u16 }\n";
    const auto refChanged = hashes({{"schemas/b/outer.hschema", outer2}, {"schemas/a/inner.hschema", inner}}, {"schemas/b/outer.hschema"});
    CHECK(refChanged.at("b.ByRef") == base.at("b.ByRef"));
    CHECK(refChanged.at("b.RDef") != base.at("b.RDef"));
}
