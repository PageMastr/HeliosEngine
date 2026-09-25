// End to end, in process: test client -> gateway -> cell and back (04 §1, §2.3–2.6, §9; 05 §1.3).
// Everything runs on a simulated clock over net::VirtualNetwork (real netcode handshakes and
// encryption, no sockets) and a FakeBus with the fake orchestrator and Session service, except the
// last case, which uses real UDP loopback sockets.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "fake_services.h"
#include "helios/core/jobs.h"
#include "helios/core/time.h"
#include "helios/server/cell_server.h"
#include "helios/server/gateway_server.h"
#include "helios/server/probe_client.h"

using namespace helios;
using namespace helios::server;
using namespace helios::server::test;

namespace {
constexpr i64 kMs = 1'000'000;

jobs::JobSystem& e2eJobs() {
    static jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 2, .name = "E2E"});
    return js;
}

const net::Address kGatewayAddr = net::Address::ipv4(10, 0, 0, 1, 7777);
const net::Address kCellAddr = net::Address::ipv4(10, 0, 0, 2, 7810);

struct ClusterOptions {
    bool bus = false;
    GatewayConfig gateway;
    std::vector<StaticZone> zones{StaticZone{1002, "tallis", 20}};
};

struct Cluster {
    using Options = ClusterOptions;
    net::VirtualNetwork network;
    std::unique_ptr<FakeBus> bus;
    std::unique_ptr<FakeOrchestrator> orch;
    std::unique_ptr<FakeSessionService> sessions;
    std::unique_ptr<CellServer> cell;
    std::unique_ptr<GatewayServer> gw;
    std::vector<std::unique_ptr<ProbeClient>> probes;
    i64 now = 5'000 * kMs;
    net::Key shardKey = insecureDevKey("test-shard");
    net::Key trunkKey = insecureDevKey("test-trunk");
    u32 nextProbeHost = 10;

    Cluster() : Cluster(Options{}) {}
    explicit Cluster(Options o) {
        if (o.bus) {
            bus = std::make_unique<FakeBus>();
            orch = std::make_unique<FakeOrchestrator>(*bus, "dev", std::vector<std::pair<u64, std::string>>{{1002, "tallis"}, {1003, "harrow"}});
            sessions = std::make_unique<FakeSessionService>(*bus, "dev");
        }
        startCell(o.zones);
        GatewayConfig& g = o.gateway;
        g.name = "gw-test";
        g.listen = kGatewayAddr;
        g.shardKey = shardKey;
        g.trunkKey = trunkKey;
        g.clientTransport = network.bind(kGatewayAddr).value();
        g.trunkTransportFactory = [this] { return std::unique_ptr<net::IDatagramTransport>(network.bind(net::Address::ipv4(10, 0, 0, 1, 0)).value()); };
        g.bus = bus.get();
        if (!bus) g.staticRoutes = {StaticRoute{0, kCellAddr}};
        gw = GatewayServer::create(std::move(g), now).value();
    }

    void startCell(const std::vector<StaticZone>& zones) {
        CellServerConfig c;
        c.name = "cell-test";
        c.trunkBind = kCellAddr;
        c.trunkKey = trunkKey;
        c.trunkTransport = network.bind(kCellAddr).value();
        c.jobs = &e2eJobs();
        c.bus = bus.get();
        if (!bus) c.staticZones = zones;
        c.zoneTickHz = {{"tallis", 20}, {"harrow", 10}};
        cell = CellServer::create(std::move(c), now).value();
    }

    ProbeClient& probe(u64 sessionId, u64 epoch, u64 zoneId) {
        ProbeConfig pc;
        pc.transport = network.bind(net::Address::ipv4(10, 0, 1, static_cast<u8>(nextProbeHost++), 5000)).value();
        probes.push_back(ProbeClient::create(std::move(pc), now).value());
        ProbeClient& p = *probes.back();
        connect(p, sessionId, epoch, zoneId);
        return p;
    }
    void connect(ProbeClient& p, u64 sessionId, u64 epoch, u64 zoneId) {
        DevTokenParams t;
        t.protocolId = kDefaultClientProtocolId;
        t.key = shardKey;
        t.sessionId = sessionId;
        t.gateways = {kGatewayAddr};
        t.user.accountId = 100 + sessionId;
        t.user.characterId = 200 + sessionId;
        t.user.sessionEpoch = epoch;
        t.user.zoneId = zoneId;
        auto token = mintDevToken(t).value();
        REQUIRE(p.connect(token, now));
        if (sessions) sessions->sessions[sessionId] = epoch;
    }

    void step(i64 ms) {
        for (i64 i = 0; i < ms; ++i) {
            now += kMs;
            if (bus) bus->pump(now / kMs);
            if (cell) cell->update(now);
            gw->update(now);
            for (auto& p : probes) p->update(now);
        }
    }
    template <class Pred>
    bool stepUntil(Pred pred, i64 maxMs) {
        for (i64 i = 0; i < maxMs; ++i) {
            if (pred()) return true;
            step(1);
        }
        return pred();
    }
};

ZoneInstance* zoneOf(Cluster& c, u64 id) { return c.cell ? c.cell->host().find(id) : nullptr; }
} // namespace

