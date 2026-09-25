#pragma once
// IBus: the control-plane message bus seen by cells and gateways (05 §2.1, §2.4): core NATS
// request/reply to Go services (`rpc.<shard>.<svc>.<Method>`, the service name as queue group),
// subscriptions to control subjects (`ctl.<shard>.gateway.all.kick`, …) and publishes.
//
// Implementations:
//   * NatsBus (nats_bus.h): nats.c 3.14 against the backend's embedded or a real NATS server;
//   * FakeBus (fake_bus.h): in-process subject routing for unit tests and single-process runs.
//
// Payloads are the Go services' JSON until schemac emits Helios-binary codecs (services/README:
// "JSON payloads until schemac emits Helios-binary codecs"). Requests carry `Helios-Deadline-Ms`;
// service errors come back as the headers `Helios-Error` (a Connect code such as
// "failed_precondition") and `Helios-Error-Message`.
//
// Nothing on a simulation thread ever blocks on the bus: request() returns at once and its reply
// callback runs later on a bus thread (FakeBus: inside pump()). Callers marshal results onto their
// own thread (a queue drained at the next update/tick).
//
// Threading: implementations are thread-safe. Callbacks run on bus threads and must not block for
// long; they may call request()/publish() but must not destroy the bus.

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::server {

/// NATS header names used by the Helios RPC convention (05 §2.1; Go pkg/rpc).
inline constexpr std::string_view kHeaderDeadlineMs = "Helios-Deadline-Ms";
inline constexpr std::string_view kHeaderError = "Helios-Error";
inline constexpr std::string_view kHeaderErrorMessage = "Helios-Error-Message";
inline constexpr std::string_view kHeaderIdem = "Helios-Idem";

using BusHeaders = std::vector<std::pair<std::string, std::string>>;

struct BusMessage {
    std::string subject;
    std::string reply;   ///< Reply subject of a request ("" for a plain publish).
    BusHeaders headers;
    std::vector<u8> data;

    /// First value of header `name` (case-sensitive, as NATS headers are), or "".
    std::string_view header(std::string_view name) const noexcept;
};

enum class BusStatus : u8 {
    Ok,            ///< A reply arrived (it may still carry a Helios-Error; see BusReply).
    Timeout,       ///< No reply before the deadline.
    NoResponders,  ///< Nobody serves the subject (the service is down or not leader).
    Disconnected,  ///< The bus connection is down.
    Error,         ///< Any other transport failure.
};
std::string_view busStatusName(BusStatus status) noexcept;

struct BusReply {
    BusStatus status = BusStatus::Ok;
    BusMessage message;
    std::string transportError; ///< Detail for non-Ok statuses.

    /// The service's error code (Helios-Error header), "" when the call succeeded.
    std::string_view errorCode() const noexcept { return message.header(kHeaderError); }
    std::string_view errorMessage() const noexcept { return message.header(kHeaderErrorMessage); }
    /// Transport Ok and no service error.
    bool ok() const noexcept { return status == BusStatus::Ok && errorCode().empty(); }
};

using BusReplyFn = std::function<void(BusReply)>;
using BusMessageFn = std::function<void(const BusMessage&)>;

class IBus {
public:
    virtual ~IBus() = default;

    /// Sends a request; `onReply` is called exactly once, on a bus thread, with the reply or the
    /// failure. `Helios-Deadline-Ms` is added from `timeout` unless present.
    virtual void request(std::string subject, std::vector<u8> payload, BusHeaders headers,
                         std::chrono::milliseconds timeout, BusReplyFn onReply) = 0;
    /// Subscribes to `subject` (NATS wildcards `*` and `>` allowed). Returns a subscription id.
    virtual Result<u64> subscribe(std::string subject, BusMessageFn onMessage) = 0;
    virtual void unsubscribe(u64 subscriptionId) = 0;
    virtual Result<void> publish(std::string subject, std::vector<u8> payload, BusHeaders headers = {}) = 0;
    virtual bool isConnected() const = 0;
};

/// NATS subject matching: tokens split on '.', `*` matches one token, a final `>` one or more.
bool subjectMatches(std::string_view pattern, std::string_view subject) noexcept;

} // namespace helios::server
