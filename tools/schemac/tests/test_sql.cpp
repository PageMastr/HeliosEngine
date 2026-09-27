// `--emit sql` (02 §3.5; 05 §3): PostgreSQL tables of @sql structs, the goose migration stub diffed
// against the baseline lock, the table identity in the lock and the attribute's rules. The goldens
// (test_golden.cpp) pin the full output; tools/schemac/tests/sql_postgres.cmake applies it to a real
// PostgreSQL where one is installed.

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

} // namespace
