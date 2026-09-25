#include "helios/server/orchestrator_client.h"

#include <algorithm>
#include <condition_variable>

#include "server_log.h"

namespace helios::server {

struct OrchestratorClient::Shared {
    enum class Kind : u8 { Register, Heartbeat };
    struct Reply {
        Kind kind;
        u64 seq;
        BusReply reply;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Reply> replies;
    // Thread-safe view of the registration for allocateIdBlocks() and the ID source.
    bool registered = false;
    u64 processId = 0;
    u64 epoch = 0;
    u32 idShard = 0;
    std::vector<u64> pool; // registration block prefixes, ascending
    bool deregisterDone = false;
};

namespace {
std::vector<authority::RegionAssignment> toRegions(const std::vector<orch::Assignment>& list) {
    std::vector<authority::RegionAssignment> out;
    out.reserve(list.size());
    for (const orch::Assignment& a : list)
        out.push_back(authority::RegionAssignment{authority::RegionId{a.zoneId}, a.zoneName, a.leaseGen});
    return out;
}

std::string describe(const BusReply& r) {
    if (r.status != BusStatus::Ok) return std::string(busStatusName(r.status)) + (r.transportError.empty() ? "" : ": " + r.transportError);
    return std::string(r.errorCode()) + ": " + std::string(r.errorMessage());
}

Error rpcError(const BusReply& r) {
    ErrorCode code = ErrorCode::IoError;
    if (r.status == BusStatus::Timeout) code = ErrorCode::Timeout;
    else if (r.errorCode() == orch::kCodeNotFound) code = ErrorCode::NotFound;
    else if (r.errorCode() == orch::kCodeInvalidArgument) code = ErrorCode::InvalidArgument;
    else if (r.errorCode() == orch::kCodePermissionDenied) code = ErrorCode::PermissionDenied;
    else if (r.errorCode() == orch::kCodeFailedPrecondition) code = ErrorCode::InvalidState;
    return Error{code, describe(r)};
}
} // namespace

OrchestratorClient::OrchestratorClient(IBus& bus, OrchestratorClientConfig config)
    : m_bus(bus), m_config(std::move(config)), m_shared(std::make_shared<Shared>()) {
    m_backoffMs = std::max<i64>(1, m_config.backoffMinMs);
}

OrchestratorClient::~OrchestratorClient() = default;

void OrchestratorClient::setLoadProvider(std::function<orch::Load()> provider) {
    m_load = std::move(provider);
}

u32 OrchestratorClient::idShard() const noexcept {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->idShard;
}

std::vector<authority::LeaseEvent> OrchestratorClient::update(i64 nowNs) {
    std::vector<authority::LeaseEvent> events;
    std::vector<Shared::Reply> replies;
    {
        std::lock_guard lock(m_shared->mutex);
        replies.swap(m_shared->replies);
    }
    for (Shared::Reply& r : replies) {
        if (r.kind == Shared::Kind::Register) applyRegister(r.seq, std::move(r.reply), nowNs, events);
        else applyHeartbeat(r.seq, std::move(r.reply), nowNs, events);
    }
    if (!m_registered && !m_registerInFlight && nowNs >= m_nextRegisterNs) sendRegister(nowNs);
    if (m_registered && !m_heartbeatInFlight && nowNs >= m_nextHeartbeatNs) sendHeartbeat(nowNs);
    return events;
}

void OrchestratorClient::sendRegister(i64 nowNs) {
    (void)nowNs;
    m_registerInFlight = true;
    const u64 seq = ++m_seq;
    std::weak_ptr<Shared> shared = m_shared;
    m_bus.request(orch::orchestratorSubject(m_config.shard, "RegisterProcess"), orch::encode(m_config.info), {},
                  std::chrono::milliseconds(m_config.requestTimeoutMs), [shared, seq](BusReply reply) {
                      if (auto s = shared.lock()) {
                          std::lock_guard lock(s->mutex);
                          s->replies.push_back(Shared::Reply{Shared::Kind::Register, seq, std::move(reply)});
                      }
                  });
}

void OrchestratorClient::sendHeartbeat(i64 nowNs) {
    m_heartbeatInFlight = true;
    m_nextHeartbeatNs = nowNs + m_heartbeatIntervalNs;
    orch::HeartbeatRequest req;
    req.processId = m_processId;
    req.epoch = m_epoch;
    if (m_load) req.load = m_load();
    const u64 seq = m_seq;
    std::weak_ptr<Shared> shared = m_shared;
    // Like the Go agent, a heartbeat may take at most one interval.
    const auto timeout = std::chrono::milliseconds(std::max<i64>(100, m_heartbeatIntervalNs / 1'000'000));
    m_bus.request(orch::orchestratorSubject(m_config.shard, "Heartbeat"), orch::encode(req), {}, timeout,
                  [shared, seq](BusReply reply) {
                      if (auto s = shared.lock()) {
                          std::lock_guard lock(s->mutex);
                          s->replies.push_back(Shared::Reply{Shared::Kind::Heartbeat, seq, std::move(reply)});
                      }
                  });
}

void OrchestratorClient::applyRegister(u64 seq, BusReply reply, i64 nowNs, std::vector<authority::LeaseEvent>& events) {
    if (seq != m_seq) {
        ++m_stats.staleReplies;
        return;
    }
    m_registerInFlight = false;
    Result<orch::RegisterResult> res = reply.ok() ? orch::decodeRegisterResult(reply.message.data)
                                                  : Result<orch::RegisterResult>(Error{ErrorCode::IoError, describe(reply)});
    if (!res) {
        if (m_stats.registerFailures++ == 0 || m_backoffMs >= m_config.backoffMaxMs)
            HELIOS_LOG_WARN(LogOrch, "RegisterProcess '{}' failed ({}); retrying in {} ms", m_config.info.name,
                            res.error().message, m_backoffMs);
        m_nextRegisterNs = nowNs + m_backoffMs * 1'000'000;
        m_backoffMs = std::min(m_backoffMs * 2, std::max<i64>(m_config.backoffMaxMs, 1));
        return;
    }
    const orch::RegisterResult& r = *res;
    m_registered = true;
    m_processId = r.processId;
    m_epoch = r.epoch;
    m_heartbeatIntervalNs = std::max<i64>(100, r.heartbeatIntervalMs > 0 ? r.heartbeatIntervalMs : 1000) * 1'000'000;
    m_nextHeartbeatNs = nowNs + m_heartbeatIntervalNs;
    m_backoffMs = std::max<i64>(1, m_config.backoffMinMs);
    ++m_stats.registrations;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->registered = true;
        m_shared->processId = r.processId;
        m_shared->epoch = r.epoch;
        m_shared->idShard = r.idShard;
        m_shared->pool = r.idBlocks;
        std::sort(m_shared->pool.begin(), m_shared->pool.end());
    }
    HELIOS_LOG_INFO(LogOrch, "registered '{}' as {} process {} epoch {} ({} zone(s), {} id block(s))", m_config.info.name,
                    m_config.info.kind, r.processId, r.epoch, r.assignments.size(), r.idBlocks.size());
    auto ev = m_leases.onAssignments(toRegions(r.assignments));
    events.insert(events.end(), ev.begin(), ev.end());
}

void OrchestratorClient::applyHeartbeat(u64 seq, BusReply reply, i64 nowNs, std::vector<authority::LeaseEvent>& events) {
    if (seq != m_seq) {
        ++m_stats.staleReplies;
        return;
    }
    m_heartbeatInFlight = false;
    if (reply.status == BusStatus::Ok && reply.errorCode() == orch::kCodeFailedPrecondition) {
        HELIOS_LOG_WARN(LogOrch, "lease lost for process {} epoch {} ({}); fencing and registering again", m_processId,
                        m_epoch, reply.errorMessage());
        loseLease(events, nowNs);
        return;
    }
    Result<orch::HeartbeatResult> res = reply.ok() ? orch::decodeHeartbeatResult(reply.message.data)
                                                   : Result<orch::HeartbeatResult>(Error{ErrorCode::IoError, describe(reply)});
    if (!res) {
        // Holder rule: an unreachable orchestrator never fences; keep the regions, keep trying.
        if (m_stats.heartbeatFailures++ == 0 || m_stats.heartbeatFailures % 30 == 0)
            HELIOS_LOG_WARN(LogOrch, "heartbeat failed ({}); keeping {} region(s) and retrying", res.error().message,
                            m_leases.held().size());
        m_leases.onControlPlaneUnreachable();
        return;
    }
    ++m_stats.heartbeatsOk;
    m_mode = res->mode;
    auto ev = m_leases.onAssignments(toRegions(res->assignments));
    events.insert(events.end(), ev.begin(), ev.end());
}

void OrchestratorClient::loseLease(std::vector<authority::LeaseEvent>& events, i64 nowNs) {
    ++m_stats.leaseLost;
    auto ev = m_leases.onLeaseLost();
    events.insert(events.end(), ev.begin(), ev.end());
    m_registered = false;
    m_heartbeatInFlight = false;
    ++m_seq; // replies to the old registration are now stale
    m_nextRegisterNs = nowNs;
    m_backoffMs = std::max<i64>(1, m_config.backoffMinMs);
    std::lock_guard lock(m_shared->mutex);
    m_shared->registered = false;
    m_shared->pool.clear();
}

void OrchestratorClient::allocateIdBlocks(u32 n, std::function<void(Result<std::vector<u64>>)> done) {
    orch::AllocateIdBlocksRequest req;
    {
        std::lock_guard lock(m_shared->mutex);
        if (!m_shared->registered) {
            // Outside the lock: `done` may take its own locks.
            req.processId = 0;
        } else {
            req.processId = m_shared->processId;
            req.epoch = m_shared->epoch;
        }
    }
    if (req.processId == 0) {
        done(Error{ErrorCode::InvalidState, "not registered with the orchestrator"});
        return;
    }
    req.n = n;
    m_bus.request(orch::orchestratorSubject(m_config.shard, "AllocateIdBlocks"), orch::encode(req), {},
                  std::chrono::milliseconds(m_config.requestTimeoutMs), [done = std::move(done)](BusReply reply) {
                      if (!reply.ok()) {
                          done(rpcError(reply));
                          return;
                      }
                      auto res = orch::decodeAllocateIdBlocksResponse(reply.message.data);
                      if (!res) {
                          done(res.error());
                          return;
                      }
                      std::vector<u64> prefixes = std::move(res->prefixes);
                      std::sort(prefixes.begin(), prefixes.end());
                      done(std::move(prefixes));
                  });
}

std::vector<u64> OrchestratorClient::takePooledBlocks(i64 abovePrefix, u32 max) {
    std::lock_guard lock(m_shared->mutex);
    auto& pool = m_shared->pool;
    pool.erase(std::remove_if(pool.begin(), pool.end(), [abovePrefix](u64 p) { return static_cast<i64>(p) <= abovePrefix; }),
               pool.end());
    const usize n = std::min<usize>(max, pool.size());
    std::vector<u64> out(pool.begin(), pool.begin() + static_cast<isize>(n));
    pool.erase(pool.begin(), pool.begin() + static_cast<isize>(n));
    return out;
}

void OrchestratorClient::resolveZone(u64 zoneId, std::string zoneName, std::function<void(Result<orch::Route>)> done) {
    orch::ResolveZoneRequest req;
    req.zoneId = zoneId;
    if (zoneId == 0) req.zoneName = std::move(zoneName);
    m_bus.request(orch::orchestratorSubject(m_config.shard, "ResolveZone"), orch::encode(req), {},
                  std::chrono::milliseconds(m_config.requestTimeoutMs), [done = std::move(done)](BusReply reply) {
                      if (!reply.ok()) {
                          done(rpcError(reply));
                          return;
                      }
                      done(orch::decodeRoute(reply.message.data));
                  });
}

std::vector<authority::LeaseEvent> OrchestratorClient::shutdown(std::chrono::milliseconds timeout) {
    std::vector<authority::LeaseEvent> events = m_leases.releaseAll();
    if (!m_registered) return events;
    orch::DeregisterRequest req{m_processId, m_epoch};
    m_registered = false;
    ++m_seq;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->registered = false;
        m_shared->pool.clear();
        m_shared->deregisterDone = false;
    }
    std::weak_ptr<Shared> shared = m_shared;
    m_bus.request(orch::orchestratorSubject(m_config.shard, "Deregister"), orch::encode(req), {},
                  std::max(timeout, std::chrono::milliseconds(100)), [shared](BusReply) {
                      if (auto s = shared.lock()) {
                          std::lock_guard lock(s->mutex);
                          s->deregisterDone = true;
                          s->cv.notify_all();
                      }
                  });
    if (timeout.count() > 0) {
        std::unique_lock lock(m_shared->mutex);
        m_shared->cv.wait_for(lock, timeout, [&] { return m_shared->deregisterDone; });
    }
    HELIOS_LOG_INFO(LogOrch, "deregistered process {} epoch {}", req.processId, req.epoch);
    return events;
}

