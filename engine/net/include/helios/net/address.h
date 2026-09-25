#pragma once
// Network addresses: IPv4 / IPv6 host plus UDP port, as a small value type.
//
//   auto a = Address::parse("127.0.0.1:40000");        // std::optional<Address>
//   auto b = Address::parse("[::1]:40001");
//   Address c = Address::ipv4(10, 0, 0, 5, 7777);
//   std::string s = c.toString();                       // "10.0.0.5:7777"
//
// The parser is self-contained (no inet_pton), strict (rejects leading garbage, overlong groups,
// octets > 255, ports > 65535, zone ids) and identical on every platform, so it is also used by
// the fuzzers. IPv4-mapped IPv6 addresses (::ffff:a.b.c.d) are kept as IPv6 by parse(); sockets
// normalise received ones to IPv4 with unmapped() so they compare equal to the IPv4 addresses in
// connect tokens.
//
// Threading: plain value type.

#include <array>
#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "helios/core/types.h"

namespace helios::net {

/// Address family. The numeric values match netcode's NETCODE_ADDRESS_* constants.
enum class AddressFamily : u8 { None = 0, IPv4 = 1, IPv6 = 2 };

class Address {
public:
    constexpr Address() noexcept = default;

    static Address ipv4(u8 a, u8 b, u8 c, u8 d, u16 port) noexcept;
    static Address ipv4(const std::array<u8, 4>& octets, u16 port) noexcept;
    /// IPv6 from eight host-order 16-bit groups (group 0 is the most significant).
    static Address ipv6(const std::array<u16, 8>& groups, u16 port) noexcept;
    /// IPv6 from 16 bytes in network order.
    static Address ipv6Bytes(const std::array<u8, 16>& bytes, u16 port) noexcept;
    static Address loopbackV4(u16 port = 0) noexcept { return ipv4(127, 0, 0, 1, port); }
    static Address loopbackV6(u16 port = 0) noexcept;
    static Address anyV4(u16 port = 0) noexcept { return ipv4(0, 0, 0, 0, port); }
    static Address anyV6(u16 port = 0) noexcept;

    /// Parses "a.b.c.d", "a.b.c.d:port", "[v6]:port", "[v6]" or a bare IPv6 literal ("::1").
    /// Missing ports are 0. Returns nullopt for anything else.
    static std::optional<Address> parse(std::string_view text) noexcept;

    constexpr AddressFamily family() const noexcept { return m_family; }
    constexpr bool isValid() const noexcept { return m_family != AddressFamily::None; }
    constexpr bool isIpv4() const noexcept { return m_family == AddressFamily::IPv4; }
    constexpr bool isIpv6() const noexcept { return m_family == AddressFamily::IPv6; }
    constexpr u16 port() const noexcept { return m_port; }
    constexpr void setPort(u16 port) noexcept { m_port = port; }
    Address withPort(u16 port) const noexcept {
        Address copy = *this;
        copy.m_port = port;
        return copy;
    }

    /// Host bytes in network order: 4 for IPv4, 16 for IPv6, empty for None.
    std::span<const u8> bytes() const noexcept;
    std::array<u8, 4> ipv4Octets() const noexcept;
    /// Host-order 16-bit groups of an IPv6 address (netcode's in-memory representation).
    std::array<u16, 8> ipv6Groups() const noexcept;

    bool isLoopback() const noexcept;
    /// 0.0.0.0 or ::.
    bool isUnspecified() const noexcept;
    /// ::ffff:a.b.c.d
    bool isV4Mapped() const noexcept;
    /// IPv4-mapped IPv6 → IPv4 (other addresses are returned unchanged).
    Address unmapped() const noexcept;
    /// IPv4 → ::ffff:a.b.c.d (other addresses are returned unchanged).
    Address toV4Mapped() const noexcept;
    /// Same family and host bytes (ports may differ).
    bool sameHost(const Address& other) const noexcept;

    /// "a.b.c.d:port" or "[v6]:port" (RFC 5952 compressed); the ":port" part is omitted for port 0,
    /// "NONE" for an invalid address.
    std::string toString() const;
    /// Hash of family, host and port (stable within a process).
    u64 hash() const noexcept;
    /// Hash of family and host only (per-IP tables).
    u64 hostHash() const noexcept;

    friend bool operator==(const Address&, const Address&) noexcept = default;

private:
    std::array<u8, 16> m_bytes{}; // unused tail bytes are always zero, so == can compare them
    u16 m_port = 0;
    AddressFamily m_family = AddressFamily::None;
};

} // namespace helios::net

template <>
struct std::hash<helios::net::Address> {
    std::size_t operator()(const helios::net::Address& a) const noexcept { return static_cast<std::size_t>(a.hash()); }
};

template <>
struct std::formatter<helios::net::Address> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(const helios::net::Address& a, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(a.toString(), ctx);
    }
};
