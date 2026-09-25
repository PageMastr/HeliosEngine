#pragma once
// CellServer: the runtime of `helios-cell` (04 §1, §3), usable in-process for tests and PIE.
//
//   gateways ==HTP trunk==> [trunk net::Server] --attach/forward--> ZoneInstance inboxes
//                                                               ZoneHost (EDF over zone clocks)
//   gateways <==HTP trunk== [trunk net::Server] <--outboxes------ zone ticks
//   orchestrator <==NATS==> OrchestratorClient: register, 1 Hz heartbeat, lease holder rule,
//                           AllocateIdBlocks for every zone's EntityId minter
//
// **Zones.** With a bus, the cell registers as kind "cell" (declaring the zone names it serves,
// empty = any) and hosts exactly the zones the orchestrator assigns, each under its lease
// generation. A zone whose lease is lost (lease_lost, unassigned, generation changed) is destroyed
// at once and every trunk is told ZoneFenced{zone, gen}; a re-assignment creates it again under the
// new generation (its old state may have been simulated elsewhere meanwhile). The orchestrator
// being unreachable never stops a zone (holder rule). Without a bus (standalone dev runs) the
// cell hosts `staticZones` at lease generation 0.
//
// **Trunks.** Gateways connect with trunk tokens (trunk protocol id + trunk key; 04 §2.6) and
// send Hello, then per session Attach / Forward / Detach. The cell routes a session's messages
// only from the trunk that attached it most recently at the highest epoch; everything from any
// other trunk is dropped (a superseded gateway cannot reach the simulation). Deliveries carry the
// zone's lease generation.
//
// Threading: update()/run()/shutdown() on one thread (the process's tick thread). Zone ticks use
// the job system for parallel stages. Bus callbacks only queue work for update().

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "helios/authority/zone_clock.h"
#include "helios/core/result.h"
#include "helios/net/endpoint.h"
#include "helios/server/bus.h"
#include "helios/server/keys.h"
#include "helios/server/orchestrator_client.h"
#include "helios/server/zone_host.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::server {

struct StaticZone {
    ZoneId id = 0;
    std::string name;
    u32 tickHz = 0; ///< 0 = the cell's default.
};

struct CellServerConfig {
    std::string name = "cell-1";
    std::string version;

    // --- trunk server (gateways connect here) ------------------------------------------------
    net::Address trunkBind = net::Address::ipv4(127, 0, 0, 1, 7810);
    /// Address gateways use (RegisterProcess `address`, trunk tokens). Invalid = the bound one.
    net::Address trunkPublic;
    u64 trunkProtocolId = kDefaultTrunkProtocolId;
    net::Key trunkKey{};
    u32 maxTrunks = 64;
    /// Optional transport (VirtualNetwork socket in tests); owned by the cell.
    std::unique_ptr<net::IDatagramTransport> trunkTransport;

    // --- zones -------------------------------------------------------------------------------
    std::vector<StaticZone> staticZones;       ///< Hosted without an orchestrator.
    std::vector<std::string> declaredZones;    ///< RegisterProcess.zones (empty = any zone).
    u32 defaultTickHz = 20;
    std::map<std::string, u32, std::less<>> zoneTickHz; ///< Per zone name.
    u32 idShard = 0;                           ///< Standalone ID shard.
    authority::TiDiConfig tidi;
    u32 maxSessionsPerZone = 256;

    // --- control plane (optional) ------------------------------------------------------------
    IBus* bus = nullptr;                       ///< Not owned; null = standalone.
    std::string shard = "dev";

    // --- execution ---------------------------------------------------------------------------
    jobs::JobSystem* jobs = nullptr;           ///< Not owned; null = the cell owns one.
    u32 jobWorkers = 0;                        ///< Owned job system size (0 = cores - 2, >= 1).
    std::function<void(ZoneInstance&)> onZoneUp; ///< Called after a zone instance is created.
    i64 statsLogIntervalMs = 60000;            ///< Per-zone tick summary in the log (0 = off).
};

struct CellStats {
    u64 trunksConnected = 0;
    u64 trunksDisconnected = 0;
    u64 trunkMessages = 0;
    u64 trunkMalformed = 0;
    u64 attachNotHosted = 0;
    u64 forwardsDropped = 0;   ///< From a trunk that does not carry the session.
    u64 deliveries = 0;
    u64 sendFailures = 0;
    u64 zonesFenced = 0;
};

class CellServer {
public:
    static Result<std::unique_ptr<CellServer>> create(CellServerConfig config, i64 nowNs);
    ~CellServer();
    CellServer(const CellServer&) = delete;
    CellServer& operator=(const CellServer&) = delete;

    /// One loop iteration: trunk IO, control plane, due zone ticks (EDF), trunk flush.
    void update(i64 nowNs);
    /// When update() should run next (the next zone tick, or `nowNs` + 1 ms for trunk IO).
    i64 nextWakeNs(i64 nowNs) const;
    /// Runs update() on the real clock until `stop` becomes true, then shutdown().
    void run(const std::atomic<bool>& stop);
    /// Releases every zone, tells the gateways, deregisters (waits up to `timeout`).
    void shutdown(i64 nowNs, std::chrono::milliseconds timeout);

    ZoneHost& host() noexcept { return m_host; }
    net::Server& trunk() noexcept { return *m_trunk; }
    OrchestratorClient* orchestrator() noexcept { return m_orch.get(); }
    /// The address gateways connect to.
    net::Address trunkAddress() const;
    const CellStats& stats() const noexcept { return m_stats; }
    usize sessionCount() const noexcept { return m_routes.size(); }
    /// 99th percentile of the recent tick durations over every hosted zone (the heartbeat's
    /// tickP99Ms); 0 without ticks.
    i64 tickP99Ns() const;

    struct TrunkHandler;

private:
    explicit CellServer(CellServerConfig config);
    Result<void> init(i64 nowNs);
    void applyLeaseEvents(const std::vector<authority::LeaseEvent>& events, i64 nowNs);
    Result<ZoneInstance*> startZone(ZoneId id, std::string name, authority::LeaseGen gen, i64 nowNs);
    void stopZone(ZoneId id, authority::LeaseGen gen);
    void onTrunkMessage(net::SessionHandle trunk, std::span<const u8> payload);
    void onTrunkDisconnected(net::SessionHandle trunk);
    void flushZoneOutput();
    ZoneInstance* zoneForAttach(u64 zoneId);

    // Declaration order is destruction order in reverse: zones (whose worlds use the job system
    // and the ID sources) go before the sources, the orchestrator client and the job system.
    CellServerConfig m_config;
    std::unique_ptr<jobs::JobSystem> m_ownedJobs;
    std::unique_ptr<OrchestratorClient> m_orch;
    std::map<ZoneId, std::unique_ptr<OrchestratorIdBlockSource>> m_idSources;
    ZoneHost m_host;
    std::unique_ptr<TrunkHandler> m_handler;
    std::unique_ptr<net::Server> m_trunk;

    struct Route {
        ZoneId zone = 0;
        net::SessionHandle trunk;
        u64 epoch = 0;
    };
    std::map<u64, Route> m_routes; ///< sessionId -> current binding
    struct TrunkPeer {
        std::string name;
        u64 processId = 0;
        bool hello = false;
    };
    std::map<u64, TrunkPeer> m_peers; ///< by trunk handle bits
    CellStats m_stats;
    i64 m_now = 0;
    i64 m_nextStatsLogNs = 0;
};

} // namespace helios::server
