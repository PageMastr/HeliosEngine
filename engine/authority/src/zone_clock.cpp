#include "helios/authority/zone_clock.h"

#include <algorithm>
#include <cmath>

#include "helios/core/assert.h"

namespace helios::authority {

// ---------------------------------------------------------------------------------------------
// TiDiController
// ---------------------------------------------------------------------------------------------

TiDiController::TiDiController(const TiDiConfig& config) : m_config(config) {}

void TiDiController::reset() noexcept {
    m_lastLoad = 0.0;
    m_belowSeconds = 0.0;
    m_overloadedTicks = 0;
    m_releaseTarget = 0.0;
}

u32 TiDiController::quantize(f64 d) const noexcept {
    const f64 floorD = std::clamp(m_config.floor, 0.0, 1.0);
    d = std::clamp(d, floorD, 1.0);
    const u32 q = std::max<u32>(1, m_config.quantumPpm);
    // Round down to the quantum (an attack is never undone by rounding) but never below the floor.
    u32 ppm = static_cast<u32>(d * kDilationOne + 1e-6);
    if (ppm >= kDilationOne) return kDilationOne;
    ppm -= ppm % q;
    const u32 floorPpm = static_cast<u32>(std::ceil(floorD * kDilationOne - 1e-6));
    return std::max(ppm, floorPpm);
}

u32 TiDiController::onTick(i64 cpuNs, i64 wallIntervalNs, u32 runningPpm, u32 targetPpm) {
    const f64 d = static_cast<f64>(runningPpm) / kDilationOne;
    const f64 target = static_cast<f64>(targetPpm) / kDilationOne;
    const f64 interval = static_cast<f64>(std::max<i64>(wallIntervalNs, 1));
    const f64 load = static_cast<f64>(std::max<i64>(cpuNs, 0)) / interval;
    const f64 wallSeconds = interval * 1e-9;
    m_lastLoad = load;

    if (load > m_config.attackLoad) {
        m_belowSeconds = 0.0;
        if (++m_overloadedTicks < std::max<u32>(1, m_config.attackTicks)) return targetPpm;
        m_overloadedTicks = 0;
        // Fast attack: aim the next ticks at `attackTarget` utilisation of their wall interval,
        // from the dilation this tick actually ran at.
        const f64 next = std::max(m_config.floor, d * m_config.attackTarget / load);
        m_releaseTarget = next;
        return std::min(targetPpm, quantize(next));
    }
    m_overloadedTicks = 0;
    if (load >= m_config.releaseLoad) {
        m_belowSeconds = 0.0; // inside the band: hold
        return targetPpm;
    }
    const f64 before = m_belowSeconds;
    m_belowSeconds += wallSeconds;
    if (targetPpm >= kDilationOne || m_belowSeconds < m_config.releaseDelaySeconds) return targetPpm;
    // Slow release: only the time past the delay counts.
    const f64 releasing = m_belowSeconds - std::max(before, m_config.releaseDelaySeconds);
    const f64 quantumD = static_cast<f64>(std::max<u32>(1, m_config.quantumPpm)) / kDilationOne;
    if (m_releaseTarget < target || m_releaseTarget > target + quantumD) m_releaseTarget = target; // resync
    m_releaseTarget = std::min(1.0, m_releaseTarget + m_config.releasePerSecond * releasing);
    return std::max(targetPpm, quantize(m_releaseTarget));
}

// ---------------------------------------------------------------------------------------------
// ZoneClock
// ---------------------------------------------------------------------------------------------

Result<void> ZoneClock::validate(const Config& config) {
    if (config.tickHz < 1 || config.tickHz > 60)
        return makeError(ErrorCode::InvalidArgument, "tick rate {} Hz outside 1..60 (ADR-007)", config.tickHz);
    if (!(config.tidi.floor > 0.0 && config.tidi.floor <= 1.0))
        return makeError(ErrorCode::InvalidArgument, "dilation floor {} outside (0, 1]", config.tidi.floor);
    if (config.maxCatchUpTicks < 1) return Error{ErrorCode::InvalidArgument, "maxCatchUpTicks must be >= 1"};
    return {};
}

namespace {
DilatableClock::Config clockConfig(const ZoneClock::Config& c) {
    DilatableClock::Config dc;
    dc.stepNanos = 1'000'000'000ull / std::max<u32>(1, c.tickHz);
    dc.minScalePpm = static_cast<u32>(std::ceil(c.tidi.floor * kDilationOne - 1e-6));
    dc.maxPendingSteps = std::max<u32>(1, c.maxCatchUpTicks);
    return dc;
}
} // namespace

ZoneClock::ZoneClock(const Config& config) : m_config(config), m_clock(clockConfig(config)), m_tidi(config.tidi) {
    HELIOS_ASSERT(validate(config).ok(), "invalid ZoneClock config");
}

void ZoneClock::start(i64 wallNowNs, Tick lastTick) {
    m_clock.reset();
    m_tick = lastTick;
    m_lastWall = wallNowNs;
    m_started = true;
    m_held = false;
    m_rejoinDue = false;
}

void ZoneClock::advanceTo(i64 wallNowNs) {
    if (!m_started) {
        start(wallNowNs);
        return;
    }
    if (wallNowNs <= m_lastWall) return;
    if (m_held) {
        if (wallNowNs < m_holdDeadline) {
            m_lastWall = wallNowNs;
            return;
        }
        // Self-release at the deadline: only the time after it accrues.
        m_held = false;
        m_lastWall = std::max(m_lastWall, m_holdDeadline);
        if (wallNowNs <= m_lastWall) return;
    }
    m_clock.advance(static_cast<u64>(wallNowNs - m_lastWall));
    m_lastWall = wallNowNs;
}

bool ZoneClock::isTickDue() const noexcept {
    return m_started && !m_held && (m_rejoinDue || m_clock.pendingSteps() > 0);
}

std::optional<Tick> ZoneClock::beginTick() {
    if (!isTickDue()) return std::nullopt;
    if (m_rejoinDue) {
        m_rejoinDue = false;
    } else if (!m_clock.consumeStep()) {
        return std::nullopt;
    }
    ++m_tick;
    if (m_pending && m_tick >= m_pending->effectiveTick) {
        m_clock.setScalePpm(m_pending->dilationPpm);
        m_pending.reset();
    }
    return m_tick;
}

void ZoneClock::onTickMeasured(i64 cpuNs) {
    const u32 target = m_pending ? m_pending->dilationPpm : dilationPpm();
    m_demandPpm = m_tidi.onTick(cpuNs, wallIntervalNs(), dilationPpm(), target);
    if (!m_config.selfLed) return;
    if (m_demandPpm != target) schedule(m_tick + m_config.scheduleLeadTicks, m_demandPpm);
}

void ZoneClock::schedule(Tick anchor, u32 ppm) {
    // A change already announced for a future tick keeps its anchor (re-anchoring on every new
    // demand could postpone it forever); only its value is updated.
    if (m_pending && m_pending->effectiveTick > m_tick) {
        m_pending->dilationPpm = ppm;
    } else {
        m_pending = DilationChange{std::max(anchor, m_tick + 1), ppm};
    }
    if (m_pending->dilationPpm == dilationPpm() && m_pending->effectiveTick > m_tick) {
        // The demand returned to the running value before the change took effect.
        m_announce.push_back(*m_pending);
        m_pending.reset();
        return;
    }
    m_announce.push_back(*m_pending);
}

i64 ZoneClock::wallIntervalNs() const noexcept {
    return static_cast<i64>(m_clock.stepNanos() * kDilationOne / std::max<u32>(1, m_clock.scalePpm()));
}

i64 ZoneClock::nextTickWallNs() const noexcept {
    if (m_rejoinDue || m_clock.pendingSteps() > 0) return m_held ? std::max(m_lastWall, m_holdDeadline) : m_lastWall;
    const f64 step = static_cast<f64>(m_clock.stepNanos());
    const f64 remainingScaled = (1.0 - m_clock.interpolationAlpha()) * step;
    const f64 wall = std::ceil(remainingScaled * kDilationOne / std::max<u32>(1, m_clock.scalePpm()));
    const i64 base = m_held ? std::max(m_lastWall, m_holdDeadline) : m_lastWall;
    return base + static_cast<i64>(wall);
}

i64 ZoneClock::dueSinceWallNs() const noexcept {
    const u32 pending = m_clock.pendingSteps();
    if (m_held || pending == 0) return nextTickWallNs();
    const f64 step = static_cast<f64>(m_clock.stepNanos());
    const f64 overdueScaled = (static_cast<f64>(pending - 1) + m_clock.interpolationAlpha()) * step;
    return m_lastWall - static_cast<i64>(overdueScaled * kDilationOne / std::max<u32>(1, m_clock.scalePpm()));
}

Result<void> ZoneClock::applySchedule(const ZoneSchedule& schedule) {
    if (schedule.tickDtNs != 0 && schedule.tickDtNs != m_clock.stepNanos())
        return makeError(ErrorCode::InvalidArgument, "schedule tick_dt {} ns differs from the zone's {} ns",
                         schedule.tickDtNs, m_clock.stepNanos());
    const u32 minPpm = static_cast<u32>(std::llround(m_clock.minScale() * kDilationOne));
    const u32 ppm = std::clamp<u32>(schedule.dilationPpm, minPpm, kDilationOne);
    if (schedule.anchorTick <= m_tick) {
        m_clock.setScalePpm(ppm);
        m_pending.reset();
        m_announce.push_back(DilationChange{m_tick + 1, ppm});
        return {};
    }
    m_pending = DilationChange{schedule.anchorTick, ppm};
    m_announce.push_back(*m_pending);
    return {};
}

void ZoneClock::applyHold(const MigrationHold& hold) {
    if (hold.deadlineWallNs <= m_lastWall) return;
    m_held = true;
    m_holdDeadline = hold.deadlineWallNs;
}

void ZoneClock::releaseHold() {
    m_held = false;
}

void ZoneClock::rejoin(Tick current, i64 wallNowNs) {
    m_clock.reset();
    m_tick = current > 0 ? current - 1 : 0;
    m_lastWall = wallNowNs;
    m_started = true;
    m_held = false;
    m_rejoinDue = true;
    m_pending.reset();
}

std::vector<DilationChange> ZoneClock::takeDilationChanges() {
    std::vector<DilationChange> out;
    out.swap(m_announce);
    return out;
}

} // namespace helios::authority
