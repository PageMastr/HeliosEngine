#pragma once
// GatewayServer: the runtime of `helios-gateway` (04 §1, §2.3–2.6, §9), usable in-process.
//
//   clients ==HTP UDP 7777==> [netcode server: tokens, crypto, 120 pps + 64/40 kbit/s policing]
//                               | session = netcode client id = Session service session id
//                               | route: ResolveZone(zone from the token) -> owning cell
//                               v
//                             [trunk net::Client per cell] ==HTP trunk==> helios-cell
//
// **Sessions.** A connecting client's token user data (Go layout v1) names its account,
// character, session_epoch and zone. The gateway resolves the zone's owning cell
// (rpc.<shard>.orch.ResolveZone, cached briefly; static routes without a bus), opens or reuses a
// trunk to that cell (trunk token under the trunk key) and sends Attach. On AttachAck the client
// gets Welcome on CONTROL and its messages flow both ways: client -> Forward on the trunk, cell ->
// Deliver back on the client's channel (STATE chunks via sendState). Messages sent while the route
// is being set up wait (reliable ones only, bounded). If the cell fences the zone (ZoneFenced),
// NACKs the attach or its trunk dies, the session is re-resolved without disconnecting the client
// (RouteState on CONTROL); after `zoneUnavailableKickMs` without a cell it is kicked.
//
// **Receivers fence** (05 §1.4.2): a Deliver below the lease generation the session was attached
// under is dropped, and a trunk only carries the sessions attached through it.
//
// **Session service** (05 §1.3): the gateway subscribes to ctl.<shard>.gateway.all.kick and
// .session_epoch. A session_epoch above the one a session was admitted with means the player
// reconnected elsewhere (or through NAT rebinding here): the old session is evicted at once via
// Server::findSession + disconnect, so netcode accepts the new connection instead of waiting for
// the old one's timeout (04 §2.4). Every `ticketIntervalMs` (60 s) it batch-calls
// SealReconnectTickets (≤ 1,024 per call) and sends each ticket on CONTROL; sessions the service
// reports missing (ended, superseded) are kicked.
//
// **Limits** (04 §9): the transport polices packets (10 connection requests/s per IP, 120 pps,
// 64 kbit/s game + 40 kbit/s voice); on top the gateway meters forwarded messages per session
// (token bucket), refuses oversized ones and kicks after 3 malformed messages.
//
// Threading: update()/run()/shutdown() on one thread. Bus callbacks only queue work for update().

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/net/endpoint.h"
#include "helios/server/bus.h"
#include "helios/server/keys.h"
#include "helios/server/orch_protocol.h"
#include "helios/server/orchestrator_client.h"
#include "helios/server/protocol.h"

namespace helios::server {

/// A route used without an orchestrator: `zoneId` (0 = every zone) lives on the cell at `cell`.
struct StaticRoute {
    u64 zoneId = 0;
    net::Address cell;
};

struct GatewayConfig {
    std::string name = "gw-1";
    std::string version;

    // --- client side (the only public game port) --------------------------------------------
    net::Address listen = net::Address::ipv4(127, 0, 0, 1, 7777);
    std::vector<net::Address> publicAddresses; ///< Addresses in tokens; empty = the bound one.
    u64 protocolId = kDefaultClientProtocolId;
    net::Key shardKey{};
    u32 keyId = 1;                              ///< The shard key generation (RegisterProcess keyId).
    i32 tokenLifetimeSeconds = 45;              ///< = the Session service's token expiry.
    u32 maxClients = 256;
    std::unique_ptr<net::IDatagramTransport> clientTransport; ///< Tests: a VirtualNetwork socket.

    // --- trunks to cells ---------------------------------------------------------------------
    u64 trunkProtocolId = kDefaultTrunkProtocolId;
    net::Key trunkKey{};
    /// Local address of trunk sockets. Unspecified (the default) binds loopback for a cell on
    /// loopback and the wildcard otherwise.
    net::Address trunkBind = net::Address::anyV4(0);
    /// Tests: makes the transport of each new trunk connection (null = a UDP socket).
    std::function<std::unique_ptr<net::IDatagramTransport>()> trunkTransportFactory;
    i64 trunkRetryMs = 500;
    /// A trunk with no session routed through it is closed after this long (04 §2.6: 10 min).
    i64 trunkIdleCloseMs = 600000;

    // --- routing -----------------------------------------------------------------------------
    std::vector<StaticRoute> staticRoutes;      ///< Used when there is no bus.
    std::string defaultZone = "tallis";          ///< Resolved by name for tokens with zone 0.
    i64 resolveRetryMs = 1000;
    i64 routeCacheMs = 5000;
    i64 attachTimeoutMs = 5000;
    i64 zoneUnavailableKickMs = 30000;
    u32 maxPendingPerSession = 64;

    // --- control plane (optional) ------------------------------------------------------------
    IBus* bus = nullptr;
    std::string shard = "dev";
    i64 ticketIntervalMs = 60000;

