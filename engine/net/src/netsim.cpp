// NetSim: deterministic network impairments (04 §10.1).

#include "helios/net/netsim.h"

#include <algorithm>
#include <cstring>

namespace helios::net {

bool NetSimLink::active() const noexcept {
    return latencyMs > 0.0 || jitterMs > 0.0 || lossPercent > 0.0 || burstLoss || duplicatePercent > 0.0 ||
           reorderPercent > 0.0 || bandwidthBitsPerSecond > 0;
}

NetSimConfig makeNetSimProfile(NetSimProfile profile, u64 seed) {
    NetSimConfig c;
    c.seed = seed;
    NetSimLink link;
    switch (profile) {
    case NetSimProfile::Lan: break;
    case NetSimProfile::Good:
        link.latencyMs = 20.0;
        link.lossPercent = 0.1;
        break;
    case NetSimProfile::Mobile:
        link.latencyMs = 60.0;
        link.jitterMs = 15.0;
        link.lossPercent = 2.0;
        break;
    case NetSimProfile::Awful:
        link.latencyMs = 125.0;
        link.jitterMs = 10.0;
        link.lossPercent = 5.0;
        link.duplicatePercent = 1.0;
        link.reorderPercent = 1.0;
        link.reorderDelayMs = 30.0;
        break;
    }
    c.outbound = link;
    c.inbound = link;
    return c;
}

std::optional<NetSimProfile> parseNetSimProfile(std::string_view name) noexcept {
    if (name == "lan" || name == "off") return NetSimProfile::Lan;
    if (name == "good") return NetSimProfile::Good;
    if (name == "mobile") return NetSimProfile::Mobile;
    if (name == "awful") return NetSimProfile::Awful;
    return std::nullopt;
}

std::string_view netSimProfileName(NetSimProfile profile) noexcept {
    switch (profile) {
    case NetSimProfile::Lan: return "lan";
    case NetSimProfile::Good: return "good";
    case NetSimProfile::Mobile: return "mobile";
    case NetSimProfile::Awful: return "awful";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// NetSimPipe
// ---------------------------------------------------------------------------------------------

NetSimPipe::NetSimPipe(const NetSimLink& link, u64 seed) : m_link(link), m_rng(seed) {}

bool NetSimPipe::later(const Item& a, const Item& b) noexcept {
    if (a.release != b.release) return a.release > b.release;
    return a.order > b.order;
}

bool NetSimPipe::chance(f64 percent) noexcept {
    if (percent <= 0.0) return false;
    if (percent >= 100.0) return true;
    return uniformDouble01(m_rng) * 100.0 < percent;
}

f64 NetSimPipe::jitterSeconds() noexcept {
    if (m_link.jitterMs <= 0.0) return 0.0;
    return uniformDouble(m_rng, -m_link.jitterMs, m_link.jitterMs) * 1e-3;
}

void NetSimPipe::enqueue(f64 release, const Address& peer, std::span<const u8> data) {
    Item item;
    item.release = release;
    item.order = m_order++;
    item.peer = peer;
    if (!m_freeBuffers.empty()) {
        item.data = std::move(m_freeBuffers.back());
        m_freeBuffers.pop_back();
    }
    item.data.assign(data.begin(), data.end());
    m_heap.push_back(std::move(item));
    std::push_heap(m_heap.begin(), m_heap.end(), later);
}

void NetSimPipe::push(f64 now, const Address& peer, std::span<const u8> data) {
    ++m_stats.submitted;
    // Loss: Gilbert–Elliott state transition first, then the state's loss plus Bernoulli loss.
    if (m_link.burstLoss) {
        if (m_burstBad) {
            if (chance(m_link.burstExitPercent)) m_burstBad = false;
        } else if (chance(m_link.burstEnterPercent)) {
            m_burstBad = true;
        }
        if (m_burstBad && chance(m_link.burstLossPercent)) {
            ++m_stats.lost;
            ++m_stats.burstLost;
            return;
        }
    }
    if (chance(m_link.lossPercent)) {
        ++m_stats.lost;
        return;
    }
    // Bandwidth cap: a serialising bottleneck with a bounded queue.
    f64 departure = now;
    if (m_link.bandwidthBitsPerSecond > 0) {
        const f64 rate = static_cast<f64>(m_link.bandwidthBitsPerSecond);
        const f64 start = std::max(now, m_linkFreeAt);
        const f64 backlogBytes = (start - now) * rate / 8.0;
        if (backlogBytes + static_cast<f64>(data.size()) > static_cast<f64>(m_link.queueLimitBytes)) {
            ++m_stats.queueDrops;
            return;
        }
        departure = start + static_cast<f64>(data.size()) * 8.0 / rate;
        m_linkFreeAt = departure;
    }
    f64 release = departure + m_link.latencyMs * 1e-3 + jitterSeconds();
    if (chance(m_link.reorderPercent)) {
        release += m_link.reorderDelayMs * 1e-3;
        ++m_stats.reordered;
    }
    enqueue(std::max(release, now), peer, data);
    if (chance(m_link.duplicatePercent)) {
        ++m_stats.duplicated;
        const f64 dupRelease = departure + m_link.latencyMs * 1e-3 + jitterSeconds();
        enqueue(std::max(dupRelease, now), peer, data);
    }
}

usize NetSimPipe::pop(f64 now, Address& peer, std::span<u8> buffer) {
    while (!m_heap.empty() && m_heap.front().release <= now) {
        std::pop_heap(m_heap.begin(), m_heap.end(), later);
        Item item = std::move(m_heap.back());
        m_heap.pop_back();
        const usize size = item.data.size();
        const bool fits = size <= buffer.size();
        if (fits) {
            std::memcpy(buffer.data(), item.data.data(), size);
            peer = item.peer;
        }
        m_freeBuffers.push_back(std::move(item.data));
        if (fits) {
            ++m_stats.delivered;
            return size;
        }
    }
    return 0;
}

f64 NetSimPipe::nextReleaseTime() const noexcept { return m_heap.empty() ? -1.0 : m_heap.front().release; }

void NetSimPipe::clear() {
    for (Item& item : m_heap) m_freeBuffers.push_back(std::move(item.data));
    m_heap.clear();
    m_linkFreeAt = 0.0;
    m_burstBad = false;
}

// ---------------------------------------------------------------------------------------------
// NetSimTransport
// ---------------------------------------------------------------------------------------------

NetSimTransport::NetSimTransport(std::unique_ptr<IDatagramTransport> inner, const NetSimConfig& config)
    : m_inner(std::move(inner)), m_config(config), m_out(config.outbound, config.seed),
      m_in(config.inbound, config.seed ^ 0x9E3779B97F4A7C15ull), m_scratch(kMaxDatagramBytes) {}

NetSimTransport::~NetSimTransport() = default;

void NetSimTransport::setConfig(const NetSimConfig& config) noexcept {
    m_config = config;
    m_out.setLink(config.outbound);
    m_in.setLink(config.inbound);
}

void NetSimTransport::send(const Address& to, std::span<const u8> data) {
    if (!m_config.outbound.active() && m_out.pending() == 0) {
        m_inner->send(to, data);
        return;
    }
    m_out.push(m_now, to, data);
}

void NetSimTransport::releaseOutbound() {
    Address to;
    for (;;) {
        const usize size = m_out.pop(m_now, to, m_scratch);
        if (size == 0) break;
        m_inner->send(to, std::span<const u8>(m_scratch.data(), size));
    }
}

usize NetSimTransport::receive(Address& from, std::span<u8> buffer) {
    if (!m_config.inbound.active() && m_in.pending() == 0) return m_inner->receive(from, buffer);
    return m_in.pop(m_now, from, buffer);
}

void NetSimTransport::update(f64 now) {
    m_now = std::max(m_now, now);
    m_inner->update(now);
    releaseOutbound();
    if (m_config.inbound.active() || m_in.pending() != 0) {
        // Pull everything the inner transport has and run it through the inbound pipe.
        Address from;
        for (;;) {
            const usize size = m_inner->receive(from, m_scratch);
            if (size == 0) break;
            m_in.push(m_now, from, std::span<const u8>(m_scratch.data(), size));
        }
    }
}

void NetSimTransport::flush() {
    releaseOutbound();
    m_inner->flush();
}

} // namespace helios::net
