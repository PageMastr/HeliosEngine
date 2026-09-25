#pragma once
// ZoneClock: a zone instance's dilatable tick clock with the TiDi controller (04 §3.2, §3.5).
//
// The game step is fixed (tick_dt = 1 / tick_hz, 1..60 Hz). Time dilation d ∈ [floor, 1]
// (floor 0.1) stretches only wall time: wall_interval = tick_dt / d, so every tick computes the
// same result dilated or not (04 design rule 8: wall time decides *when* a tick runs, never *what*
// it computes). Step accounting is core's DilatableClock (integer, parts-per-million dilation,
// bounded catch-up backlog), so the tick sequence is bit-identical on every platform for the same
// wall-time inputs.
//
// **TiDi controller** (TiDiController). Load L is the tick's CPU time over the wall interval it
// was given, L = tick_cpu / (tick_dt / d); at d = 1 this is 04 §3.2's tick_cpu_ms / (1000 /
// tick_hz). Measuring against the dilated interval makes the fast attack converge on the
// sustainable dilation instead of ratcheting down while already dilated:
//   * fast attack: L > 0.9  ->  d <- max(floor, d * 0.85 / L)   (after `attackTicks` ticks in a row);
//   * slow release: after 2 s (wall) of L < 0.7, d rises by 0.05 per wall second, up to 1.
// Output is quantised to 1 % steps so a release does not announce a new d every tick.
//
// **Schedules.** Dilation changes take effect at a future tick, `anchor_tick = now + 2`, so every
// consumer (clients, and in Phase 3 every cell of the zone) switches at the same tick number. In v0
// the zone is its own leader (Config::selfLed): the controller's demand becomes the schedule. In a
// multi-cell zone the leader collects TiDiDemand{d_wanted} (demandPpm()) and sends a ZoneSchedule
// that applySchedule() installs. Each scheduled change is also queued as a DilationChange that the
// zone announces to its clients on CONTROL (takeDilationChanges()).
//
// **Late ticks.** A tick that overruns makes the next one start late; the clock then runs the
// backlog back to back (at most maxCatchUpTicks), never skipping a tick number. Backlog beyond that
// is dropped and counted (droppedNanos()): the zone falls behind wall time, and TiDi reacts.
// **Holds.** applyHold() freezes time (MigrationHold, bounded; self-releases at its deadline).
// **Rejoin.** rejoin() resumes at a given tick number after crash recovery or a long hold.
//
// Threading: owned by the zone's tick thread (plain value semantics, no locking).

#include <optional>
#include <vector>

#include "helios/authority/types.h"
#include "helios/core/result.h"
#include "helios/core/time.h"

namespace helios::authority {

using Tick = u64;

inline constexpr u32 kDilationOne = DilatableClock::kScaleOne; ///< 1.0 in parts per million.

struct TiDiConfig {
    f64 floor = 0.1;              ///< Lowest dilation (R01-P0-3: EVE's 10 %).
    f64 attackLoad = 0.9;         ///< L above this triggers the fast attack.
    f64 attackTarget = 0.85;      ///< The attack aims for this load.
    u32 attackTicks = 1;          ///< Consecutive overloaded ticks before attacking (1 = spec).
    f64 releaseLoad = 0.7;        ///< L below this counts toward the release.
    f64 releaseDelaySeconds = 2.0;
    f64 releasePerSecond = 0.05;
    u32 quantumPpm = 10'000;      ///< Output granularity (1 %).
};

/// The TiDi control law as a pure function of per-tick measurements (deterministic for the same
/// inputs; the inputs are wall-clock measurements, so d itself is recorded, never recomputed).
class TiDiController {
public:
    explicit TiDiController(const TiDiConfig& config = {});

    /// One measured tick: `cpuNs` spent in it, `wallIntervalNs` it was given (tick_dt / d), the
    /// dilation it ran at (`runningPpm`) and the dilation already demanded for the coming ticks
    /// (`targetPpm`: a scheduled change not yet in effect, else the running value). Returns the
    /// dilation this controller wants from now on; holding returns `targetPpm`, so a scheduled
    /// attack is never undone while it waits for its anchor tick.
    u32 onTick(i64 cpuNs, i64 wallIntervalNs, u32 runningPpm, u32 targetPpm);
    /// The last load L (CPU over the dilated wall interval).
    f64 lastLoad() const noexcept { return m_lastLoad; }
    f64 secondsBelowRelease() const noexcept { return m_belowSeconds; }
    const TiDiConfig& config() const noexcept { return m_config; }
    void reset() noexcept;

private:
    u32 quantize(f64 d) const noexcept;

