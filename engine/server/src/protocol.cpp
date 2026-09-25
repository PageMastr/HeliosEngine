#include "helios/server/protocol.h"

#include <cstring>

namespace helios::server::proto {

// ---------------------------------------------------------------------------------------------
// ByteWriter / ByteReader
// ---------------------------------------------------------------------------------------------

void ByteWriter::u16v(u16 v) {
    m_out.push_back(static_cast<u8>(v));
    m_out.push_back(static_cast<u8>(v >> 8));
}
void ByteWriter::u32v(u32 v) {
    for (int i = 0; i < 4; ++i) m_out.push_back(static_cast<u8>(v >> (8 * i)));
}
void ByteWriter::u64v(u64 v) {
    for (int i = 0; i < 8; ++i) m_out.push_back(static_cast<u8>(v >> (8 * i)));
}
void ByteWriter::varint(u64 v) {
    u8 buf[net::wire::kMaxVarint64Bytes];
    const usize n = net::wire::writeVarint64(buf, v);
    m_out.insert(m_out.end(), buf, buf + n);
}
void ByteWriter::str(std::string_view s) {
    varint(s.size());
    m_out.insert(m_out.end(), s.begin(), s.end());
}

bool ByteReader::need(usize n) noexcept {
    if (m_fail || m_in.size() - m_pos < n) {
        m_fail = true;
        return false;
    }
    return true;
}
u8 ByteReader::u8v() noexcept {
    if (!need(1)) return 0;
    return m_in[m_pos++];
}
u16 ByteReader::u16v() noexcept {
    if (!need(2)) return 0;
    const u16 v = static_cast<u16>(m_in[m_pos] | (m_in[m_pos + 1] << 8));
    m_pos += 2;
    return v;
}
u32 ByteReader::u32v() noexcept {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(m_in[m_pos + static_cast<usize>(i)]) << (8 * i);
    m_pos += 4;
    return v;
}
u64 ByteReader::u64v() noexcept {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(m_in[m_pos + static_cast<usize>(i)]) << (8 * i);
    m_pos += 8;
    return v;
}
u64 ByteReader::varint() noexcept {
    if (m_fail) return 0;
    u64 v = 0;
    const usize n = net::wire::readVarint64(m_in.subspan(m_pos), v);
    if (n == 0) {
        m_fail = true;
        return 0;
    }
    m_pos += n;
    return v;
}
std::string ByteReader::str(usize maxBytes) noexcept {
    const u64 len = varint();
    if (m_fail || len > maxBytes || !need(static_cast<usize>(len))) {
        m_fail = true;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(m_in.data() + m_pos), static_cast<usize>(len));
    m_pos += static_cast<usize>(len);
    return s;
}
std::span<const u8> ByteReader::rest() noexcept {
    if (m_fail) return {};
    auto r = m_in.subspan(m_pos);
    m_pos = m_in.size();
    return r;
}

namespace {
Error malformed(std::string_view what) { return Error{ErrorCode::ParseError, std::string("malformed ") + std::string(what)}; }

template <class T>
Result<T> finish(ByteReader& r, T value, std::string_view what) {
    if (r.fail() || !r.atEnd()) return malformed(what);
    return value;
}

ByteWriter header(u64 stream, TrunkType type) {
    ByteWriter w;
    w.varint(stream);
    w.u8v(static_cast<u8>(type));
    return w;
}

bool validChannel(u8 c) noexcept { return net::isValidChannelId(c); }
} // namespace

std::string_view nackReasonName(NackReason r) noexcept {
    switch (r) {
    case NackReason::NotHosted: return "not_hosted";
    case NackReason::StaleEpoch: return "stale_epoch";
    case NackReason::Full: return "full";
    case NackReason::Draining: return "draining";
    }
    return "?";
}

std::string_view detachReasonName(DetachReason r) noexcept {
    switch (r) {
    case DetachReason::ClientLeft: return "client_left";
    case DetachReason::Superseded: return "superseded";
    case DetachReason::Kicked: return "kicked";
    case DetachReason::TimedOut: return "timed_out";
    case DetachReason::GatewayShutdown: return "gateway_shutdown";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Trunk
// ---------------------------------------------------------------------------------------------

Result<TrunkMessage> parseTrunk(std::span<const u8> bytes) {
    ByteReader r(bytes);
    TrunkMessage m;
    m.stream = r.varint();
    const u8 type = r.u8v();
    if (r.fail()) return malformed("trunk header");
    switch (static_cast<TrunkType>(type)) {
    case TrunkType::Hello:
    case TrunkType::Welcome:
    case TrunkType::ZoneFenced:
        if (m.stream != 0) return malformed("trunk stream");
        break;
    case TrunkType::Attach:
    case TrunkType::AttachAck:
    case TrunkType::AttachNack:
    case TrunkType::Detach:
    case TrunkType::Kick:
    case TrunkType::Forward:
    case TrunkType::Deliver:
        if (m.stream == 0) return malformed("trunk stream");
        break;
    default: return malformed("trunk type");
    }
    m.type = static_cast<TrunkType>(type);
    m.body = r.rest();
    return m;
}

std::vector<u8> encode(u64 stream, const Hello& m) {
    ByteWriter w = header(stream, TrunkType::Hello);
    w.u8v(m.version);
    w.u64v(m.processId);
    w.str(m.name);
    return w.take();
}
std::vector<u8> encode(u64 stream, const Welcome& m) {
    ByteWriter w = header(stream, TrunkType::Welcome);
    w.u8v(m.version);
    w.u64v(m.processId);
    w.u64v(m.epoch);
    w.str(m.name);
    return w.take();
}
std::vector<u8> encode(u64 stream, const ZoneFenced& m) {
    ByteWriter w = header(stream, TrunkType::ZoneFenced);
    w.u64v(m.zoneId);
    w.varint(m.leaseGen);
    return w.take();
}
std::vector<u8> encode(u64 stream, const Attach& m) {
    ByteWriter w = header(stream, TrunkType::Attach);
    w.u64v(m.sessionEpoch);
    w.u64v(m.zoneId);
    w.u64v(m.accountId);
    w.u64v(m.characterId);
    w.u8v(m.flags);
    return w.take();
}
std::vector<u8> encode(u64 stream, const AttachAck& m) {
    ByteWriter w = header(stream, TrunkType::AttachAck);
    w.u64v(m.sessionEpoch);
    w.u64v(m.zoneId);
    w.varint(m.leaseGen);
    w.u64v(m.tick);
    w.u32v(m.tickHz);
    w.u32v(m.dilationPpm);
    w.str(m.zoneName);
    return w.take();
}
std::vector<u8> encode(u64 stream, const AttachNack& m) {
    ByteWriter w = header(stream, TrunkType::AttachNack);
    w.u64v(m.sessionEpoch);
    w.u64v(m.zoneId);
    w.u8v(static_cast<u8>(m.reason));
    return w.take();
}
std::vector<u8> encode(u64 stream, const Detach& m) {
    ByteWriter w = header(stream, TrunkType::Detach);
    w.u64v(m.sessionEpoch);
    w.u8v(static_cast<u8>(m.reason));
    return w.take();
}
std::vector<u8> encode(u64 stream, const Kick& m) {
    ByteWriter w = header(stream, TrunkType::Kick);
    w.u8v(m.reason);
    w.str(m.message);
    return w.take();
}
std::vector<u8> encodeForward(u64 stream, net::Channel channel, std::span<const u8> payload) {
    ByteWriter w = header(stream, TrunkType::Forward);
    w.u8v(net::channelId(channel));
    w.bytes(payload);
    return w.take();
}
std::vector<u8> encodeDeliver(u64 stream, u64 leaseGen, net::Channel channel, std::span<const u8> payload) {
    ByteWriter w = header(stream, TrunkType::Deliver);
    w.varint(leaseGen);
    w.u8v(net::channelId(channel));
    w.bytes(payload);
    return w.take();
}

Result<Hello> decodeHello(std::span<const u8> body) {
    ByteReader r(body);
    Hello m;
    m.version = r.u8v();
    m.processId = r.u64v();
    m.name = r.str(64);
    return finish(r, std::move(m), "Hello");
}
Result<Welcome> decodeWelcome(std::span<const u8> body) {
    ByteReader r(body);
    Welcome m;
    m.version = r.u8v();
    m.processId = r.u64v();
    m.epoch = r.u64v();
    m.name = r.str(64);
    return finish(r, std::move(m), "Welcome");
}
Result<ZoneFenced> decodeZoneFenced(std::span<const u8> body) {
    ByteReader r(body);
    ZoneFenced m;
    m.zoneId = r.u64v();
    m.leaseGen = r.varint();
    return finish(r, m, "ZoneFenced");
}
Result<Attach> decodeAttach(std::span<const u8> body) {
    ByteReader r(body);
    Attach m;
    m.sessionEpoch = r.u64v();
    m.zoneId = r.u64v();
    m.accountId = r.u64v();
    m.characterId = r.u64v();
    m.flags = r.u8v();
    return finish(r, m, "Attach");
}
Result<AttachAck> decodeAttachAck(std::span<const u8> body) {
    ByteReader r(body);
    AttachAck m;
    m.sessionEpoch = r.u64v();
    m.zoneId = r.u64v();
    m.leaseGen = r.varint();
    m.tick = r.u64v();
    m.tickHz = r.u32v();
    m.dilationPpm = r.u32v();
    m.zoneName = r.str(64);
    return finish(r, std::move(m), "AttachAck");
}
Result<AttachNack> decodeAttachNack(std::span<const u8> body) {
    ByteReader r(body);
    AttachNack m;
    m.sessionEpoch = r.u64v();
    m.zoneId = r.u64v();
    const u8 reason = r.u8v();
    if (reason < 1 || reason > 4) return malformed("AttachNack reason");
    m.reason = static_cast<NackReason>(reason);
    return finish(r, m, "AttachNack");
}
Result<Detach> decodeDetach(std::span<const u8> body) {
    ByteReader r(body);
    Detach m;
    m.sessionEpoch = r.u64v();
    const u8 reason = r.u8v();
    if (reason < 1 || reason > 5) return malformed("Detach reason");
    m.reason = static_cast<DetachReason>(reason);
    return finish(r, m, "Detach");
}
Result<Kick> decodeKick(std::span<const u8> body) {
    ByteReader r(body);
    Kick m;
    m.reason = r.u8v();
    m.message = r.str(256);
    return finish(r, std::move(m), "Kick");
}
Result<Forward> decodeForward(std::span<const u8> body) {
    ByteReader r(body);
    Forward m;
    const u8 c = r.u8v();
    if (r.fail() || !validChannel(c)) return malformed("Forward channel");
    m.channel = static_cast<net::Channel>(c);
    m.payload = r.rest();
    return m;
}
Result<Deliver> decodeDeliver(std::span<const u8> body) {
    ByteReader r(body);
    Deliver m;
    m.leaseGen = r.varint();
    const u8 c = r.u8v();
    if (r.fail() || !validChannel(c)) return malformed("Deliver channel");
    m.channel = static_cast<net::Channel>(c);
    m.payload = r.rest();
    return m;
}

net::Channel trunkChannelFor(net::Channel clientChannel) noexcept {
    return net::isReliable(clientChannel) ? net::Channel::EventReliable : net::Channel::EventUnreliable;
}

// ---------------------------------------------------------------------------------------------
// Client CONTROL
// ---------------------------------------------------------------------------------------------

std::string_view kickReasonName(KickReason r) noexcept {
    switch (r) {
    case KickReason::None: return "none";
    case KickReason::Superseded: return "superseded";
    case KickReason::Logout: return "logout";
    case KickReason::SessionEnded: return "session_ended";
    case KickReason::ZoneUnavailable: return "zone_unavailable";
    case KickReason::Malformed: return "malformed";
    case KickReason::Shutdown: return "shutdown";
    case KickReason::Cell: return "cell";
    case KickReason::BadToken: return "bad_token";
    }
    return "?";
}

KickReason kickReasonFromString(std::string_view reason) noexcept {
    if (reason == "superseded" || reason == "reconnect") return KickReason::Superseded;
    if (reason == "logout") return KickReason::Logout;
    if (reason == "banned" || reason == "ban") return KickReason::SessionEnded;
    return KickReason::SessionEnded;
}

std::vector<u8> encode(const ClientWelcome& m) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::Welcome));
    w.u64v(m.sessionId);
    w.u64v(m.sessionEpoch);
    w.u64v(m.zoneId);
    w.u64v(m.tick);
    w.u32v(m.tickHz);
    w.u32v(m.dilationPpm);
    w.str(m.zoneName);
    return w.take();
}
std::vector<u8> encode(const ClientReconnectTicket& m) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::ReconnectTicket));
    w.i64v(m.expiresAtUnixMs);
    w.str(m.ticket);
    return w.take();
}
std::vector<u8> encode(const ClientTimeDilation& m) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::TimeDilation));
    w.u64v(m.effectiveTick);
    w.u32v(m.dilationPpm);
    return w.take();
}
std::vector<u8> encode(const ClientKick& m) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::Kick));
    w.u8v(static_cast<u8>(m.reason));
    w.str(m.message);
    return w.take();
}
std::vector<u8> encode(const ClientRouteState& m) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::RouteState));
    w.u8v(m.state);
    w.u64v(m.zoneId);
    return w.take();
}
std::vector<u8> encodePing(u64 nonce) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::Ping));
    w.u64v(nonce);
    return w.take();
}
std::vector<u8> encodePong(u64 nonce) {
    ByteWriter w;
    w.u8v(static_cast<u8>(ClientControlType::Pong));
    w.u64v(nonce);
    return w.take();
}

