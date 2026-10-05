# engine/patch — FastCDC chunking and the `.hman` manifest

`helios::patch` (L2, HEADLESS; namespace `helios::patch`, headers `helios/patch/*.h`) is the engine side of the
patch pipeline (05 §7, 08 §2.5–2.6). v0 (WP-0.16 part 1 of 2) holds:

- **BLAKE2b-256** (`Hash256`, `blake2b256`, `Blake2b256`): chunk IDs, file hashes and manifest hashes, over
  Monocypher 4.0.3's portable C (05 §7);
- **FastCDC** (`fastcdc::cut`, `splitBuffer`, `StreamChunker`, `chunkFile`) with 05 §7's sizes;
- the **`.hman` v0 manifest**: the format, its validator, a canonical writer, a hostile-input reader and a
  `ManifestBuilder` that turns chunked files into a canonical manifest.

Go has the same two pieces: [`services/pkg/cdc`](../../services/pkg/cdc) and
[`services/pkg/manifest`](../../services/pkg/manifest). The vectors in `services/testdata/vectors/` are shared:
`go test` and `patch_tests` check the same files, so a divergence between the languages fails both suites.
This README is the specification for both.

Part 2 (not here): the Ed25519 trust chain, keysets, pointers, the local CDN and `helios-patch publish`, with
CL-14's tampered, expired and rolled-back subset. The installer, `install.db`, the planner, the HTTP clients and
the `StreamingInstaller` of 08 §4.1 are later WPs (0.17, 2.7).

