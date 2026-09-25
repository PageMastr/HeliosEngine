// ZoneClock and TiDi controller (04 §3.2, §3.5).
#include <doctest/doctest.h>

#include <vector>

#include "helios/authority/zone_clock.h"

using namespace helios;
using namespace helios::authority;

namespace {
constexpr i64 kMs = 1'000'000;

ZoneClock::Config cfg(u32 hz = 20) {
    ZoneClock::Config c;
    c.tickHz = hz;
    return c;
}

/// Runs every due tick at `now`, reporting `cpuNs` for each. Returns ticks run.
u32 runDue(ZoneClock& clock, i64 now, i64 cpuNs) {
    clock.advanceTo(now);
    u32 n = 0;
    while (clock.beginTick()) {
        clock.onTickMeasured(cpuNs);
        ++n;
    }
    return n;
}

/// Drives the clock tick by tick at its own cadence for `ticks` ticks (wall time jumps to each due
/// time), reporting cpuNs(tick) per tick. Returns the final wall time.
template <class F>
i64 drive(ZoneClock& clock, i64 now, u32 ticks, F cpuNs) {
    for (u32 i = 0; i < ticks; ++i) {
        now = std::max(now, clock.nextTickWallNs());
        clock.advanceTo(now);
        auto t = clock.beginTick();
        REQUIRE(t.has_value());
        clock.onTickMeasured(cpuNs(*t));
    }
    return now;
}
} // namespace

TEST_CASE("authority.clock: validate rejects rates outside 1..60 Hz and bad floors") {
    CHECK(ZoneClock::validate(cfg(1)).ok());
    CHECK(ZoneClock::validate(cfg(60)).ok());
    CHECK_FALSE(ZoneClock::validate(cfg(0)).ok());
    CHECK_FALSE(ZoneClock::validate(cfg(61)).ok());
    ZoneClock::Config c = cfg();
    c.tidi.floor = 0.0;
    CHECK_FALSE(ZoneClock::validate(c).ok());
    c.tidi.floor = 1.5;
    CHECK_FALSE(ZoneClock::validate(c).ok());
}

TEST_CASE("authority.clock: fixed-step cadence at 20 Hz") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    CHECK(clock.tick() == 0);
    CHECK(clock.tickDtNs() == 50 * kMs);
    CHECK(clock.nextTickWallNs() == 50 * kMs);
    clock.advanceTo(49 * kMs);
    CHECK_FALSE(clock.isTickDue());
    CHECK_FALSE(clock.beginTick().has_value());
    clock.advanceTo(50 * kMs);
    REQUIRE(clock.isTickDue());
    CHECK(clock.beginTick() == Tick{1});
    CHECK(clock.nextTickWallNs() == 100 * kMs);
    CHECK(clock.gameTimeNs() == 50 * kMs);
    // Earlier wall times are ignored.
    clock.advanceTo(10 * kMs);
    CHECK_FALSE(clock.isTickDue());
}

TEST_CASE("authority.clock: overrun backlog runs back to back without skipping tick numbers") {
    ZoneClock::Config c = cfg(20);
    c.maxCatchUpTicks = 4;
    ZoneClock clock(c);
    clock.start(0);
    // A 1 s stall: 20 ticks are owed, only 4 run back to back; the rest is dropped and counted.
    clock.advanceTo(1000 * kMs);
    CHECK(clock.dueSinceWallNs() < clock.wallNowNs());
    std::vector<Tick> ticks;
    while (auto t = clock.beginTick()) ticks.push_back(*t);
    CHECK(ticks == std::vector<Tick>{1, 2, 3, 4});
    CHECK(clock.droppedNanos() > 0);
    CHECK(clock.nextTickWallNs() > 1000 * kMs);
}

