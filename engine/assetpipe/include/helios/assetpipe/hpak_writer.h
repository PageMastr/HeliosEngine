#pragma once
// `.hpak` v0 writer (02 §6.3; `helios-pack`, 08, drives it later). It does what the format needs and
// no more: the deterministic blob order, 256 KiB asset blocks coded independently with zstd (or stored
// raw when zstd does not shrink the asset), 4 KiB blob alignment, the TOC sorted by AssetId, the
// XXH3-64 pak-block, TOC and header checksums, and the 2 GiB limit as a clear error.
//
// Determinism: the same assets with the same options give byte-identical paks whatever order they are
// added in, because blobs are ordered by HpakAssetOrder and then GUID, and zstd runs single-threaded
// at a fixed level. Identical output also needs the same zstd version (vendored and pinned, see
// third_party/MANIFEST.md).
//
// Threading: an HpakWriter is not thread-safe; use one per pak. The const members (size(), build(),
// emit(), writeFile()) may run concurrently with each other once no add() is in flight. The free
// functions are pure (resealHpak() mutates only its argument).

#include <functional>
#include <span>
#include <vector>

#include "helios/asset/asset_id.h"
#include "helios/asset/hpak_format.h"
#include "helios/core/fs.h"
#include "helios/core/guid.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::assetpipe {

/// The ordering key of 02 §6.3: tier → group (zone) → language → recorded first use → GUID. A stable
/// order keeps 05's FastCDC chunking effective across builds (AAA-CNT-7).
struct HpakAssetOrder {
    static constexpr u32 kNotRecorded = 0xFFFFFFFFu;
    u8 tier = 0;
    u64 group = 0;
    u32 language = 0;
    u32 firstUse = kNotRecorded; ///< Position in the recorded first-use trace; unrecorded assets go last.
    friend bool operator==(const HpakAssetOrder&, const HpakAssetOrder&) = default;
};

struct HpakWriterOptions {
    asset::HpakPlatform platform = asset::HpakPlatform::PcClient;
    u64 contentBuild = 0;
    asset::HpakTags tags;
    /// zstd level, 1..19 (dev cooks fast, release cooks high; 02 §6.2 "Encoders").
    i32 zstdLevel = 3;
};

/// Byte layout of a pak whose blobs have the given stored sizes, in blob order.
struct HpakLayout {
    std::vector<u64> blobOffsets; ///< File offset of each blob.
    u64 dataEnd = 0;              ///< End of the blob region = the TOC offset.
    u64 tocSize = 0;
    u64 fileSize = 0;
    u32 pakBlockCount = 0;
};

/// Lays out blobs of `blobSizes` stored bytes (in blob order) and a TOC for `blobSizes.size()` assets
/// with `assetBlocks` asset blocks in all. LimitExceeded when the pak would pass 2 GiB. The writer
/// lays out every pak with it; it is public so the limit can be tested without writing 2 GiB.
Result<HpakLayout> planHpakLayout(std::span<const u64> blobSizes, u64 assetBlocks);

class HpakWriter {
public:
    /// InvalidArgument for an unknown platform, a tier above 2 or a zstd level outside 1..19.
    static Result<HpakWriter> create(const HpakWriterOptions& options = {});

    /// Codes `cooked` (copied) as the asset `guid`. Errors (the writer is unchanged by a failed add):
    /// the AssetIdSet errors (nil GUID, a GUID already added, an AssetId collision; 02 §6.1),
    /// InvalidArgument for a tier above 2, LimitExceeded for an asset above 2 GiB or one that would
    /// take the pak past 2 GiB.
    Result<asset::AssetId> add(const Guid& guid, std::span<const u8> cooked, const HpakAssetOrder& order = {});

    usize assetCount() const noexcept { return m_assets.size(); }
    /// The size of the pak build() would produce now.
    u64 size() const noexcept { return m_size; }

    /// Streams the pak to `sink` in order (header, blobs, TOC); stops at the sink's first error.
    Result<void> emit(const std::function<Result<void>(std::span<const u8>)>& sink) const;
    /// The pak in memory.
    Result<std::vector<u8>> build() const;
    /// Writes the pak to `path` through a sibling temp file that is flushed and renamed over it.
    Result<void> writeFile(const fs::Path& path) const;

private:
    struct Asset {
        Guid guid;
        asset::AssetId id;
        HpakAssetOrder order;
        Hash128 cookedHash;
        u64 rawSize = 0;
        asset::HpakCodec codec = asset::HpakCodec::None;
        std::vector<u32> blockSizes;
        std::vector<u8> stored;
    };
    struct Plan {
        std::vector<const Asset*> blobOrder; ///< Assets in blob order.
        std::vector<const Asset*> tocOrder;  ///< Assets by AssetId.
        HpakLayout layout;
        std::vector<u8> header; ///< kHeaderBlockSize bytes.
        std::vector<u8> toc;
    };

    explicit HpakWriter(const HpakWriterOptions& options) : m_options(options) {}
    Result<Plan> plan() const;

    HpakWriterOptions m_options;
    asset::AssetIdSet m_ids;
    std::vector<Asset> m_assets;
    u64 m_blobBytes = 0;  ///< Σ alignUp(stored, 4 KiB).
    u64 m_assetBlocks = 0;
    u64 m_size = asset::hpak::kHeaderBlockSize;
};

/// Recomputes, in place, every checksum of an in-memory pak that its header lets it locate: the pak
/// block hashes, the TOC hash and the header hash. Fields are not validated and out-of-range ones are
/// left alone. For tests and fuzzing, so that a corrupted field reaches the reader's field checks
/// instead of failing the checksum in front of them.
void resealHpak(std::span<u8> pak) noexcept;

} // namespace helios::assetpipe
