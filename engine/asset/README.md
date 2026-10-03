# engine/asset — asset identity, handles and `.hpak` paks

`helios::asset` (L2, HEADLESS; namespace `helios::asset`, headers `helios/asset/*.h`) is the runtime
side of the asset pipeline (02 §6.1, §6.3, §6.4; ADR-006). v0 (WP-0.8 part 1 of 3) holds:

- **`AssetId`**, the 64-bit fold of an asset's GUID, and **`AssetIdSet`**, the collision check packaging
  runs before it writes a pak ("packaging rejects collisions", 02 §6.1);
- **`AssetHandle<T>` / `AssetStore<T>`**: loaded instances behind core generational handles, with hot-reload
  versions staged and swapped only at a frame or tick boundary (02 §6.4);
- the **`.hpak` v0 reader** (`HpakReader`): header and TOC validation, XXH3-64 verification of every 64 KiB pak
  block on its first read, a re-fetch hook for bad blocks, zstd per 256 KiB asset block, the XXH3-128 cooked hash
  checked on every decode;
- **`PakMountTable`**: paks mounted in order, later ones overlaying earlier ones by `AssetId` (patch paks,
  02 §6.3 "Patching").

The writer is [`engine/assetpipe`](../assetpipe/README.md)'s (EDITOR_ONLY). Depends on `helios::core` and zstd
(private). File IO goes through core's platform layer (`fs::File` positional reads). 02 §1.1's table also lists
`reflect` as a dependency; v0 does not need it (`refl::AssetRef` carries the `Guid` that `AssetId::fromGuid`
folds), so it is not linked yet.

## Headers

| Header | Contents |
|---|---|
| `asset_id.h` | `AssetId` (`fromGuid`, `toString`, `std::hash`, `std::formatter`), `AssetIdSet` (`insert`, `check`, `find`) |
| `asset_handle.h` | `AssetHandle<T>` (= `Handle<AssetTag<T>>`), `AssetStore<T>` (`insert`, `find`, `pin`, `stage`, `commitSwaps`, `erase`) |
| `hpak_format.h` | The on-disk layout: constants, `HpakPlatform`, `HpakCodec`, `HpakTags`, `HpakEntry`, `hpak::Header`, the field encoders and decoders |
| `hpak_reader.h` | `IHpakSource` (`openHpakFile`, `makeHpakMemorySource`), `IBlockRefetcher`, `RefetchStatus`, `HpakReader`, `HpakBlockState` |
| `mount_table.h` | `PakMountTable`, `AssetLocation` |

## Usage

```cpp
using namespace helios::asset;

HpakOpenOptions options;
options.platform = HpakPlatform::PcClient;     // refuse a server or editor cook
options.refetcher = installerHook;             // optional IBlockRefetcher (08 §2.6)
auto base = HpakReader::openFile(dir / "common.hpak", options).value();
auto patch = HpakReader::openFile(dir / "patch-0042.hpak", options).value();

PakMountTable mounts;
mounts.mount(base).value();
mounts.mount(patch).value();                   // overlays base by AssetId

const AssetId id = AssetId::fromGuid(ref.guid);
Result<std::vector<u8>> bytes = mounts.read(id);  // the patch's copy if it has one

AssetStore<Mesh> meshes;                       // owned by the game thread's asset layer
AssetHandle<Mesh> h = meshes.insert(id, decodeMesh(*bytes));
meshes.stage(h, decodeMesh(newBytes));         // hot reload: not visible yet
meshes.commitSwaps();                          // at the frame or tick boundary
```

## The `.hpak` v0 format

`hpak_format.h` is the reference; all fields are little-endian and fixed width.

```
[0, 4096)              header block: magic 'HPAK', version 0, platform, flags, contentBuild u64,
                       tags {tier u8, group u64, language u32}, tocOffset, tocSize, assetCount,
                       assetBlockCount, pakBlockCount, tocHash (XXH3-64 of the TOC), headerHash
                       (XXH3-64 of the fields before it); zeros to 4 KiB
[4096, tocOffset)      blobs, each 4 KiB aligned and zero padded to 4 KiB
[tocOffset, end)       TOC: 64-byte entries sorted by AssetId {id, cookedHash XXH3-128, offset,
                       compSize, rawSize, firstBlock, blockCount, codec}, then u32 stored size per asset
                       block, then XXH3-64 per 64 KiB pak block of the blob region
```

- **Asset blocks.** An asset is `ceil(rawSize / 256 KiB)` independently coded blocks (one for a small asset),
  so a large asset can be decoded block by block and a bad block costs one block. The codec is per asset: zstd
  frames, or the raw bytes when zstd would not make the asset smaller.
- **Pak blocks.** The blob region is checksummed in 64 KiB pak blocks (02 §6.3). The header and the TOC have
  their own checksums and are verified whole at open, since the reader reads them whole anyway.
