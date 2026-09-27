// FakeBus semantics and the orchestrator client: registration, heartbeats, the holder rule,
// lease_lost, generation changes, ID blocks into an EntityId minter, ResolveZone (05 §1.4).
#include <doctest/doctest.h>

#include "fake_services.h"
#include "helios/server/app_env.h"
#include "helios/server/cell_server.h"
#include "helios/server/gateway_server.h"
#include "helios/server/orchestrator_client.h"

using namespace helios;
using namespace helios::server;
using namespace helios::server::test;

namespace {
constexpr i64 kMs = 1'000'000;

struct Rig {
    FakeBus bus;
    FakeOrchestrator orch{bus, "dev", {{1002, "tallis"}, {1003, "harrow"}}};
    i64 now = 1000 * kMs;
    std::vector<authority::LeaseEvent> events;

    /// Steps simulated time by `ms` milliseconds, 1 ms at a time, pumping the bus.
    void run(OrchestratorClient& c, i64 ms) {
        for (i64 i = 0; i < ms; ++i) {
            now += kMs;
            bus.pump(now / kMs);
            auto ev = c.update(now);
            events.insert(events.end(), ev.begin(), ev.end());
        }
    }
    u32 count(authority::LeaseEventKind k) const {
        u32 n = 0;
        for (auto& e : events) n += e.kind == k ? 1u : 0u;
        return n;
    }
};

OrchestratorClientConfig cellConfig(std::string name = "cell-a", std::vector<std::string> zones = {}) {
    OrchestratorClientConfig c;
    c.shard = "dev";
    c.info.name = std::move(name);
    c.info.kind = "cell";
    c.info.address = "127.0.0.1:7810";
    c.info.zones = std::move(zones);
    return c;
}
} // namespace

TEST_CASE("server.fakebus: request/reply, wildcards, no responders, timeouts and partitions") {
    FakeBus bus;
    u32 seen = 0;
    auto sub = bus.subscribe("rpc.dev.>", [&](const BusMessage& m) {
        ++seen;
        CHECK(m.header(kHeaderDeadlineMs) == "250");
        if (m.subject == "rpc.dev.echo") (void)bus.publish(m.reply, m.data);
    });
    REQUIRE(sub);
    std::vector<BusReply> replies;
    auto keep = [&](BusReply r) { replies.push_back(std::move(r)); };
    bus.request("rpc.dev.echo", orch::bytesOf("ping"), {}, std::chrono::milliseconds(250), keep);
    bus.request("rpc.dev.silent", {}, {}, std::chrono::milliseconds(250), keep);
    bus.request("nobody.home", {}, {}, std::chrono::milliseconds(250), keep);
    CHECK(replies.empty()); // nothing is delivered from inside request()
    bus.pump(0);
    REQUIRE(replies.size() == 2);
    // "nobody.home" fails in the first delivery round, the echo's reply arrives in the second.
    CHECK(replies[0].status == BusStatus::NoResponders);
    CHECK(replies[1].ok());
    CHECK(orch::textOf(replies[1].message.data) == "ping");
    CHECK(seen == 2);
    bus.pump(249);
    CHECK(replies.size() == 2);
    bus.pump(250);
    REQUIRE(replies.size() == 3);
    CHECK(replies[2].status == BusStatus::Timeout);

    bus.setConnected(false);
    bus.request("rpc.dev.echo", {}, {}, std::chrono::milliseconds(250), keep);
    CHECK_FALSE(bus.publish("x.y", {}));
    bus.pump(300);
    REQUIRE(replies.size() == 4);
    CHECK(replies[3].status == BusStatus::Disconnected);
    bus.setConnected(true);

    CHECK(subjectMatches("ctl.dev.gateway.all.*", "ctl.dev.gateway.all.kick"));
    CHECK_FALSE(subjectMatches("ctl.dev.gateway.all.*", "ctl.dev.gateway.all.kick.x"));
    CHECK(subjectMatches("rpc.>", "rpc.dev.orch.Heartbeat"));
    CHECK_FALSE(subjectMatches("rpc.>", "rpc"));
    CHECK_FALSE(subjectMatches("a.b", "a.b.c"));
    CHECK(subjectMatches("a.*.c", "a.b.c"));
    bus.unsubscribe(*sub);
}

