// `--emit sql` (02 §3.5; 05 §3): PostgreSQL tables of @sql structs, the goose migration stub diffed
// against the baseline lock, the table identity in the lock and the attribute's rules. The goldens
// (test_golden.cpp) pin the full output; tools/schemac/tests/sql_postgres.cmake applies it to a real
// PostgreSQL where one is installed.

#include <algorithm>

#include "test_util.h"
#include "text.h"

using namespace schemac_test;

namespace {

CompileOptions sqlOptions() {
    CompileOptions options;
    options.emitSql = true;
    options.sqlOut = "sql";
    options.namingLints = false;
    return options;
}

/// Compiles `body` (package test) with --emit sql against the lock in `fs`; the lock text is updated.
std::unique_ptr<Compiled> compileSql(const std::string& body, MemoryFileSystem& fs, CompileOptions options = sqlOptions()) {
    options.lockPath = "schemas/lock.jsonc";
    auto c = compileFiles({{"schemas/test/t.hschema", "package test;\n" + body}}, options, &fs);
    if (c->ok()) fs.files["schemas/lock.jsonc"] = c->result.lockText;
    return c;
}

bool contains(const std::string* text, const std::string& part) { return text && text->find(part) != std::string::npos; }

TEST_CASE("sql: snake_case names") {
    CHECK(snakeCase("maxForce") == "max_force");
    CHECK(snakeCase("HangarSlot") == "hangar_slot");
    CHECK(snakeCase("HTTPServer") == "http_server");
    CHECK(snakeCase("u8v") == "u8v");
    CHECK(snakeCase("v3dPos") == "v3d_pos");
    CHECK(snakeCase("already_snake") == "already_snake");
}

TEST_CASE("sql: @sql structs become PostgreSQL tables with types, defaults, checks and a key") {
    MemoryFileSystem fs;
    auto c = compileSql(R"(
enum Kind : u8 { A; B = 5 }
struct Inner { x: f32 }
/// Doc line.
struct Row @store(character) @sql(schema="svc_test") @key(owner, seq) {
  owner: EntityId
  seq: u16
  order: u8 = 7
  amount: i64 = -5
  big: u64
  ratio: f32 = 0.5
  label: string = "it's"
  kind: Kind = B
  lazy: Kind
  note: string?
  pos: vec3f
  inner: Inner
  list: list<u32>
  when: Duration = 1500ms
})",
                        fs);
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string* schema = c->output("sql/svc_test/schema.sql");
    REQUIRE(schema);
    INFO(*schema);
    CHECK(contains(schema, "CREATE SCHEMA IF NOT EXISTS svc_test;"));
    CHECK(contains(schema, "-- test.Row (lock id "));
    CHECK(contains(schema, "CREATE TABLE svc_test.row ("));
    CHECK(contains(schema, "    owner BIGINT NOT NULL CONSTRAINT row_owner_check CHECK (owner >= 0),\n")); // a key column: no DEFAULT
    CHECK(contains(schema, "    seq INTEGER NOT NULL CONSTRAINT row_seq_check CHECK (seq BETWEEN 0 AND 65535),\n"));
    CHECK(contains(schema, "    \"order\" SMALLINT NOT NULL DEFAULT 7 CONSTRAINT row_order_check CHECK (\"order\" BETWEEN 0 AND 255),\n"));
    CHECK(contains(schema, "    amount BIGINT NOT NULL DEFAULT -5,\n"));
    CHECK(contains(schema, "    big NUMERIC(20) NOT NULL DEFAULT 0 CONSTRAINT row_big_check CHECK (big BETWEEN 0 AND 18446744073709551615),\n"));
    CHECK(contains(schema, "    ratio REAL NOT NULL DEFAULT 0.5,\n"));
    CHECK(contains(schema, "    label TEXT NOT NULL DEFAULT 'it''s',\n"));
    CHECK(contains(schema, "    kind SMALLINT NOT NULL DEFAULT 5 CONSTRAINT")); // enums store their numbers
    CHECK(contains(schema, "    lazy SMALLINT NOT NULL DEFAULT 0 CONSTRAINT"));  // implicit: the first value
    CHECK(contains(schema, "    note TEXT,\n"));
    CHECK(contains(schema, "    pos JSONB CONSTRAINT row_pos_check CHECK (jsonb_typeof(pos) = 'array'), -- canonical JSONC"));
    CHECK(contains(schema, "    \"inner\" JSONB CONSTRAINT row_inner_check CHECK (jsonb_typeof(\"inner\") = 'object'),"));
    CHECK(contains(schema, "    list JSONB CONSTRAINT"));
    CHECK(contains(schema, "    \"when\" BIGINT NOT NULL DEFAULT 1500000000,\n"));
    CHECK(contains(schema, "    PRIMARY KEY (owner, seq)\n);"));
    // A new table: the stub creates it and its Down drops it.
    const std::string* migration = c->output("sql/svc_test/migration.sql");
    REQUIRE(migration);
    CHECK(contains(migration, "-- +goose Up\n-- test.Row (lock id "));
    CHECK(contains(migration, "CREATE TABLE svc_test.row ("));
    CHECK(contains(migration, "-- +goose Down\nDROP TABLE IF EXISTS svc_test.row;\n"));
    // The lock records the table (the type's SQL identity).
    CHECK(contains(&c->result.lockText, "\"sql\": \"svc_test.row\""));
    // Field @key works too; without --emit sql no SQL is written.
    auto plain = compileText("package test;\nstruct T @sql(schema=\"svc_test\") { id: u32 @key }\n");
    REQUIRE_MESSAGE(plain->ok(), plain->messages);
    CHECK(plain->output("schema.sql") == nullptr);
}

