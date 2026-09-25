#pragma once
// In-process stand-ins for the Go orchestrator and Session service on a FakeBus, speaking the
// exact JSON contracts (orch_protocol.h, checked against Go's own output in test_contracts.cpp).
// They follow services/internal/orchestrator/registry.go and services/internal/session/service.go
// closely enough for the cell and gateway tests: per-name epochs, zone placement onto cells that
// declared the zone (or accept any), per-zone lease generations, lease_lost for unknown or
// superseded registrations, ID-block prefixes from a monotonic counter, ResolveZone, ticket sealing
// with `missing`, and the gateway control subjects.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "helios/server/fake_bus.h"
#include "helios/server/orch_protocol.h"

namespace helios::server::test {

inline void replyJson(FakeBus& bus, const BusMessage& req, std::vector<u8> json) {
    (void)bus.publish(req.reply, std::move(json));
}
inline void replyError(FakeBus& bus, const BusMessage& req, std::string_view code, std::string_view message) {
    BusHeaders h{{std::string(kHeaderError), std::string(code)}, {std::string(kHeaderErrorMessage), std::string(message)}};
    (void)bus.publish(req.reply, {}, std::move(h));
}

class FakeOrchestrator {
public:
    struct Zone {
        u64 id = 0;
        std::string name;
        u64 owner = 0;
        u64 leaseGen = 0;
    };
    struct Process {
        u64 id = 0;
        u64 epoch = 0;
        orch::ProcessInfo info;
        u64 heartbeats = 0;
    };

    FakeOrchestrator(FakeBus& bus, std::string shard, std::vector<std::pair<u64, std::string>> zones)
        : m_bus(bus), m_shard(std::move(shard)) {
        for (auto& [id, name] : zones) m_zones[id] = Zone{id, name, 0, 0};
        auto sub = [&](const char* method, void (FakeOrchestrator::*fn)(const BusMessage&)) {
            m_subs.push_back(*m_bus.subscribe(orch::orchestratorSubject(m_shard, method),
                                              [this, fn](const BusMessage& m) { (this->*fn)(m); }));
        };
        sub("RegisterProcess", &FakeOrchestrator::onRegister);
        sub("Heartbeat", &FakeOrchestrator::onHeartbeat);
        sub("AllocateIdBlocks", &FakeOrchestrator::onAllocate);
        sub("Deregister", &FakeOrchestrator::onDeregister);
        sub("ResolveZone", &FakeOrchestrator::onResolve);
    }
    ~FakeOrchestrator() {
        for (u64 s : m_subs) m_bus.unsubscribe(s);
    }

    /// Answer every request with "unavailable" (a leadership change) while set.
    bool unavailable = false;
    /// Drop requests silently while set (the orchestrator is partitioned away): callers time out.
    bool silent = false;
    i64 heartbeatIntervalMs = 1000;
    u32 idShard = 3;

    /// Forgets a process (as a lease expiry or a new leader would): its next heartbeat gets
    /// lease_lost and its zones are placed again.
    void expire(u64 processId) {
        m_procs.erase(processId);
        for (auto& [id, z] : m_zones)
            if (z.owner == processId) z.owner = 0;
        place();
    }
    /// Moves a zone to a new generation on the same owner (as a re-placement would).
    void bumpGeneration(u64 zoneId) {
        auto& z = m_zones.at(zoneId);
        z.leaseGen = ++m_genCounter;
    }
    void addZone(u64 id, std::string name) {
        m_zones[id] = Zone{id, std::move(name), 0, 0};
        place();
    }
    const Zone& zone(u64 id) const { return m_zones.at(id); }
    const std::map<u64, Process>& processes() const { return m_procs; }
    u64 processByName(std::string_view name) const {
        for (const auto& [id, p] : m_procs)
            if (p.info.name == name) return id;
        return 0;
    }
    u64 registrations = 0;
    u64 heartbeats = 0;
    u64 prefixesIssued = 0;

private:
    bool gate(const BusMessage& m) {
        if (silent) return false;
        if (unavailable) {
            replyError(m_bus, m, orch::kCodeUnavailable, "orchestrator leadership changed; retry");
            return false;
        }
        return true;
    }