// ---------------------------------------------------------------------------------------------
// OrchestratorIdBlockSource
// ---------------------------------------------------------------------------------------------

struct OrchestratorIdBlockSource::State {
    std::mutex mutex;
    std::vector<u64> ready;
    bool inFlight = false;
    bool syncFailure = false;
    i64 lastPrefix = -1;
};

OrchestratorIdBlockSource::OrchestratorIdBlockSource(OrchestratorClient& client)
    : m_client(client), m_state(std::make_shared<State>()) {}

OrchestratorIdBlockSource::~OrchestratorIdBlockSource() = default;

Result<void> OrchestratorIdBlockSource::allocateIdBlocks(u32 n, std::vector<u64>& out) {
    if (n < 1 || n > ecs::BlockIdLayout::kMaxBlocksPerCall) return Error{ErrorCode::InvalidArgument, "n must be 1..16"};
    {
        std::lock_guard lock(m_state->mutex);
        std::vector<u64> pooled = m_client.takePooledBlocks(m_state->lastPrefix, n);
        if (!pooled.empty()) {
            m_state->lastPrefix = static_cast<i64>(pooled.back());
            out.insert(out.end(), pooled.begin(), pooled.end());
            return {};
        }
        if (m_state->inFlight) return Error{ErrorCode::Busy, "AllocateIdBlocks in flight"};
        m_state->inFlight = true;
        m_state->syncFailure = false;
    }
    std::weak_ptr<State> weak = m_state;
    m_client.allocateIdBlocks(n, [weak](Result<std::vector<u64>> r) {
        auto s = weak.lock();
        if (!s) return;
        std::lock_guard lock(s->mutex);
        s->inFlight = false;
        if (r) s->ready.insert(s->ready.end(), r->begin(), r->end());
        else s->syncFailure = true;
    });
    std::lock_guard lock(m_state->mutex);
    if (!m_state->inFlight && m_state->syncFailure) return Error{ErrorCode::InvalidState, "not registered with the orchestrator"};
    return Error{ErrorCode::Busy, "AllocateIdBlocks in flight"};
}

u32 OrchestratorIdBlockSource::pump() {
    std::vector<u64> ready;
    {
        std::lock_guard lock(m_state->mutex);
        ready.swap(m_state->ready);
        // Keep only prefixes newer than anything this source handed out.
        ready.erase(std::remove_if(ready.begin(), ready.end(),
                                   [this](u64 p) { return static_cast<i64>(p) <= m_state->lastPrefix; }),
                    ready.end());
        if (!ready.empty()) m_state->lastPrefix = static_cast<i64>(ready.back());
    }
    if (ready.empty() || !m_minter) return 0;
    if (auto r = m_minter->addBlocks(ready); !r) {
        HELIOS_LOG_WARN(LogOrch, "id blocks refused by the minter: {}", r.error().message);
        return 0;
    }
    return static_cast<u32>(ready.size());
}

} // namespace helios::server
