#pragma once
// The `.hpak` v0 on-disk format (02 §6.3, ADR-006): constants, the decoded header and TOC entry, and
// their little-endian encoders. The reader (hpak_reader.h) validates; these functions only translate
// bytes, so the writer (engine/assetpipe) and the reader cannot disagree on a field offset.
//
//   [0, 4096)              header block: the fields below, then zeros
//   [4096, tocOffset)      blobs: one per asset, each 4 KiB aligned and zero padded to 4 KiB; an
//                          asset is a run of independently coded 256 KiB blocks (one for small assets)
//   [tocOffset, fileSize)  TOC: entries[assetCount] sorted by AssetId, u32 blockSizes[assetBlockCount]
//                          (compressed size of each asset block, in entry order), u64 blockHashes
//                          [pakBlockCount] (XXH3-64 of each 64 KiB pak block of the blob region)
//
// Header fields (little-endian, byte offsets):
//   0 magic 'HPAK' u32 · 4 version u16 · 6 reserved u16 · 8 platform u32 · 12 flags u32
//   16 contentBuild u64 · 24 group u64 · 32 language u32 · 36 tier u8 · 37 reserved u8[3]
//   40 tocOffset u64 · 48 tocSize u64 · 56 assetCount u32 · 60 assetBlockCount u32
//   64 pakBlockCount u32 · 68 reserved u32 · 72 tocHash u64 (XXH3-64 of the TOC)
//   80 headerHash u64 (XXH3-64 of bytes [0, 80))
// TOC entry (64 bytes):
//   0 assetId u64 · 8 cookedHash XXH3-128 (low u64, high u64) · 24 offset u64 · 32 compSize u64
//   40 rawSize u64 · 48 firstBlock u32 · 52 blockCount u32 · 56 codec u8 · 57 reserved u8[7]
//
// Threading: everything here is a pure function or a constant.

#include <span>
#include <string_view>

#include "helios/asset/asset_id.h"
#include "helios/core/hash.h"
#include "helios/core/types.h"

