// Client: a netcode client instance over an IDatagramTransport with one Connection.

#include <algorithm>
#include <cstring>

#include <netcode.h>

#include "helios/core/assert.h"
#include "helios/net/endpoint.h"
#include "net_internal.h"
#include "netcode_glue.h"

namespace helios::net {

std::string_view clientStateName(ClientState state) noexcept {
    switch (state) {
    case ClientState::Disconnected: return "Disconnected";
    case ClientState::SendingRequest: return "SendingRequest";
    case ClientState::SendingResponse: return "SendingResponse";
    case ClientState::Connected: return "Connected";
    case ClientState::ConnectTokenExpired: return "ConnectTokenExpired";
    case ClientState::InvalidConnectToken: return "InvalidConnectToken";
    case ClientState::TimedOut: return "TimedOut";
    case ClientState::RequestTimedOut: return "RequestTimedOut";
    case ClientState::ResponseTimedOut: return "ResponseTimedOut";
    case ClientState::Denied: return "Denied";
    }
    return "?";
}

namespace {

ClientState mapState(int s) noexcept {
    switch (s) {
    case NETCODE_CLIENT_STATE_CONNECT_TOKEN_EXPIRED: return ClientState::ConnectTokenExpired;
    case NETCODE_CLIENT_STATE_INVALID_CONNECT_TOKEN: return ClientState::InvalidConnectToken;
    case NETCODE_CLIENT_STATE_CONNECTION_TIMED_OUT: return ClientState::TimedOut;
    case NETCODE_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT: return ClientState::ResponseTimedOut;
    case NETCODE_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT: return ClientState::RequestTimedOut;
    case NETCODE_CLIENT_STATE_CONNECTION_DENIED: return ClientState::Denied;
    case NETCODE_CLIENT_STATE_SENDING_CONNECTION_REQUEST: return ClientState::SendingRequest;
    case NETCODE_CLIENT_STATE_SENDING_CONNECTION_RESPONSE: return ClientState::SendingResponse;
    case NETCODE_CLIENT_STATE_CONNECTED: return ClientState::Connected;
    default: return ClientState::Disconnected;
    }
}

DisconnectReason reasonFor(ClientState s, bool localDisconnect) noexcept {
    switch (s) {
    case ClientState::TimedOut: return DisconnectReason::TimedOut;
    case ClientState::ConnectTokenExpired: return DisconnectReason::ConnectTokenExpired;
    case ClientState::InvalidConnectToken: return DisconnectReason::InvalidConnectToken;
    case ClientState::RequestTimedOut: return DisconnectReason::RequestTimedOut;
    case ClientState::ResponseTimedOut: return DisconnectReason::ResponseTimedOut;
    case ClientState::Denied: return DisconnectReason::ConnectionDenied;
    default: return localDisconnect ? DisconnectReason::ClientDisconnected : DisconnectReason::ServerDisconnected;
    }
}

// netcode paces handshake packets at NETCODE_PACKET_SEND_RATE (10 Hz).
constexpr f64 kNetcodeSendInterval = 1.0 / 10.0;

} // namespace

struct Client::Impl {
    struct Event {
        bool connected = false;
        SessionHandle handle;
        DisconnectReason reason = DisconnectReason::None;
    };

    detail::LibraryRef library;
    ClientConfig cfg;
    std::unique_ptr<IDatagramTransport> transport;
    netcode_client_t* client = nullptr;
    PreFilter preFilter{PreFilter::Role::Client, PreFilterConfig{}};
    std::unique_ptr<Connection> conn;
    ConnectTokenBytes token{};
    u32 generation = 0;
    bool wasConnected = false;
    bool challengeReceived = false;
    bool localDisconnect = false;
    bool malformed = false;
    bool inUpdate = false;
    bool pendingDisconnect = false; // requested from a handler; applied after delivery
    f64 now = 0.0;
    f64 netcodeOffset = 0.0; // added to `now` for netcode (fast handshake)
    f64 connectStart = -1.0;
    f64 connectedAt = -1.0;
    u32 datagramsThisUpdate = 0;
    std::vector<Event> events;

    SessionHandle handle() const { return SessionHandle(0, generation); }

    static void sendOverride(void* context, netcode_address_t* to, const uint8_t* data, int bytes) {
        auto* self = static_cast<Impl*>(context);
        self->transport->send(detail::fromNetcode(*to), std::span<const u8>(data, static_cast<usize>(bytes)));
    }

    static int receiveOverride(void* context, netcode_address_t* from, uint8_t* data, int maxBytes) {
        auto* self = static_cast<Impl*>(context);
        for (;;) {
            if (self->datagramsThisUpdate >= self->cfg.maxDatagramsPerUpdate) return 0;
            Address addr;
            const usize n = self->transport->receive(addr, std::span<u8>(data, static_cast<usize>(maxBytes)));
            if (n == 0) return 0;
            ++self->datagramsThisUpdate;
            if (self->preFilter.check(addr, std::span<const u8>(data, n), self->now) != PreFilter::Verdict::Accept) continue;
            *from = detail::toNetcode(addr);
            return static_cast<int>(n);
        }
    }