    // --- limits (04 §9) ----------------------------------------------------------------------
    f64 messagesPerSecond = 60.0;  ///< Forwarded client messages per session (token bucket).
    f64 messageBurst = 120.0;
    u32 malformedStrikeLimit = 3;
    i64 kickGraceMs = 200;         ///< The Kick message gets this long before the disconnect.
    i64 statsLogIntervalMs = 60000; ///< Summary line in the log (0 = off).
};

struct GatewayStats {
    u64 clientsConnected = 0;
    u64 clientsDisconnected = 0;
    u64 badTokens = 0;
    u64 forwarded = 0;
    u64 delivered = 0;
    u64 rateLimited = 0;
    u64 malformed = 0;
    u64 oversized = 0;
    u64 fencedDeliveries = 0;   ///< Deliveries below the session's lease generation (dropped).
    u64 strayDeliveries = 0;    ///< For sessions not attached through that trunk (dropped).
    u64 evictions = 0;          ///< session_epoch evictions.
    u64 kicks = 0;
    u64 resolves = 0;
    u64 resolveFailures = 0;
    u64 attaches = 0;
    u64 attachNacks = 0;
    u64 zoneFenced = 0;
    u64 ticketsSealed = 0;
    u64 ticketBatches = 0;
    u64 staleSealResults = 0;   ///< Seal results for a session id now held by a newer connection (ignored).
    u64 trunksOpened = 0;
    u64 trunkFailures = 0;
    u64 trunksRetired = 0;      ///< Unused trunks closed (down, or idle for trunkIdleCloseMs).
};

class GatewayServer {
public:
    enum class SessionState : u8 { Resolving, Attaching, Active, Kicking };

    struct SessionView {
        u64 sessionId = 0;
        u64 epoch = 0;
        SessionState state = SessionState::Resolving;
        u64 zoneId = 0;     ///< Routed zone (0 until attached).
        u64 leaseGen = 0;
        net::Address cell;
        net::Address trunkLocal; ///< Local address of the trunk socket that carries the session.
    };

    static Result<std::unique_ptr<GatewayServer>> create(GatewayConfig config, i64 nowNs);
    ~GatewayServer();
    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    /// One loop iteration: client IO, trunk IO, control plane, session timers, flush.
    void update(i64 nowNs);
    void run(const std::atomic<bool>& stop);
    /// Kicks every client (reason Shutdown), detaches from cells, deregisters.
    void shutdown(i64 nowNs, std::chrono::milliseconds timeout);

    /// Kicks a session (Kick on CONTROL, then disconnect after the grace period).
    bool kick(u64 sessionId, proto::KickReason reason, std::string message, i64 nowNs);
    std::optional<SessionView> session(u64 sessionId) const;
    usize sessionCount() const noexcept { return m_sessions.size(); }
    usize trunkCount() const noexcept { return m_trunks.size(); }
    net::Server& server() noexcept { return *m_server; }
    net::Address address() const;
    OrchestratorClient* orchestrator() noexcept { return m_orch.get(); }
    const GatewayStats& stats() const noexcept { return m_stats; }

    struct ClientHandler;
    struct TrunkHandler;
    struct Session;
    struct Trunk;
    struct BusEvent;

private:
    explicit GatewayServer(GatewayConfig config);
    Result<void> init(i64 nowNs);

    // client side
    void onClientConnected(net::SessionHandle h);
    void onClientDisconnected(net::SessionHandle h, net::DisconnectReason reason);
    void onClientMessage(net::SessionHandle h, net::Channel channel, std::span<const u8> payload);
    void strike(Session& s, std::string_view what);
    void sendControl(Session& s, const std::vector<u8>& payload);

    // routing
    void resolve(Session& s);
    void applyRoute(Session& s, const net::Address& cell, u64 zoneId, u64 leaseGen);
    void sendAttach(Session& s, Trunk& t);
    void rerouteSession(Session& s, std::string_view why);
    Trunk& trunkFor(const net::Address& cell);
    void openTrunk(Trunk& t);
    void onTrunkConnected(Trunk& t);
    void onTrunkLost(Trunk& t, std::string_view why);
    void onTrunkMessage(Trunk& t, std::span<const u8> payload);
    void detachFromCell(Session& s, proto::DetachReason reason);

    // control plane
    void processBusEvents();
    void onSessionEpoch(u64 sessionId, u64 epoch);
    void sealTickets();
    void applySealResponse(const orch::SealResponse& r, const std::map<u64, u64>& sentEpochs);
    void sessionTimers();
    void retireTrunks();

    Session* findById(u64 sessionId);

    GatewayConfig m_config;
    std::unique_ptr<net::Server> m_server;
    std::unique_ptr<ClientHandler> m_clientHandler;
    std::unique_ptr<OrchestratorClient> m_orch;
    std::vector<u64> m_subscriptions;

    std::map<u64, std::unique_ptr<Session>> m_sessions;   ///< by session id
    std::map<u64, u64> m_byHandle;                        ///< client handle bits -> session id
    std::map<u32, std::unique_ptr<Trunk>> m_trunks;       ///< by trunk id
    u32 m_nextTrunk = 1;

    struct CachedRoute {
        net::Address cell;
        u64 zoneId = 0;
        u64 leaseGen = 0;
        i64 expiresNs = 0;
    };
    std::map<std::string, CachedRoute> m_routeCache;      ///< by zone key ("#id" or name)
    std::map<std::string, bool> m_resolving;              ///< zone keys with a request in flight

    std::vector<std::pair<net::SessionHandle, i64>> m_graceDisconnects; ///< refused clients, after their Kick
    std::shared_ptr<std::mutex> m_busMutex;
    std::shared_ptr<std::vector<BusEvent>> m_busEvents;
    u32 m_sealBatchesInFlight = 0;
    i64 m_nextSealNs = 0;
    GatewayStats m_stats;
    i64 m_now = 0;
    i64 m_nextStatsLogNs = 0;
};

} // namespace helios::server
