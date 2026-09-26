#pragma once
// Runtime EntityId minting from time-prefixed ID blocks (ADR-004, 02 §4.1, 05 §1.4.5).
//
// Layout of a runtime EntityId. It is the same as Go's services/pkg/idgen, and both sides check the
// golden vectors in services/testdata/vectors/block_ids.json:
//
//   | 63 | 62 ................. 22 | 21 ..... 17 | 16 .......... 0 |
//   | 0  | 41-bit block prefix      | 5-bit shard | 17-bit offset   |
//   |    | (ms since 2026-01-01 UTC |             | inside the      |
//   |    |  when it was allocated)  |             | block           |
//
// Prefixes come from AllocateIdBlocks (05 §1.4.5). One PostgreSQL row per shard advances to
// max(last + n, now_ms) and hands out the n prefixes ending there, so prefixes strictly increase
// whichever process or replica asks, and no block is handed out twice. There are no node ids to
// lease, so ids minted by C++ cells and by Go services (ledger, market, ...) share one
// collision-free space. (The older 41/5/8/9 Snowflake layout was rejected by 05 §1.4.5: 256 nodes
// per shard are too few, and a 9-bit sequence can stall a volley.)
//
// EntityIdMinter holds the current block plus spares (2 blocks by default, 4 for battle-profile
// cells). Minting increments the offset: lock-free, one CAS per id, and no stall. maintain(), which
// World::beginTick() calls, asks the IdBlockSource for more blocks once the current block is half
// used. allocate() only calls the source itself when every held block is spent. A block retires
// `retireMs` (1 h) after it was allocated, but only once a fresher block is held; this keeps ids
// within an hour of wall-clock order. With no fresher block (control plane down) the minter keeps
// minting from retired blocks rather than stall.
//
// Sources:
//   * the cell's orchestrator client (AllocateIdBlocks over the control plane). An asynchronous
//     source returns ErrorCode::Busy and later delivers the prefixes with EntityIdMinter::addBlocks();
//   * LocalIdBlockSource, the same allocation rule in-process, for tests, tools, offline editor
//     worlds and deterministic replays. Drive its clock with simulated time and the id sequence is a
//     pure function of the spawn order, which 04's bit-exact replay relies on.
//
// Threading: allocate() is lock-free and may be called from any thread. maintain(), addBlocks(),
// prime(), remaining() and stats() take an internal mutex. A source must not call back into the
// minter from inside allocateIdBlocks(). LocalIdBlockSource is thread-safe.

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

struct BlockIdLayout {
    static constexpr u32 kPrefixBits = 41;
    static constexpr u32 kShardBits = 5;
    static constexpr u32 kOffsetBits = 17;
    static constexpr u32 kShardShift = kOffsetBits;
    static constexpr u32 kPrefixShift = kOffsetBits + kShardBits;
    /// Ids per block (2^17 = 131,072).
    static constexpr u32 kBlockSize = 1u << kOffsetBits;
    static constexpr u64 kMaxPrefix = (1ull << kPrefixBits) - 1;
    static constexpr u32 kMaxShard = (1u << kShardBits) - 1;
    /// AllocateIdBlocks hands out at most 16 blocks per call (05 §1.4.5).
    static constexpr u32 kMaxBlocksPerCall = 16;
    /// 2026-01-01T00:00:00Z in Unix milliseconds (prefix 0).
    static constexpr i64 kEpochUnixMs = 1767225600000;
};
static_assert(BlockIdLayout::kPrefixShift + BlockIdLayout::kPrefixBits == 63);

struct BlockIdParts {
    u64 prefix = 0; ///< Block prefix: ms since BlockIdLayout::kEpochUnixMs at allocation.
    u32 shard = 0;
    u32 offset = 0;
    friend constexpr bool operator==(const BlockIdParts&, const BlockIdParts&) noexcept = default;
};

/// Packs a runtime EntityId. Out-of-range fields are truncated to their width, as in Go's
/// idgen.Compose.
constexpr EntityId composeBlockId(u64 prefix, u32 shard, u32 offset) noexcept {
    using L = BlockIdLayout;
    return EntityId(((prefix & L::kMaxPrefix) << L::kPrefixShift) |
                    (static_cast<u64>(shard & L::kMaxShard) << L::kShardShift) | (offset & (L::kBlockSize - 1)));
}

