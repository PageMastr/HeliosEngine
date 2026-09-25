#pragma once
// Shared helpers for engine/net tests: recording handlers, a back-to-back Connection pair over
// NetSim pipes, and a server/client harness over the in-process VirtualNetwork. Everything runs
// on a simulated clock, so tests are deterministic and fast.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "helios/net/net.h"

namespace helios::net::test {

inline std::vector<u8> bytesOf(std::string_view s) { return std::vector<u8>(s.begin(), s.end()); }

/// Payload with an embedded 32-bit index and a deterministic filler, verifiable on receipt.
inline std::vector<u8> makePayload(u32 index, usize size) {
    std::vector<u8> p(std::max<usize>(size, 4));
    std::memcpy(p.data(), &index, 4);
    for (usize i = 4; i < p.size(); ++i) p[i] = static_cast<u8>((index * 31u + static_cast<u32>(i) * 7u) & 0xFF);
    return p;
}
inline u32 payloadIndex(std::span<const u8> p) {
    u32 v = 0;
    if (p.size() >= 4) std::memcpy(&v, p.data(), 4);
    return v;
}
inline bool payloadValid(std::span<const u8> p) {
    if (p.size() < 4) return false;
    const u32 index = payloadIndex(p);
    for (usize i = 4; i < p.size(); ++i)
        if (p[i] != static_cast<u8>((index * 31u + static_cast<u32>(i) * 7u) & 0xFF)) return false;
    return true;
}

/// Records everything an endpoint delivers.
struct RecordingHandler final : IEndpointHandler {
    struct Msg {
        SessionHandle session;
        Channel channel;
        std::vector<u8> data;
    };
    std::vector<SessionHandle> connected;
    std::vector<std::pair<SessionHandle, DisconnectReason>> disconnected;
    std::vector<Msg> messages;
    std::vector<PacketNotify> notifies;

    void onConnected(SessionHandle s) override { connected.push_back(s); }
    void onDisconnected(SessionHandle s, DisconnectReason r) override { disconnected.emplace_back(s, r); }
    void onMessage(SessionHandle s, Channel c, std::span<const u8> p) override {
        messages.push_back(Msg{s, c, std::vector<u8>(p.begin(), p.end())});
    }
    void onDeliveryNotify(SessionHandle, std::span<const PacketNotify> n) override {
        notifies.insert(notifies.end(), n.begin(), n.end());
    }
    std::vector<Msg> on(Channel c) const {
        std::vector<Msg> out;
        for (const Msg& m : messages)
            if (m.channel == c) out.push_back(m);
        return out;
    }
};

/// Two Connections wired back to back through NetSim pipes (no netcode): A <-> B.
class LinkedPair {
public:
    LinkedPair(const ConnectionConfig& a, const ConnectionConfig& b, const NetSimLink& ab = {}, const NetSimLink& ba = {},
               u64 seed = 1)
        : m_ab(ab, seed), m_ba(ba, seed ^ 0xABCDEFull) {
        m_a = Connection::create(a, &LinkedPair::sendFromA, this, 0.0, "A").value();
        m_b = Connection::create(b, &LinkedPair::sendFromB, this, 0.0, "B").value();
    }

    Connection& a() { return *m_a; }
    Connection& b() { return *m_b; }
    NetSimPipe& pipeAB() { return m_ab; }
    NetSimPipe& pipeBA() { return m_ba; }
    f64 now() const { return m_now; }