    static void stateChange(void* context, int previous, int current) {
        auto* self = static_cast<Impl*>(context);
        if (previous == NETCODE_CLIENT_STATE_SENDING_CONNECTION_REQUEST &&
            current == NETCODE_CLIENT_STATE_SENDING_CONNECTION_RESPONSE) {
            self->challengeReceived = true;
        }
    }

    static void packetSend(void* context, std::span<const u8> packet) {
        auto* self = static_cast<Impl*>(context);
        netcode_client_send_packet(self->client, packet.data(), static_cast<int>(packet.size()));
    }

    void dropConnection(DisconnectReason reason) {
        if (wasConnected) events.push_back(Event{false, handle(), reason});
        wasConnected = false;
        conn.reset();
    }

    /// Runs netcode_client_update; with fastHandshake, a challenge that just arrived is answered
    /// in the same update by moving netcode's clock past its 10 Hz handshake pacing.
    void runNetcode() {
        challengeReceived = false;
        netcode_client_update(client, now + netcodeOffset);
        if (cfg.fastHandshake && challengeReceived) {
            challengeReceived = false;
            netcodeOffset += kNetcodeSendInterval + 1e-6;
            netcode_client_update(client, now + netcodeOffset);
        }
    }

    void syncState() {
        const ClientState st = mapState(netcode_client_state(client));
        if (st == ClientState::Connected && !wasConnected) {
            auto c = Connection::create(cfg.connection, &Impl::packetSend, this, now, cfg.name);
            if (!c) {
                HELIOS_LOG_ERROR(LogNet, "[{}] cannot create connection: {}", cfg.name, c.error());
                netcode_client_disconnect(client);
                return;
            }
            conn = std::move(c).value();
            wasConnected = true;
            connectedAt = now;
            events.push_back(Event{true, handle(), DisconnectReason::None});
            HELIOS_LOG_DEBUG(LogNet, "[{}] connected to {} in {:.1f} ms", cfg.name, serverAddress(),
                             (now - connectStart) * 1000.0);
        } else if (st != ClientState::Connected && wasConnected) {
            dropConnection(malformed ? DisconnectReason::Malformed : reasonFor(st, localDisconnect));
        }
    }

    Address serverAddress() const {
        if (netcode_address_t* a = netcode_client_server_address(client)) return detail::fromNetcode(*a);
        return {};
    }

