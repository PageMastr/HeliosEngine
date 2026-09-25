// Full-stack HTP: netcode handshake with connect tokens, channels end to end under NetSim,
// handshake latency (NS-0.1), timeouts/reconnect, disconnects, token validation, pre-filter and
// malformed-peer handling.

#include <doctest/doctest.h>

#include <map>
#include <set>

#include <netcode.h>

#include "helios/core/time.h"
#include "net_test_util.h"
#include "netcode_glue.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::test;

namespace {

NetSimTransport& simOf(Client& c) { return static_cast<NetSimTransport&>(c.transport()); }

NetSimConfig lossy(f64 latencyMs, f64 lossPercent, u64 seed) {
    NetSimConfig c;
    c.seed = seed;
    for (NetSimLink* l : {&c.outbound, &c.inbound}) {
        l->latencyMs = latencyMs;
        l->jitterMs = latencyMs > 4 ? 3.0 : 0.0;
        l->lossPercent = lossPercent;
        l->duplicatePercent = 3.0;
        l->reorderPercent = 3.0;
        l->reorderDelayMs = 12.0;
    }
    return c;
}

/// A netcode client driven directly through the C API over a VirtualNetwork socket, so a test
/// can put arbitrary (authenticated) bytes on the wire: the "malicious but authenticated" peer.
class RawNetcodeClient {
public:
    RawNetcodeClient(VirtualNetwork& net, const Address& bind, f64 now) {
        m_socket = net.bind(bind).value();
        netcode_client_config_t cc;
        netcode_default_client_config(&cc);
        cc.callback_context = this;
        cc.override_send_and_receive = 1;
        cc.send_packet_override = &RawNetcodeClient::send;
        cc.receive_packet_override = &RawNetcodeClient::receive;
        const std::string addr = bind.toString();
        m_client = netcode_client_create(addr.c_str(), &cc, now);
        REQUIRE(m_client);
    }
    ~RawNetcodeClient() { netcode_client_destroy(m_client); }
    void connect(ConnectTokenBytes token) {
        m_token = token;
        netcode_client_connect(m_client, m_token.data());
    }
    void update(f64 now) { netcode_client_update(m_client, now); }
    bool connected() { return netcode_client_state(m_client) == NETCODE_CLIENT_STATE_CONNECTED; }
    int state() { return netcode_client_state(m_client); }
    void sendRaw(std::vector<u8> bytes) { netcode_client_send_packet(m_client, bytes.data(), static_cast<int>(bytes.size())); }

private:
    static void send(void* ctx, netcode_address_t* to, const uint8_t* data, int bytes) {
        static_cast<RawNetcodeClient*>(ctx)->m_socket->send(net::detail::fromNetcode(*to),
                                                            std::span<const u8>(data, static_cast<usize>(bytes)));
    }
    static int receive(void* ctx, netcode_address_t* from, uint8_t* data, int maxBytes) {
        Address a;
        const usize n = static_cast<RawNetcodeClient*>(ctx)->m_socket->receive(a, std::span<u8>(data, static_cast<usize>(maxBytes)));
        if (n == 0) return 0;
        *from = net::detail::toNetcode(a);
        return static_cast<int>(n);
    }
    std::unique_ptr<VirtualNetwork::Socket> m_socket;
    netcode_client_t* m_client = nullptr;
    ConnectTokenBytes m_token{};
};

} // namespace