    /// Advances time by `dt` in `substeps` updates, delivering due packets.
    void advance(f64 dt, int substeps = 1) {
        for (int i = 0; i < substeps; ++i) {
            m_now += dt / substeps;
            pump();
            m_a->update(m_now);
            m_b->update(m_now);
            pump();
        }
    }
    /// One tick: flush both sides, then advance.
    void tick(f64 dt, int substeps = 4) {
        m_a->flush(m_now);
        m_b->flush(m_now);
        advance(dt, substeps);
    }
    void pump() {
        u8 buf[kMaxDatagramBytes];
        Address peer;
        for (;;) {
            bool any = false;
            if (usize n = m_ab.pop(m_now, peer, buf)) {
                m_b->receivePacket(std::span<const u8>(buf, n), m_now);
                any = true;
            }
            if (usize n = m_ba.pop(m_now, peer, buf)) {
                m_a->receivePacket(std::span<const u8>(buf, n), m_now);
                any = true;
            }
            if (!any) break;
        }
    }
    /// Drains B's (or A's) inbox into `out`.
    static void drain(Connection& c, std::vector<std::pair<Channel, std::vector<u8>>>& out) {
        c.forEachMessage([&](const Connection::ReceivedMessage& m) {
            out.emplace_back(m.channel, std::vector<u8>(m.payload.begin(), m.payload.end()));
        });
        c.clearInbox();
    }
    u64 datagramsAB() const { return m_datagramsAB; }
    u64 datagramsBA() const { return m_datagramsBA; }
    u64 bytesAB() const { return m_bytesAB; }

private:
    static void sendFromA(void* ctx, std::span<const u8> p) {
        auto* self = static_cast<LinkedPair*>(ctx);
        ++self->m_datagramsAB;
        self->m_bytesAB += p.size();
        self->m_ab.push(self->m_now, Address::loopbackV4(2), p);
    }
    static void sendFromB(void* ctx, std::span<const u8> p) {
        auto* self = static_cast<LinkedPair*>(ctx);
        ++self->m_datagramsBA;
        self->m_ba.push(self->m_now, Address::loopbackV4(1), p);
    }

    NetSimPipe m_ab;
    NetSimPipe m_ba;
    std::unique_ptr<Connection> m_a;
    std::unique_ptr<Connection> m_b;
    f64 m_now = 0.0;
    u64 m_datagramsAB = 0;
    u64 m_datagramsBA = 0;
    u64 m_bytesAB = 0;
};

inline constexpr u64 kTestProtocolId = 0x48454C494F530001ull;

inline Key testKey(u8 seed = 7) {
    Key k{};
    for (usize i = 0; i < k.size(); ++i) k[i] = static_cast<u8>(seed + i * 13);
    return k;
}

/// Server + clients on a VirtualNetwork (optionally impaired by NetSim), simulated clock.
class Harness {
public:
    struct Options {
        Address serverAddress = Address::ipv4(10, 0, 0, 1, 7777);
        NetSimConfig serverSim{};  ///< Applied to the server's transport.
        NetSimConfig clientSim{};  ///< Applied to each client's transport.
        ConnectionConfig serverConnection = ConnectionConfig::server();
        ConnectionConfig clientConnection = ConnectionConfig::client();
        u32 maxClients = 64;
        bool fastHandshake = true;
        PreFilterConfig preFilter{};
    };

    Harness() : Harness(Options{}) {}
    explicit Harness(const Options& options) : m_options(options) {
        ServerConfig sc;
        sc.protocolId = kTestProtocolId;
        sc.privateKey = testKey();
        sc.maxClients = options.maxClients;
        sc.connection = options.serverConnection;
        sc.preFilter = options.preFilter;
        // A fixed bucket key keeps per-IP rate limiting reproducible (production keys are random).
        if (sc.preFilter.hashKey == 0) sc.preFilter.hashKey = 0x7E57'4E7F'1173'0001ull;
        auto sock = m_network.bind(options.serverAddress).value();
        m_serverSocket = sock.get();
        sc.transport = std::make_unique<NetSimTransport>(std::move(sock), options.serverSim);
        m_server = Server::create(std::move(sc), m_now).value();
    }

    Server& server() { return *m_server; }
    VirtualNetwork& network() { return m_network; }
    f64 now() const { return m_now; }
    RecordingHandler& serverEvents() { return m_serverEvents; }

