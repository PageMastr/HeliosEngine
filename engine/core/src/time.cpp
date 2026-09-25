#include "helios/core/time.h"

#include <algorithm>
#include <cmath>

#include "helios/core/thread.h"
#include "platform/os.h"

namespace helios {

u64 monotonicNanos() noexcept { return os::monotonicNanos(); }
u64 monotonicTicks() noexcept { return os::monotonicTicks(); }
u64 monotonicFrequency() noexcept { return os::monotonicFrequency(); }

i64 unixTimeNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void sleepNanos(u64 nanos) noexcept {
    if (nanos == 0) {
        yieldThread();
        return;
    }
    os::sleepNanos(nanos);
}

void sleepPrecise(u64 nanos) noexcept {
    const u64 target = monotonicNanos() + nanos;
    const u64 margin = os::sleepSpinMarginNanos();
    for (;;) {
        const u64 now = monotonicNanos();
        if (now >= target) return;
        const u64 remaining = target - now;
        if (remaining > margin) {
            os::highResolutionSleepNanos(remaining - margin);
        } else if (remaining > 50'000) {
            yieldThread();
        } else {
            cpuPause();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// DilatableClock
// ---------------------------------------------------------------------------------------------

namespace {
constexpr u64 kMaxAdvanceNanos = 3600ull * 1'000'000'000ull; // one hour per call
} // namespace

DilatableClock::DilatableClock(const Config& config) noexcept
    : m_stepNanos(config.stepNanos == 0 ? 1 : config.stepNanos),
      m_minScalePpm(std::clamp<u32>(config.minScalePpm, 1, kScaleOne)),
      m_maxPendingSteps(config.maxPendingSteps == 0 ? 1 : config.maxPendingSteps) {}

void DilatableClock::setScalePpm(u32 ppm) noexcept { m_scalePpm = std::clamp<u32>(ppm, m_minScalePpm, kScaleOne); }

void DilatableClock::setScale(f64 scale) noexcept {
    // NaN (e.g. a load ratio computed as 0/0) passes through std::clamp, and converting it to an
    // integer is undefined behavior: keep the current scale instead.
    if (std::isnan(scale)) return;
    // Round to the nearest ppm so the stored (integer) value is identical on every platform.
    const f64 clamped = std::clamp(scale, 0.0, 1.0);
    setScalePpm(static_cast<u32>(clamped * static_cast<f64>(kScaleOne) + 0.5));
}

void DilatableClock::setMinScale(f64 minScale) noexcept {
    if (std::isnan(minScale)) return;
    const f64 clamped = std::clamp(minScale, 0.000001, 1.0);
    m_minScalePpm = static_cast<u32>(clamped * static_cast<f64>(kScaleOne) + 0.5);
    setScalePpm(m_scalePpm);
}

void DilatableClock::advance(u64 realNanos) noexcept {
    realNanos = std::min(realNanos, kMaxAdvanceNanos);
    m_realTotal += realNanos;
    // scaled = realNanos * scale / 1e6, computed exactly without 128-bit math:
    // realNanos = q * 1e6 + r  ->  scaled = q * scale + (r * scale + carry) / 1e6.
    const u64 q = realNanos / kScaleOne;
    const u64 r = realNanos % kScaleOne;
    const u64 fraction = r * m_scalePpm + m_remainder; // < 1e12 + 1e6
    const u64 scaled = q * m_scalePpm + fraction / kScaleOne;
    m_remainder = fraction % kScaleOne;
    m_accumulator += scaled;

    const u64 cap = static_cast<u64>(m_maxPendingSteps) * m_stepNanos;
    if (m_accumulator >= cap + m_stepNanos) {
        // Drop whole steps of backlog but keep the phase within the step, so interpolationAlpha()
        // stays continuous across a hitch.
        const u64 kept = cap + m_accumulator % m_stepNanos;
        m_dropped += m_accumulator - kept;
        m_accumulator = kept;
    }
}

bool DilatableClock::consumeStep() noexcept {
    if (m_accumulator < m_stepNanos) return false;
    m_accumulator -= m_stepNanos;
    ++m_steps;
    return true;
}

void DilatableClock::reset() noexcept {
    m_accumulator = 0;
    m_remainder = 0;
    m_steps = 0;
    m_realTotal = 0;
    m_dropped = 0;
}

} // namespace helios