TEST_CASE("server.orch: a cell registers, is assigned zones and heartbeats") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig("cell-a", {"tallis"}));
    rig.run(client, 5);
    REQUIRE(client.isRegistered());
    CHECK(client.processId() != 0);
    CHECK(client.epoch() == 1);
    CHECK(rig.count(authority::LeaseEventKind::Acquired) == 1); // only the declared zone
    CHECK(client.leases().holds(authority::RegionId{1002}));
    CHECK_FALSE(client.leases().holds(authority::RegionId{1003}));
    CHECK(client.idShard() == 3);
    rig.run(client, 3000);
    CHECK(rig.orch.heartbeats >= 2);
    CHECK(client.stats().heartbeatsOk >= 2);
    CHECK(client.mode() == "normal");
    CHECK(rig.events.size() == 1); // steady state: no churn
}

TEST_CASE("server.orch: holder rule: an unreachable orchestrator never fences") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig());
    rig.run(client, 5);
    REQUIRE(client.leases().held().size() == 2); // accepts any zone
    rig.events.clear();

    rig.orch.silent = true; // partitioned: heartbeats time out
    rig.run(client, 20'000);
    CHECK(client.stats().heartbeatFailures >= 10);
    CHECK(client.leases().held().size() == 2);
    CHECK(rig.events.empty());

    rig.orch.silent = false;
    rig.orch.unavailable = true; // leadership change: "unavailable" is not a fence either
    rig.run(client, 3000);
    CHECK(client.leases().held().size() == 2);
    CHECK(rig.events.empty());

    rig.orch.unavailable = false;
    rig.bus.setConnected(false); // the bus itself is down
    rig.run(client, 3000);
    rig.bus.setConnected(true);
    CHECK(client.leases().held().size() == 2);
    CHECK(rig.events.empty());
    rig.run(client, 2000);
    CHECK(client.isRegistered());
    CHECK(client.leases().stats().unreachable >= 10);
}

TEST_CASE("server.orch: lease_lost fences every zone, then the process registers again") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig());
    rig.run(client, 5);
    const u64 firstId = client.processId();
    REQUIRE(client.leases().held().size() == 2);
    const auto gen1002 = client.leases().generation(authority::RegionId{1002});
    rig.events.clear();

    rig.orch.expire(firstId); // e.g. a new orchestrator leader: the registration is unknown
    rig.run(client, 1500);
    REQUIRE(rig.events.size() == 4);
    CHECK(rig.events[0].kind == authority::LeaseEventKind::Lost);
    CHECK(rig.events[0].reason == authority::LeaseLossReason::LeaseLost);
    CHECK(rig.events[1].kind == authority::LeaseEventKind::Lost);
    CHECK(rig.events[2].kind == authority::LeaseEventKind::Acquired);
    CHECK(rig.events[3].kind == authority::LeaseEventKind::Acquired);
    CHECK(client.stats().leaseLost == 1);
    CHECK(client.isRegistered());
    CHECK(client.processId() != firstId);
    CHECK(client.epoch() == 2); // per-name epoch
    CHECK(client.leases().generation(authority::RegionId{1002}) > gen1002);
}

TEST_CASE("server.orch: a zone that changes generation or disappears is lost") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig());
    rig.run(client, 5);
    rig.events.clear();
    rig.orch.bumpGeneration(1002);
    rig.run(client, 1100);
    REQUIRE(rig.events.size() == 2);
    CHECK(rig.events[0].reason == authority::LeaseLossReason::GenerationChanged);
    CHECK(rig.events[1].kind == authority::LeaseEventKind::Acquired);
    CHECK(rig.events[1].assignment.leaseGen == rig.orch.zone(1002).leaseGen);
    rig.events.clear();

    // Another cell registers declaring harrow: the orchestrator does not move an owned zone
    // (v0 placement is sticky), so nothing changes for cell-a.
    OrchestratorClient other(rig.bus, cellConfig("cell-b", {"harrow"}));
    for (int i = 0; i < 1100; ++i) {
        rig.now += kMs;
        rig.bus.pump(rig.now / kMs);
        auto ev = client.update(rig.now);
        rig.events.insert(rig.events.end(), ev.begin(), ev.end());
        (void)other.update(rig.now);
    }
    CHECK(rig.events.empty());
    CHECK(other.leases().held().empty());
}