Depends on `helios::core`, Monocypher and zstd (both private). The launcher links this module in its x86-64-v1
(`base`) image (08 §2.1.1), so it has no ISA-specific code; file IO goes through core's platform layer. Base images
link `helios_patch.base` (02 §1.1's `patch@base`), an object-library copy built at x86-64-v1 together with the
copies of core, Monocypher and zstd (`cmake/HeliosIsa.cmake`); it exists once a base image is configured, which
today is the ISA audit's `lint_isa_fixture_base` and from WP-0.17 the launcher. Every other image links
`helios_patch`, built at `avx2`.

## Headers

| Header | Contents |
|---|---|
| `blake2b.h` | `Hash256` (`toHex`, `fromHex`, `isZero`, byte-wise ordering), `blake2b256`, `Blake2b256` (streaming) |
| `fastcdc.h` | `fastcdc::{kMinSize, kAvgSize, kMaxSize, kMaskS, kMaskL, kGearSeed, gearTable, cut}`, `Chunk`, `ChunkedFile`, `chunkBoundaries`, `splitBuffer`, `StreamChunker`, `chunkFile` |
| `manifest.h` | `hman::` format constants, `hman::RawHeader` and its codec; `Manifest` and its entries, `validateManifest`, `isValidManifestPath`, `encodeManifestBody`, `writeManifest`, `readManifestHeader`, `readManifest`, `readManifestFile`, `ManifestBuilder` |

## Usage

```cpp
using namespace helios::patch;

ManifestHeader h{.productId = "sample-game", .platform = "win64", .buildId = "2026.10.04-r1",
                 .sequence = 42, .createdAt = now, .compatEpoch = 3};
ManifestBuilder builder(h);
for (const auto& [path, tier] : buildFiles) {
    ManifestFileInput in{.path = path, .content = chunkFile(root / path).value(), .tier = tier};
    builder.addFile(std::move(in)).value();
}
builder.addPack(packHash, packSize);                           // optional: small chunks in packs (05 §7)
builder.placeChunk(chunkId, packHash, offsetInPack, storedSize);
Manifest m = builder.build().value();                          // sorted, deduplicated, validated
std::vector<u8> file = writeManifest(m, {.codec = ManifestCodec::Zstd}).value(); // zstd level 19

Result<Manifest> back = readManifest(file);                    // Corrupt / VersionMismatch / Unsupported /
                                                               // LimitExceeded on hostile input
```

## FastCDC

05 §7 fixes the sizes (min 16 KiB, average 64 KiB, max 256 KiB) and leaves the rest open. Helios's definition,
in both languages:

- **Gear table:** the first 256 outputs of SplitMix64 (engine/core's `SplitMix64`) seeded with
  `kGearSeed = 0x0046415354434443` ("FASTCDC" in ASCII). The table is generated, not copied from any
  implementation; `fastcdc.json` lists it and its BLAKE2b-256.
- **Rolling hash:** `fp = (fp << 1) + gear[byte]` modulo 2^64, starting from 0 at the cut-point skip.
- **Normalized chunking, level 2:** no cut before `kMinSize` bytes; from byte `kMinSize` to `kAvgSize` a cut
  follows the first byte with `(fp & kMaskS) == 0`, and from `kAvgSize` on the first with `(fp & kMaskL) == 0`.
  `kMaskS` is the top 18 bits and `kMaskL` the top 14 (log2 of 64 KiB, plus and minus 2). The top bits of a
  shift-left gear hash depend on the most bytes (up to the last 64), so they make the widest window.
- **Maximum:** a chunk ends at `kMaxSize` bytes if no cut came first. Only an input's last chunk may be
  `kMinSize` bytes or shorter.
- **Streaming:** a boundary depends only on the bytes from the chunk's start to at most `kMaxSize` past it, so
  `StreamChunker` (and Go's `Chunker`) emit a chunk once `kMaxSize` bytes past its start are buffered, and give
  exactly the chunks of the whole buffer.

"Average 64 KiB" is FastCDC's normal size, the point where the mask relaxes. With the min-size skip, the mean
chunk on random data is about 74 KiB (75.8 KB measured over 64 MiB in both languages; the tests require 48–96
KiB). An insertion changes only the 1–3 chunks around it (tested at 100 bytes in 16 MiB).

## The `.hman` v0 format

05 §7 lists what the manifest holds (build, monotonic `sequence`, platform; per file: path, size, hash, chunk
list, tags and install tier; a pack index and patches) and says "a schema-generated binary, zstd-compressed".
v0 is a hand-written fixed-layout binary instead (see [Plan conformance](#plan-conformance)). All integers are
little-endian.

```
[0, 352)     header (fixed)
[352, end)   payload: the body as is (codec 0) or zstd frames that decode to it (codec 1)
```

**Header** (byte offsets):

| Offset | Field | Rule |
|---|---|---|
| 0 | `magic` u32 | `'HMAN'` (0x4E414D48) |
| 4 | `version` u16 | 0 (else `VersionMismatch`) |
| 6 | `headerSize` u16 | 352 |
| 8 | `flags` u32 | 0: v0 defines none (else `Unsupported`) |
| 12 | `codec` u8, 3 reserved bytes | 0 = none, 1 = zstd (else `Unsupported`) |
| 16 | `sequence` u64 | monotonic per product, channel and platform (anti-rollback, 05 §7; checked by part 2) |
| 24 | `createdAt` u64 | unix seconds |
| 32 | `expiresAt` u64 | unix seconds; 0 = never, else after `createdAt` (checked against the clock by part 2) |
| 40 | `compatEpoch` u32, 4 reserved bytes | the build's content compat epoch (05 §1.14.1) |
| 48 | `bodySize` u64 | the decoded body: 32 B .. 256 MiB (`ManifestReadOptions::maxBodySize` can lower it) |
| 56 | `payloadSize` u64 | the bytes after the header; equals `bodySize` for codec 0, at most `bodySize + bodySize/128 + 4096` for codec 1 |
| 64 | `bodyHash` [32] | BLAKE2b-256 of the decoded body |
| 96 | `productId` [32] | `^[a-z][a-z0-9-]{2,31}$` (08 §2.10.1), zero padded |
| 128 | `platform` [32] | `^[a-z][a-z0-9_-]{1,31}$` (`win64`, `linux64`), zero padded |
| 160 | `buildId` [64] | `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$` (the CDN path segment, 05 §7), zero padded |
| 224 | `keyId` [16] | the keyset subkey that signed (part 2); zeros in v0, not interpreted |
| 240 | 16 reserved bytes | zero |
| 256 | `headerHash` [32] | BLAKE2b-256 of bytes [0, 256): the manifest's identity |
| 288 | `signature` [64] | part 2: Ed25519 over bytes [0, 256); zeros in v0, not interpreted |

The signed region [0, 256) commits to the body through `bodyHash`, so part 2 signs without changing the layout,
and a verifier checks the signature before it decompresses anything. Identifier fields hold the string, then
zeros: a non-zero byte after the first zero is `Corrupt`.

**Body:** a 32-byte header `{fileCount, chunkCount, refCount, packCount, patchCount, stringBytes}` (u32 each,
then 8 reserved bytes), then the tables back to back, then the paths:

| Table | Entry (bytes) | Fields |
|---|---|---|
| files | 80 | `pathOffset` u32, `pathLength` u32, `size` u64, `hash` [32], `firstRef` u32, `refCount` u32, `group` u64, `language` u32, `tier` u8, reserved u8, `flags` u16, reserved u64 |
| refs | 16 | `chunk` u32, reserved u32, `offset` u64 |
| chunks | 56 | `hash` [32], `rawSize` u32, `storedSize` u32, `pack` u32, reserved u32, `packOffset` u64 |
| packs | 40 | `hash` [32], `size` u64 |
| patches | 80 | `file` u32, reserved u32, `fromHash` [32], `patchHash` [32], `patchSize` u64 |
| paths | `stringBytes` | every file's path, in file order, with no separators |

**Validation** (the reader's and the writer's; `validateManifest` / `Manifest.Validate`):

- **Sizes:** the counts give exactly `bodySize`; limits: 2^20 files, 2^24 chunks, 2^24 refs, 2^20 packs, 2^20
  patches and 64 MiB of paths (`LimitExceeded` beyond one). A path of 0 or more than 1024 bytes (or with a
  segment over 255) is not a limit but an invalid path: `Corrupt` from the readers, `InvalidArgument` (Go: `ErrInvalid`) from
  `validateManifest` and the writers.
- **Files:** sorted by path (byte order) and unique, also ignoring ASCII case; no file is also a directory of
  another file, also ignoring case (`a` and `A/b`); `pathOffset` is the running sum of path lengths; `tier` ≤ 2;
  `flags` ⊆ {Optional 1, Vaulted 2, Executable 4}; `firstRef` is the running sum of `refCount`s. The two
  collision rules are checked together: each path's key is the path with ASCII letters lowered and `/` mapped
  to 0x00, so byte order on keys puts a directory's contents directly after it; the keys are sorted (only if
  they are not already) and neighbours compared. That costs O(P + n log n · ℓ) byte operations for P path
  bytes, n files and common prefixes ℓ ≤ 1024, and hashes no attacker-chosen string. A build's keys mostly
  arrive sorted (lowered paths sort like the paths unless case or `+`, `-`, `.` reorder them), and then the
  check is one comparison per file; keys an attacker puts out of order cost the sort, about 50–150 ms more
  for 65,536 keys of 1 KiB (see [Performance](#performance)). (Looking up every `/`-prefix of every path in a
  set, as the first version did, costs about len²/4 per path: 2.4 s (Go) and 5 s (C++) for
  `deep-paths.hman`.)
- **Refs:** a file's refs tile it: each `chunk` exists, each `offset` is the sum of the previous chunks' raw
  sizes, and the total is the file's `size` (so `refCount` is 0 exactly when `size` is 0).
- **Chunks:** sorted by ID and unique; `rawSize` 1..256 KiB; `storedSize` ≤ 256 KiB + 4 KiB (0 = not recorded);
  every chunk is referenced; a loose chunk has `pack` = 0xFFFFFFFF and `packOffset` 0; a packed one has
  `storedSize` ≥ 1 and fits inside its pack.
- **Packs:** sorted by ID and unique; `size` 1 B..1 GiB (a zero-size pack already fails its chunks' fit check);
  each holds at least one chunk.
- **Patches:** sorted by (`file`, `fromHash`) and unique; `file` exists; `fromHash` differs from the file's hash;
  `patchSize` 1..2^40. The CDN object is `/patches/<fromHash>_<file hash>.zpatch` (05 §7).
- **Reserved bytes** are zero everywhere.

So a manifest has exactly one body: the Go and C++ writers emit identical bytes for codec 0, and identical
bodies (different zstd frames) for codec 1.

**Paths** are relative, `/`-separated ASCII of at most 1024 bytes: each segment is 1–255 bytes of
`[A-Za-z0-9._+-]` (ext4's `NAME_MAX` is 255 bytes and NTFS allows 255 UTF-16 units per name), not `.` or `..`,
does not end in `.` (Windows drops it), and is not a Windows device name (`CON`, `PRN`, `AUX`, `NUL`,
`COM0`–`COM9`, `LPT0`–`LPT9`, with any extension, any case). With the case-insensitive uniqueness rules, a
manifest's relative tree can be created on NTFS and ext4 alike (the install root plus a 1024-byte path can still
exceed Windows' 260-character `MAX_PATH`; the installer needs long-path support for such trees).

**zstd payloads** may be one or more frames (skippable frames are skipped) whose output is exactly `bodySize`
bytes, with a window of at most 32 MiB. The writers use zstd levels 1–19, whose windows fit. Write options default alike in both
languages (C++'s default `ManifestWriteOptions`, Go's zero `WriteOptions`): codec 0, and a zstd level of 0
means 19.

## Shared vectors

| File | Contents | Written by |
|---|---|---|
| `services/testdata/vectors/fastcdc.json` | Parameters, the gear table and its hash, and 18 inputs (empty, 1 byte, below/at/past the minimum, at and past the maximum, all-zero, random 1–8 MiB, a periodic input, an insertion, and three crafted ones: the earliest possible `kMaskS` match, at byte 16386, and `kMaskL`-only matches at bytes 65535, where `kMaskS` still applies, and 65536, the first byte `kMaskL` covers) with every chunk's offset, size and ID | `go test ./pkg/cdc -run TestUpdateVectors -update` |
| `hman/pipeline.json` | A build description: 8 files given by generator (shared chunks, an empty file, all-zero data, every tag), a pack with two placed chunks, a stored size, two patches, a key ID and signature | by hand |
| `hman/pipeline.hman` | That description written with codec 0: both writers must produce it byte for byte, from files added in any order | `go test ./pkg/manifest -run TestUpdateGoldens -update` |
| `hman/pipeline.go-zstd.hman`, `pipeline.cpp-zstd.hman` | The same manifest with a zstd-19 payload from each language's encoder; both readers must read both | Go: as above; C++: `HELIOS_PATCH_UPDATE_VECTORS=1 patch_tests` |
| `hman/hostile.json` | 56 edits of `pipeline.hman` (header, every table, reserved bytes, sizes, truncation, trailing bytes, a ref after the last file's, a path byte of no file, a pack that holds no chunk) and one of `pipeline.go-zstd.hman` (a trailing skippable frame one byte past the payload bound), resealed or not, each with the error kind both readers must return and the one check it breaks (`rule`, a substring of the error message both languages share); an edit overwrites or (`insert`) inserts bytes; where one edit would break two checks, the case edits the dependent field too (a chunk's raw size with its file's size, a table entry with its count) | `go test ./pkg/manifest -run TestUpdateGoldens -update` |
| `hman/deep-paths.hman` | The deepest paths the limits allow: 65,536 empty files whose 1024-byte paths sit 508 directories deep (64 MiB of paths, a 72 MB body, 187 KB with zstd-19). Both readers read it; the perf tests hold it to the read budget | `go test ./pkg/manifest -run TestUpdateGoldens -update` |
| `hman/names.json` | Valid and invalid paths (among them 255- and 256-byte segments), product IDs, platforms and build IDs, and whole path lists that must pass or fail the collision rules (the invalid ones with the check they fail) | by hand |

Inputs come from a seeded generator (`services/pkg/cdc/cdctest`, mirrored in `tests/patch_test_util.h`): `random`
(SplitMix64 outputs as little-endian bytes), `zero`, `repeat` (a random unit repeated) and `insert`, each
optionally followed by `edits` (bytes written at offsets: the crafted inputs).

## Behaviour worth knowing

- **Hostile input.** `readManifest` checks, in order: the header's size, magic, version, header size and header
  hash; flags, codec and reserved bytes; the identifiers; `bodySize` against the cap and `payloadSize` against the
  file; then it decodes the payload, checks `bodyHash`, decodes the tables (counts against the limits and the
  body size, path offsets, reserved bytes) and validates the manifest. Every failure is a `Result` error
  (`Corrupt`, `VersionMismatch`, `Unsupported` or `LimitExceeded`), never UB. Go returns errors wrapping
  `ErrCorrupt`, `ErrVersion`, `ErrUnsupported` and `ErrLimit` and never panics. `hostile.json` pins the kind
  and the failing check for 57 cases in both languages.
- **CPU.** Every check is linear in the body's bytes except the path-collision sort (above), which costs at
  most n log n key comparisons of ≤ 1 KiB each. `deep-paths.hman` (the case the first version's prefix check
  took 5 s on) reads within the read budget, and the same paths with their keys in random order within their
  own; see [Performance](#performance). A non-perf case in `patch_tests` (every PR) also holds validating paths
  508 levels deep to 4 times the cost of paths 4 levels deep, so a quadratic check fails PR CI in both
  languages, not only the nightly.
- **Memory.** A zstd payload decodes into a buffer that grows only as bytes decode (from 1 MiB, doubling) up to
  `bodySize + 1`, so a small file that claims a 256 MiB body costs what it really decodes to. C++ grows it with
  `realloc`, which moves large blocks' pages instead of copying them (doubling a `std::vector` to 72 MB cost
  140 ms of copies and page faults). The tables cost
  about the body's size again. `readManifestFile` refuses a file larger than the cap allows before reading it.
- **Header first.** `readManifestHeader` validates only the header: enough to refuse another product's or
  platform's manifest, or (part 2) a stale sequence, before decompressing anything.
- **Canonical form.** `encodeManifestBody` of a read manifest reproduces the body that was read (the fuzz targets
  check this). `ManifestBuilder::build` sorts and deduplicates; a chunk ID seen with two sizes, a duplicate path,
  a placement or patch naming an unknown chunk, pack or file, or a chunk placed twice fails with
  `InvalidArgument`.
- **Threading.** Everything is a value type or a pure function; a `StreamChunker`, `Blake2b256` or
  `ManifestBuilder` belongs to one thread (Go: a `Chunker` or `Builder` to one goroutine).

## Performance

05 §7 and 08 §2.5 give no chunking budget; R07 §7 cites FastCDC at "> 1 GB/s/core". v0's budgets, per core, the
same for both languages (C++: `perf:` cases in `tests/test_perf.cpp`, label `perf`, nightly, asserted in
optimized builds without sanitizers; Go: `TestPerfChunking` in pkg/cdc and `TestPerfManifest` and
`TestPerfDeepPaths` in pkg/manifest assert them with `HELIOS_PERF=1` and four times the time (a quarter of the
rate) otherwise, since `go test ./...` runs packages in parallel; `BenchmarkBoundaries`, `BenchmarkSplit`,
`BenchmarkChunkReader`, `BenchmarkParse`, `BenchmarkMarshal` and `BenchmarkParseDeepPaths` measure them).
Measured on the shared, loaded 4-vCPU dev VM (load average 3–5), GCC 13 RelWithDebInfo and Go 1.27.1,
2026-10-04, best of 3–5 runs per session, three sessions:

| Path | Budget | C++ | Go |
|---|---|---|---|
| FastCDC boundary detection | ≥ 1,000 MB/s | 1,583–2,358 MB/s | 1,708–1,793 MB/s |
| Chunking with a BLAKE2b-256 ID per chunk and one for the whole input | ≥ 250 MB/s | 318–436 MB/s (`splitBuffer`), 307–353 MB/s (`StreamChunker`) | 333–343 MB/s (`ChunkReader`) |
| Read a 50 GB install's manifest (20k files, 700k chunks, 52.6 MB body): BLAKE2b, decode, validate | ≤ 400 ms | 114–122 ms | 103–125 ms |
| Write it (codec 0) | ≤ 400 ms | 153–171 ms | 130–154 ms |
| Read `deep-paths.hman` (64 MiB of paths 508 directories deep, a 72 MB body, zstd), whose collision keys arrive sorted | ≤ 400 ms | 315–325 ms | 300–354 ms |
| Read the same paths with their collision keys in random order (the first 16 directories named `a` or `A` by a random permutation, so the check sorts 65,536 keys of 1 KiB); C++ also asserts at most twice the sorted-key read | ≤ 800 ms | 384–475 ms ¹ | 432–572 ms ¹ |

¹ Measured 2026-10-05 with GCC 13 (Clang 18: 399–467 ms) and Go, load average 5–9 (Go's 572 ms ran beside
pkg/cdc's timing test), when the sorted-key reads took 299–395 ms (C++) and 302–394 ms (Go). Out of order the
sort costs C++ (libstdc++ `std::sort`) about 50–150 ms here; the round-2 review measured 145 ms
for the sort alone, and a 463 ms read, with only the first directory's case alternating (294 ms in order). At
the file-count limit reads are slower still, and no budget is stated for them: 2^20 files of 64-byte paths took
C++ 661 ms in order and 848 ms with three case bits shuffled here; the review measured C++ 638 ms in order and
1,226 ms out of order, and Go 699 and 764 ms (Go's pdqsort copes better with out-of-order keys).

The deep-paths read is also held to twice the read of as many bytes of paths four directories deep (255-byte
names, the same body size): 0.9–1.3 times here, against 4.6 times (Go, 2.4 s) and 13 times (C++, 5.1 s) for
the first version's prefix check, so the ratio fails a quadratic check on any machine while the budget needs a
quiet one.
In C++ that read is mostly hashing (about 85 ms), zstd (60 ms) and validation (140 ms, most of it the paths)
of a body 1.4 times the large manifest's.

Hashing dominates the chunking rows: each byte is hashed twice (its chunk's ID and the file hash). C++ uses
Monocypher's portable BLAKE2b; Go's `x/crypto/blake2b` uses AVX2 but pays for the `io.Reader` copy. At 250
MB/s one core chunks a 50 GB build in 3.5 minutes; CL-10's full verify (50 GB in 3 minutes, about 280 MB/s)
needs two cores at this rate, or the SSE4.1/AVX2 BLAKE2b that 08 §2.1.1 lists for the launcher's self-dispatch.

## Fuzzing

- **C++:** `fuzz/fuzz_manifest_reader.cpp` is a libFuzzer target. Each input runs as given and resealed (sizes,
  `bodyHash` and `headerHash` recomputed; a zstd payload is decoded to learn its body), so mutations reach the
  field checks. It checks that the reader returns Results, that a manifest that reads is valid, re-encodes to the
  body it was read from and round-trips through the writer, and that its lookups agree with its tables. The body
  cap is 16 MiB, so `-malloc_limit_mb` catches a decoder that allocates ahead of its output. `fuzz/corpus/
  manifest_reader/` holds the seeds (`patch_fuzz_manifest_reader --make-seeds <dir>` rewrites the generated
  `seed_000`–`seed_005`; `seed_005` is a 64-file `deep-paths.hman`; `seed_pipeline*` and `seed_deep_paths.bin`
  are copies of the shared vectors). `deep-paths.hman`'s 72 MB body is above the cap, so it stops at the
  header check, and the reseal step skips a zstd frame that declares more than the cap instead of decoding it.
  Without `HELIOS_PATCH_LIBFUZZER` the target is a CTest (label `fuzz`, every PR) that replays the corpus plus
  20,000 deterministic mutations.
- **Go:** `FuzzParse` (pkg/manifest; its seeds, which every `go test` runs, are the shared goldens with
  `deep-paths.hman`, a 64-file deep-paths manifest and the 57 hostile cases) checks the same properties, and
  `FuzzChunker` (pkg/cdc) checks that the streaming chunker agrees with `Split` and the size bounds for any input
  and read pattern.

A campaign:

```
cmake --preset linux-clang -B build/fuzz -DHELIOS_BUILD_GRAPHICS=OFF -DHELIOS_BUILD_TESTS=OFF \
  -DHELIOS_PATCH_LIBFUZZER=ON "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link,address,undefined" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined"
cmake --build build/fuzz --target patch_fuzz_manifest_reader
build/fuzz/bin/patch_fuzz_manifest_reader -max_total_time=600 -rss_limit_mb=2048 -malloc_limit_mb=512 corpus-copy/
cd services && go test ./pkg/manifest -run '^$' -fuzz FuzzParse -fuzztime 5m
```

Local campaigns (Clang 18.1.3, ASan and UBSan with `-fno-sanitize-recover=undefined`, one process,
`-malloc_limit_mb=512`), 10 minutes each, found nothing. The first (2026-10-04, from 7 seeds) ran 5,434,506 inputs
at about 8,750 per second and reached 2,088 coverage edges (6,814 features; corpus 7 → 1,234 units; peak RSS 236
MB). After review round 1 (the collision sort, the realloc'd zstd buffer; 2026-10-05, from the 9 committed seeds)
one ran 6,566,017 inputs at about 10,900 per second and reached 2,104 edges (6,997 features; corpus 9 → 1,375
units; peak RSS 261 MB); `patch_tests` (all 26 cases) passed twice in the same sanitizer build. Go: `FuzzParse`
ran 977,340 and 848,423 inputs in two first runs (5.5 and 5.3 minutes) and 1,189,554 in 5 minutes after round 1
(two workers, 21 new interesting inputs), and `FuzzChunker` 22,345 inputs in 90 seconds, with no failure; on the
loaded VM the Go fuzzer ran in bursts of about 20,000 inputs per second between pauses.

## Gaps (v0)

- **Part 2 of WP-0.16:** the Ed25519 signature (`keyId`, `signature`), keysets and pointers, checking
  `sequence` and `expiresAt`, the local CDN layout, `helios-patch publish` and CL-14's tampered, expired and
  rolled-back subset.
- **Packing policy.** The format holds the pack index (05 §7: chunks under 32 KiB in ~8 MiB packs), and the
  builder places chunks it is told about; choosing packs is publish's job (part 2). Validation checks that each
  packed chunk fits inside its pack, not that two chunks' stored bytes in one pack do not overlap.
- **SIMD BLAKE2b.** The self-dispatching SSE4.1/AVX2 compression functions of 08 §2.1.1 are not written; the
  C++ hashing rate is the portable one above.
- **Nightly fuzzing.** `nightly.yml`'s libFuzzer job runs only engine/net's targets, as for engine/asset's;
  scheduling this target (and `go test -fuzz`) toward CL-14's 24 CPU-hours needs a nightly matrix entry and a
  scorecard gate. The Go timing test with `HELIOS_PERF=1` is not in the nightly perf step either.

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (05 §7, §8; 08 §2.1.1, §2.5, §2.6, §2.10.1, §4.1, §4.5; 02 §1.1, §6.3; 09 §2
WP-0.16) on 2026-10-04. Deviations and choices the plan leaves open:

- **Not schema-generated (05 §7).** The plan calls `.hman` "a schema-generated binary". schemac's binary codecs
  are the tagged format (protobuf wire, which tolerates unknown and missing fields, 02 §3.4) and need
  `engine/reflect`, which 02 §1.1's row for `patch` (deps: core) does not allow; the Go emitter has no cooked
  codec. A signed manifest needs one canonical encoding that both languages write byte for byte, so v0 is a
  hand-written fixed layout with a shared golden. The Director decides whether a later schemac emitter (a
  standalone, canonical codec) replaces it.
- **zstd envelope.** The canonical body is what both writers agree on; the zstd payload differs by encoder.
  `bodyHash` covers the decoded body and `headerHash` the header, so the manifest's identity (the pointer's
  `manifest_hash` in part 2) is the header hash, and the signature covers the header.
- **FastCDC details:** the generated gear table, the top-bit masks, normalization level 2, and "average" as the
  normal size (mean ≈ 74 KiB on random data), above.
- **Fields the plan does not list:** `expiresAt` in the manifest (05 §7 puts `expires` on the pointer; the brief
  asks for the field so part 2 can use it), `compatEpoch`, `keyId`, the chunk's `storedSize`, and the pack and
  patch entry layouts.
- **Paths** are restricted to ASCII `[A-Za-z0-9._+-]` segments of at most 255 bytes, unique ignoring case,
  with no device names.
