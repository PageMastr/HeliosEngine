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
//   net_bench --gate --advisory ns02-stack
//                                   the same gates, except that the encrypted stack's 100k per core is
//                                   printed with an "NS-0.2 advisory:" line instead of failing. Loss, a
//                                   stack that never ran, raw datagrams and NS-0.7 still fail. Only the
//                                   hosted Linux nightly passes it (the owner's approval of 2026-09-30,
//                                   docs/evidence/ns-0.2-owner-approval-2026-09-30.md); hosted Windows, a
//                                   local or a lab run gates everything. Exit code 2 for an unknown
//                                   --advisory name.
//
// The socket and stack runs also print a "cross-check" line: the whole machine's CPU per datagram or packet
// beside the measuring thread's, with the background load measured on either side of the run subtracted, and
// a warning when more than 25 % of the work ran outside the thread on an otherwise idle host
// (bench::machineCrossCheck). It reports what a per-thread figure cannot see; no gate reads it.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/net/net.h"
#include "ns02_gate.h"
#include "platform/net_os.h"
#include "trunk_gate.h"

using namespace helios;
using namespace helios::net;

namespace {

/// The machine's busy CPU around one measured run (bench::machineCrossCheck): sampled when the run starts and
/// ends, with the background load measured while the bench sleeps just before and just after it.
struct MachineWindow {
    os::MachineCpuTimes start;
    os::MachineCpuTimes end;
    f64 backgroundBefore = -1.0; ///< busy cores without the run; negative when unavailable
    f64 backgroundAfter = -1.0;
};

/// Busy cores of the whole machine over `millis` while this thread sleeps; negative when unavailable.
f64 backgroundCores(u32 millis) {
    const os::MachineCpuTimes a = os::machineCpuTimes();
    const f64 t0 = monotonicSeconds();
    sleepMillis(millis);
    const os::MachineCpuTimes b = os::machineCpuTimes();
    const f64 wall = monotonicSeconds() - t0;
    if (!a.ok || !b.ok || wall <= 0.0) return -1.0;
    return std::max(0.0, (b.busySeconds - a.busySeconds) / wall);
}

constexpr u32 kBackgroundMillis = 500;

struct SocketResult {
    u64 sent = 0;
    u64 received = 0;
    f64 cpuSeconds = 0.0;
    f64 wallSeconds = 0.0;
    MachineWindow machine;
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
    r.machine.backgroundBefore = backgroundCores(kBackgroundMillis);
    r.machine.start = os::machineCpuTimes();
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
    r.machine.end = os::machineCpuTimes();
    r.machine.backgroundAfter = backgroundCores(kBackgroundMillis);
    return r;
}

struct StackResult {
    u64 sent = 0;
    u64 packets = 0;
    f64 cpuSeconds = 0.0;
    f64 wallSeconds = 0.0;
    MachineWindow machine;
    f64 ppsPerCore() const { return cpuSeconds > 0 ? static_cast<f64>(packets) / cpuSeconds : 0.0; }
    f64 microsPerPacket() const { return packets > 0 ? cpuSeconds * 1e6 / static_cast<f64>(packets) : 0.0; }
};

/// The batch API the bench's sockets get (they all ask for Auto, as SocketTransport does by default).
std::string_view autoBatchApi() {
    UdpSocketConfig c;
    c.bindAddress = Address::loopbackV4(0);
    auto s = UdpSocket::open(c);
    return s ? udpBatchApiName(s.value().batchApi()) : std::string_view("unavailable");
}

/// Encrypted HTP packets (netcode + reliable + channels) per core: server and client in one
/// thread over UDP loopback, 700-byte EVENT_U messages, one per packet (≈ 730-byte datagrams).
StackResult runStackPps(f64 seconds) {
    StackResult r;
    // Measured before the endpoints exist: an idle session between the handshake and the run would change
    // what the run measures.
    r.machine.backgroundBefore = backgroundCores(kBackgroundMillis);
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
    r.machine.start = os::machineCpuTimes();
    const f64 wall0 = monotonicSeconds();
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
    r.wallSeconds = monotonicSeconds() - wall0;
    r.machine.end = os::machineCpuTimes();
    r.packets = ce.messages;
    r.machine.backgroundAfter = backgroundCores(kBackgroundMillis);
    return r;
}

/// Prints the machine cross-check of one run (bench::machineCrossCheck) after its result line, and warns
/// when it flags the run. It reports; it never changes a gate's verdict.
void reportCrossCheck(std::string_view what, std::string_view unit, u64 units, f64 threadCpuSeconds,
                      f64 wallSeconds, const MachineWindow& m) {
    if (!m.start.ok || !m.end.ok || m.backgroundBefore < 0.0 || m.backgroundAfter < 0.0 || units == 0) {
        HELIOS_LOG_INFO("{} cross-check: no machine-wide CPU counter on this OS", what);
        return;
    }
    // The larger of the two background samples is subtracted, so a load that changed during the run is not
    // mistaken for the run's own work.
    const bench::MachineCrossCheck c =
        bench::machineCrossCheck(threadCpuSeconds, m.end.busySeconds - m.start.busySeconds, wallSeconds,
                                 std::max(m.backgroundBefore, m.backgroundAfter), m.end.cpus);
    const f64 perUnit = 1e6 / static_cast<f64>(units);
    const std::string_view idle =
        c.valid ? "" : "; not meaningful: the host is not idle (background above a quarter of the CPUs)";
    HELIOS_LOG_INFO("{} cross-check: the machine spent {:.2f} us of CPU per {} against the thread's {:.2f} us "
                    "({:+.1f} % outside the thread; all {} CPUs, background {:.2f} cores before and {:.2f} "
                    "after subtracted){}",
                    what, c.machineCpuSeconds * perUnit, unit, threadCpuSeconds * perUnit, c.offThreadPercent,
                    m.end.cpus, m.backgroundBefore, m.backgroundAfter, idle);
    if (c.flagged) {
        HELIOS_LOG_WARN("{} cross-check: {:.0f} % more CPU per {} ran outside the measuring thread than in it "
                        "(limit {:.0f} %), so its per-core rate overstates what one core does; the gate still "
                        "uses the thread's figure",
                        what, c.offThreadPercent, unit, bench::kOffThreadLimitPercent);
    }
}

void report(const bench::TrunkGateResult& r, f64 seconds) {
    // Three decimals for the core counts: the nightly perf history (scorecard.jsonc) tracks the cell
    // thread's, and at about 0.24 cores one step of a second decimal is already a 4 % change.
    HELIOS_LOG_INFO("NS-0.7 trunk: {:.0f} s, sent {}, delivered {} ({:.0f} pps, {:.1f} Mbit/s payload, {:.1f} Mbit/s "
                    "wire), drops {:.4f} %, cell thread {:.3f} cores, gateway thread {:.3f} cores, rcvbuf {} KB, "
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
    // `--advisory ns02-stack` is the only accepted form: anything else is an error rather than a run that
    // silently gates (or does not gate) something other than what the caller meant.
    bool stackAdvisory = false;
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i] != "--advisory") continue;
        if (!gate || i + 1 >= args.size() || args[i + 1] != bench::kNs02StackAdvisoryName) {
            HELIOS_LOG_ERROR("--advisory takes '{}' and applies to --gate only",
                             bench::kNs02StackAdvisoryName);
            return 2;
        }
        stackAdvisory = true;
    }
    bool ok = true;

    if (all || gate || has("--socket")) {
        const u64 count = static_cast<u64>(number("--socket", 0, 1'000'000));
        const SocketResult s = runSocketPps(count);
        HELIOS_LOG_INFO("NS-0.2 socket: {}/{} datagrams, {:.0f} pps per core ({:.1f} ms CPU, {:.1f} ms wall, batch API "
                        "{})",
                        s.received, s.sent, s.ppsPerCore(), s.cpuSeconds * 1e3, s.wallSeconds * 1e3, autoBatchApi());
        reportCrossCheck("NS-0.2 socket", "datagram", s.received, s.cpuSeconds, s.wallSeconds, s.machine);
        if (gate && (s.received != s.sent || s.ppsPerCore() < bench::kNs02PerCore)) {
            HELIOS_LOG_ERROR("NS-0.2 FAILED: needs 100k pps per core without loss");
            ok = false;
        }
    }
    if (all || gate || has("--stack")) {
        const StackResult s = runStackPps(number("--stack", 0, gate ? 10.0 : 2.0));
        // CPU well below wall means the thread was preempted. Wall time cannot show work the OS does for the
        // thread elsewhere (the loop never sleeps or blocks, so the thread accrues CPU at the wall rate either
        // way); the machine-wide cross-check that follows does.
        HELIOS_LOG_INFO("NS-0.2 HTP stack: {}/{} encrypted packets delivered, {:.0f} packets per core (send + receive, "
                        "1 thread): {:.2f} us of CPU per packet against a budget of {:.0f} ({:.2f} s CPU, {:.2f} s wall, "
                        "batch API {})",
                        s.packets, s.sent, s.ppsPerCore(), s.microsPerPacket(), bench::kNs02BudgetMicrosPerPacket,
                        s.cpuSeconds, s.wallSeconds, autoBatchApi());
        reportCrossCheck("NS-0.2 HTP stack", "packet", s.packets, s.cpuSeconds, s.wallSeconds, s.machine);
        const bench::Ns02Verdict verdict =
            bench::ns02StackVerdict(s.sent, s.packets, s.ppsPerCore(), stackAdvisory);
        if (gate && verdict == bench::Ns02Verdict::Fail) {
            HELIOS_LOG_ERROR("NS-0.2 FAILED: the HTP stack needs 100k encrypted packets per core without loss");
            ok = false;
        }
        // Printed on every advisory run whose stack did not fail, so that a passing log still says the rate
        // was not gated; a failed stack gets only the failure line above.
        const bench::Ns02AdvisoryNote note = bench::ns02AdvisoryNote(verdict, gate && stackAdvisory);
        if (note == bench::Ns02AdvisoryNote::Below) {
            HELIOS_LOG_WARN("NS-0.2 advisory: {}. The HTP stack's {:.0f} packets per core is below 100k: "
                            "reported, not failing (loss still fails)",
                            bench::kNs02StackAdvisoryReason, s.ppsPerCore());
        } else if (note == bench::Ns02AdvisoryNote::NotGated) {
            HELIOS_LOG_INFO("NS-0.2 advisory: {}. The HTP stack's rate ({:.0f} packets per core) is not "
                            "gated on this run",
                            bench::kNs02StackAdvisoryReason, s.ppsPerCore());
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
    if (gate) {
        HELIOS_LOG_INFO("gates {}{}", ok ? "PASSED" : "FAILED",
                        stackAdvisory ? " (NS-0.2's HTP stack rate advisory)" : "");
    }
    return ok ? 0 : 1;
}
