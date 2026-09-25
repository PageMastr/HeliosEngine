#pragma once
// FakeBus: an in-process IBus with NATS subject semantics, for unit tests and single-process runs
// (a cell and a gateway in one test, the fake orchestrator in tests/).
//
// Nothing is delivered from inside request()/publish(): messages queue up and pump() delivers
// them (subscribers first, then replies to the requesters), so callbacks never run re-entrantly
// inside a caller's lock and tests control exactly when "the network" moves. A request with no
// matching subscriber fails with NoResponders; one that is not answered by its deadline fails
// with Timeout when pump() sees the deadline pass. setConnected(false) models a partition: new
// requests fail with Disconnected and nothing is delivered.
//
// Time is the caller's (pump(nowMs)); a FakeBus never reads a clock.
//
// Threading: thread-safe (one mutex); callbacks run on the pump() caller, outside the lock.

#include <deque>
#include <map>
#include <mutex>

#include "helios/server/bus.h"

namespace helios::server {

class FakeBus final : public IBus {
public:
    FakeBus();
    ~FakeBus() override;

    void request(std::string subject, std::vector<u8> payload, BusHeaders headers, std::chrono::milliseconds timeout,
                 BusReplyFn onReply) override;
    Result<u64> subscribe(std::string subject, BusMessageFn onMessage) override;
    void unsubscribe(u64 subscriptionId) override;
    Result<void> publish(std::string subject, std::vector<u8> payload, BusHeaders headers = {}) override;
    bool isConnected() const override;

    /// Delivers queued messages and replies, and times out overdue requests. Returns how many
    /// callbacks ran. Messages queued by those callbacks are delivered in the same call (up to
    /// `maxRounds` passes), so one pump() completes a request/reply round trip.
    u32 pump(i64 nowMs, u32 maxRounds = 8);
    /// Simulates a partition (false): requests fail with Disconnected, queued traffic is dropped.
    void setConnected(bool connected);
    /// Current time of the bus (the last pump()); used to stamp request deadlines.
    i64 nowMs() const;

    /// Messages published on subjects matching `pattern` since the start (publish() and replies).
    usize publishedCount(std::string_view pattern) const;
    usize pendingRequests() const;

private:
    struct Sub {
        std::string pattern;
        BusMessageFn fn;
    };
    struct Pending {
        BusReplyFn onReply;
        i64 deadlineMs = 0;
    };
    struct Queued {
        BusMessage message;
    };

    mutable std::mutex m_mutex;
    std::map<u64, Sub> m_subs;
    std::map<std::string, Pending> m_pending; // by reply inbox
    std::deque<Queued> m_queue;
    std::vector<BusReplyFn> m_failed; // requests that fail with Disconnected at the next pump
    std::vector<std::string> m_published;
    u64 m_nextSub = 1;
    u64 m_nextInbox = 1;
    i64 m_nowMs = 0;
    bool m_connected = true;
};

} // namespace helios::server
