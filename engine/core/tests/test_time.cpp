#include <doctest/doctest.h>

#include <limits>

#include "helios/core/time.h"

using namespace helios;

TEST_CASE("time: monotonic clock and stopwatch") {
    const u64 a = monotonicNanos();
    const u64 b = monotonicNanos();
    CHECK(b >= a);
    CHECK(monotonicFrequency() > 0);
    CHECK(monotonicTicks() > 0);
    CHECK(unixTimeNanos() > 1'600'000'000ll * 1'000'000'000ll); // after 2020

    Stopwatch sw;
    sleepMillis(10);
    const f64 ms = sw.elapsedMillis();
    CHECK(ms >= 9.0);
    CHECK(ms < 1000.0);
    const u64 lap = sw.lap();
    CHECK(lap >= 9'000'000ull);
    CHECK(sw.elapsedNanos() < lap);
}

TEST_CASE("time: precise sleep lands close to the target") {
    Stopwatch sw;
    sleepPrecise(3'000'000); // 3 ms
    const u64 elapsed = sw.elapsedNanos();
    CHECK(elapsed >= 3'000'000ull);
    CHECK(elapsed < 50'000'000ull); // generous: CI machines are noisy
    sleepNanos(0);                  // yields
}

TEST_CASE("dilatable clock: undilated fixed steps") {
    DilatableClock::Config cfg;
    cfg.stepNanos = 10'000'000; // 100 Hz
    cfg.maxPendingSteps = 1000;
    DilatableClock clock(cfg);
    CHECK(clock.scale() == 1.0);
    clock.advance(1'000'000'000);
    CHECK(clock.pendingSteps() == 100);
    u32 steps = 0;
    while (clock.consumeStep()) ++steps;
    CHECK(steps == 100);
    CHECK(clock.stepCount() == 100);
    CHECK(clock.gameTimeNanos() == 1'000'000'000ull);
    CHECK(clock.realTimeNanos() == 1'000'000'000ull);
    clock.advance(5'000'000);
    CHECK(clock.interpolationAlpha() == doctest::Approx(0.5));
    CHECK(!clock.consumeStep());
}

TEST_CASE("dilatable clock: time dilation slows game time and respects the floor") {
    DilatableClock::Config cfg;
    cfg.stepNanos = 1'000'000'000; // 1 Hz zone tick
    cfg.minScalePpm = 100'000;     // 10 % floor (EVE TiDi)
    cfg.maxPendingSteps = 100;
    DilatableClock clock(cfg);
    clock.setScale(0.5);
    CHECK(clock.scalePpm() == 500'000);
    clock.advance(10'000'000'000ull); // 10 real seconds
    CHECK(clock.pendingSteps() == 5);

    clock.setScale(0.01); // below the floor
    CHECK(clock.scale() == doctest::Approx(0.1));
    clock.reset();
    clock.advance(10'000'000'000ull);
    CHECK(clock.pendingSteps() == 1); // at 10 % a 1 Hz tick happens every 10 s
    clock.setScale(3.0);
    CHECK(clock.scale() == 1.0);
    clock.setMinScale(0.5);
    clock.setScale(0.2);
    CHECK(clock.scale() == doctest::Approx(0.5));
}

TEST_CASE("dilatable clock: integer accumulation is exact and deterministic") {
    DilatableClock::Config cfg;
    cfg.stepNanos = 1000;
    cfg.minScalePpm = 1;
    cfg.maxPendingSteps = 1'000'000;
    DilatableClock clock(cfg);
    clock.setScalePpm(333'333);
    // One million 1 ns advances must sum to exactly floor(1e6 * 0.333333) scaled ns (remainder carry).
    for (int i = 0; i < 1'000'000; ++i) clock.advance(1);
    u32 steps = 0;
    while (clock.consumeStep()) ++steps;
    CHECK(steps == 333);
    CHECK(clock.interpolationAlpha() == doctest::Approx(0.333));

    // A fixed irregular schedule of advances and scale changes always yields the same step count.
    auto run = [] {
        DilatableClock::Config c;
        c.stepNanos = 16'666'667;
        c.maxPendingSteps = 10'000;
        DilatableClock k(c);
        u64 seed = 12345;
        u64 total = 0;
        for (int i = 0; i < 5000; ++i) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            if (i % 97 == 0) k.setScalePpm(static_cast<u32>(100'000 + (seed >> 40) % 900'001));
            k.advance((seed >> 33) % 40'000'000);
            while (k.consumeStep()) ++total;
        }
        return total;
    };
    const u64 first = run();
    CHECK(first == run());
    CHECK(first > 0);
}

TEST_CASE("dilatable clock: backlog is capped and dropped time counted") {
    DilatableClock::Config cfg;
    cfg.stepNanos = 10'000'000;
    cfg.maxPendingSteps = 4;
    DilatableClock clock(cfg);
    clock.advance(1'000'000'000); // a 1 s hitch
    CHECK(clock.pendingSteps() == 4);
    CHECK(clock.droppedNanos() == 960'000'000ull);

    // Whole steps are dropped; the phase inside the current step survives the hitch.
    DilatableClock phase(cfg);
    phase.advance(1'005'000'000);
    CHECK(phase.pendingSteps() == 4);
    CHECK(phase.interpolationAlpha() == doctest::Approx(0.5));
    CHECK(phase.droppedNanos() == 960'000'000ull);
    // A backlog below cap + one step is never trimmed.
    DilatableClock under(cfg);
    under.advance(49'000'000);
    CHECK(under.pendingSteps() == 4);
    CHECK(under.droppedNanos() == 0);
}

TEST_CASE("dilatable clock: NaN scales are ignored instead of converting NaN to an integer") {
    // Regression: std::clamp passes NaN through, and static_cast<u32>(NaN) is undefined behavior.
    DilatableClock clock;
    clock.setScale(0.5);
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    clock.setScale(nan);
    CHECK(clock.scalePpm() == 500'000);
    clock.setMinScale(nan);
    CHECK(clock.minScale() == doctest::Approx(0.1));
    clock.setScale(std::numeric_limits<f64>::infinity());
    CHECK(clock.scale() == 1.0);
    clock.setScale(-std::numeric_limits<f64>::infinity());
    CHECK(clock.scale() == doctest::Approx(0.1)); // clamped to the floor
}
