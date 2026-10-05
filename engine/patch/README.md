# engine/patch — FastCDC chunking, the `.hman` manifest and the trust chain

`helios::patch` (L2, HEADLESS; namespace `helios::patch`, headers `helios/patch/*.h`) is the engine side of the
patch pipeline (05 §7, 08 §2.5–2.6). v0 (WP-0.16) holds:

- **BLAKE2b-256** (`Hash256`, `blake2b256`, `Blake2b256`): chunk IDs, file hashes and manifest hashes, over
  Monocypher 4.0.3's portable C (05 §7);
- **FastCDC** (`fastcdc::cut`, `splitBuffer`, `StreamChunker`, `chunkFile`) with 05 §7's sizes;
- the **`.hman` v0 manifest**: the format, its validator, a canonical writer, a hostile-input reader and a
  `ManifestBuilder` that turns chunked files into a canonical manifest (part 1);
- the **Ed25519 trust chain** (part 2): canonical keyset and pointer documents, the `TrustVerifier` that checks
  a keyset against the product's root pair, a pointer against the keyset, the manifest against the pointer and
  each chunk against the manifest, and the ratchets an install persists (`TrustState`, `TrustStateStore`);
- the **CDN layout and read path** (`cdn.h`): object paths, a local-directory fetcher, chunk objects and
  `verifyChannel`, the whole client-side check of one channel.

