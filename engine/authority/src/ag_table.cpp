#include "helios/authority/ag_table.h"

#include <algorithm>

#include "authority_log.h"

namespace helios::authority {

std::string_view agLocalStateName(AgLocalState state) noexcept {
    switch (state) {
    case AgLocalState::Loading: return "Loading";
    case AgLocalState::Active: return "Active";
    case AgLocalState::Parking: return "Parking";
    }
    return "?";
}

AgTable::AgTable(const Owner& self) : m_owner(self) {}

bool AgTable::load(IFence& fence, AgId root, Epoch currentEpoch, bool takeover) {
    if (!root.isValid() || m_records.contains(root)) return false;
    AgRecord rec;
    rec.root = root;
    rec.epoch = currentEpoch;
    rec.state = AgLocalState::Loading;
    rec.fenceOpsInFlight = 1;
    rec.incarnation = ++m_nextIncarnation;
    m_records.emplace(root, rec);
    m_pending.push_back(Pending{root, Op::Load, rec.incarnation,
                                fence.advance(root, currentEpoch, currentEpoch + 1, m_owner,
                                              takeover ? AdvanceCause::Takeover : AdvanceCause::Load)});
    return true;
}

bool AgTable::park(IFence& fence, AgId root, StreamSeq finalCheckpoint) {
    auto it = m_records.find(root);
    if (it == m_records.end() || it->second.state != AgLocalState::Active) return false;
    it->second.state = AgLocalState::Parking;
    ++it->second.fenceOpsInFlight;
    m_pending.push_back(
        Pending{root, Op::Park, it->second.incarnation, fence.park(root, it->second.epoch, m_owner, finalCheckpoint)});
    return true;
}

u32 AgTable::poll() {
    u32 applied = 0;
    for (usize i = 0; i < m_pending.size();) {
        Pending& p = m_pending[i];
        if (!p.reply.isReady()) {
            ++i;
            continue;
        }
        const FenceResult r = p.reply.get();
        auto it = m_records.find(p.root);
        if (it != m_records.end() && it->second.incarnation != p.incarnation) {
            // The tree was dropped and tracked again since this call: its reply says nothing
            // about the current tracking (whose own call is still pending).
            ++m_stats.staleReplies;
        } else if (it == m_records.end()) {
            ++m_stats.staleReplies;
        } else {
            AgRecord& rec = it->second;
            if (rec.fenceOpsInFlight > 0) --rec.fenceOpsInFlight;
            if (!r.ok()) {
                // A failed CAS means someone else owns the tree (or it moved on): drop it at once;
                // the region is re-verified with the orchestrator by the caller (05 §1.4.2).
                HELIOS_LOG_WARN(LogAuthority, "fence {} for AG {} failed: {}", p.op == Op::Load ? "load" : "park",
                                p.root.value, fenceStatusName(r.status));
                ++m_stats.rejected;
                m_records.erase(it);
            } else if (p.op == Op::Load) {
                rec.epoch = r.epoch;
                if (rec.state == AgLocalState::Loading) rec.state = AgLocalState::Active;
                ++m_stats.loaded;
            } else {
                ++m_stats.parked;
                m_records.erase(it);
            }
        }
        m_pending.erase(m_pending.begin() + static_cast<isize>(i));
        ++applied;
    }
    return applied;
}

bool AgTable::onReleased(AgId root, Epoch epoch) {
    auto it = m_records.find(root);
    if (it == m_records.end()) return false;
    // A notice for an epoch we already moved past is stale (e.g. we won the tree back). While
    // loading from epoch e, our own CAS decides for notices up to e + 1.
    const Epoch ours = it->second.state == AgLocalState::Loading ? it->second.epoch + 1 : it->second.epoch;
    if (ours >= epoch) return false;
    m_records.erase(it);
    ++m_stats.released;
    return true;
}

void AgTable::dropAll() {
    m_records.clear();
    m_pending.clear();
}

bool AgTable::isAuthoritative(AgId root) const {
    auto it = m_records.find(root);
    return it != m_records.end() && it->second.state == AgLocalState::Active;
}

std::optional<Epoch> AgTable::epoch(AgId root) const {
    auto it = m_records.find(root);
    if (it == m_records.end()) return std::nullopt;
    return it->second.epoch;
}

std::optional<AgLocalState> AgTable::state(AgId root) const {
    auto it = m_records.find(root);
    if (it == m_records.end()) return std::nullopt;
    return it->second.state;
}

bool AgTable::ledgerGateOpen(AgId root) const {
    auto it = m_records.find(root);
    return it != m_records.end() && it->second.state == AgLocalState::Active && it->second.fenceOpsInFlight == 0;
}

std::vector<AgEpoch> AgTable::owned() const {
    std::vector<AgEpoch> out;
    for (const auto& [id, rec] : m_records)
        if (rec.state == AgLocalState::Active) out.push_back(AgEpoch{id, rec.epoch});
    std::sort(out.begin(), out.end(), [](const AgEpoch& a, const AgEpoch& b) { return a.ag < b.ag; });
    return out;
}

} // namespace helios::authority
