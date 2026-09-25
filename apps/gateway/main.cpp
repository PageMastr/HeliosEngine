// helios-gateway: the public game edge (04 §1, §2.3–2.6, §9). Accepts clients with Session
// service connect tokens on UDP 7777, routes each session to the cell that owns its zone
// (ResolveZone) over HTP trunks, forwards both ways, evicts superseded sessions and hands out
// reconnect tickets. `helios-gateway probe` is a tiny client for smoke tests and the NS-0.3
// Windows <-> Linux interop check. See apps/gateway/README.md.

#include <atomic>
#include <cstdio>
#include <format>
#include <string>

#include "helios/core/cmdline.h"
#include "helios/core/crash.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/core/version.h"
#include "helios/server/app_env.h"
#include "helios/server/gateway_server.h"
#include "helios/server/probe_client.h"

using namespace helios;
using namespace helios::server;

namespace {
HELIOS_LOG_CHANNEL(LogMain, "helios-gateway");

constexpr std::string_view kUsage = R"(helios-gateway — Helios gateway (public game edge)

  helios-gateway [options]
  helios-gateway probe --connect <ip:port> [probe options]

Clients
  --listen <ip:port>        UDP bind address (default 127.0.0.1:7777; the only public game port)
  --public <ip:port>        address clients use, as listed in tokens (default: the bound one)
  --max-clients <n>         netcode slots (default 256)
  --keys <file>             shard netcode keyring (env HELIOS_NETCODE_KEYS, else
                            <backend-data>/keys/netcode-shard.json); its newest id is the keyId
  --protocol-id <n>         netcode protocol id, read like the backend reads session.protocol_id
                            (0x<hex> or decimal; env HELIOS_PROTOCOL_ID, default 0x48454c494f530001)
  --token-lifetime <s>      = the Session service's token expiry (env HELIOS_TOKEN_LIFETIME, default 45)

Cells
  --trunk-keys <file>       trunk keyring (env HELIOS_TRUNK_KEYS; falls back to the shard keyring)
  --standalone              no NATS: route with --cell / --route
  --cell <ip:port>          standalone: the cell for every zone (default 127.0.0.1:7810)
  --route <zoneId@ip:port>  standalone: a zone's cell (repeatable)
  --default-zone <name>     zone for tokens without one (default tallis)

Control plane
  --nats <url>              NATS URL (env HELIOS_NATS_URL); user/password from HELIOS_NATS_USER /
                            HELIOS_NATS_PASSWORD or <backend-data>/keys/nats-fleet.json
  --backend-data <dir>      helios-backend's data directory (default ./saved/backend)
  --shard <name>            NATS subject scope (env HELIOS_SHARD, default dev)
  --name <name>             process name (default gw-1)
  --ticket-interval <s>     SealReconnectTickets period (default 60)

Other
  --dev-insecure-keys       fixed public keys for loopback-only development without the backend
  --crash-dir <dir>, --log-level <level>, --version, --help

Probe (a minimal client: connect, Welcome, echo round trips, tick states)
  --connect <ip:port>       gateway address (default 127.0.0.1:7777)
  --zone <id>               zone id in the token (default 0 = the gateway's default zone)
  --session <id>            session id (default random)
  --echoes <n>              echo requests (default 10)
  --seconds <s>             how long to run (default 5)
  --token <base64>          use a Session service token (ConnectInfo.connectToken) instead of minting
                            one; otherwise the probe mints its own with --keys or --dev-insecure-keys
)";

int fail(int code, const std::string& message) {
    HELIOS_LOG_ERROR(LogMain, "{}", message);
    return code;
}

u64 parseU64(const std::string& s, u64 fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    return (end && *end == '\0') ? static_cast<u64>(v) : fallback;
}

int runProbeWith(const ProcessArgs& args, const net::ConnectTokenBytes& token, const net::Address& target, u64 sessionId,
                 u64 zoneId);

int runProbe(const ProcessArgs& args) {
    auto target = parseAddress(args.get("connect", "", "127.0.0.1:7777"), "--connect");
    if (!target) return fail(2, target.error().message);
    net::ConnectTokenBytes tokenBytes{};
    DevTokenParams t;
    if (auto b64 = args.option("token")) {
        // A token from the Session service (ConnectInfo.connectToken, base64): the real login path.
        auto raw = base64Decode(*b64);
        if (!raw || raw->size() != net::kConnectTokenBytes) return fail(2, "--token: not a base64 2048-byte connect token");
        std::copy(raw->begin(), raw->end(), tokenBytes.begin());
        auto info = net::parseConnectToken(tokenBytes);
        if (!info) return fail(2, "--token: " + info.error().message);
        if (!info->serverAddresses.empty()) target = info->serverAddresses[0];
        return runProbeWith(args, tokenBytes, *target, 0, 0);
    }
    auto key = loadKey(args, "keys", "HELIOS_NETCODE_KEYS", "netcode-shard.json", "helios-dev-shard", target->isLoopback());
    if (!key) return fail(2, "shard key: " + key.error().message);
    auto protocol = parseProtocolId(args.get("protocol-id", "HELIOS_PROTOCOL_ID", "0x48454c494f530001"));
    if (!protocol) return fail(2, "bad --protocol-id");

    t.protocolId = *protocol;
    t.key = key->key;
    const net::Key rnd = net::generateKey();
    u64 randomId = 0;
    for (int i = 0; i < 8; ++i) randomId = (randomId << 8) | rnd[static_cast<usize>(i)];
    t.sessionId = parseU64(args.get("session", ""), (randomId & ~(1ull << 63)) | 1);
    t.gateways = {*target};
    t.user.sessionEpoch = 1;
    t.user.zoneId = parseU64(args.get("zone", ""), 0);
    t.user.accountId = t.sessionId;
    auto token = mintDevToken(t);
    if (!token) return fail(1, "cannot mint a token: " + token.error().message);
    return runProbeWith(args, *token, *target, t.sessionId, t.user.zoneId);
}

int runProbeWith(const ProcessArgs& args, const net::ConnectTokenBytes& token, const net::Address& target, u64 sessionId,
                 u64 zoneId) {
    i64 now = static_cast<i64>(monotonicNanos());
    ProbeConfig pc;
    if (target.isLoopback()) pc.bind = target.isIpv6() ? net::Address::loopbackV6() : net::Address::loopbackV4();
    auto probe = ProbeClient::create(std::move(pc), now);
    if (!probe) return fail(1, probe.error().message);
    ProbeClient& p = **probe;
    if (auto r = p.connect(token, now); !r) return fail(1, r.error().message);
    const u64 echoes = parseU64(args.get("echoes", ""), 10);
    const f64 seconds = static_cast<f64>(parseU64(args.get("seconds", ""), 5));
    std::printf("probe: session %llu -> %s (zone %llu)\n", static_cast<unsigned long long>(sessionId), target.toString().c_str(),
                static_cast<unsigned long long>(zoneId));
    const u64 start = monotonicNanos();
    u64 sent = 0;
    i64 nextEcho = 0;
    while (static_cast<f64>(monotonicNanos() - start) * 1e-9 < seconds) {
        now = static_cast<i64>(monotonicNanos());
        p.update(now);
        if (p.stats().kick || p.stats().disconnected) break;
        if (p.isWelcomed() && sent < echoes && now >= nextEcho) {
            const std::string text = "echo " + std::to_string(sent);
            p.sendEcho(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), now);
            ++sent;
            nextEcho = now + 100'000'000;
        }
        sleepMillis(1);
    }
    const ProbeStats& s = p.stats();
    if (s.welcome)
        std::printf("welcome: zone %llu '%s' at %u Hz, tick %llu, dilation %.2f\n", static_cast<unsigned long long>(s.welcome->zoneId),
                    s.welcome->zoneName.c_str(), s.welcome->tickHz, static_cast<unsigned long long>(s.welcome->tick),
                    s.welcome->dilationPpm / 1e6);
    else
        std::printf("no welcome (client state: %s)\n", std::string(net::clientStateName(p.client().state())).c_str());
    std::printf("echoes: %llu/%llu, rtt avg %.1f ms max %.1f ms; tick states: %llu (last tick %llu); tickets: %zu\n",
                static_cast<unsigned long long>(s.echoesReceived), static_cast<unsigned long long>(sent),
                s.echoesReceived ? s.totalRttMs / static_cast<f64>(s.echoesReceived) : 0.0, s.maxRttMs,
                static_cast<unsigned long long>(s.tickStates), static_cast<unsigned long long>(s.lastTick), s.tickets.size());
    if (s.kick) std::printf("kicked: %s %s\n", std::string(proto::kickReasonName(s.kick->reason)).c_str(), s.kick->message.c_str());
    p.disconnect();
    p.update(static_cast<i64>(monotonicNanos()));
    return s.welcome && s.echoesReceived == sent ? 0 : 1;
}