std::optional<ClientControlType> clientControlType(std::span<const u8> payload) noexcept {
    if (payload.empty()) return std::nullopt;
    const u8 t = payload[0];
    if ((t >= 0x01 && t <= 0x05) || t == 0x10 || t == 0x11) return static_cast<ClientControlType>(t);
    return std::nullopt;
}

namespace {
ByteReader controlBody(std::span<const u8> payload, ClientControlType type, bool& ok) {
    ok = !payload.empty() && payload[0] == static_cast<u8>(type);
    return ByteReader(ok ? payload.subspan(1) : std::span<const u8>{});
}
} // namespace

Result<ClientWelcome> decodeClientWelcome(std::span<const u8> payload) {
    bool ok = false;
    ByteReader r = controlBody(payload, ClientControlType::Welcome, ok);
    if (!ok) return malformed("Welcome");
    ClientWelcome m;
    m.sessionId = r.u64v();
    m.sessionEpoch = r.u64v();
    m.zoneId = r.u64v();
    m.tick = r.u64v();
    m.tickHz = r.u32v();
    m.dilationPpm = r.u32v();
    m.zoneName = r.str(64);
    return finish(r, std::move(m), "Welcome");
}
Result<ClientReconnectTicket> decodeClientReconnectTicket(std::span<const u8> payload) {
    bool ok = false;
    ByteReader r = controlBody(payload, ClientControlType::ReconnectTicket, ok);
    if (!ok) return malformed("ReconnectTicket");
    ClientReconnectTicket m;
    m.expiresAtUnixMs = r.i64v();
    m.ticket = r.str(4096);
    return finish(r, std::move(m), "ReconnectTicket");
}
Result<ClientTimeDilation> decodeClientTimeDilation(std::span<const u8> payload) {
    bool ok = false;
    ByteReader r = controlBody(payload, ClientControlType::TimeDilation, ok);
    if (!ok) return malformed("TimeDilation");
    ClientTimeDilation m;
    m.effectiveTick = r.u64v();
    m.dilationPpm = r.u32v();
    return finish(r, m, "TimeDilation");
}
Result<ClientKick> decodeClientKick(std::span<const u8> payload) {
    bool ok = false;
    ByteReader r = controlBody(payload, ClientControlType::Kick, ok);
    if (!ok) return malformed("Kick");
    ClientKick m;
    m.reason = static_cast<KickReason>(r.u8v());
    m.message = r.str(256);
    return finish(r, std::move(m), "Kick");
}
Result<ClientRouteState> decodeClientRouteState(std::span<const u8> payload) {
    bool ok = false;
    ByteReader r = controlBody(payload, ClientControlType::RouteState, ok);
    if (!ok) return malformed("RouteState");
    ClientRouteState m;
    m.state = r.u8v();
    m.zoneId = r.u64v();
    return finish(r, m, "RouteState");
}
Result<ClientPing> decodePingOrPong(std::span<const u8> payload) {
    if (payload.empty() || (payload[0] != static_cast<u8>(ClientControlType::Ping) &&
                            payload[0] != static_cast<u8>(ClientControlType::Pong)))
        return malformed("Ping");
    ByteReader r(payload.subspan(1));
    ClientPing m;
    m.nonce = r.u64v();
    return finish(r, m, "Ping");
}

