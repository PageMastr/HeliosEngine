#include "helios/server/zone_instance.h"

#include <algorithm>

#include "helios/core/jobs.h"
#include "server_log.h"

namespace helios::server {

namespace {
authority::ZoneClock::Config clockConfig(const ZoneDesc& d) {
    authority::ZoneClock::Config c;
    c.tickHz = d.tickHz;
    c.tidi = d.tidi;
    c.maxCatchUpTicks = d.maxCatchUpTicks;
    return c;
}

constexpr std::array<const char*, kStageCount> kEcsHookNames = {
    "ecs.Input",        "ecs.PrePhysics",        "ecs.Physics",         "ecs.PostPhysics",
    "ecs.AuthorityFlush", "ecs.ReplicationGather", "ecs.ConnectionWrite", "ecs.Send",
};
} // namespace

Result<std::unique_ptr<ZoneInstance>> ZoneInstance::create(ZoneDesc desc, jobs::JobSystem* jobs) {
    if (desc.id == 0) return Error{ErrorCode::InvalidArgument, "zone id 0 is reserved"};
    if (desc.name.empty()) desc.name = "zone-" + std::to_string(desc.id);
    HELIOS_TRY(authority::ZoneClock::validate(clockConfig(desc)));
    std::unique_ptr<ZoneInstance> zone(new ZoneInstance(std::move(desc), jobs));
    HELIOS_TRY(zone->init());
    return zone;
}

ZoneInstance::ZoneInstance(ZoneDesc desc, jobs::JobSystem* jobs)
    : m_desc(std::move(desc)),
      m_jobs(jobs),
      m_clock(clockConfig(m_desc)),
      m_ags(authority::Owner{m_desc.cell, authority::RegionId{m_desc.id}, m_desc.leaseGen}) {}

ZoneInstance::~ZoneInstance() = default;

Result<void> ZoneInstance::init() {
    ecs::WorldDesc wd;
    wd.name = "zone:" + m_desc.name;
    wd.jobs = m_jobs;
    wd.shard = m_desc.idShard;
    wd.idBlocks = m_desc.idBlocks;
    m_world = std::make_unique<ecs::World>(wd);

    m_graph.setBudgets(StageBudgets::forTickHz(m_desc.tickHz));
    HELIOS_TRY(m_graph.addHook({"zone.input", Stage::Input, [this](TickContext& c) { stageInput(c); }, HookThread::Tick, {}}));
    for (u32 s = 0; s < kStageCount; ++s) {
        const Stage stage = static_cast<Stage>(s);
        HookDesc h;
        h.name = kEcsHookNames[s];
        h.stage = stage;
        h.fn = [this, stage](TickContext& c) {
            if (auto r = m_world->runStage(stage, c.dt); !r)
                HELIOS_LOG_ERROR(LogCell, "zone {} ECS stage {} failed: {}", m_desc.name, ecs::stageName(stage), r.error().message);
        };
        if (stage == Stage::Input) h.after = {"zone.input"};
        HELIOS_TRY(m_graph.addHook(std::move(h)));
    }
    HELIOS_TRY(m_graph.addHook({"zone.gameplay", Stage::PostPhysics, [this](TickContext& c) { stageGameplay(c); },
                                HookThread::Tick, {"ecs.PostPhysics"}}));
    HELIOS_TRY(m_graph.addHook({"zone.gather", Stage::ReplicationGather,
                                [this](TickContext&) {
                                    ecs::ChangeList changes;
                                    m_world->gatherChanges(changes); // replication arrives with engine/replication
                                },
                                HookThread::Tick, {"ecs.ReplicationGather"}}));
    HELIOS_TRY(m_graph.addHook({"zone.write", Stage::ConnectionWrite, [this](TickContext& c) { stageWrite(c); },
                                HookThread::Tick, {"ecs.ConnectionWrite"}}));
    HELIOS_TRY(m_graph.addHook({"zone.send", Stage::Send, [this](TickContext& c) { stageSend(c); }, HookThread::Tick,
                                {"ecs.Send"}}));
    HELIOS_TRY(m_graph.build());
    return {};
}

bool ZoneInstance::post(ZoneInboxItem item) {
    std::lock_guard lock(m_inboxMutex);
    if (item.kind == ZoneInboxItem::Kind::Message) {
        if (m_inboxMessages >= m_desc.maxInboxQueued) {
            ++m_inboxDropped;
            return false;
        }
        ++m_inboxMessages;
    }
    m_inbox.push_back(std::move(item));
    return true;
}

usize ZoneInstance::inboxSize() const {
    std::lock_guard lock(m_inboxMutex);
    return m_inbox.size();
}

Result<bool> ZoneInstance::tick(i64 wallNowNs) {
    m_clock.advanceTo(wallNowNs);
    const auto t = m_clock.beginTick();
    if (!t) return false;
    TickContext ctx;
    ctx.tick = *t;
    ctx.dt = 1.0f / static_cast<f32>(m_desc.tickHz);
    ctx.dilationPpm = m_clock.dilationPpm();
    ctx.wallNowNs = wallNowNs;
    ctx.user = this;
    HELIOS_TRY(m_graph.run(ctx, m_jobs));
    const i64 ns = m_graph.lastTickNs();
    m_clock.onTickMeasured(ns);
    // Clients learn about a dilation change before its effective tick (04 §3.2).
    for (const authority::DilationChange& ch : m_clock.takeDilationChanges()) {
        const std::vector<u8> notice = proto::encode(proto::ClientTimeDilation{ch.effectiveTick, ch.dilationPpm});
        for (const auto& [id, s] : m_sessions) {
            m_outbox.push_back(ZoneOutMessage{ZoneOutMessage::Kind::Deliver, s.route, id, net::Channel::Control, notice});
            ++m_stats.dilationNotices;
        }
    }
    m_recentTicks[m_recentNext++ % kRecentTicks] = ns;
    ++m_stats.ticks;
    m_stats.lastTickNs = ns;
    m_stats.maxTickNs = std::max(m_stats.maxTickNs, ns);
    m_stats.totalTickNs += ns;
    return true;
}

std::vector<i64> ZoneInstance::recentTickNs() const {
    std::vector<i64> out;
    const usize n = std::min(m_recentNext, kRecentTicks);
    out.reserve(n);
    for (usize i = m_recentNext - n; i < m_recentNext; ++i) out.push_back(m_recentTicks[i % kRecentTicks]);
    return out;
}

std::vector<ZoneOutMessage> ZoneInstance::takeOutbox() {
    std::vector<ZoneOutMessage> out;
    out.swap(m_outbox);
    return out;
}

bool ZoneInstance::sendToSession(u64 sessionId, net::Channel channel, std::vector<u8> payload) {
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || payload.size() > proto::kMaxForwardPayload) return false;
    m_pendingOut.push_back(ZoneOutMessage{ZoneOutMessage::Kind::Deliver, it->second.route, sessionId, channel, std::move(payload)});
    return true;
}

