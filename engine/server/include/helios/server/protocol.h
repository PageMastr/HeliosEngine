#pragma once
// Phase 0 wire messages of the cell/gateway skeleton, hand-written until schemac emits them from
// schemas/net/*.hschema (04 §11.1). All integers are little-endian, lengths and generations are
// canonical LEB128 varints (net::wire), strings are varint length + UTF-8. Every decoder is
// bounds-checked and returns an error on malformed input; nothing here asserts on peer data.
//
// **Trunk** (gateway <-> cell, 04 §2.6). Each trunk message is `varint stream_id` (0 = the trunk
// itself, else the session ID) + `u8 type` + body. Stream-0 management (Hello, Welcome,
// ZoneFenced) rides the trunk's CONTROL channel. Everything for one session (Attach, AttachAck,
// AttachNack, Detach, Kick and reliable Forward/Deliver) rides EVENT_R, so per-session order
// holds end to end; unreliable client traffic rides EVENT_U. Cell -> gateway Deliver carries the
// zone's lease generation, and gateways drop deliveries below the generation they route to
// (receivers fence too, 05 §1.4.2).
//
// **Client** (client <-> gateway): the gateway's own CONTROL messages (Welcome, ReconnectTicket,
// TimeDilation, Kick) plus the Phase 0 dev game messages (Echo on EVENT_R, TickState chunks on
// STATE) that the empty zone answers, so a client can see its route working end to end.
//
// **Connect-token user data** (Go services/pkg/connecttoken/userdata.go layout v1, 256 bytes):
// version, flags, account, character, session_epoch, content_build, zone, placement ticket,
// entitlements, attestation hash.
//
// Threading: pure functions and value types.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/channel.h"
#include "helios/net/connect_token.h"
#include "helios/net/wire.h"

namespace helios::server::proto {

// ---------------------------------------------------------------------------------------------
// Byte codecs
// ---------------------------------------------------------------------------------------------

class ByteWriter {
public:
    void u8v(u8 v) { m_out.push_back(v); }
    void u16v(u16 v);
    void u32v(u32 v);
    void u64v(u64 v);
    void i64v(i64 v) { u64v(static_cast<u64>(v)); }
    void varint(u64 v);
    void str(std::string_view s);
    void bytes(std::span<const u8> b) { m_out.insert(m_out.end(), b.begin(), b.end()); }
    std::vector<u8>& data() noexcept { return m_out; }
    std::vector<u8> take() noexcept { return std::move(m_out); }

private:
    std::vector<u8> m_out;
};

/// Reads from a span; any read past the end sets fail() and returns zeros.
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> in) noexcept : m_in(in) {}
    u8 u8v() noexcept;
    u16 u16v() noexcept;
    u32 u32v() noexcept;
    u64 u64v() noexcept;
    i64 i64v() noexcept { return static_cast<i64>(u64v()); }
    u64 varint() noexcept;
    /// A varint-prefixed string of at most `maxBytes`.
    std::string str(usize maxBytes = 1024) noexcept;
    /// The unread rest.
    std::span<const u8> rest() noexcept;
    bool fail() const noexcept { return m_fail; }
    bool atEnd() const noexcept { return m_pos == m_in.size(); }
    usize remaining() const noexcept { return m_in.size() - m_pos; }

private:
    bool need(usize n) noexcept;
    std::span<const u8> m_in;
    usize m_pos = 0;
    bool m_fail = false;
};

// ---------------------------------------------------------------------------------------------
// Size limits
// ---------------------------------------------------------------------------------------------

/// Largest EVENT_R payload that fits one packet (the per-channel limit of net).
inline constexpr u32 kMaxEventPayload = net::wire::maxPayloadFor(net::Channel::EventReliable, net::wire::kMaxPacketPayload);
/// Trunk framing overhead of a Forward/Deliver: stream id, type, channel, lease generation.
inline constexpr u32 kTrunkOverhead = 24;
/// Largest client message a gateway forwards (and a cell delivers) on one trunk datagram.
inline constexpr u32 kMaxForwardPayload = kMaxEventPayload - kTrunkOverhead;

// ---------------------------------------------------------------------------------------------
// Trunk
// ---------------------------------------------------------------------------------------------

inline constexpr u8 kTrunkVersion = 1;

enum class TrunkType : u8 {
    Hello = 0x01,      ///< gateway -> cell, stream 0
    Welcome = 0x02,    ///< cell -> gateway, stream 0
    ZoneFenced = 0x03, ///< cell -> gateway, stream 0: the cell stopped owning a zone
    Attach = 0x10,     ///< gateway -> cell: bind a session to a zone
    AttachAck = 0x11,  ///< cell -> gateway
    AttachNack = 0x12, ///< cell -> gateway
    Detach = 0x13,     ///< gateway -> cell: the session left this gateway
    Kick = 0x14,       ///< cell -> gateway: drop the session
    Forward = 0x20,    ///< gateway -> cell: a client message
    Deliver = 0x21,    ///< cell -> gateway: a message for the client
};

