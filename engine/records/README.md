# engine/records — the cooked record database

`helios::records` (L2, HEADLESS; namespace `helios::records`, headers `helios/records/*.h`) is 02 §1.1's
"Record DB, client/server split, tag registry" module (02 §3.3, §3.7, §6.5). v0 (WP-0.8 part 2 of 3) holds:

- **the records cook** (`cook.h`): `.hrec` canonical JSONC sources → `records.client.hrdb` and
  `records.server.hrdb`, with `$parent` inheritance, reference, tag and formula checks and the
  client/server split (AAA-SEC-4);
- **cooked layouts** (`layout.h`): how a value of any reflected type is laid out for one audience, and the
  AAA-SEC-4 type rules;
- **the `.hrdb` v0 format** (`hrdb_format.h`) and **its zero-copy loader** (`record_db.h`): every offset and
  size validated before the database is handed out, lookups by `RecordId` and `$name`, the tag table, typed
  views, and decoding back into objects.

Depends on `helios::core`, `helios::reflect` and `helios::hxl` (02 §1.1 also lists `asset`; v0 does not need it,
so it is not linked). File IO goes through core's platform layer (`fs::MappedFile`, `fs::writeFile`). The CLI is
[`helios-cook records`](../../apps/tools/helios-cook/README.md).

## Headers

| Header | Contents |
|---|---|
| `cook.h` | `SourceRecord`, `CookOptions` (strict unknown fields, HXL parameters, tag-declaration types), `cook()`, `CookOutput`/`CookStats`/`CookDiagnostic`, `collectSources()` (`records/<table>/**/*.hrec` typed by `@table`), `writeCookOutput()`, `isValidTagName()` |
| `layout.h` | `Enc`, `CookedLayout`, `LayoutCache` (per audience), `keepsField()`, `excludesType()` |
| `hrdb_format.h` | `CookAudience`, `TagIndex`, `hrdb::` constants, the header and its checksums, `hrdb::seal()` |
| `record_db.h` | `RecordDb` (`openFile`, `openBytes`, `find`, `findByName`, `record`, `tag`, `findTag`, `decode`), `RecordView`, `ValueView`, `TagView` |
| `records.h` | Umbrella |

## Usage

```cpp
using namespace helios::records;

// Cook (tools: helios-cook, later the asset processor).
auto sources = collectSources(projectRoot, refl::TypeRegistry::global()).value();
std::vector<CookDiagnostic> errors;
auto out = cook(sources, {}, &errors);          // fails with every error, sorted by file
writeCookOutput(*out, cookedDir);               // records.client.hrdb, records.server.hrdb

// Load (client, cell): memory-mapped, validated once, then read in place.
auto db = RecordDb::openFile(cookedDir / "records.client.hrdb", refl::TypeRegistry::global(),
                             {CookAudience::Client}).value();
RecordView item = db.findByName("itm/scrap_plate");         // or db.find(rid)
f32 mass = item.value.field("mass").asF32();
for (TagIndex t : item.value.field("tags").tags()) use(db.tag(t).name);
sample::items::ItemTemplateDef copy;                         // or decode into the generated type
db.decode(item, &copy);
// An HxlExpr field: view.asText() is the source, hxl::Program::decode(view.hxlBytecode()) the program.
```

## Rules

**Sources (02 §3.3, §3.7).** One record per `.hrec` file: `$rid` (a non-zero 63-bit `RecordId`, minted once),
`$name` (required and unique), optional `$parent` and `$comment`, then fields. Unknown fields and unknown
`$` keys are errors (`CookOptions::strictUnknownFields`), and so are reader warnings: a keyed-list element
without a `$key` would otherwise get a random key and make the cook non-deterministic. Enum and flag values
must be declared values (the readers also accept bare integers). Two sources with the same path, and two
record types with the same `TypeId` (the cooked type table and the loader find types by it), are errors.

**Inheritance.** `$parent` names a record of the same type by `$name`; parents resolve first (iteratively,
so a long chain cannot exhaust the stack). Fields override; nested structs merge field by field; lists
replace unless the field is `@merge(append)`; keyed lists merge by key (`$key` GUIDs, or the `@keyed(field)`
value): a child element with a parent's key merges into that element, a new key appends, and `null` clears
the list. Merging applies only to inherited data: in a record without `$parent`, and inside an element the
child adds, a keyed or `@merge(append)` list replaces the field's default, as `refl::readRecord` reads it.
Maps, sets, optionals, arrays, variants and scalars replace (02 §3.3 names only lists and keyed lists;
replacing is the v0 choice for the rest). Unknown parents, cycles, a parent of another type,
duplicate `$rid`s (T28: reused ids are rejected) and duplicate `$name`s are errors, reported per file in a
deterministic order.

