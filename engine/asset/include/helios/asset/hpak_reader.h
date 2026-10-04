#pragma once
// `.hpak` v0 reader (02 §6.3): opens one pak, validates its header and TOC, and decodes assets with
// every 64 KiB pak block verified against its XXH3-64 checksum on the block's first read.
//
// Hostile input. open() checks the header and TOC checksums and then every field: offsets, sizes,
// counts, codecs and reserved bytes are bounded by the file and by each other, so a read can only
// touch bytes inside the blob region. A read allocates one asset block of scratch plus the output,
// which read() grows only as blocks decode: an asset's rawSize is the TOC's claim, so a garbage block
// fails before the claim costs memory. Each zstd block must be exactly one frame that states its
// decoded size. Anything else fails with a Result error (Corrupt, VersionMismatch, Unsupported,
// LimitExceeded, EndOfFile or IoError), never UB. Decoded assets are checked against the TOC's
// XXH3-128 cookedHash. XXH3 detects corruption, not tampering: distribution integrity is BLAKE2b's
// job (05 §7, 08 §2.5).
//
// Bad blocks. A pak block that fails its checksum calls the IBlockRefetcher (08 §2.6: the
// StreamingInstaller's `demand`; a later WP connects it), at most once per block until
// retryBlocks() resets it, and then fails or re-reads according to what the hook returns.
//
// Threading: an open HpakReader is immutable except for its per-block verification state, which
// is atomic. read(), readInto(), verifyAll(), find(), entries(), info() and blockState() are safe
// to call concurrently from any threads; retryBlocks() too. The refetch hook runs on the reading
// thread with this reader's repair lock held (so it is serialized per pak); it must not read from or
// retry this same reader.

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "helios/asset/asset_id.h"
#include "helios/asset/hpak_format.h"
#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::asset {

/// Where a pak's bytes come from. Implementations must make size() and readAt() safe to call
/// concurrently from several threads.
class IHpakSource {
public:
    virtual ~IHpakSource() = default;
    /// Total size in bytes; constant for the source's lifetime.
    virtual u64 size() const = 0;
    /// Fills `out` from `offset`; fails (EndOfFile or IoError) unless every byte was read.
    virtual Result<void> readAt(u64 offset, std::span<u8> out) const = 0;
    /// Name for logs and errors (a path, or "memory:<name>").
    virtual std::string describe() const = 0;
};

/// A pak file opened read-only through the platform layer (fs::File positional reads).
/// Thread-safe as IHpakSource requires.
Result<std::unique_ptr<IHpakSource>> openHpakFile(const fs::Path& path);
/// A pak held in memory (tests, fuzzing, downloaded paks). Thread-safe as IHpakSource requires.
std::unique_ptr<IHpakSource> makeHpakMemorySource(std::vector<u8> bytes, std::string name = "memory");

class HpakReader;

/// A pak block that failed its checksum.
struct HpakBadBlock {
    const HpakReader* pak = nullptr;
    u32 index = 0;    ///< Pak block index.
    u64 offset = 0;   ///< File offset of the block.
    u64 size = 0;     ///< Block size (the last block of the blob region may be short).
    u64 expected = 0; ///< XXH3-64 from the TOC.
    u64 actual = 0;   ///< XXH3-64 of the bytes read.
};

/// What a refetch hook did about a bad block.
enum class RefetchStatus : u8 {
    Repaired, ///< The bytes were replaced before returning: the reader re-reads and re-verifies once.
    Pending,  ///< A re-fetch was queued: reads of the block fail with Busy until retryBlocks().
    Failed,   ///< Nothing can be done: reads of the block fail with Corrupt until retryBlocks().
};

/// Re-fetch hook for bad pak blocks (02 §6.3 "Integrity"; 08 §2.6). Called on the reading thread
/// with the reader's repair lock held, at most once per block until retryBlocks() covers it.
class IBlockRefetcher {
public:
    virtual ~IBlockRefetcher() = default;
    /// Handles `block`, which failed its checksum: replace its bytes in the pak's source and return
    /// Repaired, queue a re-fetch and return Pending, or return Failed. Runs on the reading thread
    /// under `block.pak`'s repair lock, so calls for one pak are serialized; it must not read from or
    /// call retryBlocks() on that pak (other paks are fine).
    virtual RefetchStatus refetch(const HpakBadBlock& block) = 0;
};

/// Verification state of one pak block.
enum class HpakBlockState : u8 {
    Unverified = 0, ///< Not read yet (or reset by retryBlocks()).
    Verified = 1,   ///< Matched its checksum; never re-hashed.
    Pending = 2,    ///< Failed; the hook queued a re-fetch.
    Bad = 3,        ///< Failed; no repair.
};

struct HpakOpenOptions {
    /// When set, a pak cooked for another platform fails with Unsupported.
    std::optional<HpakPlatform> platform;
    /// Bad-block hook; null means every bad block fails with Corrupt.
    std::shared_ptr<IBlockRefetcher> refetcher;
    /// read() and readInto() of an asset whose decoded size is above this fail with LimitExceeded, so a
    /// runtime consumer can bound what one read may allocate. Capped at hpak::kMaxAssetSize.
    u64 maxAssetSize = hpak::kMaxAssetSize;
};