TEST_CASE("server.e2e: standalone client -> gateway -> cell: welcome, echo, tick state, ping, detach") {
    Cluster c;
    ProbeClient& p = c.probe(42, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    const auto& w = *p.stats().welcome;
    CHECK(w.sessionId == 42);
    CHECK(w.zoneId == 1002);
    CHECK(w.zoneName == "tallis");
    CHECK(w.tickHz == 20);
    CHECK(w.dilationPpm == authority::kDilationOne);
    CHECK(c.gw->session(42)->state == GatewayServer::SessionState::Active);
    REQUIRE(zoneOf(c, 1002)->sessions().count(42) == 1);
    CHECK(zoneOf(c, 1002)->sessions().at(42).accountId == 142);

    const std::vector<u8> data{'h', 'e', 'l', 'i', 'o', 's'};
    for (int i = 0; i < 5; ++i) p.sendEcho(data, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 5; }, 1000));
    CHECK(p.stats().echoMismatches == 0);
    CHECK(p.stats().maxRttMs <= 60.0); // at most one 50 ms tick plus the virtual network
    // Tick state flows at the zone rate, in order.
    const u64 before = p.stats().tickStates;
    c.step(1000);
    CHECK(p.stats().tickStates - before >= 19);
    CHECK(p.stats().tickStates - before <= 21);
    CHECK(p.stats().tickRegressions == 0);
    p.sendPing(99);
    REQUIRE(c.stepUntil([&] { return p.stats().pongs == 1; }, 200));

    p.disconnect();
    REQUIRE(c.stepUntil([&] { return c.gw->sessionCount() == 0 && zoneOf(c, 1002)->sessions().empty(); }, 500));
    CHECK(c.cell->sessionCount() == 0);
}

TEST_CASE("server.e2e: zone 0 goes to the cell's default zone; an unknown zone is refused and re-resolved") {
    Cluster::Options o;
    o.zones = {StaticZone{1002, "tallis", 20}, StaticZone{2000, "void", 10}};
    o.gateway.zoneUnavailableKickMs = 1500;
    Cluster c(std::move(o));
    ProbeClient& p0 = c.probe(1, 1, 0);
    ProbeClient& bad = c.probe(2, 1, 9999);
    REQUIRE(c.stepUntil([&] { return p0.isWelcomed(); }, 2000));
    CHECK(p0.stats().welcome->zoneId == 1002); // the lowest hosted zone id
    // The cell NACKs zone 9999; the gateway keeps trying and finally kicks.
    REQUIRE(c.stepUntil([&] { return bad.stats().kick.has_value(); }, 4000));
    CHECK(bad.stats().kick->reason == proto::KickReason::ZoneUnavailable);
    CHECK(c.gw->stats().attachNacks >= 1);
    CHECK_FALSE(bad.isWelcomed());
}

TEST_CASE("server.e2e: orchestrated: registration, ResolveZone routing and ID blocks") {
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    REQUIRE(c.stepUntil([&] { return c.cell->host().zoneCount() == 2 && c.gw->orchestrator()->isRegistered(); }, 2000));
    CHECK(c.orch->processes().size() == 2);
    ZoneInstance* tallis = zoneOf(c, 1002);
    REQUIRE(tallis);
    CHECK(tallis->leaseGen() == c.orch->zone(1002).leaseGen);
    CHECK(tallis->desc().tickHz == 20);
    CHECK(zoneOf(c, 1003)->desc().tickHz == 10);

    ProbeClient& p = c.probe(7, 1, 1002);
    ProbeClient& h = c.probe(8, 1, 1003);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed() && h.isWelcomed(); }, 2000));
    CHECK(p.stats().welcome->zoneName == "tallis");
    CHECK(h.stats().welcome->zoneName == "harrow");
    CHECK(c.gw->session(7)->leaseGen == c.orch->zone(1002).leaseGen);
    CHECK(c.gw->stats().resolves >= 2);
    CHECK(c.gw->trunkCount() == 1); // both zones on one cell share a trunk
    p.sendEcho(std::vector<u8>{1}, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 1; }, 500));

    // Entities minted in the zone use the orchestrator's blocks.
    const ecs::EntityId id = tallis->world().idMinter().allocate();
    REQUIRE(id.isValid());
    CHECK(ecs::decodeBlockId(id).shard == c.orch->idShard);
    CHECK(ecs::decodeBlockId(id).prefix > 1000);
    // The cell's heartbeats report the attached players.
    c.step(1500);
    const auto& procs = c.orch->processes();
    const u64 cellId = c.orch->processByName("cell-test");
    REQUIRE(cellId != 0);
    CHECK(procs.at(cellId).heartbeats >= 1);
}

TEST_CASE("server.e2e: session_epoch evicts the old session so a reconnect is accepted at once") {
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(55, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 3000));

    // NAT rebinding: the same player comes back from a new address with a token at epoch 2 while
    // the old session is still connected. netcode ignores it until the old session is dropped...
    ProbeConfig pc;
    pc.transport = c.network.bind(net::Address::ipv4(10, 0, 9, 9, 6000)).value();
    c.probes.push_back(ProbeClient::create(std::move(pc), c.now).value());
    ProbeClient& again = *c.probes.back();
    c.connect(again, 55, 2, 1002);
    c.step(300);
    CHECK_FALSE(again.isConnected());
    // ...which the Session service's session_epoch signal (it CAS-incremented the epoch to 2 when
    // it minted that token) makes the gateway do right away.
    (void)c.bus->publish(orch::gatewayControlSubject("dev", "session_epoch"), orch::encode(orch::ControlMessage{55, 2, "reconnect"}));
    REQUIRE(c.stepUntil([&] { return again.isWelcomed(); }, 3000));
    CHECK(c.gw->stats().evictions == 1);
    REQUIRE(p.stats().kick.has_value());
    CHECK(p.stats().kick->reason == proto::KickReason::Superseded);
    CHECK(c.gw->session(55)->epoch == 2);
    ZoneInstance* zone = zoneOf(c, 1002);
    REQUIRE(zone->sessions().count(55) == 1);
    CHECK(zone->sessions().at(55).epoch == 2);
    // A stale signal (older epoch) changes nothing.
    orch::ControlMessage stale{55, 1, "reconnect"};
    (void)c.bus->publish(orch::gatewayControlSubject("dev", "session_epoch"), orch::encode(stale));
    c.step(50);
    CHECK(c.gw->stats().evictions == 1);
    again.sendEcho(std::vector<u8>{7}, c.now);
    REQUIRE(c.stepUntil([&] { return again.stats().echoesReceived == 1; }, 500));
}