TEST_CASE("server.orch: register failures back off and retry") {
    Rig rig;
    rig.orch.unavailable = true;
    OrchestratorClient client(rig.bus, cellConfig());
    rig.run(client, 1000);
    CHECK_FALSE(client.isRegistered());
    const u64 failures = client.stats().registerFailures;
    CHECK(failures >= 3);
    CHECK(failures <= 8); // 100, 200, 400, 800 ms ... not a tight loop
    rig.orch.unavailable = false;
    rig.run(client, 6000);
    CHECK(client.isRegistered());
}

TEST_CASE("server.orch: ID blocks flow from the pool and AllocateIdBlocks into a minter") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig());
    OrchestratorIdBlockSource source(client);
    std::vector<u64> out;
    CHECK(source.allocateIdBlocks(1, out).errorCode() == ErrorCode::InvalidState); // not registered
    rig.run(client, 5);
    REQUIRE(client.isRegistered());

    ecs::EntityIdMinter minter(ecs::EntityIdMinter::Desc{.shard = client.idShard(), .source = &source, .hold = 2});
    source.attach(&minter);
    // The registration pool (2 prefixes) serves the first blocks synchronously.
    REQUIRE(minter.prime());
    const ecs::EntityId first = minter.allocate();
    REQUIRE(first.isValid());
    CHECK(ecs::decodeBlockId(first).shard == 3);
    CHECK(ecs::decodeBlockId(first).prefix == 1001);
    // The pool is used up; the next request goes over the bus and answers Busy meanwhile.
    out.clear();
    CHECK(source.allocateIdBlocks(2, out).errorCode() == ErrorCode::Busy);
    CHECK(source.allocateIdBlocks(2, out).errorCode() == ErrorCode::Busy); // one request in flight
    rig.run(client, 2);
    CHECK(source.pump() == 2);
    CHECK(minter.lastPrefix() == 1004);
    CHECK(rig.orch.prefixesIssued == 4);
}

TEST_CASE("server.orch: ResolveZone and graceful deregistration") {
    Rig rig;
    OrchestratorClient client(rig.bus, cellConfig());
    rig.run(client, 5);
    std::optional<Result<orch::Route>> byId;
    std::optional<Result<orch::Route>> byName;
    std::optional<Result<orch::Route>> unknown;
    client.resolveZone(1002, {}, [&](Result<orch::Route> r) { byId = std::move(r); });
    client.resolveZone(0, "harrow", [&](Result<orch::Route> r) { byName = std::move(r); });
    client.resolveZone(9999, {}, [&](Result<orch::Route> r) { unknown = std::move(r); });
    rig.run(client, 2);
    REQUIRE(byId);
    REQUIRE(*byId);
    CHECK((*byId)->address == "127.0.0.1:7810");
    CHECK((*byId)->processId == client.processId());
    REQUIRE(byName);
    CHECK((*byName)->zoneId == 1003);
    REQUIRE(unknown);
    CHECK(unknown->errorCode() == ErrorCode::NotFound);

    auto released = client.shutdown(std::chrono::milliseconds(0));
    CHECK(released.size() == 2);
    rig.bus.pump(rig.now / kMs + 1);
    CHECK(rig.orch.processes().empty()); // deregistered: the zones are free again
    CHECK(rig.orch.zone(1002).owner == 0);
}

