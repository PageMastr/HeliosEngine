#pragma once
// Shared value types of the Luau scripting host (02 §7.4): task ids, error codes and the typed
// ScriptError, kill/yield reasons, fuel budgets and per-binding fuel costs, scheduler events and
// statistics.
//
// Threading: plain value types with no shared state.

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios::script {

/// Identifies a task (a coroutine run by the ScriptVm scheduler). Generational: the id of a task
/// that finished, failed, was killed or cancelled goes stale and never names a later task.
struct TaskId {
    u64 bits = 0;
    constexpr bool isValid() const noexcept { return bits != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    friend constexpr bool operator==(TaskId, TaskId) noexcept = default;
};

/// Token for an in-flight asynchronous host call made by a yielding binding (see binding.h).
struct AsyncToken {
    u64 bits = 0;
    constexpr bool isValid() const noexcept { return bits != 0; }
    friend constexpr bool operator==(AsyncToken, AsyncToken) noexcept = default;
};

/// Which host runs the VM; selects the sandbox flavour and the default budgets.
enum class HostProfile : u8 {
    Cell,   ///< Zone server: deterministic fuel budgets, no gcinfo, no weak tables.
    Client, ///< Game client: wall-time budgets (fuel is still counted for the profiler).
    Editor, ///< Editor / PIE tools: like Client.
};

/// Typed error codes carried by ScriptError (and by the Luau-side `ScriptError` table's `code`).
enum class ScriptErrorCode : u8 {
    None = 0,
    Runtime,        ///< Ordinary Luau error (error(), runtime type errors, ...).
    Compile,        ///< Source failed to compile.
    OutOfMemory,    ///< The VM or module heap cap was hit.
    StaleHandle,    ///< A handle to a destroyed (or recycled) host object was dereferenced.
    Killed,         ///< The resume exceeded its hard budget (fuel_kill / wall budget / backstop).
    Cancelled,      ///< The task was cancelled by the host or another task.
    ModuleDisabled, ///< The module was disabled after repeated kills (three-kills rule).
    NotFound,       ///< Unknown module or export.
    HostError,      ///< An asynchronous host call failed (ScriptVm::failAsync).
    InvalidState,   ///< API misuse (e.g. wait() outside a task).
};

/// Why a resume was killed.
enum class KillReason : u8 {
    None = 0,
    Fuel,         ///< Deterministic: fuel reached fuel_kill (cells).
    WallBudget,   ///< Client/editor: the resume exceeded its wall-time budget.
    WallBackstop, ///< Fault path: a binding far exceeded its calibrated charge (20 ms on cells).
};

/// Why a task's coroutine yielded. Every yield is an explicit call; `involuntaryYields` in VmStats
/// counts yields the scheduler could not attribute (must stay 0, RT-13).
enum class YieldReason : u8 {
    None = 0,
    Wait,       ///< wait(seconds) / task.wait
    TaskYield,  ///< task.yield()
    Checkpoint, ///< task.checkpoint() past the soft budget
    Async,      ///< yielding host binding (awaitService-style)
    Coroutine,  ///< coroutine.yield() at a task's top level (treated like task.yield)
    Count,
};

/// Scheduler-visible state of a live task.
enum class TaskState : u8 {
    Gone = 0, ///< Unknown or finished (the id is stale).
    Ready,    ///< Runnable; resumes on a later tick while lane fuel remains.
    Waiting,  ///< Sleeping in wait() until its zone-time deadline.
    Awaiting, ///< Suspended in an asynchronous host call.
    Running,  ///< Currently executing (only observable from inside bindings).
};

std::string_view scriptErrorCodeName(ScriptErrorCode code) noexcept;
std::string_view killReasonName(KillReason reason) noexcept;
std::string_view yieldReasonName(YieldReason reason) noexcept;
std::string_view taskStateName(TaskState state) noexcept;

/// One frame of a script stack trace (innermost first). `source` is the module name for Luau
/// frames ("[C]" for native frames); `line` is -1 when unknown.
struct StackFrame {
    std::string source;
    i32 line = -1;
    std::string function;
};

/// A script failure as seen by the host: typed code, message, owning module and stack trace mapped
/// to module names.
struct ScriptError {
    ScriptErrorCode code = ScriptErrorCode::None;
    KillReason killReason = KillReason::None;
    std::string message;
    std::string module;
    std::vector<StackFrame> stack;

    bool isError() const noexcept { return code != ScriptErrorCode::None; }
    /// "Runtime: probe:3: boom" followed by one "  at <source>:<line> in <function>" per frame.
    std::string toString() const;
};

/// Budgets of one VM (02 §7.4 table). Fuel is counted on every host; which limits decide depends
/// on the profile: cells use the fuel limits (deterministic) plus the wall backstop, clients and
/// editors use the wall limits. A limit of 0 is disabled.
///
/// 1 fuel = one Luau interrupt safepoint with gc < 0 (loop back-edges, calls, returns, pattern
/// matcher steps) plus the calibrated charges of bindings and wrapped builtins and a fixed
/// `resumeCost` per resume. The defaults are
/// placeholders until `helios-cell --calibrate-fuel` (WP-1.6) measures ns_per_fuel on the
/// reference SERVER core; at ~10 ns/fuel they approximate 2 ms / 5 ms / 7.5 ms.
struct FuelBudget {
    u64 fuelPerResume = 200'000;        ///< Soft: ScriptOverBudget + task.checkpoint() starts yielding.
    u64 fuelKill = 500'000;             ///< Hard: the resume is killed (sticky).
    u64 fuelPerTick = 750'000;          ///< Lane: stop resuming once spent (rest wait for the next tick).
    u64 wallSoftNanos = 0;              ///< Client soft budget per resume (2 ms).
    u64 wallKillNanos = 0;              ///< Client hard budget per resume (5 ms).
    u64 wallPerTickNanos = 0;           ///< Client lane budget per tick (1 ms).
    u64 wallBackstopNanos = 20'000'000; ///< Fault-path kill on every host (20 ms).
    /// Fixed charge of every resume and top-level run (lua_resume, continuation, scheduler queues),
    /// taken before any Luau code runs. Without it a lane of many trivial resumes (each yielding
    /// after 2-3 safepoints) would spend far more wall time than its fuel says. Placeholder until
    /// `--calibrate-fuel`: a trivial resume measured ~200 ns on the dev container (test_perf).
    u64 resumeCost = 16;

    /// Deterministic cell budgets (fuel decides; 20 ms wall backstop).
    static constexpr FuelBudget cell() noexcept { return FuelBudget{}; }
    /// Client/editor budgets: 2 ms soft, 5 ms kill, 1 ms lane per tick, all wall time.
    static constexpr FuelBudget client() noexcept {
        FuelBudget b;
        b.fuelPerResume = 0;
        b.fuelKill = 0;
        b.fuelPerTick = 0;
        b.wallSoftNanos = 2'000'000;
        b.wallKillNanos = 5'000'000;
        b.wallPerTickNanos = 1'000'000;
        b.wallBackstopNanos = 20'000'000;
        return b;
    }
};

/// Declared fuel cost of a Luau-callable C++ function (`@script(cost=n, each=m, of=arg)`):
/// charge = base + ceil(items * perItemMilli / 1000), taken before the call. `itemsArg` is the
/// 1-based argument whose size gives `items` (table/string/buffer length, or a number's value);
/// 0 = no per-item part.
struct FuelCost {
    u32 base = 1;
    u32 perItemMilli = 0;
    i32 itemsArg = 0;

    /// Saturates instead of wrapping, so an absurd item count always trips the kill.
    constexpr u64 charge(u64 items) const noexcept {
        if (perItemMilli != 0 && items > (~u64(0) - 999) / perItemMilli) return ~u64(0);
        return static_cast<u64>(base) + (items * perItemMilli + 999) / 1000;
    }
};

/// Scheduler / telemetry events (ScriptOverBudget, ScriptKilled, ... in 02 §7.4 and 04 §10.2).
enum class ScriptEventKind : u8 {
    TaskFinished,   ///< A task returned normally.
    TaskFailed,     ///< A task (or a top-level callback) raised an error.
    TaskKilled,     ///< A resume hit its hard budget (ScriptKilled telemetry).
    TaskCancelled,  ///< A task was cancelled.
    OverBudget,     ///< A resume crossed its soft budget (ScriptOverBudget telemetry).
    ModuleDisabled, ///< Three kills within the window disabled a module.
    ModuleReloaded, ///< A module was hot-reloaded.
};

std::string_view scriptEventKindName(ScriptEventKind kind) noexcept;

struct ScriptEvent {
    ScriptEventKind kind = ScriptEventKind::TaskFinished;
    u64 tick = 0;            ///< Scheduler tick index when the event happened.
    TaskId task;             ///< Invalid for top-level callbacks and module events.
    u64 owner = 0;           ///< Owning EntityId (0 = none).
    std::string_view module; ///< Valid during the callback only.
    u64 fuel = 0;            ///< Fuel used by the resume (kills / over-budget / failures).
    KillReason killReason = KillReason::None;
    const ScriptError* error = nullptr; ///< Failures and kills; valid during the callback only.
};

/// Receives events on the VM owner's thread, synchronously. Must not call back into the VM.
using ScriptEventSink = std::function<void(const ScriptEvent&)>;
/// Receives `print(...)` output (module name, text) in addition to the log.
using PrintSink = std::function<void(std::string_view module, std::string_view text)>;

/// Result of one ScriptVm::tick().
struct TickStats {
    u64 tick = 0;
    u32 resumed = 0;  ///< Resumes performed (tasks run).
    u32 deferred = 0; ///< Ready tasks left for the next tick because lane budget ran out.
    u32 finished = 0;
    u32 failed = 0;
    u32 killed = 0;
    u64 fuel = 0; ///< Lane fuel spent (<= fuelPerTick + fuelKill; RT-13 bound).
    u64 maxResumeFuel = 0;
    u64 wallNanos = 0;
};

/// Cumulative VM statistics.
struct VmStats {
    u64 ticks = 0;
    u64 resumes = 0;
    u64 fuelTotal = 0;
    u64 tasksSpawned = 0;
    u64 tasksFinished = 0;
    u64 tasksFailed = 0;
    u64 tasksKilled = 0;
    u64 tasksCancelled = 0;
    u64 overBudget = 0;
    std::array<u64, 4> killsByReason{};                                       ///< Indexed by KillReason.
    std::array<u64, static_cast<usize>(YieldReason::Count)> yieldsByReason{}; ///< Explicit yields.
    u64 involuntaryYields = 0; ///< Yields not caused by an explicit call; always 0 (RT-13).
    u64 bindingCalls = 0;
    u32 liveTasks = 0;
    /// Timer and ready-queue entries, including stale ones not yet compacted (bounded by roughly
    /// twice the live tasks plus a constant slack).
    usize queuedEntries = 0;
    usize heapBytes = 0; ///< Bytes currently handed to Luau by the VM allocator.
    usize heapPeakBytes = 0;
    usize heapLimitBytes = 0;
    u64 allocationFailures = 0; ///< Allocations refused by the VM or module caps.
};

} // namespace helios::script