    void place() {
        for (auto& [id, z] : m_zones) {
            if (z.owner) continue;
            u64 best = 0;
            bool bestDeclared = false;
            for (auto& [pid, p] : m_procs) {
                if (p.info.kind != orch::kKindCell) continue;
                const bool declared = std::find(p.info.zones.begin(), p.info.zones.end(), z.name) != p.info.zones.end();
                if (!declared && !p.info.zones.empty()) continue;
                if (!best || (declared && !bestDeclared)) {
                    best = pid;
                    bestDeclared = declared;
                }
            }
            if (best) {
                z.owner = best;
                z.leaseGen = ++m_genCounter;
            }
        }
    }

    std::vector<orch::Assignment> assignmentsOf(u64 pid) const {
        std::vector<orch::Assignment> out;
        for (const auto& [id, z] : m_zones)
            if (z.owner == pid) out.push_back(orch::Assignment{z.id, z.name, z.leaseGen});
        return out;
    }

    std::vector<u64> prefixes(u32 n) {
        std::vector<u64> out;
        for (u32 i = 0; i < n; ++i) out.push_back(++m_lastPrefix);
        prefixesIssued += n;
        return out;
    }

    void onRegister(const BusMessage& m) {
        if (!gate(m)) return;
        auto info = orch::decodeProcessInfo(m.data);
        if (!info || info->name.empty() || (info->kind != orch::kKindCell && info->kind != orch::kKindGateway)) {
            replyError(m_bus, m, orch::kCodeInvalidArgument, "bad process info");
            return;
        }
        for (auto it = m_procs.begin(); it != m_procs.end();) { // supersede the same name
            if (it->second.info.name == info->name) {
                for (auto& [id, z] : m_zones)
                    if (z.owner == it->first) z.owner = 0;
                it = m_procs.erase(it);
            } else {
                ++it;
            }
        }
        const u64 id = ++m_nextProcess;
        const u64 epoch = ++m_epochByName[info->name];
        m_procs[id] = Process{id, epoch, *info, 0};
        place();
        ++registrations;
        orch::RegisterResult r;
        r.processId = id;
        r.epoch = epoch;
        r.leaseTtlMs = 12000;
        r.heartbeatIntervalMs = heartbeatIntervalMs;
        r.assignments = assignmentsOf(id);
        r.idShard = idShard;
        if (info->kind == orch::kKindCell) r.idBlocks = prefixes(2);
        replyJson(m_bus, m, orch::encode(r));
    }

    void onHeartbeat(const BusMessage& m) {
        if (!gate(m)) return;
        auto req = orch::decodeHeartbeatRequest(m.data);
        if (!req) {
            replyError(m_bus, m, orch::kCodeInvalidArgument, "malformed JSON request");
            return;
        }
        auto it = m_procs.find(req->processId);
        if (it == m_procs.end() || it->second.epoch != req->epoch) {
            replyError(m_bus, m, orch::kCodeFailedPrecondition, orch::kLeaseLost);
            return;
        }
        ++it->second.heartbeats;
        ++heartbeats;
        orch::HeartbeatResult r;
        r.leaseExpires = "2026-09-25T12:00:12Z";
        r.assignments = assignmentsOf(req->processId);
        r.mode = std::string(orch::kModeNormal);
        replyJson(m_bus, m, orch::encode(r));
    }

    void onAllocate(const BusMessage& m) {
        if (!gate(m)) return;
        auto req = orch::decodeAllocateIdBlocksRequest(m.data);
        if (!req || req->n < 1 || req->n > 16) {
            replyError(m_bus, m, orch::kCodeInvalidArgument, "n must be within 1..16");
            return;
        }
        auto it = m_procs.find(req->processId);
        if (it == m_procs.end() || it->second.epoch != req->epoch) {
            replyError(m_bus, m, orch::kCodeFailedPrecondition, orch::kLeaseLost);
            return;
        }
        if (it->second.info.kind != orch::kKindCell) {
            replyError(m_bus, m, orch::kCodePermissionDenied, "only minting processes (cells) receive ID blocks");
            return;
        }
        orch::AllocateIdBlocksResponse r;
        r.idShard = idShard;
        r.prefixes = prefixes(req->n);
        replyJson(m_bus, m, orch::encode(r));
    }

    void onDeregister(const BusMessage& m) {
        if (!gate(m)) return;
        auto req = orch::decodeDeregisterRequest(m.data);
        auto it = req ? m_procs.find(req->processId) : m_procs.end();
        if (it == m_procs.end() || it->second.epoch != req->epoch) {
            replyError(m_bus, m, orch::kCodeFailedPrecondition, orch::kLeaseLost);
            return;
        }
        expire(req->processId);
        replyJson(m_bus, m, orch::bytesOf("{}"));
    }

