#pragma once
// ScriptVm: one sandboxed Luau VM (lua_State) per zone instance, client or editor (02 §7.4,
// 04 §3.1), with fuel-metered budgets, a coroutine task scheduler on the zone clock, modules with
// hot reload, and per-VM / per-module heap caps.
//
//   auto vm = ScriptVm::create(config, [&](Binder& b) { b.function("Ship", "boost", &shipBoost); });
//   vm.value()->loadModule("door", source);
//   vm.value()->spawnScript("door", entityId);
//   ... every tick: clock.consumeStep(); vm.value()->tick();
//
// Setup order (normative): luaL_openlibs → Helios API (built-ins, then the registrar) →
// luaL_sandbox. Every module gets its own environment (luaL_sandboxthread) proxying reads to the
// frozen globals. `io`, `os`, `debug`, `loadstring`, `getfenv`, `setfenv` and `math.randomseed` are
// absent; `math.random` draws from a seeded deterministic stream. On cells `gcinfo` (and
// `collectgarbage`, which Luau 0.739 lacks) are removed and `setmetatable` rejects `__mode`, so GC
// timing stays unobservable.
//
// Budgets (FuelBudget): fuel is counted by the interrupt callback at every gc < 0 safepoint and by
// binding charges. The interrupt NEVER yields: past the soft budget the resume is flagged and an
// explicit task.checkpoint() yields; at the hard budget the resume is killed with a sticky error
// that pcall cannot swallow (every later safepoint and binding call raises again). The scheduler
// then closes the coroutine (lua_resetthread). Three kills of a module within 60 s of zone time
// disable it.
//
// Threading: a ScriptVm is owned by one job at a time. It is not thread-safe; every member must
// be called by the current owner, never concurrently (checked by an assert in development
// builds). Ownership may move between threads between calls. Async completions from other
// threads must be marshalled to the owner (the zone's SimInbox) before calling completeAsync.
// Only BytecodeCache may be shared between VMs on different threads.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/hash.h"
#include "helios/core/memory.h"
#include "helios/core/result.h"
#include "helios/core/time.h"
#include "helios/script/binding.h"
#include "helios/script/compiler.h"
#include "helios/script/types.h"

namespace helios::script {

/// Pushes values onto a Luau stack and returns how many were pushed (runs inside a protected
/// call, so it may raise).
using PushArgs = std::function<int(lua_State*)>;
/// Reads `count` results starting at stack index `base`.
using ReadResults = std::function<void(lua_State*, int base, int count)>;

struct VmConfig {
    std::string name = "script";
    HostProfile profile = HostProfile::Cell;
    /// Hard cap on bytes handed to Luau (16 MB small / 256 MB large zones, 04 §3.1). Allocations
    /// beyond it fail and raise a Luau memory error.
    usize heapLimitBytes = 16u << 20;
    /// Cap per module memory category (0 = VM cap only). Enforced at allocation-page granularity.
    usize moduleHeapLimitBytes = 0;
    FuelBudget budget = FuelBudget::cell();
    /// Kills of one module within `killWindowNanos` of zone time that disable it (0 = never).
    u32 killsToDisableModule = 3;
    u64 killWindowNanos = 60'000'000'000ull;
    /// Zone clock driving wait() (zone time, so time dilation applies). May be null when the host
    /// drives the scheduler with tickAt().
    const DilatableClock* clock = nullptr;
    /// Seed of the deterministic stream behind math.random.
    u64 randomSeed = 0x48454c494f53ull;
    /// Opt-in native code generation (off by default; keep it off on servers until profiled).
    /// Ignored when luau_codegen_supported() is false.
    bool enableNativeCodegen = false;
    CompileOptions compileOptions;
    /// Shared per content version; a private cache is created when null.
    std::shared_ptr<BytecodeCache> bytecodeCache;
    ScriptEventSink onEvent;
    PrintSink onPrint;
    /// Memory tag the VM heap is attributed to (Unknown = the shared "Script" tag).
    MemoryTag memoryTag = MemoryTag::Unknown;
};

struct ModuleOptions {
    /// Compile the module to native code (requires VmConfig::enableNativeCodegen).
    bool native = false;
    /// Overrides VmConfig::compileOptions for this module.
    std::optional<CompileOptions> compile;
};

struct ModuleInfo {
    std::string name;
    u32 version = 0;           ///< Starts at 1, bumped by every successful reload.
    u8 memoryCategory = 0;     ///< lua_setmemcat category (255 is shared by overflow modules).
    bool instantiated = false; ///< Exports exist (required at least once).
    bool disabled = false;
    bool native = false;
    u32 recentKills = 0;
    usize heapBytes = 0; ///< lua_totalbytes of the module's category.
    Hash128 sourceHash;
};

class ScriptVm {
public:
    using ApiRegistrar = std::function<void(Binder&)>;