int run(const ProcessArgs& args) {
    if (args.flag("help") || args.flag("h")) {
        std::fputs(kUsage.data(), stdout);
        return 0;
    }
    if (args.flag("version")) {
        std::printf("helios-gateway %s\n", version::kString);
        return 0;
    }
    if (auto lvl = args.option("log-level"))
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    const auto positional = args.positional();
    if (!positional.empty() && positional[0] == "probe") return runProbe(args);
    if (!positional.empty()) return fail(2, "unknown command '" + positional[0] + "' (see --help)");
    if (auto r = installCrashHandler(fs::pathFromUtf8(args.get("crash-dir", "", "saved/crashes")), {.appName = "helios-gateway"}); !r)
        HELIOS_LOG_WARN(LogMain, "crash handler not installed: {}", r.error().message);

    GatewayConfig config;
    config.name = args.get("name", "HELIOS_GATEWAY_NAME", "gw-1");
    config.version = version::kString;
    config.shard = args.get("shard", "HELIOS_SHARD", "dev");
    auto listen = parseAddress(args.get("listen", "", "127.0.0.1:7777"), "--listen");
    if (!listen) return fail(2, listen.error().message);
    config.listen = *listen;
    for (const std::string& pub : args.options("public")) {
        auto a = parseAddress(pub, "--public");
        if (!a) return fail(2, a.error().message);
        config.publicAddresses.push_back(*a);
    }
    config.maxClients = static_cast<u32>(parseU64(args.get("max-clients", ""), 256));
    config.tokenLifetimeSeconds = static_cast<i32>(parseU64(args.get("token-lifetime", "HELIOS_TOKEN_LIFETIME"), 45));
    config.defaultZone = args.get("default-zone", "", "tallis");
    config.ticketIntervalMs = static_cast<i64>(parseU64(args.get("ticket-interval", ""), 60)) * 1000;
    auto protocol = parseProtocolId(args.get("protocol-id", "HELIOS_PROTOCOL_ID", "0x48454c494f530001"));
    if (!protocol) return fail(2, "bad --protocol-id / HELIOS_PROTOCOL_ID");
    config.protocolId = *protocol;

    bool loopback = config.listen.isLoopback();
    for (const net::Address& a : config.publicAddresses) loopback = loopback && a.isLoopback();
    auto shard = loadKey(args, "keys", "HELIOS_NETCODE_KEYS", "netcode-shard.json", "helios-dev-shard", loopback);
    if (!shard) return fail(2, "shard key: " + shard.error().message);
    config.shardKey = shard->key;
    config.keyId = shard->id;
    HELIOS_LOG_INFO(LogMain, "shard key generation {} from {}", shard->id, shard->source);
    auto trunk = loadKey(args, "trunk-keys", "HELIOS_TRUNK_KEYS", "", "helios-dev-trunk", loopback);
    if (!trunk && !args.flag("dev-insecure-keys")) trunk = shard; // Phase 0: the shard keyring doubles as the trunk key
    if (!trunk) return fail(2, "trunk key: " + trunk.error().message);
    config.trunkKey = trunk->key;

    std::unique_ptr<NatsBus> bus;
    if (!args.flag("standalone")) {
        auto b = connectBusFromArgs(args, config.name);
        if (!b) return fail(2, "NATS: " + b.error().message);
        bus = std::move(*b);
    }
    if (bus) {
        config.bus = bus.get();
    } else {
        auto cell = parseAddress(args.get("cell", "", "127.0.0.1:7810"), "--cell");
        if (!cell) return fail(2, cell.error().message);
        for (const std::string& r : args.options("route")) {
            const usize at = r.find('@');
            auto addr = at == std::string::npos ? Result<net::Address>(Error{ErrorCode::InvalidArgument, "want zoneId@ip:port"})
                                                : parseAddress(r.substr(at + 1), "--route");
            if (!addr) return fail(2, "--route " + r + ": " + addr.error().message);
            config.staticRoutes.push_back(StaticRoute{parseU64(r.substr(0, at), 0), *addr});
        }
        config.staticRoutes.push_back(StaticRoute{0, *cell});
        HELIOS_LOG_INFO(LogMain, "standalone: no NATS URL (--nats / HELIOS_NATS_URL); routing to cell {}", cell->toString());
    }

    auto gw = GatewayServer::create(std::move(config), static_cast<i64>(monotonicNanos()));
    if (!gw) return fail(1, "cannot start: " + gw.error().message);
    std::atomic<bool> stop{false};
    installStopSignal(stop);
    HELIOS_LOG_INFO(LogMain, "running; Ctrl+C to stop");
    (*gw)->run(stop);
    HELIOS_LOG_INFO(LogMain, "stopped");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return run(ProcessArgs(CommandLine::fromProcess()));
}
