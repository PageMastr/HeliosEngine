#pragma once
// ProbeClient: a minimal game client for the Phase 0 protocol (tests, bots, `helios-gateway probe`
// and the NS-0.3 Windows <-> Linux interop check). It connects with a netcode token, waits for the
// gateway's Welcome, sends Echo requests and measures their round trip, and records TickState
// chunks, TimeDilation notices, reconnect tickets, route changes and kicks.
//
// mintDevToken() mints a token locally with a known shard key, as the Session service would; only
// for tests and `--dev-insecure-keys` runs (production tokens come from the Session service over
// HTTPS).
//
// Threading: one thread (the owner's).

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/net/endpoint.h"
#include "helios/server/protocol.h"

namespace helios::server {

struct DevTokenParams {
    u64 protocolId = 0;
    net::Key key{};
    u64 sessionId = 0;                 ///< netcode client id
    std::vector<net::Address> gateways;
    proto::SessionUserData user;
    i32 expireSeconds = 45;
    i32 timeoutSeconds = 10;
};
Result<net::ConnectTokenBytes> mintDevToken(const DevTokenParams& params);

struct ProbeStats {
    std::optional<proto::ClientWelcome> welcome;
    u64 welcomes = 0;
    u64 echoesSent = 0;
    u64 echoesReceived = 0;
    u64 echoMismatches = 0;
    f64 lastRttMs = 0.0;
    f64 maxRttMs = 0.0;
    f64 totalRttMs = 0.0;
    u64 tickStates = 0;
    u64 lastTick = 0;
    u32 lastDilationPpm = 0;
    u64 tickRegressions = 0;         ///< TickState ticks that went backwards.
    std::vector<proto::ClientTimeDilation> dilations;
    std::vector<proto::ClientReconnectTicket> tickets;
    std::vector<proto::ClientRouteState> routeStates;
    std::optional<proto::ClientKick> kick;
    u64 pongs = 0;
    u64 otherMessages = 0;
    std::optional<net::DisconnectReason> disconnected;
};

struct ProbeConfig {
    std::unique_ptr<net::IDatagramTransport> transport; ///< Tests: a VirtualNetwork socket.
    /// Local socket address; invalid = automatic (the wildcard). A loopback address keeps a probe
    /// of a local gateway off the LAN (no Windows Firewall prompt).
    net::Address bind;
    std::string name = "probe";
};

class ProbeClient {
public:
    static Result<std::unique_ptr<ProbeClient>> create(ProbeConfig config, i64 nowNs);
    ~ProbeClient();

    Result<void> connect(std::span<const u8> token, i64 nowNs);
    /// Receives and flushes (call every loop iteration).
    void update(i64 nowNs);
    bool isConnected() const;
    bool isWelcomed() const noexcept { return m_stats.welcome.has_value(); }
    /// Sends an Echo request with `data`; returns its sequence number.
    u32 sendEcho(std::span<const u8> data, i64 nowNs);
    void sendPing(u64 nonce);
    net::SendResult send(net::Channel channel, std::span<const u8> payload);
    void disconnect();
    const ProbeStats& stats() const noexcept { return m_stats; }
    net::Client& client() noexcept { return *m_client; }

    struct Handler;

private:
    ProbeClient() = default;
    void onMessage(net::Channel channel, std::span<const u8> payload);

    std::unique_ptr<net::Client> m_client;
    std::unique_ptr<Handler> m_handler;
    ProbeStats m_stats;
    u32 m_nextSeq = 1;
    std::vector<std::pair<u32, i64>> m_inFlight; // echo seq -> send time
    i64 m_now = 0;
};

} // namespace helios::server
