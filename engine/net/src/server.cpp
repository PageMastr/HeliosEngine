// Server: a netcode server instance over an IDatagramTransport, one Connection per session.

#include <algorithm>
#include <cstring>

#include <netcode.h>

#include "helios/core/assert.h"
#include "helios/net/endpoint.h"
#include "net_internal.h"
#include "netcode_glue.h"

namespace helios::net {

std::string_view disconnectReasonName(DisconnectReason reason) noexcept {
    switch (reason) {
    case DisconnectReason::None: return "None";
    case DisconnectReason::ClientDisconnected: return "ClientDisconnected";
    case DisconnectReason::ServerDisconnected: return "ServerDisconnected";
    case DisconnectReason::TimedOut: return "TimedOut";
    case DisconnectReason::Malformed: return "Malformed";
    case DisconnectReason::ConnectTokenExpired: return "ConnectTokenExpired";
    case DisconnectReason::InvalidConnectToken: return "InvalidConnectToken";
    case DisconnectReason::ConnectionDenied: return "ConnectionDenied";
    case DisconnectReason::RequestTimedOut: return "RequestTimedOut";
    case DisconnectReason::ResponseTimedOut: return "ResponseTimedOut";
    }
    return "?";
}

struct Server::Impl {
    struct Slot {
        u32 generation = 0;
        bool connected = false;
        bool pendingDisconnect = false;
        DisconnectReason forcedReason = DisconnectReason::None;
        std::unique_ptr<Connection> conn;
        SessionInfo info;
    };
    struct SendContext {
        Impl* impl = nullptr;
        int index = 0;
    };
    struct Event {
        bool connected = false;
        SessionHandle handle;
        DisconnectReason reason = DisconnectReason::None;
    };

    detail::LibraryRef library;
    ServerConfig cfg;
    std::unique_ptr<IDatagramTransport> transport;
    netcode_server_t* server = nullptr;
    std::vector<Address> publicAddresses;
    PreFilter preFilter{PreFilter::Role::Server, PreFilterConfig{}};
    std::vector<Slot> slots;
    std::vector<SendContext> sendContexts;
    std::vector<Event> events;
    f64 now = 0.0;
    u32 datagramsThisUpdate = 0;
    bool inUpdate = false;
    ServerStats st;

    SessionHandle handleOf(int index) const {
        return SessionHandle(static_cast<u32>(index), slots[static_cast<usize>(index)].generation);
    }

    Slot* find(SessionHandle h) {
        if (h.index() >= slots.size()) return nullptr;
        Slot& s = slots[h.index()];
        if (!s.connected || s.generation != h.generation() || !s.conn) return nullptr;
        return &s;
    }
    const Slot* find(SessionHandle h) const { return const_cast<Impl*>(this)->find(h); }

    // ---- netcode callbacks ----------------------------------------------------------------------
    static void sendOverride(void* context, netcode_address_t* to, const uint8_t* data, int bytes) {
        auto* self = static_cast<Impl*>(context);
        ++self->st.datagramsSent;
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
            ++self->st.datagramsReceived;
            *from = detail::toNetcode(addr);
            return static_cast<int>(n);
        }
    }

    static void packetSend(void* context, std::span<const u8> packet) {
        auto* ctx = static_cast<SendContext*>(context);
        netcode_server_send_packet(ctx->impl->server, ctx->index, packet.data(), static_cast<int>(packet.size()));
    }

