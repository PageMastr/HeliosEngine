// Fuzz target: raw datagrams arriving at a server — the L0 pre-filter and netcode's packet read
// path (prefix/sequence decoding, connection-request token decryption, AEAD rejection, replay
// window). Input: [u16 length][datagram] records; the first byte of each record picks one of four
// source addresses. The server persists across inputs (as a real one does).

#include <cstdlib>
#include <memory>

#include "fuzz_common.h"
#include "helios/net/endpoint.h"

using namespace helios;
using namespace helios::net;

namespace {

constexpr u64 kProtocol = 0x48454C494F530001ull;

Key fuzzKey() {
    Key k{};
    for (usize i = 0; i < k.size(); ++i) k[i] = static_cast<u8>(0x31 * (i + 1));
    return k;
}

const Address kServer = Address::ipv4(10, 0, 0, 1, 7777);

struct NullHandler final : IEndpointHandler {
    void onMessage(SessionHandle, Channel, std::span<const u8>) override {}
};

struct World {
    VirtualNetwork net;
    std::unique_ptr<Server> server;
    NullHandler handler;
    f64 now = 0.0;
    World() {
        ServerConfig sc;
        sc.protocolId = kProtocol;
        sc.privateKey = fuzzKey();
        sc.maxClients = 8;
        sc.transport = net.bind(kServer).value();
        server = Server::create(std::move(sc), 0.0).value();
    }
};

World& world() {
    static World w;
    return w;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    World& w = world();
    for (const auto& rec : fuzz::splitRecords(data, size, 1400)) {
        if (rec.empty()) continue;
        const Address from = Address::ipv4(192, 0, 2, static_cast<u8>(1 + (rec[0] & 3)), 50000);
        w.net.inject(from, kServer, rec);
        w.now += 0.001;
        w.server->update(w.now, w.handler);
        w.server->flush(w.now);
    }
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    // A genuine connection request (never-expiring token for this server) plus near misses.
    VirtualNetwork net;
    auto sink = net.bind(kServer).value();
    ClientConfig cc;
    cc.transport = net.bind(Address::ipv4(192, 0, 2, 1, 50000)).value();
    auto client = Client::create(std::move(cc), 0.0).value();
    ConnectTokenParams p;
    p.protocolId = kProtocol;
    p.clientId = 99;
    p.expireSeconds = -1;
    p.publicAddresses.push_back(kServer);
    p.privateKey = fuzzKey();
    const ConnectTokenBytes token = generateConnectToken(p).value();
    if (!client->connect(token, 0.0)) return;
    NullHandler h;
    client->update(0.01, h);
    u8 buf[kMaxDatagramBytes];
    Address from;
    std::vector<u8> request;
    if (const usize n = sink->receive(from, buf)) request.assign(buf, buf + n);
    if (request.empty()) return;
    std::vector<u8> one;
    fuzz::appendRecord(one, request);
    out.push_back(one);
    std::vector<u8> replay;
    for (int i = 0; i < 3; ++i) fuzz::appendRecord(replay, request);
    out.push_back(replay);
    std::vector<u8> keepAlive{0x14, 0x00};
    keepAlive.resize(26, 0x77);
    std::vector<u8> mixed;
    fuzz::appendRecord(mixed, keepAlive);
    std::vector<u8> payload{0x15, 0x01};
    payload.resize(40, 0x33);
    fuzz::appendRecord(mixed, payload);
    fuzz::appendRecord(mixed, std::vector<u8>{0x16, 0x02});
    out.push_back(mixed);
}
