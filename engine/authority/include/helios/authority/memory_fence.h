#pragma once
// InMemoryFence: an in-process IFence with the full tree semantics of 04 §6.1, for unit tests,
// bots, PIE and single-process tools. Production cells talk to the persistence gateway's fence
// over NATS (Phase 1); both must pass the same behavioural tests (authority_tests).
//
// Rows follow `authority_fence(ag_id, root_ag, owner_cell, owner_region, owner_lease_gen, epoch,
// state, parked_seq)`. Every row of a tree carries the root's epoch, owner and state; members
// point at their tree's root (flattened) and remember the AG they joined (for Leave), with a tree
// depth of at most 3 (carrier -> fighter -> pilot).
//
// By default each call is applied at once and returns a ready Future. With Config::deferred the
// calls queue up and are applied, in call order, by pump(): that models the NATS round trip, so
// tests can check that nothing waits on a fence reply and that stale requests fail cleanly.
// Every successful advance appends a `fence.advanced` event; a load, takeover or recovery that
// supersedes another cell also emits a `released{ag, epoch}` notice for that cell (the persistence
// gateway's ctl.<shard>.cell.<prev>.released).
//
// Threading: thread-safe (one mutex). The released handler and Future continuations run on the
// thread that applies the operation (the caller, or the pump() caller), outside the lock.

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "helios/authority/fence.h"
#include "helios/core/result.h"

namespace helios::authority {

enum class RowState : u8 { Active, Dormant };

struct FenceRow {
    AgId ag;
    AgId root;    ///< The tree's root (== ag for a root).
    AgId parent;  ///< The AG this one joined (invalid for a root).
    Epoch epoch = 0;
    Owner owner;  ///< Invalid while dormant.
    RowState state = RowState::Dormant;
    StreamSeq parkedSeq = 0;
};

/// The `fence.advanced{ag, epoch, prev_owner, owner, cause}` outbox event (one per tree).
struct FenceEvent {
    AgId ag;
    Epoch epoch = 0;
    Owner previousOwner;
    Owner owner;
    AdvanceCause cause = AdvanceCause::Handoff;
};

/// `released{ag, epoch}` for the superseded owner `cell`.
struct ReleasedNotice {
    CellId cell;
    AgId ag;
    Epoch epoch = 0;
};

class InMemoryFence final : public IFence {
public:
    struct Config {
        bool deferred = false;   ///< Queue calls until pump() (async model).
        u32 maxTreeDepth = 3;
    };

    InMemoryFence();
    explicit InMemoryFence(const Config& config);
    ~InMemoryFence() override;

    /// Creates a dormant root row (the service transaction that creates the character, ship or
    /// container). AlreadyExists if the row exists.
    Result<void> createRow(AgId ag, Epoch epoch = 1);
    std::optional<FenceRow> row(AgId ag) const;
    /// Every row whose root is `root`, ordered by AgId.
    std::vector<FenceRow> tree(AgId root) const;
    std::vector<FenceEvent> events() const;
    std::vector<ReleasedNotice> releasedNotices() const;
    /// Called for each released notice (after the operation, outside the lock).
    void setReleasedHandler(std::function<void(const ReleasedNotice&)> handler);

    /// Queues further calls until pump() (true) or applies them at once (false).
    void setDeferred(bool deferred);
    /// Applies up to `maxOps` queued calls in call order; returns how many ran.
    u32 pump(u32 maxOps = 0xFFFFFFFFu);
    usize pendingOps() const;
    /// Fault injection: while set, every call completes with FenceStatus::Unavailable and changes
    /// nothing.
    void setUnavailable(bool unavailable);

    Future<FenceResult> advance(AgId root, Epoch expect, Epoch next, const Owner& owner, AdvanceCause cause) override;
    Future<FenceResult> park(AgId root, Epoch e, const Owner& owner, StreamSeq finalCheckpoint) override;
    Future<FenceResult> join(AgId member, Epoch em, AgId root, Epoch er, const Owner& owner) override;
    Future<FenceResult> leave(AgId member, Epoch e, const Owner& owner) override;
    Future<BulkFenceResult> advanceMany(std::span<const AgEpoch> roots, const Owner& owner) override;
    Future<BulkFenceResult> advanceOwnedBy(RegionId region, LeaseGen leaseGen, const Owner& owner) override;
    Future<BulkFenceResult> migrateRegion(RegionId region, LeaseGen next, const Owner& to,
                                          const AgManifest& fromSource) override;

private:
    struct Effects {
        std::vector<ReleasedNotice> released;
    };
    template <class R, class Op>
    Future<R> submit(Op op);

    FenceResult doAdvance(AgId root, Epoch expect, Epoch next, const Owner& owner, AdvanceCause cause, Effects& fx);
    FenceResult doPark(AgId root, Epoch e, const Owner& owner, StreamSeq seq, Effects& fx);
    FenceResult doJoin(AgId member, Epoch em, AgId root, Epoch er, const Owner& owner, Effects& fx);
    FenceResult doLeave(AgId member, Epoch e, const Owner& owner, Effects& fx);
    BulkFenceResult doAdvanceMany(const std::vector<AgEpoch>& roots, const Owner& owner, Effects& fx);
    BulkFenceResult doAdvanceOwnedBy(RegionId region, LeaseGen gen, const Owner& owner, Effects& fx);
    BulkFenceResult doMigrate(RegionId region, LeaseGen next, const Owner& to, const AgManifest& m, Effects& fx);

    /// Checks that every row of `root`'s tree agrees; returns the rows (ordered) or a status.
    FenceStatus collectTree(AgId root, std::vector<FenceRow*>& rows, Epoch& epoch);
    std::vector<FenceRow*> subtree(AgId member);
    u32 depthOf(const FenceRow& row) const;
    u32 subtreeHeight(AgId member) const;
    void moveTree(std::vector<FenceRow*>& rows, Epoch next, const Owner& owner, RowState state);
    void emit(AgId ag, Epoch epoch, const Owner& prev, const Owner& owner, AdvanceCause cause, Effects& fx);
    void deliver(Effects& fx);

    Config m_config;
    mutable std::mutex m_mutex;
    std::map<AgId, FenceRow> m_rows;
    std::vector<FenceEvent> m_events;
    std::vector<ReleasedNotice> m_released;
    std::function<void(const ReleasedNotice&)> m_releasedHandler;
    std::vector<std::function<void()>> m_queue;
    bool m_unavailable = false;
};

} // namespace helios::authority