Go has the same pieces: [`services/pkg/cdc`](../../services/pkg/cdc), [`services/pkg/manifest`](../../services/pkg/manifest),
[`services/pkg/patchtrust`](../../services/pkg/patchtrust) (which also signs) and
[`services/pkg/patchcdn`](../../services/pkg/patchcdn) (which also publishes: `helios-patch publish`, see
[services/README.md](../../services/README.md#patching-helios-patch)). The vectors in `services/testdata/vectors/`
are shared: `go test` and `patch_tests` check the same files, so a divergence between the languages fails both
suites. This README is the specification for both.

The installer, `install.db` (which implements `TrustStateStore`), the planner, the HTTP clients, the
`StreamingInstaller` of 08 §4.1 and the stamped product block that supplies the root pair (08 §2.10.4) are later
WPs (0.17, 2.7, 2.16).

Depends on `helios::core`, Monocypher (BLAKE2b and Ed25519) and zstd (all private). The launcher links this module
in its x86-64-v1 (`base`) image (08 §2.1.1), so it has no ISA-specific code; file IO goes through core's platform
layer.

## Headers

| Header | Contents |
|---|---|
| `blake2b.h` | `Hash256` (`toHex`, `fromHex`, `isZero`, byte-wise ordering), `blake2b256`, `Blake2b256` (streaming) |
| `fastcdc.h` | `fastcdc::{kMinSize, kAvgSize, kMaxSize, kMaskS, kMaskL, kGearSeed, gearTable, cut}`, `Chunk`, `ChunkedFile`, `chunkBoundaries`, `splitBuffer`, `StreamChunker`, `chunkFile` |
| `manifest.h` | `hman::` format constants, `hman::RawHeader` and its codec; `Manifest` and its entries, `validateManifest`, `isValidManifestPath`, `isValidProductId`, `isValidPlatform`, `isValidBuildId`, `encodeManifestBody`, `writeManifest`, `readManifestHeader`, `readManifest`, `readManifestFile`, `ManifestBuilder` |
| `trust.h` | `Keyset`, `KeysetKey`, `Pointer`, `PointerNext`, `ManifestRef`, `parseKeyset`, `parsePointer`, `encodeKeyset`, `encodePointer`, `keysetSignedMessage`, `pointerSignedMessage`, `keyFingerprint`, `isTestOnlyKey`, `TrustCheck`, `trustCheckName`, `trustCheckOf`, `RootPair`, `TrustTarget`, `TrustState`, `advanceTrustState`, `encodeTrustState`, `decodeTrustState`, `TrustStateStore`, `FileTrustStateStore`, `MemoryTrustStateStore`, `TrustOptions`, `TrustVerifier` |
| `cdn.h` | `cdn::{keysetPath, pointerPath, manifestPath, chunkPath, isValidObjectPath, kMaxChunkObject, kMaxManifestObject}`, `CdnFetch`, `localCdn`, `decodeChunkObject`, `fetchChunk`, `VerifiedChannel`, `verifyChannel` |

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

// The launcher (WP-0.17): the root pair comes from the stamped product block, the ratchets from install.db.
TrustVerifier verifier = TrustVerifier::create({"sample-game", "live", "win64"}, rootPair).value();
FileTrustStateStore ratchets(installDir / "trust.state");     // install.db implements TrustStateStore later
Result<VerifiedChannel> ok = verifyChannel(localCdn(cdnDir), verifier, unixNow, ratchets);
if (!ok) log(trustCheckOf(ok.error()));                        // e.g. TrustCheck::PointerExpired
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

## The trust chain

08 §2.10.3's chain, as 05 §7 and 08 §2.5 step 1 verify it:

```
root pair (current, next; from the product block)  --signs-->  keys/<product>/keyset.json   (rootEpoch, version)
manifest subkey of the keyset                       --signs-->  channels/<product>/<channel>/<platform>.json
                                                    --signs-->  manifests/<product>/<build>/<platform>.hman
pointer.manifest_hash = the .hman header hash; the header commits to the body (bodyHash); the body lists
every chunk by BLAKE2b-256 ID
```

**Keys.** Ed25519 (RFC 8032, SHA-512): Monocypher 4.0.3's `monocypher-ed25519` here (ADR-013), `crypto/ed25519`
in Go. A key's ID is its fingerprint, the first 16 bytes of BLAKE2b-256 of the public key: the size of the `.hman`
header's `keyId`, and what a keyset entry's `id` must equal. Subkey roles are 08 §2.10.3's `manifest` (pointers
and manifests), `news` and `addons`; a keyset may list other roles (a later phase's) and keeps verifying, but only
a `manifest` key signs pointers and manifests.

### Canonical JSON

Keysets and pointers are JSON that admits exactly one encoding per document, so the bytes on the CDN are the bytes
the signer signed and no re-encoding step can differ between Go and C++:

- one object and nothing else: no whitespace, no byte-order mark, no trailing newline;
- members in byte order of their keys, each exactly once, optional members absent (`next` is the only one);
- strings: printable ASCII 0x20–0x7E except `"` and `\`, so there are no escapes;
- numbers: unsigned integers without leading zeros, at most 2^53 − 1 (exact in every JSON reader); no sign,
  fraction or exponent; `true` and `false`; no `null`;
- hashes, keys, key IDs and signatures as lowercase hex.

The parsers are schema-directed (they expect each member name in order, so there is no generic object model and
no recursion), refuse keysets over 64 KiB and pointers over 16 KiB, and every document that parses re-encodes to
the input byte for byte (the fuzz targets check it). The signed message is a context string, then the document
without its `sig` member: `HELIOS-KEYSET-V0\n` for keysets, `HELIOS-POINTER-V0\n` for pointers (a `.hman`
signature covers the header's bytes [0, 256), which start with `HMAN`), so a signature over one kind of document
never verifies as another.

**Keyset** (`keys/<product>/keyset.json`):

```
{"keys":[{"id":H16,"notAfter":N,"notBefore":N,"pub":H32,"role":S},...],"productId":S,"rootEpoch":N,"sig":H64,"version":N}
```

| Member | Rule |
|---|---|
| `keys` | 1–64 subkeys sorted by `id`, unique; `id` = fingerprint of `pub`; `role` `^[a-z][a-z0-9-]{0,31}$`; the key may sign from `notBefore` (unix seconds) until before `notAfter` > `notBefore` |
| `productId` | the product (08 §2.10.1's pattern) |
| `rootEpoch` | 1..2^32−2: which root signed it. The product block's pair has an epoch E: `current` signs keysets of epoch E, the pre-committed `next` those of E + 1 |
| `version` | 1..2^53−1; only grows |
| `sig` | the root's signature |

**Pointer** (`channels/<product>/<channel>/<platform>.json`):

```
{"build_id":S,"cdn_hosts":[S,...],"channel":S,"compat_epoch":N,"expires":N,"key_id":H16,"manifest_hash":H32,
 "min_client":S,"min_launcher":S,"next":{"available_at":N,"build_id":S,"compat_epoch":N,"manifest_hash":H32},
 "platform":S,"product_id":S,"rollback":B,"rollout_pct":N,"sequence":N,"sig":H64,"signed_at":N}
```

| Member | Rule |
|---|---|
| `product_id`, `channel`, `platform` | where it was published: a product ID, `^[a-z][a-z0-9-]{1,31}$`, a platform |
| `build_id`, `manifest_hash`, `compat_epoch` | the build (a `.hman` build ID), its header hash, its compat epoch (u32) |
| `sequence` | 1..2^53−1, monotonic per product, channel and platform |
| `rollback` | lets `sequence` be below an install's stored one |
| `min_launcher`, `min_client` | dotted versions `^[0-9]{1,9}(\.[0-9]{1,9}){0,3}$` |
| `cdn_hosts` | 0–16 base URLs of ≤ 256 bytes without spaces or userinfo (`@`): `https://` with a non-empty authority, or `http://` whose authority is exactly `localhost`, `127.0.0.1` or `[::1]` with an optional port of 1–5 digits, then nothing or a path (the dev CDN) |
| `next` | optional pre-download target (08 §2.6) |
| `rollout_pct` | 0..100 |
| `signed_at`, `expires` | unix seconds; checked against the subkey's validity, the verifier's clock (`signed_at` at most 1 hour ahead) and the 7-day lifetime |
| `key_id`, `sig` | the signing subkey and its signature |

### Verification order

`TrustVerifier` (Go: `patchtrust.Verifier`) runs these checks in this order; a rejection's message starts with the
check's name, which both languages share and the vectors pin:

| # | Check | Fails when |
|---|---|---|
| 1 | `keyset-malformed` | the keyset is not exactly the canonical encoding of a valid keyset |
| 2 | `keyset-root` | `rootEpoch` is neither the pair's epoch E nor E + 1 |
| 3 | `keyset-root-ratchet` | `rootEpoch` is below the install's stored root epoch (it moved to `next`) |
| 4 | `keyset-signature` | that root did not sign it (another root, another product's root, a changed byte) |
| 5 | `keyset-product` | it names another product |
| 6 | `keyset-version` | `version` is below the stored one |
| 7 | `pointer-malformed` | not exactly the canonical encoding of a valid pointer |
| 8 | `pointer-key-unknown` | `key_id` is not in the keyset: unknown, or revoked (a revocation is a keyset without the key) |
| 9 | `pointer-key-role` | the subkey's role is not `manifest` |
| 10 | `pointer-signature` | the subkey did not sign it |
| 11 | `pointer-key-window` | `signed_at` is outside the subkey's [`notBefore`, `notAfter`) |
| 12 | `pointer-future` | `signed_at` is more than `kMaxClockSkew` (1 hour) after now |
| 13 | `pointer-lifetime` | `expires` ≤ `signed_at`, or more than 7 days after it |
| 14–16 | `pointer-product`, `pointer-channel`, `pointer-platform` | it was published for another product, channel or platform |
| 17 | `pointer-expired` | now ≥ `expires` |
| 18 | `pointer-sequence` | `sequence` < the stored one and `rollback` is false |
| 19 | `manifest-malformed` | the `.hman` header does not read (part 1's header checks, including its header hash) |
| 20 | `manifest-hash` | its header hash is not the pointer's `manifest_hash` |
| 21–23 | `manifest-key-unknown`, `manifest-key-role`, `manifest-signature` | as 8–10, for the header's `keyId` and the signature over bytes [0, 256) |
| 24 | `manifest-key-window` | `createdAt` is outside the subkey's validity |
| 25 | `manifest-future` | `createdAt` is more than `kMaxClockSkew` after now |
| 26–29 | `manifest-product`, `manifest-platform`, `manifest-build`, `manifest-compat-epoch` | the header disagrees with the install or the pointer |
| 30 | `manifest-expired` | `expiresAt` is set and now ≥ it |
| 31 | `manifest-body` | the payload does not decode to a valid body (part 1's reader: `bodyHash`, tables, validation) |
| 32 | `chunk-missing` | a chunk object is not on the CDN |
| 33 | `chunk-corrupt` | it differs from a recorded `storedSize` or does not decode to exactly `rawSize` bytes |
| 34 | `chunk-hash` | its bytes do not hash to the chunk ID |

Signature checks come before the fields they protect are acted on; the cheap ratchet and epoch checks (2, 3) come
before the root's signature because they need no key. The subkey window is checked at signing time (`signed_at`,
`createdAt`), as 08 §2.10.3's 30-day overlap needs. Those times are the signer's claim, so the next check bounds
them by the install's clock: at most `kMaxClockSkew` (1 hour, Go `MaxClockSkew`) ahead of now. Without it a
pre-staged next-quarter subkey could sign before its `notBefore`, and a pointer with `signed_at` far ahead would
stay valid for as long as its subkey's window allows. With it the 7-day lifetime is a bound from now: a pointer
that verifies expires at most 7 days and 1 hour later, and a subkey past its `notAfter` cannot sign a pointer that
verifies more than that after `notAfter`. A launcher should report `pointer-future` as a wrong system clock. A manifest can be named by several
channels' pointers (promotion re-points), so its own `sequence` is not compared with the pointer's.
`verifyManifestHeader` checks a header without decoding the payload; it also verifies a pointer's `next` manifest
(pass `next->ref`) for pre-download. Malformed documents fail with `Corrupt` (`LimitExceeded` when too large), a
missing chunk with `NotFound`, every other check with `PermissionDenied`; `trustCheckOf()` recovers the check.

### Ratchets

An install keeps `TrustState {rootEpoch, keysetVersion, pointerSequence}` (08 §2.5: in `install.db`, WP-0.17).
After a keyset and pointer verify, `advanceTrustState` raises the root epoch and keyset version to theirs and the
pointer sequence to its (`max`), except that a `rollback: true` pointer sets the sequence to its own: the channel's
sequence restarts there, so the publisher's next pointers verify. Pointers between the new and the old sequence
that have not expired verify again after that; a rollback that must retire them is published with a sequence above
them instead (no flag needed). `verifyChannel` saves the state once the manifest verifies, before the chunks, which
an install fetches over hours.

`TrustStateStore` is the interface the launcher implements on `install.db`; `FileTrustStateStore` keeps a 32-byte
record (`HTRS`, version 0, the three fields little-endian, the first 4 bytes of BLAKE2b-256 of the first 28) and
refuses a corrupt one instead of resetting, since a reset ratchet would accept rolled-back pointers. A missing
file is a fresh install. Go's `patchtrust.FileStateStore` (`helios-patch verify --state`) keeps the same record
(`EncodeState`, `DecodeState`) with the same rule, so either language reads the other's file; both tests pin the
same 32 bytes.

### Test-only and dev keys

No private key material is committed. The shared vectors are signed by **test-only keys** whose seeds are
BLAKE2b-256(`"helios test-only key: <name>"`), public by construction (Go package `patchtrust/trusttest`, imported
only by `_test.go` files, which `TestTrustTestImportedOnlyByTests` checks over every other file of the module;
the C++ tests derive them with Monocypher). Their roots (`root-1`, `root-2`, `root-x`) are
listed in `isTestOnlyKey` (Go: `IsTestOnlyKey`), and `TrustVerifier::create` (Go: `NewVerifier`) refuses a root
pair that contains one unless `TrustOptions::allowTestKeys` is set, which only tests do; so a product build that
somehow carried a test root would verify nothing. Tests check both lists against the derivation.

**Dev keys** for `helios-patch publish --channel dev` are generated from the OS CSPRNG at first use into
`helios-data/keys/patch/<product>/` (05 §5's `keys\`; mode 0600 for the private files, marked `"dev": true`), and
publish signs only the `dev` channel with them, for loopback CDN hosts only. They stay out of git twice, as 08
§2.10.3 asks of dev keys: the directory gets a `.gitignore` of `*` before any seed is written (wherever
`--data-dir` puts it), and the repository ignores `helios-data/`. Real roots stay offline and real
subkeys come from Vault/KMS (08 §2.10.3, `helios-tool product init`, a later WP).

**No secret is compared.** The verifiers hold public keys only; signatures are checked by Ed25519 itself, and the
values compared byte for byte (hashes, key IDs, public keys, identifiers) are public, so no comparison needs to be
constant-time. The private seeds (Go's signer, the dev key files) are used only through `crypto/ed25519`.

## The CDN layout and read path

```
chunks/<aa>/<bb>/<blake2b-hex>.zst                 immutable: one zstd frame (level 19, content size, checksum)
manifests/<product>/<build-id>/<platform>.hman     immutable, signed
channels/<product>/<channel>/<platform>.json       the signed pointer (mutable)
keys/<product>/keyset.json                         the root-signed keyset (mutable)
```

05 §7's `/packs` and `/patches` are not written or read in v0: publish stores every chunk loose, and
`verifyChannel` refuses a packed chunk as `Unsupported`. `CdnFetch` reads an object by layout path with a size
limit (`NotFound`, `LimitExceeded`); `localCdn(root)` reads a directory through the platform layer and refuses
paths with empty, `.` or `..` segments, `\` or `:`. A chunk object decodes into exactly `rawSize` bytes (one byte of
room catches more), and its frame may declare a zstd window of at most 256 KiB (`cdn::kMaxChunkWindowLog`, the
largest chunk; Go's encoder writes a single-segment frame, whose window is its content size, above 1 KiB and a
1 or 2 KiB window below), so a hostile object costs at most
`rawSize + 1` bytes of output and a decoder with a 256 KiB window.
Go's `patchcdn` has the same read path (`DirSource`, and `HTTPSource`, which tests point at an `http.FileServer`)
and writes the layout (`helios-patch publish`, [services/README.md](../../services/README.md#patching-helios-patch)).

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

| `trust/cdn/` | A CDN tree in the 05 §7 layout, made by Go's `Publish` with the test-only keys: product `vector-game`, channel `live`, platform `win64`; keyset v3 of root epoch 1 (`manifest-a`, `manifest-old` whose validity ended 100 days before now, `news-a`, and `store-a` of a role no check knows); build `2026.09.21-r1` (an executable chunk stored raw, a pak whose repeated unit dedups, a text file, an empty file); a pointer of sequence 7 with `next` | `go test ./pkg/patchcdn -run TestUpdateTrustVectors -update` |
| `trust/cases.json` | 61 cases over that tree: the root pair, now (1,790,000,000) and the stored state, then per case replacement keyset, pointer, manifest and chunk files (in `trust/`, signed by the test-only keys), byte edits (overwrite or insert), a case's own state or now, and either the check it must fail or, if accepted, the state afterwards. 12 are accepted (a fresh install, an equal sequence, a rollback pointer below and above the stored sequence, a keyset of the next root before and after the ratchet moved, a pointer without `next` or hosts, a pointer and manifest signed at their key's `notBefore`, a manifest that expires later, a pointer and a manifest signed exactly 1 hour ahead of now); 49 are rejected, and every check of the table above is the one some case fails. Among them: a pointer and a manifest signed 1 hour and 1 second ahead, the current key signing 80 days ahead, and a pre-staged next-quarter key (`keyset-prestaged.json`) signing before its window opens; and two chunk objects that only one check catches, the chunk's own bytes in another encoding (`chunk-reencoded`: only the stored size differs) and a frame of the stored size that decodes to 2 bytes too few (`chunk-short`) | as above |
| `trust/syntax.json` | 73 documents (35 keysets, 38 pointers) both parsers must refuse: the CDN's keyset or pointer with one splice (`at`, `delete`, `put` or `putHex`, then `padTo` spaces): whitespace, a BOM, a trailing newline, truncation, member order, duplicate, unknown and missing members, escapes, non-ASCII, `null`, leading zeros, signs, fractions, exponents, 2^53 and 2^64, uppercase hex, short keys, unsorted, repeated and 65 keys, a key ID that is not the fingerprint, an empty validity window, out-of-range epochs, rollout, compat epochs and sequences, bad channels, platforms, build IDs, versions and hosts (among them userinfo after a loopback name or address, a non-numeric or six-digit port, a query, an `https://` without a host), an empty or misplaced `next`, and documents one byte over the size limits | as above |

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
- **Header first.** `readManifestHeader` validates only the header: enough for `verifyManifestHeader` to refuse
  another product's, platform's or build's manifest, or a bad signature, before decompressing anything.
- **Canonical form.** `encodeManifestBody` of a read manifest reproduces the body that was read (the fuzz targets
  check this). `ManifestBuilder::build` sorts and deduplicates; a chunk ID seen with two sizes, a duplicate path,
  a placement or patch naming an unknown chunk, pack or file, or a chunk placed twice fails with
  `InvalidArgument`.
- **Trust documents.** The keyset and pointer parsers read at most 64 KiB and 16 KiB, never recurse, allocate at
  most one string per member, and stop at the first byte that is not the canonical encoding; `syntax.json` pins 73
  refusals in both languages, and every single-byte change of the vectors' keyset, pointer and manifest header
  (three changes per byte) is rejected by both (`patch_tests`, Go's `TestEveryByteTampered`).
- **Threading.** Everything is a value type or a pure function; a `StreamChunker`, `Blake2b256` or
  `ManifestBuilder` belongs to one thread (Go: a `Chunker` or `Builder` to one goroutine). A `TrustVerifier` is
  immutable and may be shared; a `TrustStateStore` is the caller's to synchronize.

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

**The trust chain** is not on a hot path, so it states no budget: a launcher runs it once per start and pointer
refresh, and chunk checks are the hashing above. Verifying the vectors' keyset, pointer and manifest header (three
Ed25519 verifications and the canonical parses) took 0.34 ms in C++ (portable Monocypher, GCC 13 RelWithDebInfo,
best of 5 × 200) and 0.17–0.24 ms in Go (`BenchmarkVerifyChain`) on the dev VM, 2026-10-05.

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
- **C++, trust:** `fuzz/fuzz_trust_parser.cpp` runs each input as a keyset, a pointer and (from 352 bytes) a `.hman`
  header. It checks that every call returns a Result, that a document that parses re-encodes to exactly the input,
  that what verifies also parses, and that every verification failure names its check. Verification uses the
  test-only keys (a keyset signed by `root-1` with a manifest, a news and an unknown-role subkey), so the seeds'
  signatures verify and mutations reach the checks behind them. `fuzz/corpus/trust_parser/` holds `seed_000`–`seed_004`
  (`--make-seeds`: a keyset, a pointer with `next`, a minimal rollback pointer, a news-key pointer, a keyset of the
  next root) and copies of the shared vectors (`vector_*`). Without `HELIOS_PATCH_LIBFUZZER` it is the CTest
  `patch_fuzz_trust_parser` (label `fuzz`), replaying the corpus plus 20,000 mutations.
- **Go:** `FuzzParse` (pkg/manifest; its seeds, which every `go test` runs, are the shared goldens with
  `deep-paths.hman`, a 64-file deep-paths manifest and the 57 hostile cases) checks the same properties, and
  `FuzzChunker` (pkg/cdc) checks that the streaming chunker agrees with `Split` and the size bounds for any input
  and read pattern. `FuzzParseKeyset` and `FuzzParsePointer` (pkg/patchtrust) check the parsers' round trip and
  that verification returns a rejection or a document, and `FuzzVerifyManifestHeader` runs the manifest checks on
  hostile `.hman` bytes; their seeds are the shared trust vectors.

A campaign:

```
cmake --preset linux-clang -B build/fuzz -DHELIOS_BUILD_GRAPHICS=OFF -DHELIOS_BUILD_TESTS=OFF \
  -DHELIOS_PATCH_LIBFUZZER=ON "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link,address,undefined" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined"
cmake --build build/fuzz --target patch_fuzz_manifest_reader patch_fuzz_trust_parser
build/fuzz/bin/patch_fuzz_manifest_reader -max_total_time=600 -rss_limit_mb=2048 -malloc_limit_mb=512 corpus-copy/
# -max_len: libFuzzer's default (4096 here) never reaches the 64 KiB keyset or 16 KiB pointer bound;
# -len_control=0 starts at that length instead of growing towards it
build/fuzz/bin/patch_fuzz_trust_parser -max_total_time=600 -max_len=70000 -len_control=0 -rss_limit_mb=2048 \
  -malloc_limit_mb=512 trust-copy/
cd services && go test ./pkg/manifest -run '^$' -fuzz FuzzParse -fuzztime 5m
go test ./pkg/patchtrust -run '^$' -fuzz '^FuzzParseKeyset$' -fuzztime 5m   # and FuzzParsePointer, FuzzVerifyManifestHeader
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

Part 2 (2026-10-05, load average 5–8): `patch_fuzz_trust_parser` (Clang 18.1.3, ASan and UBSan with
`-fno-sanitize-recover=undefined`, one process, `-malloc_limit_mb=512`, the 11 committed seeds) ran 7,330,229
inputs in 11 minutes (about 11,100 per second) and reached 1,704 coverage edges (3,374 features; corpus 11 → 574
units; peak RSS 521 MB) with no finding, and `patch_tests` (its 32 non-`perf:` cases) passed twice in the same
sanitizer build. Go, one worker, 5 minutes each:
`FuzzParseKeyset` ran 2,379,149 inputs (corpus 37 → 65), `FuzzParsePointer` 1,473,096 (39 → 52) and
`FuzzVerifyManifestHeader` 1,381,666 (13 → 20), with no failure.

After review round 1 (2026-10-05, load average 5–7; the same sanitizer flags, the 11 committed seeds):
`patch_fuzz_trust_parser -max_len=70000` ran 5,567,555 inputs in 631 seconds (about 8,800 per second) and
reached 1,769 edges (3,431 features; corpus 11 → 582 units; peak RSS 551 MB). Its length limit grew to 47,050
bytes, past the pointer bound but not the keyset's, so a second run continued from that corpus with
`-len_control=0`: 1,747,946 inputs in 181 seconds, 1,778 edges (3,467 features), inputs up to 65,708 bytes.
Neither found anything, and `patch_tests` (its 33 non-`perf:` cases) passed twice in the same build. Go, one
worker, 5 minutes each: `FuzzParseKeyset` 2,130,359 inputs (corpus 75 → 77) and `FuzzParsePointer` 868,510 and
1,188,216 (65 → 73), with no failure. `FuzzVerifyManifestHeader` ran 3,002 inputs (62 → 66) with Go's default
minimization, which spends up to a minute on each new input, then 115,162 (66 → 119) with
`-fuzzminimizetime=5s`; no failure.

## Gaps (v0)

- **Packing and patches.** The format holds the pack index (05 §7: chunks under 32 KiB in ~8 MiB packs) and
  patch entries, and the builder places chunks it is told about, but publish v0 stores every chunk loose and makes
  no patches, and the read path refuses packed chunks (`Unsupported`). Validation checks that each packed chunk
  fits inside its pack, not that two chunks' stored bytes in one pack do not overlap.
- **The product block.** The root pair is the caller's (`RootPair`); the stamped `.hprod` reader that supplies it,
  its self-signature check and CL-14's half-stamped-block clause are WP-2.16's (08 §2.10.4), as are
  `helios-tool product init`, the Shamir root ceremony, `rotate-subkey`, `revoke-subkey` and `rotate-root`
  (08 §2.10.3). The verifier already accepts a keyset of the `next` root and ratchets past the old one.
- **The launcher side.** `install.db` (which implements `TrustStateStore`), the HTTP fetchers (WinHTTP, libcurl),
  rollout selection (`hash(install_id) mod 100 < rollout_pct`), `min_launcher`/`min_client` enforcement and
  pre-downloading `next` are WP-0.17's; `verifyManifestHeader` already takes a pointer's `next` reference.
- **File hashes.** `verifyChannel` checks every chunk, not each file's whole-file hash; the installer checks file
  hashes before each rename (08 §2.5 step 1), WP-0.17.
- **Format evolution.** The documents refuse unknown members (one canonical form per document); a later field
  needs a new context string (`...-V1`) and a transition period in which publish writes both, which no WP has
  planned yet. Unknown subkey roles are accepted, so new roles need no format change.
- **SIMD BLAKE2b.** The self-dispatching SSE4.1/AVX2 compression functions of 08 §2.1.1 are not written; the
  C++ hashing rate is the portable one above.
- **`@base` builds** of this module for the launcher come with WP-0.2r; the module has no ISA-specific code.
- **Nightly fuzzing.** `nightly.yml`'s libFuzzer job runs only engine/net's targets, as for engine/asset's;
  scheduling these two targets (and `go test -fuzz`) toward CL-14's 24 CPU-hours needs a nightly matrix entry and
  a scorecard gate. The Go timing test with `HELIOS_PERF=1` is not in the nightly perf step either.
- **One publisher at a time.** `helios-patch publish` takes no lock on the CDN directory; two concurrent publishes
  to one channel can both read sequence n and both write n + 1.
- **Minimum keyset version.** A fresh install (the zero `TrustState`) accepts any keyset version. 08 §2.10.3's
  product block carries a minimum keyset version; the launcher (WP-0.17, with the product block of WP-2.16)
  should seed `TrustState::keysetVersion` from it on first run, so a fresh install cannot be handed an old keyset
  that still lists a revoked subkey. `RootPair` does not carry it in v0.
- **Publish is not atomic across files.** The keyset is written just before the pointer (the pointer is signed
  first, so only the two renames lie between them). A keyset that drops the key of the pointer already on the
  CDN leaves the two inconsistent for that moment, or until the next publish if the pointer's write fails.
  `WriteFileAtomic` syncs the file and (POSIX) its directory; a crash before the rename can leave a `.tmp-*` file
  in a served directory, which no layout path names.
- **Identifiers on NTFS.** Build IDs, channels and platforms follow part 1's grammar, which admits names that
  alias on a Windows `helios-data\cdn` (device names, trailing dots, case-only variants). This fails safe (the
  manifest-build check and publish's body-hash comparison refuse the mix-up) but gives confusing publish errors;
  tightening the grammar is a follow-up.

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (05 §5, §6.5, §7, §8; 08 §2.1.1, §2.5, §2.6, §2.10.1–§2.10.4, §4.1, §4.4 CL-14,
§4.5; 02 §1.1, §6.3; 09 §2 WP-0.16) on 2026-10-04 (part 1) and 2026-10-05 (part 2). Deviations and choices the
plan leaves open:

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
- **Document encoding (part 2).** 05 §7 and 08 §2.10.3 list the pointer's and keyset's fields but not an encoding.
  Both are the canonical JSON above, with the member names as the plan spells them (snake_case in the pointer,
  05 §7; camelCase in the keyset, 08 §2.10.3), times in unix seconds, binary values in lowercase hex, and the
  signature over a context string plus the document without `sig`.
- **Pointer fields the plan does not list:** `product_id`, `channel` and `platform` (so a pointer verifies only
  where it was published: 08 §2.10.3's product binding, extended to channels and platforms, so a `ptr` pointer
  cannot be served as `live`), `key_id` (the signing subkey) and `signed_at` (for the subkey's validity window).
- **Keyset field the plan does not list:** `rootEpoch`, which root signed it. The product block's root pair carries
  the epoch of `current` (`RootPair::epoch`), `next` signs epoch + 1, and the stored root epoch is 08 §2.10.3's
  root-epoch ratchet.
- **Subkey IDs** are fingerprints (BLAKE2b-256 of the public key, first 16 bytes: the `.hman` `keyId` size); the
  plan's ceremony record names BLAKE2b fingerprints without a length.
- **Verifier rules the plan implies:** the pointer lifetime of 7 days is enforced (`expires - signed_at`); a
  subkey is checked at signing time; a manifest's own `expiresAt`, when set, is checked; manifest and pointer
  sequences are not compared; a `rollback` pointer resets the stored sequence to its own (05 §7 says only that a
  lower sequence needs the flag). Equal sequences and keyset versions verify (a re-fetch).
- **Clock skew (review round 1).** The plan names no tolerance between a signer's clock and an install's; this
  PR refuses a `signed_at` or `createdAt` more than **1 hour** after the install's now (`kMaxClockSkew`, Go
  `MaxClockSkew`), so the 7-day lifetime holds from the install's clock (7 days + 1 hour at most) and a
  pre-staged subkey cannot sign before its window. One hour is this PR's choice, not the plan's: an install whose
  clock lags by more refuses each freshly signed pointer until its clock catches up (the launcher should say so).
  Changing it is one constant per language and a regeneration of the vectors.
- **Dev keys** live in `helios-data/keys/patch/<product>/` (05 §5's `keys\` holds the dev "manifest Ed25519"
  keys), not 08 §2.10.3's `<project>/.helios/devkeys/`, which belongs to `helios-tool product init --dev` (a
  later WP, with project files). Like 08's, they are git-ignored (the directory's own `.gitignore` and the
  repository's `helios-data/` pattern). `helios-patch` has a `verify` subcommand the plan does not name.
- **CL-14 subset.** This meets CL-14 for tampered, expired, rolled-back and wrongly keyed pointers, manifests,
  keysets and chunks, another product's keyset, pointer and manifest, a revoked subkey and a root the install has
  ratcheted past, and a subkey used before its validity window (a signing time ahead of the install's clock).
  Not here: the `.hprod` clause (WP-2.16) and the release tier's 24 CPU-hours of fuzzing.