// ---------------------------------------------------------------------------------------------
// Dev game messages
// ---------------------------------------------------------------------------------------------

std::vector<u8> encodeEchoRequest(u32 seq, std::span<const u8> data) {
    ByteWriter w;
    w.u8v(kEchoRequest);
    w.u32v(seq);
    w.bytes(data);
    return w.take();
}
std::vector<u8> encodeEchoReply(u32 seq, u64 tick, std::span<const u8> data) {
    ByteWriter w;
    w.u8v(kEchoReply);
    w.u32v(seq);
    w.u64v(tick);
    w.bytes(data);
    return w.take();
}
std::vector<u8> encode(const TickState& m) {
    ByteWriter w;
    w.u8v(kTickState);
    w.u64v(m.tick);
    w.u32v(m.dilationPpm);
    w.u64v(m.gameTimeNs);
    w.u32v(m.sessions);
    return w.take();
}
Result<EchoRequest> decodeEchoRequest(std::span<const u8> payload) {
    if (payload.empty() || payload[0] != kEchoRequest) return malformed("EchoRequest");
    ByteReader r(payload.subspan(1));
    EchoRequest m;
    m.seq = r.u32v();
    m.data = r.rest();
    if (r.fail()) return malformed("EchoRequest");
    return m;
}
Result<EchoReply> decodeEchoReply(std::span<const u8> payload) {
    if (payload.empty() || payload[0] != kEchoReply) return malformed("EchoReply");
    ByteReader r(payload.subspan(1));
    EchoReply m;
    m.seq = r.u32v();
    m.tick = r.u64v();
    m.data = r.rest();
    if (r.fail()) return malformed("EchoReply");
    return m;
}
Result<TickState> decodeTickState(std::span<const u8> payload) {
    if (payload.empty() || payload[0] != kTickState) return malformed("TickState");
    ByteReader r(payload.subspan(1));
    TickState m;
    m.tick = r.u64v();
    m.dilationPpm = r.u32v();
    m.gameTimeNs = r.u64v();
    m.sessions = r.u32v();
    return finish(r, m, "TickState");
}