    static void connectDisconnect(void* context, int index, int connected) {
        auto* self = static_cast<Impl*>(context);
        if (index < 0 || static_cast<usize>(index) >= self->slots.size()) return;
        Slot& slot = self->slots[static_cast<usize>(index)];
        if (connected) {
            ++slot.generation;
            if (slot.generation == 0) slot.generation = 1;
            slot.connected = true;
            slot.pendingDisconnect = false;
            slot.forcedReason = DisconnectReason::None;
            auto conn = Connection::create(self->cfg.connection, &Impl::packetSend,
                                           &self->sendContexts[static_cast<usize>(index)], self->now,
                                           self->cfg.name + "/" + std::to_string(index));
            if (!conn) {
                // Only an allocation failure gets here (the config was validated in create()).
                // Never call back into netcode from its callback: disconnect after this update.
                HELIOS_LOG_ERROR(LogNet, "[{}] cannot create connection: {}", self->cfg.name, conn.error());
                slot.pendingDisconnect = true;
                slot.forcedReason = DisconnectReason::ServerDisconnected;
            } else {
                slot.conn = std::move(conn).value();
            }
            slot.info = SessionInfo{};
            slot.info.handle = self->handleOf(index);
            slot.info.clientId = netcode_server_client_id(self->server, index);
            if (netcode_address_t* a = netcode_server_client_address(self->server, index)) {
                slot.info.address = detail::fromNetcode(*a);
            }
            if (const void* ud = netcode_server_client_user_data(self->server, index)) {
                std::memcpy(slot.info.userData.data(), ud, kUserDataBytes);
            }
            slot.info.connectedAt = self->now;
            ++self->st.connects;
            self->events.push_back(Event{true, slot.info.handle, DisconnectReason::None});
            HELIOS_LOG_DEBUG(LogNet, "[{}] client {:016x} connected from {} in slot {}", self->cfg.name,
                             slot.info.clientId, slot.info.address, index);
        } else {
            if (!slot.connected) return;
            DisconnectReason reason = slot.forcedReason;
            if (reason == DisconnectReason::None) {
                switch (netcode_server_client_disconnect_reason(self->server, index)) {
                case NETCODE_SERVER_CLIENT_DISCONNECT_REASON_TIMED_OUT: reason = DisconnectReason::TimedOut; break;
                case NETCODE_SERVER_CLIENT_DISCONNECT_REASON_CLIENT_DISCONNECT:
                    reason = DisconnectReason::ClientDisconnected;
                    break;
                default: reason = DisconnectReason::ServerDisconnected; break;
                }
            }
            const SessionHandle h = self->handleOf(index);
            slot.connected = false;
            slot.pendingDisconnect = false;
            slot.conn.reset();
            ++self->st.disconnects;
            if (reason == DisconnectReason::Malformed) ++self->st.malformedDisconnects;
            self->events.push_back(Event{false, h, reason});
            HELIOS_LOG_DEBUG(LogNet, "[{}] slot {} disconnected ({})", self->cfg.name, index,
                             disconnectReasonName(reason));
        }
    }

    void deliverEvents(IEndpointHandler& handler) {
        // Handlers may queue more events (e.g. disconnect from onConnected); drain until empty.
        usize i = 0;
        while (i < events.size()) {
            const Event e = events[i++];
            if (e.connected) handler.onConnected(e.handle);
            else handler.onDisconnected(e.handle, e.reason);
        }
        events.clear();
    }

    void disconnectSlot(int index, DisconnectReason reason) {
        Slot& slot = slots[static_cast<usize>(index)];
        if (!slot.connected) return;
        slot.forcedReason = reason;
        netcode_server_disconnect_client(server, index);
    }

    void applyPendingDisconnects() {
        for (usize i = 0; i < slots.size(); ++i) {
            if (slots[i].connected && slots[i].pendingDisconnect) {
                disconnectSlot(static_cast<int>(i), slots[i].forcedReason == DisconnectReason::None
                                                        ? DisconnectReason::ServerDisconnected
                                                        : slots[i].forcedReason);
            }
        }
    }
};

