#include "helios/server/zone_host.h"

#include <limits>

#include "server_log.h"

namespace helios::server {

ZoneHost::ZoneHost(jobs::JobSystem* jobs) : m_jobs(jobs) {}
ZoneHost::~ZoneHost() = default;

Result<ZoneInstance*> ZoneHost::addZone(ZoneDesc desc, i64 wallNowNs) {
    if (m_zones.contains(desc.id)) return makeError(ErrorCode::AlreadyExists, "zone {} is already hosted", desc.id);
    HELIOS_TRY_ASSIGN(std::unique_ptr<ZoneInstance> zone, ZoneInstance::create(std::move(desc), m_jobs));
    zone->clock().start(wallNowNs);
    ZoneInstance* raw = zone.get();
    m_zones.emplace(raw->id(), std::move(zone));
    ++m_stats.zonesAdded;
    HELIOS_LOG_INFO(LogCell, "zone {} '{}' up at {} Hz (lease_gen {})", raw->id(), raw->name(), raw->desc().tickHz,
                    raw->leaseGen());
    return raw;
}

bool ZoneHost::removeZone(ZoneId id) {
    auto it = m_zones.find(id);
    if (it == m_zones.end()) return false;
    HELIOS_LOG_INFO(LogCell, "zone {} '{}' down after {} ticks", id, it->second->name(), it->second->stats().ticks);
    m_zones.erase(it);
    ++m_stats.zonesRemoved;
    return true;
}

ZoneInstance* ZoneHost::find(ZoneId id) noexcept {
    auto it = m_zones.find(id);
    return it == m_zones.end() ? nullptr : it->second.get();
}

ZoneInstance* ZoneHost::findByName(std::string_view name) noexcept {
    for (auto& [id, z] : m_zones)
        if (z->name() == name) return z.get();
    return nullptr;
}

std::vector<ZoneInstance*> ZoneHost::zones() {
    std::vector<ZoneInstance*> out;
    out.reserve(m_zones.size());
    for (auto& [id, z] : m_zones) out.push_back(z.get());
    return out;
}

std::vector<const ZoneInstance*> ZoneHost::zones() const {
    std::vector<const ZoneInstance*> out;
    out.reserve(m_zones.size());
    for (const auto& [id, z] : m_zones) out.push_back(z.get());
    return out;
}

Result<u32> ZoneHost::runDue(i64 wallNowNs, std::vector<ZoneId>* order) {
    ++m_stats.runDueCalls;
    for (auto& [id, z] : m_zones) z->clock().advanceTo(wallNowNs);
    u32 ran = 0;
    for (;;) {
        ZoneInstance* next = nullptr;
        i64 bestDue = std::numeric_limits<i64>::max();
        for (auto& [id, z] : m_zones) {
            if (!z->clock().isTickDue()) continue;
            const i64 due = z->clock().dueSinceWallNs();
            if (due < bestDue) { // ties keep the lower id (map order)
                bestDue = due;
                next = z.get();
            }
        }
        if (!next) break;
        HELIOS_TRY_ASSIGN(const bool ticked, next->tick(wallNowNs));
        if (!ticked) break; // defensive: isTickDue() said otherwise
        if (order) order->push_back(next->id());
        ++ran;
        ++m_stats.ticks;
    }
    return ran;
}

i64 ZoneHost::nextDeadlineNs() const {
    i64 best = std::numeric_limits<i64>::max();
    for (const auto& [id, z] : m_zones) best = std::min(best, z->clock().nextTickWallNs());
    return best;
}

} // namespace helios::server