    /// Creates and sandboxes a VM; `registerApi` adds host bindings before the sandbox freezes the
    /// globals. Fails when the heap cap is too small for the libraries or Luau cannot start.
    static Result<std::unique_ptr<ScriptVm>> create(const VmConfig& config,
                                                    const ApiRegistrar& registerApi = {});
    ~ScriptVm();
    ScriptVm(const ScriptVm&) = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;

    // ---- modules ------------------------------------------------------------------------------
    /// Compiles (through the bytecode cache) and registers a module. Nothing runs until the module
    /// is required, instantiated or spawned as a script. Fails on compile errors or duplicates.
    Result<void> loadModule(std::string_view name, std::string_view source,
                            const ModuleOptions& options = {});

    /// Hot reload with re-require semantics:
    ///  1. the new source is compiled; on failure the old version stays active;
    ///  2. if the module was instantiated, its chunk runs again in a fresh environment producing
    ///     new exports; if they contain `__reload(old)`, it is called with the previous exports so
    ///     state can migrate (a failure there keeps the old version);
    ///  3. the registry points at the new exports: later require() calls, spawnExport/callExport
    ///     and spawnScript use the new code. Tables already captured by other modules and
    ///     coroutines already running keep the old code until they finish or re-require;
    ///  4. the module is re-enabled and its kill history cleared.
    Result<void> reloadModule(std::string_view name, std::string_view source);

    /// Host-side require(): runs the module chunk once (non-yielding, fuel-metered like a
    /// callback) and caches its exports.
    Result<void> instantiateModule(std::string_view name);
    bool hasModule(std::string_view name) const;
    std::optional<ModuleInfo> moduleInfo(std::string_view name) const;
    /// Re-enables a module disabled by the three-kills rule.
    Result<void> enableModule(std::string_view name);

    // ---- tasks --------------------------------------------------------------------------------
    /// Starts a new instance of the module's chunk as a task (own environment; its top level may
    /// wait). It first runs on the next tick(). `owner` is the EntityId used for ordering.
    Result<TaskId> spawnScript(std::string_view module, u64 owner = 0);
    /// Runs `module.function(args...)` as a task (instantiating the module if needed).
    Result<TaskId> spawnExport(std::string_view module, std::string_view function, u64 owner = 0,
                               const PushArgs& args = {});
    /// Engine→Luau callback: calls `module.function(args...)` synchronously, protected and
    /// fuel-metered as one resume. The callback cannot yield (use spawnExport). When called from
    /// inside a binding it runs nested in the current resume and shares its budget.
    Result<void> callExport(std::string_view module, std::string_view function, const PushArgs& args = {},
                            const ReadResults& results = {}, int nresults = 0);
    /// Cancels a task (closing its coroutine). False when the id is stale or the task is running.
    bool cancelTask(TaskId task);
    TaskState taskState(TaskId task) const;
    u32 liveTaskCount() const;

    /// Runs one scheduler tick at the zone clock's current game time: expired waits wake up, then
    /// ready tasks resume in (wake tick, owner EntityId, sequence) order while lane fuel remains.
    TickStats tick();
    /// Same as tick() at an explicit zone time (monotonic, nanoseconds).
    TickStats tickAt(u64 zoneNanos);

    /// Completes an asynchronous host call; `results` pushes the call's return values when the task
    /// resumes (on the next tick). False when the token is stale (task gone or cancelled).
    bool completeAsync(AsyncToken token, PushArgs results = {});
    /// Fails an asynchronous host call: the task resumes with a `HostError` raised at the call.
    bool failAsync(AsyncToken token, std::string message);

    // ---- diagnostics ----------------------------------------------------------------------------
    VmStats stats() const;
    /// The most recent failure (task error, kill, callback or module load).
    const ScriptError& lastError() const noexcept;
    u64 tickIndex() const noexcept;
    u64 zoneTimeNanos() const noexcept;
    /// Qualified names of the built-in Helios Luau API (checked against helios.d.luau).
    std::vector<std::string> apiManifest() const;
    /// Full GC cycle (host only; scripts cannot trigger GC on cells).
    void collectGarbage();
    bool nativeCodegenActive() const noexcept;
    const VmConfig& config() const noexcept;
    /// The main Luau state (for advanced binding code; never run scripts on it directly).
    lua_State* state() const noexcept;

private:
    friend struct detail::VmState;
    explicit ScriptVm(std::unique_ptr<detail::VmState> state) noexcept;
    std::unique_ptr<detail::VmState> m_state;
};

} // namespace helios::script
