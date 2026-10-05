# engine/assetpipe — asset pipeline (editor and tools)

`helios::assetpipe` (L4, EDITOR_ONLY; namespace `helios::assetpipe`, headers `helios/assetpipe/*.h`) holds the
editor-side asset pipeline (02 §6.1–§6.3). After WP-0.8 parts 1 and 3 it has:

- the **`.hpak` v0 writer** (02 §6.3; part 1), so that the reader in [`engine/asset`](../asset/README.md)
  round-trips real paks; `helios-pack` (08) drives it later;
- **`.meta` sidecars v0** (02 §6.1, 07 T24): GUID, importer and version, settings, labels, source-DCC path,
  provenance and licence, in canonical JSONC; created on first import, validated on load, moved with their file;
- the **DDC key** (02 §6.2) and the **local DDC store** (07 §3.2): content addressed, atomic, verified, LRU
  under a size cap, with a fuzzed entry reader;
- **`cookAsset`**: one asset through the DDC, the seam that `helios-assetd` (02 §6.4) and `helios-pack` build on.

Depends on `helios::core`, `helios::asset`, `helios::reflect` and zstd (private). EDITOR_ONLY: never linked into
the client, launcher, bot or servers (02 §1.1).

## Headers

| Header | Contents |
|---|---|
| `hpak_writer.h` | `HpakWriter` (`create`, `add`, `size`, `emit`, `build`, `writeFile`), `HpakWriterOptions`, `HpakAssetOrder`, `planHpakLayout`, `HpakLayout`, `resealHpak` |
| `importer.h` | `ImporterInfo` (id, version, extensions, settings type, `fonts`, `build`), `ImporterRegistry` (with `settingsTypeHash`), `BuildContext`, `BuildFn`, `importsFile`, `hashSettingsType` |
| `meta.h` | `AssetMeta`, `Provenance`, `AiProvenance`, `Origin`; `parseMeta`, `writeMeta`, `validateMeta`, `resolveSettings`, `checkLicence`, `allowedLicences`, `checkProjectPath`; on disk `loadMeta`, `saveMeta`, `ensureMeta`, `moveAsset`, `scanMetas` |
| `ddc.h` | `makeDdcKey`, `DdcKeyInputs`, `kCookerVersion`, `hashSourceFile`; the `ddc::` entry format (`encodeEntry`, `readEntryHeader`, `checkEntryPayload`, `readEntry`); `LocalDdc` (`open`, `get`, `put`, `trim`, `stats`, `sizeBytes`) |
| `cook.h` | `cookAsset`, `CookRequest`, `CookResult` |

## Usage

```cpp
using namespace helios;
using namespace helios::assetpipe;

ImporterRegistry importers;                     // filled once at startup, then read-only
importers.add({.id = "png", .version = 3, .extensions = {".png"},
               .settings = &refl::typeOf<TextureImportSettings>(), .build = buildTexture}).value();

NewMeta init;                                   // what a first import records
init.settings = R"({"mips": false})";
init.provenance = {.origin = Origin::Original, .author = "Owner", .licence = "MIT"};
const EnsuredMeta m = ensureMeta(project, "art/ships/hull.png", init, importers).value(); // GUID minted once

auto ddc = LocalDdc::open({.root = ddcRoot}).value();   // %LOCALAPPDATA%\Helios\DDC in the editor (02 §6.2)
const CookResult cooked =
    cookAsset({&importers, ddc.get(), project, "art/ships/hull.png", asset::HpakPlatform::PcClient}).value();
// cooked.hit: served from the DDC; cooked.product: the bytes the pak writer takes.

moveAsset(project, "art/ships/hull.png", "ships/scout/hull.png", importers).value(); // the GUID follows
const MetaScan scan = scanMetas(project, importers).value();  // every problem in the tree, with its file
```

## `.meta` sidecars

Every source a registered importer claims (by extension, ASCII case ignored) has `<name>.meta` next to it.
`meta.h` documents the text; in short:

```jsonc
{
  "$meta": 0,
  "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18",
  "importer": "png",
  "importerVersion": 2,
  "settings": {
    "mips": false,
    "lods": [0, 1]
  },
  "labels": ["hull", "scout"],
  "source": "art/src/scout/hull.blend",
  "provenance": {
    "origin": "original",
    "author": "Owner",
    "licence": "MIT",
    "notes": "Blocked out for the Phase 1 slice."
  }
}
```

