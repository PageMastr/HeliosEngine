#include "helios/authority/memory_fence.h"

#include <algorithm>
#include <set>

namespace helios::authority {

std::string_view advanceCauseName(AdvanceCause cause) noexcept {
    switch (cause) {
    case AdvanceCause::Handoff: return "handoff";
    case AdvanceCause::Load: return "load";
    case AdvanceCause::Takeover: return "takeover";
    case AdvanceCause::Abort: return "abort";
    case AdvanceCause::Join: return "join";
    case AdvanceCause::Leave: return "leave";
    case AdvanceCause::Park: return "park";
    case AdvanceCause::Recovery: return "recovery";
    case AdvanceCause::Migrate: return "migrate";
    }
    return "?";
}

std::string_view fenceStatusName(FenceStatus status) noexcept {
    switch (status) {
    case FenceStatus::Ok: return "OK";
    case FenceStatus::Stale: return "FENCE_STALE";
    case FenceStatus::TreeMismatch: return "FENCE_TREE_MISMATCH";
    case FenceStatus::NotFound: return "NOT_FOUND";
    case FenceStatus::NotRoot: return "NOT_ROOT";
    case FenceStatus::NeedsTakeover: return "NEEDS_TAKEOVER";
    case FenceStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case FenceStatus::Unavailable: return "UNAVAILABLE";
    }
    return "?";
}

InMemoryFence::InMemoryFence() : InMemoryFence(Config{}) {}
InMemoryFence::InMemoryFence(const Config& config) : m_config(config) {}
InMemoryFence::~InMemoryFence() = default;

Result<void> InMemoryFence::createRow(AgId ag, Epoch epoch) {
    if (!ag.isValid()) return Error{ErrorCode::InvalidArgument, "invalid AG id"};
    std::lock_guard lock(m_mutex);
    if (m_rows.contains(ag)) return makeError(ErrorCode::AlreadyExists, "fence row {} exists", ag.value);
    FenceRow row;
    row.ag = ag;
    row.root = ag;
    row.epoch = epoch;
    row.state = RowState::Dormant;
    m_rows.emplace(ag, row);
    return {};
}

std::optional<FenceRow> InMemoryFence::row(AgId ag) const {
    std::lock_guard lock(m_mutex);
    auto it = m_rows.find(ag);
    if (it == m_rows.end()) return std::nullopt;
    return it->second;
}

std::vector<FenceRow> InMemoryFence::tree(AgId root) const {
    std::lock_guard lock(m_mutex);
    std::vector<FenceRow> out;
    for (const auto& [id, r] : m_rows)
        if (r.root == root) out.push_back(r);
    return out;
}

std::vector<FenceEvent> InMemoryFence::events() const {
    std::lock_guard lock(m_mutex);
    return m_events;
}

std::vector<ReleasedNotice> InMemoryFence::releasedNotices() const {
    std::lock_guard lock(m_mutex);
    return m_released;
}

void InMemoryFence::setReleasedHandler(std::function<void(const ReleasedNotice&)> handler) {
    std::lock_guard lock(m_mutex);
    m_releasedHandler = std::move(handler);
}

void InMemoryFence::setDeferred(bool deferred) {
    std::lock_guard lock(m_mutex);
    m_config.deferred = deferred;
}

usize InMemoryFence::pendingOps() const {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

void InMemoryFence::setUnavailable(bool unavailable) {
    std::lock_guard lock(m_mutex);
    m_unavailable = unavailable;
}

u32 InMemoryFence::pump(u32 maxOps) {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lock(m_mutex);
        const usize n = std::min<usize>(maxOps, m_queue.size());
        batch.assign(std::make_move_iterator(m_queue.begin()), std::make_move_iterator(m_queue.begin() + static_cast<isize>(n)));
        m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<isize>(n));
    }
    for (auto& op : batch) op();
    return static_cast<u32>(batch.size());
}