/// Splits a runtime EntityId into its fields (meaningless for other kinds).
constexpr BlockIdParts decodeBlockId(EntityId id) noexcept {
    using L = BlockIdLayout;
    return BlockIdParts{(id.value >> L::kPrefixShift) & L::kMaxPrefix,
                        static_cast<u32>((id.value >> L::kShardShift) & L::kMaxShard),
                        static_cast<u32>(id.value & (L::kBlockSize - 1))};
}

/// Milliseconds since BlockIdLayout::kEpochUnixMs on the wall clock (clamped at 0).
u64 blockPrefixNow() noexcept;

/// The AllocateIdBlocks rule (05 §1.4.5; Go idgen.Allocate): the row advances to
/// max(last + n, nowPrefix) and the n prefixes ending there are written to `out` in ascending
/// order. `last` is -1 for a fresh row. Returns the new `last`. `out.size()` is n.
constexpr i64 allocateBlockPrefixes(i64 last, u64 nowPrefix, std::span<u64> out) noexcept {
    const i64 n = static_cast<i64>(out.size());
    const i64 now = static_cast<i64>(nowPrefix);
    const i64 newLast = last + n > now ? last + n : now;
    for (i64 i = 0; i < n; ++i) out[static_cast<usize>(i)] = static_cast<u64>(newLast - n + 1 + i);
    return newLast;
}

/// Hands out block prefixes for one shard (AllocateIdBlocks).
class IdBlockSource {
public:
    virtual ~IdBlockSource() = default;
    /// Appends `n` (1..16) strictly increasing prefixes to `out`, all greater than every prefix this
    /// source returned before. ErrorCode::Busy means a request is in flight and the prefixes arrive
    /// later through EntityIdMinter::addBlocks(). Must be thread-safe and must not call back into
    /// the minter.
    virtual Result<void> allocateIdBlocks(u32 n, std::vector<u64>& out) = 0;
};

/// In-process IdBlockSource with the same rule as the PostgreSQL row (tests, tools, offline
/// worlds, deterministic replays). Thread-safe.
class LocalIdBlockSource final : public IdBlockSource {
public:
    /// Milliseconds since BlockIdLayout::kEpochUnixMs. Must be thread-safe.
    using Clock = std::function<u64()>;
    struct Desc {
        Clock clock;          ///< Empty = blockPrefixNow (wall clock).
        i64 lastPrefix = -1;  ///< Persisted row value (lastPrefix()); -1 = fresh row.
    };

    LocalIdBlockSource();
    explicit LocalIdBlockSource(Desc desc);

    /// ErrorCode::InvalidArgument for n outside 1..16, LimitExceeded once prefixes pass 41 bits.
    Result<void> allocateIdBlocks(u32 n, std::vector<u64>& out) override;
    /// The row value: the newest prefix handed out (persist it for a restart; -1 = none yet).
    i64 lastPrefix() const;
    /// Number of allocateIdBlocks() calls that returned blocks.
    u64 calls() const;

private:
    mutable std::mutex m_mutex;
    Clock m_clock;
    i64 m_last;
    u64 m_calls = 0;
};

/// Mints runtime EntityIds for one shard from blocks (see the file comment).
class EntityIdMinter {
public:
    /// Milliseconds since BlockIdLayout::kEpochUnixMs, used for block retirement. Thread-safe.
    using Clock = std::function<u64()>;

    struct Desc {
        u32 shard = 0;                    ///< < 32.
        IdBlockSource* source = nullptr;  ///< Not owned; must outlive the minter. Required.
        u32 hold = 2;                     ///< Blocks held (current + spares); 4 for battle-profile cells; 1..16.
        u64 retireMs = 3'600'000;         ///< Block lifetime after allocation (05 §1.4.5: 1 h).
        Clock clock;                      ///< Empty = blockPrefixNow (wall clock).
    };

    struct Stats {
        u64 allocated = 0;      ///< Ids issued.
        u64 blocksReceived = 0; ///< Prefixes accepted from the source or addBlocks().
        u64 blocksRetired = 0;  ///< Blocks dropped by retirement before they were used up.
        u64 refills = 0;        ///< Source calls that delivered blocks.
        u64 refillFailures = 0; ///< Source calls that failed (including Busy) or returned bad prefixes.
        u64 stalls = 0;         ///< allocate() had to call the source because no block was left.
        u64 exhausted = 0;      ///< allocate() returned an invalid id (no block obtainable).
    };

    /// Checks the shard, hold and source.
    static Result<void> validate(const Desc& desc);

