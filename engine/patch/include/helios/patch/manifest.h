#pragma once
// The `.hman` v0 build manifest (05 §7, 08 §2.5–2.6): which files a build of a product has on one
// platform, their FastCDC chunks (BLAKE2b-256 IDs), install tiers and tags, the pack index and the
// optional patch-from deltas. Go's services/pkg/manifest reads and writes the same bytes; the shared
// vectors in services/testdata/vectors/hman/ pin both. engine/patch/README.md is the specification;
// in short (all integers little-endian):
//
//   [0, 352)    header: magic 'HMAN', version 0, headerSize 352, flags, codec, sequence, createdAt,
//               expiresAt, compatEpoch, bodySize, payloadSize, bodyHash, productId, platform, buildId,
//               keyId, headerHash (BLAKE2b-256 of bytes [0, 256)), signature (bytes [288, 352))
//   [352, end)  payload: the body, stored raw (codec 0) or as zstd frames (codec 1)
//
//   body        counts {files, chunks, refs, packs, patches, stringBytes}, then the tables files[80 B],
//               refs[16 B], chunks[56 B], packs[40 B], patches[80 B], then the paths back to back
//
// The body is canonical: files sorted by path, chunks and packs sorted by hash and unique, refs and
// paths laid out in file order, every chunk and pack referenced, reserved bytes zero. So one manifest
// has exactly one body, and both languages' writers produce the same bytes (the zstd payload differs
// by encoder; bodyHash covers the decoded body). Part 2 of WP-0.16 signs bytes [0, 256) with Ed25519
// into the reserved signature field, so signing does not change the layout; headerHash is the
// manifest's identity (the pointer's `manifest_hash`). v0 writes the signature as given (zeros by
// default) and readers do not interpret it.
//
// Hostile input: readManifest() checks the header hash, every field, the decoded body's size and hash,
// every count, offset and size against the limits below and each other, and fails with a Result error
// (Corrupt, VersionMismatch, Unsupported or LimitExceeded), never UB. A zstd payload decodes into a
// buffer that grows only as bytes decode, up to bodySize (≤ ManifestReadOptions::maxBodySize).
//
// Threading: everything is a value type or a pure function; a ManifestBuilder belongs to one thread.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/patch/blake2b.h"
#include "helios/patch/fastcdc.h"