TEST_CASE("authority.clock: dilation stretches wall time, not game time") {
    ZoneClock a(cfg(20));
    ZoneClock b(cfg(20));
    a.start(0);
    b.start(0);
    REQUIRE(b.applySchedule(ZoneSchedule{0, 0, 0, 500'000}).ok()); // d = 0.5 from now on
    CHECK(b.dilation() == doctest::Approx(0.5));
    CHECK(b.wallIntervalNs() == 100 * kMs);
    CHECK(runDue(a, 1000 * kMs, 0) == 4); // backlog cap (4) at 20 Hz
    ZoneClock a2(cfg(20));
    ZoneClock b2(cfg(20));
    a2.start(0);
    b2.start(0);
    REQUIRE(b2.applySchedule(ZoneSchedule{0, 0, 0, 500'000}).ok());
    // Tick by tick over 1 s of wall time: 20 ticks undilated, 10 at d = 0.5; the game step is
    // the same, so each tick represents the same 50 ms of game time.
    u32 na = 0;
    u32 nb = 0;
    for (i64 t = 0; t <= 1000 * kMs; t += kMs) {
        na += runDue(a2, t, 0);
        nb += runDue(b2, t, 0);
    }
    CHECK(na == 20);
    CHECK(nb == 10);
    CHECK(a2.gameTimeNs() == 20 * 50 * kMs);
    CHECK(b2.gameTimeNs() == 10 * 50 * kMs);
}

TEST_CASE("authority.tidi: fast attack aims at 85 % load and is scheduled two ticks ahead") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    i64 now = drive(clock, 0, 1, [](Tick) { return 80 * kMs; }); // L = 80/50 = 1.6
    const u32 expected = 530'000; // 0.85 / 1.6 = 0.53125, quantised down to 1 %
    CHECK(clock.demandPpm() == expected);
    CHECK(clock.tidi().lastLoad() == doctest::Approx(1.6));
    CHECK(clock.dilationPpm() == kDilationOne); // not yet: anchor_tick = now + 2
    auto changes = clock.takeDilationChanges();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0] == DilationChange{3, expected});
    now = drive(clock, now, 1, [](Tick) { return 80 * kMs; }); // tick 2: still undilated
    CHECK(clock.dilationPpm() == kDilationOne);
    CHECK(clock.takeDilationChanges().empty()); // same demand: the pending change stands
    now = drive(clock, now, 1, [](Tick) { return 1 * kMs; }); // tick 3 starts dilated
    CHECK(clock.tick() == 3);
    CHECK(clock.dilationPpm() == expected);
    CHECK(clock.wallIntervalNs() == static_cast<i64>(50 * kMs * 1'000'000ll / expected));
}

TEST_CASE("authority.tidi: the dilation floor is 10 %") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    drive(clock, 0, 6, [](Tick) { return 5000 * kMs; }); // 100x overloaded
    CHECK(clock.dilationPpm() == 100'000);
    CHECK(clock.dilation() == doctest::Approx(0.1));
}

TEST_CASE("authority.tidi: constant overload converges on the sustainable dilation (no ratchet)") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    // 75 ms of work per 50 ms tick: sustainable at d = 0.85 / 1.5 = 0.566.
    drive(clock, 0, 200, [](Tick) { return 75 * kMs; });
    const f64 d = clock.dilation();
    CHECK(d == doctest::Approx(0.56).epsilon(0.02));
    const f64 load = clock.tidi().lastLoad();
    CHECK(load > 0.7);
    CHECK(load <= 0.9);
    // Stable: another 200 ticks change nothing.
    drive(clock, clock.wallNowNs(), 200, [](Tick) { return 75 * kMs; });
    CHECK(clock.dilation() == doctest::Approx(d));
}

TEST_CASE("authority.tidi: slow release after 2 s below 70 % at 0.05 per second") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    // Attack down to 0.5 (L = 1.7 -> 0.85/1.7 = 0.5), let it take effect.
    i64 now = drive(clock, 0, 1, [](Tick) { return 85 * kMs; });
    now = drive(clock, now, 3, [](Tick) { return 40 * kMs; }); // L = 0.4-0.8 while applying
    REQUIRE(clock.dilationPpm() == 500'000);
    // Idle from here: L ~ 0 (1 ms per 100 ms interval).
    const i64 idleStart = now;
    now = drive(clock, now, 18, [](Tick) { return 1 * kMs; }); // 1.8 s of wall time
    CHECK(clock.dilationPpm() == 500'000);                  // still inside the 2 s delay
    while (now - idleStart < 3000 * kMs) now = drive(clock, now, 1, [](Tick) { return 1 * kMs; });
    // ~1 s of release at 0.05/s -> ~0.55 (quantised to 1 %, applied 2 ticks late).
    CHECK(clock.dilation() == doctest::Approx(0.55).epsilon(0.04));
    // Long idle: back to 1.0 and it stays there.
    while (now - idleStart < 20000 * kMs) now = drive(clock, now, 1, [](Tick) { return 1 * kMs; });
    CHECK(clock.dilationPpm() == kDilationOne);
}

TEST_CASE("authority.tidi: loads inside the 70-90 % band hold the dilation") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    i64 now = drive(clock, 0, 4, [](Tick t) { return t == 1 ? 100 * kMs : 1 * kMs; }); // attack to 0.42
    const u32 held = clock.dilationPpm();
    REQUIRE(held < kDilationOne);
    const i64 interval = clock.wallIntervalNs();
    now = drive(clock, now, 200, [interval](Tick) { return interval * 8 / 10; }); // L = 0.8
    CHECK(clock.dilationPpm() == held);
    CHECK(clock.tidi().secondsBelowRelease() == 0.0);
}

TEST_CASE("authority.tidi: attackTicks ignores isolated spikes") {
    ZoneClock::Config c = cfg(20);
    c.tidi.attackTicks = 3;
    ZoneClock clock(c);
    clock.start(0);
    i64 now = drive(clock, 0, 10, [](Tick t) { return t % 2 == 0 ? 60 * kMs : 1 * kMs; });
    CHECK(clock.demandPpm() == kDilationOne);
    CHECK(clock.takeDilationChanges().empty());
    drive(clock, now, 3, [](Tick) { return 60 * kMs; });
    CHECK(clock.demandPpm() < kDilationOne);
}

TEST_CASE("authority.clock: identical inputs give identical tick and dilation sequences") {
    auto run = [] {
        ZoneClock clock(cfg(30));
        clock.start(0);
        std::vector<u64> trace;
        i64 now = 0;
        for (u32 i = 0; i < 300; ++i) {
            now += 7 * kMs + (i % 5) * kMs;
            clock.advanceTo(now);
            while (auto t = clock.beginTick()) {
                clock.onTickMeasured((*t % 17) * 4 * kMs);
                trace.push_back(*t);
                trace.push_back(clock.dilationPpm());
            }
        }
        return trace;
    };
    const auto a = run();
    const auto b = run();
    CHECK(a.size() > 60);
    CHECK(a == b);
}

TEST_CASE("authority.clock: a migration hold freezes time and self-releases at its deadline") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    CHECK(runDue(clock, 50 * kMs, 0) == 1);
    clock.applyHold(MigrationHold{300 * kMs});
    CHECK(clock.isHeld());
    CHECK(runDue(clock, 200 * kMs, 0) == 0);
    CHECK(runDue(clock, 299 * kMs, 0) == 0);
    CHECK(clock.tick() == 1);
    // After the deadline only post-deadline time accrues: the next tick is due 50 ms later.
    CHECK(runDue(clock, 340 * kMs, 0) == 0);
    CHECK_FALSE(clock.isHeld());
    CHECK(runDue(clock, 350 * kMs, 0) == 1);
    CHECK(clock.tick() == 2);
}

TEST_CASE("authority.clock: rejoin resumes at the given tick number") {
    ZoneClock clock(cfg(20));
    clock.start(0);
    CHECK(runDue(clock, 100 * kMs, 0) == 2);
    clock.rejoin(500, 10'000 * kMs);
    REQUIRE(clock.isTickDue());
    CHECK(clock.beginTick() == Tick{500});
    CHECK_FALSE(clock.isTickDue());
    CHECK(clock.nextTickWallNs() == 10'050 * kMs);
}

TEST_CASE("authority.clock: follower zones take the leader's schedule, not their own demand") {
    ZoneClock::Config c = cfg(20);
    c.selfLed = false;
    ZoneClock clock(c);
    clock.start(0);
    drive(clock, 0, 3, [](Tick) { return 100 * kMs; });
    CHECK(clock.demandPpm() < kDilationOne);    // TiDiDemand for the leader
    CHECK(clock.dilationPpm() == kDilationOne); // but nothing applied locally
    CHECK(clock.takeDilationChanges().empty());
    CHECK_FALSE(clock.applySchedule(ZoneSchedule{10, 0, 33'333'333, 500'000}).ok()); // wrong tick_dt
    REQUIRE(clock.applySchedule(ZoneSchedule{5, 0, 50 * kMs, 700'000}).ok());
    auto changes = clock.takeDilationChanges();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0] == DilationChange{5, 700'000});
    drive(clock, clock.wallNowNs(), 1, [](Tick) { return 0; }); // tick 4
    CHECK(clock.dilationPpm() == kDilationOne);
    drive(clock, clock.wallNowNs(), 1, [](Tick) { return 0; }); // tick 5
    CHECK(clock.dilationPpm() == 700'000);
}