TEST_CASE("sql: the migration stub expands from the baseline lock and never drops") {
    MemoryFileSystem fs;
    const std::string v1 = R"(
struct Account @store(character) @sql(schema="svc_test") {
  id: u64 @key
  rank: u8
  amount: i32
  small: u8 = 3
  note: string
  legacy: u8
})";
    auto first = compileSql(v1, fs);
    REQUIRE_MESSAGE(first->ok(), first->messages);
    const std::string v1Lock = first->result.lockText;
    const std::string v2 = R"(
struct Account @store(character) @sql(schema="svc_test") {
  id: u64 @key
  level: u8 @was("rank")
  amount: i64
  small: u16 = 3
  note: string?
  added: bool = true
  more: list<u8>
}
struct Fresh @store(character) @sql(schema="svc_test") { id: u32 @key })";
    auto second = compileSql(v2, fs);
    REQUIRE_MESSAGE(second->ok(), second->messages);
    const std::string* m = second->output("sql/svc_test/migration.sql");
    REQUIRE(m);
    INFO(*m);
    CHECK(contains(m, "ALTER TABLE svc_test.account RENAME COLUMN rank TO level;"));
    CHECK(contains(m, "ALTER TABLE svc_test.account ALTER COLUMN amount TYPE BIGINT;"));
    CHECK(contains(m, "ALTER TABLE svc_test.account DROP CONSTRAINT IF EXISTS account_small_check;\n"
                      "ALTER TABLE svc_test.account ALTER COLUMN small TYPE INTEGER;\n"
                      "ALTER TABLE svc_test.account ADD CONSTRAINT account_small_check CHECK (small BETWEEN 0 AND 65535);"));
    CHECK(contains(m, "ALTER TABLE svc_test.account ALTER COLUMN note DROP DEFAULT;\nALTER TABLE svc_test.account ALTER COLUMN note DROP NOT NULL;"));
    CHECK(contains(m, "ALTER TABLE svc_test.account ADD COLUMN added BOOLEAN NOT NULL DEFAULT TRUE;"));
    CHECK(contains(m, "ALTER TABLE svc_test.account ADD COLUMN more JSONB CONSTRAINT account_more_check"));
    CHECK(contains(m, "CREATE TABLE svc_test.fresh ("));
    CHECK(contains(m, "--   ALTER TABLE svc_test.account DROP COLUMN legacy; -- field legacy (lock id 6) was removed"));
    CHECK_FALSE(contains(m, "\nALTER TABLE svc_test.account DROP COLUMN legacy")); // expand only (05 §3.3)
    // Down reverses the steps, last first.
    const usize down = m->find("-- +goose Down");
    REQUIRE(down != std::string::npos);
    const std::string d = m->substr(down);
    CHECK(d.find("DROP TABLE IF EXISTS svc_test.fresh;") < d.find("DROP COLUMN IF EXISTS more;"));
    CHECK(contains(&d, "ALTER TABLE svc_test.account RENAME COLUMN level TO rank;"));
    CHECK(contains(&d, "ALTER TABLE svc_test.account ALTER COLUMN amount TYPE INTEGER;"));
    CHECK(contains(&d, "ALTER TABLE svc_test.account ALTER COLUMN note SET NOT NULL;"));
    // The snapshot keeps lock-id order, so migrations and the snapshot build the same table.
    const std::string* snap = second->output("sql/svc_test/schema.sql");
    REQUIRE(snap);
    CHECK(snap->find("    level SMALLINT") < snap->find("    added BOOLEAN"));
    CHECK_FALSE(contains(snap, "legacy"));

    // Against the updated lock the stub is empty; --sql-baseline reproduces the first diff.
    auto again = compileSql(v2, fs);
    REQUIRE_MESSAGE(again->ok(), again->messages);
    CHECK(contains(again->output("sql/svc_test/migration.sql"), "-- +goose Up\n-- No changes since the baseline lock.\nSELECT 1;"));
    fs.files["schemas/v1.lock.jsonc"] = v1Lock;
    CompileOptions fromV1 = sqlOptions();
    fromV1.sqlBaseline = "schemas/v1.lock.jsonc";
    auto replay = compileSql(v2, fs, fromV1);
    REQUIRE_MESSAGE(replay->ok(), replay->messages);
    CHECK(*replay->output("sql/svc_test/migration.sql") == *m);
}