**Checks.** Every non-zero `RecordRef` resolves to a record of its declared type. Every `HxlExpr` compiles
with engine/hxl, as a bare expression over `CookOptions::hxlParams` (default `self`) or in its own
`formula Name(a, b) = …` form; the cook stores its HXL1 bytecode beside the source. Tag names are valid
(dotted `[A-Za-z_][A-Za-z0-9_]*` segments, gameplay's rule). Every `TagQuery` parses with gameplay's grammar
(`all(A, B) any(C) none(D)`, each clause at most once, at least one tag per clause; a copy of
`engine/gameplay/src/tags.cpp`'s parser, since L2 cannot link L4, and `records_tests` checks that both refuse
the same texts with the same message): a malformed query fails the cook (`query: tag query: expected ',' or ')'
at offset 14`) instead of failing at run time, and only its tag names take a `TagIndex`.

**Client/server split and AAA-SEC-4 (01 §3.8; 02 §3.3, §6.5).** A record type marked `@server_only` (or a
component's `::Server` part) goes only to the server cook; `@client_only` only to the client cook. The client
layout drops `server {}` / `@server_only` fields, the server layout drops `client {}` / `@client_only` fields,
and both drop `editor {}` fields. A field a cook keeps may not embed a type of the other side by value at
all, and may reference a record type of the other side only when it is `@opaque`: the reference then cooks
as the bare `RecordId` and the record stays out of that cook. The cook refuses a violation with an error that
names the cook, the record type, the field and the offending type. schemac already refuses such schemas; the
cook's own check also covers builder-reflected types and, later, dynamic packages (02 §3.8). Both directions
are checked ("CI lints both directions", 02 §6.5).

**Tags (06 §1.1).** The tag table holds every tag a `TagSet` value, a tag query or a formula (`tag(p, A.B)`)
uses, every tag a tag-declaration record declares (`CookOptions::tagDeclarations`, by default
`helios.gameplay.TagDef` with its `tag` and `replicate` fields), and all their ancestors, numbered in
byte-wise name order: the same `TagIndex` values `gameplay::TagRegistry` assigns to the same declarations
(`records_tests` checks it entry by entry). Both cooks carry the same table, so indices agree on client and
server; the client cook withholds the names of tags that only server-only data uses. A withheld entry keeps
its slot, parent, depth, subtree range, audience and declared flag, so a client can count the server-only
tags, see where they hang in the tree and which two visible names each sorts between: index agreement needs
the slots, and only the names are secret (AAA-SEC-4's scorecard entry records this). Both cooks carry the same
`tagTableHash` in their header (the client's view of the table: no withheld name), so a client and server
cook from different runs can tell their numberings apart. Tag queries are stored as text (gameplay compiles
them against the hot set).

## The `.hrdb` v0 format (02 §3.7)

Little-endian, relocatable, read in place. A 64-byte header `{magic 'HRDB', formatVersion, audience,
rootTypeId, flags, layoutHash, size, contentHash, tagTableHash, headerHash}`, a 64-byte root of `RelSpan`s (types, records
sorted by `RecordId`, the name index, the tag table, the visible tags), the tables, then names, record values
and tag names. Every reference is self-relative and points forward (`RelSpan<T>` = `{i32 offset, u32 count}`,
`RelPtr` = `{i32 offset}`), and every out-of-line block starts 16-byte aligned. Values are laid out by
`CookedLayout` (`layout.h` lists every encoding). A type may reach itself through a list, set or map (a
dialogue node whose choices each hold an optional next node): container elements are laid out after the
by-value chain that reached them closes, so any of the types may be laid out first; a type that holds itself
by value (directly or through a struct, optional, array or variant) is refused. The layout hash covers the format version, the audience
and the cooked layout of every record type, computed the same way by the cooker and the loader whatever
order they build layouts in, so a database cooked against another schema fails with `VersionMismatch`
("recook") instead of being misread; it is what keys the DDC (02 §3.4).

**Validation.** `RecordDb::open*()` checks the header, both XXH3-64 checksums, every table, and every
record's value against its layout: spans in bounds, aligned and forward; bools 0 or 1; declared enum values
and flag bits; UTF-8 text; ascending tag indices that exist (and, in a client cook, name a tag whose name it
carries); the name index a sorted permutation. Out-of-line data is walked at most once per byte of the file
(more means shared or overlapping blocks, as in an exponential DAG) and at most `hrdb::kMaxNesting` (32)
levels of non-empty lists deep, so validation is linear in the file size. After that the views read without
checks, so the guarantee holds for bytes that do not change: a file changed on disk while `openFile()` has it
mapped is outside it (cooks are written to a new file and renamed into place, never modified in place).
`records_fuzz_hrdb_loader` fuzzes it (below).

**Determinism.** Output depends only on the sources and the types: sets and maps are written in canonical
key order (never the container's, which for `Name` keys depends on interning), padding is zero, and errors are
sorted. `records_tests` pins the XXH3-64 of both cooks of `tests/data/project`; the same values must come
out of every toolchain (GCC and Clang locally; MSVC and clang-cl run the same test in CI).

## Budgets (measured by `records_tests_perf`, label `perf`)

| What | Budget | Source | Measured (GCC 13, RelWithDebInfo, shared 4-vCPU dev VM) |
|---|---|---|---|
| Cook | ≤ 600 ms per 1,000 records | AAA-CNT-5: 100k records compile in ≤ 60 s | 35.6 ms per 1,000 (20,100 records with every encoding, 10 % inheriting) |
| Open (map + full validation) | ≤ 20 µs per record | AAA-CNT-5: 100k records load in ≤ 2 s (02 §5.7) | 1.24 µs per record |
| `find()` by `RecordId` | ≤ 1 µs mean | this module's own (the plan sets none) | 111 ns |

Timings are asserted only in optimized builds without sanitizers. 02 §6.4 also budgets the hot-reload step
"Build: records ≤ 300 ms"; v0 always cooks every record (20,100 records take about 0.7–1 s here), so meeting
it for large projects needs the incremental cook of the asset processor (WP-2.x), which is not built.

## Tests

`records_tests` (doctest; test types in `tests/schema/records_test.hschema`, records in `tests/data/project`):
the project round trip (every record without `$parent` decodes to what `refl::readRecord` reads, minus what
its cook drops at any depth), inheritance (override, struct merge, both keyed merges, `@merge(append)`,
two-level chains, merges replacing non-empty defaults when nothing is inherited), tag queries (gameplay's
grammar and messages), a type recursive through a list of structs that hold it by value,
every source and inheritance error (reported together, sorted, identical for shuffled inputs), keyed-list key
errors, references, HXL compilation and evaluation, the tag table against `gameplay::TagRegistry`, the golden
hashes and the shared tag-table hash, the nesting limit, `collectSources`/`writeCookOutput`/`openFile`,
duplicate paths and `TypeId`s, layouts (alignment, the audience filter, order-independent hashes, recursion
through containers in any build order, by-value cycles and zero-length arrays refused), AAA-SEC-4 (planted
sentinels in the client and server bytes, among them `server {}` fields of structs nested in keyed lists merged
and appended through `$parent` by `$key` and by `@keyed(slot)`, in an optional, a struct field, a map value and
a plain list, with their server-only tags; withheld tag names, `@opaque`, refusals of a shared reference to a
server-only record, of an embedded server-only struct or enum, and of the reverse direction, a forged
audience), and the loader on hostile input (every truncation,
every single-bit flip with resealed checksums, crafted spans and values, an exponential DAG, another schema or
audience).

`lint_records_sec4`, `lint_records_sec4_m0_sample` and `lint_records_sec4_fixture_leak` (label `lint`,
registered by helios-cook) cook `samples/project` and the M0 sample project and search the raw bytes for
planted server-only sentinels. `lint_records_sec4_nested` does the same over `tests/data/project`, whose
types put `server {}` fields inside shared structs (the sample schemas have none), with
`records_fixture_cook`, a test-only copy of `helios-cook records` that registers the test types.

`records_fuzz_hrdb_loader` (label `fuzz`): libFuzzer target for the loader, built like engine/asset's (with
`-DHELIOS_RECORDS_LIBFUZZER=ON` and Clang it links libFuzzer; otherwise engine/asset's standalone driver replays
`fuzz/corpus/hrdb_loader` plus 20,000 deterministic mutations on every PR). Each input also runs resealed
(`hrdb::seal`), so mutations reach the validators behind the checksums. Regenerate the seeds after a format or
test-schema change with `records_fuzz_hrdb_loader --make-seeds fuzz/corpus/hrdb_loader`.

## Not in v0

- schemac's `--emit records` stays a stub: it would need the runtime types of a package (02 §3.8's `.htypes`
  bundle) to cook without linking generated C++; `helios-cook` links the native packages instead.
- `Cooked<T>` structs generated per type (02 §3.5 `cpp` row): reads go through `ValueView` or `decode()`.
- Tag queries are checked but cook as text; `TagQuery` masks and the hot set stay gameplay's (06 §1.1).
- An incremental cook (02 §6.4's hot-reload budget) and an atomic write of the client/server pair:
  `writeCookOutput()` replaces each file atomically, one after the other; `tagTableHash` tells a mismatched
  pair apart.
- Localization keys (02 §3.5 `records` row "loc keys") are cooked as text; no string-table extraction.
- `@authoring` types are not stripped (no record type uses them yet); field `@version` upgrade hooks.
- Part 3 of WP-0.8: `.meta` sidecars, the DDC key and the local DDC store.

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (01 §3.7, §3.8; 02 §1.1, §3.3–§3.7, §5.7, §6.5; 06 §1.1; 09 §2 WP-0.8) on
2026-10-04. Choices the plan leaves open are recorded above: replace semantics for maps, sets, optionals,
arrays and variants under `$parent`; `null` clearing an inherited list; the reverse (client-only) direction of
AAA-SEC-4 enforced like the forward one; withheld tag names in the client cook; the nesting limit; the `asset`
dependency not taken.
