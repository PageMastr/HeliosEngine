#include "helios/server/app_env.h"

#include <csignal>
#include <cstdlib>
#include <system_error>

#include "helios/core/fs.h"
#include "server_log.h"

namespace helios::server {

namespace {
bool keyEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char x = static_cast<char>((a[i] >= 'A' && a[i] <= 'Z') ? a[i] - 'A' + 'a' : a[i]);
        const char y = static_cast<char>((b[i] >= 'A' && b[i] <= 'Z') ? b[i] - 'A' + 'a' : b[i]);
        if (x != y) return false;
    }
    return true;
}

/// Splits "--key=value" / "-key" into key and optional value; empty key when not an option.
std::pair<std::string_view, std::optional<std::string_view>> splitOption(std::string_view arg) {
    if (arg.size() < 2 || arg[0] != '-' || arg == "--") return {{}, std::nullopt};
    if (arg[1] >= '0' && arg[1] <= '9') return {{}, std::nullopt}; // a negative number
    arg.remove_prefix(arg[1] == '-' ? 2 : 1);
    const usize eq = arg.find('=');
    if (eq == std::string_view::npos) return {arg, std::nullopt};
    return {arg.substr(0, eq), arg.substr(eq + 1)};
}

std::atomic<bool>* g_stop = nullptr;
void heliosOnStopSignal(int) {
    if (g_stop) g_stop->store(true);
}
} // namespace

ProcessArgs::ProcessArgs(CommandLine cmd) : m_cmd(std::move(cmd)) {}

std::vector<std::string> ProcessArgs::options(std::string_view key) const {
    std::vector<std::string> out;
    const auto& args = m_cmd.arguments();
    for (usize i = 0; i < args.size(); ++i) {
        auto [k, v] = splitOption(args[i]);
        if (k.empty() || !keyEquals(k, key)) continue;
        if (v) {
            out.emplace_back(*v);
        } else if (i + 1 < args.size() && splitOption(args[i + 1]).first.empty()) {
            out.push_back(args[i + 1]);
            ++i;
        }
    }
    return out;
}

std::optional<std::string> ProcessArgs::option(std::string_view key) const {
    auto all = options(key);
    if (all.empty()) return std::nullopt;
    return all.back();
}

bool ProcessArgs::flag(std::string_view key) const {
    for (const std::string& a : m_cmd.arguments()) {
        auto [k, v] = splitOption(a);
        if (!k.empty() && keyEquals(k, key)) {
            if (!v) return true;
            return *v == "1" || *v == "true" || *v == "yes" || *v == "on";
        }
    }
    return false;
}

std::string ProcessArgs::get(std::string_view key, std::string_view env, std::string_view fallback) const {
    if (auto v = option(key)) return *v;
    if (!env.empty())
        if (auto e = envVar(env)) return *e;
    return std::string(fallback);
}

std::vector<std::string> ProcessArgs::positional() const {
    std::vector<std::string> out;
    const auto& args = m_cmd.arguments();
    for (usize i = 0; i < args.size(); ++i) {
        auto [k, v] = splitOption(args[i]);
        if (k.empty()) {
            out.push_back(args[i]);
        } else if (!v && i + 1 < args.size() && splitOption(args[i + 1]).first.empty()) {
            ++i; // `--key value`: the value is not positional
        }
    }
    return out;
}

std::optional<std::string> envVar(std::string_view name) {
    const std::string n(name);
    const char* v = std::getenv(n.c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

std::optional<std::filesystem::path> backendKeyFile(const ProcessArgs& args, std::string_view file) {
    const std::string dir = args.get("backend-data", "", "saved/backend");
    const std::filesystem::path p = fs::pathFromUtf8(dir) / "keys" / fs::pathFromUtf8(file);
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) return p;
    return std::nullopt;
}

Result<std::unique_ptr<NatsBus>> connectBusFromArgs(const ProcessArgs& args, std::string_view connectionName) {
    NatsBusConfig c;
    c.url = args.get("nats", "HELIOS_NATS_URL");
    if (c.url.empty()) return std::unique_ptr<NatsBus>{};
    c.user = args.get("nats-user", "HELIOS_NATS_USER", "fleet");
    c.password = args.get("nats-password", "HELIOS_NATS_PASSWORD");
    if (c.password.empty()) {
        std::optional<std::filesystem::path> file;
        if (auto p = args.option("nats-keys")) file = fs::pathFromUtf8(*p);
        else file = backendKeyFile(args, "nats-fleet.json");
        if (file) {
            HELIOS_TRY_ASSIGN(Keyring ring, loadKeyring(*file));
            c.password = ring.current().secretBase64; // the backend's FleetPassword()
        }
    }
    c.name = std::string(connectionName);
    return NatsBus::connect(c);
}

Result<LoadedKey> loadKey(const ProcessArgs& args, std::string_view option, std::string_view env, std::string_view backendFile,
                          std::string_view devLabel, bool loopbackOnly) {
    std::optional<std::filesystem::path> file;
    if (auto p = args.option(option)) file = fs::pathFromUtf8(*p);
    else if (auto e = envVar(env)) file = fs::pathFromUtf8(*e);
    else if (!backendFile.empty()) file = backendKeyFile(args, backendFile);
    if (file) {
        HELIOS_TRY_ASSIGN(Keyring ring, loadKeyring(*file));
        HELIOS_TRY_ASSIGN(net::Key key, netcodeKey(ring.current()));
        return LoadedKey{key, ring.current().id, fs::pathToUtf8(*file)};
    }
    if (args.flag("dev-insecure-keys")) {
        if (!loopbackOnly)
            return Error{ErrorCode::PermissionDenied, "--dev-insecure-keys only works when every address is loopback"};
        return LoadedKey{insecureDevKey(devLabel), 1, "insecure dev key '" + std::string(devLabel) + "'"};
    }
    return makeError(ErrorCode::NotFound,
                     "no key: pass --{} <keyring.json>, set {}, run next to the backend's data dir (--backend-data), "
                     "or use --dev-insecure-keys on loopback",
                     option, env);
}

void installStopSignal(std::atomic<bool>& stop) {
    g_stop = &stop;
    std::signal(SIGINT, heliosOnStopSignal);
    std::signal(SIGTERM, heliosOnStopSignal);
}

Result<net::Address> parseAddress(std::string_view text, std::string_view what) {
    auto a = net::Address::parse(text);
    if (!a || !a->isValid()) return makeError(ErrorCode::InvalidArgument, "{}: '{}' is not an ip:port address", what, text);
    return *a;
}

} // namespace helios::server
