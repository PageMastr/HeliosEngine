#pragma once
// Control-plane messages exchanged with the Go services over NATS (05 §1.3–1.4, §2): the
// orchestrator's register / heartbeat / ID-block / directory calls and the Session service's
// reconnect-ticket sealing and gateway control messages.
//
// The encoding matches services/internal/orchestrator and services/internal/session exactly:
// JSON with lowerCamelCase names, 64-bit integers as JSON *strings* (Go `,string` tags, proto3 JSON
// mapping), `omitempty` fields left out when empty, and times as RFC 3339 strings. Decoders accept
// 64-bit values as strings or numbers and ignore unknown fields (Go's json.Unmarshal does too), so
// a newer backend can add fields. Errors travel in NATS headers (bus.h), not in the body.
//
//   rpc.<shard>.orch.RegisterProcess      ProcessInfo              -> RegisterResult
//   rpc.<shard>.orch.Heartbeat            HeartbeatRequest         -> HeartbeatResult
//                                         (failed_precondition "lease_lost": register again)
//   rpc.<shard>.orch.AllocateIdBlocks     AllocateIdBlocksRequest  -> AllocateIdBlocksResponse
//   rpc.<shard>.orch.Deregister           DeregisterRequest        -> {}
//   rpc.<shard>.orch.ResolveZone          ResolveZoneRequest       -> Route
//   rpc.<shard>.session.SealReconnectTickets  SealRequest          -> SealResponse
//   ctl.<shard>.gateway.all.kick          ControlMessage{sessionId, reason}
//   ctl.<shard>.gateway.all.session_epoch ControlMessage{sessionId, epoch}
//
// Threading: pure functions and value types.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::server::orch {

inline constexpr std::string_view kKindCell = "cell";
inline constexpr std::string_view kKindGateway = "gateway";
/// Heartbeat reply mode (degraded mode arrives in Phase 2, 05 §1.4.4).
inline constexpr std::string_view kModeNormal = "normal";
/// Connect error code names used by the services (Go pkg/rpc).
inline constexpr std::string_view kCodeFailedPrecondition = "failed_precondition";
inline constexpr std::string_view kCodeUnavailable = "unavailable";
inline constexpr std::string_view kCodeNotFound = "not_found";
inline constexpr std::string_view kCodeInvalidArgument = "invalid_argument";
inline constexpr std::string_view kCodePermissionDenied = "permission_denied";
inline constexpr std::string_view kLeaseLost = "lease_lost";

std::string orchestratorSubject(std::string_view shard, std::string_view method);
std::string sealTicketsSubject(std::string_view shard);
/// ctl.<shard>.gateway.all.<verb> ("kick", "session_epoch").
std::string gatewayControlSubject(std::string_view shard, std::string_view verb);

struct ProcessInfo {
    std::string name;
    std::string kind;              ///< "cell" | "gateway"
    std::string address;           ///< gateway: public UDP ip:port; cell: trunk address
    std::string host;
    i64 pid = 0;
    std::string version;
    std::vector<std::string> zones; ///< cell: zone names it serves (empty = any)
    u32 keyId = 0;                 ///< gateway: shard netcode key generation
    i64 capacity = 0;              ///< gateway: session slots
};

struct Assignment {
    u64 zoneId = 0;
    std::string zoneName;
    u64 leaseGen = 0;
};

struct RegisterResult {
    u64 processId = 0;
    u64 epoch = 0;
    i64 leaseTtlMs = 0;
    i64 heartbeatIntervalMs = 0;
    std::vector<Assignment> assignments;
    u32 idShard = 0;
    std::vector<u64> idBlocks;
};

struct Load {
    i64 players = 0;
    i64 freeSlots = 0;
    f64 tickP99Ms = 0.0;
};

struct HeartbeatRequest {
    u64 processId = 0;
    u64 epoch = 0;
    Load load;
};

struct HeartbeatResult {
    std::string leaseExpires; ///< RFC 3339
    std::vector<Assignment> assignments;
    std::string mode;
};

