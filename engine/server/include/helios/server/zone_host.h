#pragma once
// ZoneHost: runs many zone instances in one process (04 §3.1: small instances packed EVE-style,
// ≤ 64 instances or 200 players per process), each on its own ZoneClock and tick rate.
//
// runDue() advances every clock to the current wall time and then runs due ticks **earliest
// deadline first** (02 §2.4): among the zones with a pending tick, the one whose oldest pending
// tick became due first runs next (ties: lower zone id). A zone that fell behind runs its backlog
// (at most its catch-up cap) interleaved with the others by deadline, so one slow zone delays the
// rest by at most one of its ticks. Each tick's measured time feeds that zone's TiDi controller,
// so an overloaded zone dilates while its neighbours keep their rate.
//
// Threading: owned by the process's tick thread. Zones' inboxes may be posted from IO threads.

#include <map>
#include <memory>
#include <vector>

#include "helios/core/result.h"
#include "helios/server/zone_instance.h"

namespace helios::server {

struct ZoneHostStats {
    u64 ticks = 0;
    u64 runDueCalls = 0;
    u64 zonesAdded = 0;
    u64 zonesRemoved = 0;
};

class ZoneHost {
public:
    explicit ZoneHost(jobs::JobSystem* jobs);
    ~ZoneHost();
    ZoneHost(const ZoneHost&) = delete;
    ZoneHost& operator=(const ZoneHost&) = delete;

    /// Creates a zone instance and starts its clock at `wallNowNs` (its first tick is due one
    /// interval later). AlreadyExists for a duplicate id.
    Result<ZoneInstance*> addZone(ZoneDesc desc, i64 wallNowNs);
    /// Destroys a zone instance (its lease was lost or it was drained). False if unknown.
    bool removeZone(ZoneId id);
    ZoneInstance* find(ZoneId id) noexcept;
    ZoneInstance* findByName(std::string_view name) noexcept;
    /// All zones ordered by id.
    std::vector<ZoneInstance*> zones();
    std::vector<const ZoneInstance*> zones() const;
    usize zoneCount() const noexcept { return m_zones.size(); }

    /// Runs every tick due at `wallNowNs`, earliest deadline first. Returns the ticks run; `order`
    /// (optional) receives the zone id of each tick in execution order.
    Result<u32> runDue(i64 wallNowNs, std::vector<ZoneId>* order = nullptr);
    /// The earliest wall time at which some zone has a tick due (INT64_MAX with no zones).
    i64 nextDeadlineNs() const;
    const ZoneHostStats& stats() const noexcept { return m_stats; }
    jobs::JobSystem* jobs() const noexcept { return m_jobs; }

private:
    jobs::JobSystem* m_jobs;
    std::map<ZoneId, std::unique_ptr<ZoneInstance>> m_zones;
    ZoneHostStats m_stats;
};

} // namespace helios::server
