#include "helios/server/tick_graph.h"

#include <algorithm>
#include <map>

#include "helios/core/jobs.h"
#include "helios/core/time.h"

namespace helios::server {

StageBudgets StageBudgets::forTickHz(u32 tickHz) {
    // 04 §3.3 at 20 Hz: Input 2, PrePhysics 6, Physics 10, PostPhysics 8, AuthorityFlush 1,
    // ReplicationGather 3, ConnectionWrite 3, Send 2 = 35 ms (70 % of the 50 ms tick).
    static constexpr std::array<i64, kStageCount> kMsAt20Hz = {2, 6, 10, 8, 1, 3, 3, 2};
    StageBudgets b;
    const i64 hz = std::max<u32>(1, tickHz);
    for (usize i = 0; i < kStageCount; ++i) b.ns[i] = kMsAt20Hz[i] * 1'000'000 * 20 / hz;
    return b;
}

i64 StageBudgets::totalNs() const noexcept {
    i64 t = 0;
    for (i64 v : ns) t += v;
    return t;
}

TickGraph::TickGraph() : m_budgets(StageBudgets::forTickHz(20)) {}
TickGraph::~TickGraph() = default;

Result<void> TickGraph::addHook(HookDesc desc) {
    if (desc.name.empty() || !desc.fn) return Error{ErrorCode::InvalidArgument, "hook needs a name and a function"};
    if (static_cast<u32>(desc.stage) >= kStageCount) return Error{ErrorCode::InvalidArgument, "bad stage"};
    for (const Hook& h : m_hooks)
        if (h.desc.name == desc.name) return makeError(ErrorCode::AlreadyExists, "hook '{}' exists", desc.name);
    Hook h;
    h.desc = std::move(desc);
    m_hooks.push_back(std::move(h));
    m_built = false;
    return {};
}

namespace {
/// Kahn's algorithm over `nodes` (hook indices) with `deps`; ties by registration order.
Result<std::vector<u32>> topoSort(const std::vector<u32>& nodes, const std::vector<std::vector<u32>>& deps) {
    std::map<u32, u32> indegree;
    std::map<u32, std::vector<u32>> succ;
    for (u32 n : nodes) indegree[n] = 0;
    for (u32 n : nodes)
        for (u32 d : deps[n])
            if (indegree.contains(d)) {
                ++indegree[n];
                succ[d].push_back(n);
            }
    std::vector<u32> order;
    std::vector<u32> ready;
    for (u32 n : nodes)
        if (indegree[n] == 0) ready.push_back(n);
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end(), std::greater<u32>());
        const u32 n = ready.back();
        ready.pop_back();
        order.push_back(n);
        for (u32 s : succ[n])
            if (--indegree[s] == 0) ready.push_back(s);
    }
    if (order.size() != nodes.size()) return Error{ErrorCode::InvalidArgument, "cycle among tick hooks"};
    return order;
}
} // namespace

