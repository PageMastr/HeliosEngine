#pragma once
// AgTable: one zone instance's view of the AG trees its cell owns (04 §6.1, §3.1 "AG table").
//
// Ownership changes only through the fence. load()/park() issue the fence call and mark the tree
// Loading/Parking; poll(), called at the start of a tick (Input stage), applies every reply that
// has arrived, in issue order, so ownership flips at a tick boundary and never mid-tick. The tick
// never waits for a reply.
//
// The **fence gate**: while a fence operation on a tree is in flight, the owner sends no new
// ledger request for its AGs (ledgerGateOpen() is false); held requests are stamped with the epoch
// the reply returns. A `released{ag, epoch}` notice (a takeover or recovery superseded this cell)
// drops the tree at once without checkpointing, since its writes would fail the fence anyway.
//
// Threading: owned by the zone's tick thread. The fence's Future continuations are not used; the
// table polls isReady(), which is thread-safe.

#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "helios/authority/fence.h"

namespace helios::authority {

enum class AgLocalState : u8 {
    Loading,  ///< Load/takeover advance in flight; not authoritative yet.
    Active,   ///< Owned at `epoch`; mutations and ledger requests allowed (gate permitting).
    Parking,  ///< Park in flight; no longer simulated.
};
std::string_view agLocalStateName(AgLocalState state) noexcept;

struct AgRecord {
    AgId root;
    Epoch epoch = 0;
    AgLocalState state = AgLocalState::Loading;
    u32 fenceOpsInFlight = 0;
    /// Which tracking of the tree this is: a tree dropped (released, rejected) and loaded again
    /// gets a new incarnation, so replies to the old one's fence calls never apply to it.
    u64 incarnation = 0;
};

struct AgTableStats {
    u64 loaded = 0;
    u64 parked = 0;
    u64 rejected = 0;  ///< Fence replies other than Ok (the tree is dropped).
    u64 released = 0;  ///< Trees dropped by a released notice.
    u64 staleReplies = 0; ///< Replies for a tree that was dropped (and maybe re-tracked) meanwhile.
};

class AgTable {
public:
    explicit AgTable(const Owner& self);

    const Owner& owner() const noexcept { return m_owner; }
    /// The region lease changed generation (re-acquired): later fence calls carry the new owner.
    void setOwner(const Owner& owner) noexcept { m_owner = owner; }

    /// Loads a tree whose fence row is at `currentEpoch` (dormant; `takeover` for an active row
    /// whose owner is suspect). The tree is Loading until poll() applies the reply. False if the
    /// tree is already tracked.
    bool load(IFence& fence, AgId root, Epoch currentEpoch, bool takeover = false);
    /// Parks an Active tree after its final checkpoint (sequence `finalCheckpoint`) is durable.
    bool park(IFence& fence, AgId root, StreamSeq finalCheckpoint);
    /// Applies every fence reply that has arrived, in issue order. Returns how many were applied.
    u32 poll();
    /// The fence superseded this cell for `root` at `epoch`: drops the tree if ours is older. A
    /// tree still Loading from epoch e is dropped only for a notice above e + 1 (a notice at or
    /// below that is about an earlier ownership; if someone else took e + 1 our own CAS fails and
    /// poll() drops the tree).
    bool onReleased(AgId root, Epoch epoch);
    /// Drops every tree (the region lease was lost).
    void dropAll();

    /// True while this cell may mutate the tree's entities (Active).
    bool isAuthoritative(AgId root) const;
    std::optional<Epoch> epoch(AgId root) const;
    std::optional<AgLocalState> state(AgId root) const;
    /// False while a fence operation on the tree is in flight (04 §6.1 fence gate).
    bool ledgerGateOpen(AgId root) const;
    /// Active trees with their epochs, ordered by AgId.
    std::vector<AgEpoch> owned() const;
    usize size() const noexcept { return m_records.size(); }
    usize pendingReplies() const noexcept { return m_pending.size(); }
    const AgTableStats& stats() const noexcept { return m_stats; }

private:
    enum class Op : u8 { Load, Park };
    struct Pending {
        AgId root;
        Op op;
        u64 incarnation = 0;
        Future<FenceResult> reply;
    };

    Owner m_owner;
    u64 m_nextIncarnation = 0;
    std::unordered_map<AgId, AgRecord, IdHash> m_records;
    std::vector<Pending> m_pending;
    AgTableStats m_stats;
};

} // namespace helios::authority