- **Canonical JSONC (02 §3.7), through reflect.** The text is written with `refl::JsonWriter` in a fixed key
  order (`$meta`, then the fields above), and values at their default are omitted. The settings are read and
  written through the importer's **reflected settings type** (`ImporterInfo::settings`, 07 §1.10's
  `settingsSchema`), so they come out in schema order with defaults omitted, and an unknown setting is an
  error. `writeMeta(parseMeta(text)) == text` for canonical text, and any spelling of the same sidecar
  (comments, other key orders, explicit defaults, unsorted labels) rewrites to it. `saveMeta` writes only
  when the bytes change. Sidecars are written durably: a flushed temp file with a short hidden name of its
  own (`.<GUID>.tmp`, so it fits next to any source name and a leftover one is skipped by the scan),
  renamed over the sidecar with core's persisted `fs::rename`.
- **Identity.** `ensureMeta` mints the GUID (version 4) on first import and is the only function that
  creates a sidecar; nothing else ever changes the GUID. `saveMeta` only updates: it needs the source and
  its sidecar to exist spelled exactly as given (`NotFound` otherwise, so it cannot give a copied file, a
  missing source or another case spelling of a file an existing GUID), refuses an importer that does not
  claim the source's extension, and refuses a sidecar whose GUID differs from the one on disk or cannot be
  read.
  A sidecar stores no path, so `moveAsset` (move or rename, including a case-only rename through a
  temporary name) moves the file and its sidecar together and the GUID follows the file.
- **Fail closed on load.** `parseMeta` refuses, with the file, the JSON path and the reason: an unknown or
  duplicate key anywhere, a missing required field (`$meta`, `guid`, `importer`, `importerVersion`,
  `provenance` and its `origin`, `author` and `licence`), a GUID that is nil or not in canonical lower-case
  form, an importer that is not registered (`NotFound`), a `$meta` or `importerVersion` newer than this build
  (`VersionMismatch`), settings the importer's type rejects, more than 64 labels or a bad one, a sidecar
  above 1 MiB (`LimitExceeded`), and any provenance rule below.
- **Provenance and licence (01 §5.2).** `origin` is `original`, `commissioned` (needs `rights`), `cc0`
  (needs `url`, and the licence `CC0-1.0`) or `ai-assisted` (needs `ai` with the tool, the model and the
  full prompt, plus its inputs; 09 §4.2): the same kinds as `docs/concept`'s sidecars. The licence is an SPDX
  id on 01 §5.2's list: MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Zlib, BSL-1.0, ISC, PostgreSQL, CC0-1.0,
  plus OFL-1.1 for an importer marked `fonts` (ADR-010). Anything else fails, including SPDX expressions and
  other spellings (a different-case spelling is told the right one).