Result<void> TickGraph::build() {
    std::map<std::string, u32, std::less<>> byName;
    for (u32 i = 0; i < m_hooks.size(); ++i) byName[m_hooks[i].desc.name] = i;
    std::vector<std::vector<u32>> deps(m_hooks.size());
    for (u32 i = 0; i < m_hooks.size(); ++i) {
        Hook& h = m_hooks[i];
        h.deps.clear();
        for (const std::string& a : h.desc.after) {
            auto it = byName.find(a);
            if (it == byName.end()) return makeError(ErrorCode::NotFound, "hook '{}' waits for unknown hook '{}'", h.desc.name, a);
            const Hook& d = m_hooks[it->second];
            if (d.desc.stage != h.desc.stage)
                return makeError(ErrorCode::InvalidArgument, "hook '{}' waits for '{}' of another stage (stages are ordered already)",
                                 h.desc.name, a);
            if (h.desc.thread == HookThread::Tick && d.desc.thread == HookThread::Any)
                return makeError(ErrorCode::InvalidArgument, "tick-thread hook '{}' cannot wait for parallel hook '{}'",
                                 h.desc.name, a);
            h.deps.push_back(it->second);
        }
        deps[i] = h.deps;
    }
    for (usize s = 0; s < kStageCount; ++s) {
        std::vector<u32> tickNodes;
        std::vector<u32> anyNodes;
        for (u32 i = 0; i < m_hooks.size(); ++i) {
            if (static_cast<usize>(m_hooks[i].desc.stage) != s) continue;
            (m_hooks[i].desc.thread == HookThread::Tick ? tickNodes : anyNodes).push_back(i);
        }
        StagePlan& plan = m_plans[s];
        HELIOS_TRY_ASSIGN(plan.tickOrder, topoSort(tickNodes, deps));
        HELIOS_TRY_ASSIGN(plan.anyOrder, topoSort(anyNodes, deps));
        plan.graph.reset();
        if (plan.anyOrder.size() >= 2) {
            plan.graph = std::make_unique<jobs::TaskGraph>();
            std::map<u32, jobs::TaskGraph::NodeId> node;
            for (u32 idx : plan.anyOrder)
                node[idx] = plan.graph->addNode(m_hooks[idx].desc.name, [this, idx] { runHook(m_hooks[idx], *m_current); });
            for (u32 idx : plan.anyOrder)
                for (u32 d : m_hooks[idx].deps)
                    if (node.contains(d)) HELIOS_TRY(plan.graph->addEdge(node[d], node[idx]));
            HELIOS_TRY(plan.graph->validate());
        }
    }
    m_built = true;
    return {};
}

void TickGraph::runHook(Hook& h, TickContext& ctx) {
    const u64 start = monotonicNanos();
    h.desc.fn(ctx);
    const i64 ns = static_cast<i64>(monotonicNanos() - start);
    h.lastNs = ns;
    h.maxNs = std::max(h.maxNs, ns);
    ++h.runs;
}

Result<void> TickGraph::run(TickContext& ctx, jobs::JobSystem* jobs) {
    if (!m_built) HELIOS_TRY(build());
    const u64 tickStart = monotonicNanos();
    for (usize s = 0; s < kStageCount; ++s) {
        StagePlan& plan = m_plans[s];
        const u64 start = monotonicNanos();
        for (u32 idx : plan.tickOrder) runHook(m_hooks[idx], ctx);
        if (!plan.anyOrder.empty()) {
            if (jobs && plan.graph) {
                m_current = &ctx;
                Result<void> r = plan.graph->run(*jobs);
                m_current = nullptr;
                HELIOS_TRY(r);
            } else {
                for (u32 idx : plan.anyOrder) runHook(m_hooks[idx], ctx);
            }
        }
        const i64 ns = static_cast<i64>(monotonicNanos() - start);
        StageStats& st = m_stageStats[s];
        st.lastNs = ns;
        st.maxNs = std::max(st.maxNs, ns);
        st.totalNs += ns;
        ++st.runs;
        if (ns > m_budgets.ns[s]) ++st.overruns;
    }
    m_lastTickNs = static_cast<i64>(monotonicNanos() - tickStart);
    return {};
}

std::vector<HookStats> TickGraph::hookStats() const {
    std::vector<HookStats> out;
    for (const Hook& h : m_hooks) out.push_back(HookStats{h.desc.name, h.desc.stage, h.lastNs, h.maxNs, h.runs});
    return out;
}

std::vector<std::string> TickGraph::executionOrder(Stage stage) {
    std::vector<std::string> out;
    if (!m_built && !build()) return out;
    const StagePlan& plan = m_plans[static_cast<usize>(stage)];
    for (u32 idx : plan.tickOrder) out.push_back(m_hooks[idx].desc.name);
    for (u32 idx : plan.anyOrder) out.push_back(m_hooks[idx].desc.name);
    return out;
}

} // namespace helios::server
