#pragma once
// OrchestratorClient: the cell's and gateway's side of the orchestrator's lease protocol (05 §1.4),
// the C++ twin of services/internal/orchestrator.Agent.
//
//   unregistered --RegisterProcess ok--> registered --Heartbeat every heartbeatIntervalMs-->
//        ^                                     |
//        +----- failed_precondition (lease_lost): fence every region, register again (new epoch)
//
// * Register failures back off exponentially (100 ms .. 5 s) and retry forever.
// * **Holder rule** (05 §1.4.2): a heartbeat that times out, finds no responders or hits a
//   leadership change is only counted; the process keeps its regions and keeps heartbeating. It
//   stops acting as owner only on lease_lost, or when a region disappears from, or changes
//   generation in, the assignments a reply returns (authority::LeaseHolder).
// * Every reply is matched to the registration that sent it, so a reply that arrives after a
//   re-registration is ignored.
// * ID blocks (05 §1.4.5): registration hands a cell its first block prefixes (a pool); later ones
//   come from AllocateIdBlocks. OrchestratorIdBlockSource adapts this to an ecs::EntityIdMinter.
// * ResolveZone answers "which cell owns zone X" for gateways.
//
// Threading: update() and shutdown() on one owner thread; replies are queued by bus threads and
// applied inside update(). allocateIdBlocks(), takePooledBlocks() and resolveZone() are
// thread-safe; their callbacks run on a bus thread.

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "helios/authority/lease.h"
#include "helios/ecs/entity_id.h"
#include "helios/server/bus.h"
#include "helios/server/orch_protocol.h"

namespace helios::server {

struct OrchestratorClientConfig {
    std::string shard = "dev";
    orch::ProcessInfo info;
    i64 requestTimeoutMs = 2000;
    i64 backoffMinMs = 100;
    i64 backoffMaxMs = 5000;
};

struct OrchestratorClientStats {
    u64 registrations = 0;
    u64 registerFailures = 0;
    u64 heartbeatsOk = 0;
    u64 heartbeatFailures = 0; ///< Transport failures: never fence (holder rule).
    u64 leaseLost = 0;
    u64 staleReplies = 0;      ///< Replies for a superseded registration.
};

class OrchestratorClient {
public:
    OrchestratorClient(IBus& bus, OrchestratorClientConfig config);
    ~OrchestratorClient();
    OrchestratorClient(const OrchestratorClient&) = delete;
    OrchestratorClient& operator=(const OrchestratorClient&) = delete;

    /// Applies replies and sends RegisterProcess / Heartbeat when due. Returns the lease events the
    /// replies produced (Lost before Acquired). `nowNs` is the caller's monotonic clock.
    std::vector<authority::LeaseEvent> update(i64 nowNs);
    /// Supplies the load reported with each heartbeat (called on the owner thread).
    void setLoadProvider(std::function<orch::Load()> provider);

    bool isRegistered() const noexcept { return m_registered; }
    u64 processId() const noexcept { return m_processId; }
    u64 epoch() const noexcept { return m_epoch; }
    std::string_view mode() const noexcept { return m_mode; }
    const authority::LeaseHolder& leases() const noexcept { return m_leases; }
    authority::LeaseHolder& leases() noexcept { return m_leases; }
    const OrchestratorClientStats& stats() const noexcept { return m_stats; }
    const OrchestratorClientConfig& config() const noexcept { return m_config; }

    /// Asks for `n` (1..16) more ID-block prefixes; `done` runs on a bus thread. Fails at once
    /// (InvalidState, still on the calling thread) while unregistered.
    void allocateIdBlocks(u32 n, std::function<void(Result<std::vector<u64>>)> done);
    /// Takes up to `max` pooled prefixes (from registration) above `abovePrefix`; older ones are
    /// discarded. Empty when none is left.
    std::vector<u64> takePooledBlocks(i64 abovePrefix, u32 max);
    /// The ID shard of the current registration.
    u32 idShard() const noexcept;
    /// ResolveZone by id (or by name when zoneId is 0); `done` runs on a bus thread.
    void resolveZone(u64 zoneId, std::string zoneName, std::function<void(Result<orch::Route>)> done);

    /// Graceful shutdown: releases every region (returned as Lost events) and deregisters,
    /// waiting up to `timeout` for the reply (the bus must be pumped by someone else for a
    /// FakeBus). Owner thread.
    std::vector<authority::LeaseEvent> shutdown(std::chrono::milliseconds timeout);

    struct Shared; // reply queue shared with in-flight callbacks

private:
    void sendRegister(i64 nowNs);
    void sendHeartbeat(i64 nowNs);
    void applyRegister(u64 seq, BusReply reply, i64 nowNs, std::vector<authority::LeaseEvent>& events);
    void applyHeartbeat(u64 seq, BusReply reply, i64 nowNs, std::vector<authority::LeaseEvent>& events);
    void loseLease(std::vector<authority::LeaseEvent>& events, i64 nowNs);

    IBus& m_bus;
    OrchestratorClientConfig m_config;
    std::shared_ptr<Shared> m_shared;
    std::function<orch::Load()> m_load;
    authority::LeaseHolder m_leases;
    OrchestratorClientStats m_stats;

    bool m_registered = false;
    bool m_registerInFlight = false;
    bool m_heartbeatInFlight = false;
    u64 m_seq = 0;            ///< Registration generation (replies carry it).
    u64 m_processId = 0;
    u64 m_epoch = 0;
    i64 m_heartbeatIntervalNs = 1'000'000'000;
    i64 m_nextRegisterNs = 0;
    i64 m_nextHeartbeatNs = 0;
    i64 m_backoffMs = 100;
    std::string m_mode;
};

/// ecs::IdBlockSource over the orchestrator (05 §1.4.5): serves registration prefixes from the
/// pool synchronously, otherwise asks AllocateIdBlocks and answers Busy; pump() delivers the
/// prefixes to the minter on the owner thread (never from inside allocateIdBlocks(), which the
/// minter calls with its lock held).
///
/// Threading: allocateIdBlocks() is thread-safe (the minter may call it from any thread); attach()
/// and pump() on the zone's tick thread.
class OrchestratorIdBlockSource final : public ecs::IdBlockSource {
public:
    explicit OrchestratorIdBlockSource(OrchestratorClient& client);
    ~OrchestratorIdBlockSource() override;

    /// The minter that receives asynchronous prefixes (not owned; set once the World exists).
    void attach(ecs::EntityIdMinter* minter) noexcept { m_minter = minter; }
    Result<void> allocateIdBlocks(u32 n, std::vector<u64>& out) override;
    /// Delivers completed AllocateIdBlocks replies to the minter. Returns prefixes delivered.
    u32 pump();

    struct State;

private:
    OrchestratorClient& m_client;
    ecs::EntityIdMinter* m_minter = nullptr;
    std::shared_ptr<State> m_state;
};

} // namespace helios::server
