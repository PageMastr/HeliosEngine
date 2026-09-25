#pragma once
// Rate and congestion primitives used by Connection (04 §2.2, §2.5, §9):
//
//   TokenBucket     byte/packet budgets (send budget, 64 kbit/s upstream bucket, 40 kbit/s voice,
//                   120 pps inbound policing)
//   RttEstimator    RFC 6298 srtt/rttvar/RTO (clamped 30 ms .. 1 s), windowed min RTT and
//                   RFC 3550-style jitter
//   LossWindow      packet loss over the last second (4 x 250 ms buckets)
//   AimdController  the 04 §2.5 budget controller: loss > 5 % or srtt > min_rtt + 100 ms for 1 s
//                   -> x0.7 (floor 64 kbit/s); 5 s clean -> +16 kbit/s per second up to the cap
//
// All take the caller's clock (seconds) so behaviour is deterministic under simulated time.
// Threading: plain values owned by one connection.

#include <array>

#include "helios/core/types.h"

namespace helios::net {

class TokenBucket {
public:
    TokenBucket() noexcept = default;
    /// `rate` tokens per second, at most `capacity` tokens banked. Starts full.
    TokenBucket(f64 rate, f64 capacity, f64 now = 0.0) noexcept;

    void refill(f64 now) noexcept;
    /// Consumes `amount` if available.
    bool tryConsume(f64 amount) noexcept;
    /// Consumes unconditionally (the balance may go negative: one oversized packet borrows from
    /// the future and the next sends wait until it is repaid).
    void consume(f64 amount) noexcept { m_tokens -= amount; }
    f64 tokens() const noexcept { return m_tokens; }
    f64 rate() const noexcept { return m_rate; }
    f64 capacity() const noexcept { return m_capacity; }
    void setRate(f64 rate, f64 capacity) noexcept;

private:
    f64 m_rate = 0.0;
    f64 m_capacity = 0.0;
    f64 m_tokens = 0.0;
    f64 m_last = 0.0;
};

class RttEstimator {
public:
    struct Config {
        f64 initialRto = 0.25;   ///< Before the first sample.
        f64 minRto = 0.030;
        f64 maxRto = 1.0;
        f64 minRttWindow = 10.0; ///< min RTT forgets samples older than ~2 windows.
    };

    RttEstimator() noexcept : RttEstimator(Config{}) {}
    explicit RttEstimator(const Config& config) noexcept : m_config(config) {}

    void addSample(f64 rtt, f64 now) noexcept;
    bool hasSample() const noexcept { return m_samples > 0; }
    u64 samples() const noexcept { return m_samples; }
    f64 srtt() const noexcept { return m_srtt; }
    f64 rttvar() const noexcept { return m_rttvar; }
    f64 latest() const noexcept { return m_latest; }
    f64 minRtt() const noexcept;
    f64 jitter() const noexcept { return m_jitter; }
    /// srtt + 4 * rttvar, clamped to [minRto, maxRto].
    f64 rto() const noexcept;

private:
    Config m_config;
    u64 m_samples = 0;
    f64 m_srtt = 0.0;
    f64 m_rttvar = 0.0;
    f64 m_latest = 0.0;
    f64 m_jitter = 0.0;
    f64 m_minCurrent = 0.0;
    f64 m_minPrevious = 0.0;
    f64 m_windowStart = 0.0;
};

class LossWindow {
public:
    static constexpr f64 kBucketSeconds = 0.25;
    static constexpr u32 kBuckets = 4;

    void record(bool lost, f64 now) noexcept;
    /// Fraction of recorded packets lost in the last kBuckets * kBucketSeconds.
    f64 lossRate(f64 now) const noexcept;
    u32 samples(f64 now) const noexcept;

private:
    struct Bucket {
        i64 index = -1;
        u32 sent = 0;
        u32 lost = 0;
    };
    Bucket& bucketFor(f64 now) noexcept;
    std::array<Bucket, kBuckets> m_buckets{};
};

class AimdController {
public:
    struct Config {
        u32 initialBps = 256'000;
        u32 minBps = 64'000;
        u32 maxBps = 256'000;
        f64 lossThreshold = 0.05;     ///< Loss fraction that counts as congestion.
        u32 minLossSamples = 8;       ///< Packets in the window before loss is trusted.
        f64 rttSlack = 0.100;         ///< srtt above min RTT + slack counts as congestion.
        f64 congestionHold = 1.0;     ///< Seconds of congestion before a decrease.
        f64 decreaseFactor = 0.7;
        f64 cleanHold = 5.0;          ///< Seconds clean before increasing.
        f64 increaseBpsPerSecond = 16'000.0;
    };

    AimdController() noexcept : AimdController(Config{}) {}
    explicit AimdController(const Config& config) noexcept;

    /// Feeds the current signals. Call regularly (every update).
    void update(f64 now, f64 lossRate, u32 lossSamples, const RttEstimator& rtt) noexcept;

    u32 budgetBps() const noexcept { return static_cast<u32>(m_budget + 0.5); }
    /// Overrides the budget (clamped to [minBps, maxBps]) and restarts the hold timers.
    void setBudget(u32 bps, f64 now) noexcept;
    /// Raises or lowers the ceiling (e.g. 512 kbit/s in battles, 04 §2.5).
    void setMaxBps(u32 bps) noexcept;
    const Config& config() const noexcept { return m_config; }
    bool congested() const noexcept { return m_congested; }
    u32 decreases() const noexcept { return m_decreases; }

private:
    Config m_config;
    f64 m_budget;
    bool m_started = false;
    bool m_congested = false;
    f64 m_congestedSince = 0.0;
    f64 m_cleanSince = 0.0;
    f64 m_last = 0.0;
    u32 m_decreases = 0;
};

} // namespace helios::net