template <class R, class Op>
Future<R> InMemoryFence::submit(Op op) {
    Promise<R> promise;
    Future<R> future = promise.future();
    std::function<void()> run = [this, promise, op = std::move(op)]() mutable {
        Effects fx;
        R result;
        {
            std::lock_guard lock(m_mutex);
            if (m_unavailable) {
                result.status = FenceStatus::Unavailable;
            } else {
                result = op(fx);
            }
        }
        deliver(fx);
        promise.set(std::move(result));
    };
    bool defer = false;
    {
        std::lock_guard lock(m_mutex);
        defer = m_config.deferred;
        if (defer) m_queue.push_back(std::move(run));
    }
    if (!defer) run();
    return future;
}

void InMemoryFence::deliver(Effects& fx) {
    if (fx.released.empty()) return;
    std::function<void(const ReleasedNotice&)> handler;
    {
        std::lock_guard lock(m_mutex);
        handler = m_releasedHandler;
    }
    if (!handler) return;
    for (const ReleasedNotice& n : fx.released) handler(n);
}

// ---------------------------------------------------------------------------------------------
// Tree helpers (lock held)
// ---------------------------------------------------------------------------------------------

FenceStatus InMemoryFence::collectTree(AgId root, std::vector<FenceRow*>& rows, Epoch& epoch) {
    rows.clear();
    auto it = m_rows.find(root);
    if (it == m_rows.end()) return FenceStatus::NotFound;
    if (it->second.root != root) return FenceStatus::NotRoot;
    for (auto& [id, r] : m_rows)
        if (r.root == root) rows.push_back(&r);
    const FenceRow& head = it->second;
    epoch = head.epoch;
    for (const FenceRow* r : rows)
        if (r->epoch != head.epoch || r->state != head.state || !(r->owner == head.owner))
            return FenceStatus::TreeMismatch;
    return FenceStatus::Ok;
}

std::vector<FenceRow*> InMemoryFence::subtree(AgId member) {
    // Rows whose parent chain reaches `member` (including it), found by repeated expansion.
    std::set<AgId> in{member};
    bool grew = true;
    while (grew) {
        grew = false;
        for (auto& [id, r] : m_rows)
            if (r.parent.isValid() && in.contains(r.parent) && !in.contains(id)) {
                in.insert(id);
                grew = true;
            }
    }
    std::vector<FenceRow*> out;
    for (AgId id : in) out.push_back(&m_rows.at(id));
    return out;
}

u32 InMemoryFence::depthOf(const FenceRow& row) const {
    u32 depth = 1;
    AgId p = row.parent;
    while (p.isValid() && depth < 64) {
        ++depth;
        auto it = m_rows.find(p);
        if (it == m_rows.end()) break;
        p = it->second.parent;
    }
    return depth;
}

u32 InMemoryFence::subtreeHeight(AgId member) const {
    u32 height = 1;
    for (const auto& [id, r] : m_rows) {
        u32 d = 1;
        AgId p = r.parent;
        AgId cur = id;
        while (cur != member && p.isValid() && d < 64) {
            cur = p;
            auto it = m_rows.find(p);
            p = it == m_rows.end() ? AgId{} : it->second.parent;
            ++d;
        }
        if (cur == member) height = std::max(height, d);
    }
    return height;
}

void InMemoryFence::moveTree(std::vector<FenceRow*>& rows, Epoch next, const Owner& owner, RowState state) {
    for (FenceRow* r : rows) {
        r->epoch = next;
        r->owner = owner;
        r->state = state;
    }
}

void InMemoryFence::emit(AgId ag, Epoch epoch, const Owner& prev, const Owner& owner, AdvanceCause cause, Effects& fx) {
    m_events.push_back(FenceEvent{ag, epoch, prev, owner, cause});
    const bool supersedes = cause == AdvanceCause::Load || cause == AdvanceCause::Takeover || cause == AdvanceCause::Recovery;
    if (supersedes && prev.isValid() && prev.cell != owner.cell) {
        ReleasedNotice n{prev.cell, ag, epoch};
        m_released.push_back(n);
        fx.released.push_back(n);
    }
}

