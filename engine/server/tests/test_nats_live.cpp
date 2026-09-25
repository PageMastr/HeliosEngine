// Live NATS contract test against a running helios-backend (optional): runs only when
// HELIOS_NATS_URL is set (plus HELIOS_NATS_USER / HELIOS_NATS_PASSWORD for the `fleet` user, as the
// backend passes to its children). Start the backend with `go run ./cmd/helios-backend` in
// services/ and see engine/server/README.md.
#include <doctest/doctest.h>

#include <atomic>

#include "helios/core/time.h"
#include "helios/server/app_env.h"
#include "helios/server/nats_bus.h"
#include "helios/server/orchestrator_client.h"

using namespace helios;
using namespace helios::server;

namespace {
template <class Pred>
bool waitFor(Pred pred, f64 seconds) {
    const u64 deadline = monotonicNanos() + static_cast<u64>(seconds * 1e9);
    while (monotonicNanos() < deadline) {
        if (pred()) return true;
        sleepMillis(5);
    }
    return pred();
}

std::string uniqueName(std::string_view prefix) {
    const net::Key k = net::generateKey();
    return std::string(prefix) + "-" + base64Encode(std::span<const u8>(k.data(), 6));
}
} // namespace

TEST_CASE("server.nats-live: orchestrator and session contracts against helios-backend") {
    const auto url = envVar("HELIOS_NATS_URL");
    if (!url) {
        MESSAGE("HELIOS_NATS_URL not set: live NATS contract test skipped");
        return;
    }
    NatsBusConfig nc;
    nc.url = *url;
    nc.user = envVar("HELIOS_NATS_USER").value_or("fleet");
    nc.password = envVar("HELIOS_NATS_PASSWORD").value_or("");
    nc.name = "server_tests";
    auto bus = NatsBus::connect(nc);
    REQUIRE(bus);
    REQUIRE(waitFor([&] { return (*bus)->isConnected(); }, 5.0));
    const std::string shard = envVar("HELIOS_SHARD").value_or("dev");

    // A cell that declares a zone nobody configured, so it takes no real zone away.
    OrchestratorClientConfig cc;
    cc.shard = shard;
    cc.info.name = uniqueName("it-cell");
    cc.info.kind = "cell";
    cc.info.address = "127.0.0.1:1";
    cc.info.zones = {"it-no-such-zone"};
    OrchestratorClient cell(**bus, cc);
    REQUIRE(waitFor([&] { (void)cell.update(static_cast<i64>(monotonicNanos())); return cell.isRegistered(); }, 5.0));
    CHECK(cell.processId() != 0);
    CHECK(cell.epoch() >= 1);
    CHECK(cell.leases().held().empty());

    // ID blocks: the registration pool first, then AllocateIdBlocks over NATS.
    OrchestratorIdBlockSource source(cell);
    ecs::EntityIdMinter minter(ecs::EntityIdMinter::Desc{.shard = cell.idShard(), .source = &source, .hold = 2});
    source.attach(&minter);
    REQUIRE(minter.prime());
    const ecs::EntityId id = minter.allocate();
    REQUIRE(id.isValid());
    const i64 pooledLast = minter.lastPrefix();
    std::vector<u64> out;
    CHECK(source.allocateIdBlocks(1, out).errorCode() == ErrorCode::Busy);
    REQUIRE(waitFor([&] { return source.pump() == 1; }, 5.0));
    CHECK(minter.lastPrefix() > pooledLast);

    // Heartbeats keep the lease; the mode is reported.
    REQUIRE(waitFor([&] { (void)cell.update(static_cast<i64>(monotonicNanos())); return cell.stats().heartbeatsOk >= 1; }, 5.0));
    CHECK(cell.mode() == "normal");
    CHECK(cell.stats().leaseLost == 0);

    // ResolveZone answers with a route or a well-formed error.
    std::optional<Result<orch::Route>> route;
    cell.resolveZone(0, "it-no-such-zone", [&](Result<orch::Route> r) { route = std::move(r); });
    REQUIRE(waitFor([&] { return route.has_value(); }, 5.0));
    CHECK(route->errorCode() == ErrorCode::NotFound);

    // A gateway registers too (it must report its shard key generation).
    OrchestratorClientConfig gc;
    gc.shard = shard;
    gc.info.name = uniqueName("it-gw");
    gc.info.kind = "gateway";
    gc.info.address = "127.0.0.1:9";
    gc.info.keyId = 1;
    gc.info.capacity = 1;
    OrchestratorClient gw(**bus, gc);
    REQUIRE(waitFor([&] { (void)gw.update(static_cast<i64>(monotonicNanos())); return gw.isRegistered(); }, 5.0));

    // SealReconnectTickets: an unknown session is reported missing.
    orch::SealRequest seal;
    seal.sessions.push_back(orch::SealItem{0x1234'5678'9ABCull, 0, 1, {}});
    std::optional<BusReply> sealed;
    (*bus)->request(orch::sealTicketsSubject(shard), orch::encode(seal), {}, std::chrono::milliseconds(3000),
                    [&](BusReply r) { sealed = std::move(r); });
    REQUIRE(waitFor([&] { return sealed.has_value(); }, 5.0));
    REQUIRE(sealed->ok());
    auto sr = orch::decodeSealResponse(sealed->message.data);
    REQUIRE(sr);
    CHECK(sr->tickets.empty());
    CHECK(sr->missing == std::vector<u64>{0x1234'5678'9ABCull});

    // The gateway control subjects can be subscribed to by the fleet user.
    CHECK((*bus)->subscribe(orch::gatewayControlSubject(shard, "session_epoch"), [](const BusMessage&) {}));

    // A heartbeat from an unknown registration is refused with lease_lost (failed_precondition).
    std::optional<BusReply> stale;
    (*bus)->request(orch::orchestratorSubject(shard, "Heartbeat"), orch::encode(orch::HeartbeatRequest{999'999'999, 1, {}}), {},
                    std::chrono::milliseconds(3000), [&](BusReply r) { stale = std::move(r); });
    REQUIRE(waitFor([&] { return stale.has_value(); }, 5.0));
    CHECK(stale->status == BusStatus::Ok);
    CHECK(stale->errorCode() == orch::kCodeFailedPrecondition);
    CHECK(stale->errorMessage() == orch::kLeaseLost);

    (void)gw.shutdown(std::chrono::milliseconds(3000));
    (void)cell.shutdown(std::chrono::milliseconds(3000));
}

