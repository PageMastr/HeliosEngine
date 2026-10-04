# engine/assetpipe — asset pipeline (editor and tools)

`helios::assetpipe` (L4, EDITOR_ONLY; namespace `helios::assetpipe`, headers `helios/assetpipe/*.h`) will hold
the importers, bakers, the DDC and the cooker (02 §6.2). v0 (WP-0.8 part 1 of 3) holds only the **`.hpak` v0
writer** (02 §6.3), so that the reader in [`engine/asset`](../asset/README.md) round-trips real paks; `helios-pack`
(08) drives it later. Depends on `helios::asset`, `helios::core` and zstd (private). EDITOR_ONLY: never linked
into the client, launcher, bot or servers (02 §1.1).

## Headers

| Header | Contents |
|---|---|
| `hpak_writer.h` | `HpakWriter` (`create`, `add`, `size`, `emit`, `build`, `writeFile`), `HpakWriterOptions`, `HpakAssetOrder`, `planHpakLayout`, `HpakLayout`, `resealHpak` |

## Usage

```cpp
using namespace helios;
assetpipe::HpakWriterOptions options;
options.platform = asset::HpakPlatform::PcClient;
options.contentBuild = buildNumber;
options.tags = {2, zoneRecordHash, 0};          // tier 2, the zone's group, language-neutral
options.zstdLevel = 19;                         // release cook (dev cooks keep the default 3)
auto writer = assetpipe::HpakWriter::create(options).value();
for (const Cooked& c : cooked)                  // any order
    writer.add(c.guid, c.bytes, {c.tier, c.group, c.language, c.firstUse}).value();
writer.writeFile(outDir / "zone.hpak").value(); // temp file, flushed, renamed over the target
```

## What it guarantees

- **Deterministic bytes.** The same assets and options give a byte-identical pak whatever order they were added
  in (tested with reversed and shuffled orders). Blobs are ordered by 02 §6.3's key, **tier → group → language →
  recorded first use → GUID** (`HpakAssetOrder`; unrecorded assets after recorded ones), which keeps 05 §7's
  FastCDC chunks stable across builds. zstd runs single-threaded at a fixed level with no checksum or dictionary
  id in its frames; identical output also needs the same zstd version (vendored, pinned in `third_party/MANIFEST.md`).
- **Layout.** Each blob starts 4 KiB aligned and is zero padded; an asset is split into 256 KiB blocks, each its
  own zstd frame, and stored raw instead when zstd would not shrink it. The TOC is sorted by `AssetId`; the pak
  block, TOC and header checksums are computed over the bytes as written.
- **Identity.** `add` folds the GUID with `AssetId::fromGuid` and rejects the nil GUID, a GUID already added and
  an `AssetId` collision with another GUID (`asset::AssetIdSet`; 02 §6.1 "packaging rejects collisions").
- **Limits.** An asset above 2 GiB, or one that would take the pak past 2 GiB (02 §6.3), fails `add` with
  `LimitExceeded` (the pak-limit message says to split the content across more paks); the writer is unchanged by
  a failed `add`.
  `size()` is the exact size `build()` would produce. `planHpakLayout` is the layout and limit arithmetic the
  writer uses, public so the 2 GiB boundary is tested exactly without writing 2 GiB.
- **`resealHpak`** recomputes the checksums of a patched in-memory pak. It exists for tests and the reader's fuzz
  target, which use it to get corrupted fields past the checksums and into the reader's field checks.

Threading: an `HpakWriter` is single-threaded; its const members may run concurrently once no `add` is in flight.

## Gaps (v0)

- Memory: `add` keeps every coded blob in memory until the pak is emitted, so a 2 GiB pak needs about 2 GiB of RAM
  in the cooker (`emit` and `writeFile` stream, `build` doubles it). A streaming `helios-pack` comes with the cook.
- Duplicate assets (identical bytes under two ids) are stored twice; the format already allows sharing a blob
  (02 §6.3 "duplicate assets are stored once").
- `.meta` sidecars, the DDC key and the local DDC store are WP-0.8 part 3; importers, bakers and the cooker are
  Phase 1 and later.

## Plan conformance

Plan-Rev: 12

Written for plan revision 12 (02 §1.1, §6.2, §6.3; 05 §7; 09 §2 WP-0.8) on 2026-10-03.