enum class NackReason : u8 { NotHosted = 1, StaleEpoch = 2, Full = 3, Draining = 4 };
enum class DetachReason : u8 { ClientLeft = 1, Superseded = 2, Kicked = 3, TimedOut = 4, GatewayShutdown = 5 };
std::string_view nackReasonName(NackReason r) noexcept;
std::string_view detachReasonName(DetachReason r) noexcept;

struct Hello {
    u8 version = kTrunkVersion;
    u64 processId = 0;  ///< Orchestrator process id of the gateway (0 standalone).
    std::string name;
};
struct Welcome {
    u8 version = kTrunkVersion;
    u64 processId = 0;  ///< Orchestrator process id of the cell (0 standalone).
    u64 epoch = 0;      ///< Its registration epoch.
    std::string name;
};
struct ZoneFenced {
    u64 zoneId = 0;
    u64 leaseGen = 0;   ///< The generation the cell gave up.
};
struct Attach {
    u64 sessionEpoch = 0;
    u64 zoneId = 0;     ///< 0 = the cell's default zone.
    u64 accountId = 0;
    u64 characterId = 0;
    u8 flags = 0;
};
struct AttachAck {
    u64 sessionEpoch = 0;
    u64 zoneId = 0;
    u64 leaseGen = 0;
    u64 tick = 0;
    u32 tickHz = 0;
    u32 dilationPpm = 0;
    std::string zoneName;
};
struct AttachNack {
    u64 sessionEpoch = 0;
    u64 zoneId = 0;
    NackReason reason = NackReason::NotHosted;
};
struct Detach {
    u64 sessionEpoch = 0;
    DetachReason reason = DetachReason::ClientLeft;
};
struct Kick {
    u8 reason = 0;
    std::string message;
};
struct Forward {
    net::Channel channel = net::Channel::EventReliable;
    std::span<const u8> payload; ///< Points into the decoded buffer.
};
struct Deliver {
    u64 leaseGen = 0;
    net::Channel channel = net::Channel::EventReliable;
    std::span<const u8> payload; ///< Points into the decoded buffer.
};

/// One decoded trunk message; `stream` is the session id (0 for the trunk itself).
struct TrunkMessage {
    u64 stream = 0;
    TrunkType type = TrunkType::Hello;
    std::span<const u8> body;
};
/// Splits stream id and type; the body is decoded with the matching decode*() below.
Result<TrunkMessage> parseTrunk(std::span<const u8> bytes);

std::vector<u8> encode(u64 stream, const Hello& m);
std::vector<u8> encode(u64 stream, const Welcome& m);
std::vector<u8> encode(u64 stream, const ZoneFenced& m);
std::vector<u8> encode(u64 stream, const Attach& m);
std::vector<u8> encode(u64 stream, const AttachAck& m);
std::vector<u8> encode(u64 stream, const AttachNack& m);
std::vector<u8> encode(u64 stream, const Detach& m);
std::vector<u8> encode(u64 stream, const Kick& m);
std::vector<u8> encodeForward(u64 stream, net::Channel channel, std::span<const u8> payload);
std::vector<u8> encodeDeliver(u64 stream, u64 leaseGen, net::Channel channel, std::span<const u8> payload);

Result<Hello> decodeHello(std::span<const u8> body);
Result<Welcome> decodeWelcome(std::span<const u8> body);
Result<ZoneFenced> decodeZoneFenced(std::span<const u8> body);
Result<Attach> decodeAttach(std::span<const u8> body);
Result<AttachAck> decodeAttachAck(std::span<const u8> body);
Result<AttachNack> decodeAttachNack(std::span<const u8> body);
Result<Detach> decodeDetach(std::span<const u8> body);
Result<Kick> decodeKick(std::span<const u8> body);
Result<Forward> decodeForward(std::span<const u8> body);
Result<Deliver> decodeDeliver(std::span<const u8> body);

/// The trunk channel that carries a session message sent to/received from the client on `c`:
/// reliable client channels map to EVENT_R (ordered with Attach/Detach), the rest to EVENT_U.
net::Channel trunkChannelFor(net::Channel clientChannel) noexcept;

// ---------------------------------------------------------------------------------------------
// Client <-> gateway CONTROL
// ---------------------------------------------------------------------------------------------

enum class ClientControlType : u8 {
    Welcome = 0x01,         ///< gateway -> client: routed to a zone
    ReconnectTicket = 0x02, ///< gateway -> client: sealed by the Session service, valid 5 min
    TimeDilation = 0x03,    ///< cell -> client: d from effectiveTick on (04 §3.2)
    Kick = 0x04,            ///< gateway -> client: reason, just before the disconnect
    RouteState = 0x05,      ///< gateway -> client: the route is being re-resolved (cell lost)
    Ping = 0x10,            ///< client -> gateway
    Pong = 0x11,            ///< gateway -> client
};

