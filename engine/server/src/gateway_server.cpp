#include "helios/server/gateway_server.h"

#include <algorithm>
#include <cstring>

#include "helios/core/time.h"
#include "server_log.h"

namespace helios::server {

namespace {
f64 seconds(i64 ns) { return static_cast<f64>(ns) * 1e-9; }
constexpr i64 kMs = 1'000'000;
/// How long a failed trunk that no session uses is kept (with its back-off) before it is dropped.
constexpr i64 kDownTrunkRetireMs = 30'000;

std::string zoneKey(u64 zoneId, std::string_view name) {
    return zoneId != 0 ? "#" + std::to_string(zoneId) : std::string(name);
}

/// Per-session message meter (04 §9: token bucket per RPC; per channel until schemas declare it).
struct Bucket {
    f64 tokens = 0.0;
    i64 lastNs = 0;
    bool take(f64 rate, f64 burst, i64 nowNs) {
        if (lastNs == 0) tokens = burst;
        else tokens = std::min(burst, tokens + rate * seconds(nowNs - lastNs));
        lastNs = nowNs;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};
} // namespace

// ---------------------------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------------------------

struct GatewayServer::Session {
    net::SessionHandle client;
    u64 sessionId = 0;
    u64 epoch = 0;
    proto::SessionUserData user;
    SessionState state = SessionState::Resolving;
    u64 zoneId = 0;         // routed zone (after attach)
    u64 leaseGen = 0;       // highest generation known for the zone: deliveries below it are fenced
    u64 attachedGen = 0;    // generation of the zone instance that acked the attach (0 = not yet)
    u32 trunk = 0;          // trunk id (0 = none)
    bool attachSent = false;
    i64 stateSinceNs = 0;
    i64 resolvingSinceNs = 0;
    i64 nextResolveNs = 0;
    i64 kickDeadlineNs = 0;
    std::string lastTicket;
    Bucket bucket;
    u32 strikes = 0;
    std::deque<std::pair<net::Channel, std::vector<u8>>> pending;
};

struct GatewayServer::Trunk {
    u32 id = 0;
    net::Address cell;
    std::unique_ptr<net::Client> client;
    bool connected = false;
    bool welcomed = false;
    u64 cellProcessId = 0;
    i64 retryAtNs = 0;
    u32 failures = 0;
    bool lost = false;       // set by the handler; acted on after the client's update() returns
    std::string lostWhy;
    i64 idleSinceNs = 0;     // no session routed through it since then (0 = in use)
};

struct GatewayServer::BusEvent {
    enum class Kind : u8 { Kick, SessionEpoch, Resolved, ResolveFailed, Sealed, SealFailed };
    Kind kind = Kind::Kick;
    orch::ControlMessage control;
    std::string key;
    orch::Route route;
    orch::SealResponse seal;
    std::map<u64, u64> sealEpochs; ///< Sealed: the session_epoch each session was sent with.
    std::string error;
};

struct GatewayServer::ClientHandler final : net::IEndpointHandler {
    GatewayServer& gw;
    explicit ClientHandler(GatewayServer& g) : gw(g) {}
    void onConnected(net::SessionHandle s) override { gw.onClientConnected(s); }
    void onDisconnected(net::SessionHandle s, net::DisconnectReason r) override { gw.onClientDisconnected(s, r); }
    void onMessage(net::SessionHandle s, net::Channel c, std::span<const u8> p) override { gw.onClientMessage(s, c, p); }
};

struct GatewayServer::TrunkHandler final : net::IEndpointHandler {
    GatewayServer& gw;
    Trunk& trunk;
    TrunkHandler(GatewayServer& g, Trunk& t) : gw(g), trunk(t) {}
    void onConnected(net::SessionHandle) override { gw.onTrunkConnected(trunk); }
    void onDisconnected(net::SessionHandle, net::DisconnectReason r) override {
        // Never destroy the client from inside its own update(): flag it for update().
        trunk.lost = true;
        trunk.lostWhy = std::string(net::disconnectReasonName(r));
    }
    void onMessage(net::SessionHandle, net::Channel, std::span<const u8> p) override { gw.onTrunkMessage(trunk, p); }
};

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

GatewayServer::GatewayServer(GatewayConfig config)
    : m_config(std::move(config)),
      m_busMutex(std::make_shared<std::mutex>()),
      m_busEvents(std::make_shared<std::vector<BusEvent>>()) {}

GatewayServer::~GatewayServer() {
    if (m_config.bus)
        for (u64 id : m_subscriptions) m_config.bus->unsubscribe(id);
}

Result<std::unique_ptr<GatewayServer>> GatewayServer::create(GatewayConfig config, i64 nowNs) {
    std::unique_ptr<GatewayServer> gw(new GatewayServer(std::move(config)));
    HELIOS_TRY(gw->init(nowNs));
    return gw;
}

Result<void> GatewayServer::init(i64 nowNs) {
    m_now = nowNs;
    net::ServerConfig sc;
    sc.protocolId = m_config.protocolId;
    sc.privateKey = m_config.shardKey;
    sc.bindAddress = m_config.listen;
    sc.publicAddresses = m_config.publicAddresses;
    sc.maxClients = m_config.maxClients;
    sc.maxConnectTokenLifetimeSeconds = m_config.tokenLifetimeSeconds;
    sc.connection = net::ConnectionConfig::server();
    sc.transport = std::move(m_config.clientTransport);
    sc.name = m_config.name;
    HELIOS_TRY_ASSIGN(m_server, net::Server::create(std::move(sc), seconds(nowNs)));
    m_clientHandler = std::make_unique<ClientHandler>(*this);

    if (IBus* bus = m_config.bus) {
        OrchestratorClientConfig oc;
        oc.shard = m_config.shard;
        oc.info.name = m_config.name;
        oc.info.kind = std::string(orch::kKindGateway);
        oc.info.address = address().toString();
        oc.info.version = m_config.version;
        oc.info.keyId = m_config.keyId;
        oc.info.capacity = m_config.maxClients;
        m_orch = std::make_unique<OrchestratorClient>(*bus, std::move(oc));
        m_orch->setLoadProvider([this] {
            orch::Load load;
            load.players = static_cast<i64>(m_sessions.size());
            load.freeSlots = static_cast<i64>(m_server->maxClients()) - static_cast<i64>(m_server->connectedCount());
            return load;
        });
        auto mutex = m_busMutex;
        auto events = m_busEvents;
        for (const char* verb : {"kick", "session_epoch"}) {
            const bool isKick = std::string_view(verb) == "kick";
            auto sub = bus->subscribe(orch::gatewayControlSubject(m_config.shard, verb), [mutex, events, isKick](const BusMessage& m) {
                auto msg = orch::decodeControlMessage(m.data);
                if (!msg) return;
                BusEvent e;
                e.kind = isKick ? BusEvent::Kind::Kick : BusEvent::Kind::SessionEpoch;
                e.control = std::move(*msg);
                std::lock_guard lock(*mutex);
                events->push_back(std::move(e));
            });
            if (!sub) return sub.error();
            m_subscriptions.push_back(*sub);
        }
        m_nextSealNs = nowNs + m_config.ticketIntervalMs * kMs;
    }
    HELIOS_LOG_INFO(LogGateway, "gateway '{}' listening on {} ({})", m_config.name, address().toString(),
                    m_config.bus ? "orchestrated" : "standalone");
    return {};
}

net::Address GatewayServer::address() const {
    const auto addrs = m_server->publicAddresses();
    return addrs.empty() ? m_config.listen : addrs[0];
}

void GatewayServer::update(i64 nowNs) {
    m_now = nowNs;
    m_server->update(seconds(nowNs), *m_clientHandler);
    // Trunks may be added while iterating (never removed here), so walk by id.
    std::vector<u32> ids;
    for (auto& [id, t] : m_trunks) ids.push_back(id);
    for (u32 id : ids) {
        auto it = m_trunks.find(id);
        if (it == m_trunks.end()) continue;
        Trunk& t = *it->second;
        if (!t.client) {
            if (nowNs >= t.retryAtNs && t.idleSinceNs == 0) openTrunk(t); // unused trunks are not reopened
            continue;
        }
        TrunkHandler h(*this, t);
        t.client->update(seconds(nowNs), h);
        const net::ClientState st = t.client->state();
        if (t.lost) {
            onTrunkLost(t, t.lostWhy);
        } else if (!t.connected && st != net::ClientState::Connected && !t.client->isConnecting()) {
            onTrunkLost(t, net::clientStateName(st)); // the handshake failed
        }
    }
    if (m_orch) (void)m_orch->update(nowNs); // gateways hold no regions; lease events are empty
    processBusEvents();
    sessionTimers();
    retireTrunks();
    if (m_config.bus && m_sealBatchesInFlight == 0 && nowNs >= m_nextSealNs) sealTickets();
    m_server->flush(seconds(nowNs));
    for (auto& [id, t] : m_trunks)
        if (t->client) t->client->flush(seconds(nowNs));
    if (m_config.statsLogIntervalMs > 0 && nowNs >= m_nextStatsLogNs) {
        if (m_nextStatsLogNs != 0)
            HELIOS_LOG_INFO(LogGateway, "{} session(s), {} trunk(s); forwarded {}, delivered {}, rate-limited {}, kicks {}, tickets {}",
                            m_sessions.size(), m_trunks.size(), m_stats.forwarded, m_stats.delivered, m_stats.rateLimited,
                            m_stats.kicks, m_stats.ticketsSealed);
        m_nextStatsLogNs = nowNs + m_config.statsLogIntervalMs * kMs;
    }
}

void GatewayServer::run(const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_relaxed)) {
        update(static_cast<i64>(monotonicNanos()));
        sleepNanos(1'000'000);
    }
    shutdown(static_cast<i64>(monotonicNanos()), std::chrono::milliseconds(2000));
}