TEST_CASE("sql: @sql misuse and unstorable fields are errors") {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"record R @sql(schema=\"svc_test\") { x: u8 }", "@sql is only valid on structs"},
        {"struct S @sql(schema=\"test\") { x: u8 @key }", "tables live in a service schema named svc_<service>"},
        {"struct S @sql(schema=\"svc_\") { x: u8 @key }", "tables live in a service schema named svc_<service>"},
        {"struct S @sql(schema=\"svc_Test\") { x: u8 @key }", "tables live in a service schema named svc_<service>"},
        {"struct S @sql(schema=\"svc_test\", table=\"Bad-Name\") { x: u8 @key }", "@sql table name 'Bad-Name' must be lowercase"},
        {"struct S @sql(schema=\"svc_test\", owner=\"x\") { x: u8 @key }", "@sql takes schema="},
        {"struct S @store(checkpoint) @sql(schema=\"svc_test\") { x: u8 @key }", "checkpoint data is a blob in ag_checkpoint"},
        {"struct S @store(ledger) @sql(schema=\"svc_test\") { x: u8 @key }", "value-bearing data lives only in svc_ledger"},
        {"struct S @sql(schema=\"svc_ledger\") { x: u8 @key }", "svc_ledger holds only @store(ledger) data"},
        {"struct S @sql(schema=\"svc_test\") { x: u8 }", "'S' is @sql but has no key"},
        {"struct S @sql(schema=\"svc_test\") @key(y) { x: u8 }", "@key names 'y', which is not a field of 'S'"},
        {"struct S @sql(schema=\"svc_test\") { x: u8? @key }", "key field 'x' of 'S' must be a non-optional scalar"},
        {"struct S @sql(schema=\"svc_test\") { x: u8 @key; h: NetHandle }", "a NetHandle is scoped to one zone instance"},
        {"struct H { h: list<NetHandle?> }\nstruct S @sql(schema=\"svc_test\") { x: u8 @key; inner: map<u8, H> }",
         "field 'inner' of 'S' cannot be a column of svc_test.s: it holds a NetHandle"},
        {"struct S @sql(schema=\"svc_test\") { x: u8 @key; aB: u8; a_b: u8 }", "fields 'aB' and 'a_b' both become the column a_b"},
        {"struct S @sql(schema=\"svc_test\") { x: u8 @key }\nstruct T @sql(schema=\"svc_test\", table=\"s\") { x: u8 @key }",
         "both map to the table svc_test.s"},
        {"struct S @sql(schema=\"svc_test\") { x: u8 @key; " + std::string(64, 'a') + ": u8 }", "longer than PostgreSQL's 63 bytes"},
    };
    for (const auto& [body, expected] : cases) {
        INFO(body);
        const std::string messages = diagnosticsOf(body, sqlOptions());
        CHECK_MESSAGE(messages.find(expected) != std::string::npos, messages);
    }
    // A table cannot move, and a new field cannot join the key of an existing table.
    MemoryFileSystem fs;
    REQUIRE(compileSql("struct S @sql(schema=\"svc_test\") { x: u8 @key }", fs)->ok());
    auto moved = compileSql("struct S @sql(schema=\"svc_test\", table=\"t\") { x: u8 @key }", fs);
    CHECK(moved->messages.find("is stored in the table svc_test.s") != std::string::npos);
    // A @was rename moves the default table name with it; the message names the attribute that keeps it.
    {
        MemoryFileSystem renames = fs;
        auto renamedType = compileSql("struct S2 @was(\"S\") @sql(schema=\"svc_test\") { x: u8 @key }", renames);
        CHECK_MESSAGE(renamedType->messages.find("a table cannot move to svc_test.s2 — keep it with @sql(schema=\"svc_test\", table=\"s\")") !=
                          std::string::npos,
                      renamedType->messages);
        CHECK(compileSql("struct S2 @was(\"S\") @sql(schema=\"svc_test\", table=\"s\") { x: u8 @key }", renames)->ok());
    }
    auto rekeyed = compileSql("struct S @sql(schema=\"svc_test\") { x: u8 @key; y: u8 @key }", fs);
    CHECK_MESSAGE(rekeyed->messages.find("new field 'y' cannot join the key of the existing table svc_test.s") != std::string::npos,
                  rekeyed->messages);
    // --sql-baseline needs --lock (field ids), and a missing baseline is an error.
    CompileOptions noLock = sqlOptions();
    noLock.sqlBaseline = "schemas/lock.jsonc";
    CHECK(diagnosticsOf("struct S @sql(schema=\"svc_test\") { x: u8 @key }", noLock).find("--sql-baseline needs --lock") != std::string::npos);
    CompileOptions missing = sqlOptions();
    missing.sqlBaseline = "schemas/none.jsonc";
    CHECK(compileSql("struct S @sql(schema=\"svc_test\") { x: u8 @key }", fs, missing)->messages.find("cannot read the SQL baseline lock") !=
          std::string::npos);
}

