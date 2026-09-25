#include "helios/authority/lease.h"

#include "authority_log.h"

namespace helios::authority {

std::string_view leaseLossReasonName(LeaseLossReason reason) noexcept {
    switch (reason) {
    case LeaseLossReason::None: return "none";
    case LeaseLossReason::LeaseLost: return "lease_lost";
    case LeaseLossReason::Unassigned: return "unassigned";
    case LeaseLossReason::GenerationChanged: return "generation_changed";
    case LeaseLossReason::HigherGenerationSeen: return "higher_generation_seen";
    case LeaseLossReason::CheckpointRejected: return "checkpoint_rejected";
    case LeaseLossReason::Released: return "released";
    }
    return "?";
}

LeaseEvent LeaseHolder::lose(std::map<RegionId, Held>::iterator it, LeaseLossReason reason) {
    LeaseEvent e{LeaseEventKind::Lost, it->second.assignment, reason};
    HELIOS_LOG_INFO(LogAuthority, "region {} ('{}') lease_gen {} lost: {}", e.assignment.region.value,
                    e.assignment.name, e.assignment.leaseGen, leaseLossReasonName(reason));
    m_held.erase(it);
    ++m_stats.lost;
    if (reason != LeaseLossReason::LeaseLost && reason != LeaseLossReason::Released)
        raiseFloor(e.assignment.region, e.assignment.leaseGen + 1);
    return e;
}

void LeaseHolder::raiseFloor(RegionId region, LeaseGen gen) {
    LeaseGen& f = m_floor[region];
    if (gen > f) f = gen;
}

LeaseGen LeaseHolder::generationFloor(RegionId region) const {
    auto it = m_floor.find(region);
    return it == m_floor.end() ? 0 : it->second;
}

std::vector<LeaseEvent> LeaseHolder::onAssignments(std::span<const RegionAssignment> assignments) {
    std::vector<LeaseEvent> lost;
    std::vector<LeaseEvent> acquired;
    std::map<RegionId, const RegionAssignment*> incoming;
    for (const RegionAssignment& a : assignments) {
        if (!a.region.isValid()) continue;
        if (a.leaseGen < generationFloor(a.region)) {
            ++m_stats.staleAssignmentsIgnored; // a generation this registration already gave up
            continue;
        }
        auto it = m_held.find(a.region);
        if (it != m_held.end() && a.leaseGen < it->second.assignment.leaseGen) {
            ++m_stats.staleAssignmentsIgnored; // an older reply; generations only grow in PG
            incoming[a.region] = &it->second.assignment;
            continue;
        }
        incoming[a.region] = &a;
    }
    // Losses first: regions no longer listed, or listed at a new generation.
    for (auto it = m_held.begin(); it != m_held.end();) {
        auto in = incoming.find(it->first);
        if (in == incoming.end()) {
            lost.push_back(lose(it++, LeaseLossReason::Unassigned));
        } else if (in->second->leaseGen != it->second.assignment.leaseGen) {
            lost.push_back(lose(it++, LeaseLossReason::GenerationChanged));
        } else {
            it->second.verify = false; // the orchestrator confirmed the current generation
            if (in->second->name != it->second.assignment.name) it->second.assignment.name = in->second->name;
            ++it;
        }
    }
    for (const auto& [region, a] : incoming) {
        if (m_held.contains(region)) continue;
        m_held.emplace(region, Held{*a, false});
        ++m_stats.acquired;
        HELIOS_LOG_INFO(LogAuthority, "region {} ('{}') acquired at lease_gen {}", region.value, a->name, a->leaseGen);
        acquired.push_back(LeaseEvent{LeaseEventKind::Acquired, *a, LeaseLossReason::None});
    }
    lost.insert(lost.end(), acquired.begin(), acquired.end());
    return lost;
}

std::vector<LeaseEvent> LeaseHolder::onLeaseLost() {
    ++m_stats.leaseLost;
    std::vector<LeaseEvent> out;
    while (!m_held.empty()) out.push_back(lose(m_held.begin(), LeaseLossReason::LeaseLost));
    m_floor.clear(); // the next registration starts afresh
    return out;
}

std::vector<LeaseEvent> LeaseHolder::onHigherGeneration(RegionId region, LeaseGen seen) {
    std::vector<LeaseEvent> out;
    auto it = m_held.find(region);
    if (it != m_held.end() && seen > it->second.assignment.leaseGen)
        out.push_back(lose(it, LeaseLossReason::HigherGenerationSeen));
    if (region.isValid()) raiseFloor(region, seen); // never (re)acquire below what exists elsewhere
    return out;
}

std::vector<LeaseEvent> LeaseHolder::onRegionCheckpointRejected(RegionId region) {
    std::vector<LeaseEvent> out;
    auto it = m_held.find(region);
    if (it != m_held.end()) out.push_back(lose(it, LeaseLossReason::CheckpointRejected));
    return out;
}

void LeaseHolder::onAgFenceRejected(RegionId region) {
    auto it = m_held.find(region);
    if (it != m_held.end()) it->second.verify = true;
}

std::vector<LeaseEvent> LeaseHolder::releaseAll() {
    std::vector<LeaseEvent> out;
    while (!m_held.empty()) out.push_back(lose(m_held.begin(), LeaseLossReason::Released));
    m_floor.clear();
    return out;
}

bool LeaseHolder::holds(RegionId region, LeaseGen gen) const {
    auto it = m_held.find(region);
    return it != m_held.end() && it->second.assignment.leaseGen == gen;
}

std::optional<LeaseGen> LeaseHolder::generation(RegionId region) const {
    auto it = m_held.find(region);
    if (it == m_held.end()) return std::nullopt;
    return it->second.assignment.leaseGen;
}

bool LeaseHolder::needsVerification(RegionId region) const {
    auto it = m_held.find(region);
    return it != m_held.end() && it->second.verify;
}

std::vector<RegionAssignment> LeaseHolder::held() const {
    std::vector<RegionAssignment> out;
    for (const auto& [region, h] : m_held) out.push_back(h.assignment);
    return out;
}

} // namespace helios::authority