namespace helios::patch {

namespace hman {

inline constexpr u32 kMagic = 0x4E414D48u; ///< "HMAN" as little-endian bytes.
inline constexpr u16 kVersion = 0;         ///< The v0 format (WP-0.16 part 1).
inline constexpr usize kHeaderSize = 352;
inline constexpr usize kSignedBytes = 256; ///< headerHash and (part 2) the signature cover [0, 256).
inline constexpr usize kHeaderHashOffset = 256;
inline constexpr usize kSignatureOffset = 288;
inline constexpr usize kSignatureSize = 64;
inline constexpr usize kKeyIdSize = 16;
inline constexpr usize kBodyHeaderSize = 32;
inline constexpr usize kFileEntrySize = 80;
inline constexpr usize kRefEntrySize = 16;
inline constexpr usize kChunkEntrySize = 56;
inline constexpr usize kPackEntrySize = 40;
inline constexpr usize kPatchEntrySize = 80;

inline constexpr usize kProductIdMax = 32; ///< productId: ^[a-z][a-z0-9-]{2,31}$ (08 §2.10.1).
inline constexpr usize kPlatformMax = 32;  ///< platform: ^[a-z][a-z0-9_-]{1,31}$ ("win64", "linux64").
inline constexpr usize kBuildIdMax = 64; ///< buildId: ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ (a CDN path segment).

inline constexpr u64 kMaxBodySize = 256 * kMiB;
inline constexpr u32 kMaxFiles = 1u << 20;
inline constexpr u32 kMaxChunks = 1u << 24; ///< 1 TiB of unique content at 64 KiB per chunk.
inline constexpr u32 kMaxRefs = 1u << 24;
inline constexpr u32 kMaxPacks = 1u << 20;
inline constexpr u32 kMaxPatches = 1u << 20;
inline constexpr u32 kMaxStringBytes = 64 * 1024 * 1024;
inline constexpr u32 kMaxPathBytes = 1024;
/// A path segment's limit: NAME_MAX on ext4 is 255 bytes and NTFS allows 255 UTF-16 units (paths are
/// ASCII), so every valid path can be installed on both.
inline constexpr u32 kMaxSegmentBytes = 255;
inline constexpr u64 kMaxPackSize = 1 * kGiB;
inline constexpr u64 kMaxPatchSize = u64(1) << 40;
inline constexpr u8 kMaxTier = 2;
inline constexpr u32 kNoPack = 0xFFFFFFFFu;  ///< ManifestChunk::pack of a chunk stored loose on the CDN.
inline constexpr int kMaxZstdWindowLog = 25; ///< Readers refuse frames needing a larger window (32 MiB).

/// The fixed header as stored, without validation (readManifestHeader validates).
struct RawHeader {
    u32 magic = kMagic;
    u16 version = kVersion;
    u16 headerSize = static_cast<u16>(kHeaderSize);
    u32 flags = 0;
    u8 codec = 0;
    u8 reserved0[3] = {};
    u64 sequence = 0;
    u64 createdAt = 0;
    u64 expiresAt = 0;
    u32 compatEpoch = 0;
    u32 reserved1 = 0;
    u64 bodySize = 0;
    u64 payloadSize = 0;
    Hash256 bodyHash;
    std::array<char, kProductIdMax> productId{};
    std::array<char, kPlatformMax> platform{};
    std::array<char, kBuildIdMax> buildId{};
    std::array<u8, kKeyIdSize> keyId{};
    u8 reserved2[16] = {};
    Hash256 headerHash;
    std::array<u8, kSignatureSize> signature{};
};

/// Writes `h` as the 352-byte header (headerHash as given).
void encodeHeader(const RawHeader& h, std::span<u8, kHeaderSize> out) noexcept;
/// Reads the 352-byte header; no validation.
RawHeader decodeHeader(std::span<const u8, kHeaderSize> in) noexcept;
/// BLAKE2b-256 of an encoded header's first kSignedBytes bytes.
Hash256 computeHeaderHash(std::span<const u8, kHeaderSize> encoded) noexcept;

} // namespace hman

/// How the payload is stored.
enum class ManifestCodec : u8 {
    None = 0, ///< The body as is.
    Zstd = 1, ///< One or more zstd frames that decode to the body.
};

/// File tags (08 §2.6; R05-P2-25). Unknown bits are rejected.
enum class ManifestFileFlags : u16 {
    None = 0,
    Optional = 1 << 0,   ///< Optional content (an HD pack, an extra VO language): not installed by default.
    Vaulted = 1 << 1,    ///< Vaulted content: kept in the manifest, not installed or streamed.
    Executable = 1 << 2, ///< Gets the executable bit on POSIX installs.
};
HELIOS_ENUM_FLAGS(ManifestFileFlags)
inline constexpr u16 kManifestFileFlagsKnown = 0x7;

/// The header fields a manifest carries besides its tables.
struct ManifestHeader {
    std::string productId; ///< Whose build: ^[a-z][a-z0-9-]{2,31}$ (08 §2.10.1).
    std::string platform;  ///< The install platform: ^[a-z][a-z0-9_-]{1,31}$.
    std::string buildId;   ///< ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$; the CDN path segment (05 §7).
    u64 sequence = 0;      ///< Monotonic per product, channel and platform (anti-rollback, 05 §7).
    u64 createdAt = 0;     ///< Unix seconds.
    u64 expiresAt = 0;     ///< Unix seconds; 0 = never, else after createdAt. Checked by part 2's TrustChain.
    u32 compatEpoch = 0;   ///< The build's content compat epoch (05 §1.14.1).
    std::array<u8, hman::kKeyIdSize> keyId{};         ///< The keyset subkey that signs (part 2); zeros in v0.
    std::array<u8, hman::kSignatureSize> signature{}; ///< Ed25519 over bytes [0, 256) (part 2); zeros in v0.
    friend bool operator==(const ManifestHeader&, const ManifestHeader&) = default;
};

/// One installed file.
struct ManifestFile {
    std::string path; ///< Relative, '/'-separated, ASCII [A-Za-z0-9._+-] segments (README "Paths").
    u64 size = 0;
    Hash256 hash;     ///< BLAKE2b-256 of the whole file (checked before the staging rename, 08 §2.5).
    u32 firstRef = 0; ///< Its first entry in Manifest::refs; its refs are contiguous and in file order.
    u32 refCount = 0; ///< 0 exactly when size is 0.
    u64 group = 0;    ///< Tier-2 streaming group: the zone record hash (0 = none, 08 §2.6).
    u32 language = 0; ///< VO/text language code (0 = neutral).
    u8 tier = 1;      ///< 0 = launcher, client and login area; 1 = common; 2 = streamable (08 §2.6).
    ManifestFileFlags flags = ManifestFileFlags::None;
    friend bool operator==(const ManifestFile&, const ManifestFile&) = default;
};

/// One chunk of a file, in file order: offset is the sum of the previous chunks' sizes.
struct ManifestChunkRef {
    u32 chunk = 0; ///< Index into Manifest::chunks.
    u64 offset = 0;
    friend bool operator==(const ManifestChunkRef&, const ManifestChunkRef&) = default;
};

/// A unique chunk.
struct ManifestChunk {
    Hash256 hash;             ///< BLAKE2b-256 of the raw bytes: the chunk ID.
    u32 rawSize = 0;          ///< 1..fastcdc::kMaxSize.
    u32 storedSize = 0;       ///< Bytes of its zstd object on the CDN; 0 = not recorded (loose only).
    u32 pack = hman::kNoPack; ///< Index into Manifest::packs, or kNoPack for a loose chunk.
    u64 packOffset = 0;       ///< Offset of its stored bytes in the pack (0 when loose).
    friend bool operator==(const ManifestChunk&, const ManifestChunk&) = default;
};

/// A pack of small chunks (05 §7: chunks under 32 KiB grouped into ~8 MiB packs).
struct ManifestPack {
    Hash256 hash; ///< BLAKE2b-256 of the .pack object.
    u64 size = 0; ///< 1..kMaxPackSize.
    friend bool operator==(const ManifestPack&, const ManifestPack&) = default;
};

/// A zstd --patch-from delta that turns an older version of a file into this one (05 §7).
struct ManifestPatch {
    u32 file = 0;      ///< Index into Manifest::files (the "to" side; its hash is the target).
    Hash256 fromHash;  ///< BLAKE2b-256 of the older file; differs from the target's.
    Hash256 patchHash; ///< BLAKE2b-256 of the .zpatch object.
    u64 patchSize = 0; ///< 1..kMaxPatchSize.
    friend bool operator==(const ManifestPatch&, const ManifestPatch&) = default;
};

/// A decoded, canonical manifest. validateManifest() states the invariants; readManifest() only returns
/// valid ones, and writeManifest() refuses invalid ones.
struct Manifest {
    ManifestHeader header;
    std::vector<ManifestFile> files; ///< Sorted by path (byte order), unique ignoring ASCII case.
    std::vector<ManifestChunkRef> refs;
    std::vector<ManifestChunk> chunks;  ///< Sorted by hash, unique.
    std::vector<ManifestPack> packs;    ///< Sorted by hash, unique.
    std::vector<ManifestPatch> patches; ///< Sorted by (file, fromHash), unique.