// ---------------------------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------------------------

void ZoneInstance::stageInput(TickContext& ctx) {
    if (m_desc.onTickStart) m_desc.onTickStart();
    m_world->beginTick();
    m_ags.poll();
    std::deque<ZoneInboxItem> batch;
    {
        std::lock_guard lock(m_inboxMutex);
        const usize n = std::min<usize>(m_desc.maxInboxPerTick, m_inbox.size());
        for (usize i = 0; i < n; ++i) {
            if (m_inbox.front().kind == ZoneInboxItem::Kind::Message) --m_inboxMessages;
            batch.push_back(std::move(m_inbox.front()));
            m_inbox.pop_front();
        }
        if (!m_inbox.empty()) ++m_stats.inboxDeferred;
        m_stats.inboxDropped = m_inboxDropped;
    }
    for (ZoneInboxItem& item : batch) {
        ++m_stats.inboxItems;
        switch (item.kind) {
        case ZoneInboxItem::Kind::Attach: handleAttach(item, ctx.tick); break;
        case ZoneInboxItem::Kind::Detach: {
            auto it = m_sessions.find(item.sessionId);
            // Only the binding that is current may detach (a late detach from an older gateway
            // after a reconnect must not drop the new binding).
            if (it != m_sessions.end() && it->second.route == item.route && item.sessionEpoch >= it->second.epoch) {
                HELIOS_LOG_DEBUG(LogCell, "zone {}: session {} detached ({})", m_desc.name, item.sessionId,
                                 proto::detachReasonName(item.detachReason));
                m_sessions.erase(it);
                ++m_stats.detaches;
            }
            break;
        }
        case ZoneInboxItem::Kind::Message: handleMessage(item); break;
        }
    }
}

