#pragma once
// NetSim (04 §10.1): an in-process network impairment layer at L0, used by unit tests, bots and
// the PIE toolbar (07 §1.6). It wraps any IDatagramTransport and applies, per direction:
//
//   * latency + jitter (uniform ±jitter per datagram, never below zero, so jitter reorders),
//   * Bernoulli loss and Gilbert–Elliott burst loss,
//   * duplication (an extra copy with its own jitter),
//   * explicit reordering (a datagram is held back by reorderDelayMs),
//   * a bandwidth cap: a serialising bottleneck with a tail-drop queue.
//
// All randomness comes from a seeded Xoshiro256** and all timing from the endpoint's clock
// (IDatagramTransport::update), so a NetSim run is bit-for-bit reproducible.
//
// Profiles (--netsim=lan|good|mobile|awful): the numbers in 04 §10.1 are round-trip figures;
// a profile applies half of the latency and jitter to each direction and the stated loss to
// each direction.
//
// Threading: owned by the endpoint's thread, like any transport.

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "helios/core/random.h"
#include "helios/core/types.h"
#include "helios/net/transport.h"

namespace helios::net {

/// Impairments for one direction.
struct NetSimLink {
    f64 latencyMs = 0.0;          ///< One-way base delay.
    f64 jitterMs = 0.0;           ///< Uniform ±jitter per datagram (delay clamped at >= 0).
    f64 lossPercent = 0.0;        ///< Independent (Bernoulli) loss.
    bool burstLoss = false;       ///< Gilbert–Elliott two-state loss on top of lossPercent.
    f64 burstEnterPercent = 0.0;  ///< Per datagram: P(good -> bad).
    f64 burstExitPercent = 25.0;  ///< Per datagram: P(bad -> good).
    f64 burstLossPercent = 100.0; ///< Loss probability while in the bad state.
    f64 duplicatePercent = 0.0;   ///< Probability of delivering one extra copy.
    f64 reorderPercent = 0.0;     ///< Probability of holding a datagram back by reorderDelayMs.
    f64 reorderDelayMs = 10.0;
    u64 bandwidthBitsPerSecond = 0;     ///< Bottleneck rate; 0 = unlimited.
    u32 queueLimitBytes = 256 * 1024;   ///< Bottleneck queue; datagrams beyond it are dropped.

    /// True when any impairment is configured (an inactive link is a zero-cost pass-through).
    bool active() const noexcept;
};

struct NetSimConfig {
    NetSimLink outbound; ///< Datagrams this endpoint sends.
    NetSimLink inbound;  ///< Datagrams this endpoint receives.
    u64 seed = 0x4E657453696D0001ull;
};

enum class NetSimProfile : u8 { Lan, Good, Mobile, Awful };

/// lan: 0 ms / 0 %; good: 40 ms RTT, 0.1 %; mobile: 120 ± 30 ms RTT, 2 %; awful: 250 ms RTT, 5 %.
NetSimConfig makeNetSimProfile(NetSimProfile profile, u64 seed = 0x4E657453696D0001ull);
std::optional<NetSimProfile> parseNetSimProfile(std::string_view name) noexcept;
std::string_view netSimProfileName(NetSimProfile profile) noexcept;

struct NetSimStats {
    u64 submitted = 0;  ///< Datagrams handed to the link.
    u64 delivered = 0;  ///< Datagrams (including duplicates) released to the far side.
    u64 lost = 0;       ///< Dropped by Bernoulli or burst loss.
    u64 burstLost = 0;  ///< Subset of `lost` dropped in the Gilbert–Elliott bad state.
    u64 duplicated = 0;
    u64 reordered = 0;
    u64 queueDrops = 0; ///< Dropped by the bandwidth cap's queue.
};

/// One impaired direction: datagrams go in with push(), come out of pop() once due.
class NetSimPipe {
public:
    explicit NetSimPipe(const NetSimLink& link = {}, u64 seed = 1);

    void setLink(const NetSimLink& link) noexcept { m_link = link; }
    const NetSimLink& link() const noexcept { return m_link; }

    void push(f64 now, const Address& peer, std::span<const u8> data);
    /// Copies the earliest datagram due at `now` into `buffer`; returns its size or 0.
    usize pop(f64 now, Address& peer, std::span<u8> buffer);
    /// Release time of the earliest queued datagram, or a negative value when empty.
    f64 nextReleaseTime() const noexcept;
    usize pending() const noexcept { return m_heap.size(); }
    const NetSimStats& stats() const noexcept { return m_stats; }
    void clear();

private:
    struct Item {
        f64 release = 0.0;
        u64 order = 0;
        Address peer;
        std::vector<u8> data;
    };
    static bool later(const Item& a, const Item& b) noexcept;
    void enqueue(f64 release, const Address& peer, std::span<const u8> data);
    bool chance(f64 percent) noexcept;
    f64 jitterSeconds() noexcept;

    NetSimLink m_link;
    Xoshiro256 m_rng;
    std::vector<Item> m_heap;
    std::vector<std::vector<u8>> m_freeBuffers;
    u64 m_order = 0;
    bool m_burstBad = false;
    f64 m_linkFreeAt = 0.0;
    NetSimStats m_stats;
};

/// IDatagramTransport decorator applying a NetSimConfig to an inner transport. update(now) moves
/// due outbound datagrams to the inner transport and pulls inbound ones through the inbound pipe.
class NetSimTransport final : public IDatagramTransport {
public:
    NetSimTransport(std::unique_ptr<IDatagramTransport> inner, const NetSimConfig& config);
    ~NetSimTransport() override;

    /// Changes impairments at run time (PIE toolbar). Queued datagrams keep their release times.
    void setConfig(const NetSimConfig& config) noexcept;
    const NetSimConfig& config() const noexcept { return m_config; }

    void send(const Address& to, std::span<const u8> data) override;
    usize receive(Address& from, std::span<u8> buffer) override;
    void update(f64 now) override;
    void flush() override;
    Address localAddress() const override { return m_inner->localAddress(); }

    IDatagramTransport& inner() noexcept { return *m_inner; }
    const NetSimStats& outboundStats() const noexcept { return m_out.stats(); }
    const NetSimStats& inboundStats() const noexcept { return m_in.stats(); }

private:
    void releaseOutbound();

    std::unique_ptr<IDatagramTransport> m_inner;
    NetSimConfig m_config;
    NetSimPipe m_out;
    NetSimPipe m_in;
    f64 m_now = 0.0;
    std::vector<u8> m_scratch;
};

} // namespace helios::net