// ---------------------------------------------------------------------------------------------
// Operations (lock held)
// ---------------------------------------------------------------------------------------------

FenceResult InMemoryFence::doAdvance(AgId root, Epoch expect, Epoch next, const Owner& owner, AdvanceCause cause,
                                     Effects& fx) {
    if (next != expect + 1 || !owner.isValid()) return {FenceStatus::InvalidArgument, 0, {}};
    if (cause != AdvanceCause::Handoff && cause != AdvanceCause::Load && cause != AdvanceCause::Takeover &&
        cause != AdvanceCause::Abort)
        return {FenceStatus::InvalidArgument, 0, {}};
    std::vector<FenceRow*> rows;
    Epoch current = 0;
    const FenceStatus s = collectTree(root, rows, current);
    if (s != FenceStatus::Ok) return {s, current, {}};
    const FenceRow& head = m_rows.at(root);
    const Owner prev = head.owner;
    if (current != expect) return {FenceStatus::Stale, current, prev};
    const bool active = head.state == RowState::Active;
    switch (cause) {
    case AdvanceCause::Load:
        if (active) return {FenceStatus::NeedsTakeover, current, prev};
        break;
    case AdvanceCause::Takeover: break;
    default:
        if (!active) return {FenceStatus::Stale, current, prev};
        break;
    }
    moveTree(rows, next, owner, RowState::Active);
    emit(root, next, prev, owner, cause, fx);
    return {FenceStatus::Ok, next, prev};
}

FenceResult InMemoryFence::doPark(AgId root, Epoch e, const Owner& owner, StreamSeq seq, Effects& fx) {
    std::vector<FenceRow*> rows;
    Epoch current = 0;
    const FenceStatus s = collectTree(root, rows, current);
    if (s != FenceStatus::Ok) return {s, current, {}};
    const FenceRow& head = m_rows.at(root);
    const Owner prev = head.owner;
    if (current != e || head.state != RowState::Active || head.owner.cell != owner.cell)
        return {FenceStatus::Stale, current, prev};
    moveTree(rows, e + 1, Owner{}, RowState::Dormant);
    for (FenceRow* r : rows) r->parkedSeq = seq;
    emit(root, e + 1, prev, Owner{}, AdvanceCause::Park, fx);
    return {FenceStatus::Ok, e + 1, prev};
}

FenceResult InMemoryFence::doJoin(AgId member, Epoch em, AgId root, Epoch er, const Owner& owner, Effects& fx) {
    if (member == root || !owner.isValid()) return {FenceStatus::InvalidArgument, 0, {}};
    std::vector<FenceRow*> memberRows;
    std::vector<FenceRow*> rootRows;
    Epoch cm = 0;
    Epoch cr = 0;
    FenceStatus s = collectTree(member, memberRows, cm);
    if (s != FenceStatus::Ok) return {s, cm, {}};
    s = collectTree(root, rootRows, cr);
    if (s != FenceStatus::Ok) return {s, cr, {}};
    const FenceRow& m = m_rows.at(member);
    const FenceRow& r = m_rows.at(root);
    const bool memberOwned = m.state == RowState::Active && m.owner.cell == owner.cell;
    const bool rootOwned = r.state == RowState::Active && r.owner.cell == owner.cell;
    if (cm != em || cr != er || !memberOwned || !rootOwned) return {FenceStatus::Stale, cm != em ? cm : cr, r.owner};
    if (1 + subtreeHeight(member) > m_config.maxTreeDepth) return {FenceStatus::InvalidArgument, cr, r.owner};
    const Owner prev = r.owner;
    const Epoch next = std::max(em, er) + 1;
    for (FenceRow* row : memberRows) row->root = root;
    m_rows.at(member).parent = root;
    moveTree(memberRows, next, r.owner, RowState::Active);
    moveTree(rootRows, next, r.owner, RowState::Active);
    emit(root, next, prev, prev, AdvanceCause::Join, fx);
    emit(member, next, prev, prev, AdvanceCause::Join, fx);
    return {FenceStatus::Ok, next, prev};
}

