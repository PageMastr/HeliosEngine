#pragma once
// HTP endpoints (04 §2): netcode 1.4.8 client and server instances (L1: connect tokens,
// challenge/response, ChaCha20-Poly1305, replay protection, keep-alive, timeouts, slots) wrapped
// with Helios allocators and logging, one Connection (L2 reliable + L3 channels) per session, and
// an IDatagramTransport underneath (L0: UdpSocket, VirtualNetwork or NetSim).
//
//   auto server = Server::create(std::move(serverConfig), now).value();
//   auto client = Client::create(ClientConfig{}, now).value();
//   client->connect(tokenFromSessionService, now);
//   loop: server->update(now, handler); ... game tick ...; server->flush(now);
//
// update(now, handler) receives, decrypts, acks, delivers messages and notifications and runs
// timers; call it often (every I/O poll). flush(now) packs queued messages into packets; call it
// once per tick. Time is the caller's monotonic clock in seconds (helios::monotonicSeconds() in
// production, a simulated clock in tests).
//
// Server-side protections (04 §9): an L0 pre-filter drops datagrams whose size or type byte is
// impossible and limits connection requests to 10/s per IP before netcode sees them; sessions
// are policed at 120 pps (240 burst) and 64 + 40 kbit/s; 3 malformed packets disconnect a
// session (DisconnectReason::Malformed).
//
// Handshake latency (NS-0.1): netcode's client paces all handshake packets at 10 Hz, which
// would delay the connection response up to 100 ms after the challenge arrives. With
// ClientConfig::fastHandshake the client advances netcode's clock past that pacing gap when the
// challenge arrives, so the server completes the handshake 1.5 RTT after the first request.
//
// Threading: a Server or Client is owned by one thread (its I/O or tick thread). Different
// instances may run on different threads. Handlers are called on the updating thread; they may
// call send() and disconnect() (applied after delivery) but not update()/flush().

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/handle.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/address.h"
#include "helios/net/channel.h"
#include "helios/net/connect_token.h"
#include "helios/net/connection.h"
#include "helios/net/transport.h"

namespace helios::net {

struct SessionTag;
/// Identifies one session: slot index + a per-slot generation, so a handle to a departed client
/// never reaches the next client in that slot.
using SessionHandle = Handle<SessionTag>;

enum class DisconnectReason : u8 {
    None,
    ClientDisconnected,  ///< The client said goodbye.
    ServerDisconnected,  ///< The server disconnected the session (kick, shutdown).
    TimedOut,            ///< No packets for the token's timeout (linkdead, 04 §2.4).
    Malformed,           ///< Strike limit of malformed packets (04 §9).
    ConnectTokenExpired, ///< Client: the token expired before the handshake finished.
    InvalidConnectToken, ///< Client: the token could not be read.
    ConnectionDenied,    ///< Client: the server was full.
    RequestTimedOut,     ///< Client: no challenge from any server in the token.
    ResponseTimedOut,    ///< Client: no keep-alive after the challenge response.
};

std::string_view disconnectReasonName(DisconnectReason reason) noexcept;

/// Receives endpoint events. All callbacks happen inside update() on the updating thread.
class IEndpointHandler {
public:
    virtual ~IEndpointHandler() = default;
    virtual void onConnected(SessionHandle session) { (void)session; }
    virtual void onDisconnected(SessionHandle session, DisconnectReason reason) {
        (void)session;
        (void)reason;
    }
    /// `payload` is valid only during the call.
    virtual void onMessage(SessionHandle session, Channel channel, std::span<const u8> payload) = 0;
    /// STATE delivery notifications (seq values returned by sendState()).
    virtual void onDeliveryNotify(SessionHandle session, std::span<const PacketNotify> notifies) {
        (void)session;
        (void)notifies;
    }
};

// ---------------------------------------------------------------------------------------------
// L0 pre-filter
// ---------------------------------------------------------------------------------------------

struct PreFilterConfig {
    bool enabled = true;
    f64 requestsPerSecondPerIp = 10.0; ///< Connection requests (04 §9).
    f64 requestBurstPerIp = 10.0;
    u32 tableSize = 4096;              ///< Per-IP buckets (hashed; collisions share a bucket).
    /// Key of the bucket hash. 0 (default) draws a random key per PreFilter, so an attacker cannot
    /// compute which spoofed source shares a victim's bucket and starve its connection requests.
    /// Tests may fix it for reproducibility.
    u64 hashKey = 0;
};

struct PreFilterStats {
    u64 accepted = 0;
    u64 badSize = 0;
    u64 badType = 0;
    u64 rateLimited = 0;
};

/// Cheap checks on raw datagrams before netcode decrypts anything: packet type in the low nibble
/// of the prefix byte must be one the receiving role accepts, the sequence byte count 1..8, and
/// the size exactly what that type encrypts to (requests: exactly 1,078 bytes, unencrypted).
/// Connection requests are rate-limited per source: an IPv4 address, or an IPv6 /64 (one
/// subscriber's prefix, so rotating addresses inside it does not multiply the allowance).
class PreFilter {
public:
    enum class Role : u8 { Server, Client };
    enum class Verdict : u8 { Accept, BadSize, BadType, RateLimited };