    void deliverEvents(IEndpointHandler& handler) {
        usize i = 0;
        while (i < events.size()) {
            const Event e = events[i++];
            if (e.connected) handler.onConnected(e.handle);
            else handler.onDisconnected(e.handle, e.reason);
        }
        events.clear();
    }
};

Result<std::unique_ptr<Client>> Client::create(ClientConfig config, f64 now) {
    auto impl = std::make_unique<Impl>();
    Impl& m = *impl;
    if (!m.library.ok()) return Error{ErrorCode::Unsupported, "netcode_init failed"};
    m.transport = std::move(config.transport);
    m.cfg = std::move(config);
    m.now = now;
    m.preFilter = PreFilter(PreFilter::Role::Client, m.cfg.preFilter);
    {
        auto probe = Connection::create(m.cfg.connection, &Impl::packetSend, &m, now, "probe");
        if (!probe) return std::move(probe).error();
    }
    if (!m.transport) {
        SocketTransportConfig sc = m.cfg.socket;
        Address bind = m.cfg.bindAddress;
        if (!bind.isValid()) bind = UdpSocket::isIpv6Supported() ? Address::anyV6(0) : Address::anyV4(0);
        sc.socket.bindAddress = bind;
        sc.socket.dualStack = true;
        HELIOS_TRY_ASSIGN(auto socket, SocketTransport::open(sc));
        m.transport = std::move(socket);
    }
    netcode_client_config_t cc;
    netcode_default_client_config(&cc);
    cc.allocator_context = nullptr;
    cc.allocate_function = &detail::netAllocate;
    cc.free_function = &detail::netFree;
    cc.callback_context = &m;
    cc.state_change_callback = &Impl::stateChange;
    cc.override_send_and_receive = 1;
    cc.send_packet_override = &Impl::sendOverride;
    cc.receive_packet_override = &Impl::receiveOverride;
    // With the override netcode opens no socket; the address only labels the instance.
    Address local = m.transport->localAddress();
    if (!local.isValid()) local = Address::anyV4(0);
    const std::string localString = local.toString();
    m.client = netcode_client_create(localString.c_str(), &cc, now);
    if (m.client == nullptr) {
        return makeError(ErrorCode::IoError, "Client: netcode_client_create failed (error {})", netcode_client_create_error());
    }
    return std::unique_ptr<Client>(new Client(std::move(impl)));
}

Client::Client(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Client::~Client() {
    if (!m || !m->client) return;
    if (m->wasConnected) netcode_client_disconnect(m->client);
    m->transport->flush();
    m->conn.reset();
    netcode_client_destroy(m->client);
}

Result<void> Client::connect(std::span<const u8> token, f64 now) {
    Impl& s = *m;
    if (s.inUpdate) return Error{ErrorCode::InvalidState, "Client::connect called from a handler"};
    if (token.size() != kConnectTokenBytes) return Error{ErrorCode::InvalidArgument, "connect token must be 2048 bytes"};
    s.pendingDisconnect = false;
    s.now = std::max(s.now, now);
    // A new token replaces any current session (reconnect after timeout, 04 §2.4).
    if (s.wasConnected) s.localDisconnect = true;
    s.dropConnection(DisconnectReason::ClientDisconnected);
    std::memcpy(s.token.data(), token.data(), kConnectTokenBytes);
    ++s.generation;
    if (s.generation == 0) s.generation = 1;
    s.localDisconnect = false;
    s.malformed = false;
    s.connectStart = s.now;
    s.connectedAt = -1.0;
    netcode_client_connect(s.client, s.token.data());
    s.transport->flush();
    if (netcode_client_state(s.client) == NETCODE_CLIENT_STATE_INVALID_CONNECT_TOKEN) {
        return Error{ErrorCode::InvalidArgument, "connect token rejected (malformed public part)"};
    }
    return {};
}

void Client::disconnect() {
    Impl& s = *m;
    if (s.inUpdate) {
        s.pendingDisconnect = true; // the connection is being iterated; drop it after delivery
        return;
    }
    s.localDisconnect = true;
    netcode_client_disconnect(s.client);
    s.dropConnection(DisconnectReason::ClientDisconnected);
    s.transport->flush();
}

void Client::update(f64 now, IEndpointHandler& handler) {
    Impl& s = *m;
    s.now = std::max(s.now, now);
    s.inUpdate = true;
    s.transport->update(s.now);
    s.datagramsThisUpdate = 0;
    s.runNetcode();
    s.syncState();
    s.deliverEvents(handler);
    if (s.conn && s.wasConnected) {
        Connection& conn = *s.conn;
        for (;;) {
            int bytes = 0;
            uint64_t sequence = 0;
            uint8_t* packet = netcode_client_receive_packet(s.client, &bytes, &sequence);
            if (!packet) break;
            if (bytes > 0) conn.receivePacket(std::span<const u8>(packet, static_cast<usize>(bytes)), s.now);
            netcode_client_free_packet(s.client, packet);
        }
        conn.update(s.now);
        const SessionHandle h = s.handle();
        conn.forEachMessage([&](const Connection::ReceivedMessage& msg) { handler.onMessage(h, msg.channel, msg.payload); });
        conn.clearInbox();
        if (!conn.notifications().empty()) {
            handler.onDeliveryNotify(h, conn.notifications());
            conn.clearNotifications();
        }
        if (conn.isMalformed()) {
            HELIOS_LOG_WARN(LogNet, "[{}] server sent {} malformed packets; disconnecting", s.cfg.name, conn.strikes());
            s.malformed = true;
            s.localDisconnect = true;
            netcode_client_disconnect(s.client);
            s.dropConnection(DisconnectReason::Malformed);
            s.deliverEvents(handler);
        }
    }
    s.inUpdate = false;
    if (s.pendingDisconnect) {
        s.pendingDisconnect = false;
        disconnect();
        s.deliverEvents(handler);
    }
    s.transport->flush();
}

void Client::flush(f64 now) {
    Impl& s = *m;
    s.now = std::max(s.now, now);
    s.transport->update(s.now); // time-driven transports (NetSim) stamp these sends with `now`
    if (s.conn && s.wasConnected) s.conn->flush(s.now);
    s.transport->flush();
}

SendResult Client::send(Channel channel, std::span<const u8> payload) {
    if (!m->conn || !m->wasConnected || m->pendingDisconnect) return SendResult::NotConnected;
    return m->conn->send(channel, payload);
}

SendResult Client::sendState(std::span<const u8> chunk, u64* outSeq) {
    if (!m->conn || !m->wasConnected || m->pendingDisconnect) return SendResult::NotConnected;
    return m->conn->sendState(chunk, outSeq);
}

ClientState Client::state() const noexcept { return mapState(netcode_client_state(m->client)); }

bool Client::isConnecting() const noexcept {
    const ClientState s = state();
    return s == ClientState::SendingRequest || s == ClientState::SendingResponse;
}

SessionHandle Client::session() const noexcept { return m->handle(); }
i32 Client::clientIndex() const noexcept { return netcode_client_index(m->client); }
Connection* Client::connection() noexcept { return m->wasConnected ? m->conn.get() : nullptr; }
f64 Client::connectStartTime() const noexcept { return m->connectStart; }
f64 Client::connectedTime() const noexcept { return m->connectedAt; }
Address Client::serverAddress() const noexcept { return m->serverAddress(); }
IDatagramTransport& Client::transport() noexcept { return *m->transport; }
const PreFilterStats& Client::preFilterStats() const noexcept { return m->preFilter.stats(); }

} // namespace helios::net
