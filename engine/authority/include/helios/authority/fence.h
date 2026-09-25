#pragma once
// IFence: the epoch-fenced single-writer contract (04 §6.1, §11.2; 05 §1.13).
//
// The Fence (persistence gateway; rows in the shard primary) is the linearization point for AG
// ownership. Every operation is a conditional update over one AG *tree* (a root and its members,
// which always carry the root's epoch, owner and state):
//
//   advance(root, e, e+1, owner, cause)  handoff / load / takeover / abort: the whole tree moves
//                                        from epoch e to e+1 under `owner`. A load accepts a
//                                        dormant row; an active row only as a Takeover, which
//                                        returns the previous owner (who then gets `released`).
//   park(root, e, owner, ckpt)           logout / unload: the tree becomes dormant at e+1 with no
//                                        owner, remembering the final checkpoint's stream seq.
//   join(member, em, root, er, owner)    boarding / docking: both trees owned by owner.cell; both
//                                        move to max(em, er) + 1 and the member joins root's tree.
//   leave(member, e, owner)              disembark / undock: the member subtree becomes its own
//                                        tree; both trees move to e + 1.
//   advanceMany([(root, e)…], owner)     bundle handoff: all-or-nothing CAS of every listed tree.
//   advanceOwnedBy(region, gen, owner)   crash recovery: every *active* row of the dead region
//                                        (dormant rows are never resurrected).
//   migrateRegion(region, next, to, m)   planned migration: the manifest must list exactly the
//                                        region's active trees at their current epochs.
//
// A failed CAS changes nothing and returns FenceStatus::Stale (FENCE_STALE); stale requests are
// retried with the same idempotency key by re-stamping them. Ledger and persistence reject writes
// stamped with a superseded (ag, epoch, owner), which is what makes a crash roll back position or
// XP but never duplicate value.
//
// Threading: implementations are thread-safe. Every call is asynchronous and must never be
// awaited inside a tick; results arrive as Future completions, which the owner applies at the
// next tick boundary (the fence gate holds new ledger requests of a tree meanwhile, ag_table.h).

#include <span>
#include <vector>

#include "helios/authority/future.h"
#include "helios/authority/types.h"

namespace helios::authority {

/// The trees a source cell hands over in a planned region migration (04 §6.7).
struct AgManifest {
    std::vector<AgEpoch> roots;
};

class IFence {
public:
    virtual ~IFence() = default;

    /// Whole-tree CAS from `expect` to `next` (= expect + 1). `cause` is Handoff, Load, Takeover or
    /// Abort; recovery and migration have their own calls.
    virtual Future<FenceResult> advance(AgId root, Epoch expect, Epoch next, const Owner& owner,
                                        AdvanceCause cause) = 0;
    /// Requires the tree at (e, owner.cell, active); leaves it dormant at e + 1 with no owner.
    virtual Future<FenceResult> park(AgId root, Epoch e, const Owner& owner, StreamSeq finalCheckpoint) = 0;
    /// Both trees owned by owner.cell at the stated epochs; both move to max(em, er) + 1.
    virtual Future<FenceResult> join(AgId member, Epoch em, AgId root, Epoch er, const Owner& owner) = 0;
    /// The member's tree at (e, owner.cell); the member subtree splits off; both trees move to e + 1.
    virtual Future<FenceResult> leave(AgId member, Epoch e, const Owner& owner) = 0;
    /// All-or-nothing CAS of each listed root's tree to epoch + 1 under `owner`.
    virtual Future<BulkFenceResult> advanceMany(std::span<const AgEpoch> roots, const Owner& owner) = 0;
    /// Advances every active tree owned under (region, leaseGen or older) to `owner` (recovery).
    virtual Future<BulkFenceResult> advanceOwnedBy(RegionId region, LeaseGen leaseGen, const Owner& owner) = 0;
    /// Cooperative migration: the manifest's trees move to `to` (whose leaseGen is `next`).
    virtual Future<BulkFenceResult> migrateRegion(RegionId region, LeaseGen next, const Owner& to,
                                                  const AgManifest& fromSource) = 0;
};

} // namespace helios::authority
