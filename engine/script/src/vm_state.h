#pragma once
// Private state of a ScriptVm, shared by the implementation files of engine/script.
//
// Error model: Helios builds Luau with LUA_USE_LONGJMP=0, so Luau errors are C++ exceptions
// (lua_exception, derived from std::exception). Host code never lets one escape: every host-side
// operation that touches the Luau heap runs inside VmState::protect() (lua_cpcall) or inside a
// lua_resume/lua_pcall, and binding frames unwind through RAII.

#include <algorithm>
#include <atomic>
#include <functional>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lua.h"
#include "lualib.h"

#include "helios/core/handle.h"
#include "helios/core/log.h"
#include "helios/core/random.h"
#include "helios/script/vm.h"

namespace helios::script {

HELIOS_LOG_CHANNEL(LogScript, "Script");

namespace detail {

// Userdata tags (LUA_UTAG_LIMIT = 128). Object types take the upper range.
inline constexpr int kTagWorldPos = 1;
inline constexpr int kTagTask = 2;
inline constexpr int kTagScriptError = 3;
inline constexpr int kFirstObjectTag = 32;
inline constexpr int kLastObjectTag = 127;

inline constexpr u64 kNoLimit = ~u64(0);
/// Fuel and deadlines saturate at kNoLimit instead of wrapping (a saturated charge must stay a kill).
constexpr u64 saturatingAdd(u64 a, u64 b) noexcept { return b > kNoLimit - a ? kNoLimit : a + b; }
/// Interpreter safepoints between wall-clock reads when a wall limit is active.
inline constexpr u64 kWallCheckInterval = 64;
/// Longest distance the VM's inline fuel counter is armed with (fuel-counter patch). Also the value
/// it holds while no run is active, so host-side setup and GC never reach the interrupt at gc < 0.
inline constexpr u64 kMaxCounterDistance = static_cast<u64>(std::numeric_limits<i64>::max());
/// Cells cap the subject of pattern functions (02 §7.4).
inline constexpr usize kCellPatternSubjectLimit = 64u << 10;
/// print() keeps at most this many bytes per call (the rest is summarized as truncated).
inline constexpr usize kMaxPrintBytes = 4u << 10;
/// ScriptError::message keeps at most this many bytes of a script's error string.
inline constexpr usize kMaxErrorMessageBytes = 4u << 10;
/// Memory category shared by modules beyond the 254 dedicated ones.
inline constexpr u8 kOverflowMemcat = 255;

struct BindingInfo {
    std::string qualifiedName;
    lua_CFunction fn = nullptr;
    FuelCost cost;
    void* userdata = nullptr;
    bool async = false;
    int objectType = 0; ///< Property getters: tag whose handle is resolved first.
    u64 calls = 0;
};

struct ObjectTypeInfo {
    std::string name;
    ResolveFn resolve = nullptr;
    void* context = nullptr;
    int tag = 0;
    int methodsRef = LUA_NOREF;    ///< name -> closure
    int propertiesRef = LUA_NOREF; ///< name -> lightuserdata(BindingInfo*)
};

struct Module {
    std::string name;
    std::string chunkName; ///< "@name": Luau reports "name:line" in errors and tracebacks.
    BytecodePtr bytecode;
    ModuleOptions options;
    CompileOptions compileOptions;
    u32 version = 0;
    u8 memcat = 0;
    int exportsRef = LUA_NOREF;
    bool instantiated = false;
    bool instantiating = false;
    bool disabled = false;
    std::vector<u64> killTimes; ///< Zone nanos of recent kills (three-kills rule).
};

struct TaskTag {};
using TaskHandle = Handle<TaskTag>;
struct AsyncTag {};
using AsyncHandle = Handle<AsyncTag>;

/// What a suspended task receives when it resumes (delivered by taskContinuation inside the
/// protected resume, so pushing results can never throw into host code).
enum class ResumeKind : u8 { None, Wait, Checkpoint, TaskYield, Async };

struct Task {
    lua_State* thread = nullptr;
    int threadRef = LUA_NOREF;
    u32 module = 0;
    u64 owner = 0;
    TaskState state = TaskState::Ready;
    u64 seq = 0; ///< Sequence of the current queue entry; stale entries are skipped.
    u64 wakeTick = 0;
    u64 wakeNanos = 0;
    u64 waitStartNanos = 0;
    int pendingArgs = 0; ///< Arguments on the thread stack for the first resume.
    bool started = false;
    ResumeKind resumeKind = ResumeKind::None;
    AsyncHandle awaiting;
};

struct AsyncOp {
    TaskHandle task;
    bool completed = false;
    bool failed = false;
    PushArgs results;
    std::string error;
};

struct VmState;

/// Budget accounting of one resume (or one top-level callback / module load).
///
/// Fuel metering uses the vendored `fuel-counter` Luau patch (third_party/MANIFEST.md): while a run
/// is the VM's active run (`armed`), the VM decrements its inline counter at every gc < 0 safepoint
/// and binding charges subtract from it, so `fuel` is exact only as of the last arm; read it through
/// VmState::fuelOf(). The counter is armed with the distance from `fuel` to `nextCheck`, the next
/// decision point, and the interrupt runs only when it reaches zero, so the fuel counted is the same
/// as calling the host at every safepoint.
struct RunContext {
    /// Fuel value at which the slow path must run (min of kill, soft, wall-check points; 0 once
    /// killed).
    u64 nextCheck = kNoLimit;
    TaskHandle task;
    u32 module = 0;
    u64 owner = 0;
    lua_State* thread = nullptr; ///< The task's own coroutine (explicit yields must happen here).
    u64 fuel = 0;
    u64 armedFuel = 0;     ///< `fuel` when the counter was last armed.
    u64 armedDistance = 0; ///< Counter value then: fuel to `nextCheck` (0 = due at the next safepoint).
    bool armed = false;    ///< This run owns the VM counter (it is the VM's active run).
    u64 softFuel = kNoLimit;
    u64 killFuel = kNoLimit;
    u64 wallStart = 0;
    u64 wallSoftAt = kNoLimit;
    u64 wallKillAt = kNoLimit;
    u64 wallBackstopAt = kNoLimit;
    u64 nextWallCheck = kNoLimit;
    bool overBudget = false;
    bool killed = false;
    KillReason killReason = KillReason::None;
    YieldReason yield = YieldReason::None;
    u64 allocFailuresAtStart = 0;
};

struct QueueEntry {
    u64 key = 0; ///< wake tick (ready queue) or wake nanos (timer queue)
    u64 owner = 0;
    u64 seq = 0;
    u64 task = 0;
    friend bool operator>(const QueueEntry& a, const QueueEntry& b) noexcept {
        if (a.key != b.key) return a.key > b.key;
        if (a.owner != b.owner) return a.owner > b.owner;
        return a.seq > b.seq;
    }
};

/// Min-heap on (key, owner, seq) — deterministic because seq is unique.
class EntryQueue {
public:
    bool empty() const noexcept { return m_heap.empty(); }
    usize size() const noexcept { return m_heap.size(); }
    const QueueEntry& top() const noexcept { return m_heap.front(); }
    void push(const QueueEntry& e);
    QueueEntry pop();
    const std::vector<QueueEntry>& entries() const noexcept { return m_heap; }
    void clear() noexcept { m_heap.clear(); }
    /// Drops the entries for which `stale(entry)` is true and restores the heap order.
    template <class Pred>
    void compact(Pred stale);

private:
    std::vector<QueueEntry> m_heap;
};

template <class Pred>
void EntryQueue::compact(Pred stale) {
    std::erase_if(m_heap, stale);
    std::make_heap(m_heap.begin(), m_heap.end(), std::greater<>{});
}

struct VmState {
    ScriptVm* scriptVm = nullptr; ///< The public object owning this state.
    VmConfig config;
    lua_State* mainL = nullptr; ///< Main thread: host-side protected calls and callbacks.
    MemoryTag tag = MemoryTag::Unknown;
    bool deterministic = true; ///< Cell profile.