void GatewayServer::shutdown(i64 nowNs, std::chrono::milliseconds timeout) {
    for (auto& [id, s] : m_sessions) {
        sendControl(*s, proto::encode(proto::ClientKick{proto::KickReason::Shutdown, "gateway shutting down"}));
        detachFromCell(*s, proto::DetachReason::GatewayShutdown);
    }
    m_server->flush(seconds(nowNs));
    for (auto& [id, t] : m_trunks)
        if (t->client) t->client->flush(seconds(nowNs));
    m_server->disconnectAll();
    m_server->update(seconds(nowNs), *m_clientHandler);
    m_server->flush(seconds(nowNs));
    for (auto& [id, t] : m_trunks)
        if (t->client) t->client->disconnect();
    if (m_orch) (void)m_orch->shutdown(timeout);
}

// ---------------------------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------------------------

GatewayServer::Session* GatewayServer::findById(u64 sessionId) {
    auto it = m_sessions.find(sessionId);
    return it == m_sessions.end() ? nullptr : it->second.get();
}

std::optional<GatewayServer::SessionView> GatewayServer::session(u64 sessionId) const {
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) return std::nullopt;
    const Session& s = *it->second;
    SessionView v;
    v.sessionId = s.sessionId;
    v.epoch = s.epoch;
    v.state = s.state;
    v.zoneId = s.zoneId;
    v.leaseGen = s.leaseGen;
    if (auto t = m_trunks.find(s.trunk); t != m_trunks.end()) {
        v.cell = t->second->cell;
        if (t->second->client) v.trunkLocal = t->second->client->transport().localAddress();
    }
    return v;
}

