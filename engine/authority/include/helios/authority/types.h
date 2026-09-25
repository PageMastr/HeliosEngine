#pragma once
// Authority vocabulary (04 §6.1, 05 §1.4.2): authority groups (AGs), epochs, owners and region
// lease generations.
//
// An AG is the unit of authority, persistence and handoff: a root entity plus its attached
// hierarchy (a ship and its interior grid). Exactly one cell owns an AG at a time, at a 64-bit
// *epoch*; every fence operation is a compare-and-set on that epoch, so a superseded owner's
// writes are refused everywhere (ledger, persistence, trunks). An owner is named by the process
// incarnation (`cell`) plus the region lease it simulates under (`region`, `leaseGen`). In v0 a
// region is a whole zone instance (one cell per zone, 04 §6.5).
//
// Threading: plain value types.

#include <compare>
#include <string_view>
#include <vector>

#include "helios/core/hash.h"
#include "helios/core/types.h"

namespace helios::authority {

/// A 64-bit identifier made distinct per `Tag` (so an AgId never converts into a CellId). 0 is
/// the invalid value.
template <class Tag>
struct Id {
    u64 value = 0;

    constexpr Id() noexcept = default;
    constexpr explicit Id(u64 v) noexcept : value(v) {}
    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return value != 0; }
    friend constexpr bool operator==(Id, Id) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Id, Id) noexcept = default;
};

struct AgTag;
struct CellTag;
struct RegionTag;
/// Authority group: the root entity's EntityId for persistent AGs.
using AgId = Id<AgTag>;
/// A process incarnation (the orchestrator's process ID of a cell).
using CellId = Id<CellTag>;
/// An orchestrator region (v0: one per zone instance, so the zone ID).
using RegionId = Id<RegionTag>;

/// Hash functor for Id<Tag> keys in unordered containers.
struct IdHash {
    template <class Tag>
    usize operator()(Id<Tag> id) const noexcept {
        return static_cast<usize>(mix64(id.value));
    }
};

using Epoch = u64;
/// Region lease generation, allocated in PostgreSQL before any holder is told (05 §1.4.2).
using LeaseGen = u64;
/// Position in a persistence stream (the sequence of a final checkpoint).
using StreamSeq = u64;

/// Who simulates an AG: the process incarnation plus the region lease it runs under. An invalid
/// (all-zero) owner means "nobody" (a dormant fence row).
struct Owner {
    CellId cell;
    RegionId region;
    LeaseGen leaseGen = 0;

    constexpr bool isValid() const noexcept { return cell.isValid(); }
    friend constexpr bool operator==(const Owner&, const Owner&) noexcept = default;
};

struct AgEpoch {
    AgId ag;
    Epoch epoch = 0;
    friend constexpr bool operator==(const AgEpoch&, const AgEpoch&) noexcept = default;
};

/// Why a fence row advanced; recorded in the `fence.advanced` outbox event (04 §6.1).
enum class AdvanceCause : u8 {
    Handoff,   ///< Owner-to-owner transfer (also bundles, AdvanceMany).
    Load,      ///< A cell loads a dormant tree (login, instance or structure load).
    Takeover,  ///< A load that supersedes an active owner (suspect owner, other content version).
    Abort,     ///< A handoff aborted back to its source.
    Join,      ///< Boarding, docking, landing in a hangar.
    Leave,     ///< Disembark, undock, launch.
    Park,      ///< Logout, linkdead expiry, unload: the tree becomes dormant.
    Recovery,  ///< Crash recovery of a dead region's active rows (AdvanceOwnedBy).
    Migrate,   ///< Planned region migration (MigrateRegion).
};
std::string_view advanceCauseName(AdvanceCause cause) noexcept;

/// Outcome of one fence operation.
enum class FenceStatus : u8 {
    Ok,
    Stale,          ///< FENCE_STALE: epoch, owner or state did not match (nothing changed).
    TreeMismatch,   ///< FENCE_TREE_MISMATCH: rows of one tree disagree (alerts; nothing changed).
    NotFound,       ///< No fence row for the AG.
    NotRoot,        ///< The AG is a member; the operation needs its tree's root.
    NeedsTakeover,  ///< A load found an active row and was not flagged as a takeover.
    InvalidArgument,
    Unavailable,    ///< The fence could not be reached (transport; retry with the same arguments).
};
std::string_view fenceStatusName(FenceStatus status) noexcept;

struct FenceResult {
    FenceStatus status = FenceStatus::Ok;
    /// On Ok: the tree's new epoch. On Stale: the tree's current epoch (when known).
    Epoch epoch = 0;
    /// The owner before the operation (invalid for a dormant row).
    Owner previousOwner;

    bool ok() const noexcept { return status == FenceStatus::Ok; }
};

struct BulkFenceResult {
    FenceStatus status = FenceStatus::Ok;
    /// Roots that advanced, with their new epochs.
    std::vector<AgEpoch> advanced;

    bool ok() const noexcept { return status == FenceStatus::Ok; }
};

} // namespace helios::authority
