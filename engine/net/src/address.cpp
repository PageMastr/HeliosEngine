// Address parsing/formatting. Hand-written (no inet_pton/inet_ntop) so behaviour is identical on
// Windows and Linux and the parser can be fuzzed without a socket library.

#include "helios/net/address.h"

#include <cstring>

#include "helios/core/hash.h"

namespace helios::net {
namespace {

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Decimal port, 1-5 digits, <= 65535.
std::optional<u16> parsePort(std::string_view s) noexcept {
    if (s.empty() || s.size() > 5) return std::nullopt;
    u32 value = 0;
    for (char c : s) {
        if (!isDigit(c)) return std::nullopt;
        value = value * 10 + static_cast<u32>(c - '0');
    }
    if (value > 0xFFFFu) return std::nullopt;
    return static_cast<u16>(value);
}

/// Dotted-quad IPv4, strict: exactly four octets, no leading zeros, each <= 255.
bool parseIpv4(std::string_view s, std::array<u8, 4>& out) noexcept {
    usize pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= s.size()) return false;
        usize start = pos;
        u32 value = 0;
        while (pos < s.size() && isDigit(s[pos])) {
            value = value * 10 + static_cast<u32>(s[pos] - '0');
            if (pos - start >= 3 || value > 255) return false;
            ++pos;
        }
        const usize digits = pos - start;
        if (digits == 0) return false;
        if (digits > 1 && s[start] == '0') return false; // "01" is octal in some parsers: reject
        out[static_cast<usize>(i)] = static_cast<u8>(value);
        if (i < 3) {
            if (pos >= s.size() || s[pos] != '.') return false;
            ++pos;
        }
    }
    return pos == s.size();
}

/// Parses one side of an IPv6 literal (text between the start/end and "::"): colon-separated
/// hex groups, the last of which may be a dotted IPv4 tail when `allowIpv4Tail`.
bool parseIpv6Groups(std::string_view s, bool allowIpv4Tail, u16* groups, int& count) noexcept {
    count = 0;
    if (s.empty()) return true;
    usize pos = 0;
    while (true) {
        usize end = s.find(':', pos);
        const std::string_view token = s.substr(pos, end == std::string_view::npos ? s.size() - pos : end - pos);
        if (token.empty()) return false;
        if (end == std::string_view::npos && allowIpv4Tail && token.find('.') != std::string_view::npos) {
            std::array<u8, 4> v4{};
            if (!parseIpv4(token, v4) || count > 6) return false;
            groups[count++] = static_cast<u16>((v4[0] << 8) | v4[1]);
            groups[count++] = static_cast<u16>((v4[2] << 8) | v4[3]);
            return true;
        }
        if (token.size() > 4 || count >= 8) return false;
        u32 value = 0;
        for (char c : token) {
            const int h = hexValue(c);
            if (h < 0) return false;
            value = (value << 4) | static_cast<u32>(h);
        }
        groups[count++] = static_cast<u16>(value);
        if (end == std::string_view::npos) return true;
        pos = end + 1;
        if (pos >= s.size()) return false; // trailing single ':'
    }
}

bool parseIpv6(std::string_view s, std::array<u16, 8>& out) noexcept {
    if (s.empty() || s.size() > 45) return false;
    out.fill(0);
    const usize dbl = s.find("::");
    if (dbl == std::string_view::npos) {
        int count = 0;
        u16 groups[8] = {};
        if (!parseIpv6Groups(s, true, groups, count) || count != 8) return false;
        for (int i = 0; i < 8; ++i) out[static_cast<usize>(i)] = groups[i];
        return true;
    }
    if (s.find("::", dbl + 1) != std::string_view::npos) return false; // at most one "::"
    const std::string_view left = s.substr(0, dbl);
    const std::string_view right = s.substr(dbl + 2);
    u16 lg[8] = {}, rg[8] = {};
    int lc = 0, rc = 0;
    if (!parseIpv6Groups(left, false, lg, lc)) return false;
    if (!parseIpv6Groups(right, true, rg, rc)) return false;
    if (lc + rc > 7) return false; // "::" must stand for at least one zero group
    for (int i = 0; i < lc; ++i) out[static_cast<usize>(i)] = lg[i];
    for (int i = 0; i < rc; ++i) out[static_cast<usize>(8 - rc + i)] = rg[i];
    return true;
}

void appendHex16(std::string& s, u16 v) {
    constexpr char kDigits[] = "0123456789abcdef";
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const u32 nibble = (static_cast<u32>(v) >> shift) & 0xFu;
        if (nibble != 0 || started || shift == 0) {
            s.push_back(kDigits[nibble]);
            started = true;
        }
    }
}

} // namespace

Address Address::ipv4(u8 a, u8 b, u8 c, u8 d, u16 port) noexcept {
    return ipv4(std::array<u8, 4>{a, b, c, d}, port);
}

Address Address::ipv4(const std::array<u8, 4>& octets, u16 port) noexcept {
    Address addr;
    std::memcpy(addr.m_bytes.data(), octets.data(), 4);
    addr.m_port = port;
    addr.m_family = AddressFamily::IPv4;
    return addr;
}

Address Address::ipv6(const std::array<u16, 8>& groups, u16 port) noexcept {
    Address addr;
    for (usize i = 0; i < 8; ++i) {
        addr.m_bytes[i * 2] = static_cast<u8>(groups[i] >> 8);
        addr.m_bytes[i * 2 + 1] = static_cast<u8>(groups[i] & 0xFF);
    }
    addr.m_port = port;
    addr.m_family = AddressFamily::IPv6;
    return addr;
}

