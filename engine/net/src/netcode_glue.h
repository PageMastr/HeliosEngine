#pragma once
// Address conversion between helios::net::Address and netcode_address_t. Internal.

#include <cstring>

#include <netcode.h>

#include "helios/net/address.h"

namespace helios::net::detail {

inline netcode_address_t toNetcode(const Address& a) noexcept {
    netcode_address_t out;
    std::memset(&out, 0, sizeof(out));
    const Address u = a.unmapped();
    if (u.isIpv4()) {
        out.type = NETCODE_ADDRESS_IPV4;
        const auto o = u.ipv4Octets();
        for (int i = 0; i < 4; ++i) out.data.ipv4[i] = o[static_cast<std::size_t>(i)];
    } else if (u.isIpv6()) {
        out.type = NETCODE_ADDRESS_IPV6;
        const auto g = u.ipv6Groups();
        for (int i = 0; i < 8; ++i) out.data.ipv6[i] = g[static_cast<std::size_t>(i)];
    } else {
        out.type = NETCODE_ADDRESS_NONE;
    }
    out.port = u.port();
    return out;
}

inline Address fromNetcode(const netcode_address_t& a) noexcept {
    if (a.type == NETCODE_ADDRESS_IPV4) {
        return Address::ipv4(a.data.ipv4[0], a.data.ipv4[1], a.data.ipv4[2], a.data.ipv4[3], a.port);
    }
    if (a.type == NETCODE_ADDRESS_IPV6) {
        std::array<u16, 8> g{};
        for (int i = 0; i < 8; ++i) g[static_cast<std::size_t>(i)] = a.data.ipv6[i];
        return Address::ipv6(g, a.port);
    }
    return {};
}

} // namespace helios::net::detail