TEST_CASE("server.orch: registration carries fd and serverBuild; heartbeats report held regions (05 §1.4)") {
    Rig rig;
    OrchestratorClientConfig cfg = cellConfig();
    cfg.info.fd = orch::FailureDomain{"az-1", "r12", "srv-07"};
    cfg.info.serverBuild = 42;
    OrchestratorClient client(rig.bus, cfg);
    rig.run(client, 5);
    REQUIRE(client.isRegistered());
    const u64 pid = client.processId();
    CHECK(rig.orch.processes().at(pid).info.fd == cfg.info.fd);
    CHECK(rig.orch.processes().at(pid).info.serverBuild == 42);

    // Every heartbeat lists the regions held, by region, with the generations held.
    auto heldNow = [&] {
        std::vector<orch::HeldRegion> out;
        for (u64 z : {1002ull, 1003ull}) out.push_back({z, rig.orch.zone(z).leaseGen});
        return out;
    };
    rig.run(client, 1100);
    REQUIRE(rig.orch.lastHeld.contains(pid));
    CHECK(rig.orch.lastHeld.at(pid) == heldNow());
    // A new generation for one region is reported from the heartbeat after the one that saw it.
    rig.orch.bumpGeneration(1003);
    rig.run(client, 2100);
    CHECK(client.leases().generation(authority::RegionId{1003}) == rig.orch.zone(1003).leaseGen);
    CHECK(rig.orch.lastHeld.at(pid) == heldNow());

    // Unreachable: the list sent keeps the generations held (nothing was given up), which is
    // what the leader reconciles against region_lease when the control plane recovers (§1.4.4).
    const auto before = heldNow();
    rig.orch.unavailable = true;
    rig.run(client, 3000);
    rig.orch.unavailable = false;
    CHECK(rig.orch.lastHeld.at(pid) == before);

    // After lease_lost the new registration reports only what it was assigned again.
    rig.orch.expire(pid);
    rig.run(client, 2200);
    REQUIRE(client.isRegistered());
    CHECK(client.processId() != pid);
    CHECK(rig.orch.lastHeld.at(client.processId()) == heldNow());
    // A gateway holds no regions: `held` is left out of its heartbeats.
    OrchestratorClientConfig g;
    g.info.name = "gw-1";
    g.info.kind = "gateway";
    g.info.address = "127.0.0.1:7777";
    g.info.keyId = 1;
    OrchestratorClient gw(rig.bus, g);
    rig.run(gw, 1100);
    REQUIRE(gw.isRegistered());
    REQUIRE(rig.orch.lastHeld.contains(gw.processId()));
    CHECK(rig.orch.lastHeld.at(gw.processId()).empty());
}

TEST_CASE("server.app: placement options fill fd and serverBuild; malformed values fail at start") {
    auto parse = [](std::vector<std::string> argv) {
        argv.insert(argv.begin(), "helios-cell");
        return placementFromArgs(ProcessArgs(CommandLine::parse(argv)));
    };
    auto p = parse({"--fd-az=az-1", "--fd-rack", "r12", "--fd-host=srv-07", "--server-build", "9007199254740993"});
    REQUIRE(p);
    CHECK(p->fd == orch::FailureDomain{"az-1", "r12", "srv-07"});
    CHECK(p->serverBuild == 9007199254740993);
    CHECK_FALSE(parse({"--fd-az=az-1", "--server-build=12x"}));
    CHECK_FALSE(parse({"--fd-az=az-1", "--server-build=-3"}));
    CHECK_FALSE(parse({"--fd-az=az-1", "--server-build=99999999999999999999"}));
    CHECK_FALSE(parse({"--fd-rack=" + std::string(orch::kMaxFdLabel + 1, 'r')}));
    CHECK_FALSE(parse({"--fd-host=srv\xff"})); // an ANSI-code-page value, not UTF-8

    // A cell or gateway with a registration the orchestrator would refuse does not start.
    FakeBus bus;
    CellServerConfig cc;
    cc.bus = &bus;
    cc.trunkBind = net::Address::ipv4(127, 0, 0, 1, 0);
    cc.failureDomain.host = std::string(orch::kMaxFdHost + 1, 'h');
    CHECK(CellServer::create(std::move(cc), 0).errorCode() == ErrorCode::InvalidArgument);
    CellServerConfig latin1;
    latin1.bus = &bus;
    latin1.trunkBind = net::Address::ipv4(127, 0, 0, 1, 0);
    latin1.failureDomain.host = "srv\xff";
    CHECK(CellServer::create(std::move(latin1), 0).errorCode() == ErrorCode::InvalidArgument);
    CellServerConfig longZone;
    longZone.bus = &bus;
    longZone.trunkBind = net::Address::ipv4(127, 0, 0, 1, 0);
    longZone.declaredZones = {std::string(orch::kMaxZoneName + 1, 'z')};
    CHECK(CellServer::create(std::move(longZone), 0).errorCode() == ErrorCode::InvalidArgument);
    GatewayConfig gc;
    gc.bus = &bus;
    gc.listen = net::Address::ipv4(127, 0, 0, 1, 0);
    gc.serverBuild = -1;
    CHECK(GatewayServer::create(std::move(gc), 0).errorCode() == ErrorCode::InvalidArgument);
}