Address Address::ipv6Bytes(const std::array<u8, 16>& bytes, u16 port) noexcept {
    Address addr;
    addr.m_bytes = bytes;
    addr.m_port = port;
    addr.m_family = AddressFamily::IPv6;
    return addr;
}

Address Address::loopbackV6(u16 port) noexcept { return ipv6({0, 0, 0, 0, 0, 0, 0, 1}, port); }
Address Address::anyV6(u16 port) noexcept { return ipv6({0, 0, 0, 0, 0, 0, 0, 0}, port); }

std::optional<Address> Address::parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64) return std::nullopt;
    if (text.front() == '[') {
        const usize close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        std::array<u16, 8> groups{};
        if (!parseIpv6(text.substr(1, close - 1), groups)) return std::nullopt;
        u16 port = 0;
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::nullopt;
            const auto p = parsePort(rest.substr(1));
            if (!p) return std::nullopt;
            port = *p;
        }
        return ipv6(groups, port);
    }
    const usize firstColon = text.find(':');
    if (firstColon != std::string_view::npos && text.find(':', firstColon + 1) != std::string_view::npos) {
        std::array<u16, 8> groups{};
        if (!parseIpv6(text, groups)) return std::nullopt;
        return ipv6(groups, 0);
    }
    std::array<u8, 4> octets{};
    u16 port = 0;
    std::string_view host = text;
    if (firstColon != std::string_view::npos) {
        host = text.substr(0, firstColon);
        const auto p = parsePort(text.substr(firstColon + 1));
        if (!p) return std::nullopt;
        port = *p;
    }
    if (!parseIpv4(host, octets)) return std::nullopt;
    return ipv4(octets, port);
}

std::span<const u8> Address::bytes() const noexcept {
    switch (m_family) {
    case AddressFamily::IPv4: return {m_bytes.data(), 4};
    case AddressFamily::IPv6: return {m_bytes.data(), 16};
    default: return {};
    }
}

std::array<u8, 4> Address::ipv4Octets() const noexcept {
    return {m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3]};
}

std::array<u16, 8> Address::ipv6Groups() const noexcept {
    std::array<u16, 8> g{};
    for (usize i = 0; i < 8; ++i) g[i] = static_cast<u16>((m_bytes[i * 2] << 8) | m_bytes[i * 2 + 1]);
    return g;
}

bool Address::isLoopback() const noexcept {
    if (m_family == AddressFamily::IPv4) return m_bytes[0] == 127;
    if (m_family == AddressFamily::IPv6) {
        if (isV4Mapped()) return m_bytes[12] == 127;
        for (usize i = 0; i < 15; ++i)
            if (m_bytes[i] != 0) return false;
        return m_bytes[15] == 1;
    }
    return false;
}

bool Address::isUnspecified() const noexcept {
    const auto b = bytes();
    if (b.empty()) return false;
    for (u8 v : b)
        if (v != 0) return false;
    return true;
}

bool Address::isV4Mapped() const noexcept {
    if (m_family != AddressFamily::IPv6) return false;
    for (usize i = 0; i < 10; ++i)
        if (m_bytes[i] != 0) return false;
    return m_bytes[10] == 0xFF && m_bytes[11] == 0xFF;
}

Address Address::unmapped() const noexcept {
    if (!isV4Mapped()) return *this;
    return ipv4(m_bytes[12], m_bytes[13], m_bytes[14], m_bytes[15], m_port);
}

Address Address::toV4Mapped() const noexcept {
    if (m_family != AddressFamily::IPv4) return *this;
    std::array<u8, 16> b{};
    b[10] = 0xFF;
    b[11] = 0xFF;
    std::memcpy(b.data() + 12, m_bytes.data(), 4);
    return ipv6Bytes(b, m_port);
}

bool Address::sameHost(const Address& other) const noexcept {
    return m_family == other.m_family && m_bytes == other.m_bytes;
}

std::string Address::toString() const {
    std::string s;
    if (m_family == AddressFamily::IPv4) {
        s = std::format("{}.{}.{}.{}", m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3]);
        if (m_port != 0) s += std::format(":{}", m_port);
        return s;
    }
    if (m_family != AddressFamily::IPv6) return "NONE";
    const auto g = ipv6Groups();
    std::string host;
    if (isV4Mapped()) {
        host = std::format("::ffff:{}.{}.{}.{}", m_bytes[12], m_bytes[13], m_bytes[14], m_bytes[15]);
    } else {
        // RFC 5952: compress the longest run (>= 2) of zero groups, the first one on ties.
        int bestStart = -1, bestLen = 0;
        for (int i = 0; i < 8;) {
            if (g[static_cast<usize>(i)] != 0) {
                ++i;
                continue;
            }
            int j = i;
            while (j < 8 && g[static_cast<usize>(j)] == 0) ++j;
            if (j - i > bestLen && j - i >= 2) {
                bestStart = i;
                bestLen = j - i;
            }
            i = j;
        }
        for (int i = 0; i < 8; ++i) {
            if (i == bestStart) {
                host += "::";
                i += bestLen - 1;
                continue;
            }
            if (!host.empty() && host.back() != ':') host.push_back(':');
            appendHex16(host, g[static_cast<usize>(i)]);
        }
    }
    if (m_port == 0) return host;
    return std::format("[{}]:{}", host, m_port);
}

u64 Address::hash() const noexcept {
    return hashCombine(hostHash(), m_port);
}

u64 Address::hostHash() const noexcept {
    u8 key[17];
    key[0] = static_cast<u8>(m_family);
    std::memcpy(key + 1, m_bytes.data(), 16);
    return hash64(key, sizeof(key));
}

} // namespace helios::net