void ZoneInstance::handleAttach(const ZoneInboxItem& item, u64 tick) {
    auto nack = [&](proto::NackReason reason) {
        ++m_stats.nacks;
        m_pendingOut.push_back(ZoneOutMessage{ZoneOutMessage::Kind::AttachNack, item.route, item.sessionId,
                                              net::Channel::EventReliable,
                                              proto::encode(item.sessionId, proto::AttachNack{item.sessionEpoch, m_desc.id, reason})});
    };
    auto it = m_sessions.find(item.sessionId);
    if (it != m_sessions.end()) {
        if (item.sessionEpoch < it->second.epoch) {
            nack(proto::NackReason::StaleEpoch);
            return;
        }
        // Rebind (reconnect through another gateway, 04 §2.4 ClientRebind): the newer epoch wins.
        it->second.epoch = item.sessionEpoch;
        it->second.route = item.route;
        ++m_stats.rebinds;
    } else {
        if (m_sessions.size() >= m_desc.maxSessions) {
            nack(proto::NackReason::Full);
            return;
        }
        ZoneSession s;
        s.sessionId = item.sessionId;
        s.epoch = item.sessionEpoch;
        s.route = item.route;
        s.accountId = item.accountId;
        s.characterId = item.characterId;
        s.attachedTick = tick;
        m_sessions.emplace(item.sessionId, s);
        ++m_stats.attaches;
    }
    proto::AttachAck ack;
    ack.sessionEpoch = item.sessionEpoch;
    ack.zoneId = m_desc.id;
    ack.leaseGen = m_desc.leaseGen;
    ack.tick = tick;
    ack.tickHz = m_desc.tickHz;
    ack.dilationPpm = m_clock.dilationPpm();
    ack.zoneName = m_desc.name;
    m_pendingOut.push_back(ZoneOutMessage{ZoneOutMessage::Kind::AttachAck, item.route, item.sessionId,
                                          net::Channel::EventReliable, proto::encode(item.sessionId, ack)});
    HELIOS_LOG_DEBUG(LogCell, "zone {}: session {} attached at epoch {}", m_desc.name, item.sessionId, item.sessionEpoch);
}

void ZoneInstance::handleMessage(ZoneInboxItem& item) {
    auto it = m_sessions.find(item.sessionId);
    if (it == m_sessions.end() || it->second.route != item.route) {
        ++m_stats.staleMessages;
        return;
    }
    ZoneSession& s = it->second;
    ++s.messages;
    if (m_handler && m_handler(*this, s, item.channel, item.payload)) return;
    if (item.channel == net::Channel::EventReliable && !item.payload.empty() && item.payload[0] == proto::kEchoRequest) {
        m_echoes.push_back(PendingEcho{item.sessionId, std::move(item.payload)});
        return;
    }
    ++m_stats.ignoredMessages;
}

void ZoneInstance::stageGameplay(TickContext& ctx) {
    for (PendingEcho& e : m_echoes) {
        auto req = proto::decodeEchoRequest(e.request);
        if (!req) {
            ++m_stats.ignoredMessages;
            continue;
        }
        if (sendToSession(e.sessionId, net::Channel::EventReliable, proto::encodeEchoReply(req->seq, ctx.tick, req->data)))
            ++m_stats.echoes;
    }
    m_echoes.clear();
}

void ZoneInstance::stageWrite(TickContext& ctx) {
    if (!m_desc.sendTickState || m_sessions.empty()) return;
    proto::TickState ts;
    ts.tick = ctx.tick;
    ts.dilationPpm = ctx.dilationPpm;
    ts.gameTimeNs = m_clock.gameTimeNs();
    ts.sessions = static_cast<u32>(m_sessions.size());
    const std::vector<u8> chunk = proto::encode(ts);
    for (const auto& [id, s] : m_sessions) {
        m_pendingOut.push_back(ZoneOutMessage{ZoneOutMessage::Kind::Deliver, s.route, id, net::Channel::State, chunk});
        ++m_stats.tickStatesSent;
    }
}

void ZoneInstance::stageSend(TickContext&) {
    m_outbox.insert(m_outbox.end(), std::make_move_iterator(m_pendingOut.begin()), std::make_move_iterator(m_pendingOut.end()));
    m_pendingOut.clear();
}

} // namespace helios::server