TEST_CASE("server.e2e: reconnect tickets are sealed every interval; ended sessions are kicked") {
    Cluster::Options o;
    o.bus = true;
    o.gateway.ticketIntervalMs = 1000;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(61, 3, 1002);
    ProbeClient& gone = c.probe(62, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed() && gone.isWelcomed(); }, 3000));
    c.sessions->sessions.erase(62); // the Session service no longer knows it
    REQUIRE(c.stepUntil([&] { return !p.stats().tickets.empty(); }, 2500));
    CHECK(p.stats().tickets[0].ticket.starts_with("ticket-61-"));
    CHECK(p.stats().tickets[0].expiresAtUnixMs == orch::parseRfc3339UnixMs("2026-09-25T12:05:00.5Z").value());
    bool sawEpoch = false;
    for (const auto& item : c.sessions->lastItems) sawEpoch |= item.sessionId == 61 && item.epoch == 3 && item.zoneId == 1002;
    CHECK(sawEpoch);
    REQUIRE(c.stepUntil([&] { return gone.stats().kick.has_value(); }, 500));
    CHECK(gone.stats().kick->reason == proto::KickReason::SessionEnded);
    // The next batch carries the ticket the gateway holds (to rebuild a record lost with Valkey).
    REQUIRE(c.stepUntil([&] { return p.stats().tickets.size() >= 2; }, 1500));
    bool sawTicket = false;
    for (const auto& item : c.sessions->lastItems) sawTicket |= item.sessionId == 61 && item.ticket == p.stats().tickets[0].ticket;
    CHECK(sawTicket);
    REQUIRE(c.stepUntil([&] { return !gone.isConnected(); }, 1000));
}

TEST_CASE("server.e2e: a control kick reaches the client before the disconnect") {
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(71, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 3000));
    c.sessions->kick(71, "logout");
    REQUIRE(c.stepUntil([&] { return p.stats().kick.has_value(); }, 500));
    CHECK(p.stats().kick->reason == proto::KickReason::Logout);
    REQUIRE(c.stepUntil([&] { return !p.isConnected(); }, 1000));
    REQUIRE(c.stepUntil([&] { return zoneOf(c, 1002)->sessions().empty(); }, 500));
}

TEST_CASE("server.e2e: a zone that changes generation is fenced and the session re-routed") {
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(81, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 3000));
    const u64 gen1 = c.gw->session(81)->leaseGen;
    c.orch->bumpGeneration(1002);
    // The cell sees the new generation at its next heartbeat, drops the zone (ZoneFenced to the
    // gateway), then hosts it again under the new generation; the gateway re-attaches.
    REQUIRE(c.stepUntil([&] { return p.stats().welcomes == 2; }, 4000));
    CHECK(c.cell->stats().zonesFenced == 1);
    CHECK(c.gw->stats().zoneFenced == 1);
    CHECK_FALSE(p.stats().routeStates.empty());
    CHECK(c.gw->session(81)->leaseGen > gen1);
    CHECK(zoneOf(c, 1002)->leaseGen() == c.gw->session(81)->leaseGen);
    CHECK(p.isConnected()); // never disconnected
    p.sendEcho(std::vector<u8>{3}, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 1; }, 500));
}

