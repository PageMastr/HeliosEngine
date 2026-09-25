// Per-tick job graph, zone instances and the zone host (04 §3.1–3.3; NS-0.6).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "helios/core/jobs.h"
#include "helios/core/time.h"
#include "helios/server/zone_host.h"

using namespace helios;
using namespace helios::server;

namespace {
constexpr i64 kMs = 1'000'000;

jobs::JobSystem& testJobs() {
    static jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 3, .name = "ZoneTest"});
    return js;
}

ZoneDesc zoneDesc(u64 id, std::string name, u32 hz = 20) {
    ZoneDesc d;
    d.id = id;
    d.name = std::move(name);
    d.tickHz = hz;
    return d;
}

ZoneInboxItem attach(u64 session, u64 epoch, u64 route = 1) {
    ZoneInboxItem i;
    i.kind = ZoneInboxItem::Kind::Attach;
    i.sessionId = session;
    i.sessionEpoch = epoch;
    i.route = route;
    return i;
}
ZoneInboxItem message(u64 session, std::vector<u8> payload, u64 route = 1, net::Channel c = net::Channel::EventReliable) {
    ZoneInboxItem i;
    i.kind = ZoneInboxItem::Kind::Message;
    i.sessionId = session;
    i.route = route;
    i.channel = c;
    i.payload = std::move(payload);
    return i;
}

/// Runs the zone for `ticks` ticks at its own cadence starting after `now`; returns the new time.
i64 runTicks(ZoneInstance& z, i64 now, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i) {
        now = std::max(now, z.clock().nextTickWallNs());
        REQUIRE(z.tick(now).value());
    }
    return now;
}

} // namespace

TEST_CASE("server.tickgraph: stages run in order; hooks honour `after`") {
    TickGraph g;
    std::mutex mu;
    std::vector<std::string> log;
    auto rec = [&](std::string s) {
        return [&, s](TickContext&) {
            std::lock_guard lock(mu);
            log.push_back(s);
        };
    };
    // Registered out of order on purpose.
    REQUIRE(g.addHook({"send", Stage::Send, rec("send"), HookThread::Tick, {}}));
    REQUIRE(g.addHook({"gameplay", Stage::PostPhysics, rec("gameplay"), HookThread::Tick, {}}));
    REQUIRE(g.addHook({"sim.b", Stage::PrePhysics, rec("sim.b"), HookThread::Tick, {"sim.a"}}));
    REQUIRE(g.addHook({"sim.a", Stage::PrePhysics, rec("sim.a"), HookThread::Tick, {}}));
    REQUIRE(g.addHook({"input", Stage::Input, rec("input"), HookThread::Tick, {}}));
    REQUIRE(g.addHook({"physics", Stage::Physics, rec("physics"), HookThread::Tick, {}}));
    REQUIRE(g.addHook({"gather", Stage::ReplicationGather, rec("gather"), HookThread::Tick, {}}));
    CHECK_FALSE(g.addHook({"input", Stage::Input, rec("dup"), HookThread::Tick, {}})); // unique names
    TickContext ctx;
    REQUIRE(g.run(ctx, &testJobs()));
    CHECK(log == std::vector<std::string>{"input", "sim.a", "sim.b", "physics", "gameplay", "gather", "send"});
    CHECK(g.executionOrder(Stage::PrePhysics) == std::vector<std::string>{"sim.a", "sim.b"});
    CHECK(g.stageStats(Stage::Input).runs == 1);
}

