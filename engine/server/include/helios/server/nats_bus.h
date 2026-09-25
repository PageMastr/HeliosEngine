#pragma once
// NatsBus: IBus over nats.c 3.14 (ADR-013), the cell's and gateway's link to the Go services.
//
// Connects as the backend's `fleet` user (HELIOS_NATS_USER / HELIOS_NATS_PASSWORD, or the
// password read from keys/nats-fleet.json). The connection retries forever in the background
// (RetryOnFailedConnect + unlimited reconnects), so a cell starts and keeps simulating while the
// control plane is down (holder rule, 05 §1.4.2); requests made meanwhile fail fast with
// Disconnected and the callers retry on their own schedule.
//
// request() hands the blocking natsConnection_RequestMsg to a small pool of request threads, so
// callers never block; the reply callback runs on that pool. Subscriptions run on nats.c's
// delivery threads. nats.h is private to this file's implementation (no third-party types in
// public headers, 02 §1.1).
//
// Threading: thread-safe. Destroying the bus closes the connection (in-flight requests complete
// with Disconnected or Timeout) and waits for callbacks to finish.

#include <memory>
#include <string>

#include "helios/server/bus.h"

namespace helios::server {

struct NatsBusConfig {
    std::string url = "nats://127.0.0.1:4222";
    std::string user;
    std::string password;
    std::string name = "helios";       ///< Connection name shown by the NATS server.
    u32 requestThreads = 2;
    i64 connectTimeoutMs = 2000;
    i64 reconnectWaitMs = 500;
    /// Wait up to this long for the first connection before returning (0 = do not wait; the
    /// connection completes in the background).
    i64 initialConnectWaitMs = 2000;
};

class NatsBus final : public IBus {
public:
    static Result<std::unique_ptr<NatsBus>> connect(const NatsBusConfig& config);
    ~NatsBus() override;
    NatsBus(const NatsBus&) = delete;
    NatsBus& operator=(const NatsBus&) = delete;

    void request(std::string subject, std::vector<u8> payload, BusHeaders headers, std::chrono::milliseconds timeout,
                 BusReplyFn onReply) override;
    Result<u64> subscribe(std::string subject, BusMessageFn onMessage) override;
    void unsubscribe(u64 subscriptionId) override;
    Result<void> publish(std::string subject, std::vector<u8> payload, BusHeaders headers = {}) override;
    bool isConnected() const override;

    struct Impl;

private:
    explicit NatsBus(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

} // namespace helios::server