TEST_CASE("server.e2e: losing the cell re-routes sessions without disconnecting them") {
    Cluster c;
    ProbeClient& p = c.probe(91, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    c.cell.reset(); // crash (the virtual socket goes away with it)
    REQUIRE(c.stepUntil([&] { return c.gw->session(91)->state == GatewayServer::SessionState::Resolving; }, 12'000));
    CHECK(p.isConnected());
    c.startCell({StaticZone{1002, "tallis", 20}});
    REQUIRE(c.stepUntil([&] { return p.stats().welcomes == 2; }, 8000));
    p.sendEcho(std::vector<u8>{4}, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 1; }, 500));
}

TEST_CASE("server.e2e: 04 §9 limits: message rate, oversized and malformed messages") {
    Cluster::Options o;
    o.gateway.messagesPerSecond = 20;
    o.gateway.messageBurst = 10;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(101, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    for (int i = 0; i < 40; ++i) p.sendEcho(std::vector<u8>{1}, c.now);
    c.step(500);
    CHECK(c.gw->stats().rateLimited >= 20);
    CHECK(p.stats().echoesReceived >= 10);
    CHECK(p.stats().echoesReceived <= 21);

    ProbeClient& m = c.probe(102, 1, 1002);
    REQUIRE(c.stepUntil([&] { return m.isWelcomed(); }, 2000));
    const std::vector<u8> big(proto::kMaxForwardPayload + 1, 0xAB);
    CHECK(m.send(net::Channel::EventReliable, big) == net::SendResult::Ok);
    CHECK(m.send(net::Channel::Control, std::vector<u8>{0x44}) == net::SendResult::Ok);
    CHECK(m.send(net::Channel::Control, std::vector<u8>{0x01, 2}) == net::SendResult::Ok);
    REQUIRE(c.stepUntil([&] { return m.stats().kick.has_value(); }, 500));
    CHECK(m.stats().kick->reason == proto::KickReason::Malformed);
    CHECK(c.gw->stats().oversized == 1);
    REQUIRE(c.stepUntil([&] { return !m.isConnected(); }, 1000));
}

TEST_CASE("server.e2e: tokens without Helios user data are refused") {
    Cluster c;
    ProbeConfig pc;
    pc.transport = c.network.bind(net::Address::ipv4(10, 0, 3, 3, 5000)).value();
    c.probes.push_back(ProbeClient::create(std::move(pc), c.now).value());
    ProbeClient& p = *c.probes.back();
    net::ConnectTokenParams t;
    t.protocolId = kDefaultClientProtocolId;
    t.clientId = 5;
    t.publicAddresses = {kGatewayAddr};
    t.privateKey = c.shardKey; // user data left all zero: layout version 0
    REQUIRE(p.connect(net::generateConnectToken(t).value(), c.now));
    REQUIRE(c.stepUntil([&] { return p.stats().kick.has_value(); }, 1000));
    CHECK(p.stats().kick->reason == proto::KickReason::BadToken);
    CHECK(c.gw->stats().badTokens == 1);
    CHECK(c.gw->sessionCount() == 0);
}

TEST_CASE("server.e2e: the cell drops forwards from a trunk that no longer carries the session") {
    Cluster c;
    ProbeClient& p = c.probe(111, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    // A second gateway process (same keys) attaches the same session at a higher epoch: the cell
    // rebinds it there and fences the first gateway's trunk for this session.
    GatewayConfig g2;
    g2.name = "gw-two";
    const net::Address gw2Addr = net::Address::ipv4(10, 0, 0, 5, 7777);
    g2.listen = gw2Addr;
    g2.shardKey = c.shardKey;
    g2.trunkKey = c.trunkKey;
    g2.clientTransport = c.network.bind(gw2Addr).value();
    g2.trunkTransportFactory = [&c] { return std::unique_ptr<net::IDatagramTransport>(c.network.bind(net::Address::ipv4(10, 0, 0, 5, 0)).value()); };
    g2.staticRoutes = {StaticRoute{0, kCellAddr}};
    auto gw2 = GatewayServer::create(std::move(g2), c.now).value();
    ProbeConfig pc;
    pc.transport = c.network.bind(net::Address::ipv4(10, 0, 4, 4, 5000)).value();
    auto p2 = ProbeClient::create(std::move(pc), c.now).value();
    DevTokenParams t;
    t.protocolId = kDefaultClientProtocolId;
    t.key = c.shardKey;
    t.sessionId = 111;
    t.gateways = {gw2Addr};
    t.user.sessionEpoch = 2;
    t.user.zoneId = 1002;
    REQUIRE(p2->connect(mintDevToken(t).value(), c.now));
    for (int i = 0; i < 2000 && !p2->isWelcomed(); ++i) {
        c.step(1);
        gw2->update(c.now);
        p2->update(c.now);
    }
    REQUIRE(p2->isWelcomed());
    CHECK(zoneOf(c, 1002)->sessions().at(111).epoch == 2);
    const u64 dropped = c.cell->stats().forwardsDropped;
    p.sendEcho(std::vector<u8>{1}, c.now); // through the first gateway: fenced at the cell
    p2->sendEcho(std::vector<u8>{2}, c.now);
    for (int i = 0; i < 300; ++i) {
        c.step(1);
        gw2->update(c.now);
        p2->update(c.now);
    }
    CHECK(c.cell->stats().forwardsDropped == dropped + 1);
    CHECK(p.stats().echoesReceived == 0);
    CHECK(p2->stats().echoesReceived == 1);
}

namespace {
/// A hand-driven "cell" that speaks the trunk protocol, to check what the gateway does with
/// deliveries from a superseded lease generation and for sessions it did not attach there.
struct ScriptedCell final : net::IEndpointHandler {
    std::unique_ptr<net::Server> server;
    net::SessionHandle trunk;
    std::vector<std::pair<u64, proto::Attach>> attaches;
    std::vector<std::pair<u64, proto::TrunkType>> log; ///< Every session message, in arrival order.
    void onConnected(net::SessionHandle s) override { trunk = s; }
    void onMessage(net::SessionHandle, net::Channel, std::span<const u8> p) override {
        auto m = proto::parseTrunk(p);
        if (!m) return;
        if (m->stream != 0) log.emplace_back(m->stream, m->type);
        if (m->type == proto::TrunkType::Attach) attaches.emplace_back(m->stream, *proto::decodeAttach(m->body));
    }
};

/// A gateway with one probe session routed to a ScriptedCell over a VirtualNetwork.
struct ScriptedRig {
    net::VirtualNetwork network;
    ScriptedCell cell;
    std::unique_ptr<GatewayServer> gw;
    std::unique_ptr<ProbeClient> probe;
    i64 now = 1000 * kMs;

    explicit ScriptedRig(GatewayConfig g) {
        const net::Key shard = insecureDevKey("s");
        const net::Key trunkKey = insecureDevKey("t");
        net::ServerConfig sc;
        sc.protocolId = kDefaultTrunkProtocolId;
        sc.privateKey = trunkKey;
        sc.connection = net::ConnectionConfig::trunk();
        sc.transport = network.bind(kCellAddr).value();
        cell.server = net::Server::create(std::move(sc), 1.0).value();
        g.listen = kGatewayAddr;
        g.shardKey = shard;
        g.trunkKey = trunkKey;
        g.clientTransport = network.bind(kGatewayAddr).value();
        g.trunkTransportFactory = [this] {
            return std::unique_ptr<net::IDatagramTransport>(network.bind(net::Address::ipv4(10, 0, 0, 1, 0)).value());
        };
        g.staticRoutes = {StaticRoute{0, kCellAddr}};
        gw = GatewayServer::create(std::move(g), now).value();
        ProbeConfig pc;
        pc.transport = network.bind(net::Address::ipv4(10, 0, 1, 1, 5000)).value();
        probe = ProbeClient::create(std::move(pc), now).value();
        DevTokenParams t;
        t.protocolId = kDefaultClientProtocolId;
        t.key = shard;
        t.sessionId = 5;
        t.gateways = {kGatewayAddr};
        t.user.sessionEpoch = 1;
        t.user.zoneId = 1002;
        REQUIRE(probe->connect(mintDevToken(t).value(), now));
    }
    void step(int ms) {
        for (int i = 0; i < ms; ++i) {
            now += kMs;
            cell.server->update(static_cast<f64>(now) * 1e-9, cell);
            cell.server->flush(static_cast<f64>(now) * 1e-9);
            gw->update(now);
            probe->update(now);
        }
    }
    void send(const std::vector<u8>& msg, net::Channel channel = net::Channel::EventReliable) {
        (void)cell.server->send(cell.trunk, channel, msg);
    }
};
} // namespace

TEST_CASE("server.e2e: gateways fence deliveries below the attached lease generation") {
    net::VirtualNetwork network;
    const net::Key shard = insecureDevKey("s");
    const net::Key trunkKey = insecureDevKey("t");
    ScriptedCell cell;
    {
        net::ServerConfig sc;
        sc.protocolId = kDefaultTrunkProtocolId;
        sc.privateKey = trunkKey;
        sc.connection = net::ConnectionConfig::trunk();
        sc.transport = network.bind(kCellAddr).value();
        cell.server = net::Server::create(std::move(sc), 1.0).value();
    }
    GatewayConfig g;
    g.listen = kGatewayAddr;
    g.shardKey = shard;
    g.trunkKey = trunkKey;
    g.clientTransport = network.bind(kGatewayAddr).value();
    g.trunkTransportFactory = [&network] { return std::unique_ptr<net::IDatagramTransport>(network.bind(net::Address::ipv4(10, 0, 0, 1, 0)).value()); };
    g.staticRoutes = {StaticRoute{0, kCellAddr}};
    i64 now = 1000 * kMs;
    auto gw = GatewayServer::create(std::move(g), now).value();
    ProbeConfig pc;
    pc.transport = network.bind(net::Address::ipv4(10, 0, 1, 1, 5000)).value();
    auto p = ProbeClient::create(std::move(pc), now).value();
    DevTokenParams t;
    t.protocolId = kDefaultClientProtocolId;
    t.key = shard;
    t.sessionId = 5;
    t.gateways = {kGatewayAddr};
    t.user.sessionEpoch = 1;
    t.user.zoneId = 1002;
    REQUIRE(p->connect(mintDevToken(t).value(), now));
    auto step = [&](int ms) {
        for (int i = 0; i < ms; ++i) {
            now += kMs;
            cell.server->update(static_cast<f64>(now) * 1e-9, cell);
            cell.server->flush(static_cast<f64>(now) * 1e-9);
            gw->update(now);
            p->update(now);
        }
    };
    for (int i = 0; i < 2000 && cell.attaches.empty(); ++i) step(1);
    REQUIRE(cell.attaches.size() == 1);
    CHECK(cell.attaches[0].first == 5);
    CHECK(cell.attaches[0].second.zoneId == 1002);
    (void)cell.server->send(cell.trunk, net::Channel::EventReliable, proto::encode(5, proto::AttachAck{1, 1002, 7, 100, 20, 1'000'000, "tallis"}));
    for (int i = 0; i < 500 && !p->isWelcomed(); ++i) step(1);
    REQUIRE(p->isWelcomed());
    auto echo = [](u32 seq) { return proto::encodeEchoReply(seq, 1, {}); };
    (void)cell.server->send(cell.trunk, net::Channel::EventReliable, proto::encodeDeliver(5, 6, net::Channel::EventReliable, echo(1))); // stale gen
    (void)cell.server->send(cell.trunk, net::Channel::EventReliable, proto::encodeDeliver(5, 7, net::Channel::EventReliable, echo(2)));
    (void)cell.server->send(cell.trunk, net::Channel::EventReliable, proto::encodeDeliver(6, 7, net::Channel::EventReliable, echo(3))); // not attached
    step(100);
    CHECK(gw->stats().fencedDeliveries == 1);
    CHECK(gw->stats().strayDeliveries == 1);
    CHECK(gw->stats().delivered == 1);
    CHECK(p->stats().echoMismatches == 1); // the one delivery that got through (no request was sent)
}

TEST_CASE("server.e2e: real UDP loopback sockets") {
    const net::Key shard = insecureDevKey("udp-shard");
    const net::Key trunkKey = insecureDevKey("udp-trunk");
    CellServerConfig cc;
    cc.name = "udp-cell";
    cc.trunkBind = net::Address::ipv4(127, 0, 0, 1, 0);
    cc.trunkKey = trunkKey;
    cc.staticZones = {StaticZone{1002, "tallis", 20}};
    cc.jobs = &e2eJobs();
    i64 now = static_cast<i64>(monotonicNanos());
    auto cell = CellServer::create(std::move(cc), now).value();
    GatewayConfig g;
    g.listen = net::Address::ipv4(127, 0, 0, 1, 0);
    g.shardKey = shard;
    g.trunkKey = trunkKey; // trunkBind left unspecified: a loopback cell gets a loopback trunk socket
    g.staticRoutes = {StaticRoute{0, cell->trunkAddress()}};
    auto gw = GatewayServer::create(std::move(g), now).value();
    REQUIRE(gw->address().port() != 0);
    ProbeConfig pcfg;
    pcfg.bind = net::Address::loopbackV4();
    auto p = ProbeClient::create(std::move(pcfg), now).value();
    CHECK(p->client().transport().localAddress().isLoopback());
    DevTokenParams t;
    t.protocolId = kDefaultClientProtocolId;
    t.key = shard;
    t.sessionId = 1;
    t.gateways = {gw->address()};
    t.user.sessionEpoch = 1;
    t.user.zoneId = 1002;
    REQUIRE(p->connect(mintDevToken(t).value(), now));
    auto pump = [&] {
        now = static_cast<i64>(monotonicNanos());
        cell->update(now);
        gw->update(now);
        p->update(now);
        sleepMillis(1);
    };
    const u64 deadline = monotonicNanos() + 10'000'000'000ull;
    while (!p->isWelcomed() && monotonicNanos() < deadline) pump();
    REQUIRE(p->isWelcomed());
    // Nothing listens beyond loopback on a dev box (no Windows Firewall prompt, 04 §1).
    CHECK(gw->session(1)->trunkLocal.isLoopback());
    CHECK(gw->session(1)->trunkLocal.port() != 0);
    p->sendEcho(std::vector<u8>{'u', 'd', 'p'}, now);
    while (p->stats().echoesReceived == 0 && monotonicNanos() < deadline) pump();
    CHECK(p->stats().echoesReceived == 1);
    while (p->stats().tickStates < 5 && monotonicNanos() < deadline) pump();
    CHECK(p->stats().tickStates >= 5);
}

TEST_CASE("server.e2e: the cell refuses trunks whose token does not name a gateway") {
    Cluster c;
    net::ClientConfig cc;
    cc.connection = net::ConnectionConfig::trunk();
    cc.transport = c.network.bind(net::Address::ipv4(10, 0, 7, 7, 4000)).value();
    auto rogue = net::Client::create(std::move(cc), static_cast<f64>(c.now) * 1e-9).value();
    net::ConnectTokenParams t;
    t.protocolId = kDefaultTrunkProtocolId;
    t.clientId = 77;
    t.publicAddresses = {kCellAddr};
    t.privateKey = c.trunkKey; // the right key, but user data that names no gateway
    REQUIRE(rogue->connect(net::generateConnectToken(t).value(), static_cast<f64>(c.now) * 1e-9));
    struct Sink final : net::IEndpointHandler {
        void onMessage(net::SessionHandle, net::Channel, std::span<const u8>) override {}
    } sink;
    bool everConnected = false;
    for (int i = 0; i < 500; ++i) {
        c.step(1);
        rogue->update(static_cast<f64>(c.now) * 1e-9, sink);
        rogue->flush(static_cast<f64>(c.now) * 1e-9);
        everConnected |= rogue->isConnected();
        if (everConnected && !rogue->isConnected()) break;
    }
    CHECK(c.cell->stats().trunksConnected == 0);
    CHECK_FALSE(rogue->isConnected());
    // A real gateway's trunk still works.
    ProbeClient& p = c.probe(5, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    CHECK(c.cell->stats().trunksConnected == 1);
}

// Review regressions -------------------------------------------------------------------------

TEST_CASE("server.e2e: a slow SealReconnectTickets reply never reaches a newer connection of the session") {
    Cluster::Options o;
    o.bus = true;
    o.gateway.ticketIntervalMs = 1000;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(301, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 3000));

    // The seal request for epoch 1 is slow; meanwhile the player's connection drops and comes back
    // through this same gateway with a reconnect token at epoch 2.
    c.sessions->holdSeals = true;
    REQUIRE(c.stepUntil([&] { return c.sessions->heldSeals() == 1; }, 1500));
    p.disconnect();
    REQUIRE(c.stepUntil([&] { return !c.gw->session(301).has_value(); }, 1000));
    ProbeClient& again = c.probe(301, 2, 1002); // the Session service is at epoch 2 now
    REQUIRE(c.stepUntil([&] { return again.isWelcomed(); }, 2000));
    // The service answers from its current state: 301 at epoch 1 is "missing". That is about the
    // old connection; the new one must not be kicked.
    c.sessions->releaseSeals();
    c.step(300);
    CHECK_FALSE(again.stats().kick.has_value());
    CHECK(again.isConnected());
    CHECK(c.gw->stats().staleSealResults == 1);

    // Same race, answered the other way: the service still sealed at the old epoch, so the ticket
    // (which the service would refuse to redeem) must not go to the newer connection either.
    REQUIRE(c.stepUntil([&] { return c.sessions->heldSeals() == 1; }, 1500)); // epoch 2's request
    again.disconnect();
    REQUIRE(c.stepUntil([&] { return !c.gw->session(301).has_value(); }, 1000));
    ProbeClient& third = c.probe(301, 3, 1002);
    REQUIRE(c.stepUntil([&] { return third.isWelcomed(); }, 2000));
    c.sessions->sessions[301] = 2; // answered before the reconnect's CAS: a ticket for epoch 2
    c.sessions->holdSeals = false;
    c.sessions->releaseSeals();
    c.sessions->sessions[301] = 3;
    c.step(300);
    CHECK(third.stats().tickets.empty());
    CHECK(c.gw->stats().staleSealResults == 2);
    // The next round is for epoch 3 and does reach it.
    REQUIRE(c.stepUntil([&] { return !third.stats().tickets.empty(); }, 1500));
    CHECK_FALSE(third.stats().kick.has_value());
}

TEST_CASE("server.e2e: unused trunks are retired and a new session opens a new one") {
    Cluster::Options o;
    o.gateway.trunkIdleCloseMs = 2000;
    Cluster c(std::move(o));
    ProbeClient& p = c.probe(401, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 2000));
    CHECK(c.gw->trunkCount() == 1);
    p.disconnect();
    REQUIRE(c.stepUntil([&] { return c.gw->sessionCount() == 0; }, 500));
    c.step(1000);
    CHECK(c.gw->trunkCount() == 1); // idle, but not for long enough yet
    REQUIRE(c.stepUntil([&] { return c.gw->trunkCount() == 0; }, 2000));
    CHECK(c.gw->stats().trunksRetired == 1);
    REQUIRE(c.stepUntil([&] { return c.cell->stats().trunksDisconnected == 1; }, 2000)); // closed, not abandoned

    ProbeClient& q = c.probe(402, 1, 1002);
    REQUIRE(c.stepUntil([&] { return q.isWelcomed(); }, 2000));
    CHECK(c.gw->trunkCount() == 1);
    // A cell that dies while nobody needs its trunk: the trunk is not retried forever.
    q.disconnect();
    REQUIRE(c.stepUntil([&] { return c.gw->sessionCount() == 0; }, 500));
    c.cell.reset();
    REQUIRE(c.stepUntil([&] { return c.gw->trunkCount() == 0; }, 15'000));
    const u64 opened = c.gw->stats().trunksOpened;
    c.step(10'000);
    CHECK(c.gw->stats().trunksOpened == opened);
}

TEST_CASE("server.e2e: a re-route releases the session at the old cell before attaching again") {
    GatewayConfig g;
    g.attachTimeoutMs = 300;
    ScriptedRig rig(std::move(g));
    for (int i = 0; i < 2000 && rig.cell.attaches.size() < 2; ++i) rig.step(1); // the cell never answers
    REQUIRE(rig.cell.attaches.size() >= 2);
    // Attach, (attach timed out) Detach, Attach: the old binding is released at the cell.
    std::vector<proto::TrunkType> types;
    for (const auto& [stream, type] : rig.cell.log)
        if (stream == 5) types.push_back(type);
    REQUIRE(types.size() >= 3);
    CHECK(types[0] == proto::TrunkType::Attach);
    CHECK(types[1] == proto::TrunkType::Detach);
    CHECK(types[2] == proto::TrunkType::Attach);
}

TEST_CASE("server.e2e: ZoneFenced re-routes only sessions attached under the fenced generation") {
    ScriptedRig rig(GatewayConfig{});
    for (int i = 0; i < 2000 && rig.cell.attaches.empty(); ++i) rig.step(1);
    REQUIRE(rig.cell.attaches.size() == 1);
    rig.send(proto::encode(5, proto::AttachAck{1, 1002, 7, 100, 20, 1'000'000, "tallis"}));
    for (int i = 0; i < 500 && !rig.probe->isWelcomed(); ++i) rig.step(1);
    REQUIRE(rig.probe->isWelcomed());
    // The cell gives up an older generation of the zone (e.g. a late notice): not this session's.
    rig.send(proto::encode(0, proto::ZoneFenced{1002, 6}), net::Channel::Control);
    rig.step(100);
    CHECK(rig.gw->session(5)->state == GatewayServer::SessionState::Active);
    CHECK(rig.probe->stats().routeStates.empty());
    // The generation it is attached under: re-routed without a disconnect.
    rig.send(proto::encode(0, proto::ZoneFenced{1002, 7}), net::Channel::Control);
    for (int i = 0; i < 200 && rig.probe->stats().routeStates.empty(); ++i) rig.step(1);
    CHECK(rig.probe->stats().routeStates.size() == 1);
    CHECK(rig.gw->session(5)->state != GatewayServer::SessionState::Active);
    CHECK(rig.probe->isConnected());
}

TEST_CASE("server.cell: invalid zone settings fail at start, not when a zone is assigned later") {
    FakeBus bus;
    auto make = [&](auto tweak) {
        CellServerConfig c;
        c.name = "cfg-cell";
        c.trunkBind = net::Address::ipv4(127, 0, 0, 1, 0);
        c.trunkKey = insecureDevKey("cfg");
        c.jobs = &e2eJobs();
        c.bus = &bus; // orchestrated: zones would only be created once assigned
        tweak(c);
        return CellServer::create(std::move(c), 0);
    };
    CHECK(make([](CellServerConfig&) {}));
    CHECK(make([](CellServerConfig& c) { c.defaultTickHz = 0; }).errorCode() == ErrorCode::InvalidArgument);
    CHECK(make([](CellServerConfig& c) { c.defaultTickHz = 61; }).errorCode() == ErrorCode::InvalidArgument);
    CHECK(make([](CellServerConfig& c) { c.zoneTickHz = {{"tallis", 120}}; }).errorCode() == ErrorCode::InvalidArgument);
    CHECK(make([](CellServerConfig& c) { c.tidi.floor = 0.0; }).errorCode() == ErrorCode::InvalidArgument);
    CHECK(make([](CellServerConfig& c) { c.zoneTickHz = {{"harrow", 10}}; }));
}

TEST_CASE("conformance/holder_rule: a cell keeps its zones through a 60 s control-plane outage (CONF-03)") {
    // 09 §5.10 CONF-03 / 05 §1.4.2: with the control plane unreachable for 60 s the C++ cell host
    // keeps simulating its regions, and drops one only after seeing a higher lease_gen.
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    REQUIRE(c.stepUntil([&] { return c.cell->host().zoneCount() == 2; }, 2000));
    ProbeClient& p = c.probe(501, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 3000));
    const u64 gen1002 = zoneOf(c, 1002)->leaseGen();
    const u64 gen1003 = zoneOf(c, 1003)->leaseGen();
    const u64 ticksBefore = zoneOf(c, 1002)->clock().tick();

    c.orch->silent = true; // partitioned: every heartbeat times out
    c.step(60'000);
    CHECK(c.cell->orchestrator()->stats().heartbeatFailures >= 50);
    CHECK(c.cell->orchestrator()->stats().leaseLost == 0);
    REQUIRE(zoneOf(c, 1002) != nullptr);
    REQUIRE(zoneOf(c, 1003) != nullptr);
    CHECK(zoneOf(c, 1002)->leaseGen() == gen1002);
    CHECK(zoneOf(c, 1002)->clock().tick() - ticksBefore >= 20 * 60 - 5); // kept ticking at 20 Hz
    CHECK(c.cell->stats().zonesFenced == 0);
    CHECK(p.isConnected());
    p.sendEcho(std::vector<u8>{9}, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 1; }, 500)); // still served

    // The control plane is back and has moved 1002 to a new generation: only 1002 is dropped
    // (and hosted again under the new generation); 1003 is untouched.
    c.orch->silent = false;
    c.orch->bumpGeneration(1002);
    REQUIRE(c.stepUntil([&] { return zoneOf(c, 1002) && zoneOf(c, 1002)->leaseGen() > gen1002; }, 3000));
    CHECK(c.cell->stats().zonesFenced == 1);
    CHECK(zoneOf(c, 1003)->leaseGen() == gen1003);
    CHECK(c.cell->orchestrator()->stats().leaseLost == 0);
}

TEST_CASE("server.cell: the heartbeat's tickP99Ms is a percentile of recent ticks, not the last one") {
    Cluster::Options o;
    o.bus = true;
    Cluster c(std::move(o));
    REQUIRE(c.stepUntil([&] { return c.cell->host().zoneCount() == 2; }, 2000));
    ZoneInstance* z = zoneOf(c, 1002);
    int spikes = 0;
    REQUIRE(z->graph().addHook({"spike", Stage::PostPhysics, [&spikes](TickContext&) {
                                    if (spikes++ == 40) sleepMillis(30); // one slow tick among many
                                }, HookThread::Tick, {}}));
    REQUIRE(c.stepUntil([&] { return spikes > 100; }, 8000));
    std::vector<i64> all;
    for (const ZoneInstance* zone : std::as_const(c.cell->host()).zones())
        for (i64 t : zone->recentTickNs()) all.push_back(t);
    REQUIRE(all.size() >= 100);
    std::sort(all.begin(), all.end());
    const usize rank = (all.size() * 99 + 99) / 100 - 1; // nearest rank
    CHECK(c.cell->tickP99Ns() == all[rank]);
    CHECK(all.back() >= 30 * kMs); // the spike is in the window (it was not the last tick)
    CHECK(c.cell->tickP99Ns() <= all.back());
}

TEST_CASE("server.e2e: a session routed at a new generation but acked by the old zone instance is re-routed") {
    // ResolveZone already names generation g2 while the cell, until its next heartbeat, still runs
    // the zone at g1: the attach is acked by the g1 instance. When the cell fences g1 the session
    // must follow the zone to g2 (it would otherwise sit "active" on a destroyed instance).
    Cluster::Options o;
    o.bus = true;
    o.gateway.routeCacheMs = 0; // resolve every time
    Cluster c(std::move(o));
    REQUIRE(c.stepUntil([&] { return c.cell->host().zoneCount() == 2; }, 2000));
    const u64 g1 = zoneOf(c, 1002)->leaseGen();
    const u64 hb = c.orch->heartbeats;
    REQUIRE(c.stepUntil([&] { return c.orch->heartbeats > hb; }, 2000)); // just after a heartbeat
    c.orch->bumpGeneration(1002);
    const u64 g2 = c.orch->zone(1002).leaseGen;
    ProbeClient& p = c.probe(601, 1, 1002);
    REQUIRE(c.stepUntil([&] { return p.isWelcomed(); }, 500));
    REQUIRE(zoneOf(c, 1002)->leaseGen() == g1); // the cell has not heard of g2 yet
    // The cell's next heartbeat fences g1 and hosts g2; the session re-attaches there.
    REQUIRE(c.stepUntil([&] { return p.stats().welcomes == 2; }, 3000));
    CHECK(zoneOf(c, 1002)->leaseGen() == g2);
    CHECK(zoneOf(c, 1002)->sessions().count(601) == 1);
    p.sendEcho(std::vector<u8>{6}, c.now);
    REQUIRE(c.stepUntil([&] { return p.stats().echoesReceived == 1; }, 500));
}