    /// The chunk refs of `file` (which must be one of this manifest's files).
    std::span<const ManifestChunkRef> fileRefs(const ManifestFile& file) const noexcept;
    /// The file with exactly this path, or nullptr. O(log n).
    const ManifestFile* findFile(std::string_view path) const noexcept;
    /// The index of the chunk with this ID. O(log n).
    std::optional<u32> findChunk(const Hash256& hash) const noexcept;

    friend bool operator==(const Manifest&, const Manifest&) = default;
};

/// Checks every invariant of the format (README "Validation"): identifiers, paths, ordering, uniqueness,
/// counts and sizes within the limits, refs tiling each file, every chunk and pack referenced, packs
/// holding their chunks, patch targets. Fails with LimitExceeded for a limit and InvalidArgument otherwise.
Result<void> validateManifest(const Manifest& manifest);

/// True if `path` is a valid manifest path on its own (no ordering or collision checks): 1..1024 bytes of
/// '/'-separated segments of 1..255 bytes of [A-Za-z0-9._+-], none "." or "..", none ending in '.', none
/// a Windows device name (README "Paths"). Thread-safe.
bool isValidManifestPath(std::string_view path) noexcept;

/// The canonical body of a valid manifest (what bodyHash covers). InvalidArgument/LimitExceeded if invalid.
Result<std::vector<u8>> encodeManifestBody(const Manifest& manifest);

/// How writeManifest() stores the payload. The defaults match Go's zero WriteOptions (pkg/manifest): codec
/// None, and zstdLevel 0 means 19 when the codec is Zstd.
struct ManifestWriteOptions {
    ManifestCodec codec = ManifestCodec::None;
    int zstdLevel = 0; ///< 1..19, or 0 for 19 (levels up to 19 keep the window within kMaxZstdWindowLog).
};

/// A complete .hman file for a valid manifest. InvalidArgument for an unknown codec or a zstd level
/// outside 0..19; InvalidArgument/LimitExceeded for an invalid manifest. Thread-safe (a pure function).
Result<std::vector<u8>> writeManifest(const Manifest& manifest, const ManifestWriteOptions& options = {});

struct ManifestReadOptions {
    u64 maxBodySize =
        hman::kMaxBodySize; ///< Larger bodies fail with LimitExceeded; capped at, and 0 means, kMaxBodySize.
};

/// The validated fixed header of a .hman file, without decoding the payload.
struct ManifestHeaderInfo {
    ManifestHeader header;
    ManifestCodec codec = ManifestCodec::None;
    u64 bodySize = 0;
    u64 payloadSize = 0;
    Hash256 bodyHash;
    Hash256 headerHash; ///< The manifest's identity.
};

/// Validates the header of `file` (its first 352 bytes and the payload size): cheap enough to refuse a
/// wrong product, platform or sequence before decoding anything.
Result<ManifestHeaderInfo> readManifestHeader(std::span<const u8> file,
                                              const ManifestReadOptions& options = {});

/// Reads and validates a whole .hman file.
Result<Manifest> readManifest(std::span<const u8> file, const ManifestReadOptions& options = {});
/// readManifest() of the file at `path`, read through the platform layer. Files larger than
/// the header plus maxBodySize plus the zstd bound fail with LimitExceeded before they are read.
Result<Manifest> readManifestFile(const fs::Path& path, const ManifestReadOptions& options = {});

/// What a ManifestBuilder takes per file.
struct ManifestFileInput {
    std::string path;
    ChunkedFile content; ///< From splitBuffer/StreamChunker/chunkFile: size, hash and chunks in order.
    u8 tier = 1;
    ManifestFileFlags flags = ManifestFileFlags::None;
    u64 group = 0;
    u32 language = 0;
};

/// Builds a canonical Manifest from files in any order: sorts files by path, stores each distinct chunk
/// once (sorted by ID), lays out refs, and resolves pack placements and patches given by ID and path.
/// The same inputs give the same manifest in any order, in C++ and in Go (pkg/manifest.Builder).
/// Not thread-safe.
class ManifestBuilder {
public:
    explicit ManifestBuilder(ManifestHeader header) : m_header(std::move(header)) {}
    /// Adds a file. Fails (InvalidArgument) on an invalid path or tags, or chunks that do not tile
    /// `content.size`; a duplicate path fails in build().
    Result<void> addFile(ManifestFileInput file);
    /// Declares a pack object.
    void addPack(const Hash256& hash, u64 size);
    /// Records that `chunk` is stored in `pack` at `offset` (storedSize bytes); resolved in build().
    void placeChunk(const Hash256& chunk, const Hash256& pack, u64 offset, u32 storedSize);
    /// Records a loose chunk's stored (zstd) size; resolved in build().
    void setStoredSize(const Hash256& chunk, u32 storedSize);
    /// Adds a patch to the file at `path`; resolved in build().
    void addPatch(std::string path, const Hash256& fromHash, const Hash256& patchHash, u64 patchSize);
    /// The canonical manifest, validated. Fails on a duplicate path, a chunk ID seen with two sizes, a
    /// placement or patch naming an unknown chunk, pack or path, or anything validateManifest() refuses.
    Result<Manifest> build() const;

private:
    struct Placement {
        Hash256 chunk;
        Hash256 pack;
        u64 offset = 0;
        u32 storedSize = 0;
        bool packed = false;
    };
    struct PendingPatch {
        std::string path;
        Hash256 fromHash;
        Hash256 patchHash;
        u64 patchSize = 0;
    };
    ManifestHeader m_header;
    std::vector<ManifestFileInput> m_files;
    std::vector<ManifestPack> m_packs;
    std::vector<Placement> m_placements;
    std::vector<PendingPatch> m_patches;
};

} // namespace helios::patch
