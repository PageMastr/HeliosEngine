#pragma once
// LeaseHolder: the process side of region leases and the **holder rule** (05 §1.4.2).
//
// The orchestrator allocates a region's `lease_gen` in PostgreSQL before it tells the holder, and
// every receiver (ledger, persistence, gateways, neighbour cells) rejects messages below the
// generation it knows. Safety therefore comes from generations, not from a holder racing a TTL,
// and a holder never fences itself because the control plane is unreachable. It keeps simulating
// and stops acting as the owner of a region only when it *observes* one of:
//
//   * lease_lost: the orchestrator no longer knows this registration (onLeaseLost);
//   * the region missing from, or at another generation in, the assignments a register or
//     heartbeat reply returns (onAssignments);
//   * a higher generation for the region from any other source: the DIRECTORY watch, a gateway
//     RouteUpdate, a trunk Fenced NACK or a ctl message (onHigherGeneration);
//   * a rejected region checkpoint (onRegionCheckpointRejected).
//
// A failed fence CAS or STALE_EPOCH for one AG drops that AG only (AgTable) and marks the region
// for re-verification (onAgFenceRejected); the next assignment list settles it. A region that
// comes back at a new generation is reported as Lost (old generation) then Acquired (new one): its
// old state may have been simulated elsewhere in between, so it must be reloaded, never resumed.
//
// Generations only grow (PostgreSQL allocates them), so once a region is lost at generation g, or a
// higher generation g' was seen for it, an assignment below g + 1 (or g') is a reply that was in
// flight before the change and is ignored: a region is never re-acquired at a generation it
// already gave up. These floors belong to one registration; lease_lost and releaseAll() clear
// them (the next registration's assignments are all new).
//
// Every call returns the resulting events, Lost before Acquired, each list ordered by region.
//
// Threading: owned by one thread (the process's control-plane or main thread).

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/authority/types.h"

namespace helios::authority {

/// A region placed on this process under a lease generation (v0: a whole zone).
struct RegionAssignment {
    RegionId region;
    std::string name;
    LeaseGen leaseGen = 0;
    friend bool operator==(const RegionAssignment&, const RegionAssignment&) = default;
};

enum class LeaseEventKind : u8 { Acquired, Lost };

enum class LeaseLossReason : u8 {
    None,
    LeaseLost,              ///< The orchestrator answered lease_lost.
    Unassigned,             ///< Missing from the latest assignments.
    GenerationChanged,      ///< Assigned again at another generation.
    HigherGenerationSeen,   ///< A receiver or watcher reported a higher generation.
    CheckpointRejected,     ///< Persistence refused a region checkpoint below the current generation.
    Released,               ///< Given up by this process (shutdown, drain).
};
std::string_view leaseLossReasonName(LeaseLossReason reason) noexcept;

struct LeaseEvent {
    LeaseEventKind kind = LeaseEventKind::Acquired;
    RegionAssignment assignment;
    LeaseLossReason reason = LeaseLossReason::None;
};

struct LeaseHolderStats {
    u64 acquired = 0;
    u64 lost = 0;
    u64 leaseLost = 0;
    u64 staleAssignmentsIgnored = 0;  ///< Entries below the generation held, or below the floor.
    u64 unreachable = 0;              ///< Control-plane failures (never fence).
};

class LeaseHolder {
public:
    /// The full set of regions the orchestrator says this process holds (register or heartbeat
    /// reply). An entry below the held generation, or below the region's floor (a generation this
    /// process already lost or saw superseded), is ignored (a stale reply).
    std::vector<LeaseEvent> onAssignments(std::span<const RegionAssignment> assignments);
    /// The orchestrator answered lease_lost: every region is lost (the process registers again).
    std::vector<LeaseEvent> onLeaseLost();
    /// Another source saw `seen` for the region; lost if it is above the held generation.
    std::vector<LeaseEvent> onHigherGeneration(RegionId region, LeaseGen seen);
    /// Persistence refused a region checkpoint: a higher generation exists.
    std::vector<LeaseEvent> onRegionCheckpointRejected(RegionId region);
    /// A fence CAS for one of the region's AGs failed: re-verify the region (no event).
    void onAgFenceRejected(RegionId region);
    /// The control plane could not be reached. Never fences (holder rule); only counted.
    void onControlPlaneUnreachable() noexcept { ++m_stats.unreachable; }
    /// Gives every region up (graceful shutdown or drain).
    std::vector<LeaseEvent> releaseAll();

    bool holds(RegionId region) const { return m_held.contains(region); }
    bool holds(RegionId region, LeaseGen gen) const;
    std::optional<LeaseGen> generation(RegionId region) const;
    /// The lowest generation at which `region` may still be acquired (0 = no floor).
    LeaseGen generationFloor(RegionId region) const;
    bool needsVerification(RegionId region) const;
    std::vector<RegionAssignment> held() const;
    const LeaseHolderStats& stats() const noexcept { return m_stats; }

private:
    struct Held {
        RegionAssignment assignment;
        bool verify = false;
    };
    LeaseEvent lose(std::map<RegionId, Held>::iterator it, LeaseLossReason reason);
    void raiseFloor(RegionId region, LeaseGen gen);

    std::map<RegionId, Held> m_held;
    std::map<RegionId, LeaseGen> m_floor;
    LeaseHolderStats m_stats;
};

} // namespace helios::authority