TEST_CASE("sql: a new type cannot take over another type's table") {
    // The round-1 reproducer: A is removed and B (a rename without @was) claims A's table. Field ids
    // are per type, so a stub diffed by table name would "rename" A's columns into B's.
    MemoryFileSystem fs;
    REQUIRE(compileSql("struct A @store(character) @sql(schema=\"svc_test\") { id: u32 @key; x: i32 }", fs)->ok());
    const std::string v1Lock = fs.files["schemas/lock.jsonc"];
    const std::string takeover = "struct B @store(character) @sql(schema=\"svc_test\", table=\"a\") { k: u32 @key; y: string }";
    auto taken = compileSql(takeover, fs);
    CHECK_FALSE(taken->ok());
    CHECK_MESSAGE(taken->messages.find("the table svc_test.a belongs to 'test.A' (lock id") != std::string::npos, taken->messages);
    CHECK(fs.files["schemas/lock.jsonc"] == v1Lock); // nothing recorded
    // A @was rename keeps the lock entry, so it keeps the table and its columns.
    auto renamed = compileSql("struct B @store(character) @sql(schema=\"svc_test\", table=\"a\") @was(\"A\") { id: u32 @key; x: i64 }", fs);
    REQUIRE_MESSAGE(renamed->ok(), renamed->messages);
    CHECK(contains(renamed->output("sql/svc_test/migration.sql"), "ALTER TABLE svc_test.a ALTER COLUMN x TYPE BIGINT;"));
    // A hand-edited lock cannot record one table twice, and the stub takes its baseline by lock id:
    // with A's table removed from the current lock by hand, B's table is new to the stub even though
    // the --sql-baseline lock had A in it.
    MemoryFileSystem edited;
    REQUIRE(compileSql("struct A @store(character) @sql(schema=\"svc_test\") { id: u32 @key; x: i32 }", edited)->ok());
    std::string lock = edited.files["schemas/lock.jsonc"];
    edited.files["schemas/v1.lock.jsonc"] = lock;
    const usize at = lock.find("\"sql\": \"svc_test.a\"");
    REQUIRE(at != std::string::npos);
    edited.files["schemas/lock.jsonc"] = lock.erase(at, lock.find('\n', at) - at + 1);
    REQUIRE(edited.files["schemas/lock.jsonc"].find("svc_test.a") == std::string::npos);
    CompileOptions fromV1 = sqlOptions();
    fromV1.sqlBaseline = "schemas/v1.lock.jsonc";
    auto fresh = compileSql(takeover, edited, fromV1);
    REQUIRE_MESSAGE(fresh->ok(), fresh->messages);
    const std::string* m = fresh->output("sql/svc_test/migration.sql");
    INFO((m ? *m : std::string()));
    CHECK(contains(m, "CREATE TABLE svc_test.a ("));
    CHECK_FALSE(contains(m, "RENAME COLUMN"));
}

