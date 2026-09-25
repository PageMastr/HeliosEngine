#pragma once
// TickGraph: a zone's per-tick job graph (04 §3.3, 02 §2.4 "per-zone stage graph").
//
// A tick runs the eight 04 §3.3 stages in order (ecs::Stage): Input (SimInbox drain) -> PrePhysics
// (sim) -> Physics -> PostPhysics (gameplay, Luau) -> AuthorityFlush -> ReplicationGather ->
// ConnectionWrite -> Send. Work is registered as named *hooks* on a stage:
//
//   * HookThread::Tick hooks run on the tick thread, one after another (the ECS World's stages,
//     inbox drains and anything else that must stay on the owning thread);
//   * HookThread::Any hooks may run on job workers, in parallel with the stage's other Any hooks,
//     after all of its Tick hooks (a jobs::TaskGraph per stage; e.g. one hook per AG or grid).
//
// `after` orders hooks inside a stage (Any hooks may wait for Tick hooks, which run first anyway;
// a Tick hook may not wait for an Any hook). build() validates names, stages and cycles once.
// Every stage finishes before the next starts, so stage order is the only cross-stage ordering.
//
// **Budgets.** Each stage has a wall-time budget from 04 §3.3 at 20 Hz (2 + 6 + 10 + 8 + 1 + 3 + 3
// + 2 = 35 ms, the p99 target at 70 % of the tick), scaled by 20 / tick_hz (11.7 ms at 60 Hz). run()
// measures every stage and hook and counts overruns; the tick's total feeds the TiDi controller.
//
// Threading: build and run on the tick thread; hooks follow the rules above. Not reentrant.

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/system.h"

namespace helios::jobs {
class JobSystem;
class TaskGraph;
} // namespace helios::jobs

namespace helios::server {

using Stage = ecs::Stage;
inline constexpr u32 kStageCount = ecs::kStageCount;

/// Per-stage wall budgets in nanoseconds.
struct StageBudgets {
    std::array<i64, kStageCount> ns{};

    /// 04 §3.3's table at 20 Hz, scaled by 20 / tickHz.
    static StageBudgets forTickHz(u32 tickHz);
    i64 totalNs() const noexcept;
};

/// What a hook sees of the running tick.
struct TickContext {
    u64 tick = 0;
    f32 dt = 0.0f;           ///< Fixed game step in seconds (never dilated).
    u32 dilationPpm = 0;
    i64 wallNowNs = 0;
    void* user = nullptr;    ///< The owner (e.g. the ZoneInstance).
};

using HookFn = std::function<void(TickContext&)>;

enum class HookThread : u8 { Tick, Any };

struct HookDesc {
    std::string name;
    Stage stage = Stage::PrePhysics;
    HookFn fn;
    HookThread thread = HookThread::Tick;
    std::vector<std::string> after; ///< Hooks of the same stage that must finish first.
};

struct StageStats {
    i64 lastNs = 0;
    i64 maxNs = 0;
    i64 totalNs = 0;
    u64 runs = 0;
    u64 overruns = 0; ///< Runs longer than the stage budget.
};

struct HookStats {
    std::string name;
    Stage stage = Stage::PrePhysics;
    i64 lastNs = 0;
    i64 maxNs = 0;
    u64 runs = 0;
};

class TickGraph {
public:
    TickGraph();
    ~TickGraph();
    TickGraph(const TickGraph&) = delete;
    TickGraph& operator=(const TickGraph&) = delete;

    /// Adds a hook (names are unique). Invalidates the built order.
    Result<void> addHook(HookDesc desc);
    /// Validates `after` references (same stage, known, Tick hooks not waiting on Any hooks) and
    /// cycles, and prepares the per-stage task graphs. run() builds on demand.
    Result<void> build();
    /// Runs every stage in order. `jobs` may be null (Any hooks then run on this thread, in order).
    Result<void> run(TickContext& ctx, jobs::JobSystem* jobs);

    void setBudgets(const StageBudgets& budgets) noexcept { m_budgets = budgets; }
    const StageBudgets& budgets() const noexcept { return m_budgets; }
    const StageStats& stageStats(Stage stage) const noexcept { return m_stageStats[static_cast<usize>(stage)]; }
    std::vector<HookStats> hookStats() const;
    /// Wall time of the last run() (all stages).
    i64 lastTickNs() const noexcept { return m_lastTickNs; }
    /// Tick-thread hooks of `stage` in execution order, then its Any hooks in a valid order.
    std::vector<std::string> executionOrder(Stage stage);
    usize hookCount() const noexcept { return m_hooks.size(); }

private:
    struct Hook {
        HookDesc desc;
        std::vector<u32> deps; // indices of hooks in `after`
        i64 lastNs = 0;
        i64 maxNs = 0;
        u64 runs = 0;
    };
    struct StagePlan {
        std::vector<u32> tickOrder; // Tick hooks, topologically sorted
        std::vector<u32> anyOrder;  // Any hooks, topologically sorted
        std::unique_ptr<jobs::TaskGraph> graph; // Any hooks, when there are two or more
    };
    void runHook(Hook& h, TickContext& ctx);

    std::vector<Hook> m_hooks;
    std::array<StagePlan, kStageCount> m_plans;
    std::array<StageStats, kStageCount> m_stageStats{};
    StageBudgets m_budgets;
    bool m_built = false;
    i64 m_lastTickNs = 0;
    TickContext* m_current = nullptr; // for task-graph nodes during run()
};

} // namespace helios::server