FenceResult InMemoryFence::doLeave(AgId member, Epoch e, const Owner& owner, Effects& fx) {
    auto it = m_rows.find(member);
    if (it == m_rows.end()) return {FenceStatus::NotFound, 0, {}};
    if (it->second.root == member) return {FenceStatus::InvalidArgument, it->second.epoch, it->second.owner};
    const AgId root = it->second.root;
    std::vector<FenceRow*> rows;
    Epoch current = 0;
    const FenceStatus s = collectTree(root, rows, current);
    if (s != FenceStatus::Ok) return {s, current, {}};
    const FenceRow& head = m_rows.at(root);
    const Owner prev = head.owner;
    if (current != e || head.state != RowState::Active || head.owner.cell != owner.cell)
        return {FenceStatus::Stale, current, prev};
    std::vector<FenceRow*> sub = subtree(member);
    for (FenceRow* row : sub) row->root = member;
    m_rows.at(member).parent = AgId{};
    moveTree(rows, e + 1, prev, RowState::Active); // `rows` still covers both trees' rows
    emit(root, e + 1, prev, prev, AdvanceCause::Leave, fx);
    emit(member, e + 1, prev, prev, AdvanceCause::Leave, fx);
    return {FenceStatus::Ok, e + 1, prev};
}

BulkFenceResult InMemoryFence::doAdvanceMany(const std::vector<AgEpoch>& roots, const Owner& owner, Effects& fx) {
    BulkFenceResult out;
    if (roots.empty() || !owner.isValid()) {
        out.status = FenceStatus::InvalidArgument;
        return out;
    }
    std::set<AgId> seen;
    std::vector<std::vector<FenceRow*>> trees;
    for (const AgEpoch& ae : roots) {
        if (!seen.insert(ae.ag).second) {
            out.status = FenceStatus::InvalidArgument;
            return out;
        }
        std::vector<FenceRow*> rows;
        Epoch current = 0;
        const FenceStatus s = collectTree(ae.ag, rows, current);
        if (s != FenceStatus::Ok) {
            out.status = s;
            return out;
        }
        if (current != ae.epoch || m_rows.at(ae.ag).state != RowState::Active) {
            out.status = FenceStatus::Stale;
            return out;
        }
        trees.push_back(std::move(rows));
    }
    for (usize i = 0; i < roots.size(); ++i) {
        const Owner prev = m_rows.at(roots[i].ag).owner;
        moveTree(trees[i], roots[i].epoch + 1, owner, RowState::Active);
        emit(roots[i].ag, roots[i].epoch + 1, prev, owner, AdvanceCause::Handoff, fx);
        out.advanced.push_back(AgEpoch{roots[i].ag, roots[i].epoch + 1});
    }
    return out;
}

BulkFenceResult InMemoryFence::doAdvanceOwnedBy(RegionId region, LeaseGen gen, const Owner& owner, Effects& fx) {
    BulkFenceResult out;
    if (!region.isValid() || !owner.isValid()) {
        out.status = FenceStatus::InvalidArgument;
        return out;
    }
    std::vector<AgId> roots;
    for (const auto& [id, r] : m_rows)
        if (r.root == id && r.state == RowState::Active && r.owner.region == region && r.owner.leaseGen <= gen)
            roots.push_back(id);
    for (AgId root : roots) {
        std::vector<FenceRow*> rows;
        Epoch current = 0;
        if (collectTree(root, rows, current) != FenceStatus::Ok) continue; // mismatched trees alert separately
        const Owner prev = m_rows.at(root).owner;
        moveTree(rows, current + 1, owner, RowState::Active);
        emit(root, current + 1, prev, owner, AdvanceCause::Recovery, fx);
        out.advanced.push_back(AgEpoch{root, current + 1});
    }
    return out;
}