- **Limits.** A pak is at most 2 GiB (02 §6.3), an asset at most 2 GiB decoded (a v0 choice that keeps every
  size within the pak's range). Version 0 has no flags; reserved bytes must be zero.
- **Shared blobs.** Two entries may point at the same bytes (the reader bounds each read by its own entry), so a
  writer can store duplicate assets once without a format change.

## Behaviour worth knowing

- **Hostile input.** `open()` fails with `Corrupt`, `VersionMismatch`, `Unsupported` (another platform),
  `LimitExceeded` (above 2 GiB), `EndOfFile` or `IoError`, and never reads outside the file: every offset, size
  and count is checked against the file and against each other in 64-bit arithmetic (the block table must be
  consumed in entry order, so validation is linear in the TOC). The TOC allocation is bounded by the file size;
  a read allocates the asset (`read`) or nothing (`readInto`, caller's buffer) plus one asset block of scratch.
  `read()` allocates `rawSize` bytes, up to 2 GiB: use `readInto` where the caller wants to bound memory.
- **First-read verification.** A read hashes each pak block it touches the first time (it reads the whole block
  for that), marks it `Verified` and never hashes it again. Decoded bytes are always checked against the cooked
  hash. XXH3 detects corruption, not tampering: distribution integrity is BLAKE2b's (05 §7, 08 §2.5).
- **Bad blocks.** A mismatch calls `IBlockRefetcher::refetch` on the reading thread, under the pak's repair lock,
  **at most once per block** until `retryBlocks()` covers it. `Repaired`: the reader re-reads the block once and
  verifies it (still bad: `Bad`). `Pending`: reads of the block fail with `Busy` until the installer calls
  `retryBlocks()` after its re-fetch lands. `Failed` (or no hook): `Corrupt`. The hook must not read from the
  same reader.
- **Overlay.** `PakMountTable::find` returns the most recently mounted pak's entry. Unmounting rebuilds the index;
  a location already returned keeps its pak alive. A mount changes what later lookups resolve to; instances
  already loaded keep their version until their `AssetStore` swaps at a frame or tick boundary.
- **Threading.** Every function documents its rules. In short: `AssetId` is a value; `AssetIdSet` and the writer
  are single-threaded; `AssetStore`, `HpakReader` and `PakMountTable` are thread-safe.

## Performance

No plan budget covers these paths, so v0 states its own (`perf:` cases in `tests/test_perf.cpp`, label `perf`,
nightly; asserted in optimized builds without sanitizers). Measured on the shared 4-vCPU dev VM, GCC 13,
RelWithDebInfo, 2026-10-03:

| Path | Budget | Measured |
|---|---|---|
| `HpakReader::open` of a 100k-asset pak (6.4 MB TOC) | ≤ 60 ms | 6.9 ms |
| `PakMountTable::mount` of it | ≤ 40 ms | 7.3 ms |
| `PakMountTable::find`, random ids, 100k-asset table | ≤ 250 ns mean | 90 ns |
| First read: block verify + zstd decode + cooked hash, 35 MB of mixed assets, one thread | ≥ 300 MB/s (≤ 0.85 ms per 256 KiB block) | 798 MB/s |
| Later reads (blocks already verified) | ≥ 400 MB/s | 851 MB/s |

The read budget is twice 02 §5.7's sustained I/O budget (150 MB/s), so one decode thread keeps up with the disk.

## Fuzzing

`fuzz/fuzz_hpak_reader.cpp` is a libFuzzer target (02 §8.3 and 09 §6 list the `.hpak` reader among the fuzzed
readers; AAA-SEC-7). Each input runs as given and again after `assetpipe::resealHpak` has recomputed its
checksums, so mutated fields reach the validators behind the header and TOC checksums. It checks that open and
reads return Results, that opened entries are sorted, aligned and in bounds, that successful reads match the
cooked hash and repeat identically, that the hook runs at most once per block, and that a second mount overlays
the first. `fuzz/corpus/hpak_reader/` holds the seeds (`asset_fuzz_hpak_reader --make-seeds <dir>` rewrites
them). Without `HELIOS_ASSET_LIBFUZZER` the target is a CTest (label `fuzz`, every PR) that replays the corpus
plus 20,000 deterministic mutations. A campaign:

```
cmake --preset linux-clang -B build/fuzz -DHELIOS_BUILD_GRAPHICS=OFF -DHELIOS_BUILD_TESTS=OFF \
  -DHELIOS_ASSET_LIBFUZZER=ON "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link,address,undefined" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined"
cmake --build build/fuzz --target asset_fuzz_hpak_reader
build/fuzz/bin/asset_fuzz_hpak_reader -max_total_time=600 -rss_limit_mb=2048 corpus-copy/
```

The nightly libFuzzer job (`nightly.yml`) runs only engine/net's targets; adding this one needs its matrix to
name a target and corpus per gate and a scorecard gate with a criterion to hang on, which is not a small change
(see Gaps).

## Gaps (v0)

- **`registry.hreg`** (02 §6.1) is not built: the reader does not need it. It comes with the cook (assetpipe,
  part 3 and later).
- **`IResidencyProvider`** (02 §5.7: `resident`, `demand`, `prioritizeGroup`) is not defined; 09 §2 gives the
  residency hook to WP-2.1. v0's bad-block hook is `IBlockRefetcher`, which that WP routes to the
  `StreamingInstaller`'s `demand` (08 §2.6). Pak mounts are not yet `core::IMountProvider`s and have no async
  ranged reads (02 §5.7): assets are addressed by `AssetId`.
- **Patch deletions.** A patch pak can replace and add assets but not remove one (no tombstones).
- **Duplicate storage.** The reader accepts shared blobs, but the writer does not yet store duplicates once.
- **Nightly fuzzing** of this target, toward 02 §8.3's ≥ 24 CPU-hours per release, is not scheduled (above).
- **Hot reload transport** (`helios-assetd` messages, 02 §6.4) and the asset manager that turns pak bytes into
  typed instances are later work; `AssetStore` is the swap primitive they will use.

## Plan conformance

Plan-Rev: 12

Written for plan revision 12 (02 §1.1, §5.7, §6.1, §6.3, §6.4, §8.3; 08 §2.5, §2.6; 09 §2 WP-0.8) on 2026-10-03.
Choices the plan leaves open are recorded above: the fold (XXH3-64 of the GUID's canonical bytes; a plain
`high ^ low` maps structured GUIDs with equal halves to 0), the per-asset codec, the 2 GiB asset cap, the
`IBlockRefetcher` hook in place of `IResidencyProvider`, and the `reflect` dependency not yet taken.
