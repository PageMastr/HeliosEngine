#pragma once
// Derived-data cache v0 (02 §6.2, 07 §3.2): the DDC key, the entry format and the local store.
//
// **Key.** 02 §6.2: `XXH3-128(builder, version, sourceHash, settingsHash, platform, layout hashes,
// dependency hashes)`, plus the cooker version. makeDdcKey() hashes this preimage (little-endian):
//
//   [0, 4)    magic 'HDDK'                 [24, 40)  sourceHash      XXH3-128 of the source bytes
//   [4, 8)    key format 1                 [40, 56)  settingsHash    XXH3-128 of the canonical settings
//   [8, 12)   builderVersion               [56, 72)  settingsType    hashSettingsType() of their type
//   [12, 16)  cookerVersion                [72, 80)  n, the builder id's length
//   [16, 20)  platform (HpakPlatform)      [80, 88)  d, the dependency count
//   [20, 24)  reserved, 0                  [88, 88+n) builder id, then d dependency keys (16 bytes each,
//                                                     in the builder's order). Hash128s: low half first.
//
// The canonical settings omit values at their default, and settingsType records the defaults (and every
// field's name, id and type), so the two together pin the settings a build step resolves. Format 0 hashed
// the type's layoutHash instead, which has no defaults (and, for StructBuilder types, no field types).
// Paths, file times, the order settings were written in, and the asset's identity (GUID, labels,
// provenance) are not inputs, so moving, touching or re-saving a source with the same bytes and settings
// keeps its key (resolveSettings() canonicalizes), and the build step sees none of them (BuildContext).
//
// **Entry.** One file per key, `<root>/<k0k1>/<32 hex digits>.hddc` (the key's toHex(); k0k1 its first
// two digits), a 64-byte little-endian header and the payload:
//
//   [0, 4)    magic 'HDDC'                 [32, 48)  payloadHash XXH3-128 of the payload
//   [4, 6)    entry version 0              [48, 52)  flags, 0
//   [6, 8)    header size 64               [52, 56)  reserved, 0
//   [8, 24)   key                          [56, 64)  headerHash XXH3-64 of [0, 56)
//   [24, 32)  payloadSize (≤ 2 GiB)        [64, 64 + payloadSize)  payload; nothing after it
//
// A reader trusts nothing: every field is checked, the file must be exactly 64 + payloadSize bytes, the
// key must be the one asked for (a renamed or copied file is not a hit) and the payload must match its
// hash, so a damaged entry is a miss and never bad data. XXH3 detects corruption, not tampering: the local
// store is the user's own cache, and the shared tier (WP-2.1) distributes by BLAKE2b (02 §6.2, 05 §7).
//
// Threading: the free functions are pure. LocalDdc is thread-safe, and several processes may share one
// root (see LocalDdc).

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "helios/asset/hpak_format.h"
#include "helios/core/fs.h"
#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::assetpipe {

/// The cook pipeline's own version, in every key: bump it when the cooker (not an importer) changes what
/// it stores for the same inputs.
inline constexpr u32 kCookerVersion = 1;

struct DdcKeyInputs {
    std::string_view builder; ///< Importer or builder id.
    u32 builderVersion = 0;
    Hash128 sourceHash;               ///< XXH3-128 of the source bytes (hashSourceFile()).
    std::string_view settings = "{}"; ///< Canonical settings (resolveSettings()).
    /// hashSettingsType() of the settings' type (ImporterRegistry::settingsTypeHash); zero without settings.
    Hash128 settingsType;
    asset::HpakPlatform platform = asset::HpakPlatform::PcClient; ///< The consumer (02 §6.5).
    u32 cookerVersion = kCookerVersion;
    std::span<const Hash128> dependencies; ///< Dependency keys, in the builder's order.
};

/// The key of the layout above. Pure.
Hash128 makeDdcKey(const DdcKeyInputs& inputs) noexcept;

/// XXH3-128 of a file's bytes, streamed in 1 MiB reads through fs::File.
Result<Hash128> hashSourceFile(const fs::Path& path);

namespace ddc {

inline constexpr u32 kEntryMagic = 0x43444448u; ///< "HDDC" as little-endian bytes.
inline constexpr u16 kEntryVersion = 0;
inline constexpr usize kEntryHeaderBytes = 64;
inline constexpr usize kEntryHeaderHashedBytes = 56;
inline constexpr u64 kMaxPayload = 2 * kGiB;
inline constexpr std::string_view kEntryExtension = ".hddc";

struct EntryHeader {
    Hash128 key;
    u64 payloadSize = 0;
    Hash128 payloadHash;
};

/// The entry for `payload` under `key` (header + payload). LimitExceeded above kMaxPayload.
Result<std::vector<u8>> encodeEntry(const Hash128& key, std::span<const u8> payload);
/// The 64 header bytes for this key and payload.
void encodeEntryHeader(const Hash128& key, std::span<const u8> payload,
                       std::span<u8, kEntryHeaderBytes> out) noexcept;

/// Validates the first kEntryHeaderBytes of `bytes` (bounds checked: fewer bytes is EndOfFile): magic
/// and header size (Corrupt), version (VersionMismatch), header hash, flags and reserved bytes (Corrupt),
/// payload size (LimitExceeded) and the key (Corrupt unless it is `expectedKey`).
Result<EntryHeader> readEntryHeader(std::span<const u8> bytes, const Hash128& expectedKey);
/// Checks a payload against its validated header: exact size (EndOfFile when short, Corrupt when long)
/// and hash (Corrupt).
Result<void> checkEntryPayload(const EntryHeader& header, std::span<const u8> payload);
/// A whole entry in memory: readEntryHeader, then exactly payloadSize bytes after the header that match
/// the hash. Returns the payload as a view into `entry`.
Result<std::span<const u8>> readEntry(std::span<const u8> entry, const Hash128& expectedKey);

} // namespace ddc