Result<std::unique_ptr<Server>> Server::create(ServerConfig config, f64 now) {
    auto impl = std::make_unique<Impl>();
    Impl& m = *impl;
    if (!m.library.ok()) return Error{ErrorCode::Unsupported, "netcode_init failed"};
    if (config.maxClients == 0 || config.maxClients > NETCODE_MAX_CLIENTS)
        return Error{ErrorCode::InvalidArgument, "Server: maxClients must be 1..256"};
    if (config.publicAddresses.size() > 2) return Error{ErrorCode::InvalidArgument, "Server: at most 2 public addresses"};
    m.transport = std::move(config.transport);
    m.cfg = std::move(config);
    m.now = now;
    m.preFilter = PreFilter(PreFilter::Role::Server, m.cfg.preFilter);
    if (!m.transport) {
        SocketTransportConfig sc = m.cfg.socket;
        sc.socket.bindAddress = m.cfg.bindAddress;
        HELIOS_TRY_ASSIGN(auto socket, SocketTransport::open(sc));
        m.transport = std::move(socket);
    }
    const Address local = m.transport->localAddress();
    m.publicAddresses = m.cfg.publicAddresses;
    if (m.publicAddresses.empty()) {
        Address a = local;
        if (a.isUnspecified()) a = local.isIpv6() ? Address::loopbackV6(local.port()) : Address::loopbackV4(local.port());
        m.publicAddresses.push_back(a);
    }
    for (Address& a : m.publicAddresses) {
        if (!a.isValid()) return Error{ErrorCode::InvalidArgument, "Server: invalid public address"};
        if (a.port() == 0) a.setPort(local.port());
    }

    {
        // Validate the per-session config once so session set-up cannot fail on it later.
        auto probe = Connection::create(m.cfg.connection, &Impl::packetSend, nullptr, now, "probe");
        if (!probe) return std::move(probe).error();
    }
    m.slots.resize(m.cfg.maxClients);
    m.sendContexts.resize(m.cfg.maxClients);
    for (u32 i = 0; i < m.cfg.maxClients; ++i) m.sendContexts[i] = Impl::SendContext{&m, static_cast<int>(i)};

    netcode_server_config_t nc;
    netcode_default_server_config(&nc);
    nc.protocol_id = m.cfg.protocolId;
    std::memcpy(nc.private_key, m.cfg.privateKey.data(), NETCODE_KEY_BYTES);
    nc.allocator_context = nullptr;
    nc.allocate_function = &detail::netAllocate;
    nc.free_function = &detail::netFree;
    nc.callback_context = &m;
    nc.connect_disconnect_callback = &Impl::connectDisconnect;
    nc.override_send_and_receive = 1;
    nc.send_packet_override = &Impl::sendOverride;
    nc.receive_packet_override = &Impl::receiveOverride;
    nc.max_connect_token_lifetime = m.cfg.maxConnectTokenLifetimeSeconds;
    const std::string first = m.publicAddresses[0].toString();
    const std::string second = m.publicAddresses.size() > 1 ? m.publicAddresses[1].toString() : std::string();
    m.server = netcode_server_create_dual(first.c_str(), second.empty() ? nullptr : second.c_str(), &nc, now);
    if (m.server == nullptr) {
        return makeError(ErrorCode::IoError, "Server: netcode_server_create failed (error {})", netcode_server_create_error());
    }
    netcode_server_start(m.server, static_cast<int>(m.cfg.maxClients));
    HELIOS_LOG_INFO(LogNet, "[{}] listening on {} (public {}{}{}), {} slots", m.cfg.name, local, first,
                    second.empty() ? "" : ", ", second, m.cfg.maxClients);
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

Server::Server(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Server::~Server() {
    if (!m) return;
    if (m->server) {
        netcode_server_stop(m->server); // sends disconnect packets to every client
        m->transport->flush();
        for (auto& slot : m->slots) slot.conn.reset();
        netcode_server_destroy(m->server);
    }
}

void Server::update(f64 now, IEndpointHandler& handler) {
    Impl& s = *m;
    s.now = std::max(s.now, now);
    s.inUpdate = true;
    s.transport->update(s.now);
    s.datagramsThisUpdate = 0;
    netcode_server_update(s.server, s.now);
    s.deliverEvents(handler);
    for (usize i = 0; i < s.slots.size(); ++i) {
        Impl::Slot& slot = s.slots[i];
        if (!slot.connected || !slot.conn) continue;
        Connection& conn = *slot.conn;
        const int index = static_cast<int>(i);
        for (;;) {
            int bytes = 0;
            uint64_t sequence = 0;
            uint8_t* packet = netcode_server_receive_packet(s.server, index, &bytes, &sequence);
            if (!packet) break;
            if (bytes > 0) conn.receivePacket(std::span<const u8>(packet, static_cast<usize>(bytes)), s.now);
            netcode_server_free_packet(s.server, packet);
        }
        conn.update(s.now);
        const SessionHandle h = s.handleOf(index);
        conn.forEachMessage([&](const Connection::ReceivedMessage& msg) { handler.onMessage(h, msg.channel, msg.payload); });
        conn.clearInbox();
        if (!conn.notifications().empty()) {
            handler.onDeliveryNotify(h, conn.notifications());
            conn.clearNotifications();
        }
        if (conn.isMalformed() && !slot.pendingDisconnect) {
            HELIOS_LOG_WARN(LogNet, "[{}] disconnecting {} after {} malformed packets", s.cfg.name, slot.info.address,
                            conn.strikes());
            slot.pendingDisconnect = true;
            slot.forcedReason = DisconnectReason::Malformed;
        }
    }
    s.applyPendingDisconnects();
    s.deliverEvents(handler);
    s.st.preFilter = s.preFilter.stats();
    s.inUpdate = false;
    s.transport->flush();
}

void Server::flush(f64 now) {
    Impl& s = *m;
    s.now = std::max(s.now, now);
    s.transport->update(s.now); // time-driven transports (NetSim) stamp these sends with `now`
    for (auto& slot : s.slots) {
        if (slot.connected && slot.conn) slot.conn->flush(s.now);
    }
    s.transport->flush();
}

SendResult Server::send(SessionHandle session, Channel channel, std::span<const u8> payload) {
    Impl::Slot* slot = m->find(session);
    if (!slot || slot->pendingDisconnect) return SendResult::NotConnected;
    return slot->conn->send(channel, payload);
}

SendResult Server::sendState(SessionHandle session, std::span<const u8> chunk, u64* outSeq) {
    Impl::Slot* slot = m->find(session);
    if (!slot || slot->pendingDisconnect) return SendResult::NotConnected;
    return slot->conn->sendState(chunk, outSeq);
}

void Server::disconnect(SessionHandle session) {
    Impl::Slot* slot = m->find(session);
    if (!slot) return;
    if (m->inUpdate) {
        slot->pendingDisconnect = true;
        if (slot->forcedReason == DisconnectReason::None) slot->forcedReason = DisconnectReason::ServerDisconnected;
        return;
    }
    m->disconnectSlot(static_cast<int>(session.index()), DisconnectReason::ServerDisconnected);
    m->transport->flush();
}

void Server::disconnectAll() {
    for (const SessionHandle h : sessions()) disconnect(h);
}

bool Server::isConnected(SessionHandle session) const noexcept { return m->find(session) != nullptr; }

u32 Server::connectedCount() const noexcept {
    u32 n = 0;
    for (const auto& slot : m->slots) n += slot.connected ? 1u : 0u;
    return n;
}

u32 Server::maxClients() const noexcept { return m->cfg.maxClients; }

std::vector<SessionHandle> Server::sessions() const {
    std::vector<SessionHandle> out;
    for (usize i = 0; i < m->slots.size(); ++i) {
        if (m->slots[i].connected) out.push_back(m->handleOf(static_cast<int>(i)));
    }
    return out;
}

const SessionInfo* Server::sessionInfo(SessionHandle session) const noexcept {
    const Impl::Slot* slot = m->find(session);
    return slot ? &slot->info : nullptr;
}

SessionHandle Server::findSession(u64 clientId) const noexcept {
    for (usize i = 0; i < m->slots.size(); ++i) {
        const Impl::Slot& slot = m->slots[i];
        if (slot.connected && slot.conn && slot.info.clientId == clientId) return m->handleOf(static_cast<int>(i));
    }
    return {};
}

Connection* Server::connection(SessionHandle session) noexcept {
    Impl::Slot* slot = m->find(session);
    return slot ? slot->conn.get() : nullptr;
}

std::span<const Address> Server::publicAddresses() const noexcept { return m->publicAddresses; }
IDatagramTransport& Server::transport() noexcept { return *m->transport; }

ServerStats Server::stats() const {
    ServerStats s = m->st;
    s.preFilter = m->preFilter.stats();
    return s;
}

} // namespace helios::net