    struct ClientSlot {
        std::unique_ptr<Client> client;
        RecordingHandler events;
        IEndpointHandler* handlerOverride = nullptr; ///< Replaces `events` when set.
        Address address;
    };
    void setClientHandler(usize i, IEndpointHandler* handler) { m_clients[i]->handlerOverride = handler; }

    /// Adds a client bound to 10.0.1.<n>:<port>; returns its index.
    usize addClient() {
        auto slot = std::make_unique<ClientSlot>();
        const u8 host = static_cast<u8>(1 + m_clients.size() % 250);
        slot->address = Address::ipv4(10, 0, 1, host, static_cast<u16>(40000 + m_clients.size()));
        ClientConfig cc;
        cc.connection = m_options.clientConnection;
        cc.fastHandshake = m_options.fastHandshake;
        NetSimConfig sim = m_options.clientSim;
        sim.seed ^= 0x1000 + m_clients.size();
        cc.transport = std::make_unique<NetSimTransport>(m_network.bind(slot->address).value(), sim);
        slot->client = Client::create(std::move(cc), m_now).value();
        m_clients.push_back(std::move(slot));
        return m_clients.size() - 1;
    }
    Client& client(usize i) { return *m_clients[i]->client; }
    RecordingHandler& clientEvents(usize i) { return m_clients[i]->events; }

    ConnectTokenBytes token(u64 clientId, i32 timeoutSeconds = 10, i32 expireSeconds = 45) {
        ConnectTokenParams p;
        p.protocolId = kTestProtocolId;
        p.clientId = clientId;
        p.timeoutSeconds = timeoutSeconds;
        p.expireSeconds = expireSeconds;
        p.publicAddresses = {m_options.serverAddress};
        p.privateKey = testKey();
        for (usize i = 0; i < p.userData.size(); ++i) p.userData[i] = static_cast<u8>(clientId + i);
        return generateConnectToken(p).value();
    }

    void connect(usize i, u64 clientId, i32 timeoutSeconds = 10) {
        const auto t = token(clientId, timeoutSeconds);
        REQUIRE(client(i).connect(t, m_now));
    }

    /// Advances the simulated clock by `dt` in `steps` updates (server + all clients).
    void run(f64 dt, int steps = 1, bool flush = true) {
        for (int s = 0; s < steps; ++s) {
            m_now += dt / steps;
            step(flush);
        }
    }
    void step(bool flush = true) {
        m_server->update(m_now, m_serverEvents);
        for (auto& c : m_clients) {
            if (c->handlerOverride) c->client->update(m_now, *c->handlerOverride);
            else c->client->update(m_now, c->events);
        }
        if (flush) {
            m_server->flush(m_now);
            for (auto& c : m_clients) c->client->flush(m_now);
        }
    }
    /// Runs until `pred` holds or `timeout` simulated seconds pass. Returns whether it held.
    template <class Pred>
    bool runUntil(Pred pred, f64 timeout, f64 dt = 0.005) {
        const f64 end = m_now + timeout;
        while (m_now < end) {
            if (pred()) return true;
            run(dt);
        }
        return pred();
    }
    bool allConnected() {
        for (auto& c : m_clients)
            if (!c->client->isConnected()) return false;
        return true;
    }
    SessionHandle serverSessionFor(u64 clientId) {
        for (SessionHandle h : m_server->sessions()) {
            const SessionInfo* info = m_server->sessionInfo(h);
            if (info && info->clientId == clientId) return h;
        }
        return {};
    }

private:
    Options m_options;
    VirtualNetwork m_network;
    VirtualNetwork::Socket* m_serverSocket = nullptr;
    std::unique_ptr<Server> m_server;
    RecordingHandler m_serverEvents;
    std::vector<std::unique_ptr<ClientSlot>> m_clients;
    f64 m_now = 1.0;
};

} // namespace helios::net::test
