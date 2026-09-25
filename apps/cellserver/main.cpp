// helios-cell: the headless simulation server (04 §1, §3). Hosts zone instances on dilatable
// clocks, accepts HTP trunks from gateways, and — with a NATS URL — registers with the
// orchestrator, heartbeats and hosts exactly the zones it is assigned (holder rule, 05 §1.4.2).
// See apps/cellserver/README.md.

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
#include "helios/server/cell_server.h"

using namespace helios;
using namespace helios::server;

namespace {
HELIOS_LOG_CHANNEL(LogMain, "helios-cell");

constexpr std::string_view kUsage = R"(helios-cell — Helios cell server (headless simulation)

  helios-cell [options]

Zones and mode
  --zone <name[:id]>        a zone to serve (repeatable). Orchestrated: the names declared to the
                            orchestrator (none = any zone). Standalone: the zones hosted, with ids
                            (default tallis:1002).
  --tick-hz <n>             default zone tick rate, 1..60 (default 20)
  --zone-hz <name:hz>       per-zone tick rate (repeatable)
  --standalone              ignore NATS; host the --zone list at lease generation 0

Trunk (gateways connect here)
  --trunk <ip:port>         bind address (default 127.0.0.1:7810)
  --trunk-public <ip:port>  address gateways use (default: the bound one)
  --trunk-keys <file>       keyring with the trunk key (env HELIOS_TRUNK_KEYS; falls back to the
                            shard keyring HELIOS_NETCODE_KEYS / <backend-data>/keys/netcode-shard.json)
  --dev-insecure-keys       fixed public keys for loopback-only development without the backend

Control plane
  --nats <url>              NATS URL (env HELIOS_NATS_URL); user/password from HELIOS_NATS_USER /
                            HELIOS_NATS_PASSWORD or <backend-data>/keys/nats-fleet.json
  --backend-data <dir>      helios-backend's data directory (default ./saved/backend)
  --shard <name>            NATS subject scope (env HELIOS_SHARD, default dev)
  --name <name>             process name (default cell-1)

Other
  --workers <n>             job workers (default: cores - 2)
  --crash-dir <dir>         crash reports (default ./saved/crashes)
  --log-level <level>       trace|debug|info|warn|error (default info)
  --version, --help
)";

int fail(int code, const std::string& message) {
    HELIOS_LOG_ERROR(LogMain, "{}", message);
    return code;
}

Result<StaticZone> parseZone(std::string_view text, u64 fallbackId) {
    StaticZone z;
    const usize colon = text.rfind(':');
    z.name = std::string(text.substr(0, colon));
    z.id = fallbackId;
    if (colon != std::string_view::npos) {
        const std::string idText(text.substr(colon + 1));
        char* end = nullptr;
        z.id = std::strtoull(idText.c_str(), &end, 10);
        if (!end || *end != '\0' || z.id == 0) return makeError(ErrorCode::InvalidArgument, "bad zone id in '{}'", text);
    }
    if (z.name.empty()) return makeError(ErrorCode::InvalidArgument, "bad zone '{}'", text);
    return z;
}

int run(const ProcessArgs& args) {
    if (args.flag("help") || args.flag("h")) {
        std::fputs(kUsage.data(), stdout);
        return 0;
    }
    if (args.flag("version")) {
        std::printf("helios-cell %s\n", std::string(version::kString).c_str());
        return 0;
    }
    if (auto lvl = args.option("log-level"))
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    if (auto r = installCrashHandler(fs::pathFromUtf8(args.get("crash-dir", "", "saved/crashes")), {.appName = "helios-cell"}); !r)
        HELIOS_LOG_WARN(LogMain, "crash handler not installed: {}", r.error().message);

    CellServerConfig config;
    config.name = args.get("name", "HELIOS_CELL_NAME", "cell-1");
    config.version = std::string(version::kString);
    config.shard = args.get("shard", "HELIOS_SHARD", "dev");
    auto bind = parseAddress(args.get("trunk", "", "127.0.0.1:7810"), "--trunk");
    if (!bind) return fail(2, std::format("{}", bind.error().message));
    config.trunkBind = *bind;
    if (auto pub = args.option("trunk-public")) {
        auto a = parseAddress(*pub, "--trunk-public");
        if (!a) return fail(2, std::format("{}", a.error().message));
        config.trunkPublic = *a;
    }
    config.defaultTickHz = static_cast<u32>(std::strtoul(args.get("tick-hz", "", "20").c_str(), nullptr, 10));
    for (const std::string& zh : args.options("zone-hz")) {
        const usize c = zh.rfind(':');
        if (c == std::string::npos) return fail(2, std::format("--zone-hz wants name:hz, got '{}'", zh));
        config.zoneTickHz[zh.substr(0, c)] = static_cast<u32>(std::strtoul(zh.substr(c + 1).c_str(), nullptr, 10));
    }
    if (auto w = args.option("workers")) config.jobWorkers = static_cast<u32>(std::strtoul(w->c_str(), nullptr, 10));

    const bool loopback = config.trunkBind.isLoopback() && (!config.trunkPublic.isValid() || config.trunkPublic.isLoopback());
    auto trunkKey = loadKey(args, "trunk-keys", "HELIOS_TRUNK_KEYS", "", "helios-dev-trunk", loopback);
    if (!trunkKey && !args.flag("dev-insecure-keys")) // Phase 0: the shard keyring doubles as the trunk key
        trunkKey = loadKey(args, "keys", "HELIOS_NETCODE_KEYS", "netcode-shard.json", "helios-dev-trunk", loopback);
    if (!trunkKey) return fail(2, std::format("trunk key: {}", trunkKey.error().message));
    config.trunkKey = trunkKey->key;
    HELIOS_LOG_INFO(LogMain, "trunk key from {}", trunkKey->source);

    std::unique_ptr<NatsBus> bus;
    if (!args.flag("standalone")) {
        auto b = connectBusFromArgs(args, config.name);
        if (!b) return fail(2, std::format("NATS: {}", b.error().message));
        bus = std::move(*b);
    }
    u64 nextId = 1002;
    for (const std::string& zt : args.options("zone")) {
        auto z = parseZone(zt, nextId++);
        if (!z) return fail(2, std::format("{}", z.error().message));
        config.declaredZones.push_back(z->name);
        config.staticZones.push_back(*z);
    }
    if (bus) {
        config.bus = bus.get();
        config.staticZones.clear();
    } else {
        if (config.staticZones.empty()) config.staticZones.push_back(StaticZone{1002, "tallis", 0});
        HELIOS_LOG_INFO(LogMain, "standalone: no NATS URL (--nats / HELIOS_NATS_URL); hosting {} zone(s)", config.staticZones.size());
    }

    auto cell = CellServer::create(std::move(config), static_cast<i64>(monotonicNanos()));
    if (!cell) return fail(1, std::format("cannot start: {}", cell.error().message));
    std::atomic<bool> stop{false};
    installStopSignal(stop);
    HELIOS_LOG_INFO(LogMain, "running; Ctrl+C to stop");
    (*cell)->run(stop);
    HELIOS_LOG_INFO(LogMain, "stopped");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return run(ProcessArgs(CommandLine::fromProcess()));
}