- **Paths, Windows first.** Sidecar functions take a project root and a `/`-separated relative path, and so
  does the `source` field. `checkProjectPath` refuses text that is not valid UTF-8 (NTFS stores names as
  UTF-16, and on MSVC converting invalid UTF-8 to a `std::filesystem::path` throws, so every sidecar
  function and `cookAsset` check the path before converting it), `\`, absolute paths, empty, `.` and `..`
  components, components above 255 bytes, `<>:"|?*` and control characters, and every component
  engine/core's `fs::isNonPortableComponent` refuses: Windows device names in any case and with any
  extension (`CON`, `nul.png`, `COM1`, `CONOUT$`...) and names ending in `.` or a space. That is the rule
  `DirectoryMount` already applied, made public by this work instead of a third copy being written
  (toolsfw's `.hrec` confinement has the other). A source's file name is at most 250 bytes
  (`kMaxSourceNameBytes`), so its sidecar's name fits in 255; the temporary names of the sidecar writers
  (`.<GUID>.tmp`, and `.<GUID>.moving` in a case-only rename) do not grow with the source's, so every name
  up to that cap can be imported, saved and renamed (tested at the cap). Names that differ only in ASCII
  case are one file on Windows: `scanMetas` reports them, `moveAsset` refuses a target that another file
  holds in any case of any component, or whose directory exists only in another spelling (a move between
  two spellings that already exist is allowed, since it merges them), `ensureMeta` refuses to mint a GUID
  when a sidecar exists in another case (on Windows that sidecar is the file's own and is used) or when one
  of the path's directories does (one directory on Windows, two on Linux), and `saveMeta` takes only the
  exact spelling. Case is folded for ASCII letters only (see Gaps). Every string in a sidecar must be UTF-8,
  so a sidecar that is written always loads.
- **The scan** (`scanMetas`, the input of a registry rebuild, 02 §6.1) returns the valid assets and every
  problem with its file: a source without a sidecar, an orphan sidecar, a sidecar spelled in another case or
  with an upper-case `.META`, an invalid sidecar, an importer that does not import the source's extension,
  names that differ only in case, non-portable names, a duplicate GUID (naming both sidecars; the usual cause
  is a copied file) and an `AssetId` fold collision (02 §6.1). Hidden files and directories are skipped.
- **Why not a schema type.** `schemas/` has no `.meta` type, and the sidecar is not one: its settings object
  is typed by whichever importer the sidecar names, and most of its rules (licences, the provenance rules per
  origin, paths, GUID spelling) are cross-field checks that `.hschema` does not express. The envelope is
  therefore read and written by hand through reflect's JSONC reader and canonical writer, and the settings
  through reflect's type-driven `readJson`/`writeJson`. When the Phase 1 importers declare their settings in
  `.hschema`, the generated types plug in as `ImporterInfo::settings` unchanged.

## DDC key

`makeDdcKey` is 02 §6.2's `XXH3-128(builder, version, sourceHash, settingsHash, platform, layout hashes,
dependency hashes)` plus `kCookerVersion`, over a length-prefixed little-endian preimage (key format 1)
documented in `ddc.h` (and re-derived byte by byte in a test). The inputs:

| Input | From | Changes the key |
|---|---|---|
| Builder id and version | The **registered** importer (not the sidecar's `importerVersion`) | A new importer version |
| Source hash | XXH3-128 of the source bytes | Any byte of the source |
| Settings hash | The canonical settings text (`resolveSettings`: values at their default omitted) | A setting's value |
| Settings type | `hashSettingsType` of the settings type (computed once by `ImporterRegistry::add`) | A field added, removed, renamed, renumbered or retyped, or a default changed, also in a nested type |
| Platform | `pc-client`, `server` or `editor` (02 §6.5: one key per consumer) | The consumer |
| Cooker version | `kCookerVersion` | A cook-pipeline change |
| Dependency keys | The builder, in its order | Any dependency's key (none in v0's cook) |

The settings hash and the settings type together pin the settings a build step resolves: the canonical text
leaves defaults out, and the type's fingerprint records them. `hashSettingsType` walks the type and every type
reachable from it and hashes each type's kind, qualified name, id and version, each struct field's name, id,
flags, type and default value (as a default-constructed object holds it, written as canonical JSON), enum
values, variant alternatives, container element and key types and array sizes; a type reached twice is
recorded once and then referred to, so recursive types terminate, and nesting is bounded at 64 levels.
`TypeInfo::layoutHash`, which key format 0 used, has no default values, and `StructBuilder`'s has no field
types either, so a schema-only default edit or a retyped builder field left every key unchanged and the store
served stale products (review round 1). Importer authors bump `ImporterInfo::version` when the build step's
code changes its products; settings-type changes need no bump.

Not inputs, by design: the path (moving or renaming keeps the key), file times, the spelling of the settings
(key order, comments, explicit defaults), and the asset's identity: GUID, labels, source-DCC path and
provenance. The build step does not see them either: `BuildContext` holds only the source bytes, the
canonical settings and the platform, so two assets with the same bytes and settings share one product, and
a build step cannot make a product that depends on something the key does not cover (a test pins the
context's members). An importer that needs the GUID (sub-asset ids, say) needs it in the key first. Tests
check that each input changes the key and that these do not.

## Local DDC store

`LocalDdc` stores one file per key, `<root>/<first two hex digits>/<32 hex digits>.hddc`: a 64-byte
little-endian header (magic `HDDC`, version 0, header size, the key, the payload size, the payload's
XXH3-128, flags and reserved bytes at 0, and an XXH3-64 of the header) followed by the payload.

- **Atomic put.** A uniquely named temp file next to the entry, renamed over it with core's
  `fs::renameNoSync`. Readers see the old entry, the new one or none. Writers of the same key race
  harmlessly: each renames a complete entry, and a writer whose rename fails still succeeds when a valid
  entry for the key is in place (which covers Windows refusing to replace a file another process has open).
  Nothing is flushed, neither the entry nor the rename: it is a cache, so a put does not wait for the file
  system's journal (`fs::rename` fsyncs the directory on POSIX and writes through on Windows, which cost
  milliseconds per put on the loaded dev VM), and an entry lost or torn by a crash is a verified miss that
  the next put replaces.
- **Verified get.** The header is read first and checked field by field (bounds checked, with `Result`
  errors: `EndOfFile`, `Corrupt`, `VersionMismatch`, `LimitExceeded`). The key in the header must be the key
  asked for, the file must be exactly 64 + payload bytes, and the payload must match its hash. Only then is
  the payload allocated (bounded by the real file size) and returned. A damaged entry is a miss and never
  data; `get` reports why, and `stats().bad` counts it. A name that is not a regular file (a directory, a
  FIFO, a device) is a damaged entry and is never opened, so `get` cannot block on a FIFO.
- **Eviction: LRU under a size cap.** 02 §6.2 and 07 §3.2: LRU, 200 GB by default (`capBytes`). Recency is
  the entry file's last write time: `put` sets it, and a hit refreshes it when it is older than
  `touchInterval` (1 h by default, so a hot entry costs one metadata write an hour). When this process's
  running total passes the cap, `put` runs `trim()`, which lists the store, removes temp files older than
  `staleTempAge` (crashed writers), and deletes least recently used entries until the total is at most
  `trimTargetPercent` (90 %) of the cap. `trim` deletes only files named like entries or their temps inside
  the two-hex-digit directories, so a mistaken root loses nothing else. A trim corrects the running total by
  what it measured and evicted, so puts that finish while it lists the store stay counted (they may count
  twice until the next trim, an overcount that only trims early). Concurrent puts that pass the cap share
  one trim. After a trim that could not get down to its target (entries that could not be deleted), `put`
  trims again only once the total passes what that trim left plus the trim headroom (the cap minus the
  target, at least 1 % of the cap), and after a trim that could not list the store, once another 1 % of
  the cap has been put; so such a store is not listed again on every put. `stats().trims` counts the
  listings. Several processes may share a root; each re-measures at its own trims, so the store can pass
  the cap by what the others wrote since.
- **Threading.** Every `LocalDdc` member is thread-safe; trims in one process are serialized. A reader whose
  entry is evicted under it finishes reading (the file stays readable while open) or misses.
- **Integrity, not authenticity.** XXH3 detects corruption, not tampering. The local store is the user's own
  cache; the shared tier distributes by BLAKE2b (02 §6.2, 05 §7).

## Cooking

`cookAsset` loads and validates the sidecar, reads the source once (the key and the build see the same
bytes), forms the key and returns the DDC's product on a hit. On a miss (or a damaged entry, logged) it runs
the importer's `build` step with the keyed inputs only and stores the product; a failed put is logged and the
product is still returned. The cook tests run a small asset twice (the second is a hit with byte-identical
output, and the importer does not run), and show that the importer version, a setting, the source bytes and
the platform each miss, while a move, a touch, labels and provenance do not; that two assets with the same
bytes and settings share one product; and that a changed settings default or a retyped settings field
misses on a warm DDC and gives what a clean cook gives.

## Performance

The plan's budgets are per build step and end to end: 02 §6.4's hot-reload build steps (records ≤ 300 ms,
Luau ≤ 100 ms, textures ≤ 800 ms, shaders ≤ 1,000 ms, containers ≤ 500 ms) and 07 §4.1's cold open with a warm
DDC (≤ 10 s, AAA-ITR-2). It sets none for the cache itself, so v0 states its own: a hit costs at most 1 % of
the smallest build-step budget (Luau's 100 ms). They are the `perf:` cases in `tests/test_perf.cpp` (label
`perf`, nightly; asserted in optimized builds without sanitizers). Measured on the shared, loaded 4-vCPU dev
VM, GCC 13, RelWithDebInfo, 2026-10-04, three runs after review round 1 (load average about 3):

| Path | Budget | Measured |
|---|---|---|
| `makeDdcKey` (settings text and 4 dependency keys) | ≤ 1 µs mean | 42–63 ns |
| `hashSourceFile`, 64 MiB, page cached | ≥ 1 GB/s | 2.5–2.7 GB/s |
| `LocalDdc::get`, 256 KiB entry (a hit) | ≤ 1 ms p95 | 70–84 µs p95 (36–49 µs p50) |
| `LocalDdc::put`, 256 KiB entry | ≤ 5 ms p95 | 0.22–0.31 ms p95 (0.15–0.21 ms p50) |
| `cookAsset` hit, 64 KiB source (sidecar load and validation, source read and hash, key, get) | ≤ 1 ms p95 | 49–57 µs p95 (30–34 µs p50) |
| `ensureMeta`, `saveMeta`, `moveAsset` in a directory of 2,000 entries (case-check listings, durable write) | ≤ 50 ms p95 each | 15.7–22.3, 18.1–23.9 and 12.0–19.9 ms p95 |

The sidecar writers' row was measured after review round 2 (2026-10-05, three runs, load average about 5.7).
Their budget is half the smallest build-step budget, since a first import or a settings edit comes before a
build step. Each call lists every directory on its path once for the case checks (a listing of 2,000 entries
took about 4.4 ms here, a stat per entry) and writes durably (a flushed temp file and a persisted rename: 1–12
ms on this VM's disk). So a first import of n files into one directory lists O(n²) entries in all: 1,000 files
took 9.8–11.3 s here, mostly in flushes. A batch import that lists each directory once is `helios-assetd`'s
(Gaps).

Puts no longer fsync the fan-out directory (`fs::renameNoSync`; strace counts no `fsync` in 64 puts). With the
fsync, the same case measured 1.0–2.4 ms p95 here at a load average of about 3, and the reviewer saw 4.0–10.7 ms
p95 at a load average of about 10, one run above the gate. `hashSettingsType` runs once per importer, in
`ImporterRegistry::add`, so a cook pays nothing for it.

## Fuzzing

`fuzz/fuzz_ddc_entry.cpp` is a libFuzzer target for the entry reader (02 §8.3 fuzzes the cooked readers, and a
DDC entry is read back from a directory any local process can write). Each input runs as given and with its
header checksum and payload hash recomputed, so mutations reach the size, key and reserved-byte checks. It
checks that `readEntry` returns a `Result` with a payload view inside the input; that a successful read means
the input is exactly `encodeEntry(key, payload)` (the format has no slack) and fails under any other key; and
that `LocalDdc::get` on a file holding the same bytes agrees (a hit exactly when `readEntry` succeeds, with the
same payload). A header claiming up to 2 GiB is refused before anything is allocated. `fuzz/corpus/ddc_entry/`
holds the seeds (`assetpipe_fuzz_ddc_entry --make-seeds <dir>` rewrites them). It is built behind
engine/asset's switch, `HELIOS_ASSET_LIBFUZZER`; without it the target links engine/asset's standalone driver
and is a CTest (label `fuzz`, every PR) that replays the corpus plus 20,000 deterministic mutations. A campaign:

```
cmake --preset linux-clang -B build/fuzz -DHELIOS_BUILD_GRAPHICS=OFF -DHELIOS_BUILD_TESTS=OFF \
  -DHELIOS_ASSET_LIBFUZZER=ON "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link,address,undefined" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined"
cmake --build build/fuzz --target assetpipe_fuzz_ddc_entry
build/fuzz/bin/assetpipe_fuzz_ddc_entry -max_total_time=600 -rss_limit_mb=2048 -malloc_limit_mb=64 corpus-copy/
```

An 11-minute local campaign (Clang 18, ASan and UBSan, one process, `-malloc_limit_mb=64`, 2026-10-04) ran 2.29
million inputs at about 3,500 per second (each one also writes, reads and removes a store file), reached 951
coverage edges with 85 corpus units and found nothing. With the reader's trailing-bytes check removed, the
CTest replay (the corpus plus its mutations) aborts on the re-encoding property, so the harness catches a lax reader.

## Gaps

- **Shared DDC** (`helios-ddc`, the HTTP CAS filled by CI; 02 §6.2, 07 §3.2) is WP-2.1, and with it
  distribution by BLAKE2b (05 §7).
- **`helios-assetd`** (02 §6.4, 07 §3.1: watching, the job DAG, priorities, the IPC transport, hot-reload
  notifications) is a later WP. `cookAsset` has no dependency tracking: v0's builds have no dependencies, and
  `DdcKeyInputs::dependencies` is where the job DAG will put them.
- **Registry.** `.helios/registry.db` (07 §3.4) and the cooked `registry.hreg` (02 §6.1, AAA-ITR-2) are not
  built; `scanMetas` is the rebuild's input.
- **Importers.** The Phase 1 importers (glTF, FBX, PNG, EXR, audio; 02 §6.2, the T24 MVP) and 07 §1.10's
  `IImporter` interface replace v0's `ImporterInfo` and its `build` function. Settings migration between
  importer versions (an old sidecar's settings that the new type rejects) is theirs.
- **Default DDC root.** `LocalDdc` takes its root from the caller. Choosing `%LOCALAPPDATA%\Helios\DDC`
  (`$XDG_CACHE_HOME/helios/ddc` on Linux) is the editor's and `helios-assetd`'s job, since core has no public
  environment lookup.
- **Provenance for captures.** 09 §4.3.3 asks a mocap clip's `.meta` to record the performer, session and
  consent; v0 has `notes` for them. The fields come with WP-3.13.
- **Case folding beyond ASCII.** The case checks fold ASCII letters only. NTFS also folds other letters
  (`Ä.png` and `ä.png` are one file there), so on Linux `scanMetas`, `ensureMeta` and `moveAsset` accept two
  sidecars, with two GUIDs, for what is one file on Windows. Folding like NTFS needs its upcase table (simple
  Unicode case mapping of UTF-16 units), which core does not have yet.
- **Batch import.** Each sidecar writer lists the directories on its path (see Performance), so importing n
  files into one directory one call at a time lists O(n²) entries. `helios-assetd`'s import queue should
  list each directory once per batch.
- **Single writer.** Sidecar creation assumes one writer per project (02 §6.1: `helios-assetd`). Two
  processes creating the same sidecar at once can each mint a GUID, and the last rename wins (core has no
  exclusive create).
- **`StructBuilder` layout hashes** (engine/reflect, not this module): `StructBuilder` hashes field names
  and ids but not field types, although `TypeInfo::layoutHash`'s documentation says types are included. The
  DDC key no longer depends on it (`hashSettingsType`); other users of `layoutHash` with builder-reflected
  types are engine/reflect's to check.
- **Nightly fuzzing** of this target, toward 02 §8.3's ≥ 24 CPU-hours per release, is not scheduled: the
  same gap as engine/asset's target (the nightly job runs only engine/net's).

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (01 §5.2; 02 §1.1, §3.7, §6.1–§6.5, §8.3; 05 §7; 07 §1.10, §2.5 T24, §3.1, §3.2,
§3.4, §4.1, §4.1.1; 09 §2 WP-0.8) on 2026-10-04. Choices the plan leaves open are recorded above: the sidecar's
envelope and provenance fields (the origins of `docs/concept`), SPDX ids for 01 §5.2's licence names (BSD as
BSD-2-Clause or BSD-3-Clause, CC0 as CC0-1.0), the key's preimage layout and `kCookerVersion`, the settings
type's fingerprint as the key's "layout hashes" for settings (it adds default values and field types, which
`layoutHash` lacks), a build context limited to keyed inputs (as 07 §1.10's `IBuilder` has its declared inputs
feed the key), the entry format, LRU by file time with a touch interval, the 90 % trim target, the store's
fan-out, not flushing entries or their renames, the 250-byte cap on source file names with short hidden
temporary names for the sidecar writers, `saveMeta` as an update only, and the sidecar writers' 50 ms budget.
The `.hpak` writer was written for revision 12; nothing it implements changed in revision 13.
