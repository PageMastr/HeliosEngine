// net_bench: HTP performance gates.
//
//   net_bench                       quick run of everything (seconds)
//   net_bench --socket [count]      NS-0.2: raw loopback datagrams per core (send + receive, 1 thread)
//   net_bench --stack [seconds]     encrypted HTP packets per core through the full stack (1 thread)
//   net_bench --trunk [s] [pps]     NS-0.7: trunk throughput, cell and gateway on their own threads
//   net_bench --gate                the Phase 0 gates: NS-0.2 >= 100k pps/core without loss, both for raw
//                                   datagrams and for encrypted HTP packets through the full stack, and
//                                   NS-0.7 20k pps of 1,200 B datagrams for 600 s, < 0.1 % drops,
//                                   <= 1 core per side. Exit code 1 if any gate fails (nightly CI,
//                                   evidence N).

#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/net/net.h"
#include "platform/net_os.h"
#include "trunk_gate.h"

using namespace helios;
using namespace helios::net;

namespace {

struct SocketResult {
    u64 sent = 0;
    u64 received = 0;
    f64 cpuSeconds = 0.0;
    f64 wallSeconds = 0.0;
    f64 ppsPerCore() const { return cpuSeconds > 0 ? static_cast<f64>(received) / cpuSeconds : 0.0; }
};

SocketResult runSocketPps(u64 total) {
    SocketResult r;
    UdpSocketConfig c;
    c.bindAddress = Address::loopbackV4(0);
    auto tx = UdpSocket::open(c).value();
    auto rx = UdpSocket::open(c).value();
    constexpr usize kBatch = 64;
    std::vector<u8> payload(100, 0x5A);
    std::vector<OutDatagram> out(kBatch, OutDatagram{rx.localAddress(), payload});
    std::vector<u8> arena(kBatch * 2048);
    std::vector<InDatagram> slots(kBatch);
    const auto refill = [&] {
        for (usize i = 0; i < kBatch; ++i) slots[i].buffer = std::span<u8>(arena.data() + i * 2048, 2048);
    };
    const f64 cpu0 = os::threadCpuSeconds();
    const f64 t0 = monotonicSeconds();
    while (r.sent < total) {
        r.sent += tx.sendBatch(out);
        for (;;) {
            refill();
            const usize n = rx.receiveBatch(slots);
            r.received += n;
            if (n < kBatch) break;
        }
    }
    const f64 end = monotonicSeconds() + 2.0;
    while (r.received < r.sent && monotonicSeconds() < end) {
        refill();
        r.received += rx.receiveBatch(slots);
    }
    r.cpuSeconds = os::threadCpuSeconds() - cpu0;
    r.wallSeconds = monotonicSeconds() - t0;
    return r;
}

struct StackResult {
    u64 sent = 0;
    u64 packets = 0;
    f64 cpuSeconds = 0.0;
    f64 ppsPerCore() const { return cpuSeconds > 0 ? static_cast<f64>(packets) / cpuSeconds : 0.0; }
};

/// Encrypted HTP packets (netcode + reliable + channels) per core: server and client in one
/// thread over UDP loopback, 100-byte EVENT_U messages, one per packet.
StackResult runStackPps(f64 seconds) {
    StackResult r;
    struct Counter final : IEndpointHandler {
        u64 messages = 0;
        std::vector<SessionHandle> sessions;
        void onConnected(SessionHandle s) override { sessions.push_back(s); }
        void onMessage(SessionHandle, Channel, std::span<const u8>) override { ++messages; }
    } se, ce;
    const Key key = generateKey();
    ServerConfig sc;
    sc.protocolId = 1;
    sc.privateKey = key;
    sc.bindAddress = Address::loopbackV4(0);
    sc.connection = ConnectionConfig::trunk();
    auto server = Server::create(std::move(sc), monotonicSeconds()).value();
    ClientConfig cc;
    cc.bindAddress = Address::loopbackV4(0);
    cc.connection = ConnectionConfig::trunk();
    auto client = Client::create(std::move(cc), monotonicSeconds()).value();
    ConnectTokenParams p;
    p.protocolId = 1;
    p.clientId = 1;
    p.publicAddresses.push_back(server->publicAddresses()[0]);
    p.privateKey = key;
    (void)client->connect(generateConnectToken(p).value(), monotonicSeconds());
    const f64 deadline = monotonicSeconds() + 3.0;
    while (monotonicSeconds() < deadline && !(client->isConnected() && !se.sessions.empty())) {
        const f64 now = monotonicSeconds();
        server->update(now, se);
        client->update(now, ce);
        server->flush(now);
        client->flush(now);
        sleepMillis(1);
    }
    if (se.sessions.empty()) return r;
    const std::vector<u8> msg(700, 1); // one message per packet
    const f64 cpu0 = os::threadCpuSeconds();
    const f64 end = monotonicSeconds() + seconds;
    while (monotonicSeconds() < end) {
        const f64 now = monotonicSeconds();
        // 64 packets per round: EVENT_U messages sized so that each fills its own packet.
        for (int i = 0; i < 64; ++i) {
            if (server->send(se.sessions[0], Channel::EventUnreliable, msg) == SendResult::Ok) ++r.sent;
        }
        server->flush(now);
        client->update(now, ce);
        client->flush(now);
        server->update(now, se);
    }
    // Collect stragglers so "without loss" compares everything that was sent.
    const f64 drainEnd = monotonicSeconds() + 0.5;
    while (ce.messages < r.sent && monotonicSeconds() < drainEnd) {
        const f64 now = monotonicSeconds();
        server->flush(now);
        client->update(now, ce);
        client->flush(now);
        server->update(now, se);
    }
    r.cpuSeconds = os::threadCpuSeconds() - cpu0;
    r.packets = ce.messages;
    return r;
}

void report(const bench::TrunkGateResult& r, f64 seconds) {
    HELIOS_LOG_INFO("NS-0.7 trunk: {:.0f} s, sent {}, delivered {} ({:.0f} pps, {:.1f} Mbit/s payload, {:.1f} Mbit/s "
                    "wire), drops {:.4f} %, cell thread {:.2f} cores, gateway thread {:.2f} cores, rcvbuf {} KB, "
                    "syscalls send {} recv {}",
                    seconds, r.sent, r.delivered, r.deliveredPps, r.payloadMbps, r.wireMbps, r.dropPercent,
                    r.senderCores, r.receiverCores, r.grantedReceiveBuffer / 1024, r.senderSendSyscalls,
                    r.receiverRecvSyscalls);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const auto has = [&](const char* flag) {
        for (const auto& a : args)
            if (a == flag) return true;
        return false;
    };
    const auto number = [&](const char* flag, usize index, f64 fallback) {
        for (usize i = 0; i < args.size(); ++i) {
            if (args[i] == flag && i + 1 + index < args.size() && args[i + 1 + index].rfind("--", 0) != 0) {
                return std::stod(args[i + 1 + index]);
            }
        }
        return fallback;
    };
    const bool gate = has("--gate");
    const bool all = args.empty();
    bool ok = true;

    if (all || gate || has("--socket")) {
        const u64 count = static_cast<u64>(number("--socket", 0, 1'000'000));
        const SocketResult s = runSocketPps(count);
        HELIOS_LOG_INFO("NS-0.2 socket: {}/{} datagrams, {:.0f} pps per core ({:.1f} ms CPU, {:.1f} ms wall)", s.received,
                        s.sent, s.ppsPerCore(), s.cpuSeconds * 1e3, s.wallSeconds * 1e3);
        if (gate && (s.received != s.sent || s.ppsPerCore() < 100'000.0)) {
            HELIOS_LOG_ERROR("NS-0.2 FAILED: needs 100k pps per core without loss");
            ok = false;
        }
    }
    if (all || gate || has("--stack")) {
        const StackResult s = runStackPps(number("--stack", 0, gate ? 10.0 : 2.0));
        HELIOS_LOG_INFO("NS-0.2 HTP stack: {}/{} encrypted packets delivered, {:.0f} packets per core (send + receive, "
                        "1 thread)",
                        s.packets, s.sent, s.ppsPerCore());
        if (gate && (s.sent == 0 || s.packets != s.sent || s.ppsPerCore() < 100'000.0)) {
            HELIOS_LOG_ERROR("NS-0.2 FAILED: the HTP stack needs 100k encrypted packets per core without loss");
            ok = false;
        }
    }
    if (all || gate || has("--trunk")) {
        bench::TrunkGateConfig cfg;
        cfg.seconds = gate ? 600.0 : number("--trunk", 0, 5.0);
        cfg.targetPps = static_cast<u32>(number("--trunk", 1, 20'000));
        const bench::TrunkGateResult r = bench::runTrunkGate(cfg);
        if (!r.connected) {
            HELIOS_LOG_ERROR("NS-0.7: trunk did not connect");
            ok = false;
        } else {
            report(r, cfg.seconds);
            if (gate && (r.deliveredPps < 0.999 * cfg.targetPps || r.dropPercent >= 0.1 || r.senderCores > 1.0 ||
                         r.receiverCores > 1.0)) {
                HELIOS_LOG_ERROR("NS-0.7 FAILED: needs 20k pps, < 0.1 % drops, <= 1 core per side");
                ok = false;
            }
        }
    }
    if (gate) HELIOS_LOG_INFO("gates {}", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