TEST_CASE("sql: migration notes for revived retyped fields and ledger partitions") {
    MemoryFileSystem fs;
    REQUIRE(compileSql("struct R @store(character) @sql(schema=\"svc_test\") { id: u32 @key; hp: i32 }", fs)->ok());
    REQUIRE(compileSql("struct R @store(character) @sql(schema=\"svc_test\") { id: u32 @key }", fs)->ok());
    auto revived = compileSql("struct R @store(character) @sql(schema=\"svc_test\") { id: u32 @key; hp: i64 }", fs);
    REQUIRE_MESSAGE(revived->ok(), revived->messages);
    const std::string* m = revived->output("sql/svc_test/migration.sql");
    INFO((m ? *m : std::string()));
    CHECK(contains(m, "ALTER TABLE svc_test.r ADD COLUMN IF NOT EXISTS hp BIGINT"));
    CHECK(contains(m, "-- TODO: svc_test.r.hp was i32 (INTEGER) when it was removed and is now i64 (BIGINT); if the old column still exists"));
    auto ledger = compileSql("struct Entry @store(ledger) @sql(schema=\"svc_ledger\") { id: u64 @key; amount: i64 }", fs);
    REQUIRE_MESSAGE(ledger->ok(), ledger->messages);
    const std::string* l = ledger->output("sql/svc_ledger/migration.sql");
    CHECK(contains(l, "-- TODO: 05 §3.3 keeps ledger tables in monthly range partitions"));
    CHECK_FALSE(contains(ledger->output("sql/svc_ledger/schema.sql"), "TODO"));
}

TEST_CASE("sql: a carriage return in a doc comment cannot reach the generated SQL") {
    // The round-1 reproducer: PostgreSQL ends a -- comment at a lone CR, so the rest of the doc line
    // would run as SQL from the snapshot and the stub. The lexer rejects it (and every control
    // character but tab) for every emitter.
    const std::string body = "/// harmless doc\rDROP SCHEMA svc_test CASCADE; CREATE TABLE public.pwned (x int);\n"
                             "struct V @store(character) @sql(schema=\"svc_test\") { id: u32 @key }";
    MemoryFileSystem fs;
    auto c = compileSql(body, fs);
    CHECK_FALSE(c->ok());
    CHECK_MESSAGE(c->messages.find("control character U+000D in a doc comment") != std::string::npos, c->messages);
    CHECK(c->output("sql/svc_test/schema.sql") == nullptr);
    // CRLF line endings are fine: the CR before the newline is not part of the doc text.
    auto crlf = compileSql("/// a doc line\r\nstruct V @store(character) @sql(schema=\"svc_test\") { id: u32 @key }", fs);
    REQUIRE_MESSAGE(crlf->ok(), crlf->messages);
    CHECK(contains(crlf->output("sql/svc_test/schema.sql"), "-- test.V (lock id"));
}