TEST_CASE("server.tickgraph: parallel hooks run after the stage's tick hooks and before the next stage") {
    TickGraph g;
    std::atomic<int> phase{0};
    std::atomic<int> parallelSeen{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> concurrent{0};
    std::atomic<bool> orderOk{true};
    REQUIRE(g.addHook({"tick.first", Stage::PrePhysics, [&](TickContext&) { phase = 1; }, HookThread::Tick, {}}));
    for (int i = 0; i < 6; ++i) {
        REQUIRE(g.addHook({"par." + std::to_string(i), Stage::PrePhysics,
                           [&](TickContext&) {
                               if (phase.load() != 1) orderOk = false;
                               const int c = ++concurrent;
                               int m = maxConcurrent.load();
                               while (c > m && !maxConcurrent.compare_exchange_weak(m, c)) {}
                               const u64 until = monotonicNanos() + 2'000'000;
                               while (monotonicNanos() < until) {}
                               --concurrent;
                               ++parallelSeen;
                           },
                           HookThread::Any, {"tick.first"}}));
    }
    REQUIRE(g.addHook({"par.last", Stage::PrePhysics, [&](TickContext&) {
                           if (parallelSeen.load() != 6) orderOk = false;
                       },
                       HookThread::Any, {"par.0", "par.1", "par.2", "par.3", "par.4", "par.5"}}));
    REQUIRE(g.addHook({"next.stage", Stage::Physics, [&](TickContext&) {
                           if (parallelSeen.load() != 6) orderOk = false;
                       },
                       HookThread::Tick, {}}));
    TickContext ctx;
    for (int i = 0; i < 3; ++i) {
        phase = 0;
        parallelSeen = 0;
        REQUIRE(g.run(ctx, &testJobs()));
    }
    CHECK(orderOk.load());
    CHECK(maxConcurrent.load() >= 1);
    // Without a job system, parallel hooks run in order on the calling thread.
    phase = 0;
    parallelSeen = 0;
    REQUIRE(g.run(ctx, nullptr));
    CHECK(orderOk.load());
}

TEST_CASE("server.tickgraph: invalid graphs are rejected at build") {
    auto noop = [](TickContext&) {};
    {
        TickGraph g;
        REQUIRE(g.addHook({"a", Stage::Input, noop, HookThread::Tick, {"b"}}));
        REQUIRE(g.addHook({"b", Stage::Input, noop, HookThread::Tick, {"a"}}));
        CHECK_FALSE(g.build());
    }
    {
        TickGraph g;
        REQUIRE(g.addHook({"a", Stage::Input, noop, HookThread::Tick, {"missing"}}));
        CHECK(g.build().errorCode() == ErrorCode::NotFound);
    }
    {
        TickGraph g;
        REQUIRE(g.addHook({"a", Stage::Input, noop, HookThread::Tick, {}}));
        REQUIRE(g.addHook({"b", Stage::Send, noop, HookThread::Tick, {"a"}})); // cross-stage
        CHECK_FALSE(g.build());
    }
    {
        TickGraph g;
        REQUIRE(g.addHook({"par", Stage::Input, noop, HookThread::Any, {}}));
        REQUIRE(g.addHook({"tick", Stage::Input, noop, HookThread::Tick, {"par"}})); // tick waits on parallel
        CHECK_FALSE(g.build());
    }
    CHECK_FALSE(TickGraph().addHook({"", Stage::Input, noop, HookThread::Tick, {}}));
}

TEST_CASE("server.tickgraph: budgets follow 04 §3.3 and overruns are counted") {
    const StageBudgets b20 = StageBudgets::forTickHz(20);
    CHECK(b20.totalNs() == 35 * kMs);
    CHECK(b20.ns[static_cast<usize>(Stage::Physics)] == 10 * kMs);
    const StageBudgets b60 = StageBudgets::forTickHz(60);
    CHECK(b60.totalNs() == doctest::Approx(11.67 * kMs).epsilon(0.01));
    TickGraph g;
    StageBudgets tiny{};
    tiny.ns.fill(1); // 1 ns: everything overruns
    g.setBudgets(tiny);
    REQUIRE(g.addHook({"slow", Stage::PostPhysics, [](TickContext&) { sleepMillis(1); }, HookThread::Tick, {}}));
    TickContext ctx;
    REQUIRE(g.run(ctx, nullptr));
    CHECK(g.stageStats(Stage::PostPhysics).overruns == 1);
    CHECK(g.stageStats(Stage::PostPhysics).lastNs >= kMs);
    CHECK(g.lastTickNs() >= kMs);
    const auto hooks = g.hookStats();
    REQUIRE(hooks.size() == 1);
    CHECK(hooks[0].runs == 1);
}

TEST_CASE("server.zone: sessions attach, echo, receive tick state and detach") {
    auto zone = ZoneInstance::create(zoneDesc(1002, "tallis"), &testJobs()).value();
    zone->clock().start(0);
    CHECK(zone->graph().executionOrder(Stage::Input) == std::vector<std::string>{"zone.input", "ecs.Input"});
    zone->post(attach(7, 1));
    zone->post(message(7, proto::encodeEchoRequest(1, std::vector<u8>{'h', 'i'})));
    i64 now = runTicks(*zone, 0, 1);
    auto out = zone->takeOutbox();
    // Attach ack, then the echo (same tick), then the tick state.
    REQUIRE(out.size() == 3);
    CHECK(out[0].kind == ZoneOutMessage::Kind::AttachAck);
    auto ack = proto::decodeAttachAck(proto::parseTrunk(out[0].payload)->body);
    REQUIRE(ack);
    CHECK(ack->zoneId == 1002);
    CHECK(ack->tick == 1);
    CHECK(ack->tickHz == 20);
    CHECK(out[1].kind == ZoneOutMessage::Kind::Deliver);
    CHECK(out[1].channel == net::Channel::EventReliable);
    auto echo = proto::decodeEchoReply(out[1].payload);
    REQUIRE(echo);
    CHECK(echo->seq == 1);
    CHECK(echo->tick == 1);
    CHECK(out[2].channel == net::Channel::State);
    CHECK(proto::decodeTickState(out[2].payload)->tick == 1);
    CHECK(zone->sessions().size() == 1);

    // Unknown payloads are counted, messages from another route are fenced.
    zone->post(message(7, std::vector<u8>{0x42}));
    zone->post(message(7, proto::encodeEchoRequest(2, {}), /*route=*/99));
    now = runTicks(*zone, now, 1);
    CHECK(zone->stats().ignoredMessages == 1);
    CHECK(zone->stats().staleMessages == 1);
    CHECK(zone->stats().echoes == 1);
    (void)zone->takeOutbox();

    ZoneInboxItem d;
    d.kind = ZoneInboxItem::Kind::Detach;
    d.sessionId = 7;
    d.sessionEpoch = 1;
    d.route = 1;
    zone->post(d);
    runTicks(*zone, now, 1);
    CHECK(zone->sessions().empty());
    CHECK(zone->stats().detaches == 1);
    CHECK(zone->takeOutbox().empty());
}

TEST_CASE("server.zone: epochs decide rebinds; stale and full attaches are refused") {
    ZoneDesc desc = zoneDesc(5, "small");
    desc.maxSessions = 1;
    auto zone = ZoneInstance::create(desc, &testJobs()).value();
    zone->clock().start(0);
    zone->post(attach(1, 3, /*route=*/10));
    i64 now = runTicks(*zone, 0, 1);
    (void)zone->takeOutbox();
    // A reconnect through another gateway at a higher epoch rebinds the session.
    zone->post(attach(1, 4, /*route=*/20));
    now = runTicks(*zone, now, 1);
    CHECK(zone->sessions().at(1).route == 20);
    CHECK(zone->stats().rebinds == 1);
    // The old gateway's late detach does not drop the new binding; neither does a stale attach.
    ZoneInboxItem late;
    late.kind = ZoneInboxItem::Kind::Detach;
    late.sessionId = 1;
    late.sessionEpoch = 3;
    late.route = 10;
    zone->post(late);
    zone->post(attach(1, 2, /*route=*/30));
    zone->post(attach(2, 1, /*route=*/10)); // the zone is full
    (void)zone->takeOutbox();
    runTicks(*zone, now, 1);
    CHECK(zone->sessions().at(1).route == 20);
    auto out = zone->takeOutbox();
    std::vector<proto::NackReason> nacks;
    for (auto& m : out)
        if (m.kind == ZoneOutMessage::Kind::AttachNack) nacks.push_back(proto::decodeAttachNack(proto::parseTrunk(m.payload)->body)->reason);
    CHECK(nacks == std::vector<proto::NackReason>{proto::NackReason::StaleEpoch, proto::NackReason::Full});
}

TEST_CASE("server.zone: the inbox drains count-capped per tick") {
    ZoneDesc desc = zoneDesc(6, "capped");
    desc.maxInboxPerTick = 4;
    desc.sendTickState = false;
    auto zone = ZoneInstance::create(desc, &testJobs()).value();
    zone->clock().start(0);
    zone->post(attach(1, 1));
    for (u32 i = 0; i < 10; ++i) zone->post(message(1, proto::encodeEchoRequest(i, {})));
    i64 now = runTicks(*zone, 0, 1);
    CHECK(zone->inboxSize() == 7);
    CHECK(zone->stats().inboxDeferred == 1);
    now = runTicks(*zone, now, 2);
    CHECK(zone->inboxSize() == 0);
    CHECK(zone->stats().echoes == 10);
}

TEST_CASE("server.zone: an overloaded zone dilates and tells its sessions ahead of time") {
    ZoneDesc desc = zoneDesc(8, "hot");
    desc.sendTickState = false;
    auto zone = ZoneInstance::create(desc, &testJobs()).value();
    REQUIRE(zone->graph().addHook({"burn", Stage::PostPhysics, [](TickContext&) { sleepMillis(80); }, HookThread::Tick, {}}));
    zone->clock().start(0);
    zone->post(attach(1, 1));
    const i64 now = runTicks(*zone, 0, 1); // 80 ms of work in a 50 ms tick
    CHECK(zone->clock().demandPpm() < authority::kDilationOne);
    auto out = zone->takeOutbox();
    std::vector<proto::ClientTimeDilation> notices;
    for (auto& m : out)
        if (m.kind == ZoneOutMessage::Kind::Deliver && m.channel == net::Channel::Control)
            notices.push_back(*proto::decodeClientTimeDilation(m.payload));
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].effectiveTick == 3);
    CHECK(notices[0].dilationPpm < authority::kDilationOne);
    CHECK(notices[0].dilationPpm >= 100'000);
    (void)now;
}