TEST_CASE("server.nats-live: unsubscribe waits for a running handler; a handler may unsubscribe itself") {
    const auto url = envVar("HELIOS_NATS_URL");
    if (!url) {
        MESSAGE("HELIOS_NATS_URL not set: live NATS subscription test skipped");
        return;
    }
    NatsBusConfig nc;
    nc.url = *url;
    nc.user = envVar("HELIOS_NATS_USER").value_or("fleet");
    nc.password = envVar("HELIOS_NATS_PASSWORD").value_or("");
    nc.name = "server_tests_subs";
    for (int round = 0; round < 3; ++round) { // the bus is destroyed each round: its closed callback is awaited
        auto bus = NatsBus::connect(nc);
        REQUIRE(bus);
        REQUIRE(waitFor([&] { return (*bus)->isConnected(); }, 5.0));
        const std::string subject = "tst." + uniqueName("sub") + ".x";
        // nats.c may still be inside the handler when natsSubscription_Unsubscribe() returns; the
        // bus must not return (and let the caller free what the handler uses) before it is done.
        std::atomic<int> state{0};
        auto sub = (*bus)->subscribe(subject, [&](const BusMessage&) {
            state = 1;
            sleepMillis(300);
            state = 2;
        });
        REQUIRE(sub);
        REQUIRE((*bus)->publish(subject, orch::bytesOf("x")));
        REQUIRE(waitFor([&] { return state.load() == 1; }, 5.0));
        (*bus)->unsubscribe(*sub);
        CHECK(state.load() == 2);

        // Unsubscribing from inside the handler neither deadlocks nor frees the running closure.
        const std::string self = "tst." + uniqueName("self") + ".x";
        std::atomic<u64> selfId{0};
        std::atomic<bool> done{false};
        auto sub2 = (*bus)->subscribe(self, [&](const BusMessage&) {
            (*bus)->unsubscribe(selfId.load());
            done = true;
        });
        REQUIRE(sub2);
        selfId = *sub2;
        REQUIRE((*bus)->publish(self, orch::bytesOf("y")));
        REQUIRE(waitFor([&] { return done.load(); }, 5.0));
        // A subscription left open is closed by the destructor.
        auto open = (*bus)->subscribe("tst." + uniqueName("open") + ".x", [](const BusMessage&) {});
        CHECK(open);
    }
}