TEST_SUITE("net.endpoint") {
    TEST_CASE("loopback connect via connect token; messages both ways; session info") {
        Harness h;
        const usize c = h.addClient();
        h.connect(c, 0xC0FFEE);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected() && h.server().connectedCount() == 1; }, 2.0));
        REQUIRE(h.serverEvents().connected.size() == 1);
        REQUIRE(h.clientEvents(c).connected.size() == 1);
        const SessionHandle s = h.serverEvents().connected[0];
        const SessionInfo* info = h.server().sessionInfo(s);
        REQUIRE(info);
        CHECK(info->clientId == 0xC0FFEE);
        CHECK(info->address == Address::ipv4(10, 0, 1, 1, 40000));
        for (usize i = 0; i < kUserDataBytes; ++i) REQUIRE(info->userData[i] == static_cast<u8>(0xC0FFEE + i));
        CHECK(h.client(c).clientIndex() == static_cast<i32>(s.index()));
        CHECK(h.client(c).serverAddress() == Address::ipv4(10, 0, 0, 1, 7777));

        REQUIRE(h.server().send(s, Channel::Control, bytesOf("welcome")) == SendResult::Ok);
        REQUIRE(h.client(c).send(Channel::EventReliable, bytesOf("hello")) == SendResult::Ok);
        u64 stateSeq = 0;
        REQUIRE(h.server().sendState(s, bytesOf("chunk"), &stateSeq) == SendResult::Ok);
        REQUIRE(h.runUntil([&] { return h.clientEvents(c).messages.size() == 2 && !h.serverEvents().messages.empty() &&
                                        !h.serverEvents().notifies.empty(); },
                           1.0));
        CHECK(h.clientEvents(c).on(Channel::Control)[0].data == bytesOf("welcome"));
        CHECK(h.clientEvents(c).on(Channel::State)[0].data == bytesOf("chunk"));
        CHECK(h.serverEvents().on(Channel::EventReliable)[0].data == bytesOf("hello"));
        CHECK(h.serverEvents().notifies[0].seq == stateSeq);
        CHECK(h.serverEvents().notifies[0].delivered);
        CHECK(h.server().stats().connects == 1);
    }

    TEST_CASE("NS-0.1: the server completes the handshake 1.5 RTT after the first request") {
        for (const bool fast : {true, false}) {
            CAPTURE(fast);
            Harness::Options o;
            o.clientSim.outbound.latencyMs = 25.0; // RTT 50 ms
            o.clientSim.inbound.latencyMs = 25.0;
            o.fastHandshake = fast;
            Harness h(o);
            const usize c = h.addClient();
            h.connect(c, 1);
            const f64 start = h.now();
            constexpr f64 kStep = 0.0001; // simulation granularity; each hop can cost one step
            REQUIRE(h.runUntil([&] { return !h.serverEvents().connected.empty(); }, 1.0, kStep));
            const f64 serverDone = h.now() - start;
            REQUIRE(h.runUntil([&] { return h.client(c).isConnected(); }, 1.0, kStep));
            const f64 clientDone = h.client(c).connectedTime() - h.client(c).connectStartTime();
            MESSAGE("fastHandshake=" << fast << ": server connected after " << serverDone * 1000.0
                                     << " ms, client after " << clientDone * 1000.0 << " ms (RTT 50 ms)");
            if (fast) {
                CHECK(serverDone <= 0.075 + 10 * kStep); // 1.5 RTT
                CHECK(serverDone >= 0.075);
                CHECK(clientDone <= 0.100 + 10 * kStep); // + the keep-alive back to the client
            } else {
                CHECK(serverDone >= 0.100); // netcode's 10 Hz handshake pacing
            }
        }
    }

    TEST_CASE("all channel guarantees end to end: 16 clients, 10% loss, reorder, duplication") {
        Harness::Options o;
        o.clientSim = lossy(15.0, 10.0, 77);
        Harness h(o);
        constexpr usize kClients = 16;
        for (usize i = 0; i < kClients; ++i) {
            h.addClient();
            h.connect(i, 1000 + i);
        }
        REQUIRE(h.runUntil([&] { return h.allConnected() && h.server().connectedCount() == kClients; }, 10.0));
        std::map<u64, SessionHandle> sessions;
        for (usize i = 0; i < kClients; ++i) sessions[1000 + i] = h.serverSessionFor(1000 + i);
        constexpr u32 kMessages = 150;
        for (u32 m = 0; m < kMessages; ++m) {
            for (usize i = 0; i < kClients; ++i) {
                const SessionHandle s = sessions[1000 + i];
                REQUIRE(h.server().send(s, Channel::EventReliable, makePayload(m, 64 + m % 200)) == SendResult::Ok);
                REQUIRE(h.server().send(s, Channel::Latest, makePayload(m, 16)) == SendResult::Ok);
                REQUIRE(h.client(i).send(Channel::Control, makePayload(m, 32)) == SendResult::Ok);
                REQUIRE(h.client(i).send(Channel::Input, makePayload(m, 12)) == SendResult::Ok);
            }
            h.run(0.02, 4);
        }
        h.run(3.0, 600);
        // Server side: per client, CONTROL exactly once in order; INPUT in order, nearly all.
        std::map<u32, u32> nextControl, inputs;
        std::map<u32, i64> lastInput;
        for (const auto& msg : h.serverEvents().messages) {
            const u32 slot = msg.session.index();
            REQUIRE(payloadValid(msg.data));
            if (msg.channel == Channel::Control) {
                CHECK(payloadIndex(msg.data) == nextControl[slot]);
                ++nextControl[slot];
            } else if (msg.channel == Channel::Input) {
                const i64 idx = payloadIndex(msg.data);
                CHECK(idx > (lastInput.count(slot) ? lastInput[slot] : -1));
                lastInput[slot] = idx;
                ++inputs[slot];
            }
        }
        for (usize i = 0; i < kClients; ++i) {
            const u32 slot = sessions[1000 + i].index();
            CHECK(nextControl[slot] == kMessages);
            CHECK(inputs[slot] >= kMessages - 3);
            // Client side: EVENT_R exactly once in order; LATEST strictly increasing.
            u32 next = 0;
            i64 lastLatest = -1;
            for (const auto& msg : h.clientEvents(i).messages) {
                REQUIRE(payloadValid(msg.data));
                if (msg.channel == Channel::EventReliable) {
                    CHECK(payloadIndex(msg.data) == next);
                    ++next;
                } else if (msg.channel == Channel::Latest) {
                    CHECK(static_cast<i64>(payloadIndex(msg.data)) > lastLatest);
                    lastLatest = payloadIndex(msg.data);
                }
            }
            CHECK(next == kMessages);
        }
        CHECK(simOf(h.client(0)).outboundStats().lost > 0);
        CHECK(simOf(h.client(0)).inboundStats().duplicated > 0);
        CHECK(h.server().stats().malformedDisconnects == 0);
    }

    TEST_CASE("timeouts, linkdead detection and reconnect with a fresh token") {
        Harness h;
        const usize c = h.addClient();
        h.connect(c, 42, /*timeoutSeconds=*/1);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected(); }, 2.0));
        REQUIRE(h.runUntil([&] { return h.server().connectedCount() == 1; }, 1.0));
        const SessionHandle first = h.serverSessionFor(42);
        const SessionHandle clientFirst = h.client(c).session();
        // Keep-alives hold an idle connection open well past the timeout.
        h.run(3.0, 300);
        CHECK(h.client(c).isConnected());
        CHECK(h.server().isConnected(first));
        // Cut the link both ways: both sides time out after the token's 1 s.
        NetSimConfig blackhole;
        blackhole.outbound.lossPercent = 100.0;
        blackhole.inbound.lossPercent = 100.0;
        simOf(h.client(c)).setConfig(blackhole);
        REQUIRE(h.runUntil([&] { return !h.client(c).isConnected() && h.server().connectedCount() == 0; }, 3.0));
        CHECK(h.client(c).state() == ClientState::TimedOut);
        REQUIRE(h.clientEvents(c).disconnected.size() == 1);
        CHECK(h.clientEvents(c).disconnected[0].first == clientFirst);
        CHECK(h.clientEvents(c).disconnected[0].second == DisconnectReason::TimedOut);
        REQUIRE(h.serverEvents().disconnected.size() == 1);
        CHECK(h.serverEvents().disconnected[0].second == DisconnectReason::TimedOut);
        CHECK(h.server().send(first, Channel::Control, bytesOf("x")) == SendResult::NotConnected);
        // The network heals; the client redeems a fresh token (04 §2.4) on the same Client object.
        simOf(h.client(c)).setConfig(NetSimConfig{});
        h.connect(c, 42, 1);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected() && h.server().connectedCount() == 1; }, 2.0));
        const SessionHandle second = h.serverSessionFor(42);
        CHECK(second.isValid());
        CHECK(h.client(c).session() != clientFirst);
        CHECK_FALSE(h.server().isConnected(first));
        REQUIRE(h.server().send(second, Channel::EventReliable, bytesOf("back")) == SendResult::Ok);
        REQUIRE(h.runUntil([&] { return !h.clientEvents(c).on(Channel::EventReliable).empty(); }, 1.0));
    }

    TEST_CASE("graceful disconnects from either side") {
        Harness h;
        const usize a = h.addClient();
        const usize b = h.addClient();
        h.connect(a, 1);
        h.connect(b, 2);
        REQUIRE(h.runUntil([&] { return h.allConnected() && h.server().connectedCount() == 2; }, 2.0));
        h.client(a).disconnect();
        REQUIRE(h.runUntil([&] { return h.server().connectedCount() == 1; }, 1.0));
        REQUIRE(h.serverEvents().disconnected.size() == 1);
        CHECK(h.serverEvents().disconnected[0].second == DisconnectReason::ClientDisconnected);
        h.run(0.01);
        REQUIRE(h.clientEvents(a).disconnected.size() == 1);
        CHECK(h.clientEvents(a).disconnected[0].second == DisconnectReason::ClientDisconnected);

        const SessionHandle sb = h.serverSessionFor(2);
        h.server().disconnect(sb);
        REQUIRE(h.runUntil([&] { return !h.client(b).isConnected(); }, 1.0));
        REQUIRE(h.clientEvents(b).disconnected.size() == 1);
        CHECK(h.clientEvents(b).disconnected[0].second == DisconnectReason::ServerDisconnected);
        h.run(0.01);
        REQUIRE(h.serverEvents().disconnected.size() == 2);
        CHECK(h.serverEvents().disconnected[1].second == DisconnectReason::ServerDisconnected);
        CHECK(h.client(b).send(Channel::Control, bytesOf("x")) == SendResult::NotConnected);
    }

    TEST_CASE("stale session handles never reach the next client in the slot") {
        Harness::Options o;
        o.maxClients = 1;
        Harness h(o);
        const usize a = h.addClient();
        const usize b = h.addClient();
        h.connect(a, 1);
        REQUIRE(h.runUntil([&] { return h.client(a).isConnected(); }, 1.0));
        h.run(0.05, 5);
        const SessionHandle sa = h.serverSessionFor(1);
        h.client(a).disconnect();
        h.run(0.05, 5);
        h.connect(b, 2);
        REQUIRE(h.runUntil([&] { return h.client(b).isConnected(); }, 1.0));
        h.run(0.05, 5);
        const SessionHandle sb = h.serverSessionFor(2);
        CHECK(sb.index() == sa.index());
        CHECK(sb.generation() != sa.generation());
        CHECK(h.server().send(sa, Channel::Control, bytesOf("for a")) == SendResult::NotConnected);
        CHECK(h.server().sessionInfo(sa) == nullptr);
        CHECK(h.server().send(sb, Channel::Control, bytesOf("for b")) == SendResult::Ok);
    }

    TEST_CASE("token validation: protocol, key, address, expiry, server full, reuse") {
        SUBCASE("wrong protocol id / wrong key / other server's address are ignored") {
            Harness h;
            ConnectTokenParams p;
            p.protocolId = kTestProtocolId + 1;
            p.clientId = 1;
            p.timeoutSeconds = 1;
            p.publicAddresses = {Address::ipv4(10, 0, 0, 1, 7777)};
            p.privateKey = testKey();
            const usize c1 = h.addClient();
            REQUIRE(h.client(c1).connect(generateConnectToken(p).value(), h.now()));
            p.protocolId = kTestProtocolId;
            p.privateKey = testKey(99);
            p.clientId = 2;
            const usize c2 = h.addClient();
            REQUIRE(h.client(c2).connect(generateConnectToken(p).value(), h.now()));
            p.privateKey = testKey();
            p.clientId = 3;
            p.internalAddresses = {Address::ipv4(10, 0, 0, 2, 7777)}; // the server checks this list
            const usize c3 = h.addClient();
            REQUIRE(h.client(c3).connect(generateConnectToken(p).value(), h.now()));
            h.run(2.0, 400);
            CHECK(h.server().connectedCount() == 0);
            CHECK(h.client(c1).state() == ClientState::RequestTimedOut);
            CHECK(h.client(c2).state() == ClientState::RequestTimedOut);
            CHECK(h.client(c3).state() == ClientState::RequestTimedOut);
            CHECK(h.clientEvents(c1).disconnected.empty()); // never connected: no disconnect event
        }
        SUBCASE("expired token") {
            Harness h;
            const usize c = h.addClient();
            const auto t = h.token(5, 10, /*expireSeconds=*/0);
            REQUIRE(h.client(c).connect(t, h.now()));
            h.run(1.0, 100);
            CHECK(h.server().connectedCount() == 0);
            CHECK(h.client(c).state() == ClientState::ConnectTokenExpired);
        }
        SUBCASE("server full: denied") {
            Harness::Options o;
            o.maxClients = 1;
            Harness h(o);
            const usize a = h.addClient();
            const usize b = h.addClient();
            h.connect(a, 1);
            REQUIRE(h.runUntil([&] { return h.client(a).isConnected(); }, 1.0));
            h.connect(b, 2);
            REQUIRE(h.runUntil([&] { return h.client(b).state() == ClientState::Denied; }, 2.0));
            CHECK(h.server().connectedCount() == 1);
        }
        SUBCASE("a token is single-use: a second address presenting it is ignored") {
            Harness h;
            const usize a = h.addClient();
            const usize b = h.addClient();
            const auto t = h.token(7);
            REQUIRE(h.client(a).connect(t, h.now()));
            REQUIRE(h.runUntil([&] { return h.client(a).isConnected(); }, 1.0));
            REQUIRE(h.client(b).connect(t, h.now()));
            h.run(1.0, 200);
            CHECK_FALSE(h.client(b).isConnected());
            CHECK(h.server().connectedCount() == 1);
        }
        SUBCASE("malformed token bytes") {
            Harness h;
            const usize c = h.addClient();
            ConnectTokenBytes junk{};
            CHECK_FALSE(h.client(c).connect(junk, h.now()));
            CHECK_FALSE(h.client(c).connect(std::span<const u8>(junk.data(), 100), h.now()));
        }
    }

    TEST_CASE("L0 pre-filter drops impossible datagrams and request floods") {
        Harness h;
        const Address server = Address::ipv4(10, 0, 0, 1, 7777);
        const Address attacker = Address::ipv4(66, 6, 6, 6, 1234);
        std::vector<u8> junk(100, 0x55);
        junk[0] = 0x0F; // type 15
        CHECK(h.network().inject(attacker, server, junk));
        junk[0] = 0x02; // challenge: never valid towards a server
        h.network().inject(attacker, server, junk);
        junk[0] = 0x15; // payload with 1 sequence byte but ...
        junk.resize(5); // ... shorter than a MAC
        h.network().inject(attacker, server, junk);
        std::vector<u8> request(1078, 0);
        for (int i = 0; i < 30; ++i) h.network().inject(attacker, server, request); // 30 requests at once
        request.resize(1077);
        h.network().inject(attacker, server, request);
        h.run(0.01);
        const PreFilterStats s = h.server().stats().preFilter;
        CHECK(s.badType == 2);
        CHECK(s.badSize == 2);
        CHECK(s.rateLimited == 20); // burst of 10 per IP
        // Legitimate clients are unaffected.
        const usize c = h.addClient();
        h.connect(c, 9);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected(); }, 1.0));
        // The limit refills at 10/s.
        h.run(1.0, 10);
        for (int i = 0; i < 12; ++i) h.network().inject(attacker, server, std::vector<u8>(1078, 0));
        h.run(0.01);
        CHECK(h.server().stats().preFilter.rateLimited == 22);
    }

    TEST_CASE("an authenticated peer sending malformed packets is disconnected after 3 strikes") {
        Harness h;
        RawNetcodeClient evil(h.network(), Address::ipv4(10, 9, 9, 9, 999), h.now());
        evil.connect(h.token(666));
        REQUIRE(h.runUntil(
            [&] {
                evil.update(h.now());
                return evil.connected() && h.server().connectedCount() == 1;
            },
            2.0));
        const SessionHandle s = h.serverSessionFor(666);
        REQUIRE(s.isValid());
        // A well-formed reliable header followed by garbage HTP bytes, then pure garbage.
        evil.sendRaw({0x20, 0x00, 0x00, 0x00, 0x09, 0x01}); // reliable seq 0, ack 0 + channel 9
        evil.sendRaw({0x20, 0x01, 0x00, 0x01, 0x44, 0x00}); // reserved bits
        evil.sendRaw({0x00});                                // unreadable reliable header
        REQUIRE(h.runUntil(
            [&] {
                evil.update(h.now());
                return h.server().connectedCount() == 0;
            },
            1.0));
        REQUIRE(h.serverEvents().disconnected.size() == 1);
        CHECK(h.serverEvents().disconnected[0].first == s);
        CHECK(h.serverEvents().disconnected[0].second == DisconnectReason::Malformed);
        CHECK(h.server().stats().malformedDisconnects == 1);
        CHECK(h.serverEvents().on(Channel::Control).empty());
    }

    TEST_CASE("handlers may send and disconnect from callbacks") {
        struct Echo final : IEndpointHandler {
            Server* server = nullptr;
            std::vector<SessionHandle> kicked;
            std::vector<SendResult> echoes;
            void onConnected(SessionHandle s) override { server->send(s, Channel::Control, bytesOf("hi")); }
            void onMessage(SessionHandle s, Channel ch, std::span<const u8> p) override {
                if (p.size() == 4 && std::memcmp(p.data(), "kick", 4) == 0) {
                    server->disconnect(s); // deferred until after delivery
                    kicked.push_back(s);
                    return;
                }
                echoes.push_back(server->send(s, ch, p));
            }
        };
        VirtualNetwork net;
        ServerConfig sc;
        sc.protocolId = kTestProtocolId;
        sc.privateKey = testKey();
        sc.transport = net.bind(Address::ipv4(10, 0, 0, 1, 7777)).value();
        auto server = Server::create(std::move(sc), 0.0).value();
        Echo echo;
        echo.server = server.get();
        ClientConfig cc;
        cc.transport = net.bind(Address::ipv4(10, 0, 1, 1, 5)).value();
        auto client = Client::create(std::move(cc), 0.0).value();
        RecordingHandler events;
        ConnectTokenParams p;
        p.protocolId = kTestProtocolId;
        p.clientId = 77;
        p.publicAddresses = {Address::ipv4(10, 0, 0, 1, 7777)};
        p.privateKey = testKey();
        REQUIRE(client->connect(generateConnectToken(p).value(), 0.0));
        f64 t = 0.0;
        const auto step = [&] {
            t += 0.005;
            server->update(t, echo);
            client->update(t, events);
            server->flush(t);
            client->flush(t);
        };
        for (int i = 0; i < 100 && events.messages.empty(); ++i) step();
        REQUIRE(events.messages.size() == 1);
        CHECK(events.messages[0].data == bytesOf("hi"));
        client->send(Channel::EventReliable, bytesOf("ping"));
        client->send(Channel::EventReliable, bytesOf("kick"));
        client->send(Channel::EventReliable, bytesOf("late"));
        for (int i = 0; i < 100 && client->isConnected(); ++i) step();
        CHECK_FALSE(client->isConnected());
        CHECK(echo.kicked.size() == 1);
        // "ping" was echoed; "late" arrived after the kick request and was refused.
        REQUIRE(echo.echoes.size() == 2);
        CHECK(echo.echoes[0] == SendResult::Ok);
        CHECK(echo.echoes[1] == SendResult::NotConnected);
        REQUIRE(!events.disconnected.empty());
        CHECK(events.disconnected.back().second == DisconnectReason::ServerDisconnected);
        CHECK(server->connectedCount() == 0);
    }

    TEST_CASE("a client handler may disconnect mid-delivery") {
        Harness h;
        const usize c = h.addClient();
        h.connect(c, 5);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected() && h.server().connectedCount() == 1; }, 1.0));
        const SessionHandle s = h.serverSessionFor(5);
        struct Quitter final : IEndpointHandler {
            Client* client = nullptr;
            int messages = 0;
            SendResult afterQuit = SendResult::Ok;
            std::vector<DisconnectReason> reasons;
            Result<void> reconnect;
            void onMessage(SessionHandle, Channel, std::span<const u8>) override {
                if (++messages == 1) {
                    client->disconnect(); // must not destroy the connection being iterated
                    afterQuit = client->send(Channel::Control, bytesOf("x"));
                    reconnect = client->connect(ConnectTokenBytes{}, 0.0);
                }
            }
            void onDisconnected(SessionHandle, DisconnectReason r) override { reasons.push_back(r); }
        } quitter;
        quitter.client = &h.client(c);
        h.setClientHandler(c, &quitter);
        for (int i = 0; i < 5; ++i) REQUIRE(h.server().send(s, Channel::EventReliable, makePayload(i, 20)) == SendResult::Ok);
        REQUIRE(h.runUntil([&] { return !h.client(c).isConnected(); }, 1.0));
        CHECK_FALSE(h.client(c).isConnected());
        CHECK(quitter.messages == 5); // the batch in flight is still delivered
        CHECK(quitter.afterQuit == SendResult::NotConnected);
        CHECK(quitter.reconnect.errorCode() == ErrorCode::InvalidState);
        REQUIRE(quitter.reasons.size() == 1);
        CHECK(quitter.reasons[0] == DisconnectReason::ClientDisconnected);
        REQUIRE(h.runUntil([&] { return h.server().connectedCount() == 0; }, 1.0));
    }

    TEST_CASE("real UDP sockets on loopback (IPv4, and IPv6 when available)") {
        std::vector<Address> binds = {Address::loopbackV4(0)};
        if (UdpSocket::isIpv6Supported()) binds.push_back(Address::loopbackV6(0));
        else MESSAGE("IPv6 sockets unavailable here; IPv6 is covered over VirtualNetwork");
        for (const Address& bind : binds) {
            CAPTURE(bind);
            ServerConfig sc;
            sc.protocolId = kTestProtocolId;
            sc.privateKey = testKey();
            sc.bindAddress = bind;
            sc.maxClients = 4;
            auto server = Server::create(std::move(sc), monotonicSeconds()).value();
            REQUIRE(server->publicAddresses().size() == 1);
            const Address pub = server->publicAddresses()[0];
            CHECK(pub.port() != 0);
            auto client = Client::create(ClientConfig{}, monotonicSeconds()).value();
            ConnectTokenParams p;
            p.protocolId = kTestProtocolId;
            p.clientId = 5;
            p.publicAddresses = {pub};
            p.privateKey = testKey();
            REQUIRE(client->connect(generateConnectToken(p).value(), monotonicSeconds()));
            RecordingHandler se, ce;
            const f64 deadline = monotonicSeconds() + 5.0;
            bool sent = false;
            while (monotonicSeconds() < deadline) {
                const f64 now = monotonicSeconds();
                server->update(now, se);
                client->update(now, ce);
                if (client->isConnected() && !se.connected.empty() && !sent) {
                    REQUIRE(client->send(Channel::EventReliable, bytesOf("udp ping")) == SendResult::Ok);
                    REQUIRE(server->send(se.connected[0], Channel::EventReliable, bytesOf("udp pong")) == SendResult::Ok);
                    sent = true;
                }
                server->flush(now);
                client->flush(now);
                if (!ce.messages.empty() && !se.messages.empty()) break;
                sleepMillis(1);
            }
            REQUIRE(client->isConnected());
            REQUIRE(se.messages.size() == 1);
            REQUIRE(ce.messages.size() == 1);
            CHECK(se.messages[0].data == bytesOf("udp ping"));
            CHECK(ce.messages[0].data == bytesOf("udp pong"));
            const SessionInfo* info = server->sessionInfo(se.connected[0]);
            REQUIRE(info);
            CHECK(info->address.isLoopback());
        }
    }

    TEST_CASE("pre-filter buckets are keyed: a precomputed colliding source cannot starve a victim") {
        // Regression: buckets were indexed by an unkeyed hash of the source address, so an
        // attacker could compute a spoofable source that shares a player's bucket and keep it
        // empty with 10 requests/s, blocking that player's connection requests.
        const Address victim = Address::ipv4(203, 0, 113, 50, 40000);
        const std::vector<u8> request(1078, 0);
        PreFilterConfig known;
        known.hashKey = 0x1111;
        PreFilter f1(PreFilter::Role::Server, known);
        Address attacker;
        for (u32 i = 1; i < 65536 && !attacker.isValid(); ++i) {
            const Address a = Address::ipv4(198, 18, static_cast<u8>(i >> 8), static_cast<u8>(i & 0xFF), 1234);
            if (f1.bucketIndex(a) == f1.bucketIndex(victim)) attacker = a;
        }
        REQUIRE(attacker.isValid());
        for (int i = 0; i < 10; ++i) REQUIRE(f1.check(attacker, request, 1.0) == PreFilter::Verdict::Accept);
        CHECK(f1.check(victim, request, 1.0) == PreFilter::Verdict::RateLimited); // what a known key allows
        // A different (secret) key scatters the same pair.
        PreFilterConfig other;
        other.hashKey = 0x2222;
        PreFilter f2(PreFilter::Role::Server, other);
        REQUIRE(f2.bucketIndex(attacker) != f2.bucketIndex(victim));
        for (int i = 0; i < 30; ++i) (void)f2.check(attacker, request, 1.0);
        CHECK(f2.check(victim, request, 1.0) == PreFilter::Verdict::Accept);
        // Servers draw a random key per instance by default.
        PreFilter r1(PreFilter::Role::Server, PreFilterConfig{});
        PreFilter r2(PreFilter::Role::Server, PreFilterConfig{});
        CHECK(r1.hashKey() != 0);
        CHECK(r1.hashKey() != r2.hashKey());
    }

    TEST_CASE("pre-filter rate-limits IPv6 sources per /64 and IPv4-mapped sources as IPv4") {
        PreFilterConfig c;
        c.hashKey = 7;
        PreFilter f(PreFilter::Role::Server, c);
        const std::vector<u8> request(1078, 0);
        const Address a1 = Address::parse("[2001:db8:0:5::1]:1000").value();
        const Address a2 = Address::parse("[2001:db8:0:5:ffff:1:2:3]:2000").value(); // same /64
        const Address other = Address::parse("[2001:db8:0:6::1]:1000").value();      // next /64
        CHECK(f.bucketIndex(a1) == f.bucketIndex(a2));
        REQUIRE(f.bucketIndex(a1) != f.bucketIndex(other));
        for (int i = 0; i < 10; ++i) REQUIRE(f.check(a1, request, 1.0) == PreFilter::Verdict::Accept);
        CHECK(f.check(a2, request, 1.0) == PreFilter::Verdict::RateLimited); // rotating inside the /64
        CHECK(f.check(other, request, 1.0) == PreFilter::Verdict::Accept);
        const Address v4 = Address::ipv4(198, 51, 100, 7, 1);
        CHECK(f.bucketIndex(v4) == f.bucketIndex(v4.toV4Mapped()));
        CHECK(f.bucketIndex(v4) == f.bucketIndex(v4.withPort(999)));
    }

    TEST_CASE("NetSim stamps datagrams sent by flush() with the flush time") {
        // Regression: flush() did not advance the transport clock, so a NetSimTransport timed a
        // tick's packets from the previous update() and delivered them early.
        Harness::Options o;
        o.clientSim.outbound.latencyMs = 10.0;
        Harness h(o);
        const usize c = h.addClient();
        h.connect(c, 1);
        REQUIRE(h.runUntil([&] { return h.client(c).isConnected() && h.server().connectedCount() == 1; }, 2.0));
        h.run(0.1, 20);
        const f64 t = h.now();
        // The client polled (update) at t; its tick flushes 50 ms later. Due at t + 60 ms.
        REQUIRE(h.client(c).send(Channel::EventReliable, bytesOf("late")) == SendResult::Ok);
        h.client(c).flush(t + 0.05);
        h.run(0.058, 58, /*flush=*/false);
        CHECK(h.serverEvents().on(Channel::EventReliable).empty());
        h.run(0.01, 10, /*flush=*/false);
        CHECK(h.serverEvents().on(Channel::EventReliable).size() == 1);
    }

    TEST_CASE("reconnect from a new address (NAT rebinding) needs the old session dropped first") {
        Harness h;
        const usize a = h.addClient();
        h.connect(a, 4242);
        REQUIRE(h.runUntil([&] { return h.client(a).isConnected() && h.server().connectedCount() == 1; }, 2.0));
        const SessionHandle old = h.server().findSession(4242);
        REQUIRE(old.isValid());
        CHECK(old == h.serverSessionFor(4242));
        CHECK_FALSE(h.server().findSession(999).isValid());
        // The same session (client id) arrives from a new address with a fresh token while the old
        // path is silently gone: netcode ignores it as a duplicate client id...
        NetSimConfig blackhole;
        blackhole.outbound.lossPercent = 100.0;
        blackhole.inbound.lossPercent = 100.0;
        static_cast<NetSimTransport&>(h.client(a).transport()).setConfig(blackhole);
        const usize b = h.addClient();
        h.connect(b, 4242);
        h.run(1.0, 200);
        CHECK_FALSE(h.client(b).isConnected());
        CHECK(h.server().isConnected(old));
        // ... until the gateway drops the old copy (the session_epoch signal, 04 §2.4).
        h.server().disconnect(h.server().findSession(4242));
        REQUIRE(h.runUntil([&] { return h.client(b).isConnected(); }, 1.0));
        CHECK(h.server().findSession(4242).isValid());
        CHECK(h.server().findSession(4242) != old);
    }
}