BulkFenceResult InMemoryFence::doMigrate(RegionId region, LeaseGen next, const Owner& to, const AgManifest& m,
                                         Effects& fx) {
    BulkFenceResult out;
    if (!region.isValid() || !to.isValid() || to.leaseGen != next || to.region != region) {
        out.status = FenceStatus::InvalidArgument;
        return out;
    }
    std::set<AgId> active;
    for (const auto& [id, r] : m_rows)
        if (r.root == id && r.state == RowState::Active && r.owner.region == region) active.insert(id);
    std::set<AgId> listed;
    for (const AgEpoch& ae : m.roots) listed.insert(ae.ag);
    if (listed != active || listed.size() != m.roots.size()) {
        out.status = FenceStatus::Stale;
        return out;
    }
    std::vector<std::vector<FenceRow*>> trees;
    for (const AgEpoch& ae : m.roots) {
        std::vector<FenceRow*> rows;
        Epoch current = 0;
        const FenceStatus s = collectTree(ae.ag, rows, current);
        if (s != FenceStatus::Ok) {
            out.status = s;
            return out;
        }
        if (current != ae.epoch) {
            out.status = FenceStatus::Stale;
            return out;
        }
        trees.push_back(std::move(rows));
    }
    for (usize i = 0; i < m.roots.size(); ++i) {
        const Owner prev = m_rows.at(m.roots[i].ag).owner;
        moveTree(trees[i], m.roots[i].epoch + 1, to, RowState::Active);
        emit(m.roots[i].ag, m.roots[i].epoch + 1, prev, to, AdvanceCause::Migrate, fx);
        out.advanced.push_back(AgEpoch{m.roots[i].ag, m.roots[i].epoch + 1});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// IFence
// ---------------------------------------------------------------------------------------------

Future<FenceResult> InMemoryFence::advance(AgId root, Epoch expect, Epoch next, const Owner& owner, AdvanceCause cause) {
    return submit<FenceResult>([=, this](Effects& fx) { return doAdvance(root, expect, next, owner, cause, fx); });
}

Future<FenceResult> InMemoryFence::park(AgId root, Epoch e, const Owner& owner, StreamSeq finalCheckpoint) {
    return submit<FenceResult>([=, this](Effects& fx) { return doPark(root, e, owner, finalCheckpoint, fx); });
}

Future<FenceResult> InMemoryFence::join(AgId member, Epoch em, AgId root, Epoch er, const Owner& owner) {
    return submit<FenceResult>([=, this](Effects& fx) { return doJoin(member, em, root, er, owner, fx); });
}

Future<FenceResult> InMemoryFence::leave(AgId member, Epoch e, const Owner& owner) {
    return submit<FenceResult>([=, this](Effects& fx) { return doLeave(member, e, owner, fx); });
}

Future<BulkFenceResult> InMemoryFence::advanceMany(std::span<const AgEpoch> roots, const Owner& owner) {
    std::vector<AgEpoch> copy(roots.begin(), roots.end());
    return submit<BulkFenceResult>([this, copy = std::move(copy), owner](Effects& fx) { return doAdvanceMany(copy, owner, fx); });
}

Future<BulkFenceResult> InMemoryFence::advanceOwnedBy(RegionId region, LeaseGen leaseGen, const Owner& owner) {
    return submit<BulkFenceResult>([=, this](Effects& fx) { return doAdvanceOwnedBy(region, leaseGen, owner, fx); });
}

Future<BulkFenceResult> InMemoryFence::migrateRegion(RegionId region, LeaseGen next, const Owner& to,
                                                     const AgManifest& fromSource) {
    return submit<BulkFenceResult>(
        [this, region, next, to, m = fromSource](Effects& fx) { return doMigrate(region, next, to, m, fx); });
}

} // namespace helios::authority