void GatewayServer::sendControl(Session& s, const std::vector<u8>& payload) {
    (void)m_server->send(s.client, net::Channel::Control, payload);
}

void GatewayServer::onClientConnected(net::SessionHandle h) {
    ++m_stats.clientsConnected;
    const net::SessionInfo* info = m_server->sessionInfo(h);
    if (!info) return;
    auto user = proto::parseUserData(info->userData);
    if (!user) {
        ++m_stats.badTokens;
        HELIOS_LOG_WARN(LogGateway, "client {} has unreadable token user data: {}", info->clientId, user.error().message);
        (void)m_server->send(h, net::Channel::Control, proto::encode(proto::ClientKick{proto::KickReason::BadToken, {}}));
        m_graceDisconnects.emplace_back(h, m_now + m_config.kickGraceMs * kMs); // let the Kick leave first
        return;
    }
    if (Session* old = findById(info->clientId)) {
        // Cannot normally happen (netcode refuses a second connection with the same client id).
        m_byHandle.erase(old->client.toBits());
        m_server->disconnect(old->client);
        m_sessions.erase(info->clientId);
    }
    auto s = std::make_unique<Session>();
    s->client = h;
    s->sessionId = info->clientId;
    s->epoch = user->sessionEpoch;
    s->user = *user;
    s->state = SessionState::Resolving;
    s->stateSinceNs = s->resolvingSinceNs = m_now;
    s->nextResolveNs = m_now;
    HELIOS_LOG_INFO(LogGateway, "session {} (account {}, epoch {}) connected from {}; zone {}", s->sessionId,
                    user->accountId, user->sessionEpoch, info->address.toString(), user->zoneId);
    Session& ref = *s;
    m_byHandle[h.toBits()] = s->sessionId;
    m_sessions.emplace(s->sessionId, std::move(s));
    resolve(ref);
}

void GatewayServer::onClientDisconnected(net::SessionHandle h, net::DisconnectReason reason) {
    ++m_stats.clientsDisconnected;
    auto it = m_byHandle.find(h.toBits());
    if (it == m_byHandle.end()) return;
    const u64 id = it->second;
    m_byHandle.erase(it);
    auto sit = m_sessions.find(id);
    if (sit == m_sessions.end()) return;
    Session& s = *sit->second;
    detachFromCell(s, reason == net::DisconnectReason::TimedOut ? proto::DetachReason::TimedOut : proto::DetachReason::ClientLeft);
    HELIOS_LOG_INFO(LogGateway, "session {} disconnected: {}", id, net::disconnectReasonName(reason));
    m_sessions.erase(sit);
}

void GatewayServer::strike(Session& s, std::string_view what) {
    ++m_stats.malformed;
    if (++s.strikes >= m_config.malformedStrikeLimit && s.state != SessionState::Kicking) {
        HELIOS_LOG_WARN(LogGateway, "session {} kicked after {} malformed messages (last: {})", s.sessionId, s.strikes, what);
        kick(s.sessionId, proto::KickReason::Malformed, std::string(what), m_now);
    }
}

