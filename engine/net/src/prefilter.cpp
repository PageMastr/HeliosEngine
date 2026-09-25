// L0 pre-filter (04 §9): shape checks on raw datagrams and per-IP connection-request limits.

#include <algorithm>
#include <cstring>

#include "helios/core/hash.h"
#include "helios/net/endpoint.h"

namespace helios::net {
namespace {

// netcode 1.02 packet geometry (netcode_write_packet).
constexpr usize kRequestBytes = 1 + 13 + 8 + 8 + 24 + 1024; // 1,078
constexpr usize kMacBytes = 16;
constexpr usize kChallengeBody = 8 + 300;
constexpr usize kKeepAliveBody = 8;
constexpr usize kMaxPayload = 1200;

enum : u8 {
    kRequest = 0,
    kDenied = 1,
    kChallenge = 2,
    kResponse = 3,
    kKeepAlive = 4,
    kPayload = 5,
    kDisconnect = 6,
};

} // namespace

PreFilter::PreFilter(Role role, const PreFilterConfig& config) : m_role(role), m_config(config), m_key(config.hashKey) {
    m_buckets.resize(std::max<u32>(1, config.tableSize));
    if (m_key == 0 && role == Role::Server) {
        // Secret per instance (netcode's CSPRNG): bucket collisions cannot be precomputed.
        const Key random = generateKey();
        std::memcpy(&m_key, random.data(), sizeof(m_key));
        m_key |= 1; // never 0
    }
}

usize PreFilter::bucketIndex(const Address& from) const noexcept {
    // IPv4: the whole address. IPv6: the /64 prefix (IPv4-mapped addresses count as IPv4).
    const Address a = from.unmapped();
    u8 key[9] = {};
    key[0] = static_cast<u8>(a.family());
    const std::span<const u8> host = a.bytes();
    std::memcpy(key + 1, host.data(), std::min<usize>(host.size(), 8));
    const usize keyBytes = a.isIpv6() ? 9 : 5;
    return static_cast<usize>(hash64(key, keyBytes, m_key) % m_buckets.size());
}

PreFilter::Verdict PreFilter::check(const Address& from, std::span<const u8> datagram, f64 now) {
    if (!m_config.enabled) {
        ++m_stats.accepted;
        return Verdict::Accept;
    }
    if (datagram.empty() || datagram.size() > kRequestBytes + 256) {
        ++m_stats.badSize;
        return Verdict::BadSize;
    }
    const u8 prefix = datagram[0];
    const u8 type = prefix & 0x0F;
    const usize seqBytes = prefix >> 4;
    const usize size = datagram.size();

    if (type == kRequest) {
        if (m_role != Role::Server || prefix != 0) {
            ++m_stats.badType;
            return Verdict::BadType;
        }
        if (size != kRequestBytes) {
            ++m_stats.badSize;
            return Verdict::BadSize;
        }
        // Requests are the only unauthenticated, state-creating packets: rate-limit per IP.
        Bucket& b = m_buckets[bucketIndex(from)];
        if (b.last < 0.0) {
            b.tokens = m_config.requestBurstPerIp;
        } else if (now > b.last) {
            b.tokens = std::min(m_config.requestBurstPerIp, b.tokens + (now - b.last) * m_config.requestsPerSecondPerIp);
        }
        b.last = std::max(b.last, now);
        if (b.tokens < 1.0) {
            ++m_stats.rateLimited;
            return Verdict::RateLimited;
        }
        b.tokens -= 1.0;
        ++m_stats.accepted;
        return Verdict::Accept;
    }

    if (seqBytes < 1 || seqBytes > 8) {
        ++m_stats.badType;
        return Verdict::BadType;
    }
    const usize header = 1 + seqBytes;
    bool allowed = false;
    usize minSize = 0, maxSize = 0;
    switch (type) {
    case kDenied:
        allowed = m_role == Role::Client;
        minSize = maxSize = header + kMacBytes;
        break;
    case kChallenge:
        allowed = m_role == Role::Client;
        minSize = maxSize = header + kChallengeBody + kMacBytes;
        break;
    case kResponse:
        allowed = m_role == Role::Server;
        minSize = maxSize = header + kChallengeBody + kMacBytes;
        break;
    case kKeepAlive:
        allowed = true;
        minSize = maxSize = header + kKeepAliveBody + kMacBytes;
        break;
    case kPayload:
        allowed = true;
        minSize = header + 1 + kMacBytes;
        maxSize = header + kMaxPayload + kMacBytes;
        break;
    case kDisconnect:
        allowed = true;
        minSize = maxSize = header + kMacBytes;
        break;
    default: allowed = false; break;
    }
    if (!allowed) {
        ++m_stats.badType;
        return Verdict::BadType;
    }
    if (size < minSize || size > maxSize) {
        ++m_stats.badSize;
        return Verdict::BadSize;
    }
    ++m_stats.accepted;
    return Verdict::Accept;
}

} // namespace helios::net
