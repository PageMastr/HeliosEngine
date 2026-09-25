// Fuzz target: connect token public-part parser and private-part decryption (netcode's AEAD).

#include <cstdlib>

#include "fuzz_common.h"
#include "helios/net/connect_token.h"

using namespace helios;
using namespace helios::net;

namespace {
Key fuzzKey() {
    Key k{};
    for (usize i = 0; i < k.size(); ++i) k[i] = static_cast<u8>(0xA0 + i);
    return k;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::span<const u8> in(data, size);
    (void)parseConnectToken(in); // any size: must reject, never read out of bounds
    ConnectTokenBytes token{};
    std::memcpy(token.data(), data, std::min(size, token.size()));
    if (auto info = parseConnectToken(token)) {
        if (info->serverAddresses.empty() || info->serverAddresses.size() > kMaxServersPerToken) std::abort();
        if (info->createTimestamp > info->expireTimestamp) std::abort();
    }
    if (auto priv = openPrivateConnectToken(token, fuzzKey())) {
        if (priv->serverAddresses.empty() || priv->serverAddresses.size() > kMaxServersPerToken) std::abort();
    }
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    ConnectTokenParams p;
    p.protocolId = 0x48454C494F530001ull;
    p.privateKey = fuzzKey();
    p.expireSeconds = -1;
    for (u64 id = 1; id <= 3; ++id) {
        p.clientId = id;
        p.timeoutSeconds = static_cast<i32>(id * 5);
        p.publicAddresses.push_back(id == 2 ? Address::parse("[2001:db8::7]:7778").value() : Address::ipv4(10, 0, 0, static_cast<u8>(id), 7777));
        const auto t = generateConnectToken(p);
        if (t) out.emplace_back(t->begin(), t->end());
    }
}