void GatewayServer::onClientMessage(net::SessionHandle h, net::Channel channel, std::span<const u8> payload) {
    auto it = m_byHandle.find(h.toBits());
    if (it == m_byHandle.end()) return;
    Session* s = findById(it->second);
    if (!s || s->state == SessionState::Kicking) return;
    if (channel == net::Channel::Control) {
        // CONTROL from a client is for the gateway itself.
        const auto type = proto::clientControlType(payload);
        if (type == proto::ClientControlType::Ping) {
            if (auto ping = proto::decodePingOrPong(payload)) {
                sendControl(*s, proto::encodePong(ping->nonce));
                return;
            }
        }
        strike(*s, "unexpected CONTROL message");
        return;
    }
    if (payload.size() > proto::kMaxForwardPayload) {
        ++m_stats.oversized;
        strike(*s, "oversized message");
        return;
    }
    if (!s->bucket.take(m_config.messagesPerSecond, m_config.messageBurst, m_now)) {
        ++m_stats.rateLimited;
        return;
    }
    if (s->state == SessionState::Active) {
        auto t = m_trunks.find(s->trunk);
        if (t != m_trunks.end() && t->second->connected) {
            const net::SendResult r = t->second->client->send(proto::trunkChannelFor(channel),
                                                              proto::encodeForward(s->sessionId, channel, payload));
            if (r == net::SendResult::Ok) ++m_stats.forwarded;
            return;
        }
    }
    // Not routed yet: keep reliable messages (bounded); unreliable ones are stale by then.
    if (net::isReliable(channel) && s->pending.size() < m_config.maxPendingPerSession)
        s->pending.emplace_back(channel, std::vector<u8>(payload.begin(), payload.end()));
}

