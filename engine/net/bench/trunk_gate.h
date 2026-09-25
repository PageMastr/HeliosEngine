#pragma once
// NS-0.7 trunk throughput gate and NS-0.2 loopback pps measurement, shared by net_tests (short
// runs) and net_bench (full-length gate). Header-only; test/bench code, not part of helios_net.
//
// Trunk gate: a cell-side Server and a gateway-side Client, both with ConnectionConfig::trunk(),
// connect over real UDP loopback with a connect token. The cell sends STATE messages that fill a
// whole datagram (1,188 B payload + 3 B HTP header + reliable header = the 1,200 B netcode payload
// NS-0.7 specifies) at a target rate from its own thread; the gateway receives on another thread.
// Reported: delivered pps, payload Mbit/s, drop %, CPU cores per side.

#include <atomic>
#include <thread>
#include <vector>

#include "helios/core/time.h"
#include "helios/net/net.h"
#include "platform/net_os.h"

namespace helios::net::bench {

struct TrunkGateConfig {
    f64 seconds = 1.0;
    u32 targetPps = 20'000;
    /// STATE payload per datagram. Default: the largest that fits one packet, so every netcode
    /// payload is ~1,200 B (NS-0.7: "20k pps of 1,200 B payloads (~200 Mbit/s)").
    u32 payloadBytes = wire::maxPayloadFor(Channel::State, wire::kMaxPacketPayload);
    u32 socketBufferBytes = 32u * 1024 * 1024;
};

struct TrunkGateResult {
    bool connected = false;
    u64 sent = 0;
    u64 delivered = 0;
    u64 notifiedLost = 0;
    f64 seconds = 0.0;
    f64 deliveredPps = 0.0;
    f64 payloadMbps = 0.0;
    f64 wireMbps = 0.0;
    f64 dropPercent = 0.0;
    f64 senderCores = 0.0;   ///< CPU seconds / wall seconds of the sending (cell) thread.
    f64 receiverCores = 0.0; ///< Same for the receiving (gateway) thread.
    u64 receiverRecvSyscalls = 0;
    u64 senderSendSyscalls = 0;
    u32 grantedReceiveBuffer = 0;
};

namespace detail {
struct CountingHandler final : IEndpointHandler {
    std::atomic<u64> messages{0};
    std::atomic<u64> lost{0};
    std::vector<SessionHandle> sessions;
    void onConnected(SessionHandle s) override { sessions.push_back(s); }
    void onMessage(SessionHandle, Channel, std::span<const u8>) override {
        messages.fetch_add(1, std::memory_order_relaxed);
    }
    void onDeliveryNotify(SessionHandle, std::span<const PacketNotify> n) override {
        u64 l = 0;
        for (const PacketNotify& x : n) l += x.delivered ? 0 : 1;
        lost.fetch_add(l, std::memory_order_relaxed);
    }
};
} // namespace detail

inline TrunkGateResult runTrunkGate(const TrunkGateConfig& cfg) {
    TrunkGateResult r;
    const Key key = generateKey();
    constexpr u64 kProtocol = 0x48454C494F54524Eull; // "HELIOTRN"

    ServerConfig sc;
    sc.name = "cell";
    sc.protocolId = kProtocol;
    sc.privateKey = key;
    sc.bindAddress = Address::loopbackV4(0);
    sc.maxClients = 4;
    sc.connection = ConnectionConfig::trunk();
    sc.socket = SocketTransportConfig::trunk();
    sc.socket.socket.sendBufferBytes = cfg.socketBufferBytes;
    sc.socket.socket.receiveBufferBytes = cfg.socketBufferBytes;
    sc.preFilter.enabled = true;
    auto serverR = Server::create(std::move(sc), monotonicSeconds());
    if (!serverR) return r;
    auto server = std::move(serverR).value();

    ClientConfig cc;
    cc.name = "gateway";
    cc.bindAddress = Address::loopbackV4(0);
    cc.connection = ConnectionConfig::trunk();
    cc.socket = SocketTransportConfig::trunk();
    cc.socket.socket.sendBufferBytes = cfg.socketBufferBytes;
    cc.socket.socket.receiveBufferBytes = cfg.socketBufferBytes;
    auto clientR = Client::create(std::move(cc), monotonicSeconds());
    if (!clientR) return r;
    auto client = std::move(clientR).value();
    r.grantedReceiveBuffer = static_cast<SocketTransport&>(client->transport()).socket().receiveBufferBytes();

    ConnectTokenParams p;
    p.protocolId = kProtocol;
    p.clientId = 1;
    p.timeoutSeconds = 10;
    p.publicAddresses.push_back(server->publicAddresses()[0]);
    p.privateKey = key;
    auto token = generateConnectToken(p);
    if (!token || !client->connect(token.value(), monotonicSeconds())) return r;

    detail::CountingHandler cellEvents, gatewayEvents;
    const f64 connectDeadline = monotonicSeconds() + 3.0;
    while (monotonicSeconds() < connectDeadline && !(client->isConnected() && !cellEvents.sessions.empty())) {
        const f64 now = monotonicSeconds();
        server->update(now, cellEvents);
        client->update(now, gatewayEvents);
        server->flush(now);
        client->flush(now);
        sleepMillis(1);
    }
    if (!client->isConnected() || cellEvents.sessions.empty()) return r;
    r.connected = true;
    const SessionHandle session = cellEvents.sessions[0];

    std::atomic<bool> senderDone{false};
    std::atomic<bool> stopReceiver{false};
    f64 senderCpu = 0.0, receiverCpu = 0.0, senderWall = 0.0, receiverWall = 0.0;
    u64 sent = 0;

    std::thread receiver([&] {
        const f64 cpu0 = os::threadCpuSeconds();
        const f64 wall0 = monotonicSeconds();
        while (!stopReceiver.load(std::memory_order_acquire)) {
            const f64 now = monotonicSeconds();
            const u64 before = gatewayEvents.messages.load(std::memory_order_relaxed);
            client->update(now, gatewayEvents);
            client->flush(now); // acks
            if (gatewayEvents.messages.load(std::memory_order_relaxed) == before) sleepNanos(50'000);
        }
        receiverCpu = os::threadCpuSeconds() - cpu0;
        receiverWall = monotonicSeconds() - wall0;
    });

    std::thread sender([&] {
        const std::vector<u8> chunk(cfg.payloadBytes, 0xC5);
        const f64 cpu0 = os::threadCpuSeconds();
        const f64 start = monotonicSeconds();
        const f64 end = start + cfg.seconds;
        for (;;) {
            const f64 now = monotonicSeconds();
            if (now >= end) break;
            server->update(now, cellEvents);
            const u64 due = static_cast<u64>((now - start) * cfg.targetPps);
            bool any = false;
            while (sent < due) {
                if (server->sendState(session, chunk) != SendResult::Ok) break;
                ++sent;
                any = true;
            }
            if (any) server->flush(now);
            // ~20 datagrams per millisecond at 20k pps: pace in 250 us steps.
            sleepNanos(250'000);
        }
        // Let the last acks/notifications arrive.
        const f64 drainEnd = monotonicSeconds() + 0.3;
        while (monotonicSeconds() < drainEnd) {
            const f64 now = monotonicSeconds();
            server->update(now, cellEvents);
            server->flush(now);
            sleepNanos(500'000);
        }
        senderCpu = os::threadCpuSeconds() - cpu0;
        senderWall = monotonicSeconds() - start;
        senderDone.store(true, std::memory_order_release);
    });

    sender.join();
    stopReceiver.store(true, std::memory_order_release);
    receiver.join();

    r.sent = sent;
    r.delivered = gatewayEvents.messages.load();
    r.notifiedLost = cellEvents.lost.load();
    r.seconds = cfg.seconds;
    r.deliveredPps = static_cast<f64>(r.delivered) / cfg.seconds;
    r.payloadMbps = r.deliveredPps * cfg.payloadBytes * 8.0 / 1e6;
    if (const Connection* conn = server->connection(session)) {
        r.wireMbps = static_cast<f64>(conn->stats().wireBytesSent) * 8.0 / cfg.seconds / 1e6;
    }
    r.dropPercent = r.sent == 0 ? 100.0 : 100.0 * static_cast<f64>(r.sent - std::min(r.sent, r.delivered)) / static_cast<f64>(r.sent);
    r.senderCores = senderWall > 0 ? senderCpu / senderWall : 0.0;
    r.receiverCores = receiverWall > 0 ? receiverCpu / receiverWall : 0.0;
    r.senderSendSyscalls = static_cast<SocketTransport&>(server->transport()).socket().stats().sendSyscalls;
    r.receiverRecvSyscalls = static_cast<SocketTransport&>(client->transport()).socket().stats().receiveSyscalls;
    return r;
}

} // namespace helios::net::bench