namespace helios::asset {

/// Cook platform a pak was built for (02 §6.2). 0 is not a platform, so a zeroed header is rejected.
enum class HpakPlatform : u32 {
    PcClient = 1, ///< `pc-client` (identical on Windows and Linux).
    Server = 2,   ///< `server` (02 §6.5).
    Editor = 3,   ///< `editor`.
};

/// Stable lower-case name ("pc-client"), or "unknown".
std::string_view hpakPlatformName(HpakPlatform platform) noexcept;

/// How an asset's blocks are coded. One codec per asset: the writer stores an asset raw when zstd
/// would not make it smaller.
enum class HpakCodec : u8 {
    None = 0, ///< Each block is the raw bytes.
    Zstd = 1, ///< Each block is one zstd frame.
};

/// The pak's install classification (08 §2.6): tier 0 (launcher, client, login area), 1 (common) or
/// 2 (per zone); group = the zone record hash (0 = none); language = a cook-defined code (0 = neutral).
struct HpakTags {
    u8 tier = 0;
    u64 group = 0;
    u32 language = 0;
    friend bool operator==(const HpakTags&, const HpakTags&) = default;
};

/// One TOC entry: where an asset's blob is and how to decode it.
struct HpakEntry {
    AssetId id;
    Hash128 cookedHash; ///< XXH3-128 of the decoded (cooked) bytes.
    u64 offset = 0;     ///< File offset of the blob (4 KiB aligned).
    u64 compSize = 0;   ///< Stored bytes (the sum of the block sizes).
    u64 rawSize = 0;    ///< Decoded bytes.
    u32 firstBlock = 0; ///< Index of the first block size in the TOC's block-size table.
    u32 blockCount = 0; ///< ceil(rawSize / 256 KiB).
    HpakCodec codec = HpakCodec::None;
    friend bool operator==(const HpakEntry&, const HpakEntry&) = default;
};

namespace hpak {

inline constexpr u32 kMagic = 0x4B415048u;          ///< "HPAK" as little-endian bytes.
inline constexpr u16 kVersion = 0;                  ///< The v0 format (WP-0.8).
inline constexpr u64 kHeaderBlockSize = 4 * kKiB;   ///< The header block; blobs start right after it.
inline constexpr usize kHeaderBytes = 88;           ///< Encoded header fields (the rest of the block is 0).
inline constexpr usize kHeaderHashedBytes = 80;     ///< headerHash covers [0, 80).
inline constexpr usize kTocHashOffset = 72;         ///< Header offset of tocHash.
inline constexpr usize kHeaderHashOffset = 80;      ///< Header offset of headerHash.
inline constexpr u64 kBlobAlignment = 4 * kKiB;     ///< Every blob and the TOC start 4 KiB aligned.
inline constexpr u64 kAssetBlockSize = 256 * kKiB;  ///< Raw bytes per independently coded asset block.
inline constexpr u64 kPakBlockSize = 64 * kKiB;     ///< Bytes per checksummed pak block.
inline constexpr u64 kMaxPakSize = 2 * kGiB;        ///< 02 §6.3: a pak is at most 2 GiB.
inline constexpr u64 kMaxAssetSize = 2 * kGiB;      ///< Decoded size cap per asset (v0 choice).
inline constexpr usize kTocEntryBytes = 64;
inline constexpr usize kBlockSizeBytes = 4;
inline constexpr usize kBlockHashBytes = 8;
inline constexpr u8 kMaxTier = 2;

/// Decoded header fields, without validation.
struct Header {
    u32 magic = kMagic;
    u16 version = kVersion;
    u16 reserved0 = 0;
    u32 platform = 0; ///< HpakPlatform value; kept raw so a hostile value can be reported.
    u32 flags = 0;
    u64 contentBuild = 0;
    HpakTags tags;
    u8 reserved1[3] = {};
    u64 tocOffset = 0;
    u64 tocSize = 0;
    u32 assetCount = 0;
    u32 assetBlockCount = 0;
    u32 pakBlockCount = 0;
    u32 reserved2 = 0;
    u64 tocHash = 0;
    u64 headerHash = 0;
};

/// Writes the header fields to `out` (headerHash as given; see computeHeaderHash()).
void encodeHeader(const Header& header, std::span<u8, kHeaderBytes> out) noexcept;
/// Reads the header fields from `in`; no validation.
Header decodeHeader(std::span<const u8, kHeaderBytes> in) noexcept;
/// XXH3-64 of the encoded header's first kHeaderHashedBytes bytes.
u64 computeHeaderHash(std::span<const u8, kHeaderBytes> encoded) noexcept;

/// A TOC entry as stored, with the codec and reserved bytes kept raw for validation.
struct RawEntry {
    HpakEntry entry;
    u8 codec = 0;
    u8 reserved[7] = {};
};

void encodeEntry(const HpakEntry& entry, std::span<u8, kTocEntryBytes> out) noexcept;
RawEntry decodeEntry(std::span<const u8, kTocEntryBytes> in) noexcept;

/// Number of 256 KiB asset blocks an asset of `rawSize` bytes has (0 for an empty asset).
constexpr u64 assetBlockCount(u64 rawSize) noexcept {
    return rawSize / kAssetBlockSize + (rawSize % kAssetBlockSize != 0 ? 1 : 0);
}
/// Number of 64 KiB pak blocks covering a blob region of `dataBytes` bytes.
constexpr u64 pakBlockCount(u64 dataBytes) noexcept {
    return dataBytes / kPakBlockSize + (dataBytes % kPakBlockSize != 0 ? 1 : 0);
}
/// TOC size for these counts.
constexpr u64 tocSize(u64 assetCount, u64 assetBlocks, u64 pakBlocks) noexcept {
    return assetCount * kTocEntryBytes + assetBlocks * kBlockSizeBytes + pakBlocks * kBlockHashBytes;
}

} // namespace hpak
} // namespace helios::asset
