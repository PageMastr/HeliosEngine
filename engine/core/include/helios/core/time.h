#pragma once
// Time: monotonic high-resolution clock (QueryPerformanceCounter / clock_gettime(CLOCK_MONOTONIC)),
// stopwatch, sleeps, and DilatableClock — the fixed-step game clock whose rate can be scaled down
// under load (EVE-style time dilation, ADR-007).
//
// Threading: free functions are thread-safe. Stopwatch and DilatableClock are plain values owned
// by one thread (the zone/tick thread for DilatableClock).

#include <chrono>

#include "helios/core/types.h"

namespace helios {

/// Monotonic nanoseconds since an unspecified epoch (never goes backwards, unaffected by wall
/// clock changes).
u64 monotonicNanos() noexcept;
inline f64 monotonicSeconds() noexcept { return static_cast<f64>(monotonicNanos()) * 1e-9; }
/// Raw counter ticks and their frequency (ticks per second), for profilers.
u64 monotonicTicks() noexcept;
u64 monotonicFrequency() noexcept;
/// Wall clock, nanoseconds since the Unix epoch (UTC). May jump; never use for intervals.
i64 unixTimeNanos() noexcept;

/// Sleeps at least `nanos` with OS granularity (may overshoot by the scheduler quantum).
void sleepNanos(u64 nanos) noexcept;
inline void sleepMillis(u32 millis) noexcept { sleepNanos(static_cast<u64>(millis) * 1'000'000ull); }
inline void sleepFor(std::chrono::nanoseconds d) noexcept {
    sleepNanos(d.count() > 0 ? static_cast<u64>(d.count()) : 0);
}
/// Sleeps close to exactly `nanos`: OS sleep (high-resolution timer on Windows) for the bulk,
/// then a short spin. Costs some CPU in the final ~0.5-1 ms. For frame pacing / tick loops.
void sleepPrecise(u64 nanos) noexcept;

/// Measures elapsed monotonic time.
class Stopwatch {
public:
    Stopwatch() noexcept : m_start(monotonicNanos()) {}
    void reset() noexcept { m_start = monotonicNanos(); }
    u64 elapsedNanos() const noexcept { return monotonicNanos() - m_start; }
    f64 elapsedSeconds() const noexcept { return static_cast<f64>(elapsedNanos()) * 1e-9; }
    f64 elapsedMillis() const noexcept { return static_cast<f64>(elapsedNanos()) * 1e-6; }
    /// Returns elapsed nanoseconds and restarts.
    u64 lap() noexcept {
        const u64 now = monotonicNanos();
        const u64 elapsed = now - m_start;
        m_start = now;
        return elapsed;
    }

private:
    u64 m_start;
};

/// Fixed-step game clock with time dilation.
///
/// Real time is fed with advance(realNanos); it accumulates *scaled* by the dilation factor
/// scale ∈ [minScale, 1]. The simulation consumes fixed steps (consumeStep()), so each step always
/// represents stepNanos of game time: at scale 0.1 a 1 Hz zone ticks every 10 real seconds, but
/// every tick is identical to an undilated one — the simulation stays deterministic.
///
/// All arithmetic is integer (scale is stored in parts-per-million with a carried remainder), so a
/// given sequence of advance()/setScale() calls produces bit-identical step counts on every
/// platform. Backlog is capped at maxPendingSteps; excess game time is dropped and counted
/// (hitch/"spiral of death" protection).
class DilatableClock {
public:
    static constexpr u32 kScaleOne = 1'000'000; ///< 1.0 in parts per million.

    struct Config {
        u64 stepNanos = 33'333'333; ///< 30 Hz by default.
        u32 minScalePpm = 100'000;  ///< Dilation floor (EVE uses 10%).
        u32 maxPendingSteps = 8;
    };

    DilatableClock() noexcept : DilatableClock(Config{}) {}
    explicit DilatableClock(const Config& config) noexcept;

    /// Sets the dilation factor, clamped to [minScale, 1]. Takes effect for subsequent advance().
    /// NaN is ignored (the scale is left unchanged).
    void setScale(f64 scale) noexcept;
    void setScalePpm(u32 ppm) noexcept;
    f64 scale() const noexcept { return static_cast<f64>(m_scalePpm) / kScaleOne; }
    u32 scalePpm() const noexcept { return m_scalePpm; }
    void setMinScale(f64 minScale) noexcept;
    f64 minScale() const noexcept { return static_cast<f64>(m_minScalePpm) / kScaleOne; }

    /// Feeds elapsed real time (a single call is clamped to one hour).
    void advance(u64 realNanos) noexcept;
    /// Takes one fixed step if enough scaled time has accumulated.
    bool consumeStep() noexcept;
    u32 pendingSteps() const noexcept { return static_cast<u32>(m_accumulator / m_stepNanos); }
    /// Fraction of the next step already accumulated, in [0, 1): for render interpolation.
    f64 interpolationAlpha() const noexcept {
        return static_cast<f64>(m_accumulator % m_stepNanos) / static_cast<f64>(m_stepNanos);
    }

    u64 stepCount() const noexcept { return m_steps; }
    u64 stepNanos() const noexcept { return m_stepNanos; }
    f64 stepSeconds() const noexcept { return static_cast<f64>(m_stepNanos) * 1e-9; }
    /// Game time simulated so far (stepCount * stepNanos).
    u64 gameTimeNanos() const noexcept { return m_steps * m_stepNanos; }
    f64 gameTimeSeconds() const noexcept { return static_cast<f64>(gameTimeNanos()) * 1e-9; }
    /// Total real time fed to advance().
    u64 realTimeNanos() const noexcept { return m_realTotal; }
    /// Scaled time discarded by the backlog cap (whole steps; the phase within a step is kept).
    u64 droppedNanos() const noexcept { return m_dropped; }

    /// Clears accumulated time and counters (keeps configuration and scale).
    void reset() noexcept;

private:
    u64 m_stepNanos;
    u32 m_scalePpm = kScaleOne;
    u32 m_minScalePpm;
    u32 m_maxPendingSteps;
    u64 m_accumulator = 0;
    u64 m_remainder = 0; // fractional scaled nanoseconds, in 1e-6 ns units
    u64 m_steps = 0;
    u64 m_realTotal = 0;
    u64 m_dropped = 0;
};

} // namespace helios