    PreFilter(Role role, const PreFilterConfig& config);
    Verdict check(const Address& from, std::span<const u8> datagram, f64 now);
    const PreFilterStats& stats() const noexcept { return m_stats; }
    /// Rate-limit bucket of a source address (exposed for tests).
    usize bucketIndex(const Address& from) const noexcept;
    u64 hashKey() const noexcept { return m_key; }

private:
    struct Bucket {
        f64 tokens = 0.0;
        f64 last = -1.0;
    };
    Role m_role;
    PreFilterConfig m_config;
    u64 m_key = 0;
    std::vector<Bucket> m_buckets;
    PreFilterStats m_stats;
};

// ---------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------

struct ServerConfig {
    u64 protocolId = 0;
    Key privateKey{};
    /// Socket bind address (ignored when `transport` is set). Port 0 = ephemeral.
    Address bindAddress = Address::anyV4(0);
    /// Addresses this server answers to in tokens (<= 2, e.g. one IPv4 and one IPv6). Port 0 is
    /// replaced by the bound port. Empty: the bound address (loopback if bound to "any").
    std::vector<Address> publicAddresses;
    u32 maxClients = 256; ///< <= 256 slots per netcode instance (04 §2.6).
    /// Must match the Session service's token lifetime (04 §2.3: 45 s).
    i32 maxConnectTokenLifetimeSeconds = kDefaultTokenExpirySeconds;
    ConnectionConfig connection = ConnectionConfig::server();
    SocketTransportConfig socket;
    /// Optional transport (VirtualNetwork socket, NetSimTransport, ...). Owned by the server.
    std::unique_ptr<IDatagramTransport> transport;
    PreFilterConfig preFilter;
    /// Datagrams read per update(); the rest wait in the socket (netcode queues 256 per client).
    u32 maxDatagramsPerUpdate = 4096;
    std::string name = "server";
};

struct SessionInfo {
    SessionHandle handle;
    u64 clientId = 0;
    Address address;
    UserData userData{};
    f64 connectedAt = 0.0;
};

struct ServerStats {
    u64 connects = 0;
    u64 disconnects = 0;
    u64 malformedDisconnects = 0;
    u64 datagramsReceived = 0; ///< Passed the pre-filter.
    u64 datagramsSent = 0;
    PreFilterStats preFilter;
};

class Server {
public:
    static Result<std::unique_ptr<Server>> create(ServerConfig config, f64 now);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Receives, runs netcode and per-session timers, delivers events, messages and notifies.
    void update(f64 now, IEndpointHandler& handler);
    /// Packs and sends queued messages for every session (once per tick).
    void flush(f64 now);