/// Header fields of an open pak.
struct HpakInfo {
    HpakPlatform platform = HpakPlatform::PcClient;
    u64 contentBuild = 0;
    HpakTags tags;
    u64 fileSize = 0;
    u64 dataEnd = 0; ///< End of the blob region (= the TOC offset).
    u32 assetCount = 0;
    u32 pakBlockCount = 0;
};

class HpakReader {
public:
    /// Reads and validates the header and the TOC (one read each). Blob bytes are not read.
    static Result<std::shared_ptr<HpakReader>> open(std::unique_ptr<IHpakSource> source,
                                                    const HpakOpenOptions& options = {});
    /// openHpakFile() + open().
    static Result<std::shared_ptr<HpakReader>> openFile(const fs::Path& path,
                                                        const HpakOpenOptions& options = {});

    HpakReader(const HpakReader&) = delete;
    HpakReader& operator=(const HpakReader&) = delete;
    ~HpakReader();

    const HpakInfo& info() const noexcept { return m_info; }
    /// The TOC, sorted by id (strictly ascending).
    std::span<const HpakEntry> entries() const noexcept { return m_entries; }
    /// The entry of `id` (binary search), or nullptr.
    const HpakEntry* find(AssetId id) const noexcept;
    /// The source's description.
    const std::string& name() const noexcept { return m_name; }

    /// Decodes the asset `id`. NotFound when the pak has no such asset.
    Result<std::vector<u8>> read(AssetId id) const;
    /// Decodes `entry`, which must be one of entries() (InvalidArgument otherwise). The result grows
    /// as blocks decode (past a 1 MiB start, to at most twice the bytes decoded), so a hostile rawSize
    /// costs only what really decodes. LimitExceeded above HpakOpenOptions::maxAssetSize.
    Result<std::vector<u8>> read(const HpakEntry& entry) const;
    /// read(entry) into `out`, replacing its contents and reusing its capacity (a loader's buffer).
    /// On failure `out` holds unspecified bytes; its capacity grew only for blocks that decoded.
    Result<void> read(const HpakEntry& entry, std::vector<u8>& out) const;
    /// Decodes `entry` into `out`, whose size must equal entry.rawSize (InvalidArgument otherwise).
    /// LimitExceeded above HpakOpenOptions::maxAssetSize. On failure `out` holds unspecified bytes.
    Result<void> readInto(const HpakEntry& entry, std::span<u8> out) const;

    /// Verifies every pak block not yet verified (the launcher's full check; tests). Stops at the
    /// first block that stays bad and returns its error.
    Result<void> verifyAll() const;
    /// Resets the Pending and Bad blocks overlapping [offset, offset + size) to Unverified, so their
    /// next read verifies again (and may call the hook again). Call it once a re-fetch has landed.
    /// Const like every other member: it changes only the atomic block states, under the repair lock,
    /// so an installer can call it through the `const HpakReader` that mounts and hooks hand out.
    void retryBlocks(u64 offset, u64 size) const;
    /// State of pak block `index` (Unverified for an index out of range).
    HpakBlockState blockState(u32 index) const noexcept;

private:
    HpakReader() = default;
    Result<void> parse(const HpakOpenOptions& options);
    bool owns(const HpakEntry& entry) const noexcept;
    /// InvalidArgument for a foreign entry, LimitExceeded above m_maxAssetSize.
    Result<void> checkReadable(const HpakEntry& entry) const;
    /// Decodes `entry` into `out` (exactly rawSize bytes) or, when `grow` is set, into `*grow`, which
    /// it extends block by block.
    Result<void> decode(const HpakEntry& entry, std::span<u8> out, std::vector<u8>* grow) const;
    /// Bytes [start, start + size) of the blob region with every pak block they touch verified.
    /// `scratch` holds the bytes; the returned span points into it.
    Result<std::span<const u8>> fetch(u64 start, u64 size, std::vector<u8>& scratch) const;
    /// Verifies pak block `index`, whose bytes the caller just read into `bytes` (re-read in place
    /// when the hook repairs the block). `readAfterVerified`: the block was already Verified when
    /// the caller read it, so the bytes are trusted unhashed; otherwise they are hashed even if
    /// another reader has verified the block since.
    Result<void> verifyBlock(u32 index, std::span<u8> bytes, bool readAfterVerified) const;
    u64 blockOffset(u32 index) const noexcept {
        return hpak::kHeaderBlockSize + u64(index) * hpak::kPakBlockSize;
    }

    std::unique_ptr<IHpakSource> m_source;
    std::string m_name;
    HpakInfo m_info;
    std::vector<HpakEntry> m_entries;
    std::vector<u32> m_blockSizes;
    std::vector<u64> m_blockHashes;
    std::unique_ptr<std::atomic<u8>[]> m_blockStates;
    std::shared_ptr<IBlockRefetcher> m_refetcher;
    u64 m_maxAssetSize = hpak::kMaxAssetSize;
    mutable std::mutex m_repairMutex;
};

} // namespace helios::asset