struct LocalDdcOptions {
    fs::Path
        root; ///< Required. The plan's default is `%LOCALAPPDATA%\Helios\DDC` (02 §6.2); the caller chooses.
    /// Size cap of the store (02 §6.2 / 07 §3.2: 200 GB default). When this process's running total passes
    /// it, put() trims to trimTargetPercent of it (see LocalDdc's eviction for the exception).
    u64 capBytes = 200ull * 1000 * 1000 * 1000;
    u32 trimTargetPercent = 90;
    /// LRU granularity: a hit refreshes the entry's last write time when it is older than this, so a hot
    /// entry costs at most one metadata write per interval (0: every hit).
    std::chrono::seconds touchInterval{std::chrono::hours(1)};
    /// trim() removes temp files of writers that crashed once they are older than this.
    std::chrono::seconds staleTempAge{std::chrono::hours(1)};
};

struct DdcStats {
    u64 hits = 0;
    u64 misses = 0; ///< No entry.
    u64 bad = 0;    ///< Entries that failed verification (also misses for the caller).
    u64 puts = 0;
    u64 evicted = 0; ///< Entries removed by trim().
    u64 evictedBytes = 0;
    u64 trims = 0; ///< Store listings by trim(), explicit or from put().
};

struct TrimResult {
    u64 entries = 0; ///< Entries left.
    u64 bytes = 0;   ///< Their size on disk.
    u64 evicted = 0;
    u64 evictedBytes = 0;
    u64 staleTemps = 0; ///< Crashed writers' temp files removed.
};

/// The local DDC tier (07 §3.2): content addressed by key, LRU eviction under a size cap.
///
/// - **Atomic put.** The entry is written to a uniquely named temp file next to its final name and renamed
///   over it (fs::renameNoSync), so readers see the old entry, the new one or none, never a partial one.
///   Writers of the same key race harmlessly: each renames a complete entry, the last one wins, and a
///   writer whose rename fails succeeds anyway when a valid entry for the key is already in place. Neither
///   the entry nor the rename is flushed to disk (it is a cache, and a put should not wait for the file
///   system's journal): a crash can lose an entry or leave a truncated one, which the verified read turns
///   into a miss and the next put replaces.
/// - **Verified get.** See the entry format: a damaged, truncated, renamed or foreign file is a miss. A
///   name that is not a regular file (a directory, a FIFO, a device) is a damaged entry and is never
///   opened, so get() cannot block on it.
/// - **Eviction.** LRU by the entry file's last write time: put() sets it, and a hit refreshes it when it
///   is older than touchInterval. trim() lists the store, removes stale temp files, and deletes the least
///   recently used entries until the total is at most trimTargetPercent of the cap. It only ever deletes
///   files named like entries (or their temps) inside the two-hex-digit directories, so a mistaken root
///   loses nothing else. open() and trim() measure the store; put() adds to the running total, and trim()
///   corrects it by what it measured and evicted (keeping the puts that finished while it listed). put()
///   trims when the running total passes the cap, except after a trim that could not get down to its
///   target (entries that could not be deleted, e.g. held open on Windows): then only once the total
///   passes what that trim left plus the trim headroom (the cap minus the target, at least 1 % of the
///   cap), so such a store is not re-listed on every put. Several processes may share a root: each trims
///   by its own running total, re-measured at every trim, so the store can pass the cap by what the
///   others wrote since.
/// - **Thread safety.** Every member is thread-safe. Concurrent trims in one process are serialized; a
///   reader whose entry is evicted under it either finishes reading (the file stays readable while open)
///   or misses.
class LocalDdc {
public:
    /// Creates `root` if needed and measures the store (O(entries)). InvalidArgument for an empty root,
    /// a cap of 0 or a trim target outside 1..100.
    static Result<std::unique_ptr<LocalDdc>> open(const LocalDdcOptions& options);

    /// The payload stored under `key`. NotFound: no entry. Corrupt, EndOfFile, VersionMismatch or
    /// LimitExceeded: a damaged entry (a miss: never data). IoError: the file could not be read.
    Result<std::vector<u8>> get(const Hash128& key);
    /// Stores `payload` under `key` (replacing any entry). LimitExceeded above ddc::kMaxPayload.
    Result<void> put(const Hash128& key, std::span<const u8> payload);
    /// Evicts as described above; also run by put() when the running total passes the trim threshold.
    Result<TrimResult> trim();

    /// The entry's file (whether or not it exists).
    fs::Path entryPath(const Hash128& key) const;
    /// This process's running total of the store's size.
    u64 sizeBytes() const noexcept { return m_bytes.load(std::memory_order_relaxed); }
    DdcStats stats() const noexcept;
    const LocalDdcOptions& options() const noexcept { return m_options; }

private:
    explicit LocalDdc(LocalDdcOptions options)
        : m_options(std::move(options)), m_trimAt(m_options.capBytes) {}
    /// put()'s trim: skipped when another thread's trim already brought the total under the threshold.
    void trimIfOver();
    Result<TrimResult> trimLocked();

    LocalDdcOptions m_options;
    std::mutex m_trimMutex;
    std::atomic<u64> m_bytes{0};
    std::atomic<u64> m_trimAt; ///< put() trims when m_bytes passes it: the cap, or more after a short trim.
    std::atomic<u64> m_hits{0}, m_misses{0}, m_bad{0}, m_puts{0}, m_evicted{0}, m_evictedBytes{0}, m_trims{0};
};

} // namespace helios::assetpipe