TEST_CASE("server.zonehost: NS-0.6 an empty zone ticks in < 0.5 ms") {
    ZoneHost host(&testJobs());
    ZoneInstance* zone = host.addZone(zoneDesc(1002, "tallis"), 0).value();
    std::vector<i64> ticks;
    i64 now = 0;
    for (int i = 0; i < 400; ++i) {
        now = zone->clock().nextTickWallNs();
        REQUIRE(host.runDue(now).value() == 1);
        ticks.push_back(zone->stats().lastTickNs);
    }
    std::sort(ticks.begin(), ticks.end());
    const i64 median = ticks[ticks.size() / 2];
    const i64 p90 = ticks[ticks.size() * 9 / 10];
    MESSAGE("empty zone tick: median " << median / 1000 << " us, p90 " << p90 / 1000 << " us, max " << ticks.back() / 1000 << " us");
    CHECK(median < 500'000);
    CHECK(p90 < 500'000);
    CHECK(zone->graph().stageStats(Stage::Input).overruns == 0);
}

TEST_CASE("server.zonehost: several zones at their own rates, earliest deadline first") {
    ZoneHost host(&testJobs());
    REQUIRE(host.addZone(zoneDesc(1, "twenty", 20), 0));
    REQUIRE(host.addZone(zoneDesc(2, "sixty", 60), 0));
    REQUIRE(host.addZone(zoneDesc(3, "one", 1), 0));
    REQUIRE(host.addZone(zoneDesc(4, "ten", 10), 0));
    CHECK_FALSE(host.addZone(zoneDesc(4, "dup", 10), 0));
    CHECK(host.findByName("sixty")->id() == 2);
    // One simulated second, polled every millisecond.
    for (i64 t = 0; t <= 1000 * kMs; t += kMs) REQUIRE(host.runDue(t));
    CHECK(host.find(1)->clock().tick() == 20);
    CHECK(host.find(2)->clock().tick() == 60);
    CHECK(host.find(3)->clock().tick() == 1);
    CHECK(host.find(4)->clock().tick() == 10);
    CHECK(host.nextDeadlineNs() > 1000 * kMs);

    // After a 200 ms stall every zone has a backlog: they run interleaved by deadline, and each
    // zone's ticks stay in order.
    std::vector<ZoneId> order;
    REQUIRE(host.runDue(1200 * kMs, &order));
    std::vector<u64> perZone(5, 0);
    for (ZoneId z : order) ++perZone[z];
    CHECK(perZone[1] == 4);  // 20 Hz: 4 ticks owed
    CHECK(perZone[2] == 4);  // 60 Hz: 12 owed, catch-up cap 4
    CHECK(perZone[4] == 2);  // 10 Hz
    CHECK(std::find(order.begin(), order.end(), ZoneId{3}) == order.end());
    // Oldest pending deadline first: the 20 Hz zone's (1050 ms). The 60 Hz zone owed 12 ticks,
    // but its backlog was capped at 4, so its oldest *kept* tick is due at 1150 ms.
    CHECK(order[0] == 1);
    CHECK(host.removeZone(2));
    CHECK_FALSE(host.removeZone(2));
    CHECK(host.zoneCount() == 3);
}

TEST_CASE("server.zonehost: TiDi is per zone: one hot zone dilates, its neighbour does not") {
    ZoneHost host(&testJobs());
    ZoneInstance* hot = host.addZone(zoneDesc(1, "hot", 20), 0).value();
    ZoneInstance* calm = host.addZone(zoneDesc(2, "calm", 20), 0).value();
    REQUIRE(hot->graph().addHook({"burn", Stage::PostPhysics, [](TickContext&) { sleepMillis(70); }, HookThread::Tick, {}}));
    i64 now = 0;
    for (int i = 0; i < 8; ++i) {
        now = std::min(hot->clock().nextTickWallNs(), calm->clock().nextTickWallNs());
        REQUIRE(host.runDue(now));
    }
    CHECK(hot->clock().dilationPpm() < authority::kDilationOne);
    CHECK(calm->clock().dilationPpm() == authority::kDilationOne);
}

TEST_CASE("server.zone: client messages beyond maxInboxQueued are dropped; attach and detach never are") {
    ZoneDesc desc = zoneDesc(9, "bounded");
    desc.maxInboxPerTick = 4;
    desc.maxInboxQueued = 10;
    desc.sendTickState = false;
    auto zone = ZoneInstance::create(desc, &testJobs()).value();
    zone->clock().start(0);
    CHECK(zone->post(attach(1, 1)));
    u32 accepted = 0;
    for (u32 i = 0; i < 25; ++i) accepted += zone->post(message(1, proto::encodeEchoRequest(i, {}))) ? 1u : 0u;
    CHECK(accepted == 10);
    CHECK(zone->inboxSize() == 11);
    // Control items still get in while the message quota is used up.
    ZoneInboxItem d;
    d.kind = ZoneInboxItem::Kind::Detach;
    d.sessionId = 2;
    d.route = 1;
    CHECK(zone->post(d));
    i64 now = runTicks(*zone, 0, 1); // drains attach + 3 messages
    CHECK(zone->stats().inboxDropped == 15);
    // Drained messages free their quota.
    CHECK(zone->post(message(1, proto::encodeEchoRequest(99, {}))));
    now = runTicks(*zone, now, 4);
    CHECK(zone->inboxSize() == 0);
    CHECK(zone->stats().echoes == 11);
}

TEST_CASE("server.zone: recent tick durations feed the heartbeat's tick p99") {
    ZoneDesc desc = zoneDesc(10, "p99");
    desc.sendTickState = false;
    auto zone = ZoneInstance::create(desc, &testJobs()).value();
    CHECK(zone->recentTickNs().empty());
    zone->clock().start(0);
    i64 now = runTicks(*zone, 0, 3);
    auto recent = zone->recentTickNs();
    REQUIRE(recent.size() == 3);
    CHECK(recent.back() == zone->stats().lastTickNs);
    now = runTicks(*zone, now, static_cast<u32>(ZoneInstance::kRecentTicks));
    recent = zone->recentTickNs();
    CHECK(recent.size() == ZoneInstance::kRecentTicks); // a bounded window
    CHECK(recent.back() == zone->stats().lastTickNs);
    (void)now;
}