enum class KickReason : u8 {
    None = 0,
    Superseded = 1,      ///< a newer session_epoch (reconnect elsewhere) or a new login
    Logout = 2,
    SessionEnded = 3,    ///< the Session service no longer knows the session
    ZoneUnavailable = 4, ///< no cell could be found for the zone
    Malformed = 5,       ///< too many malformed messages (04 §9)
    Shutdown = 6,
    Cell = 7,            ///< the cell asked for it
    BadToken = 8,        ///< unreadable connect-token user data
};
std::string_view kickReasonName(KickReason r) noexcept;
/// Maps the Session service's control reasons ("superseded", "logout", "reconnect", …).
KickReason kickReasonFromString(std::string_view reason) noexcept;

struct ClientWelcome {
    u64 sessionId = 0;
    u64 sessionEpoch = 0;
    u64 zoneId = 0;
    u64 tick = 0;
    u32 tickHz = 0;
    u32 dilationPpm = 0;
    std::string zoneName;
};
struct ClientReconnectTicket {
    i64 expiresAtUnixMs = 0;
    std::string ticket;
};
struct ClientTimeDilation {
    u64 effectiveTick = 0;
    u32 dilationPpm = 0;
};
struct ClientKick {
    KickReason reason = KickReason::None;
    std::string message;
};
struct ClientRouteState {
    u8 state = 0; ///< 0 = resolving, 1 = routed
    u64 zoneId = 0;
};
struct ClientPing {
    u64 nonce = 0;
};

std::vector<u8> encode(const ClientWelcome& m);
std::vector<u8> encode(const ClientReconnectTicket& m);
std::vector<u8> encode(const ClientTimeDilation& m);
std::vector<u8> encode(const ClientKick& m);
std::vector<u8> encode(const ClientRouteState& m);
std::vector<u8> encodePing(u64 nonce);
std::vector<u8> encodePong(u64 nonce);

/// The control type of a CONTROL payload (first byte), or nullopt when empty.
std::optional<ClientControlType> clientControlType(std::span<const u8> payload) noexcept;
Result<ClientWelcome> decodeClientWelcome(std::span<const u8> payload);
Result<ClientReconnectTicket> decodeClientReconnectTicket(std::span<const u8> payload);
Result<ClientTimeDilation> decodeClientTimeDilation(std::span<const u8> payload);
Result<ClientKick> decodeClientKick(std::span<const u8> payload);
Result<ClientRouteState> decodeClientRouteState(std::span<const u8> payload);
Result<ClientPing> decodePingOrPong(std::span<const u8> payload);

// ---------------------------------------------------------------------------------------------
// Phase 0 dev game messages (answered by every zone)
// ---------------------------------------------------------------------------------------------

inline constexpr u8 kEchoRequest = 0xE0; ///< EVENT_R c->s: u32 seq, bytes
inline constexpr u8 kEchoReply = 0xE1;   ///< EVENT_R s->c: u32 seq, u64 tick, bytes
inline constexpr u8 kTickState = 0x51;   ///< STATE s->c: u64 tick, u32 dilation ppm, u64 game ns, u32 sessions

struct EchoRequest {
    u32 seq = 0;
    std::span<const u8> data;
};
struct EchoReply {
    u32 seq = 0;
    u64 tick = 0;
    std::span<const u8> data;
};
struct TickState {
    u64 tick = 0;
    u32 dilationPpm = 0;
    u64 gameTimeNs = 0;
    u32 sessions = 0;
};

std::vector<u8> encodeEchoRequest(u32 seq, std::span<const u8> data);
std::vector<u8> encodeEchoReply(u32 seq, u64 tick, std::span<const u8> data);
std::vector<u8> encode(const TickState& m);
Result<EchoRequest> decodeEchoRequest(std::span<const u8> payload);
Result<EchoReply> decodeEchoReply(std::span<const u8> payload);
Result<TickState> decodeTickState(std::span<const u8> payload);

// ---------------------------------------------------------------------------------------------
// Connect-token user data (Go layout v1)
// ---------------------------------------------------------------------------------------------

inline constexpr u8 kUserDataVersion1 = 1;
inline constexpr u8 kUserFlagReconnect = 1u << 0;
inline constexpr u8 kUserFlagBot = 1u << 1;

struct SessionUserData {
    u8 flags = 0;
    u64 accountId = 0;
    u64 characterId = 0;
    u64 sessionEpoch = 0;
    u64 contentBuild = 0;
    u64 zoneId = 0;
    u64 placementTicket = 0;
    u64 entitlements = 0;
    std::array<u8, 32> attestationHash{};
};

/// Decodes the v1 layout (ParseError for another version byte). Reserved bytes are ignored.
Result<SessionUserData> parseUserData(const net::UserData& data);
net::UserData writeUserData(const SessionUserData& data);

/// Trunk tokens carry who opened the trunk: kind (1 = gateway) and its orchestrator process id.
inline constexpr u8 kTrunkPeerGateway = 1;
net::UserData writeTrunkUserData(u8 kind, u64 processId);

} // namespace helios::server::proto
