// Token buckets, RTT estimation, loss windows and the AIMD budget controller (04 §2.5).

#include "helios/net/congestion.h"

#include <algorithm>
#include <cmath>

namespace helios::net {

// ---------------------------------------------------------------------------------------------
// TokenBucket
// ---------------------------------------------------------------------------------------------

TokenBucket::TokenBucket(f64 rate, f64 capacity, f64 now) noexcept
    : m_rate(rate), m_capacity(capacity), m_tokens(capacity), m_last(now) {}

void TokenBucket::refill(f64 now) noexcept {
    if (now > m_last) {
        m_tokens = std::min(m_capacity, m_tokens + (now - m_last) * m_rate);
        m_last = now;
    }
}

bool TokenBucket::tryConsume(f64 amount) noexcept {
    if (m_tokens < amount) return false;
    m_tokens -= amount;
    return true;
}

void TokenBucket::setRate(f64 rate, f64 capacity) noexcept {
    m_rate = rate;
    m_capacity = capacity;
    m_tokens = std::min(m_tokens, capacity);
}

// ---------------------------------------------------------------------------------------------
// RttEstimator
// ---------------------------------------------------------------------------------------------

void RttEstimator::addSample(f64 rtt, f64 now) noexcept {
    if (!(rtt >= 0.0)) return; // also rejects NaN
    if (m_samples == 0) {
        m_srtt = rtt;
        m_rttvar = rtt * 0.5;
        m_minCurrent = rtt;
        m_minPrevious = rtt;
        m_windowStart = now;
    } else {
        // RFC 6298 (alpha 1/8, beta 1/4) and RFC 3550 jitter (1/16 of |delta| between samples).
        m_rttvar = 0.75 * m_rttvar + 0.25 * std::fabs(m_srtt - rtt);
        m_srtt = 0.875 * m_srtt + 0.125 * rtt;
        m_jitter += (std::fabs(rtt - m_latest) - m_jitter) / 16.0;
        if (now - m_windowStart >= m_config.minRttWindow) {
            m_minPrevious = m_minCurrent;
            m_minCurrent = rtt;
            m_windowStart = now;
        } else {
            m_minCurrent = std::min(m_minCurrent, rtt);
        }
    }
    m_latest = rtt;
    ++m_samples;
}

f64 RttEstimator::minRtt() const noexcept { return std::min(m_minCurrent, m_minPrevious); }

f64 RttEstimator::rto() const noexcept {
    if (m_samples == 0) return std::clamp(m_config.initialRto, m_config.minRto, m_config.maxRto);
    return std::clamp(m_srtt + 4.0 * m_rttvar, m_config.minRto, m_config.maxRto);
}

// ---------------------------------------------------------------------------------------------
// LossWindow
// ---------------------------------------------------------------------------------------------

LossWindow::Bucket& LossWindow::bucketFor(f64 now) noexcept {
    const i64 index = static_cast<i64>(std::floor(now / kBucketSeconds));
    Bucket& b = m_buckets[static_cast<usize>(((index % kBuckets) + kBuckets) % kBuckets)];
    if (b.index != index) {
        b.index = index;
        b.sent = 0;
        b.lost = 0;
    }
    return b;
}

void LossWindow::record(bool lost, f64 now) noexcept {
    Bucket& b = bucketFor(now);
    ++b.sent;
    if (lost) ++b.lost;
}

u32 LossWindow::samples(f64 now) const noexcept {
    const i64 current = static_cast<i64>(std::floor(now / kBucketSeconds));
    u32 n = 0;
    for (const Bucket& b : m_buckets)
        if (b.index >= 0 && current - b.index < static_cast<i64>(kBuckets)) n += b.sent;
    return n;
}

f64 LossWindow::lossRate(f64 now) const noexcept {
    const i64 current = static_cast<i64>(std::floor(now / kBucketSeconds));
    u32 sent = 0, lost = 0;
    for (const Bucket& b : m_buckets) {
        if (b.index >= 0 && current - b.index < static_cast<i64>(kBuckets)) {
            sent += b.sent;
            lost += b.lost;
        }
    }
    return sent == 0 ? 0.0 : static_cast<f64>(lost) / static_cast<f64>(sent);
}

// ---------------------------------------------------------------------------------------------
// AimdController
// ---------------------------------------------------------------------------------------------

AimdController::AimdController(const Config& config) noexcept
    : m_config(config),
      m_budget(std::clamp<f64>(config.initialBps, config.minBps, std::max(config.minBps, config.maxBps))) {}

void AimdController::update(f64 now, f64 lossRate, u32 lossSamples, const RttEstimator& rtt) noexcept {
    if (!m_started) {
        m_started = true;
        m_cleanSince = now;
        m_last = now;
    }
    const bool lossy = lossSamples >= m_config.minLossSamples && lossRate > m_config.lossThreshold;
    const bool queued = rtt.hasSample() && rtt.srtt() > rtt.minRtt() + m_config.rttSlack;
    const bool congested = lossy || queued;
    const f64 minBps = static_cast<f64>(m_config.minBps);
    const f64 maxBps = std::max(minBps, static_cast<f64>(m_config.maxBps));
    if (congested) {
        if (!m_congested) m_congestedSince = now;
        if (now - m_congestedSince >= m_config.congestionHold) {
            m_budget = std::max(minBps, m_budget * m_config.decreaseFactor);
            ++m_decreases;
            m_congestedSince = now; // the next decrease needs another full hold
        }
    } else {
        if (m_congested) m_cleanSince = now;
        const f64 increaseFrom = std::max(m_last, m_cleanSince + m_config.cleanHold);
        if (now > increaseFrom) {
            m_budget = std::min(maxBps, m_budget + (now - increaseFrom) * m_config.increaseBpsPerSecond);
        }
    }
    m_congested = congested;
    m_last = now;
}

void AimdController::setBudget(u32 bps, f64 now) noexcept {
    const f64 minBps = static_cast<f64>(m_config.minBps);
    const f64 maxBps = std::max(minBps, static_cast<f64>(m_config.maxBps));
    m_budget = std::clamp(static_cast<f64>(bps), minBps, maxBps);
    m_congestedSince = now;
    m_cleanSince = now;
}

void AimdController::setMaxBps(u32 bps) noexcept {
    m_config.maxBps = std::max(bps, m_config.minBps);
    m_budget = std::min(m_budget, static_cast<f64>(m_config.maxBps));
}

} // namespace helios::net
