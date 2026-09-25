# helios-schemac — the Helios schema compiler

`helios-schemac` compiles `.hschema` files into C++ (types, reflection, codecs), Go (types and a
byte-identical codec) and a machine-readable schema description. It implements ADR-004.

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
| `luau`, `repl`, `sql`, `proto`, `editor`, `records`, `lint`, `docs` | Planned; `--emit <name>` fails with exit code 2 "not yet implemented" |

The lints of the planned `lint` emitter (AAA-SEC-1, AAA-SEC-4, ledger/persist, keyed lists,
naming) already run on every compilation.

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
| `--emit cpp,go,json` | Generators (default `cpp`) |
| `--cpp-out`, `--go-out`, `--go-package`, `--json-out` | Output locations |
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
  (TypeInfo/FieldInfo `doc`, C++ and Go comments).

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
| `scriptlib` | `scriptlib Physics @realm(server, client) { fn raycast(from: WorldPos, dir: vec3f) -> RayHit? @script(cost=24) @pure; }` | signatures of hand-written C++ functions Luau may call: checked (types, fuel lints) and listed in `--emit json` (`scriptlibs`); the call glue is `--emit luau` (later work package) |

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
| `@server_only`, `server {}`, `@opaque` | **AAA-SEC-4**: a shared field may not reference a server-only type unless `@opaque` |
| `@table`, `@exclusive`/`@acyclic`/`@target`, `@client`/`@server` | only on records, relations, viewmodels respectively |
| `@range(min, max)`, `@step`, `@unit`, `@max(n)`, `@normalized`, `@asset` | numeric/vector/container/AssetRef field checks; typed payloads in TypeInfo (`attrs::Range`, …) |
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
- Runs that may rewrite the lock serialize on a sibling directory `<lock>.writing` (created
  atomically; one left behind by a killed run is taken over after 2 minutes), so parallel
  `helios_schema()` calls sharing a lock never drop each other's entries.
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

## CMake: `helios_schema()`

```cmake
helios_schema(<target>
    FILES <a.hschema> ...           # schemas to compile (imports are found through INCLUDE_DIRS)
    [INCLUDE_DIRS <dir> ...]        # import roots / output layout (default: ${PROJECT_SOURCE_DIR}/schemas)
    [LOCK <file>]                   # default: ${CMAKE_CURRENT_SOURCE_DIR}/schema.lock.jsonc
    [CPP_OUT <dir>]                 # default: ${CMAKE_CURRENT_BINARY_DIR}/<target>_schema
    [GO_OUT <dir> [GO_PACKAGE <n>]] # also generate Go
    [JSON_OUT <file>]               # also write the schema description
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
files, the CLI (incl. parallel runs sharing one lock), deterministic fuzzing of schemas and locks, Go interop (`go vet` + `go test` on the generated packages and byte-for-byte
vectors in both directions; skipped when Go is missing or `HELIOS_SKIP_GO=1`) and the 2,000-type
performance budget of 02 §3.5 (asserted in optimized builds without sanitizers).

Golden files live in `tests/golden/`. After an intended generator change:

```
HELIOS_UPDATE_GOLDEN=1 build/<dir>/bin/schemac_tests -tc="golden*"
```

and review the diff of `tests/golden/*.expected` like any code change.

## Deviations and limitations

- **Namespace `helios::refl`** instead of the spec's `helios::reflect`: `engine/math` declares a
  function `helios::reflect()` (vector reflection), which makes a namespace of the same name
  ill-formed. The CMake target is still `helios::reflect` and the headers live in
  `helios/reflect/`. Resolving it needs a rename in `engine/math` (outside this package).
- `refl::EntityId` / `refl::NetHandle` / `refl::Tick` mirror `ecs::EntityId{u64}`,
  `ecs::NetHandle{u32}` and `ecs::Tick = u64`; `engine/ecs` should alias the `refl` vocabulary
  types (or vice versa) so generated components use one set.
- Not generated yet (later work packages): `registerComponents(ecs::World&)` / flecs traits,
  `ComponentRepDesc` and quantizers (`repl`), cooked layouts (`Cooked<T>`), NATS stubs, Luau
  glue, SQL migrations, editor JSON, record cooking, HXL compilation of formulas and `@validate`,
  `upgrade<T>` hooks for `@version`.
- Struct inheritance (`struct A : B`) is not supported (embed a field).
- Go output is `go vet`-clean but not `gofmt`-formatted (run `gofmt` before committing Go); the
  runtime is copied into each generated package; Go's JSON output is compatible with (readable
  by) the C++ reader but not canonical JSONC (C++ owns the canonical form).
- `list<bool>` is not supported.
- `Name` values read from untrusted input are interned (`helios::Name`); servers should bound the
  input size before parsing.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