struct AllocateIdBlocksRequest {
    u64 processId = 0;
    u64 epoch = 0;
    u32 n = 0;
};

struct AllocateIdBlocksResponse {
    u32 idShard = 0;
    std::vector<u64> prefixes;
};

struct DeregisterRequest {
    u64 processId = 0;
    u64 epoch = 0;
};

struct ResolveZoneRequest {
    u64 zoneId = 0;       ///< 0 = by name
    std::string zoneName;
};

struct Route {
    u64 zoneId = 0;
    std::string zoneName;
    u64 leaseGen = 0;
    u64 processId = 0;
    std::string process;
    u64 epoch = 0;
    std::string address;  ///< The owning cell's trunk address.
};

struct SealItem {
    u64 sessionId = 0;
    u64 zoneId = 0;
    u64 epoch = 0;        ///< The session_epoch this gateway serves (0 = unknown).
    std::string ticket;   ///< The latest ticket held (rebuilds a record lost with Valkey).
};

struct SealRequest {
    std::vector<SealItem> sessions;
};

struct SealedTicket {
    u64 sessionId = 0;
    std::string ticket;
    std::string expiresAt; ///< RFC 3339
};

struct SealResponse {
    std::vector<SealedTicket> tickets;
    std::vector<u64> missing; ///< Ended, superseded or unknown sessions: drop them.
};

struct ControlMessage {
    u64 sessionId = 0;
    u64 epoch = 0;
    std::string reason;
};

/// The Session service's batch limit (MaxTicketBatch).
inline constexpr usize kMaxTicketBatch = 1024;

std::vector<u8> encode(const ProcessInfo& m);
std::vector<u8> encode(const RegisterResult& m);
std::vector<u8> encode(const HeartbeatRequest& m);
std::vector<u8> encode(const HeartbeatResult& m);
std::vector<u8> encode(const AllocateIdBlocksRequest& m);
std::vector<u8> encode(const AllocateIdBlocksResponse& m);
std::vector<u8> encode(const DeregisterRequest& m);
std::vector<u8> encode(const ResolveZoneRequest& m);
std::vector<u8> encode(const Route& m);
std::vector<u8> encode(const SealRequest& m);
std::vector<u8> encode(const SealResponse& m);
std::vector<u8> encode(const ControlMessage& m);

Result<ProcessInfo> decodeProcessInfo(std::span<const u8> json);
Result<RegisterResult> decodeRegisterResult(std::span<const u8> json);
Result<HeartbeatRequest> decodeHeartbeatRequest(std::span<const u8> json);
Result<HeartbeatResult> decodeHeartbeatResult(std::span<const u8> json);
Result<AllocateIdBlocksRequest> decodeAllocateIdBlocksRequest(std::span<const u8> json);
Result<AllocateIdBlocksResponse> decodeAllocateIdBlocksResponse(std::span<const u8> json);
Result<DeregisterRequest> decodeDeregisterRequest(std::span<const u8> json);
Result<ResolveZoneRequest> decodeResolveZoneRequest(std::span<const u8> json);
Result<Route> decodeRoute(std::span<const u8> json);
Result<SealRequest> decodeSealRequest(std::span<const u8> json);
Result<SealResponse> decodeSealResponse(std::span<const u8> json);
Result<ControlMessage> decodeControlMessage(std::span<const u8> json);

/// Parses an RFC 3339 timestamp ("2026-09-25T12:00:00.123456789Z", "…+02:00") to Unix ms.
Result<i64> parseRfc3339UnixMs(std::string_view text);
/// Formats Unix ms as RFC 3339 in UTC with millisecond precision ("2026-09-25T12:00:00.123Z").
std::string formatRfc3339UnixMs(i64 unixMs);

inline std::vector<u8> bytesOf(std::string_view s) { return std::vector<u8>(s.begin(), s.end()); }
inline std::string_view textOf(std::span<const u8> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

} // namespace helios::server::orch