    void onResolve(const BusMessage& m) {
        if (!gate(m)) return;
        auto req = orch::decodeResolveZoneRequest(m.data);
        const Zone* z = nullptr;
        for (const auto& [id, zone] : m_zones)
            if ((req->zoneId != 0 && zone.id == req->zoneId) || (req->zoneId == 0 && zone.name == req->zoneName)) z = &zone;
        if (!z) {
            replyError(m_bus, m, orch::kCodeNotFound, "unknown zone");
            return;
        }
        auto p = m_procs.find(z->owner);
        if (!z->owner || p == m_procs.end()) {
            replyError(m_bus, m, orch::kCodeUnavailable, "zone has no live cell");
            return;
        }
        orch::Route r{z->id, z->name, z->leaseGen, p->first, p->second.info.name, p->second.epoch, p->second.info.address};
        replyJson(m_bus, m, orch::encode(r));
    }

    FakeBus& m_bus;
    std::string m_shard;
    std::vector<u64> m_subs;
    std::map<u64, Zone> m_zones;
    std::map<u64, Process> m_procs;
    std::map<std::string, u64> m_epochByName;
    u64 m_nextProcess = 0;
    u64 m_genCounter = 0;
    u64 m_lastPrefix = 1000;
};

/// The Session service's gateway-facing half: SealReconnectTickets and the control subjects.
class FakeSessionService {
public:
    FakeSessionService(FakeBus& bus, std::string shard) : m_bus(bus), m_shard(std::move(shard)) {
        m_sub = *m_bus.subscribe(orch::sealTicketsSubject(m_shard), [this](const BusMessage& m) { onSeal(m); });
    }
    ~FakeSessionService() { m_bus.unsubscribe(m_sub); }

    /// Sessions the service knows, with their current session_epoch.
    std::map<u64, u64> sessions;
    u64 sealCalls = 0;
    std::vector<orch::SealItem> lastItems;
    /// While set, SealReconnectTickets requests are held (a slow service); releaseSeals() answers
    /// them from the state at that moment.
    bool holdSeals = false;
    usize heldSeals() const { return m_held.size(); }
    void releaseSeals() {
        std::vector<BusMessage> held;
        held.swap(m_held);
        for (const BusMessage& m : held) answerSeal(m);
    }

    /// A reconnect elsewhere: CAS-increments session_epoch and tells the gateways.
    void reconnect(u64 sessionId) {
        const u64 epoch = ++sessions[sessionId];
        (void)m_bus.publish(orch::gatewayControlSubject(m_shard, "session_epoch"),
                            orch::encode(orch::ControlMessage{sessionId, epoch, "reconnect"}));
    }
    void kick(u64 sessionId, std::string reason) {
        sessions.erase(sessionId);
        (void)m_bus.publish(orch::gatewayControlSubject(m_shard, "kick"), orch::encode(orch::ControlMessage{sessionId, 0, std::move(reason)}));
    }

private:
    void onSeal(const BusMessage& m) {
        if (holdSeals) {
            m_held.push_back(m);
            return;
        }
        answerSeal(m);
    }
    void answerSeal(const BusMessage& m) {
        ++sealCalls;
        auto req = orch::decodeSealRequest(m.data);
        if (!req) {
            replyError(m_bus, m, orch::kCodeInvalidArgument, "malformed JSON request");
            return;
        }
        lastItems = req->sessions;
        orch::SealResponse r;
        for (const orch::SealItem& it : req->sessions) {
            auto s = sessions.find(it.sessionId);
            if (s == sessions.end() || (it.epoch != 0 && s->second > it.epoch)) {
                r.missing.push_back(it.sessionId);
                continue;
            }
            r.tickets.push_back(orch::SealedTicket{it.sessionId, "ticket-" + std::to_string(it.sessionId) + "-" + std::to_string(sealCalls),
                                                   "2026-09-25T12:05:00.5Z"});
        }
        replyJson(m_bus, m, orch::encode(r));
    }

    FakeBus& m_bus;
    std::string m_shard;
    u64 m_sub = 0;
    std::vector<BusMessage> m_held;
};

} // namespace helios::server::test