    TiDiConfig m_config;
    f64 m_lastLoad = 0.0;
    f64 m_belowSeconds = 0.0;
    u32 m_overloadedTicks = 0;
    f64 m_releaseTarget = 0.0; ///< Unquantised dilation while releasing.
};

/// A leader-issued schedule (04 §3.5): tick n starts at anchorWallNs + (n - anchorTick) * tick_dt / d.
struct ZoneSchedule {
    Tick anchorTick = 0;
    i64 anchorWallNs = 0;
    u64 tickDtNs = 0;
    u32 dilationPpm = kDilationOne;
};

/// A bounded freeze during a planned region migration (04 §6.7); self-releases at the deadline.
struct MigrationHold {
    i64 deadlineWallNs = 0;
};

/// A dilation change that takes effect at `effectiveTick` (announced to clients in advance).
struct DilationChange {
    Tick effectiveTick = 0;
    u32 dilationPpm = kDilationOne;
    friend bool operator==(const DilationChange&, const DilationChange&) = default;
};

class ZoneClock {
public:
    struct Config {
        u32 tickHz = 20;            ///< 1..60 (ADR-007).
        TiDiConfig tidi;
        u32 maxCatchUpTicks = 4;    ///< Backlog run back to back after an overrun.
        bool selfLed = true;        ///< v0: the zone is its own leader (04 §3.5).
        u32 scheduleLeadTicks = 2;  ///< Dilation changes take effect this many ticks ahead.
    };

    static Result<void> validate(const Config& config);

    /// `config` must pass validate() (asserted).
    explicit ZoneClock(const Config& config);

    /// Starts the clock at wall time `wallNowNs` with tick() == `lastTick` (the next tick is
    /// lastTick + 1, due one wall interval later).
    void start(i64 wallNowNs, Tick lastTick = 0);
    /// Feeds wall time up to `wallNowNs` (monotonic; earlier values are ignored). No time accrues
    /// while a hold is active.
    void advanceTo(i64 wallNowNs);
    /// A tick is due at the current wall time.
    bool isTickDue() const noexcept;
    /// Consumes a due tick and returns its number (tick() afterwards); nullopt if none is due.
    /// Applies a scheduled dilation whose anchor tick has been reached.
    std::optional<Tick> beginTick();
    /// Feeds the tick's measured CPU time to the TiDi controller (self-led zones schedule the
    /// demanded dilation `scheduleLeadTicks` ahead).
    void onTickMeasured(i64 cpuNs);

    /// Wall time the next tick is due (<= wallNowNs() when one is due already).
    i64 nextTickWallNs() const noexcept;
    /// Earliest-deadline-first key: when the oldest pending tick became due (nextTickWallNs() if
    /// none is pending).
    i64 dueSinceWallNs() const noexcept;

    Tick tick() const noexcept { return m_tick; }
    u32 tickHz() const noexcept { return m_config.tickHz; }
    u64 tickDtNs() const noexcept { return m_clock.stepNanos(); }
    f64 dilation() const noexcept { return m_clock.scale(); }
    u32 dilationPpm() const noexcept { return m_clock.scalePpm(); }
    /// tick_dt / d: the wall interval between tick starts at the current dilation.
    i64 wallIntervalNs() const noexcept;
    /// Last wall time fed to advanceTo() (a WallTime, never converted into simulation time).
    i64 wallNowNs() const noexcept { return m_lastWall; }
    /// Game time of the current tick (tick() * tick_dt).
    u64 gameTimeNs() const noexcept { return m_tick * m_clock.stepNanos(); }
    /// What the controller wants (TiDiDemand for a zone leader).
    u32 demandPpm() const noexcept { return m_demandPpm; }
    const TiDiController& tidi() const noexcept { return m_tidi; }
    /// Scaled time dropped because the backlog exceeded maxCatchUpTicks.
    u64 droppedNanos() const noexcept { return m_clock.droppedNanos(); }
    /// The pending scheduled change, if any.
    std::optional<DilationChange> pendingChange() const noexcept { return m_pending; }

    /// Installs a leader's schedule: dilation from anchorTick on. Fails (InvalidArgument) if its
    /// tick_dt differs from this zone's.
    Result<void> applySchedule(const ZoneSchedule& schedule);
    void applyHold(const MigrationHold& hold);
    void releaseHold();
    bool isHeld() const noexcept { return m_held; }
    /// Resumes at tick `current` (it runs next, due now) after recovery or a late rejoin (04 §3.5);
    /// accumulated time is discarded.
    void rejoin(Tick current, i64 wallNowNs);

    /// Changes scheduled since the last call (to announce to clients).
    std::vector<DilationChange> takeDilationChanges();

private:
    void schedule(Tick anchor, u32 ppm);

    Config m_config;
    DilatableClock m_clock;
    TiDiController m_tidi;
    Tick m_tick = 0;
    i64 m_lastWall = 0;
    bool m_started = false;
    bool m_held = false;
    i64 m_holdDeadline = 0;
    bool m_rejoinDue = false;
    u32 m_demandPpm = kDilationOne;
    std::optional<DilationChange> m_pending;
    std::vector<DilationChange> m_announce;
};

} // namespace helios::authority