// ---------------------------------------------------------------------------------------------
// User data
// ---------------------------------------------------------------------------------------------

namespace {
u64 le64(const u8* p) noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
    return v;
}
void put64(u8* p, u64 v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
} // namespace

Result<SessionUserData> parseUserData(const net::UserData& data) {
    if (data[0] != kUserDataVersion1) return makeError(ErrorCode::ParseError, "user data layout version {}", data[0]);
    SessionUserData u;
    u.flags = data[1];
    u.accountId = le64(&data[8]);
    u.characterId = le64(&data[16]);
    u.sessionEpoch = le64(&data[24]);
    u.contentBuild = le64(&data[32]);
    u.zoneId = le64(&data[40]);
    u.placementTicket = le64(&data[48]);
    u.entitlements = le64(&data[56]);
    std::memcpy(u.attestationHash.data(), &data[64], 32);
    return u;
}

net::UserData writeUserData(const SessionUserData& u) {
    net::UserData d{};
    d[0] = kUserDataVersion1;
    d[1] = u.flags;
    put64(&d[8], u.accountId);
    put64(&d[16], u.characterId);
    put64(&d[24], u.sessionEpoch);
    put64(&d[32], u.contentBuild);
    put64(&d[40], u.zoneId);
    put64(&d[48], u.placementTicket);
    put64(&d[56], u.entitlements);
    std::memcpy(&d[64], u.attestationHash.data(), 32);
    return d;
}

net::UserData writeTrunkUserData(u8 kind, u64 processId) {
    net::UserData d{};
    d[0] = kUserDataVersion1;
    d[1] = kind;
    put64(&d[8], processId);
    return d;
}

} // namespace helios::server::proto