    SendResult send(SessionHandle session, Channel channel, std::span<const u8> payload);
    SendResult sendState(SessionHandle session, std::span<const u8> chunk, u64* outSeq = nullptr);
    /// Disconnects a session (deferred until after delivery when called from a handler). The
    /// onDisconnected event arrives in the next update().
    void disconnect(SessionHandle session);
    void disconnectAll();

    bool isConnected(SessionHandle session) const noexcept;
    u32 connectedCount() const noexcept;
    u32 maxClients() const noexcept;
    std::vector<SessionHandle> sessions() const;
    const SessionInfo* sessionInfo(SessionHandle session) const noexcept;
    /// The connected session with this client id (the Session service's session ID), or an invalid
    /// handle. netcode ignores a new connection request while a session with the same client id is
    /// connected, so a client reconnecting from a new address (NAT rebinding, 04 §2.4) waits until
    /// the old session times out unless the gateway drops it first: the session_epoch signal finds
    /// it here and calls disconnect().
    SessionHandle findSession(u64 clientId) const noexcept;
    /// Per-session connection (stats, budgets); null for stale handles.
    Connection* connection(SessionHandle session) noexcept;

    std::span<const Address> publicAddresses() const noexcept;
    IDatagramTransport& transport() noexcept;
    ServerStats stats() const;

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

// ---------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------

enum class ClientState : u8 {
    Disconnected,
    SendingRequest,
    SendingResponse,
    Connected,
    ConnectTokenExpired,
    InvalidConnectToken,
    TimedOut,
    RequestTimedOut,
    ResponseTimedOut,
    Denied,
};

std::string_view clientStateName(ClientState state) noexcept;

struct ClientConfig {
    /// Socket bind address. Invalid (default) = automatic: dual-stack [::]:0 when the OS has
    /// IPv6, else 0.0.0.0:0. Ignored when `transport` is set.
    Address bindAddress;
    ConnectionConfig connection = ConnectionConfig::client();
    SocketTransportConfig socket;
    std::unique_ptr<IDatagramTransport> transport;
    PreFilterConfig preFilter;
    u32 maxDatagramsPerUpdate = 256;
    /// Answer the challenge immediately instead of at netcode's 10 Hz pacing (NS-0.1).
    bool fastHandshake = true;
    std::string name = "client";
};

class Client {
public:
    static Result<std::unique_ptr<Client>> create(ClientConfig config, f64 now);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Starts connecting with a 2,048-byte token (dropping any current connection). Also used to
    /// reconnect after a timeout with a fresh token (04 §2.4). Not callable from a handler
    /// (InvalidState).
    Result<void> connect(std::span<const u8> token, f64 now);
    /// Sends netcode's disconnect packets and drops the connection (deferred until after delivery
    /// when called from a handler; onDisconnected follows in the same update).
    void disconnect();

    void update(f64 now, IEndpointHandler& handler);
    void flush(f64 now);

    SendResult send(Channel channel, std::span<const u8> payload);
    SendResult sendState(std::span<const u8> chunk, u64* outSeq = nullptr);

    ClientState state() const noexcept;
    bool isConnected() const noexcept { return state() == ClientState::Connected; }
    bool isConnecting() const noexcept;
    /// Handle of the current (or last) connection; the generation changes on every connect().
    SessionHandle session() const noexcept;
    /// Slot the server assigned (valid while connected).
    i32 clientIndex() const noexcept;
    Connection* connection() noexcept;
    /// Clock value of the last connect() and of the transition to Connected (-1 if none).
    f64 connectStartTime() const noexcept;
    f64 connectedTime() const noexcept;
    Address serverAddress() const noexcept;
    IDatagramTransport& transport() noexcept;
    const PreFilterStats& preFilterStats() const noexcept;

    struct Impl;

private:
    explicit Client(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

} // namespace helios::net