bool GatewayServer::kick(u64 sessionId, proto::KickReason reason, std::string message, i64 nowNs) {
    Session* s = findById(sessionId);
    if (!s || s->state == SessionState::Kicking) return false;
    ++m_stats.kicks;
    HELIOS_LOG_INFO(LogGateway, "kicking session {}: {} {}", sessionId, proto::kickReasonName(reason), message);
    sendControl(*s, proto::encode(proto::ClientKick{reason, std::move(message)}));
    detachFromCell(*s, reason == proto::KickReason::Superseded ? proto::DetachReason::Superseded : proto::DetachReason::Kicked);
    s->state = SessionState::Kicking;
    s->kickDeadlineNs = nowNs + m_config.kickGraceMs * kMs;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------------------------

void GatewayServer::resolve(Session& s) {
    const u64 zone = s.user.zoneId;
    if (!m_config.bus) {
        for (const StaticRoute& r : m_config.staticRoutes) {
            if (r.zoneId == zone || r.zoneId == 0) {
                applyRoute(s, r.cell, zone, 0);
                return;
            }
        }
        s.nextResolveNs = m_now + m_config.resolveRetryMs * kMs;
        return;
    }
    const std::string key = zoneKey(zone, m_config.defaultZone);
    if (auto c = m_routeCache.find(key); c != m_routeCache.end() && c->second.expiresNs > m_now) {
        applyRoute(s, c->second.cell, c->second.zoneId, c->second.leaseGen);
        return;
    }
    s.nextResolveNs = m_now + m_config.resolveRetryMs * kMs;
    if (m_resolving[key]) return;
    m_resolving[key] = true;
    ++m_stats.resolves;
    auto mutex = m_busMutex;
    auto events = m_busEvents;
    m_orch->resolveZone(zone, zone == 0 ? m_config.defaultZone : std::string{}, [mutex, events, key](Result<orch::Route> r) {
        BusEvent e;
        e.key = key;
        if (r) {
            e.kind = BusEvent::Kind::Resolved;
            e.route = std::move(*r);
        } else {
            e.kind = BusEvent::Kind::ResolveFailed;
            e.error = r.error().message;
        }
        std::lock_guard lock(*mutex);
        events->push_back(std::move(e));
    });
}

void GatewayServer::applyRoute(Session& s, const net::Address& cell, u64 zoneId, u64 leaseGen) {
    Trunk& t = trunkFor(cell);
    s.trunk = t.id;
    s.zoneId = zoneId;
    s.leaseGen = leaseGen;
    s.attachedGen = 0;
    s.state = SessionState::Attaching;
    s.stateSinceNs = m_now;
    s.attachSent = false;
    if (t.connected) sendAttach(s, t);
}

void GatewayServer::sendAttach(Session& s, Trunk& t) {
    proto::Attach a;
    a.sessionEpoch = s.epoch;
    a.zoneId = s.zoneId;
    a.accountId = s.user.accountId;
    a.characterId = s.user.characterId;
    a.flags = s.user.flags;
    if (t.client->send(net::Channel::EventReliable, proto::encode(s.sessionId, a)) == net::SendResult::Ok) {
        s.attachSent = true;
        s.stateSinceNs = m_now;
        ++m_stats.attaches;
    }
}

void GatewayServer::detachFromCell(Session& s, proto::DetachReason reason) {
    if (s.state != SessionState::Attaching && s.state != SessionState::Active) return;
    auto t = m_trunks.find(s.trunk);
    if (t != m_trunks.end() && t->second->connected && (s.attachSent || s.state == SessionState::Active))
        (void)t->second->client->send(net::Channel::EventReliable, proto::encode(s.sessionId, proto::Detach{s.epoch, reason}));
}

void GatewayServer::rerouteSession(Session& s, std::string_view why) {
    if (s.state == SessionState::Kicking) return;
    HELIOS_LOG_INFO(LogGateway, "session {} re-resolving its route: {}", s.sessionId, why);
    // The old cell may still hold the session (an AttachAck that is merely late, a zone that moved):
    // release it there, or it would keep a ghost session. A cell that already dropped it ignores
    // this; a re-attach to the same cell follows on the same ordered channel.
    detachFromCell(s, proto::DetachReason::Superseded);
    if (s.state == SessionState::Active) {
        sendControl(s, proto::encode(proto::ClientRouteState{0, s.zoneId}));
        s.resolvingSinceNs = m_now; // a new outage starts; failed attach retries keep the old start
    }
    s.state = SessionState::Resolving;
    s.stateSinceNs = m_now;
    s.trunk = 0;
    s.attachSent = false;
    s.attachedGen = 0;
    s.nextResolveNs = m_now + m_config.resolveRetryMs * kMs / 4;
}

GatewayServer::Trunk& GatewayServer::trunkFor(const net::Address& cell) {
    for (auto& [id, t] : m_trunks)
        if (t->cell == cell) return *t;
    auto t = std::make_unique<Trunk>();
    t->id = m_nextTrunk++;
    t->cell = cell;
    Trunk& ref = *t;
    m_trunks.emplace(ref.id, std::move(t));
    openTrunk(ref);
    return ref;
}

void GatewayServer::openTrunk(Trunk& t) {
    net::ClientConfig cc;
    cc.connection = net::ConnectionConfig::trunk();
    // The trunk socket's local address. An unspecified trunkBind follows the cell: a cell on
    // loopback (every dev box, 04 §1) gets a loopback socket, so nothing listens on the LAN and
    // Windows Firewall never asks to allow the gateway; otherwise the wildcard of the cell's family.
    // (net::Client binds ClientConfig::bindAddress; the socket config's own address is not used.)
    net::Address bind = m_config.trunkBind;
    if (!bind.isValid() || bind.isUnspecified()) {
        const u16 port = bind.isValid() ? bind.port() : 0;
        if (t.cell.isLoopback()) bind = t.cell.isIpv6() ? net::Address::loopbackV6(port) : net::Address::loopbackV4(port);
        else bind = t.cell.isIpv6() ? net::Address::anyV6(port) : net::Address::anyV4(port);
    }
    cc.bindAddress = bind;
    cc.socket = net::SocketTransportConfig::trunk(bind);
    if (m_config.trunkTransportFactory) cc.transport = m_config.trunkTransportFactory();
    cc.name = m_config.name + "-trunk-" + std::to_string(t.id);
    auto client = net::Client::create(std::move(cc), seconds(m_now));
    if (!client) {
        HELIOS_LOG_ERROR(LogGateway, "cannot open a trunk socket to {}: {}", t.cell.toString(), client.error().message);
        t.retryAtNs = m_now + m_config.trunkRetryMs * kMs;
        ++m_stats.trunkFailures;
        return;
    }
    net::ConnectTokenParams p;
    p.protocolId = m_config.trunkProtocolId;
    const net::Key nonce = net::generateKey(); // a fresh random client id per trunk connection
    std::memcpy(&p.clientId, nonce.data(), sizeof(p.clientId));
    p.clientId &= ~(1ull << 63);
    if (p.clientId == 0) p.clientId = 1;
    p.publicAddresses = {t.cell};
    p.privateKey = m_config.trunkKey;
    p.userData = proto::writeTrunkUserData(proto::kTrunkPeerGateway, m_orch ? m_orch->processId() : 0);
    auto token = net::generateConnectToken(p);
    if (!token) {
        HELIOS_LOG_ERROR(LogGateway, "cannot mint a trunk token for {}: {}", t.cell.toString(), token.error().message);
        t.retryAtNs = m_now + m_config.trunkRetryMs * kMs;
        ++m_stats.trunkFailures;
        return;
    }
    if (auto r = (*client)->connect(*token, seconds(m_now)); !r) {
        t.retryAtNs = m_now + m_config.trunkRetryMs * kMs;
        ++m_stats.trunkFailures;
        return;
    }
    t.client = std::move(*client);
    t.connected = false;
    t.welcomed = false;
    ++m_stats.trunksOpened;
    HELIOS_LOG_INFO(LogGateway, "opening trunk {} to cell {}", t.id, t.cell.toString());
}

void GatewayServer::onTrunkConnected(Trunk& t) {
    t.connected = true;
    t.failures = 0;
    proto::Hello hello;
    hello.processId = m_orch ? m_orch->processId() : 0;
    hello.name = m_config.name;
    (void)t.client->send(net::Channel::Control, proto::encode(0, hello));
    for (auto& [id, s] : m_sessions)
        if (s->trunk == t.id && s->state == SessionState::Attaching && !s->attachSent) sendAttach(*s, t);
}

void GatewayServer::onTrunkLost(Trunk& t, std::string_view why) {
    ++m_stats.trunkFailures;
    HELIOS_LOG_WARN(LogGateway, "trunk {} to cell {} lost ({})", t.id, t.cell.toString(), why);
    t.connected = false;
    t.welcomed = false;
    t.lost = false;
    t.client.reset();
    t.retryAtNs = m_now + std::min<i64>(m_config.trunkRetryMs << std::min<u32>(t.failures, 4), 5000) * kMs;
    ++t.failures;
    // The cell may be gone or moved: re-resolve (and forget cached routes to it).
    for (auto it = m_routeCache.begin(); it != m_routeCache.end();) {
        if (it->second.cell == t.cell) it = m_routeCache.erase(it);
        else ++it;
    }
    for (auto& [id, s] : m_sessions)
        if (s->trunk == t.id) rerouteSession(*s, "trunk lost");
}

void GatewayServer::onTrunkMessage(Trunk& t, std::span<const u8> payload) {
    auto msg = proto::parseTrunk(payload);
    if (!msg) return;
    const u64 stream = msg->stream;
    switch (msg->type) {
    case proto::TrunkType::Welcome:
        if (auto w = proto::decodeWelcome(msg->body)) {
            t.welcomed = true;
            t.cellProcessId = w->processId;
        }
        return;
    case proto::TrunkType::ZoneFenced:
        if (auto f = proto::decodeZoneFenced(msg->body)) {
            ++m_stats.zoneFenced;
            for (auto it = m_routeCache.begin(); it != m_routeCache.end();) {
                if (it->second.zoneId == f->zoneId) it = m_routeCache.erase(it);
                else ++it;
            }
            // Sessions on that zone instance: attached under the generation the cell gave up (or an
            // older one), or not acked yet. One attached to a newer instance stays (a late notice).
            for (auto& [id, s] : m_sessions)
                if (s->trunk == t.id && s->zoneId == f->zoneId && s->attachedGen <= f->leaseGen)
                    rerouteSession(*s, "zone fenced by its cell");
        }
        return;
    default: break;
    }
    Session* s = findById(stream);
    if (!s || s->trunk != t.id) {
        ++m_stats.strayDeliveries;
        return;
    }
    switch (msg->type) {
    case proto::TrunkType::AttachAck: {
        auto ack = proto::decodeAttachAck(msg->body);
        if (!ack || ack->sessionEpoch != s->epoch || s->state != SessionState::Attaching) return;
        s->state = SessionState::Active;
        s->stateSinceNs = m_now;
        s->zoneId = ack->zoneId;
        s->leaseGen = std::max(s->leaseGen, ack->leaseGen);
        s->attachedGen = ack->leaseGen;
        proto::ClientWelcome w;
        w.sessionId = s->sessionId;
        w.sessionEpoch = s->epoch;
        w.zoneId = ack->zoneId;
        w.tick = ack->tick;
        w.tickHz = ack->tickHz;
        w.dilationPpm = ack->dilationPpm;
        w.zoneName = ack->zoneName;
        sendControl(*s, proto::encode(w));
        while (!s->pending.empty()) {
            auto& [channel, bytes] = s->pending.front();
            if (t.client->send(proto::trunkChannelFor(channel), proto::encodeForward(s->sessionId, channel, bytes)) ==
                net::SendResult::Ok)
                ++m_stats.forwarded;
            s->pending.pop_front();
        }
        HELIOS_LOG_INFO(LogGateway, "session {} routed to zone {} '{}' on cell {} (lease_gen {})", s->sessionId, ack->zoneId,
                        ack->zoneName, t.cell.toString(), ack->leaseGen);
        return;
    }
    case proto::TrunkType::AttachNack: {
        auto nack = proto::decodeAttachNack(msg->body);
        if (!nack || nack->sessionEpoch != s->epoch) return;
        ++m_stats.attachNacks;
        if (nack->reason == proto::NackReason::StaleEpoch) {
            kick(s->sessionId, proto::KickReason::Superseded, "a newer session exists", m_now);
            return;
        }
        m_routeCache.erase(zoneKey(s->user.zoneId, m_config.defaultZone));
        rerouteSession(*s, std::string("attach refused: ") + std::string(proto::nackReasonName(nack->reason)));
        return;
    }
    case proto::TrunkType::Kick: {
        auto k = proto::decodeKick(msg->body);
        kick(s->sessionId, proto::KickReason::Cell, k ? k->message : std::string{}, m_now);
        return;
    }
    case proto::TrunkType::Deliver: {
        auto d = proto::decodeDeliver(msg->body);
        if (!d || s->state != SessionState::Active) return;
        if (d->leaseGen < s->leaseGen) {
            ++m_stats.fencedDeliveries; // from a superseded owner of the zone
            return;
        }
        const net::SendResult r = d->channel == net::Channel::State ? m_server->sendState(s->client, d->payload)
                                                                   : m_server->send(s->client, d->channel, d->payload);
        if (r == net::SendResult::Ok) ++m_stats.delivered;
        return;
    }
    default: return;
    }
}

// ---------------------------------------------------------------------------------------------
// Control plane and timers
// ---------------------------------------------------------------------------------------------

void GatewayServer::processBusEvents() {
    std::vector<BusEvent> events;
    {
        std::lock_guard lock(*m_busMutex);
        events.swap(*m_busEvents);
    }
    for (BusEvent& e : events) {
        switch (e.kind) {
        case BusEvent::Kind::Kick:
            if (findById(e.control.sessionId))
                kick(e.control.sessionId, proto::kickReasonFromString(e.control.reason), e.control.reason, m_now);
            break;
        case BusEvent::Kind::SessionEpoch: onSessionEpoch(e.control.sessionId, e.control.epoch); break;
        case BusEvent::Kind::Resolved: {
            m_resolving.erase(e.key);
            auto addr = net::Address::parse(e.route.address);
            if (!addr || !addr->isValid() || addr->port() == 0) {
                ++m_stats.resolveFailures;
                HELIOS_LOG_WARN(LogGateway, "zone {} resolved to an unusable cell address '{}'", e.key, e.route.address);
                break;
            }
            m_routeCache[e.key] = CachedRoute{*addr, e.route.zoneId, e.route.leaseGen, m_now + m_config.routeCacheMs * kMs};
            for (auto& [id, s] : m_sessions)
                if (s->state == SessionState::Resolving && zoneKey(s->user.zoneId, m_config.defaultZone) == e.key)
                    applyRoute(*s, *addr, e.route.zoneId, e.route.leaseGen);
            break;
        }
        case BusEvent::Kind::ResolveFailed:
            m_resolving.erase(e.key);
            ++m_stats.resolveFailures;
            HELIOS_LOG_DEBUG(LogGateway, "ResolveZone {} failed: {}", e.key, e.error);
            break;
        case BusEvent::Kind::Sealed:
            if (m_sealBatchesInFlight > 0) --m_sealBatchesInFlight;
            applySealResponse(e.seal, e.sealEpochs);
            break;
        case BusEvent::Kind::SealFailed:
            if (m_sealBatchesInFlight > 0) --m_sealBatchesInFlight;
            HELIOS_LOG_WARN(LogGateway, "SealReconnectTickets failed: {}", e.error);
            break;
        }
    }
}

void GatewayServer::onSessionEpoch(u64 sessionId, u64 epoch) {
    Session* s = findById(sessionId);
    if (!s || epoch <= s->epoch || s->state == SessionState::Kicking) return;
    // The player reconnected with a newer session_epoch (another gateway, or this one after NAT
    // rebinding): drop this copy so netcode accepts the new connection (04 §2.4). The Kick gets
    // `kickGraceMs` to reach the client (netcode discards payloads that arrive with its disconnect
    // packets), then sessionTimers() finds the slot with Server::findSession and disconnects it.
    ++m_stats.evictions;
    HELIOS_LOG_INFO(LogGateway, "session {} superseded (epoch {} > {}); evicting", sessionId, epoch, s->epoch);
    kick(sessionId, proto::KickReason::Superseded, "reconnected elsewhere", m_now);
}

void GatewayServer::sealTickets() {
    m_nextSealNs = m_now + m_config.ticketIntervalMs * kMs;
    if (m_sessions.empty()) return;
    std::vector<orch::SealRequest> batches(1);
    for (auto& [id, s] : m_sessions) {
        if (s->state == SessionState::Kicking) continue;
        if (batches.back().sessions.size() >= orch::kMaxTicketBatch) batches.emplace_back();
        orch::SealItem item;
        item.sessionId = s->sessionId;
        item.zoneId = s->zoneId != 0 ? s->zoneId : s->user.zoneId;
        item.epoch = s->epoch;
        item.ticket = s->lastTicket;
        batches.back().sessions.push_back(std::move(item));
    }
    auto mutex = m_busMutex;
    auto events = m_busEvents;
    for (orch::SealRequest& b : batches) {
        if (b.sessions.empty()) continue;
        ++m_stats.ticketBatches;
        ++m_sealBatchesInFlight;
        // The reply names sessions only by id. Remember the epoch each was sent with: by the time
        // the reply arrives the id may belong to a newer connection (a reconnect through this
        // gateway), which must neither get the old epoch's ticket nor be kicked as "missing".
        std::map<u64, u64> epochs;
        for (const orch::SealItem& item : b.sessions) epochs[item.sessionId] = item.epoch;
        m_config.bus->request(orch::sealTicketsSubject(m_config.shard), orch::encode(b), {}, std::chrono::milliseconds(5000),
                              [mutex, events, epochs = std::move(epochs)](BusReply reply) mutable {
                                  BusEvent e;
                                  if (reply.ok()) {
                                      auto r = orch::decodeSealResponse(reply.message.data);
                                      if (r) {
                                          e.kind = BusEvent::Kind::Sealed;
                                          e.seal = std::move(*r);
                                          e.sealEpochs = std::move(epochs);
                                      } else {
                                          e.kind = BusEvent::Kind::SealFailed;
                                          e.error = r.error().message;
                                      }
                                  } else {
                                      e.kind = BusEvent::Kind::SealFailed;
                                      e.error = std::string(busStatusName(reply.status)) + " " + std::string(reply.errorCode()) + " " +
                                                std::string(reply.errorMessage());
                                  }
                                  std::lock_guard lock(*mutex);
                                  events->push_back(std::move(e));
                              });
    }
}

void GatewayServer::applySealResponse(const orch::SealResponse& r, const std::map<u64, u64>& sentEpochs) {
    // Only the connection the request was made for: same session id *and* session_epoch.
    auto current = [&](u64 sessionId) -> Session* {
        Session* s = findById(sessionId);
        if (!s || s->state == SessionState::Kicking) return nullptr;
        auto sent = sentEpochs.find(sessionId);
        if (sent == sentEpochs.end() || sent->second != s->epoch) {
            ++m_stats.staleSealResults;
            return nullptr;
        }
        return s;
    };
    for (const orch::SealedTicket& t : r.tickets) {
        Session* s = current(t.sessionId);
        if (!s) continue;
        s->lastTicket = t.ticket;
        proto::ClientReconnectTicket msg;
        msg.ticket = t.ticket;
        msg.expiresAtUnixMs = orch::parseRfc3339UnixMs(t.expiresAt).valueOr(0);
        sendControl(*s, proto::encode(msg));
        ++m_stats.ticketsSealed;
    }
    for (u64 id : r.missing)
        if (current(id)) kick(id, proto::KickReason::SessionEnded, "session ended", m_now);
}

void GatewayServer::retireTrunks() {
    std::map<u32, u32> users;
    for (auto& [id, s] : m_sessions)
        if (s->trunk != 0) ++users[s->trunk];
    for (auto it = m_trunks.begin(); it != m_trunks.end();) {
        Trunk& t = *it->second;
        if (users.contains(t.id)) {
            t.idleSinceNs = 0;
            ++it;
            continue;
        }
        if (t.idleSinceNs == 0) t.idleSinceNs = m_now;
        // A trunk nobody routes through is not reopened after it fails (update() skips its retry;
        // cells come and go at new addresses, and retrying all of them forever would pile up
        // sockets) and is dropped after a short grace, which keeps its back-off for a session that
        // re-resolves to the same cell. A live one is retired after `trunkIdleCloseMs` without
        // sessions (04 §2.6). A session that needs the cell again opens a new trunk.
        const bool down = !t.client;
        const i64 limitMs = down ? std::min<i64>(m_config.trunkIdleCloseMs, kDownTrunkRetireMs) : m_config.trunkIdleCloseMs;
        if (m_now - t.idleSinceNs >= limitMs * kMs) {
            HELIOS_LOG_INFO(LogGateway, "trunk {} to cell {} retired ({})", t.id, t.cell.toString(),
                            down ? "down and unused" : "idle");
            if (t.client) {
                t.client->disconnect();
                t.client->flush(seconds(m_now));
            }
            ++m_stats.trunksRetired;
            it = m_trunks.erase(it);
        } else {
            ++it;
        }
    }
}

void GatewayServer::sessionTimers() {
    std::vector<u64> disconnect;
    for (auto& [id, sp] : m_sessions) {
        Session& s = *sp;
        switch (s.state) {
        case SessionState::Kicking:
            if (m_now >= s.kickDeadlineNs) disconnect.push_back(id);
            break;
        case SessionState::Resolving:
            if (m_now - s.resolvingSinceNs > m_config.zoneUnavailableKickMs * kMs) {
                kick(id, proto::KickReason::ZoneUnavailable, "no cell hosts the zone", m_now);
            } else if (m_now >= s.nextResolveNs) {
                resolve(s);
            }
            break;
        case SessionState::Attaching:
            if (m_now - s.resolvingSinceNs > m_config.zoneUnavailableKickMs * kMs) {
                kick(id, proto::KickReason::ZoneUnavailable, "no cell accepted the session", m_now);
            } else if (m_now - s.stateSinceNs > m_config.attachTimeoutMs * kMs) {
                rerouteSession(s, "attach timed out");
            }
            break;
        case SessionState::Active: break;
        }
    }
    for (auto it = m_graceDisconnects.begin(); it != m_graceDisconnects.end();) {
        if (m_now >= it->second) {
            m_server->disconnect(it->first);
            it = m_graceDisconnects.erase(it);
        } else {
            ++it;
        }
    }
    for (u64 id : disconnect) {
        Session* s = findById(id);
        if (!s) continue;
        // The netcode client id is the session id: this finds the slot even if the handle we hold
        // went stale.
        net::SessionHandle h = m_server->findSession(id);
        if (!h.isValid()) h = s->client;
        m_byHandle.erase(s->client.toBits());
        m_sessions.erase(id);
        m_server->disconnect(h);
    }
}

} // namespace helios::server