TEST_CASE("sql: a widened enum or flags underlying type widens its columns") {
    // The round-2 reproducer: a field of enum type keeps its signature (test.K) when K widens from u8
    // to u16, which the lock accepts, but its column (SMALLINT, CHECK 0..255) must widen with it.
    MemoryFileSystem fs;
    REQUIRE(compileSql("enum K : u8 { A; B }\nflags F : u8 { X; Y }\n"
                       "struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K; f: F; o: K? }",
                       fs)
                ->ok());
    const std::string v2 = "enum K : u16 { A; B; C = 300 }\nflags F : u16 { X; Y }\n"
                           "struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K; f: F; o: K?; p: K? }";
    auto c = compileSql(v2, fs);
    REQUIRE_MESSAGE(c->ok(), c->messages); // the lock accepts u8 -> u16
    CHECK(contains(c->output("sql/svc_test/schema.sql"), "    k INTEGER NOT NULL DEFAULT 0 CONSTRAINT s_k_check CHECK (k BETWEEN 0 AND 65535),"));
    const std::string* m = c->output("sql/svc_test/migration.sql");
    INFO((m ? *m : std::string()));
    CHECK(contains(m, "ALTER TABLE svc_test.s DROP CONSTRAINT IF EXISTS s_k_check;\n"
                      "ALTER TABLE svc_test.s ALTER COLUMN k TYPE INTEGER;\n"
                      "ALTER TABLE svc_test.s ADD CONSTRAINT s_k_check CHECK (k BETWEEN 0 AND 65535);"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ALTER COLUMN f TYPE INTEGER;"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ADD CONSTRAINT s_f_check CHECK (f BETWEEN 0 AND 65535);"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ALTER COLUMN o TYPE INTEGER;"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ADD COLUMN p INTEGER CONSTRAINT s_p_check CHECK (p BETWEEN 0 AND 65535);"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ALTER COLUMN k TYPE SMALLINT;")); // the Down
    CHECK(contains(m, "ALTER TABLE svc_test.s ADD CONSTRAINT s_k_check CHECK (k BETWEEN 0 AND 255);"));

    // T -> T? of a widened enum: the old column is the baseline's SMALLINT, not the current INTEGER.
    MemoryFileSystem opt;
    REQUIRE(compileSql("enum K : u8 { A }\nstruct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K }", opt)->ok());
    auto toOptional = compileSql("enum K : u16 { A }\nstruct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K? }", opt);
    REQUIRE_MESSAGE(toOptional->ok(), toOptional->messages);
    const std::string* o = toOptional->output("sql/svc_test/migration.sql");
    INFO((o ? *o : std::string()));
    CHECK(contains(o, "ALTER TABLE svc_test.s ALTER COLUMN k TYPE INTEGER;"));
    CHECK(contains(o, "ALTER TABLE svc_test.s ALTER COLUMN k DROP NOT NULL;"));
    CHECK(contains(o, "ALTER TABLE svc_test.s ALTER COLUMN k TYPE SMALLINT;"));
    // An i8 enum widening to i16 drops its -128..127 CHECK (SMALLINT holds every i16).
    MemoryFileSystem signedEnum;
    REQUIRE(compileSql("enum K : i8 { A }\nstruct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K }", signedEnum)->ok());
    auto i16 = compileSql("enum K : i16 { A }\nstruct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K }", signedEnum);
    REQUIRE_MESSAGE(i16->ok(), i16->messages);
    CHECK(contains(i16->output("sql/svc_test/migration.sql"), "ALTER TABLE svc_test.s DROP CONSTRAINT IF EXISTS s_k_check;"));
}

TEST_CASE("sql: a type renamed with @was is not a type change of the fields that use it") {
    MemoryFileSystem fs;
    REQUIRE(compileSql("enum K : u8 { A }\nstruct Inner { x: f32 }\n"
                       "struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K = A; inner: Inner; l: list<K> }",
                       fs)
                ->ok());
    auto c = compileSql("enum K2 : u8 @was(\"K\") { A; B }\nstruct Inner2 @was(\"Inner\") { x: f32 }\n"
                        "struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; k: K2 = B; inner: Inner2; l: list<K2> }",
                        fs, [] {
                            CompileOptions o = sqlOptions();
                            o.allowDefaultChange = true;
                            return o;
                        }());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string* m = c->output("sql/svc_test/migration.sql");
    INFO((m ? *m : std::string()));
    CHECK_FALSE(contains(m, "TODO"));
    CHECK(contains(m, "ALTER TABLE svc_test.s ALTER COLUMN k SET DEFAULT 1;")); // the default is still diffed
    CHECK_FALSE(contains(m, "ALTER COLUMN inner"));
}

TEST_CASE("sql: string defaults read the same whatever standard_conforming_strings is") {
    // With standard_conforming_strings off, '\'' is an escaped quote: the round-2 probe's default ended
    // the literal and ran the CREATE TABLE. A string with a backslash becomes E'...' with it doubled.
    MemoryFileSystem fs;
    auto c = compileSql("struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key;\n"
                        "  a: string = \"\\\\'); CREATE TABLE public.scs_pwned (x int); --\"\n  b: string = \"it's\" }",
                        fs);
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string* s = c->output("sql/svc_test/schema.sql");
    INFO((s ? *s : std::string()));
    CHECK(contains(s, "    a TEXT NOT NULL DEFAULT E'\\\\''); CREATE TABLE public.scs_pwned (x int); --',\n"));
    CHECK(contains(s, "    b TEXT NOT NULL DEFAULT 'it''s',\n")); // no backslash: a plain literal
}

TEST_CASE("sql: string defaults stay on one line and hold no NUL") {
    // A NUL ends psql's input line, so the rest of the snapshot shifts out of the literal and the next
    // string default runs as SQL; PostgreSQL TEXT cannot hold U+0000 anyway.
    MemoryFileSystem fs;
    auto nul = compileSql("struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key;\n"
                          "  a: string = \"\\0\"\n  b: string = \"); CREATE TABLE public.pwned (x int); --\" }",
                          fs);
    CHECK_FALSE(nul->ok());
    CHECK_MESSAGE(nul->messages.find("the default of field 'a' of 'S' holds U+0000, which a PostgreSQL TEXT column cannot store") !=
                      std::string::npos,
                  nul->messages);
    CHECK(nul->result.outputs.empty());
    MemoryFileSystem fsU;
    auto nulU = compileSql("struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key; a: Name = \"x\\u0000y\" }", fsU);
    CHECK_FALSE(nulU->ok()); // \u0000 decodes to the same NUL
    // goose parses a stub line by line: a line inside a literal that reads as an annotation is one
    // (ENVSUB ON expanded the migration runner's environment into the stored default).
    MemoryFileSystem fs2;
    auto nl = compileSql("struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key;\n"
                         "  a: string = \"x\\n-- +goose ENVSUB ON\\n${SECRET}\" }",
                         fs2);
    REQUIRE_MESSAGE(nl->ok(), nl->messages);
    const std::string* s = nl->output("sql/svc_test/schema.sql");
    INFO((s ? *s : std::string()));
    CHECK(contains(s, "    a TEXT NOT NULL DEFAULT E'x\\n-- +goose ENVSUB ON\\n${SECRET}',\n"));
    // Every control character is an escape, so each literal stays on one line; a backslash is doubled
    // in the same E'…' literal, and the rest of the text (UTF-8 included) is copied.
    MemoryFileSystem fs3;
    auto ctl = compileSql("struct S @store(character) @sql(schema=\"svc_test\") { id: u32 @key;\n"
                          "  a: string = \"t\\tr\\rb\\u0008f\\u000Cs\\u0001d\\u007F\\\\q'\\u00e9\" }",
                          fs3);
    REQUIRE_MESSAGE(ctl->ok(), ctl->messages);
    const std::string* c = ctl->output("sql/svc_test/schema.sql");
    INFO((c ? *c : std::string()));
    CHECK(contains(c, "    a TEXT NOT NULL DEFAULT E't\\tr\\rb\\bf\\fs\\x01d\\x7F\\\\q''\xC3\xA9',\n"));
    const std::string* m = ctl->output("sql/svc_test/migration.sql");
    REQUIRE(m);
    CHECK(contains(m, "DEFAULT E't\\tr\\rb\\bf\\fs\\x01d\\x7F\\\\q''\xC3\xA9',\n")); // the stub shares the column text
    for (const std::string* text : {c, m}) {
        CHECK(std::none_of(text->begin(), text->end(), [](char ch) { return (static_cast<unsigned char>(ch) < 0x20 && ch != '\n') || ch == 0x7f; }));
    }
}

TEST_CASE("sql: a table no struct stores any more is listed for the contract release") {
    // 05 §3.3: release N only expands, so a removed table, like a removed column, is a note for N+2.
    MemoryFileSystem fs;
    CompileOptions both = sqlOptions();
    both.lockPath = "schemas/lock.jsonc";
    both.files = {"schemas/test/a.hschema", "schemas/test/b.hschema"};
    const std::string b = "package test;\nstruct Kept @store(character) @sql(schema=\"svc_test\") { id: u32 @key }\n";
    auto first = compileFiles({{"schemas/test/a.hschema", "package test;\nimport \"b.hschema\";\n"
                                                          "struct Live @store(character) @sql(schema=\"svc_test\") { id: u32 @key }\n"
                                                          "struct Gone @store(character) @sql(schema=\"svc_test\") { id: u32 @key }\n"
                                                          "struct Plain @store(character) @sql(schema=\"svc_test\") { id: u32 @key }\n"},
                               {"schemas/test/b.hschema", b}},
                              both, &fs);
    REQUIRE_MESSAGE(first->ok(), first->messages);
    fs.files["schemas/lock.jsonc"] = first->result.lockText;
    // v2: Gone is removed, Plain loses @sql, and b.hschema is only imported (Kept is not generated here).
    CompileOptions onlyA = sqlOptions();
    onlyA.lockPath = "schemas/lock.jsonc";
    auto second = compileFiles({{"schemas/test/a.hschema", "package test;\nimport \"b.hschema\";\n"
                                                           "struct Live @store(character) @sql(schema=\"svc_test\") { id: u32 @key }\n"
                                                           "struct Plain @store(character) { id: u32 @key }\n"},
                                {"schemas/test/b.hschema", b}},
                               onlyA, &fs);
    REQUIRE_MESSAGE(second->ok(), second->messages);
    const std::string* m = second->output("sql/svc_test/migration.sql");
    INFO((m ? *m : std::string()));
    CHECK(contains(m, "-- +goose Up\n-- No changes since the baseline lock.\nSELECT 1;\n")); // expand only: no DROP runs
    CHECK(contains(m, "-- Contract release (N+2, 05 §3.3), once no reader uses these tables:\n"));
    CHECK(contains(m, "--   DROP TABLE svc_test.gone; -- test.Gone (lock id "));
    CHECK(contains(m, ") was removed\n"));
    CHECK(contains(m, "--   DROP TABLE svc_test.plain; -- test.Plain (lock id "));
    CHECK(contains(m, ") is no longer @sql\n"));
    CHECK_FALSE(contains(m, "svc_test.kept")); // imported, still @sql
    CHECK_FALSE(contains(second->output("sql/svc_test/schema.sql"), "gone"));
}

TEST_CASE("sql: a control character in a schema file name is an error") {
    // The round-2 probe: every emitter prints the file name in a generated comment, which a lone CR ends.
    const std::string name = "schemas/test/x\rDROP SCHEMA svc_test CASCADE; --.hschema";
    auto c = compileFiles({{name, "package test;\nstruct V @store(character) @sql(schema=\"svc_test\") { id: u32 @key }"}}, sqlOptions());
    CHECK_FALSE(c->ok());
    CHECK_MESSAGE(c->messages.find("the schema path 'test/x\\x0DDROP SCHEMA svc_test CASCADE; --.hschema' has the control character U+000D") !=
                      std::string::npos,
                  c->messages);
    CHECK(c->result.outputs.empty());
    // An import is checked the same way.
    auto imported = compileFiles({{"schemas/test/a.hschema", "package test;\nimport \"b\\tc.hschema\";\n"},
                                  {"schemas/test/b\tc.hschema", "package test;\n"}},
                                 sqlOptions());
    CHECK_FALSE(imported->ok());
    CHECK_MESSAGE(imported->messages.find("has the control character U+0009") != std::string::npos, imported->messages);
}

} // namespace