    // Allocator.
    usize heapBytes = 0;
    usize heapPeak = 0;
    u64 allocFailures = 0;
    u8 activeMemcat = 0;

    // Registry references (pre-built kill errors).
    int killErrorRefs[4] = {LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

    // Bindings.
    std::deque<BindingInfo> bindings;       ///< Stable addresses (lightuserdata upvalues).
    std::deque<ObjectTypeInfo> objectTypes; ///< Stable addresses (lightuserdata upvalues).
    std::vector<std::string> manifest;
    bool registeringCore = false;
    const BindingInfo* currentBinding = nullptr;
    lua_State* bindingThread = nullptr;

    // Modules.
    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::string, u32> moduleIndex;
    u32 nextMemcat = 1;

    /// The VM's inline fuel counter (lua_fuelcounter, fuel-counter patch). Armed by the active run.
    i64* fuelCounter = nullptr;

    // Scheduler.
    HandlePool<Task, TaskTag> tasks;
    HandlePool<AsyncOp, AsyncTag> asyncOps;
    EntryQueue ready;
    EntryQueue timers;
    u64 seqCounter = 0;
    u64 tickIndex = 0;
    u64 now = 0;
    RunContext* run = nullptr;

    // Diagnostics.
    VmStats stats;
    ScriptError lastError;
    std::vector<StackFrame> capturedFrames;
    Random rng;
    std::shared_ptr<BytecodeCache> cache;
    bool codegen = false;
    char taskThreadMarker = 0; ///< lua_setthreaddata value identifying task coroutines.

    // Ownership check (one job at a time).
    std::atomic<u32> ownerDepth{0};
    std::thread::id ownerThread;

    ~VmState();

    // ---- setup (vm.cpp / stdlib.cpp) -------------------------------------------------------------
    void installStdlib();          // Helios built-ins + sandbox removals
    void installBuiltinWrappers(); // charging wrappers (+ setmetatable on cells)

    // ---- budgets (vm.cpp) ------------------------------------------------------------------------
    void beginRun(RunContext& run, TaskHandle task, u32 module, u64 owner, lua_State* thread);
    /// Fuel spent by `r` so far: its committed fuel plus what the VM counted since the counter was
    /// armed. The counter only moves down between arms and stops at 0 (the VM resets it there before
    /// calling the interrupt), so it stays within [0, armedDistance].
    u64 fuelOf(const RunContext& r) const noexcept {
        if (!r.armed) return r.fuel;
        const u64 left = static_cast<u64>(std::max<i64>(*fuelCounter, 0));
        return r.armedFuel + (r.armedDistance - std::min(left, r.armedDistance));
    }
    /// Arms the VM counter for `r` (the active run) with the distance from its fuel to nextCheck.
    void armCounter(RunContext& r) noexcept {
        const u64 distance =
            r.nextCheck > r.fuel ? std::min(r.nextCheck - r.fuel, kMaxCounterDistance) : 0;
        r.armedFuel = r.fuel;
        r.armedDistance = distance;
        r.armed = true;
        *fuelCounter = static_cast<i64>(distance);
    }
    /// Commits the counted fuel into r.fuel and releases the counter (no run is active afterwards).
    void disarmCounter(RunContext& r) noexcept {
        r.fuel = fuelOf(r);
        r.armed = false;
        *fuelCounter = static_cast<i64>(kMaxCounterDistance);
    }
    /// Charges `fuel` (binding/builtin charges); raises the sticky kill at fuel_kill. The hot path
    /// only moves the VM counter: a charge that stops short of the next decision point needs no check.
    void chargeRun(lua_State* L, RunContext& r, u64 fuel) {
        const i64 left = *fuelCounter;
        if (r.armed && left > 0 && fuel < static_cast<u64>(left)) {
            *fuelCounter = left - static_cast<i64>(fuel);
            return;
        }
        const u64 now = fuelOf(r);
        r.fuel = saturatingAdd(now, fuel);
        chargeSlow(L, r, r.fuel - now); // what was actually added (less once saturated)
    }
    /// Slow path of a charge or safepoint: r.fuel is current (`fuel` is what was just added). Handles
    /// the sticky kill, fuel_kill, the soft budget and the wall-clock read, then re-arms the counter.
    void chargeSlow(lua_State* L, RunContext& run, u64 fuel);
    static void updateNextCheck(RunContext& run) noexcept;
    void wallCheck(lua_State* L, RunContext& run);
    [[noreturn]] void kill(lua_State* L, RunContext& run, KillReason reason);
    [[noreturn]] void raiseKill(lua_State* L, const RunContext& run);
    void markOverBudget(RunContext& run);
    bool hasWallLimits() const noexcept;
    int invokeBinding(lua_State* L, BindingInfo& info);

    // ---- errors (vm.cpp) --------------------------------------------------------------------------
    ScriptError extractError(lua_State* L, int status, const RunContext* run);
    void captureStack(lua_State* thread, int firstLevel, std::vector<StackFrame>& out) const;
    void setLastError(const ScriptError& error);
    void emit(const ScriptEvent& event) const;
    [[noreturn]] void raiseTyped(lua_State* L, ScriptErrorCode code, std::string_view message);

    // ---- protected host calls --------------------------------------------------------------------
    /// Runs `body(L)` inside lua_cpcall on `thread` (main state by default). Returns the Luau
    /// status; on failure the error object is extracted into `error` (if non-null).
    template <class F>
    int protect(F& body, ScriptError* error = nullptr, lua_State* thread = nullptr);
    int protectRaw(lua_State* thread, void (*fn)(lua_State*, void*), void* data, ScriptError* error);

    // ---- modules (vm.cpp) -------------------------------------------------------------------------
    Module* findModule(std::string_view name) noexcept;
    void loadChunk(lua_State* L, Module& module, const BytecodePtr& bytecode);
    void instantiateInline(lua_State* L, Module& module);
    Result<void> instantiateTopLevel(Module& module);
    /// Accounts a top-level run (callback, module load or reload): stats, kill/failure events,
    /// the three-kills rule. Returns the error as a Result.
    Result<void> finishTopLevel(RunContext& run, u32 module, bool failed, ScriptError& error);
    void recordKill(Module& module);
    /// Full GC after an allocation failure (Luau has no emergency collection).
    void collectAfterOom();
    void disableModule(Module& module, std::string_view reason);

    // ---- scheduler (scheduler.cpp) ----------------------------------------------------------------
    Task* taskOf(TaskHandle h) noexcept { return tasks.get(h); }
    Task* runningTask() noexcept { return run ? tasks.get(run->task) : nullptr; }
    TaskHandle createTask(lua_State* thread, int threadRef, u32 module, u64 owner, int nargs);
    void makeReady(TaskHandle h, Task& task, u64 wakeTick);
    void addTimer(TaskHandle h, Task& task, u64 wakeNanos);
    void resumeTask(TaskHandle h, TickStats& stats);
    void finishTask(TaskHandle h, ScriptEventKind kind, const ScriptError* error, u64 fuel);
    /// Drops queue entries of finished or rescheduled tasks once they outnumber the live tasks
    /// (a cancelled wait(1e9) would otherwise keep its timer entry for 1e9 seconds).
    void compactQueues();
    int yieldTask(lua_State* L, YieldReason reason, ResumeKind kind);
    bool requireYieldableTask(lua_State* L, const char* what, bool raise);
    TickStats runTick(u64 zoneNanos);
    ScriptError errorFromThread(lua_State* thread, int status, const RunContext& run);
    bool cancelTaskInternal(TaskHandle h);
};

/// VmState owning a Luau state (set as lua_callbacks()->userdata).
inline VmState* stateOf(lua_State* L) noexcept { return static_cast<VmState*>(lua_callbacks(L)->userdata); }

/// RAII guard asserting single-owner use of a VM.
class OwnerGuard {
public:
    explicit OwnerGuard(const VmState& s) noexcept;
    ~OwnerGuard();
    OwnerGuard(const OwnerGuard&) = delete;
    OwnerGuard& operator=(const OwnerGuard&) = delete;

private:
    VmState& m_state;
};

/// Sets the active memory category of `L` (and the allocator's view of it) for a scope.
class MemcatScope {
public:
    MemcatScope(VmState& s, lua_State* L, u8 memcat) noexcept;
    ~MemcatScope();
    MemcatScope(const MemcatScope&) = delete;
    MemcatScope& operator=(const MemcatScope&) = delete;

private:
    VmState& m_state;
    lua_State* m_L;
    u8 m_prevThread;
    u8 m_prevActive;
};

/// Installs a top-level RunContext (engine→Luau callback or host-side module load).
class RunScope {
public:
    RunScope(VmState& s, u32 module) noexcept;
    ~RunScope();
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;
    RunContext& context() noexcept { return m_run; }

private:
    VmState& m_state;
    RunContext m_run;
    RunContext* m_prev;
    u8 m_prevMemcat;
};

/// Pushes a ScriptError userdata (code, kill reason, message stored inline; immutable, unforgeable).
void pushScriptError(lua_State* L, ScriptErrorCode code, KillReason reason, std::string_view message);
/// Reads a ScriptError userdata at `idx` (false if the value is not one).
bool readScriptError(lua_State* L, int idx, ScriptError& out) noexcept;

/// A `Killed` error object observed outside a killed run was stored and rethrown by a script:
/// report it as an ordinary runtime error (only the scheduler decides what is a kill).
inline void normalizeKill(ScriptError& e, const RunContext& run) noexcept {
    if (!run.killed && e.code == ScriptErrorCode::Killed) {
        e.code = ScriptErrorCode::Runtime;
        e.killReason = KillReason::None;
    }
}

/// lua_checkstack that reports an out-of-memory failure as false instead of raising.
bool ensureStack(lua_State* L, int n) noexcept;
/// Reads the error object on top of `L` (left in place) into `error`.
void readErrorObject(lua_State* L, int status, ScriptError& error);

// Shared C functions (binding.cpp / stdlib.cpp / scheduler.cpp).
int bindingTrampoline(lua_State* L);
int taskContinuation(lua_State* L, int status);
int tracebackHandler(lua_State* L);
void registerFunction(VmState& s, std::string_view library, std::string_view name, lua_CFunction fn,
                      FuelCost cost, void* userdata, bool async);
/// registerFunction for the built-in Helios API (recorded in the manifest checked against helios.d.luau).
void registerCoreFunction(VmState& s, std::string_view library, std::string_view name, lua_CFunction fn,
                          FuelCost cost, bool async);
/// Names ("lib.name") of every builtin the sandbox wraps or replaces (must all be disabledBuiltins).
std::vector<std::string> sandboxWrappedBuiltins();
BindingInfo& addBinding(VmState& s, std::string_view qualifiedName, lua_CFunction fn, FuelCost cost,
                        void* userdata, bool async);
void pushBindingClosure(lua_State* L, BindingInfo& info);
u64 itemsOfArg(lua_State* L, int idx) noexcept;
void pushTaskHandle(lua_State* L, TaskHandle h);
Result<void> scriptErrorToResult(const ScriptError& error);

template <class F>
int VmState::protect(F& body, ScriptError* error, lua_State* thread) {
    return protectRaw(
        thread ? thread : mainL, [](lua_State* state, void* data) { (*static_cast<F*>(data))(state); }, &body,
        error);
}

} // namespace detail
} // namespace helios::script
