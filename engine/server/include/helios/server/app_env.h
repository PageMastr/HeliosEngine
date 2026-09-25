#pragma once
// Process-level helpers shared by helios-cell and helios-gateway: options given as `--key=value`
// or `--key value` (the backend's helios.toml writes `args = ["--zone", "tallis"]`), the
// environment the backend passes to supervised children (HELIOS_SHARD, HELIOS_NATS_URL,
// HELIOS_NATS_USER, HELIOS_NATS_PASSWORD, HELIOS_NETCODE_KEYS, HELIOS_PROTOCOL_ID,
// HELIOS_TOKEN_LIFETIME), key-file lookup and Ctrl+C handling.
//
// Threading: call from the main thread during start-up.

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/cmdline.h"
#include "helios/core/result.h"
#include "helios/net/connect_token.h"
#include "helios/server/keys.h"
#include "helios/server/nats_bus.h"

namespace helios::server {

class ProcessArgs {
public:
    explicit ProcessArgs(CommandLine cmd);
    /// `--key=value`, or `--key value` when the next argument is not an option. Last one wins.
    std::optional<std::string> option(std::string_view key) const;
    /// Every occurrence, in order.
    std::vector<std::string> options(std::string_view key) const;
    bool flag(std::string_view key) const;
    /// The option, else the environment variable `env`, else `fallback`.
    std::string get(std::string_view key, std::string_view env, std::string_view fallback = {}) const;
    const CommandLine& commandLine() const noexcept { return m_cmd; }
    /// Arguments that are neither options nor option values (subcommands).
    std::vector<std::string> positional() const;

private:
    CommandLine m_cmd;
};

/// Value of an environment variable, or nullopt when unset or empty.
std::optional<std::string> envVar(std::string_view name);

/// Where the backend keeps its keys: `--backend-data=<dir>` (the helios-backend `--data` dir),
/// else ./saved/backend relative to the working directory. Returns <dir>/keys/<file> if it exists.
std::optional<std::filesystem::path> backendKeyFile(const ProcessArgs& args, std::string_view file);

/// Connects to NATS from `--nats` / HELIOS_NATS_URL with `--nats-user` / HELIOS_NATS_USER (default
/// "fleet") and `--nats-password` / HELIOS_NATS_PASSWORD, falling back to the password in
/// `--nats-keys` or <backend data>/keys/nats-fleet.json. Returns nullptr when no URL is configured.
Result<std::unique_ptr<NatsBus>> connectBusFromArgs(const ProcessArgs& args, std::string_view connectionName);

/// A netcode key from a keyring file option / env var / backend key file, in that order, or the
/// public insecure dev key (`--dev-insecure-keys`, loopback only). Also returns the generation id.
struct LoadedKey {
    net::Key key{};
    u32 id = 0;
    std::string source;
};
Result<LoadedKey> loadKey(const ProcessArgs& args, std::string_view option, std::string_view env, std::string_view backendFile,
                          std::string_view devLabel, bool loopbackOnly);

/// Sets `stop` on SIGINT / SIGTERM (Ctrl+C in a Windows console). Idempotent.
void installStopSignal(std::atomic<bool>& stop);

/// Parses "a.b.c.d:port" / "[v6]:port"; fails with a readable error.
Result<net::Address> parseAddress(std::string_view text, std::string_view what);

} // namespace helios::server
