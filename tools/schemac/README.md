# helios-schemac — the Helios schema compiler

`helios-schemac` compiles `.hschema` files into C++ (types, reflection, codecs), Go (types and a
byte-identical codec), Luau glue (scriptlib bindings, `.d.luau` declarations, fuel defaults),
PostgreSQL DDL (table snapshots and goose migration stubs), replication descriptors with full-state
codecs, a lint report, and a machine-readable schema description.
It implements ADR-004.

**The normative specification is [docs/plan/02-engine-runtime.md §3](../../docs/plan/02-engine-runtime.md#3-schema-and-reflection-normative)**
(§3.1 language, §3.2 attributes, §3.3 records and the client/server split, §3.4 versioning,
§3.5 the compiler, §3.6 reflection, §3.7 formats). This README summarizes the language as
implemented, documents the parts the spec leaves open (wire format details, lock format, generated
APIs) and lists what is not implemented yet.

- Compiler sources: `tools/schemac/src` (depends only on `helios::core` and yyjson, so it builds
  first and for the build host).
- Runtime: `engine/reflect` (`helios::reflect` target, namespace `helios::refl`; see
  [its README](../../engine/reflect/README.md)).
- Build integration: [`cmake/HeliosSchema.cmake`](../../cmake/HeliosSchema.cmake).
- Sample schemas: [`schemas/sample`](../../schemas/sample) (also the test fixture).

## Status

| `--emit` | Status |
|---|---|
| `cpp` | Implemented: structs/enums/variants, `TypeInfo` registration, canonical JSONC and tagged-binary codecs, equality, `Mut<C>` dirty-bit mutators, record refs, sample values (`--samples`) |
| `go` | Implemented: structs with JSON tags, byte-identical tagged codec, generated round-trip / cross-language test |
| `json` | Implemented: schema description (types, ids, fields, attributes, defaults, layout hashes, services, constants, aliases, formulas, scriptlibs with fuel costs) |
| `luau` | Implemented for `scriptlib`s: the C++ call glue on engine/script's `Binder` with the fuel charges, binding ids from the lock, `schema.d.luau` and `fuel_costs.defaults.json` ([Generated Luau](#generated-luau)). Tagged-userdata glue for components and records (`@script` fields) is WP-1.6's |
| `sql` | Implemented for structs marked `@sql(schema="svc_<service>")`: a PostgreSQL snapshot and a goose migration stub diffed against the baseline lock, per service schema ([Generated SQL](#generated-sql)) |
| `repl` | Implemented for Phase 0 (04 §11.3: descriptors, full state): `ComponentRepDesc` tables with quantizers, typed full-state codecs, rpc and event tables and a protocol hash per file ([Generated replication](#generated-replication)). Change masks, deltas and variable-size fields are WP-1.10 |
| `lint` | Implemented: the size-budget lints and a report of what every lint checked, the SEC-1 classification of client→server rpcs and every finding ([Lint report](#lint-report)); the rule lints below already fail every compilation |
| `proto`, `editor`, `records`, `docs` | Planned; `--emit <name>` fails with exit code 2 "not yet implemented" |

The rule lints (AAA-SEC-1, AAA-SEC-4, ledger/persist, keyed lists, naming, script fuel) run on every
compilation; `--emit lint` adds the size budgets and the report.

## Quick start

```cmake
# CMakeLists.txt of a module or game
helios_schema(my_target
  FILES ${PROJECT_SOURCE_DIR}/schemas/game/ship.hschema
  LOCK  ${PROJECT_SOURCE_DIR}/schemas/schema.lock.jsonc      # committed, append-only
  GO_OUT ${PROJECT_SOURCE_DIR}/services/internal/gen/ship GO_PACKAGE ship)   # optional
```

```cpp
#include "game/ship.gen.h"          // path of the schema relative to its include root

game::ship::registerShipTypes();    // TypeRegistry::global()
game::ship::ThrusterMount m{};      // defaults from the schema
std::string text = helios::refl::toJson(m);                     // canonical JSONC
std::vector<helios::u8> blob = helios::refl::encodeTagged(m);   // tagged binary
```

Command line (what `helios_schema()` runs):

```
helios-schemac -I schemas --lock schemas/schema.lock.jsonc --emit cpp,go,json \
    --cpp-out build/gen --go-out services/internal/gen/ship --json-out build/gen/ship.schema.json \
    schemas/game/ship.hschema
```

| Option | Meaning |
|---|---|
| `-I <dir>` | Import root (repeatable). Also defines output paths: `schemas/game/ship.hschema` with `-I schemas` generates `game/ship.gen.h` |
| `--lock <file>` | Schema lock (created if missing, updated in place). Without it ids are per-run (warning) |
| `--check-lock` | Fail (exit 1) instead of updating an out-of-date lock (CI) |
| `--allow-default-change` | Accept changed explicit defaults (they are part of the wire contract) |
| `--emit cpp,go,json,luau,sql,repl,lint` | Generators (default `cpp`; `repl` writes next to the C++ output) |
| `--lint-out <file>` | Lint report of `--emit lint` (default `schema.lint.json`) |
| `--cpp-out`, `--go-out`, `--go-package`, `--json-out`, `--luau-out`, `--sql-out` | Output locations (`--luau-out`: `schema.d.luau` and `fuel_costs.defaults.json`; the Luau glue goes to `--cpp-out`. `--sql-out`: `<schema>/schema.sql` and `<schema>/migration.sql`) |
| `--sql-baseline <lock>` | Lock the SQL migration stub starts from (default: `--lock` as it was before this run) |
| `--samples` | Also emit `<file>.samples.gen.h` (deterministic sample values shared with the Go test) |
| `--depfile <f>`, `--depfile-target <p>` | Makefile-style dependency file (every schema, import and the lock) |
| `--Werror`, `--no-naming-lints`, `--quiet` | Diagnostics control |

Exit codes: `0` success, `1` schema errors or stale lock with `--check-lock`, `2` usage error or
not-yet-implemented generator, `3` I/O error writing outputs. Outputs are only rewritten when their
content changes (no spurious rebuilds). Diagnostics are `file:line:col: error|warning|note: message`
followed by the source line and a caret.

## The language

### Files, packages, imports

```
/// File doc comment (optional).
package game.ship;                       // required first; maps to C++ namespace game::ship
import "helios/world/frames.hschema";    // relative to this file, then to each -I root
```

- One `package` per file; several files may share a package.
- `import` makes the declarations of that file visible. Only declarations of the same file and of
  **directly imported** files are visible (the generated header includes exactly those headers).
  Import cycles are errors.
- Name lookup: nested types of the enclosing declarations, then the package and its parent
  packages (`common.Point` finds `game.common.Point` from `game.ship`; fully qualified names always
  work), then unqualified names of imported packages. A name found in two imported packages is an
  "ambiguous" error; qualify it.
- `///` doc comments attach to the next declaration, field or enum value and become `@doc`
  (TypeInfo/FieldInfo `doc`, C++ and Go comments). A control character other than tab in a doc
  comment is an error: a lone carriage return ends a generated `//` or `--` comment in GCC, Clang,
  Luau and PostgreSQL, so the rest of the line would run as code. (A CRLF line ending is fine.)
  For the same reason a schema file path (an input or an import) with a control character is an
  error: every emitter prints it in a header comment.

### Declarations

| Kind | Example | Generates |
|---|---|---|
| `enum` | `enum ShipSize : u8 { Small; Medium; Large }` | `enum class`, enum TypeInfo; values count from 0 or the previous value + 1; default underlying `u32` |
| `flags` | `flags Perm : u16 { None = 0; Read; Write }` | bit enum (`HELIOS_ENUM_FLAGS`); implicit values are successive bits; unsigned base |
| `struct` | `struct P { x: f32; y: f32 = 1 }` | aggregate with default member initializers, `operator==`, codecs |
| `component` | `component Health replicate(all) { hp: f32; server { regen: f32 } }` | struct + `X::Server` / `X::Client` parts; replicated components get `_dirty`, `kReplicatedFields`, `Mut<X>` |
| `record` | `record ShipHullDef @table("hull") { … }` | struct + `using ShipHullRef = RecordRef<ShipHullDef>` (`FooDef` → `FooRef`, else `FooRef` from `Foo`) |
| `event`, `message`, `viewmodel`, `relation` | `event Died { who: EntityId }` | struct (TypeInfo `decl` tells them apart); a `relation` may have no body |
| `rpc` | `rpc Dock(bay: u8) client->server reliable @ratelimit(2/s) @intent(dock);` | argument struct `Dock` |
| `service` | `service Ledger { rpc Execute(tx: LedgerTx) -> LedgerResult; }` | argument structs `<Service><Rpc>Request` (result types are ordinary structs) |
| `const` | `const MaxStack: u32 = 9999;` | `inline constexpr` (scalars, strings, `Duration`) |
| `alias` | `alias Credits = i64;` / `alias Pair = { x: f32; y: f32 };` | `using`; an alias of an inline type *names* that type |
| `formula` | `formula TtW(ship) = attr(ship, Thrust) / attr(ship, Mass);` | parsed and kept (body text); HXL compilation is a later work package |
| `scriptlib` | `scriptlib Physics @realm(server, client) { fn raycast(from: WorldPos, dir: vec3f) -> RayHit? @script(cost=24) @pure; }` | signatures of hand-written C++ functions Luau may call: checked (types, fuel lints) and listed in `--emit json` (`scriptlibs`, with binding ids); `--emit luau` generates the call glue |

Header attributes may omit the `@` when they are on the declaration line (`replicate(all) lod(core)`,
`reliable`), as in the spec's examples. A body is `{ members }`; members are fields (`name: type [=
default] {@attr}`), separated by `;`, `,` or line breaks, plus `client {}` / `server {}` / `editor {}`
blocks (not nested; `editor {}` is not allowed in components).

### Types

| Schema | C++ | Go | Tagged wire | Canonical JSONC |
|---|---|---|---|---|
| `bool` | `bool` | `bool` | VARINT | `true` |
| `i8`…`i64` | `helios::i8`… | `int8`… | VARINT (zigzag) | number |
| `u8`…`u64` | `helios::u8`… | `uint8`… | VARINT | number |
| `f32`, `f64` | `f32`, `f64` | `float32`, `float64` | I32, I64 (f64 readers accept I32) | shortest round-trip; `"nan"`, `"inf"`, `"-inf"`, `-0.0` |
| `string`, `Name` | `std::string`, `helios::Name` | `string` | LEN | string |
| `vec2f` `vec3f` `vec4f` `quatf` `color` | `Vec2` `Vec3` `Vec4` `Quat` `Color` | `Vec2` `Vec3` `Vec4` `Quat` `RGBA` | LEN, packed f32 | `[x, y, z]` |
| `vec3d` `quatd` `WorldPos` | `DVec3` `DQuat` `refl::WorldPos` | `DVec3` `DQuat` `WorldPos` | LEN, packed f64 | array |
| `Guid` | `helios::Guid` | `Guid` | LEN, 16 bytes big-endian | `"0f8fad5b-d9cb-469f-a165-70867728950e"` |
| `AssetRef<Kind>` | `refl::AssetRef` | `AssetRef` | LEN, 16 bytes | `"guid:…"` |
| `EntityId` | `refl::EntityId` | `EntityId` | I64 (fixed) | `"ent:<decimal>"` |
| `Ref<T>` / `FooRef` | `refl::RecordRef<T>` | `FooRef` (uint64) | I64 (fixed) | RecordId number |
| `NetHandle`, `Tick` | `refl::NetHandle`, `refl::Tick` | `NetHandle`, `uint64` | VARINT | number |
| `Duration` | `refl::Duration` (ns) | `Duration` (ns) | VARINT (zigzag) | `"1500ms"`, `"30d"` (largest exact unit) |
| `LocString` | `refl::LocString` | `LocString` | LEN | `"loc:<key>"` |
| `TagQuery`, `HxlExpr` | `refl::TagQuery`, `refl::HxlExpr` | `string` | LEN | string |
| `TagSet` | `refl::TagSet` (sorted, unique) | `TagSet` | LEN message `{1: tag}*` | `["A.B", "C"]` |
| `list<T>` | `std::vector<T>` | `[]T` (`U8List` for `list<u8>`) | packed (scalars) or one occurrence per element | array |
| `list<S> @keyed` | `refl::KeyedList<S>` | `[]Keyed[S]` | entries `{1: guid, 2: S}` | array of objects with `"$key"` first |
| `list<S> @keyed(field)` | `std::vector<S>` | `[]S` | as a list | array (diffs/paths key by `field`) |
| `set<K>` | `std::set<K>` | `[]K` (sorted) | as a list, canonical order | array |
| `map<K, V>` | `std::map<K, V>` | `map[K]V` | entries `{1: key, 2: value}` in key order | object (keys as text) |
| `T?` | `std::optional<T>` | `*T` | present or absent | value or `null` |
| `T[N]` | `std::array<T, N>` | `[N]T` | as a list | array |
| `variant { A; B { x: f32 } }` | `std::variant<…>` of structs | struct of pointers | LEN message with one field (alternative id) | `"A"` or `{"B": {…}}` |
| `enum`, `flags` | `enum class` | named integer | VARINT | `"Name"`, `["Fly", "Dock"]` |
| struct types | struct | struct | LEN nested message | object |

Keys of `map`/`set` must be integers, enums, strings, Names, Guids, EntityIds or record refs.
Not supported (errors with a hint): `list<bool>` (use `flags` or `list<u8>`), nested optionals,
optional containers (`list<T>?`: an empty container already means "none").

**Inline types** (`handling: { … }`, `mode: enum { … }`, `shape: variant { … }`) become nested types
named after the field in PascalCase (`ShipHullDef::Handling`, Go `ShipHullDefHandling`); variant
alternatives become sibling structs `<Variant><Alt>` (`EffectDef::DurationPeriodic`). A type may not
contain itself by value (use `list`, `map`, `Ref` or an optional of a record ref).

### Defaults and literals

`= 50000`, `= -1.5e3`, `= 0xFF_FF`, `= true`, `= "text"`, `= Medium` (enum value), `= [Fly, Dock]`
(flags), `= [0, 0, 1]` (tuples, arrays — shorter lists pad with zeros), `= 1.5s` / `= 250ms` / `= 30d`
(durations), `= "0f8fad5b-…"` (GUID), `= null` (optionals). Values are range-checked against the
field type and `@range`. Optionals default to none and containers to empty — nothing else is
allowed, because writers omit default values and a non-empty default would make "empty"
unencodable. `f32` defaults are rounded to `f32` at compile time; the canonical JSON of every
default is recorded in the lock and must match the runtime writer (tested).

### Attributes

All attributes of 02 §3.2 are accepted and validated for target (type / field / rpc / enum value)
and argument count; unknown attributes are warnings with a "did you mean" hint. Checked semantics:

| Attribute | Checks / effect |
|---|---|
| `@replicate(all\|owner\|server\|none)` | components only; ≤ 64 replicated fields (dirty mask); `server {}` fields are never replicated |
| `@quant`, `@predicted`, `@interp`, `@lod` | only on (fields of) replicated components; `@predicted` → `FieldFlags::Predicted` |
| `@ratelimit(n/s)`, `@intent(x)` | **AAA-SEC-1**: every `client->server` rpc needs both; rate syntax `n/s\|m\|h` |
| `@timeout(500ms)` | valid duration |
| `@reason_required` | the rpc must carry a `ReasonCodeRef` (directly or in a parameter struct) |
| `@store(checkpoint\|ledger\|character\|activity\|config)`, `@persist`, `@ledger_policy` | ledger data is never `@persist` (ADR-008); `@ledger_policy` requires `@store(ledger)` |
| `@sql(schema="svc_<service>"[, table=])`, `@key` | structs only; a PostgreSQL table for `--emit sql` ([Generated SQL](#generated-sql)); `@store(checkpoint)` is never a table; ledger data only in `svc_ledger` and only ledger data there |
| `@server_only`, `server {}`, `@opaque` | **AAA-SEC-4**: a shared field may not reference a server-only type unless `@opaque` |
| `@table`, `@exclusive`/`@acyclic`/`@target`, `@client`/`@server` | only on records, relations, viewmodels respectively |
| `@range(min, max)`, `@step`, `@unit`, `@max(n)`, `@normalized`, `@asset` | numeric/vector/container/AssetRef field checks (`@max`: elements of a container, bytes of a string, `Name` or text builtin); typed payloads in TypeInfo (`attrs::Range`, …) |
| `@editor(category=, widget=, order=)` | named arguments only |
| `@keyed` / `@keyed(field)` | list of structs; the key field must be a valid key type |
| `@was("old", …)` | field or type rename (see the lock); readers accept the old JSON key |
| `@version(n)` | positive, may only increase |
| `@merge(append)` | lists only |
| `@script(cost=n[, each=m, of=result\|<param>])`, `@pure`, `@realm(server\|client\|editor)` | scriptlib fns (02 §7.4): every `fn` needs `cost`; `each` and `of` go together; `of=result` needs `@pure` and a container result; `of=<param>` must name a parameter; a fn's `@realm` overrides its scriptlib's. On fields `@script(read\|write\|none)` |

AAA-SEC-1 also applies to `client->server` rpcs declared inside a `service`. Names that would not
compile as generated C++ are errors: C++ keywords as type names, enum values or package components
(field names get a `_` suffix instead), a field named like its struct or a nested type, and a
replicated field whose `Mut` constant would clash (`allFields`). Go generation additionally rejects
invalid Go package names and fields/alternatives named like generated methods (`MarshalHelios`, …).

**Limits** (hostile or generated input yields a diagnostic, never a stack overflow): 64 levels of
nested types / inline bodies / list literals / `?` and `[N]` wrappers, 64 chained aliases, 64 levels
of by-value struct nesting and 64 levels of nested imports. Numeric attribute arguments must be
finite decimal or hex numbers (`inf`/`nan` are rejected; `1_000` and `0x40` are fine).

Naming lints (PascalCase types and enum values, camelCase fields) are warnings (`--no-naming-lints`).

## Stable ids: the schema lock

`schemas/schema.lock.jsonc` (02 §3.4) is generated, committed and append-only (illustrative entry
after a rename and a deletion):

```jsonc
{
  "format": 1,
  "types": {
    "sample.ship.HullDamage": {
      "id": 3381868587,
      "kind": "struct",
      "version": 2,
      "nextField": 4,
      "fields": [
        {"id": 1, "name": "hp", "type": "f32", "default": "100"},
        {"id": 2, "name": "breaches", "type": "list<u8>", "was": ["holes"]},   // renamed via @was
        {"id": 3, "name": "armor", "type": "u8", "tombstone": true}           // deleted field
      ]
    }
  }
}
```

- **Type ids** are `fnv1a32(qualified name)` (salted on collision) minted once; builtins and
  containers use `fnv1a32` of their canonical name at runtime.
- **Field ids** are sequential per type (`nextField`); they are the tagged field numbers. Variant
  alternatives get ids the same way; enum values keep their numbers.
- **SQL tables**: the entry of a struct marked `@sql` records its table (`"sql": "svc_x.table"`),
  once; a table cannot move to another name or schema, and a table another entry recorded (a
  removed type, or one renamed without `@was`) cannot be taken by a new type. The loader rejects a
  table recorded twice, field names that are not identifiers and types with control characters,
  since they reach generated SQL.
- **Binding ids**: every `scriptlib` fn has an entry of kind `"fn"` with only its id, minted like a
  type id from `<package>.<Lib>.<fn>` (`"sample.ship.ShipQueries.hullOf": {"id": …, "kind": "fn"}`).
  It keys the fn's calibrated fuel cost (02 §7.4). A renamed fn gets a new id; a removed fn keeps its
  entry, so the id is never reused.
- **Renames**: `@was("old")` on a field or type keeps the id (the lock records `was`). A `@was` can
  also claim a tombstone. The attribute can be dropped once the lock has the new name. JSON readers
  (C++ compiled, walker and Go) accept the old key; when an object has both, the current name wins
  regardless of member order.
- **Deletions** become tombstones; ids and enum numbers are never reused (reusing a removed enum
  value is an error).
- **Type changes** keep the id only for widenings: `i8→i16→i32→i64`, `u8→…→u64`, `f32→f64`, `T→T?`.
  Anything else is an error ("add a new field instead"). Enum/flags underlying types may widen.
- **Explicit defaults** are part of the wire contract (writers omit defaults) and may only change
  with `--allow-default-change`.
- **Kinds** (struct/enum/flags/variant) cannot change; `@version` cannot decrease.
- **Hand edits** are detected: duplicate type or field ids, missing (deleted) entries, ids at or
  above `nextField`, duplicate names, malformed JSON.
- Entries of types that are not part of the current compilation are kept, so several schema sets
  can share one lock.
- Runs that may rewrite the lock serialize on a sibling directory `<lock>.writing`
  (`src/lock_mutex.h`), so parallel `helios_schema()` calls sharing a lock never drop each other's
  entries. It is created atomically and removed on release; a removal that fails for a moment
  (an indexer or virus scanner holding it) is retried for 2 s, then reported as a warning.
- **Killed runs.** A `.writing` older than 2 minutes was left behind by a killed run. It is
  removed, and so taken over, only by the waiter holding `<lock>.writing-takeover`, after
  re-checking its age there. A `-takeover` left behind itself (by a run killed within those few
  calls, or one that could not remove it and warned) is removed the same way under
  `<lock>.writing-takeover2`. A stale `-takeover2` would need a failure inside that second
  recovery; it is never removed automatically, and the run fails asking you to delete it by hand
  once no build is running.
- **Staleness is decided by age alone,** which assumes two things. A live run finishes within
  2 minutes (it holds the mutex for milliseconds); one suspended for longer, e.g. in a debugger or
  a paused VM, loses the mutex, and its release then removes the next holder's. And the lock is on
  a local file system: on a network share, clock skew over 2 minutes makes every mutex look stale.
  OS file locks would remove both assumptions; they need a core platform API that does not exist
  yet.
- The errors that racing runs cause for a moment are retried with a bounded back-off, for up to
  10 s of unbroken failures: the directory vanishing between the OS call and the standard
  library's is-a-directory check, and Windows' delete-pending and sharing-violation states.
  Something in the way (a file or a symlink, even a dangling one) fails at once, naming the path.
  Every wait ends in a clear error, after 5 minutes at most.
- The build updates the lock and always prints `updated schema lock …; commit it`. CI configures
  with `-DHELIOS_SCHEMA_CHECK_LOCK=ON` (or passes `--check-lock`), which fails on any change,
  including non-canonical formatting.

## Formats

**Canonical JSONC** (02 §3.7): two-space indent, LF, UTF-8, final newline; `$`-keys first
(`$key`, `$rid`, `$name`, `$parent`, `$comment`), then schema order; defaults omitted; arrays of
scalars on one line; floats in shortest round-trip form (`0.1`, `1e+21`, `-0.0`); references as
`"guid:…"` (AssetRef), `"ent:…"` (EntityId), `"loc:…"` (LocString) or a RecordId number. Readers
accept comments, trailing commas, `@was` names, the bare forms of references, and warn on unknown
members (error in strict mode). Documents nested deeper than `kMaxJsonDepth` (128 arrays/objects)
are rejected at parse time (`LimitExceeded`); the tagged reader's limit is 64 nested messages. Record files (`.hrec`) are handled by `helios/reflect/record.h`.

**Tagged binary** (normative summary; the C++ and Go codecs are byte-identical and cross-tested):

- Protobuf wire format: `tag = (fieldId << 3) | wireType`, wire types VARINT (0), I64 (1), LEN (2),
  I32 (5). Field ids come from the lock. Fields equal to their default are omitted; readers start
  from defaults and skip unknown fields (forward and backward compatible).
- Struct fields are written in declaration order. Scalars and enums use VARINT (signed integers and
  `Duration` zigzag), `f32` I32, `f64` I64, `EntityId` and record refs fixed I64.
- `Guid`/`AssetRef` are LEN with 16 big-endian bytes; math tuples are LEN with packed little-endian
  floats; strings are LEN UTF-8 (readers reject malformed UTF-8 in strings, Names and tags, as
  protobuf does); nested structs and `TagSet` (`{1: tag}*`) are LEN messages.
- `list<scalar>` (and `set`, arrays) is one packed LEN field; lists of LEN types repeat the field;
  elements that are themselves lists, optionals or maps are wrapped in a message `{1: element}`
  (an absent `1` is a none/empty element).
- `map<K,V>`: one LEN entry `{1: key, 2: value}` per element in canonical key order; keyed lists:
  `{1: guid key, 2: value}` in list order; variants: a LEN message with exactly one field (the
  alternative id; a unit alternative is an empty message).
- An optional field is written whenever it is engaged (even with a default value).
- `encodeEnvelope()` adds a versioned header: magic `HTB1`, varint type id, then the message.

## Generated C++

For `schemas/game/ship.hschema` (`package game.ship`), `ship.gen.h` / `ship.gen.cpp` contain:

- The types in `namespace game::ship`, in dependency order, with schema defaults as member
  initializers and `operator==` (bitwise float equality; `_dirty` ignored).
- `helios::refl::TypeOf<T>` and `Codec<T>` specializations (compiled JSONC/tagged codecs), static
  `TypeInfo`s with lock ids, offsets, flags, typed attributes, docs, defaults and layout hashes
  (transitive: a type's hash covers every type reachable through its fields, except record refs,
  and does not depend on declaration or file order).
- `Result<void> registerShipTypes(TypeRegistry& = TypeRegistry::global())`.
- For replicated components: `u64 _dirty`, `static constexpr auto kReplicatedFields =
  std::make_tuple(&C::a, …)` (the contract used by `ecs::Mut<C>`) and `refl::Mut<C>` with
  `setX(v)` / `editX()` / `raw()`; bit *i* is `FieldInfo::repIndex` *i*.
- `using FooRef = refl::RecordRef<FooDef>;` for records; constants and aliases.

Generated code compiles warning-free with `-Wall -Wextra` (GCC, Clang) and is MSVC-compatible
(no `__VA_OPT__`, no GNU extensions; `offsetof` warnings are suppressed locally).

## Generated Go

One Go package per `--go-out` directory: `<stem>.go` per schema file, `helios_codecs.go` (shared
codec functions), `helios_runtime.go` (a copy of [runtime/helios_runtime.go](runtime/helios_runtime.go):
wire reader/writer and vocabulary types) and `helios_schema_test.go` (round trip, corrupt-input
and C++-vector tests). Types are named by flattening (`ShipHullDef.Handling` →
`ShipHullDefHandling`); enum constants are `<Type><Value>`. Every struct has `New<T>()` (schema
defaults), `MarshalHelios()` / `UnmarshalHelios([]byte)` and JSON support compatible with the C++
reader (enums by name, variants as `{"Alt": {…}}`, durations as text, `list<u8>` as numbers,
reference prefixes, `@was` keys accepted; Go writes empty slices/maps/TagSets as `null`, which the C++
readers take as empty). Sets and TagSets encode canonically (sorted, duplicates dropped) even when a
Go slice holds them unsorted or repeated. Differences: Go's reader requires `"$key"` on keyed-list
elements (the C++ reader mints one with a warning) and, like `encoding/json`, matches member names
case-insensitively. All
files of one package must be generated together; a type whose Go name collides with a runtime
type (`Vec3`, `RGBA`, `Keyed`, …) or another type is an error.

## Generated Luau

`--emit luau` (02 §3.5, §7.4) writes, for each generated `<file>.hschema`, `<file>.luau.gen.h` and
`<file>.luau.gen.cpp` next to the C++ output, and once per compilation `schema.d.luau` and
`fuel_costs.defaults.json` into `--luau-out`. A file without scriptlibs gets an empty glue pair, so
build systems know the outputs in advance. The glue includes `helios/script/binding.h` and the
file's `.gen.h`, so a target that compiles it links `helios::script` (`helios_schema(LUAU_OUT …)`
does that).

```cpp
#include "sample/ship.luau.gen.h"

struct Ships final : sample::ship::ShipQueries {           // one virtual per fn, hand-written
    std::optional<sample::ship::ShipHullRef> hullOf(lua_State* L, helios::refl::EntityId ship) override;
    std::vector<helios::refl::EntityId> dockedShips(lua_State* L, helios::refl::EntityId station) override;
};
Ships ships;                                               // ships.glueLimits: per-call caps (below)
auto vm = ScriptVm::create(config, [&](Binder& b) {       // config.profile = HostProfile::Cell
    if (auto r = sample::ship::bindShipQueries(b, ships, "server"); !r) HELIOS_LOG_ERROR(LogScript, "{}", r.error().message);
});
```

- **Interface.** Per scriptlib `Lib`, the abstract class `<pkg>::Lib` has:
  - one pure virtual per fn, taking the `lua_State*` first;
  - `static constexpr u32 k<Fn>Binding` constants (the lock ids);
  - `GlueLimits glueLimits`, the per-call conversion caps described below.

  `bindLib(binder, impl, realm)` registers the fns whose effective `@realm` includes `realm` as the
  Luau library `Lib`, and returns `Result<u32>` with the number registered. `impl` must outlive the VM.
  The realm must be one the glue knows and the one the VM's host profile runs: `"server"` on
  `HostProfile::Cell`, `"client"` on `Client`, `"editor"` on `Editor`. Anything else, such as a typo or
  a client realm on a cell, is `InvalidArgument` and registers nothing. Implementations follow
  `binding.h`'s threading rules: owner thread, may raise Luau errors, call back into Luau only through
  `callLuau`.
- **Fuel** (02 §7.4).
  - `cost` is the binding's `FuelCost::base`, charged by the host before the call.
  - `each` with `of=<list, set, string or integer argument>` is charged by the host from that argument
    (`FuelCost::itemsArg`), before the call.
  - `of=<any other argument>` (a record ref such as a `PrefabRef`) adds a pure virtual
    `<fn>ItemCount(L, arg)`. The glue charges its count before the call.
  - `of=result` is charged right after the call, per result element.
  - The glue uses the schema's numbers. `--calibrate-fuel` (WP-1.6) replaces them by binding id; for
    that, WP-1.6 must thread `k<Fn>Binding` through `Binder::function`, which takes no binding id today.
- **Values.**
  - `bool`, and `f64`.
  - `f32` must be finite and within float's range; NaN, ±inf and 1e39 are rejected before the cast.
  - Integers are exact and range-checked; 64-bit ones only within ±(2⁵³ − 1).
  - `string`, `LocString`, `TagQuery` and `HxlExpr` are strings.
  - **`Name` is `std::string` in C++** (sets of them `std::set<std::string>`, lexical). Script input is
    never interned into the process-wide `Name` table, which never shrinks and is shared by every VM,
    and the order of a set does not depend on what the process interned before, which replays need
    (04 §10.2). An implementation that needs a `Name` interns deliberately. A `Name` inside a struct
    *argument* is therefore an error. Inside a struct *result* it is pushed as text, and a `set<Name>`
    field is pushed in lexical order.
  - `vec3f` is a Luau `vector`; its components follow the `f32` rule (NaN and ±inf are rejected).
  - `WorldPos` is the host's `WorldPos` userdata (`helios::FramePos` in C++, with its frame). It is not
    allowed inside structs, where it would lose the frame.
  - `Duration` is seconds, and `Tick` a number.
  - Enums are their value names.
  - Structs are tables: fields by schema name, missing fields take their defaults, other keys are
    ignored.
  - `list` and `set` are arrays. `T[N]` is an array of exactly N elements. `T?` is `T` or `nil`.
  - `EntityId` and record refs are **light userdata** with tags 1 (`EntityId`) and 2 (`RecordRef`):
    exact 64-bit values, comparable with `==`, usable as table keys, and impossible to forge from a
    script.
  - Other types are errors in a signature under `--emit luau`: maps, variants, flags, keyed lists,
    `Guid`, `AssetRef`, other math types, `TagSet` and `NetHandle`.
- **Hostile arguments.**
  - Every conversion reads tables raw (`lua_rawgetfield`/`lua_rawgeti`), so no metamethod or other Luau
    code runs inside a binding.
  - `@max` is enforced wherever sema accepts it, on the raw value before anything is converted. That
    covers string and text builtin (bytes), list and set (elements) parameters and the fields of struct
    parameters, recursively. Results are checked the same way: the fn's `@max` and struct fields' `@max`.
  - **Per-call budget.** One call converts at most `glueLimits.maxValues` values (8,192: every number,
    string, table and element counts, a `nil` element of a `T?` list included, so a table referenced
    from many places costs once per reference) and `glueLimits.maxStringBytes` string bytes
    (256 KiB). A list longer than the values left fails before its first element. The plan gives no
    default (02 §7.4, 04 §10.2), so these are conservative and the host may change them.
  - A value nests at most 32 tables; a cyclic table stops there.
  - Every rejection is a script error naming the fn, the argument and the cap, and the implementation is
    never called.
  - **Budget:** a call rejected by the caps fails in < 1 ms (`perf: rejecting a call over the per-call
    caps …` gates the median of 9 rejections: ≈ 0.2 ms for the reviewer's 22-table DAG, ≤ 0.05 ms for
    65,536 references to one 16 KiB string, GCC RelWithDebInfo). A call within the caps converts at
    most 8,192 values, and that work is not charged beyond the fn's `cost` (up to ≈ 0.8 ms for an
    8,190-value `echo` pushed back, measured in review). Calibration (02 §7.4) takes the p95 over
    zone traces, not the worst case at the caps, so it cannot cover this: a deterministic charge per
    converted value and string byte, taken in the glue before the call, is a WP-1.6 follow-up.
- **`schema.d.luau`** declares the scriptlib globals (each fn's doc comment, fuel charge and realms,
  `--!strict`) and the types their signatures reach:
  - `EntityId` and `<Record>Ref` as opaque `declare extern type`s;
  - enums as string unions;
  - structs as table types, named like the Go types (`ShipHullDefHandling`). An argument uses
    `<Type>Input`, whose fields are all optional because the glue defaults them; a result uses
    `<Type>`, whose fields are all present.

  Every realm's fns are declared, and in a host of another realm a fn is `nil` at runtime. Each fn's
  doc lists its realms, since one definitions file serves luau-lsp for every host. Load it after
  `engine/script/defs/helios.d.luau`. These are errors: names that collide with the host's
  (`WorldPos`, `Task`, …), Luau reserved words as fn, parameter or field names, and two declarations
  with one Luau name.
- **`fuel_costs.defaults.json`**: `{"format": 1, "functions": {"<binding id>": {"name", "cost",
  "each", "of"}}}`, sorted by id, the defaults `--calibrate-fuel` replaces in
  `content/profiles/fuel_costs.jsonc` (02 §7.4).

## Generated SQL

`--emit sql` (02 §3.5; 05 §3) writes PostgreSQL DDL for the structs marked
`@sql(schema="svc_<service>"[, table="<name>"])`, one directory per service schema in `--sql-out`:

| File | Contents |
|---|---|
| `<schema>/schema.sql` | Snapshot: `CREATE SCHEMA IF NOT EXISTS` and every table as the schemas define it now, for an empty database |
| `<schema>/migration.sql` | goose v3 stub (`-- +goose Up` / `-- +goose Down`) from the **baseline lock** to now: the lock as it was before this run, or `--sql-baseline <lock>` (e.g. the lock of the last release, from version control) |

```
helios-schemac -I schemas --lock schemas/sample/schema.lock.jsonc --emit sql --sql-out build/sql \
    --sql-baseline /tmp/lock-at-last-release.jsonc schemas/sample/*.hschema
```

- **Where tables live.** `schema` must be a service schema `svc_<service>` (05 §3, CONF-06); the table
  defaults to the snake_case type name. `@sql` is valid on structs only (records cook to `.hrdb`,
  components persist through checkpoints). `@store(checkpoint)` data is a blob in `ag_checkpoint`,
  never a table; `@store(ledger)` data lives only in `svc_ledger`, and `svc_ledger` holds only
  ledger data (ADR-008).
- **Identity.** The lock records the table of each `@sql` struct (`"sql": "svc_x.table"` on its
  entry). A table cannot move, and a new type cannot take over a table another lock entry holds
  (both errors): the stub diffs a table against its own type's baseline entry, found by lock id.
  Columns follow field ids, so renames (`@was`) are `RENAME COLUMN`, and columns appear in lock-id
  order in the snapshot and in the migrated table alike, with one exception: a field revived after the
  N+2 contract dropped its column is an `ADD COLUMN`, which PostgreSQL appends at the end (SQL that
  names its columns does not notice). A type renamed with `@was` keeps its entry, so the fields that
  use it do not change type; a renamed `@sql` struct keeps its table with `@sql(table="<old name>")`,
  since the default table name follows the type name.
- **Key.** Every table has a primary key: the fields marked `@key`, or `@key(a, b)` on the struct.
  Key fields are non-optional scalars. A new field cannot join the key of an existing table (a
  hand-written migration changes a primary key).
- **Columns** (names in snake_case; PostgreSQL's reserved words, as columns or tables, are quoted):

  | Schema | Column |
  |---|---|
  | `bool` | `BOOLEAN` |
  | `i8`, `i16`, `u8` / `i32`, `u16` / `i64`, `u32` | `SMALLINT` / `INTEGER` / `BIGINT`, with a `CHECK` of the unsigned or `i8` range |
  | `u64` | `NUMERIC(20)` with a `CHECK` of the `u64` range (PostgreSQL has no unsigned `BIGINT`) |
  | `f32`, `f64` | `REAL`, `DOUBLE PRECISION` |
  | `string`, `Name`, `LocString` (key), `TagQuery`, `HxlExpr` | `TEXT` |
  | `Guid`, `AssetRef<T>` | `UUID` |
  | `EntityId`, record refs, `Tick` | `BIGINT` with `CHECK (x >= 0)` (63-bit ids, 05 §3.2) |
  | `Duration` | `BIGINT` nanoseconds |
  | enums, flags | their underlying integer (the value numbers, not names) |
  | math tuples, `WorldPos`, `TagSet`, `list`, `set`, keyed lists, `T[N]` / structs, `map` / variants | `JSONB` holding the canonical JSONC, with `CHECK (jsonb_typeof(x) = 'array'` / `'object')` |
  | `NetHandle` | an error, also inside a `JSONB` column: it is scoped to one zone instance (04 §4.6) |

  Scalar columns are `NOT NULL DEFAULT <the schema default>` (explicit or implicit: 0, `FALSE`, `''`,
  the nil UUID, the first enum value), so `ADD COLUMN` needs no backfill; key columns have no
  default. A string default with a backslash or a control character is an `E'…'` literal (backslash
  and quote doubled, control characters as `\n`, `\r`, `\t`, `\b`, `\f` or `\xHH`), so every literal
  stays on one line (goose reads a stub line by line, and a line inside a literal that reads
  `-- +goose …` would be an annotation) and reads the same whatever `standard_conforming_strings` is.
  A string default holding U+0000 is an error (`TEXT` cannot store it, and a NUL ends psql's input
  line). `T?` columns are nullable without a default. `JSONB` columns are nullable, and NULL means
  the field's default (none for `T?`), as writers omit defaults (§3.7). `CHECK` constraints are named
  `<table>_<column>_check`.
- **Migrations only expand** (05 §3.3: release N adds, contraction happens in N+2). A new struct is a
  `CREATE TABLE`; a new field is an `ADD COLUMN` with its default; a renamed field is a `RENAME COLUMN`
  (and of its `CHECK`); a widening (`i32→i64`, `u8→u16`, `f32→f64`, `T→T?`, and an enum's or flags'
  underlying type, which changes the column but not the field's signature: the stub compares it with
  the enum's baseline entry) is an `ALTER COLUMN TYPE` with the new `CHECK`, or `DROP NOT NULL`; a
  changed explicit default is `SET DEFAULT`; a revived field is `ADD COLUMN IF NOT EXISTS`, with a
  `-- TODO` when its column changed while it was removed (before the N+2 contract the old column still
  exists and keeps its old type). A removed field is only a
  comment listing the `DROP COLUMN` for the contract release, and so is a table of the schema whose
  struct was removed or lost `@sql` (`DROP TABLE`). The lock keeps such a struct's entry and table, so
  that note stays in later stubs: the lock cannot tell when the contract release dropped the table. A
  struct the run only imports keeps its table, and a schema whose last table goes gets no stub (drop
  it by hand). Like the snapshot, this assumes one run compiles all of a service schema's structs.
  Down reverses Up, last step first. Against an up-to-date baseline the stub's Up and Down are
  `SELECT 1;`. Copy a stub into
  `services/migrations/<service>/` and review it: indexes, partitioning, grants and backfills are
  hand-written; a new `svc_ledger` table carries a `-- TODO` for 05 §3.3's range partitions, which
  `ALTER` cannot add later. The header lists the schema files sorted, so the command line's order
  does not change the output.
- **Not generated** (hand-written, or later work packages): indexes, `UNIQUE` constraints,
  partitioning, sequences, grants and backfills; timestamps (the language has no timestamp type, so
  `TIMESTAMPTZ` columns such as 05's `created_at` are hand-written); byte columns (`BYTEA`, e.g. 05 §6.6's
  `*_ct` ciphertext and `*_bidx` blind indexes); `@pii` column classes (05 §3.2, §6.6), which the language
  does not have yet. Changing the key of an existing table is a hand-written migration (the lock does not
  record keys). `helios_schema()` has no SQL option: run the CLI, since stubs are copied by hand.

## Generated replication

`--emit repl` (02 §3.5; 04 §4.1, §4.5, §4.6) writes `<file>.repl.gen.h` and `<file>.repl.gen.cpp` next
to the C++ output for every generated file. The runtime is `helios/reflect/repl.h`
(engine/reflect).

- **`RepOf<C>`** for each replicated component `C`:
  - `desc()` returns a `ComponentRepDesc`: qualified name, lock type id, audience (`all`, `owner`,
    `server`), LOD group, and per replicated field (never `server {}` fields) the name, lock id, byte
    offset, change-mask index (`FieldInfo::repIndex`, the `Mut<C>` dirty bit), LOD (`lod(near)` on the
    field overrides the component's), `@predicted`, `@interp(linear|slerp)`, quantizer and worst-case
    bits. It also holds the component's worst case and a descriptor hash.
  - `writeFullState(BitWriter&, const C&)` and `readFullState(BitReader&, C&)` carry every replicated
    field in change-mask order. This is 04 §4.2's full-state chunk; masks and deltas are WP-1.10.
- **`@quant`** forms:

  | Form | Fields | Wire |
  |---|---|---|
  | `range=±x, bits=n` (or `range=x`) | `f32`, `f64`, `vec2f`, `vec3f`, `vec4f`, `vec3d`, `color` | each component clamped to [−x, x] on 2ⁿ−1 steps |
  | `smallest3, bits=n` | `quatf` | index of the largest component (2 bits) and the other three in ±1/√2 at n bits |
  | `frame_cell, cell=<m>, res=<m>` | `WorldPos` | the position rounded to `res`, per axis a zigzag varint cell index and the offset in ⌈log₂(cell/res)⌉ bits (04 §4.5: `cell=4096m, res=1/256m` is 20 bits per axis) |
  | none | fixed-size values | raw: `bool` 1 bit, integers and enums at their width, floats as IEEE bits, ids at 64 (`NetHandle` 32) |

  `bits` is 1 to 32 (3 to 32 for `smallest3`: at 1 or 2 bits rounding pushes most quaternions past
  unit length), and `cell` must be a whole multiple of `res` (2 to 2³² steps). An argument the form
  does not use (`bits=` on `frame_cell`, `range=` on `smallest3`) is an error, not ignored. Lengths
  are metres: `m` or no unit (`cell=4km` is an error, not a 4 m cell); fractions (`1/256m`), hex
  (`0xA`) and `±` are accepted, and `range=x` is the same as `range=±x` (02 §3.1 writes `range=4096`, 04 §4.1
  `range=±4096`). The bound must fit an `f32` component for `f32` fields, and the range's width must
  be finite. With 2ⁿ−1 steps, 0 is not exact in a symmetric range (`range=±4096, bits=16` sends a
  stationary velocity as +0.0625 m/s per axis); 04 §4.5's at-rest bit (WP-1.10) is meant to cover
  velocities. The Phase 0 codec carries no strings, `Name`s, containers, structs or variants: a
  replicated field of those types is an error under `--emit repl`.
- **Determinism.** Quantizers use f64 arithmetic with round-half-up, and frame cells use integer
  steps, so a cell and a client produce the same bits (04 §4.5), and a decoded state re-encodes to
  identical bits.
- **Non-finite input** stays decodable: range maps NaN to its minimum, frame cells map a non-finite
  axis to 0, and smallest-three normalises its input and sends a non-finite or zero quaternion as the
  identity (and steps a rounded-up component back toward 0 when the three would exceed unit length),
  so a reader always accepts what a writer wrote.
- **Hostile input.** Readers are bounds-checked and never overread. They reject truncated streams,
  varints longer than 10 bytes or overlong, frame-cell offsets of a whole cell or more, cell indices
  whose position would overflow, enum values and flag bits the schema does not declare, and
  smallest-three components whose squares sum to more than 1 (no unit quaternion sends them).
- **`<stem>Replication()`** returns the file's `FileRepTables`:
  - its replicated components;
  - its top-level rpcs, with direction, reliability, `@ratelimit` per second and `@intent` (service
    rpcs are backend calls, 05, not netcode);
  - its events, with `@audience(owner|relevant|party)`, default `relevant`, and reliability
    (`@unreliable` is EVENT_U, otherwise EVENT_R; 04 §2.2);
  - a **protocol hash**, which is `protocolHash()` over the descriptor, rpc and event hashes.
  `protocolHash()` over several files' hashes gives a build's hash, independent of order; it is an
  input of 05 §1.14.1's compat fingerprint. For components it covers type and field ids, names,
  types, audience, LOD, prediction, interpolation, every quantizer parameter, each field's worst-case
  bits, and the underlying types (the raw wire width) and values of the enums and flags the fields
  use. For rpcs and events it covers the direction, reliability, rate, intent and audience, and the
  payload: each argument's or field's lock id, name, type, explicit default and `@max`, a top-level
  rpc's `-> T` result type (04 §4.6 defines no reply yet; the result is hashed so that peers agree on
  it), and the fields, defaults, `@max`es, enum and flags underlying types and values, and
  alternatives of every struct, variant, enum and flags type the payload reaches (by name; reached
  types contribute no lock ids, so the hash does not depend on which files are compiled). So any
  wire change changes it, and comments or declaration order do not.

## Lint report

`--emit lint` (02 §3.5: "SEC-1/SEC-4, ledger/persist, keyed lists, naming, size budgets"; for CI):

- **Size budgets** are warnings, printed like any diagnostic with the rule id in brackets. `--Werror`
  makes them errors.
  - `size.unbounded`: a `string`, `Name`, text builtin (`LocString`, `TagQuery`, `HxlExpr`: strings on
    the wire, which take `@max(n)` bytes like `string`), `TagSet`, `list`, `set`, keyed list or `map`
    needs `@max(n)` when it is network input. That means it is reachable from rpc arguments or `-> T`
    results (top-level and service rpcs: the result of a server→client rpc is the client's reply),
    events, messages, or the replicated fields of components, through struct and variant fields.
    Otherwise one peer could make the receiver allocate at will. A struct reached from several network
    types is reported once. `@max` bounds a field's own length or count only, so an element that would
    need a bound but cannot carry one is a finding too: a string, `Name`, text builtin, `TagSet` or
    container inside a `list`, `set`, `map` (keys included) or `T[N]`, such as `string[4]`,
    `list<string>`, `map<string, u8>` or `list<list<i16>>`. Hold such elements in a struct with a
    bounded field. An rpc result that is itself a string or container cannot carry `@max` either:
    return a struct with a bounded field. `T[N]` needs no `@max` (its count is fixed); its elements are
    checked like any container's. Service rpcs (backend calls, 05) and NATS `message`s count as network
    input too, which is conservative.
  - `size.unreliable`: an unreliable rpc's worst-case tagged payload, and its result's, fits one
    message on an unreliable channel: 1,186 B, engine/net's `wire::maxPayloadFor(Channel::Latest,
    wire::kMaxPacketPayload)` (the 1,200 B netcode payload less reliable's 9 B header and the message's
    own header, 04 §2.1; EVENT_U allows 1,188 B), since `Connection::send` refuses a larger message
    rather than fragment it. `schemac_tests` pins the number to `wire.h`. The worst case counts tags,
    length prefixes, 10-byte varints and `@max` bytes per string and elements per container (each
    entry with its tag, length and a keyed list's key, 22 B), saturating rather than wrapping, and is
    computed once per type, so shared struct graphs stay linear. 02 §3.7 sends rpcs bit-packed, which
    is never larger than the tagged form, so this budget is an upper bound; `@max` on a `TagSet` is
    counted as bytes, which is approximate. **Not covered yet (WP-1.10):** reliable gameplay rpcs and
    events have the same one-message limit (04 §2.2: reliable fragmentation serves only CONTROL, and
    `channel.h` does not mark EVENT_R jumbo), and WP-1.10's rpc header will come out of the budget;
    WP-1.10, which defines both, owns extending the check to them.
- **Gate.** The CTest `lint_schemac_size_gameplay` (label `lint`, so every CI test job runs it) runs
  `--emit lint --Werror --check-lock` over `schemas/gameplay`, the schemas compiled into the engine:
  a size finding there fails CI. `schemas/sample` keeps known findings, pinned by the corpus golden
  `tests/golden/corpus/sample/schema.lint.json.expected`, and `lint_schemac_size_fixture_sample` runs
  the same command over it and must fail with a `[size.unbounded]` error, so a gate that stops
  reporting fails as well. A new production schema directory adds its own gate.
- **Report** (`--lint-out`, canonical JSON):
  - `checked`: counts per rule (rpcs, client→server rpcs, fields under the SEC-4 check, ledger
    types, keyed lists, scriptlib fns with charges, fields and rpc results under `size.unbounded`, each
    once however many network types reach it, unreliable rpcs);
  - `clientToServer`: every client→server rpc with its `@ratelimit`, `@intent` and reliability.
    This is AAA-SEC-1's "every client→server message is classified", and a compilation fails before
    the report if one is missing;
  - `findings`: every finding with rule id, include-relative file, line, column and message, sorted
    by the printed file, line and column (as `clientToServer` is by the printed rpc name). Other
    compiler warnings, such as naming, appear under rule `schemac`. `files` is sorted too, so the
    report does not depend on the command line's order or the checkout's location.
  - A run that fails, for example on a size finding under `--Werror`, still writes the report (the
    other outputs are not written), so CI keeps the report of a failing gate.

## CMake: `helios_schema()`

```cmake
helios_schema(<target>
    FILES <a.hschema> ...           # schemas to compile (imports are found through INCLUDE_DIRS)
    [INCLUDE_DIRS <dir> ...]        # import roots / output layout (default: ${PROJECT_SOURCE_DIR}/schemas)
    [LOCK <file>]                   # default: ${CMAKE_CURRENT_SOURCE_DIR}/schema.lock.jsonc
    [CPP_OUT <dir>]                 # default: ${CMAKE_CURRENT_BINARY_DIR}/<target>_schema
    [GO_OUT <dir> [GO_PACKAGE <n>]] # also generate Go
    [JSON_OUT <file>]               # also write the schema description
    [LUAU_OUT <dir>]                # also generate the Luau glue (links helios::script), schema.d.luau
                                    # and fuel_costs.defaults.json
    [REPL]                          # also generate <file>.repl.gen.h/.cpp (replication descriptors)
    [SAMPLES])                      # also generate <file>.samples.gen.h
```

It adds a custom command with a depfile (edits to any schema, import or the lock regenerate),
adds the generated `.cpp` files to the target, puts `CPP_OUT` on its include path and links
`helios::reflect`. The command depends on the `helios-schemac` target (`$<TARGET_FILE:…>`), so it is
rebuilt first. A target may call `helios_schema()` several times.

**Cross-compiling** (e.g. MinGW from Linux): schemac must run on the build host.
- `-DHELIOS_HOST_SCHEMAC=/path/to/host/helios-schemac` uses a prebuilt host binary (recommended for
  CI: build it once in the host configuration);
- with `CMAKE_CROSSCOMPILING_EMULATOR` set (e.g. Wine), the target binary runs through the emulator;
- otherwise a host copy is built automatically with `ExternalProject` in `<build>/host-schemac`
  (host compilers from `HELIOS_HOST_C_COMPILER` / `HELIOS_HOST_CXX_COMPILER`, else the defaults;
  `CC`/`CXX`/`*FLAGS`/`CMAKE_TOOLCHAIN_FILE` from the environment are ignored for this sub-build,
  and a Visual Studio generator builds it for the default host platform).

`-DHELIOS_SCHEMA_CHECK_LOCK=ON` makes every `helios_schema()` pass `--check-lock` (CI).

## Tests

`schemac_tests` (doctest): lexer/parser (valid grammar incl. the 02 §3.1 example verbatim with its
scriptlibs, ~40 malformed inputs with exact `file:line:col` diagnostics, recovery, nesting limits),
semantic analysis and lints (incl. scriptlib fuel lints and pathological alias/struct/import chains),
the lock (stability, renames, tombstones, widening, defaults, enums, `--check-lock`, hand edits),
the generated sample code (registration, metadata, JSONC/binary round trips compiled vs.
reflection walker, evolution tolerance, fuzzed corrupt input, `Mut<C>`, property paths,
diff/patch, record files, default-value and number-format consistency with the runtime), golden
files, the CLI (incl. parallel runs sharing one lock), the lock mutex (the `create_directory()`
races of each standard library, injected; bounded waits; release retries; takeover of a stale
mutex and of a left-behind takeover directory by one waiter, with the re-checks injected;
mutual exclusion under real contention), deterministic fuzzing of schemas and locks, Go interop (`go vet` + `go test` on the generated packages and byte-for-byte
vectors in both directions; skipped when Go is missing or `HELIOS_SKIP_GO=1`) and the 2,000-type
performance budget of 02 §3.5 (asserted in optimized builds without sanitizers).

Golden files live in `tests/golden/`. After an intended generator change:

```
HELIOS_UPDATE_GOLDEN=1 build/<dir>/bin/schemac_tests -tc="golden*"
```

and review the diff of `tests/golden/*.expected` like any code change.

The golden fixture's expectations include the SQL outputs (`svc_golden.schema.sql` and a migration stub
against `golden.sql-baseline.lock.jsonc`, the lock of the fixture's previous release), and the Luau outputs (`golden.luau.gen.*`, `schema.d.luau`,
`fuel_costs.defaults.json`); `tests/golden/corpus/<set>/` holds the same for the committed
`schemas/` corpus, compiled as CMake compiles it. `test_luau.cpp` runs the generated glue of the golden
fixture and the sample schemas on a real engine/script VM (every value form, realms, the fuel charged
per fn, hostile arguments: wrong types, out-of-range and inexact integers, oversized and cyclic tables,
metatables that must not run), type-checks `schema.d.luau` with Luau.Analysis (a strict script passes;
wrong argument and result types and unknown enum values fail), and covers binding ids in the lock and
the signatures `--emit luau` rejects. PR #22's review round 1 added: the per-call value and byte caps
(the reviewer's 10-level DAG and 65,536 references to a 16 KiB string fail fast, with a `perf:` budget),
Names never interned and sets of names in lexical order, `@max` on string parameters, struct fields and
results, exact `T[N]`, exact integers up to 2⁵³ − 1, finite `f32`, and realm checks against the host
profile. 22 mutants of the generator (the reviewer's 11 and 11 more) are each killed by a behavioural
case. `test_lint.cpp` covers both size budgets (what counts as network
input, `server {}` fields, `T[N]`, one report per struct, the exact worst case against 1,200 B), the
report's positions and SEC-1 table, and `--Werror`. `test_repl.cpp` runs the generated replication code of the golden
fixture and the sample schemas: descriptors against the schema and the `TypeInfo` (ids, offsets,
change-mask indices), full-state round trips within each quantizer's precision (and re-encoding to the
same bits), every truncated prefix and random input rejected cleanly, an undeclared enum value, the rpc
and event tables, the protocol hash (stable under comments and the order of files, changed by a
quantizer, an audience, an enum's values, an rpc argument's or event field's type, name or default,
and a field of a struct an rpc reaches), undeclared flag bits, and 22 `@quant` / field diagnostics. `test_sql.cpp` covers the column mapping, the migration stub
(renames, widenings, `T→T?`, new columns and tables, removed fields and tables as contract comments,
Down in reverse, the empty stub, `--sql-baseline`), string defaults (one line, `E'…'`, no NUL) and
the rules of `@sql`. The CTest `schemac_sql_postgres`
(`tests/sql_postgres.cmake`) runs them on a real PostgreSQL: it compiles `tests/sql/v1` and `v2` as two
releases, checks that v1's and v2's stubs (plus v2's contract step) build exactly v2's snapshot
(`pg_dump --schema-only`), that v2's Down returns to v1's snapshot, and that the committed SQL goldens
apply. It prints `SKIPPED:` without PostgreSQL binaries (`PGBIN`, `/usr/lib/postgresql/*/bin`), runs the
server as `nobody` when started as root, and is not registered on Windows or when cross-compiling.

## Deviations and limitations

- **Namespace `helios::refl`** instead of the spec's `helios::reflect`: `engine/math` declares a
  function `helios::reflect()` (vector reflection), which makes a namespace of the same name
  ill-formed. The CMake target is still `helios::reflect` and the headers live in
  `helios/reflect/`. Resolving it needs a rename in `engine/math` (outside this package).
- `refl::EntityId` / `refl::NetHandle` / `refl::Tick` mirror `ecs::EntityId{u64}`,
  `ecs::NetHandle{u32}` and `ecs::Tick = u64`; `engine/ecs` should alias the `refl` vocabulary
  types (or vice versa) so generated components use one set.
- Not generated yet (later work packages): `registerComponents(ecs::World&)` / flecs traits,
  replication change masks, deltas and variable-size replicated fields (WP-1.10), cooked layouts (`Cooked<T>`), NATS stubs, the Luau
  tagged-userdata glue for components and records (`@script(read|write)` fields; WP-1.6, with the
  host's `Entity` type), editor JSON, record cooking, HXL compilation of formulas and
  `@validate`, `upgrade<T>` hooks for `@version`.
- **Luau, Phase 0 choices** (02 §3.5 leaves them open). The glue owns light-userdata tags 1 and 2 for
  `EntityId` and record refs, and `EntityId` is declared in `schema.d.luau`; both belong in
  engine/script (tags and `helios.d.luau`) once WP-1.6 defines the host's entity type. Until then
  definitions files of separate compilations each declare `EntityId`, so load them into separate
  luau-lsp environments, or compile the packages one realm uses together. Record refs share one tag:
  the type checker tells `ThingRef` from `ShipHullRef`, the runtime does not. `@realm(world)`
  (05 §1.23) is not accepted yet (no `worldscript` support).
- Struct inheritance (`struct A : B`) is not supported (embed a field).
- Go output is `go vet`-clean but not `gofmt`-formatted (run `gofmt` before committing Go); the
  runtime is copied into each generated package; Go's JSON output is compatible with (readable
  by) the C++ reader but not canonical JSONC (C++ owns the canonical form).
- `list<bool>` is not supported.
- `Name` values read from untrusted input are interned (`helios::Name`); servers should bound the
  input size before parsing.

## Plan conformance

Plan-Rev: 11

Written to plan revision 11 by WP-0.7b (the Phase 0 emitters, 09 §2: `luau`, `sql`, `repl` and `lint`), after being
reconciled by hand with revision 6 on 2026-09-25 under `docs/plan/09-roadmap-and-process.md`
§5.10.2 D7. Revisions 7–11 changed no anchor of this package. No conformance delta is open; see
§5.10.4 (c) there.
