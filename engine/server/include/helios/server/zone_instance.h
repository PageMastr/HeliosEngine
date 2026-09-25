#pragma once
// ZoneInstance: one running copy of a zone on a cell (04 §3.1, §7; 02 §5.5). It owns its flecs
// world (ecs::World), its dilatable ZoneClock with the TiDi controller, its per-tick job graph,
// its AG table and the sessions the gateways attached to it. Instances share nothing mutable;
// the host talks to one only through its inbox (post) and outbox (takeOutbox).
//
// A tick (tick()) consumes one due clock step and runs the TickGraph:
//   Input             zone.input: ID-block delivery, fence replies (AgTable::poll), inbox drain
//                     (count-capped; attach/detach/client messages), then the ECS Input stage;
//                     World::beginTick() (tick counter, ID-block refill) runs first
//   PrePhysics        ECS systems (sim)
//   Physics           ECS systems (Jolt grids arrive with engine/physics: stub)
//   PostPhysics       ECS systems + zone.gameplay (Phase 0 dev game: Echo replies)
//   AuthorityFlush    ECS systems (effects, ghosts, handoff offers arrive in v1)
//   ReplicationGather ECS systems + zone.gather (World::gatherChanges; replication arrives later)
//   ConnectionWrite   ECS systems + zone.write (a TickState STATE chunk per session)
//   Send              ECS systems + zone.send (outbox for the host's trunk IO)
// then feeds the tick's measured time to the TiDi controller and announces dilation changes to
// every session on CONTROL (clients are informed ahead of the change's effective tick).
//
// Phase 0 dev game: an Echo request on EVENT_R is answered in the gameplay stage with the tick
// number, and every session gets a TickState chunk each tick, so a client can see its route and
// the zone clock working end to end. setMessageHandler() lets tests and gameplay take over.
//
// Threading: post() is thread-safe (IO threads fill the inbox). Everything else runs on the
// host's tick thread.

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <map>
#include <vector>

#include "helios/authority/ag_table.h"
#include "helios/authority/zone_clock.h"
#include "helios/core/result.h"
#include "helios/ecs/world.h"
#include "helios/net/channel.h"
#include "helios/server/protocol.h"
#include "helios/server/tick_graph.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::server {

using ZoneId = u64;

struct ZoneDesc {
    ZoneId id = 0;
    std::string name;
    u32 tickHz = 20;
    authority::LeaseGen leaseGen = 0;
    authority::CellId cell;                ///< This process (for AG ownership).
    u32 idShard = 0;
    ecs::IdBlockSource* idBlocks = nullptr; ///< Orchestrator source (not owned); null = local source.
    /// Called at the start of every tick's Input stage (delivers async ID blocks to the minter).
    std::function<void()> onTickStart;
    authority::TiDiConfig tidi;
    u32 maxCatchUpTicks = 4;
    u32 maxInboxPerTick = 256;             ///< 05 §2.4: replies drain count-capped per tick.
    /// Client messages that may wait in the inbox. Gateways meter each session, but many sessions
    /// together can still post faster than the capped drain; beyond this, new messages are dropped
    /// (and counted) instead of growing memory without bound. Attach/Detach are never dropped.
    u32 maxInboxQueued = 4096;
    u32 maxSessions = 256;
    bool sendTickState = true;
};

/// What IO threads post to a zone.
struct ZoneInboxItem {
    enum class Kind : u8 { Attach, Detach, Message };
    Kind kind = Kind::Message;
    u64 route = 0;          ///< The host's opaque route to the session's gateway (trunk handle bits).
    u64 sessionId = 0;
    u64 sessionEpoch = 0;
    u64 accountId = 0;
    u64 characterId = 0;
    u8 flags = 0;
    proto::DetachReason detachReason = proto::DetachReason::ClientLeft;
    net::Channel channel = net::Channel::EventReliable;
    std::vector<u8> payload;
};

/// What a tick produces for the host's IO.
struct ZoneOutMessage {
    enum class Kind : u8 { Deliver, AttachAck, AttachNack };
    Kind kind = Kind::Deliver;
    u64 route = 0;
    u64 sessionId = 0;
    net::Channel channel = net::Channel::EventReliable;
    std::vector<u8> payload;  ///< Deliver: the client message. AttachAck/Nack: the encoded trunk message.
};

struct ZoneSession {
    u64 sessionId = 0;
    u64 epoch = 0;
    u64 route = 0;
    u64 accountId = 0;
    u64 characterId = 0;
    u64 attachedTick = 0;
    u64 messages = 0;
};

