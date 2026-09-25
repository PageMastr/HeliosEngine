#include "helios/server/cell_server.h"

#include <algorithm>
#include <limits>
#include <thread>

#include "helios/core/jobs.h"
#include "helios/core/time.h"
#include "server_log.h"

namespace helios::server {

namespace {
f64 seconds(i64 ns) { return static_cast<f64>(ns) * 1e-9; }

std::unique_ptr<jobs::JobSystem> makeJobs(const CellServerConfig& c) {
    if (c.jobs) return nullptr;
    jobs::JobSystemDesc d;
    const u32 hw = std::max(1u, std::thread::hardware_concurrency());
    // 02 §2.3: a cell runs cores - IO threads (the tick thread helps while waiting).
    d.workerCount = c.jobWorkers ? c.jobWorkers : std::max(1u, hw > 2 ? hw - 2 : 1u);
    d.name = "Cell";
    return std::make_unique<jobs::JobSystem>(d);
}
} // namespace

struct CellServer::TrunkHandler final : net::IEndpointHandler {
    CellServer& cell;
    explicit TrunkHandler(CellServer& c) : cell(c) {}
    void onConnected(net::SessionHandle s) override {
        const net::SessionInfo* info = cell.m_trunk->sessionInfo(s);
        // Only gateways open trunks to a cell in Phase 0 (cell <-> cell trunks arrive with v1).
        if (!info || info->userData[0] != proto::kUserDataVersion1 || info->userData[1] != proto::kTrunkPeerGateway) {
            HELIOS_LOG_WARN(LogCell, "trunk #{} refused: its token does not name a gateway", s.index());
            cell.m_trunk->disconnect(s);
            return;
        }
        ++cell.m_stats.trunksConnected;
        cell.m_peers[s.toBits()] = TrunkPeer{};
        HELIOS_LOG_INFO(LogCell, "trunk #{} connected from {}", s.index(), info->address.toString());
    }
    void onDisconnected(net::SessionHandle s, net::DisconnectReason r) override {
        if (!cell.m_peers.contains(s.toBits())) return; // refused at connect
        HELIOS_LOG_INFO(LogCell, "trunk #{} disconnected: {}", s.index(), net::disconnectReasonName(r));
        cell.onTrunkDisconnected(s);
    }
    void onMessage(net::SessionHandle s, net::Channel, std::span<const u8> payload) override {
        if (cell.m_peers.contains(s.toBits())) cell.onTrunkMessage(s, payload);
    }
};

CellServer::CellServer(CellServerConfig config)
    : m_config(std::move(config)),
      m_ownedJobs(makeJobs(m_config)),
      m_host(m_config.jobs ? m_config.jobs : m_ownedJobs.get()) {}

CellServer::~CellServer() = default;

Result<std::unique_ptr<CellServer>> CellServer::create(CellServerConfig config, i64 nowNs) {
    std::unique_ptr<CellServer> cell(new CellServer(std::move(config)));
    HELIOS_TRY(cell->init(nowNs));
    return cell;
}

Result<void> CellServer::init(i64 nowNs) {
    m_now = nowNs;
    // Zone settings are checked up front: an orchestrator assigns zones later, and a zone that
    // then fails to start would leave this cell holding a lease it does not serve (gateways would
    // be refused until the lease moved).
    auto checkRate = [&](u32 hz, std::string_view what) -> Result<void> {
        authority::ZoneClock::Config c;
        c.tickHz = hz;
        c.tidi = m_config.tidi;
        if (auto r = authority::ZoneClock::validate(c); !r)
            return makeError(ErrorCode::InvalidArgument, "{}: {}", what, r.error().message);
        return {};
    };
    HELIOS_TRY(checkRate(m_config.defaultTickHz, "default tick rate"));
    for (const auto& [zone, hz] : m_config.zoneTickHz) HELIOS_TRY(checkRate(hz, "zone '" + zone + "'"));
    for (const StaticZone& z : m_config.staticZones) {
        if (z.id == 0) return makeError(ErrorCode::InvalidArgument, "static zone '{}' has id 0", z.name);
        if (z.tickHz != 0) HELIOS_TRY(checkRate(z.tickHz, "zone '" + z.name + "'"));
    }
    net::ServerConfig sc;
    sc.protocolId = m_config.trunkProtocolId;
    sc.privateKey = m_config.trunkKey;
    sc.bindAddress = m_config.trunkBind;
    if (m_config.trunkPublic.isValid()) sc.publicAddresses = {m_config.trunkPublic};
    sc.maxClients = m_config.maxTrunks;
    sc.connection = net::ConnectionConfig::trunk();
    sc.socket = net::SocketTransportConfig::trunk(m_config.trunkBind);
    sc.transport = std::move(m_config.trunkTransport);
    sc.name = m_config.name + "-trunk";
    HELIOS_TRY_ASSIGN(m_trunk, net::Server::create(std::move(sc), seconds(nowNs)));
    m_handler = std::make_unique<TrunkHandler>(*this);

    if (m_config.bus) {
        OrchestratorClientConfig oc;
        oc.shard = m_config.shard;
        oc.info.name = m_config.name;
        oc.info.kind = std::string(orch::kKindCell);
        oc.info.address = trunkAddress().toString();
        oc.info.version = m_config.version;
        oc.info.zones = m_config.declaredZones;
        m_orch = std::make_unique<OrchestratorClient>(*m_config.bus, std::move(oc));
        m_orch->setLoadProvider([this] {
            orch::Load load;
            load.players = static_cast<i64>(m_routes.size());
            load.tickP99Ms = static_cast<f64>(tickP99Ns()) * 1e-6;
            return load;
        });
    } else {
        for (const StaticZone& z : m_config.staticZones) HELIOS_TRY(startZone(z.id, z.name, 0, nowNs));
    }
    HELIOS_LOG_INFO(LogCell, "cell '{}' trunk on {} ({})", m_config.name, trunkAddress().toString(),
                    m_config.bus ? "orchestrated" : "standalone");
    return {};
}

i64 CellServer::tickP99Ns() const {
    // Over the recent ticks of every hosted zone (the process's worst case, 04 §1 ReportLoad).
    std::vector<i64> ticks;
    for (const ZoneInstance* z : m_host.zones()) {
        const std::vector<i64> recent = z->recentTickNs();
        ticks.insert(ticks.end(), recent.begin(), recent.end());
    }
    if (ticks.empty()) return 0;
    const usize idx = std::min(ticks.size() - 1, (ticks.size() * 99 + 99) / 100 - 1);
    std::nth_element(ticks.begin(), ticks.begin() + static_cast<isize>(idx), ticks.end());
    return ticks[idx];
}

net::Address CellServer::trunkAddress() const {
    const auto addrs = m_trunk->publicAddresses();
    return addrs.empty() ? m_config.trunkBind : addrs[0];
}

Result<ZoneInstance*> CellServer::startZone(ZoneId id, std::string name, authority::LeaseGen gen, i64 nowNs) {
    ZoneDesc d;
    d.id = id;
    d.name = std::move(name);
    auto hz = m_config.zoneTickHz.find(d.name);
    d.tickHz = hz != m_config.zoneTickHz.end() ? hz->second : m_config.defaultTickHz;
    d.leaseGen = gen;
    d.tidi = m_config.tidi;
    d.maxSessions = m_config.maxSessionsPerZone;
    d.cell = authority::CellId{m_orch ? m_orch->processId() : 0};
    OrchestratorIdBlockSource* source = nullptr;
    if (m_orch) {
        auto src = std::make_unique<OrchestratorIdBlockSource>(*m_orch);
        source = src.get();
        m_idSources[id] = std::move(src);
        d.idBlocks = source;
        d.idShard = m_orch->idShard();
        d.onTickStart = [source] { source->pump(); };
    } else {
        d.idShard = m_config.idShard;
        for (const StaticZone& z : m_config.staticZones)
            if (z.id == id && z.tickHz) d.tickHz = z.tickHz;
    }
    auto zone = m_host.addZone(std::move(d), nowNs);
    if (!zone) {
        m_idSources.erase(id);
        return zone.error();
    }
    if (source) source->attach(&(*zone)->world().idMinter());
    if (m_config.onZoneUp) m_config.onZoneUp(**zone);
    return zone;
}

void CellServer::stopZone(ZoneId id, authority::LeaseGen gen) {
    if (!m_host.removeZone(id)) return;
    m_idSources.erase(id);
    ++m_stats.zonesFenced;
    // Every session of the zone is gone from this cell; gateways re-resolve them.
    for (auto it = m_routes.begin(); it != m_routes.end();) {
        if (it->second.zone == id) it = m_routes.erase(it);
        else ++it;
    }
    const std::vector<u8> msg = proto::encode(0, proto::ZoneFenced{id, gen});
    for (const net::SessionHandle t : m_trunk->sessions()) (void)m_trunk->send(t, net::Channel::Control, msg);
}

void CellServer::applyLeaseEvents(const std::vector<authority::LeaseEvent>& events, i64 nowNs) {
    for (const authority::LeaseEvent& e : events) {
        const ZoneId id = e.assignment.region.value;
        if (e.kind == authority::LeaseEventKind::Lost) {
            stopZone(id, e.assignment.leaseGen);
        } else if (auto z = startZone(id, e.assignment.name, e.assignment.leaseGen, nowNs); !z) {
            HELIOS_LOG_ERROR(LogCell, "cannot host zone {} '{}': {}", id, e.assignment.name, z.error().message);
        }
    }
}

ZoneInstance* CellServer::zoneForAttach(u64 zoneId) {
    if (zoneId != 0) return m_host.find(zoneId);
    auto zones = m_host.zones();
    return zones.empty() ? nullptr : zones.front(); // the default zone: the lowest id
}

void CellServer::onTrunkMessage(net::SessionHandle trunk, std::span<const u8> payload) {
    ++m_stats.trunkMessages;
    auto msg = proto::parseTrunk(payload);
    if (!msg) {
        ++m_stats.trunkMalformed;
        return;
    }
    const u64 stream = msg->stream;
    switch (msg->type) {
    case proto::TrunkType::Hello: {
        auto hello = proto::decodeHello(msg->body);
        if (!hello) {
            ++m_stats.trunkMalformed;
            return;
        }
        TrunkPeer& peer = m_peers[trunk.toBits()];
        peer.hello = true;
        peer.name = hello->name;
        peer.processId = hello->processId;
        proto::Welcome w;
        w.processId = m_orch ? m_orch->processId() : 0;
        w.epoch = m_orch ? m_orch->epoch() : 0;
        w.name = m_config.name;
        (void)m_trunk->send(trunk, net::Channel::Control, proto::encode(0, w));
        HELIOS_LOG_INFO(LogCell, "trunk #{} is gateway '{}' (process {})", trunk.index(), peer.name, peer.processId);
        return;
    }
    case proto::TrunkType::Attach: {
        auto a = proto::decodeAttach(msg->body);
        if (!a) {
            ++m_stats.trunkMalformed;
            return;
        }
        ZoneInstance* zone = zoneForAttach(a->zoneId);
        if (!zone) {
            ++m_stats.attachNotHosted;
            (void)m_trunk->send(trunk, net::Channel::EventReliable,
                                proto::encode(stream, proto::AttachNack{a->sessionEpoch, a->zoneId, proto::NackReason::NotHosted}));
            return;
        }
        auto it = m_routes.find(stream);
        if (it != m_routes.end() && a->sessionEpoch < it->second.epoch) {
            (void)m_trunk->send(trunk, net::Channel::EventReliable,
                                proto::encode(stream, proto::AttachNack{a->sessionEpoch, zone->id(), proto::NackReason::StaleEpoch}));
            return;
        }
        if (it != m_routes.end() && it->second.zone != zone->id()) {
            // Moving zones through a re-attach: the old zone drops its copy.
            if (ZoneInstance* old = m_host.find(it->second.zone)) {
                ZoneInboxItem d;
                d.kind = ZoneInboxItem::Kind::Detach;
                d.route = it->second.trunk.toBits();
                d.sessionId = stream;
                d.sessionEpoch = it->second.epoch;
                d.detachReason = proto::DetachReason::Superseded;
                old->post(std::move(d));
            }
        }
        m_routes[stream] = Route{zone->id(), trunk, a->sessionEpoch};
        ZoneInboxItem item;
        item.kind = ZoneInboxItem::Kind::Attach;
        item.route = trunk.toBits();
        item.sessionId = stream;
        item.sessionEpoch = a->sessionEpoch;
        item.accountId = a->accountId;
        item.characterId = a->characterId;
        item.flags = a->flags;
        zone->post(std::move(item));
        return;
    }
    case proto::TrunkType::Detach: {
        auto d = proto::decodeDetach(msg->body);
        if (!d) {
            ++m_stats.trunkMalformed;
            return;
        }
        auto it = m_routes.find(stream);
        if (it == m_routes.end() || it->second.trunk != trunk || d->sessionEpoch < it->second.epoch) return;
        if (ZoneInstance* zone = m_host.find(it->second.zone)) {
            ZoneInboxItem item;
            item.kind = ZoneInboxItem::Kind::Detach;
            item.route = trunk.toBits();
            item.sessionId = stream;
            item.sessionEpoch = d->sessionEpoch;
            item.detachReason = d->reason;
            zone->post(std::move(item));
        }
        m_routes.erase(it);
        return;
    }
    case proto::TrunkType::Forward: {
        auto f = proto::decodeForward(msg->body);
        if (!f) {
            ++m_stats.trunkMalformed;
            return;
        }
        auto it = m_routes.find(stream);
        if (it == m_routes.end() || it->second.trunk != trunk) {
            ++m_stats.forwardsDropped; // not this trunk's session (any more): fenced
            return;
        }
        ZoneInstance* zone = m_host.find(it->second.zone);
        if (!zone) {
            ++m_stats.forwardsDropped;
            return;
        }
        ZoneInboxItem item;
        item.kind = ZoneInboxItem::Kind::Message;
        item.route = trunk.toBits();
        item.sessionId = stream;
        item.sessionEpoch = it->second.epoch;
        item.channel = f->channel;
        item.payload.assign(f->payload.begin(), f->payload.end());
        if (!zone->post(std::move(item))) ++m_stats.forwardsDropped; // the zone's inbox is full
        return;
    }
    default: ++m_stats.trunkMalformed; return; // cell -> gateway types are not accepted here
    }
}

void CellServer::onTrunkDisconnected(net::SessionHandle trunk) {
    ++m_stats.trunksDisconnected;
    m_peers.erase(trunk.toBits());
    for (auto it = m_routes.begin(); it != m_routes.end();) {
        if (it->second.trunk != trunk) {
            ++it;
            continue;
        }
        if (ZoneInstance* zone = m_host.find(it->second.zone)) {
            ZoneInboxItem item;
            item.kind = ZoneInboxItem::Kind::Detach;
            item.route = trunk.toBits();
            item.sessionId = it->first;
            item.sessionEpoch = it->second.epoch;
            item.detachReason = proto::DetachReason::TimedOut;
            zone->post(std::move(item));
        }
        it = m_routes.erase(it);
    }
}

void CellServer::flushZoneOutput() {
    for (ZoneInstance* zone : m_host.zones()) {
        for (ZoneOutMessage& m : zone->takeOutbox()) {
            const net::SessionHandle trunk = net::SessionHandle::fromBits(m.route);
            net::SendResult r = net::SendResult::Ok;
            switch (m.kind) {
            case ZoneOutMessage::Kind::Deliver:
                r = m_trunk->send(trunk, proto::trunkChannelFor(m.channel),
                                  proto::encodeDeliver(m.sessionId, zone->leaseGen(), m.channel, m.payload));
                ++m_stats.deliveries;
                break;
            case ZoneOutMessage::Kind::AttachAck: r = m_trunk->send(trunk, net::Channel::EventReliable, m.payload); break;
            case ZoneOutMessage::Kind::AttachNack: {
                r = m_trunk->send(trunk, net::Channel::EventReliable, m.payload);
                auto it = m_routes.find(m.sessionId);
                if (it != m_routes.end() && it->second.trunk == trunk && it->second.zone == zone->id()) m_routes.erase(it);
                break;
            }
            }
            if (r != net::SendResult::Ok) ++m_stats.sendFailures;
        }
    }
}

void CellServer::update(i64 nowNs) {
    m_now = nowNs;
    m_trunk->update(seconds(nowNs), *m_handler);
    if (m_orch) applyLeaseEvents(m_orch->update(nowNs), nowNs);
    if (auto r = m_host.runDue(nowNs); !r) HELIOS_LOG_ERROR(LogCell, "zone tick failed: {}", r.error().message);
    flushZoneOutput();
    m_trunk->flush(seconds(nowNs));
    if (m_config.statsLogIntervalMs > 0 && nowNs >= m_nextStatsLogNs) {
        if (m_nextStatsLogNs != 0)
            for (ZoneInstance* z : m_host.zones()) {
                const ZoneStats& st = z->stats();
                HELIOS_LOG_INFO(LogCell, "zone {} '{}': tick {}, {} session(s), tick avg {:.1f} us / max {:.1f} us, d {:.2f}",
                                z->id(), z->name(), z->clock().tick(), z->sessions().size(),
                                st.ticks ? static_cast<f64>(st.totalTickNs) / static_cast<f64>(st.ticks) * 1e-3 : 0.0,
                                static_cast<f64>(st.maxTickNs) * 1e-3, z->clock().dilation());
            }
        m_nextStatsLogNs = nowNs + m_config.statsLogIntervalMs * 1'000'000;
    }
}

i64 CellServer::nextWakeNs(i64 nowNs) const {
    return std::min(m_host.nextDeadlineNs(), nowNs + 1'000'000);
}

void CellServer::run(const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_relaxed)) {
        const i64 now = static_cast<i64>(monotonicNanos());
        update(now);
        // Trunk IO is polled every millisecond; a zone tick due within that window gets a precise
        // wait (OS sleep, then a short spin), so ticks start on time without spinning otherwise.
        const i64 after = static_cast<i64>(monotonicNanos());
        const i64 tickDue = m_host.nextDeadlineNs();
        if (tickDue <= after + 1'000'000) {
            if (tickDue > after) sleepPrecise(static_cast<u64>(tickDue - after));
        } else {
            sleepNanos(1'000'000);
        }
    }
    shutdown(static_cast<i64>(monotonicNanos()), std::chrono::milliseconds(2000));
}

void CellServer::shutdown(i64 nowNs, std::chrono::milliseconds timeout) {
    std::vector<authority::LeaseEvent> events;
    if (m_orch) {
        events = m_orch->shutdown(timeout);
    } else {
        for (ZoneInstance* z : m_host.zones())
            events.push_back(authority::LeaseEvent{authority::LeaseEventKind::Lost,
                                                   {authority::RegionId{z->id()}, z->name(), z->leaseGen()},
                                                   authority::LeaseLossReason::Released});
    }
    applyLeaseEvents(events, nowNs);
    m_trunk->flush(seconds(nowNs));
    m_trunk->disconnectAll();
    m_trunk->update(seconds(nowNs), *m_handler);
    m_trunk->flush(seconds(nowNs));
}

} // namespace helios::server
