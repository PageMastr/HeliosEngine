#include "helios/ecs/entity_id.h"

#include <algorithm>
#include <array>

#include "helios/core/assert.h"
#include "helios/core/guid.h"
#include "helios/core/hash.h"
#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/ecs/os_api.h"

namespace helios::ecs {

namespace {
using L = BlockIdLayout;
// Seed of hash62(entityGuid): "helios.entity" (FNV-1a), fixed forever (content ids are persisted).
constexpr u64 kContentIdSeed = fnv1a64("helios.entity");
constexpr u64 kRetryDelayMs = 1000;      // a failed source is retried at most once per second (as in Go)
constexpr u64 kRequestTimeoutMs = 10'000; // an unanswered asynchronous request is re-sent (Go: Timeout)
} // namespace

EntityId EntityId::fromContentGuid(const Guid& guid) noexcept {
    const std::array<u8, 16> bytes = guid.toBytes();
    return contentPlaced(hash64(bytes.data(), bytes.size(), kContentIdSeed));
}

u64 blockPrefixNow() noexcept {
    const i64 ms = unixTimeNanos() / 1'000'000 - L::kEpochUnixMs;
    return ms > 0 ? static_cast<u64>(ms) : 0;
}

// ---------------------------------------------------------------------------------------------
// LocalIdBlockSource
// ---------------------------------------------------------------------------------------------

LocalIdBlockSource::LocalIdBlockSource() : LocalIdBlockSource(Desc{}) {}

LocalIdBlockSource::LocalIdBlockSource(Desc desc)
    : m_clock(std::move(desc.clock)), m_last(std::max<i64>(-1, desc.lastPrefix)) {}

Result<void> LocalIdBlockSource::allocateIdBlocks(u32 n, std::vector<u64>& out) {
    if (n < 1 || n > L::kMaxBlocksPerCall) {
        return makeError(ErrorCode::InvalidArgument, "allocateIdBlocks: n={} outside 1..{}", n, L::kMaxBlocksPerCall);
    }
    const u64 nowMs = m_clock ? m_clock() : blockPrefixNow();
    std::lock_guard lock(m_mutex);
    std::array<u64, L::kMaxBlocksPerCall> prefixes{};
    const i64 newLast = allocateBlockPrefixes(m_last, nowMs, std::span<u64>(prefixes.data(), n));
    if (newLast > static_cast<i64>(L::kMaxPrefix)) {
        return Error{ErrorCode::LimitExceeded, "allocateIdBlocks: block prefix exceeds 41 bits"};
    }
    m_last = newLast;
    ++m_calls;
    out.insert(out.end(), prefixes.begin(), prefixes.begin() + n);
    return {};
}

i64 LocalIdBlockSource::lastPrefix() const {
    std::lock_guard lock(m_mutex);
    return m_last;
}

u64 LocalIdBlockSource::calls() const {
    std::lock_guard lock(m_mutex);
    return m_calls;
}

// ---------------------------------------------------------------------------------------------
// EntityIdMinter
// ---------------------------------------------------------------------------------------------

Result<void> EntityIdMinter::validate(const Desc& desc) {
    if (desc.shard > L::kMaxShard) return makeError(ErrorCode::OutOfRange, "shard {} > {}", desc.shard, L::kMaxShard);
    if (desc.hold < 1 || desc.hold > L::kMaxBlocksPerCall) {
        return makeError(ErrorCode::OutOfRange, "hold {} outside 1..{}", desc.hold, L::kMaxBlocksPerCall);
    }
    if (!desc.source) return Error{ErrorCode::InvalidArgument, "EntityIdMinter needs an IdBlockSource"};
    if (desc.retireMs == 0) return Error{ErrorCode::InvalidArgument, "retireMs must be > 0"};
    return {};
}

EntityIdMinter::EntityIdMinter(const Desc& desc)
    : m_source(desc.source), m_clock(desc.clock), m_shard(desc.shard & L::kMaxShard),
      m_hold(std::clamp<u32>(desc.hold, 1, L::kMaxBlocksPerCall)), m_retireMs(desc.retireMs == 0 ? 1 : desc.retireMs) {
    HELIOS_ASSERT(validate(desc).hasValue(), "invalid EntityIdMinter::Desc");
}

u64 EntityIdMinter::now() const { return m_clock ? m_clock() : blockPrefixNow(); }

EntityId EntityIdMinter::allocate() noexcept {
    u64 cur = m_state.load(std::memory_order_relaxed);
    for (;;) {
        const u64 offset = cur & kStateOffsetMask;
        if (offset >= L::kBlockSize) return allocateSlow();
        if (m_state.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return composeBlockId(cur >> kStateOffsetBits, m_shard, static_cast<u32>(offset));
        }
    }
}

EntityId EntityIdMinter::allocateSlow() noexcept {
    std::lock_guard lock(m_mutex);
    for (;;) {
        u64 cur = m_state.load(std::memory_order_relaxed);
        const u64 offset = cur & kStateOffsetMask;
        if (offset < L::kBlockSize) {
            // Another thread switched blocks while we waited for the lock.
            if (m_state.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return composeBlockId(cur >> kStateOffsetBits, m_shard, static_cast<u32>(offset));
            }
            continue;
        }
        if (m_spares.empty()) {
            // Every held block is spent: the only place minting waits for the source.
            ++m_stats.stalls;
            const u64 nowMs = now();
            if (canRequestLocked(nowMs, true)) (void)refillLocked(m_hold, nowMs);
            if (m_spares.empty()) {
                ++m_stats.exhausted;
                HELIOS_LOG_ERROR(LogEcs, "EntityIdMinter (shard {}): no ID block available", m_shard);
                return EntityId();
            }
        }
        const Block next = m_spares.front();
        m_spares.erase(m_spares.begin());
        const u32 first = firstOffset(next);
        switchCurrentLocked(next, first + 1); // `first` is ours
        return composeBlockId(next.prefix, m_shard, first);
    }
}

void EntityIdMinter::switchCurrentLocked(const Block& b, u64 startOffset) {
    const u64 next = (b.prefix << kStateOffsetBits) | startOffset;
    u64 cur = m_state.load(std::memory_order_relaxed);
    // Lock-free minters may still advance the old block; the CAS makes the switch exact.
    while (!m_state.compare_exchange_weak(cur, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    if (m_haveCurrent) m_issuedBefore += std::min<u64>(cur & kStateOffsetMask, L::kBlockSize);
    m_current = b;
    m_haveCurrent = true;
}

bool EntityIdMinter::canRequestLocked(u64 nowMs, bool force) const noexcept {
    if (m_requestInFlight && nowMs < m_inFlightUntilMs) return false;
    return force || nowMs >= m_retryAtMs;
}

Result<void> EntityIdMinter::refillLocked(u32 n, u64 nowMs) {
    n = std::clamp<u32>(n, 1, L::kMaxBlocksPerCall);
    m_scratch.clear();
    m_requestInFlight = false;
    Result<void> r = m_source->allocateIdBlocks(n, m_scratch);
    if (!r) {
        ++m_stats.refillFailures;
        if (r.errorCode() == ErrorCode::Busy) {
            m_requestInFlight = true;
            m_inFlightUntilMs = nowMs + kRequestTimeoutMs;
        } else {
            m_retryAtMs = nowMs + kRetryDelayMs;
            HELIOS_LOG_WARN(LogEcs, "ID block refill failed (shard {}); minting from held blocks: {}", m_shard, r.error());
        }
        return r;
    }
    Result<void> added = addBlocksLocked(m_scratch, nowMs);
    if (!added) {
        ++m_stats.refillFailures;
        m_retryAtMs = nowMs + kRetryDelayMs;
        HELIOS_LOG_ERROR(LogEcs, "ID block source returned unusable prefixes (shard {}): {}", m_shard, added.error());
        return added;
    }
    ++m_stats.refills;
    return {};
}

Result<void> EntityIdMinter::addBlocksLocked(std::span<const u64> prefixes, u64 nowMs) {
    if (prefixes.empty()) return Error{ErrorCode::InvalidArgument, "no prefixes"};
    i64 last = m_lastPrefix;
    for (const u64 p : prefixes) {
        if (p > L::kMaxPrefix) return makeError(ErrorCode::OutOfRange, "prefix {} exceeds 41 bits", p);
        if (static_cast<i64>(p) <= last) {
            return makeError(ErrorCode::InvalidArgument, "non-increasing prefix {} after {}", p, last);
        }
        last = static_cast<i64>(p);
    }
    for (const u64 p : prefixes) m_spares.push_back(Block{p, nowMs});
    m_lastPrefix = last;
    m_stats.blocksReceived += prefixes.size();
    return {};
}

Result<void> EntityIdMinter::addBlocks(std::span<const u64> prefixes) {
    std::lock_guard lock(m_mutex);
    m_requestInFlight = false;
    return addBlocksLocked(prefixes, now());
}

Result<void> EntityIdMinter::prime() {
    std::lock_guard lock(m_mutex);
    const u64 nowMs = now();
    const u32 held = static_cast<u32>(m_spares.size()) + (m_haveCurrent ? 1u : 0u);
    if (held >= m_hold) return {};
    return refillLocked(m_hold - held, nowMs);
}

void EntityIdMinter::maintain() {
    std::lock_guard lock(m_mutex);
    const u64 nowMs = now();

    // Retirement: drop retired blocks while a fresher block is held (never stall on them).
    usize firstFresh = m_spares.size();
    for (usize i = 0; i < m_spares.size(); ++i) {
        if (isFresh(m_spares[i], nowMs)) {
            firstFresh = i;
            break;
        }
    }
    const bool currentRetired = m_haveCurrent && !isFresh(m_current, nowMs);
    if (firstFresh < m_spares.size()) {
        if (currentRetired) {
            // Jump to the first fresh spare; the retired current block's remainder is discarded.
            const Block next = m_spares[firstFresh];
            m_stats.blocksRetired += 1 + firstFresh;
            m_spares.erase(m_spares.begin(), m_spares.begin() + static_cast<isize>(firstFresh) + 1);
            switchCurrentLocked(next, firstOffset(next));
        } else if (firstFresh > 0) {
            m_stats.blocksRetired += firstFresh;
            m_spares.erase(m_spares.begin(), m_spares.begin() + static_cast<isize>(firstFresh));
        }
    }

    // Refill: fewer than `hold` fresh blocks, the current one counting as gone once half used.
    u32 fresh = 0;
    for (const Block& b : m_spares) fresh += isFresh(b, nowMs) ? 1u : 0u;
    if (m_haveCurrent && isFresh(m_current, nowMs)) {
        const u64 used = m_state.load(std::memory_order_relaxed) & kStateOffsetMask;
        if (used < L::kBlockSize / 2) ++fresh;
    }
    if (fresh >= m_hold || !canRequestLocked(nowMs, false)) return;
    (void)refillLocked(m_hold - fresh, nowMs);
}

u64 EntityIdMinter::remaining() const {
    std::lock_guard lock(m_mutex);
    u64 n = static_cast<u64>(m_spares.size()) * L::kBlockSize;
    if (m_haveCurrent) {
        const u64 used = std::min<u64>(m_state.load(std::memory_order_relaxed) & kStateOffsetMask, L::kBlockSize);
        n += L::kBlockSize - used;
    }
    return n;
}

i64 EntityIdMinter::lastPrefix() const {
    std::lock_guard lock(m_mutex);
    return m_lastPrefix;
}

EntityIdMinter::Stats EntityIdMinter::stats() const {
    std::lock_guard lock(m_mutex);
    Stats s = m_stats;
    s.allocated = m_issuedBefore;
    if (m_haveCurrent) s.allocated += std::min<u64>(m_state.load(std::memory_order_relaxed) & kStateOffsetMask, L::kBlockSize);
    return s;
}

} // namespace helios::ecs