struct ZoneStats {
    u64 ticks = 0;
    u64 inboxItems = 0;
    u64 inboxDeferred = 0;  ///< Items left for a later tick by the per-tick cap.
    u64 inboxDropped = 0;   ///< Client messages refused because maxInboxQueued were waiting.
    u64 attaches = 0;
    u64 rebinds = 0;
    u64 detaches = 0;
    u64 nacks = 0;
    u64 echoes = 0;
    u64 ignoredMessages = 0;
    u64 staleMessages = 0;  ///< From a route that no longer carries the session (fenced).
    u64 tickStatesSent = 0;
    u64 dilationNotices = 0;
    i64 lastTickNs = 0;
    i64 maxTickNs = 0;
    i64 totalTickNs = 0;
};

class ZoneInstance {
public:
    /// Custom client-message handling; return true when handled (the dev game is skipped).
    using MessageHandler = std::function<bool(ZoneInstance&, ZoneSession&, net::Channel, std::span<const u8>)>;

    static Result<std::unique_ptr<ZoneInstance>> create(ZoneDesc desc, jobs::JobSystem* jobs);
    ~ZoneInstance();
    ZoneInstance(const ZoneInstance&) = delete;
    ZoneInstance& operator=(const ZoneInstance&) = delete;

    ZoneId id() const noexcept { return m_desc.id; }
    const std::string& name() const noexcept { return m_desc.name; }
    authority::LeaseGen leaseGen() const noexcept { return m_desc.leaseGen; }
    const ZoneDesc& desc() const noexcept { return m_desc; }
    authority::ZoneClock& clock() noexcept { return m_clock; }
    const authority::ZoneClock& clock() const noexcept { return m_clock; }
    TickGraph& graph() noexcept { return m_graph; }
    ecs::World& world() noexcept { return *m_world; }
    authority::AgTable& ags() noexcept { return m_ags; }

    /// Queues an item for the next tick's Input stage. Returns false (and drops it) for a client
    /// message while maxInboxQueued messages are waiting. Thread-safe.
    bool post(ZoneInboxItem item);
    usize inboxSize() const;
    /// Runs one tick if one is due at `wallNowNs` (after clock().advanceTo()). Returns false when
    /// none was due.
    Result<bool> tick(i64 wallNowNs);
    /// Messages produced by the ticks since the last call.
    std::vector<ZoneOutMessage> takeOutbox();

    /// Queues a message for a session (delivered by this tick's Send stage). Tick thread.
    bool sendToSession(u64 sessionId, net::Channel channel, std::vector<u8> payload);
    void setMessageHandler(MessageHandler handler) { m_handler = std::move(handler); }
    const std::map<u64, ZoneSession>& sessions() const noexcept { return m_sessions; }
    const ZoneStats& stats() const noexcept { return m_stats; }
    /// Durations of the most recent ticks (up to kRecentTicks, oldest first), for load reports
    /// (the orchestrator's tick p99, 04 §1).
    static constexpr usize kRecentTicks = 128;
    std::vector<i64> recentTickNs() const;

private:
    ZoneInstance(ZoneDesc desc, jobs::JobSystem* jobs);
    Result<void> init();
    void stageInput(TickContext& ctx);
    void stageGameplay(TickContext& ctx);
    void stageWrite(TickContext& ctx);
    void stageSend(TickContext& ctx);
    void handleAttach(const ZoneInboxItem& item, u64 tick);
    void handleMessage(ZoneInboxItem& item);

    ZoneDesc m_desc;
    jobs::JobSystem* m_jobs;
    authority::ZoneClock m_clock;
    TickGraph m_graph;
    std::unique_ptr<ecs::World> m_world;
    authority::AgTable m_ags;
    MessageHandler m_handler;

    mutable std::mutex m_inboxMutex;
    std::deque<ZoneInboxItem> m_inbox;
    usize m_inboxMessages = 0; // Message items in m_inbox
    u64 m_inboxDropped = 0;    // under m_inboxMutex; copied into m_stats by the tick

    std::map<u64, ZoneSession> m_sessions; // ordered: per-session output is deterministic
    struct PendingEcho {
        u64 sessionId;
        std::vector<u8> request;
    };
    std::vector<PendingEcho> m_echoes;
    std::vector<ZoneOutMessage> m_pendingOut; // produced during the tick, moved to m_outbox by Send
    std::vector<ZoneOutMessage> m_outbox;
    ZoneStats m_stats;
    std::array<i64, kRecentTicks> m_recentTicks{};
    usize m_recentNext = 0;
};

} // namespace helios::server