    /// `desc` must pass validate() (asserted). Holds no block until prime(), maintain() or the first
    /// allocate().
    explicit EntityIdMinter(const Desc& desc);
    EntityIdMinter(const EntityIdMinter&) = delete;
    EntityIdMinter& operator=(const EntityIdMinter&) = delete;

    /// Next id, strictly greater than every id this minter issued before. Lock-free unless the
    /// current block is spent. Returns an invalid EntityId (and counts Stats::exhausted) when no
    /// block can be obtained. Thread-safe.
    EntityId allocate() noexcept;
    /// Fills `out` with the ids out.size() calls of allocate() would return, reserving each run
    /// within the current block with one CAS (the spawn-group path, ADR-004a item 1). Ids that
    /// cannot be obtained are invalid, as from allocate(). Thread-safe; a concurrent allocate() may
    /// interleave at block boundaries only.
    void allocateN(std::span<EntityId> out) noexcept;

    /// Fetches the held blocks synchronously, so a start-up failure surfaces at start.
    Result<void> prime();
    /// Retires stale blocks (when a fresher one is held) and asks the source for more once fewer
    /// than `hold` fresh blocks remain, counting the current block as gone once half used. A failed
    /// source is retried at most once per second. Call once per tick (World::beginTick does).
    void maintain();
    /// Delivers prefixes from an asynchronous source (which answered Busy; a request that is not
    /// answered within 10 s is asked again). They must be in range and ascending above every prefix
    /// held or used before; otherwise nothing is added and an error is returned.
    Result<void> addBlocks(std::span<const u64> prefixes);

    /// Ids left in the held blocks.
    u64 remaining() const;
    /// Newest prefix held or used (-1 before the first block).
    i64 lastPrefix() const;
    Stats stats() const;
    u32 shard() const noexcept { return m_shard; }
    u32 hold() const noexcept { return m_hold; }

private:
    struct Block {
        u64 prefix = 0;
        u64 allocatedAtMs = 0;
    };
    // m_state packs (prefix << kStateOffsetBits) | nextOffset; nextOffset == kBlockSize means the
    // current block is spent (or there is none yet).
    static constexpr u32 kStateOffsetBits = BlockIdLayout::kOffsetBits + 1;
    static constexpr u64 kStateOffsetMask = (1ull << kStateOffsetBits) - 1;

    u64 now() const;
    bool isFresh(const Block& b, u64 nowMs) const noexcept {
        return nowMs < b.allocatedAtMs || nowMs - b.allocatedAtMs < m_retireMs; // a clock step back keeps it fresh
    }
    EntityId allocateSlow() noexcept;
    /// First offset issued from `b`: 1 for prefix 0 on shard 0, whose offset 0 would be EntityId 0
    /// (invalid; reachable with a simulated clock that starts at 0).
    u32 firstOffset(const Block& b) const noexcept { return b.prefix == 0 && m_shard == 0 ? 1u : 0u; }
    Result<void> refillLocked(u32 n, u64 nowMs);
    /// False while an asynchronous request is pending (until it times out) or, unless `force`,
    /// during the back-off after a failure.
    bool canRequestLocked(u64 nowMs, bool force) const noexcept;
    Result<void> addBlocksLocked(std::span<const u64> prefixes, u64 nowMs);
    /// Makes `b` the current block (the old one's remainder is abandoned). Lock held.
    void switchCurrentLocked(const Block& b, u64 startOffset);

    IdBlockSource* m_source;
    Clock m_clock;
    u32 m_shard;
    u32 m_hold;
    u64 m_retireMs;
    std::atomic<u64> m_state{BlockIdLayout::kBlockSize};

    mutable std::mutex m_mutex;
    std::vector<Block> m_spares; // ascending prefixes, not yet current
    Block m_current;
    bool m_haveCurrent = false;
    u64 m_issuedBefore = 0;      // ids issued from blocks before the current one
    i64 m_lastPrefix = -1;
    u64 m_retryAtMs = 0;            // back-off after a failed request
    u64 m_inFlightUntilMs = 0;      // an async source answered Busy; re-ask after this
    bool m_requestInFlight = false;
    std::vector<u64> m_scratch;
    Stats m_stats;               // allocated is computed on demand
};

/// Mints client-local ids (EntityIdKind::ClientLocal) from a counter. Thread-safe.
class ClientLocalIdAllocator {
public:
    EntityId allocate() noexcept { return EntityId::clientLocal(m_next.fetch_add(1, std::memory_order_relaxed)); }

private:
    std::atomic<u64> m_next{1};
};

} // namespace helios::ecs
