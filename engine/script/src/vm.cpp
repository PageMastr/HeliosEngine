// ScriptVm core: creation and sandboxing, the tagged/capped allocator, fuel metering through the
// VM's inline fuel counter and the interrupt callback, kill semantics, error extraction with stack
// traces, and modules (require, instantiation, hot reload).

#include <algorithm>
#include <cstring>
#include <format>

#include "luacodegen.h"

#include "helios/core/assert.h"
#include "vm_state.h"

// Fuel metering needs the vendored Luau patches (third_party/luau/patches, listed in
// third_party/MANIFEST.md; rebased on every Luau bump). `lint_vendor_patches` checks all of them.
#if !defined(LUA_FUELCOUNTER)
#error "engine/script needs Luau with the vendored fuel-counter patch (third_party/luau/patches)"
#endif

namespace helios::script {

// ---------------------------------------------------------------------------------------------
// Names and ScriptError
// ---------------------------------------------------------------------------------------------
std::string_view scriptErrorCodeName(ScriptErrorCode code) noexcept {
    switch (code) {
    case ScriptErrorCode::None: return "None";
    case ScriptErrorCode::Runtime: return "Runtime";
    case ScriptErrorCode::Compile: return "Compile";
    case ScriptErrorCode::OutOfMemory: return "OutOfMemory";
    case ScriptErrorCode::StaleHandle: return "StaleHandle";
    case ScriptErrorCode::Killed: return "Killed";
    case ScriptErrorCode::Cancelled: return "Cancelled";
    case ScriptErrorCode::ModuleDisabled: return "ModuleDisabled";
    case ScriptErrorCode::NotFound: return "NotFound";
    case ScriptErrorCode::HostError: return "HostError";
    case ScriptErrorCode::InvalidState: return "InvalidState";
    }
    return "Unknown";
}

std::string_view killReasonName(KillReason reason) noexcept {
    switch (reason) {
    case KillReason::None: return "None";
    case KillReason::Fuel: return "Fuel";
    case KillReason::WallBudget: return "WallBudget";
    case KillReason::WallBackstop: return "WallBackstop";
    }
    return "Unknown";
}

std::string_view yieldReasonName(YieldReason reason) noexcept {
    switch (reason) {
    case YieldReason::None: return "None";
    case YieldReason::Wait: return "Wait";
    case YieldReason::TaskYield: return "TaskYield";
    case YieldReason::Checkpoint: return "Checkpoint";
    case YieldReason::Async: return "Async";
    case YieldReason::Coroutine: return "Coroutine";
    case YieldReason::Count: break;
    }
    return "Unknown";
}

std::string_view taskStateName(TaskState state) noexcept {
    switch (state) {
    case TaskState::Gone: return "dead";
    case TaskState::Ready: return "ready";
    case TaskState::Waiting: return "waiting";
    case TaskState::Awaiting: return "awaiting";
    case TaskState::Running: return "running";
    }
    return "unknown";
}

std::string_view scriptEventKindName(ScriptEventKind kind) noexcept {
    switch (kind) {
    case ScriptEventKind::TaskFinished: return "TaskFinished";
    case ScriptEventKind::TaskFailed: return "TaskFailed";
    case ScriptEventKind::TaskKilled: return "ScriptKilled";
    case ScriptEventKind::TaskCancelled: return "TaskCancelled";
    case ScriptEventKind::OverBudget: return "ScriptOverBudget";
    case ScriptEventKind::ModuleDisabled: return "ModuleDisabled";
    case ScriptEventKind::ModuleReloaded: return "ModuleReloaded";
    }
    return "Unknown";
}

std::string ScriptError::toString() const {
    std::string out(scriptErrorCodeName(code));
    if (killReason != KillReason::None) {
        out += " (";
        out += killReasonName(killReason);
        out += ')';
    }
    out += ": ";
    out += message;
    for (const StackFrame& f : stack) {
        out += "\n  at ";
        out += f.source;
        if (f.line >= 0) out += std::format(":{}", f.line);
        if (!f.function.empty()) {
            out += " in ";
            out += f.function;
        }
    }
    return out;
}

namespace detail {

namespace {

// ---------------------------------------------------------------------------------------------
// Allocator: Helios tagged heap with a hard per-VM cap and page-granular per-module caps.
// ---------------------------------------------------------------------------------------------
void* luauAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* s = static_cast<VmState*>(ud);
    if (nsize == 0) {
        if (ptr) {
            alignedFree(ptr);
            s->heapBytes -= osize;
        }
        return nullptr;
    }
    const usize oldSize = ptr ? osize : 0;
    if (nsize > oldSize) {
        const usize grow = nsize - oldSize;
        if (s->heapBytes + grow > s->config.heapLimitBytes) {
            ++s->allocFailures;
            return nullptr;
        }
        // Module caps use Luau's own per-category accounting of the running thread's category.
        if (s->config.moduleHeapLimitBytes != 0 && s->mainL != nullptr && s->activeMemcat != 0 &&
            lua_totalbytes(s->mainL, s->activeMemcat) + grow > s->config.moduleHeapLimitBytes) {
            ++s->allocFailures;
            return nullptr;
        }
    }
    void* block = alignedAlloc(nsize, kDefaultAlignment, s->tag);
    if (!block) {
        if (ptr && nsize <= oldSize) {
            // A shrink must never fail (Luau shrinks outside protected calls, e.g. in
            // lua_resetthread): keep the larger block.
            s->heapBytes -= oldSize - nsize;
            return ptr;
        }
        ++s->allocFailures;
        return nullptr;
    }
    if (ptr) {
        std::memcpy(block, ptr, std::min<usize>(oldSize, nsize));
        alignedFree(ptr);
    }
    s->heapBytes = s->heapBytes - oldSize + nsize;
    s->heapPeak = std::max(s->heapPeak, s->heapBytes);
    return block;
}

// Every VM safepoint with gc < 0 costs one fuel. The VM counts them itself (the vendored
// `fuel-counter` patch): it decrements an inline counter armed with the fuel left until the next
// decision point (kill, soft budget, wall-clock read) and calls this only when the counter reaches
// zero, so the per-safepoint cost is a decrement and a branch instead of a call (RT-13's <= 10 %).
// The callback never yields (02 §7.4): it either returns or raises the (sticky) kill.
void luauInterrupt(lua_State* L, int gc) {
    VmState* s = stateOf(L);
    RunContext* run = s->run;
    if (gc >= 0) {
        // GC-step invocations are not safepoints (04 §10.2): no fuel, and never raise here. But a
        // GC step means the script allocated, and one large allocation (a multi-MB concat or
        // string.upper) can take longer than the whole wall budget while costing a single
        // safepoint, so the periodic clock read (every kWallCheckInterval fuel) could land far past
        // the 20 ms backstop. Force a clock read at the next real safepoint instead: re-armed at
        // distance 0, the counter takes the slow path there.
        if (run && run->armed && run->nextWallCheck != kNoLimit && !run->killed) {
            run->fuel = s->fuelOf(*run);
            run->nextWallCheck = run->fuel;
            run->nextCheck = run->fuel;
            s->armCounter(*run);
        }
        return;
    }
    if (!run || !run->armed) { // host-side setup: unmetered
        *s->fuelCounter = static_cast<i64>(kMaxCounterDistance);
        return;
    }
    // The counter reached zero at this safepoint, the armedDistance-th since it was armed (the first
    // when it was armed at 0, i.e. due). Saturating: a killed run whose fuel saturated stays there.
    run->fuel = saturatingAdd(run->armedFuel, std::max<u64>(run->armedDistance, 1));
    s->chargeSlow(L, *run, run->fuel - run->armedFuel);
}

struct ProtectCall {
    void (*fn)(lua_State*, void*);
    void* data;
};

int protectTrampoline(lua_State* L) {
    auto* call = static_cast<ProtectCall*>(lua_tolightuserdata(L, 1));
    lua_remove(L, 1);
    call->fn(L, call->data);
    return 0;
}

} // namespace

bool ensureStack(lua_State* L, int n) noexcept {
    try {
        return lua_checkstack(L, n) != 0;
    } catch (...) {
        return false;
    }
}

void readErrorObject(lua_State* L, int status, ScriptError& e) {
    try {
        if (status == LUA_ERRMEM) {
            e.code = ScriptErrorCode::OutOfMemory;
            e.message = "not enough memory";
            return;
        }
        e.code = ScriptErrorCode::Runtime;
        const int type = lua_type(L, -1);
        if (type == LUA_TSTRING) {
            usize len = 0;
            const char* text = lua_tolstring(L, -1, &len);
            // Bounded: extraction runs on the host, unmetered, once per failure; a script must not
            // make every failure copy (and log) a heap-sized message.
            e.message.assign(text, std::min(len, kMaxErrorMessageBytes));
            if (len > kMaxErrorMessageBytes)
                e.message += std::format("... ({} bytes truncated)", len - kMaxErrorMessageBytes);
        } else if (readScriptError(L, -1, e)) {
            // typed error (StaleHandle, NotFound, Killed, ...): code and message read in place
        } else {
            e.message = std::format("(error object is a {} value)", lua_typename(L, type));
        }
        if (status == LUA_ERRERR) e.message = "error in error handling: " + e.message;
    } catch (...) {
        e.code = ScriptErrorCode::Runtime;
        e.message = "(unreadable error object)";
    }
}

// ---------------------------------------------------------------------------------------------
// EntryQueue
// ---------------------------------------------------------------------------------------------
void EntryQueue::push(const QueueEntry& e) {
    m_heap.push_back(e);
    std::push_heap(m_heap.begin(), m_heap.end(), std::greater<>{});
}

QueueEntry EntryQueue::pop() {
    std::pop_heap(m_heap.begin(), m_heap.end(), std::greater<>{});
    const QueueEntry e = m_heap.back();
    m_heap.pop_back();
    return e;
}

// ---------------------------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------------------------
OwnerGuard::OwnerGuard(const VmState& s) noexcept : m_state(const_cast<VmState&>(s)) {
    if (m_state.ownerDepth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        m_state.ownerThread = std::this_thread::get_id();
    } else {
        HELIOS_ASSERT(m_state.ownerThread == std::this_thread::get_id(),
                      "ScriptVm '{}' used by two threads at once (a VM is owned by one job at a time)",
                      m_state.config.name);
    }
}

OwnerGuard::~OwnerGuard() { m_state.ownerDepth.fetch_sub(1, std::memory_order_acq_rel); }

MemcatScope::MemcatScope(VmState& s, lua_State* L, u8 memcat) noexcept
    : m_state(s), m_L(L), m_prevThread(s.activeMemcat), m_prevActive(s.activeMemcat) {
    // Luau has no getter for a thread's category. Invariant: activeMemcat always equals the
    // category of the thread running Luau code (resumeTask and RunScope maintain it).
    lua_setmemcat(L, memcat);
    s.activeMemcat = memcat;
}

MemcatScope::~MemcatScope() {
    lua_setmemcat(m_L, m_prevThread);
    m_state.activeMemcat = m_prevActive;
}

RunScope::RunScope(VmState& s, u32 module) noexcept
    : m_state(s), m_prev(s.run), m_prevMemcat(s.activeMemcat) {
    if (m_prev) s.disarmCounter(*m_prev); // the VM counter belongs to the active run only
    s.beginRun(m_run, TaskHandle{}, module, 0, nullptr);
    s.run = &m_run;
    s.armCounter(m_run);
    // Top-level runs execute on the main thread: attribute their allocations to the module.
    const u8 memcat = module < s.modules.size() ? s.modules[module]->memcat : 0;
    lua_setmemcat(s.mainL, memcat);
    s.activeMemcat = memcat;
}

RunScope::~RunScope() {
    m_state.disarmCounter(m_run);
    m_state.run = m_prev;
    if (m_prev) m_state.armCounter(*m_prev);
    lua_setmemcat(m_state.mainL, m_prevMemcat);
    m_state.activeMemcat = m_prevMemcat;
}

// ---------------------------------------------------------------------------------------------
// Budgets and kills
// ---------------------------------------------------------------------------------------------
bool VmState::hasWallLimits() const noexcept {
    const FuelBudget& b = config.budget;
    return b.wallSoftNanos != 0 || b.wallKillNanos != 0 || b.wallBackstopNanos != 0;
}

void VmState::beginRun(RunContext& r, TaskHandle task, u32 module, u64 owner, lua_State* thread) {
    r = RunContext{};
    r.task = task;
    r.module = module;
    r.owner = owner;
    r.thread = thread;
    const FuelBudget& b = config.budget;
    r.allocFailuresAtStart = allocFailures;
    r.softFuel = b.fuelPerResume != 0 ? b.fuelPerResume : kNoLimit;
    r.killFuel = b.fuelKill != 0 ? b.fuelKill : kNoLimit;
    // The fixed resume charge counts like any other fuel (it can make the soft budget or even the
    // kill trip at the first safepoint when budgets are tiny).
    r.fuel = b.resumeCost;
    if (hasWallLimits()) {
        r.wallStart = monotonicNanos();
        if (b.wallSoftNanos) r.wallSoftAt = saturatingAdd(r.wallStart, b.wallSoftNanos);
        if (b.wallKillNanos) r.wallKillAt = saturatingAdd(r.wallStart, b.wallKillNanos);
        if (b.wallBackstopNanos) r.wallBackstopAt = saturatingAdd(r.wallStart, b.wallBackstopNanos);
        r.nextWallCheck = r.fuel + kWallCheckInterval;
    }
    updateNextCheck(r);
}

void VmState::updateNextCheck(RunContext& r) noexcept {
    r.nextCheck =
        r.killed ? 0 : std::min({r.killFuel, r.nextWallCheck, r.overBudget ? kNoLimit : r.softFuel});
}

// Slow path of a charge or safepoint (the fuel is already added): sticky kill, fuel_kill, soft
// budget, wall. Every exit re-arms the VM counter, including the raising ones.
void VmState::chargeSlow(lua_State* L, RunContext& r, u64 fuel) {
    if (r.killed) {
        r.fuel -= std::min(fuel, r.fuel); // a killed run spends nothing more
        armCounter(r);                    // at distance 0 (nextCheck is 0 once killed)
        raiseKill(L, r);                  // sticky: every later safepoint and charge raises again
    }
    if (r.fuel >= r.killFuel) {
        // The charge that trips the kill is not spent beyond the limit: the call it pays for never
        // happens, so the resume is charged exactly fuel_kill.
        r.fuel = r.killFuel;
        if (!r.overBudget && r.fuel >= r.softFuel) markOverBudget(r);
        kill(L, r, KillReason::Fuel);
    }
    if (!r.overBudget && r.fuel >= r.softFuel) markOverBudget(r);
    if (r.fuel >= r.nextWallCheck) wallCheck(L, r);
    updateNextCheck(r);
    armCounter(r);
}

// Reads the clock (r.fuel must be current) and re-arms the counter.
void VmState::wallCheck(lua_State* L, RunContext& r) {
    if (r.nextWallCheck == kNoLimit) return;
    r.nextWallCheck = r.fuel + kWallCheckInterval;
    const u64 t = monotonicNanos();
    if (t >= r.wallBackstopAt) kill(L, r, KillReason::WallBackstop);
    if (t >= r.wallKillAt) kill(L, r, KillReason::WallBudget);
    if (!r.overBudget && t >= r.wallSoftAt) markOverBudget(r);
    updateNextCheck(r);
    armCounter(r);
}

void VmState::markOverBudget(RunContext& r) {
    r.overBudget = true;
    ++stats.overBudget;
    ScriptEvent ev;
    ev.kind = ScriptEventKind::OverBudget;
    ev.tick = tickIndex;
    ev.task = TaskId{r.task.toBits()};
    ev.owner = r.owner;
    ev.module = r.module < modules.size() ? std::string_view(modules[r.module]->name) : std::string_view();
    ev.fuel = r.fuel;
    emit(ev);
}

void VmState::kill(lua_State* L, RunContext& r, KillReason reason) {
    r.killed = true;
    r.killReason = reason;
    r.nextCheck = 0;
    armCounter(r); // due: the next safepoint or charge takes the slow path and raises again
    ++stats.killsByReason[static_cast<usize>(reason)];
    raiseKill(L, r);
}

void VmState::raiseKill(lua_State* L, const RunContext& r) {
    const int ref = killErrorRefs[static_cast<usize>(r.killReason)];
    if (ensureStack(L, 1) && ref != LUA_NOREF) {
        lua_getref(L, ref); // pre-built and frozen: raising a kill never allocates
    } else {
        lua_pushnil(L);
    }
    lua_error(L);
}

int VmState::invokeBinding(lua_State* L, BindingInfo& info) {
    ++stats.bindingCalls;
    ++info.calls;
    RunContext* r = run;
    if (r) {
        const u64 items = info.cost.itemsArg > 0 ? itemsOfArg(L, info.cost.itemsArg) : 0;
        chargeRun(L, *r, info.cost.charge(items)); // before any side effect
        if (r->nextWallCheck != kNoLimit) {        // bindings may be slow: always read the clock
            r->fuel = fuelOf(*r);
            wallCheck(L, *r);
        }
    }
    struct BindingScope {
        VmState& s;
        const BindingInfo* prevInfo;
        lua_State* prevThread;
        ~BindingScope() {
            s.currentBinding = prevInfo;
            s.bindingThread = prevThread;
        }
    } scope{*this, currentBinding, bindingThread};
    currentBinding = &info;
    bindingThread = L;
    const int results = info.fn(L);
    // A binding cannot swallow a kill raised in a callback it made (callExport / callLuau).
    if (r && r->killed) raiseKill(L, *r);
    return results;
}

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------
void VmState::captureStack(lua_State* thread, int firstLevel, std::vector<StackFrame>& out) const {
    lua_Debug ar;
    for (int level = firstLevel; level < firstLevel + 64; ++level) {
        std::memset(&ar, 0, sizeof(ar));
        if (!lua_getinfo(thread, level, "sln", &ar)) break;
        StackFrame frame;
        frame.source = ar.short_src ? ar.short_src : "?";
        frame.line = ar.currentline;
        if (ar.name) {
            frame.function = ar.name;
        } else if (ar.what && std::strcmp(ar.what, "main") == 0) {
            frame.function = "<main>";
        }
        out.push_back(std::move(frame));
    }
}

void VmState::setLastError(const ScriptError& error) {
    lastError = error;
    HELIOS_LOG_DEBUG(LogScript, "[{}] {}", config.name, error.toString());
}

void VmState::emit(const ScriptEvent& event) const {
    if (config.onEvent) config.onEvent(event);
}

void VmState::raiseTyped(lua_State* L, ScriptErrorCode code, std::string_view message) {
    lua_checkstack(L, 3);
    luaL_where(L, 1); // "module:line: " of the calling Luau function
    usize whereLen = 0;
    const char* where = lua_tolstring(L, -1, &whereLen);
    std::string text(where, whereLen);
    text.append(message);
    lua_pop(L, 1);
    pushScriptError(L, code, KillReason::None, text);
    lua_error(L);
}

int VmState::protectRaw(lua_State* thread, void (*fn)(lua_State*, void*), void* data, ScriptError* error) {
    const int top = lua_gettop(thread);
    ProtectCall call{fn, data};
    const int status = lua_cpcall(thread, &protectTrampoline, &call);
    if (status == LUA_ERRMEM && run == nullptr && mainL != nullptr) {
        // Garbage may still hold the heap (Luau has no emergency GC): collect it so the next call
        // can succeed, but never retry here. Bodies are not idempotent: VM setup runs the host's
        // registrar and spawnExport its PushArgs callback, which must not run twice.
        lua_settop(thread, top);
        collectAfterOom();
    }
    if (status != LUA_OK && error) readErrorObject(thread, status, *error);
    lua_settop(thread, top);
    return status;
}

Result<void> scriptErrorToResult(const ScriptError& error) {
    ErrorCode code = ErrorCode::Unknown;
    switch (error.code) {
    case ScriptErrorCode::None: return {};
    case ScriptErrorCode::Runtime: code = ErrorCode::Unknown; break;
    case ScriptErrorCode::Compile: code = ErrorCode::ParseError; break;
    case ScriptErrorCode::OutOfMemory: code = ErrorCode::OutOfMemory; break;
    case ScriptErrorCode::StaleHandle: code = ErrorCode::InvalidState; break;
    case ScriptErrorCode::Killed: code = ErrorCode::LimitExceeded; break;
    case ScriptErrorCode::Cancelled: code = ErrorCode::Cancelled; break;
    case ScriptErrorCode::ModuleDisabled: code = ErrorCode::PermissionDenied; break;
    case ScriptErrorCode::NotFound: code = ErrorCode::NotFound; break;
    case ScriptErrorCode::HostError: code = ErrorCode::IoError; break;
    case ScriptErrorCode::InvalidState: code = ErrorCode::InvalidState; break;
    }
    std::string message(scriptErrorCodeName(error.code));
    if (error.killReason != KillReason::None)
        message += std::format(" ({})", killReasonName(error.killReason));
    message += ": ";
    message += error.message;
    return Error{code, std::move(message)};
}

int tracebackHandler(lua_State* L) {
    VmState* s = stateOf(L);
    s->capturedFrames.clear();
    s->captureStack(L, 1, s->capturedFrames); // level 0 is this handler
    return 1;                                 // the error object, unchanged
}

// ---------------------------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------------------------
Module* VmState::findModule(std::string_view name) noexcept {
    const auto it = moduleIndex.find(std::string(name));
    return it == moduleIndex.end() ? nullptr : modules[it->second].get();
}

void VmState::loadChunk(lua_State* L, Module& m, const BytecodePtr& bytecode) {
    MemcatScope memcat(*this, L, m.memcat);
    lua_checkstack(L, 2);
    // Per-module environment: a fresh table proxying reads to the frozen globals (safeenv).
    lua_State* envThread = lua_newthread(L);
    luaL_sandboxthread(envThread);
    const int rc = luau_load(envThread, m.chunkName.c_str(), bytecode->data.data(), bytecode->data.size(), 0);
    lua_xmove(envThread, L, 1); // the function, or the load error message
    lua_remove(L, -2);          // the helper thread
    if (rc != 0) lua_error(L);
    if (codegen && m.options.native) luau_codegen_compile(L, -1);
}

void VmState::instantiateInline(lua_State* L, Module& m) {
    // A disabled module is refused even if it was required before (its cached exports would
    // otherwise keep handing its code to new callers).
    if (m.disabled)
        raiseTyped(L, ScriptErrorCode::ModuleDisabled, std::format("module '{}' is disabled", m.name));
    if (m.instantiated) {
        lua_getref(L, m.exportsRef);
        return;
    }
    if (m.instantiating)
        raiseTyped(L, ScriptErrorCode::Runtime, std::format("cyclic require of module '{}'", m.name));
    struct Flag {
        bool& flag;
        ~Flag() { flag = false; }
    } flag{m.instantiating};
    m.instantiating = true;
    {
        MemcatScope memcat(*this, L, m.memcat);
        loadChunk(L, m, m.bytecode);
        lua_call(L, 0, 1); // not yieldable: a module's top level cannot wait
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    m.exportsRef = lua_ref(L, -1);
    lua_pop(L, 1);
    m.instantiated = true;
}

namespace {
int instantiateThunk(lua_State* L) {
    auto* m = static_cast<Module*>(lua_tolightuserdata(L, 1));
    stateOf(L)->instantiateInline(L, *m);
    return 1;
}
} // namespace

Result<void> VmState::instantiateTopLevel(Module& m) {
    if (m.instantiated) return {};
    if (m.disabled) return Error{ErrorCode::PermissionDenied, std::format("module '{}' is disabled", m.name)};
    const u32 index = moduleIndex.at(m.name);
    RunScope scope(*this, index);
    ScriptError err;
    bool failed = false;
    auto body = [&](lua_State* L) {
        lua_checkstack(L, 4);
        lua_pushcfunction(L, &tracebackHandler, "traceback");
        const int errIdx = lua_gettop(L);
        capturedFrames.clear();
        lua_pushcfunction(L, &instantiateThunk, "require");
        lua_pushlightuserdata(L, &m);
        const int status = lua_pcall(L, 1, 1, errIdx);
        if (status != LUA_OK) {
            failed = true;
            readErrorObject(L, status, err);
            err.stack = capturedFrames;
        }
    };
    if (protect(body, &err) != LUA_OK) failed = true;
    return finishTopLevel(scope.context(), index, failed, err);
}

void VmState::collectAfterOom() {
    // Luau has no emergency GC: after an allocation failure the garbage that filled the heap is
    // only reclaimed by later collection steps, which themselves need allocations to trigger.
    lua_gc(mainL, LUA_GCCOLLECT, 0);
}

Result<void> VmState::finishTopLevel(RunContext& r, u32 module, bool failed, ScriptError& err) {
    if (r.armed) { // still the active run (RunScope releases the counter afterwards)
        r.fuel = fuelOf(r);
        armCounter(r);
    }
    ++stats.resumes;
    stats.fuelTotal = saturatingAdd(stats.fuelTotal, r.fuel);
    if (allocFailures != r.allocFailuresAtStart) collectAfterOom();
    if (module < modules.size()) err.module = modules[module]->name;
    if (!r.killed && !failed) return {};
    normalizeKill(err, r);
    ScriptEvent ev;
    ev.tick = tickIndex;
    ev.owner = r.owner;
    ev.module = err.module;
    ev.fuel = r.fuel;
    if (r.killed) {
        err.code = ScriptErrorCode::Killed;
        err.killReason = r.killReason;
        if (err.message.empty()) err.message = "script killed";
        ev.kind = ScriptEventKind::TaskKilled;
        ev.killReason = r.killReason;
    } else {
        ev.kind = ScriptEventKind::TaskFailed;
    }
    ev.error = &err;
    setLastError(err);
    emit(ev);
    if (r.killed && module < modules.size()) recordKill(*modules[module]);
    return scriptErrorToResult(err);
}

void VmState::recordKill(Module& m) {
    if (config.killsToDisableModule == 0) return;
    const u64 windowStart = now > config.killWindowNanos ? now - config.killWindowNanos : 0;
    std::erase_if(m.killTimes, [&](u64 t) { return t < windowStart; });
    m.killTimes.push_back(now);
    if (!m.disabled && m.killTimes.size() >= config.killsToDisableModule) {
        disableModule(m, std::format("{} kills within {} s", m.killTimes.size(),
                                     config.killWindowNanos / 1'000'000'000ull));
    }
}

VmState::~VmState() {
    if (mainL) {
        lua_close(mainL);
        mainL = nullptr;
        HELIOS_ASSERT(heapBytes == 0, "Luau heap not fully released ({} bytes)", heapBytes);
    }
}

} // namespace detail

using detail::Module;
using detail::OwnerGuard;
using detail::RunScope;
using detail::VmState;

// ---------------------------------------------------------------------------------------------
// ScriptVm: creation
// ---------------------------------------------------------------------------------------------
ScriptVm::ScriptVm(std::unique_ptr<detail::VmState> state) noexcept : m_state(std::move(state)) {
    m_state->scriptVm = this;
}

ScriptVm::~ScriptVm() = default;

Result<std::unique_ptr<ScriptVm>> ScriptVm::create(const VmConfig& config, const ApiRegistrar& registerApi) {
    if (config.heapLimitBytes < (256u << 10)) {
        return Error{ErrorCode::InvalidArgument, "VmConfig::heapLimitBytes must be at least 256 KiB"};
    }
    if (config.enableNativeCodegen && config.profile == HostProfile::Cell) {
        // 02 §7.4: cells and world-script hosts (which run the cell profile) refuse native codegen.
        // The vendored codegen-fornloop-fuel patch, which makes native fuel equal to the
        // interpreter's, is the precondition for lifting this, not the lift itself: that is 02 §8.1's
        // P3 "codegen opt-in on cells", gated by 04 §10.2's interpreter-vs-native corpus run.
        return Error{ErrorCode::InvalidArgument,
                     std::format("ScriptVm '{}': native codegen is refused on cells and world-script hosts "
                                 "(02 §7.4); use the interpreter",
                                 config.name)};
    }
    auto newState = std::make_unique<VmState>();
    newState->config = config;
    newState->deterministic = config.profile == HostProfile::Cell;
    newState->tag = config.memoryTag != MemoryTag::Unknown ? config.memoryTag : registerMemoryTag("Script");
    newState->cache = config.bytecodeCache ? config.bytecodeCache : std::make_shared<BytecodeCache>();
    newState->rng.reseed(config.randomSeed);

    lua_State* L = lua_newstate(&detail::luauAlloc, newState.get());
    if (!L) return Error{ErrorCode::OutOfMemory, "lua_newstate failed"};
    newState->mainL = L;
    lua_Callbacks* callbacks = lua_callbacks(L);
    callbacks->userdata = newState.get();
    callbacks->interrupt = &detail::luauInterrupt;
    // No run is active: host-side setup runs unmetered and never reaches the interrupt at gc < 0.
    newState->fuelCounter = lua_fuelcounter(L);
    *newState->fuelCounter = static_cast<i64>(detail::kMaxCounterDistance);
    lua_setthreaddata(L, &newState->taskThreadMarker); // scripts may never resume the main thread
    if (config.enableNativeCodegen && luau_codegen_supported()) {
        luau_codegen_create(L);
        newState->codegen = true;
    }

    std::unique_ptr<ScriptVm> vm(new ScriptVm(std::move(newState)));
    VmState& s = *vm->m_state;
    ScriptError err;
    // Normative order (02 §7.4): openlibs, Helios API, then the sandbox freezes everything.
    auto setup = [&](lua_State* mainThread) {
        luaL_openlibs(mainThread);
        s.installStdlib();
        s.installBuiltinWrappers();
        if (registerApi) {
            Binder binder(s);
            registerApi(binder);
        }
        luaL_sandbox(mainThread);
    };
    if (s.protect(setup, &err) != LUA_OK) {
        return Error{err.code == ScriptErrorCode::OutOfMemory ? ErrorCode::OutOfMemory : ErrorCode::Unknown,
                     "Luau VM setup failed: " + err.message};
    }
    HELIOS_LOG_DEBUG(LogScript, "ScriptVm '{}' created ({} KiB heap, codegen {})", config.name,
                     s.heapBytes >> 10, s.codegen ? "on" : "off");
    return vm;
}

// ---------------------------------------------------------------------------------------------
// ScriptVm: modules
// ---------------------------------------------------------------------------------------------
Result<void> ScriptVm::loadModule(std::string_view name, std::string_view source,
                                  const ModuleOptions& options) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    if (name.empty()) return Error{ErrorCode::InvalidArgument, "module name is empty"};
    if (s.findModule(name))
        return Error{ErrorCode::AlreadyExists, std::format("module '{}' already loaded", name)};
    const CompileOptions compileOptions = options.compile.value_or(s.config.compileOptions);
    auto bytecode = s.cache->getOrCompile(source, compileOptions, name);
    if (!bytecode) {
        ScriptError err;
        err.code = ScriptErrorCode::Compile;
        err.module = std::string(name);
        err.message = bytecode.error().message;
        s.setLastError(err);
        return bytecode.error();
    }
    auto module = std::make_unique<Module>();
    module->name = std::string(name);
    module->chunkName = "@" + module->name;
    module->bytecode = *bytecode;
    module->options = options;
    module->compileOptions = compileOptions;
    module->version = 1;
    module->memcat =
        s.nextMemcat < detail::kOverflowMemcat ? static_cast<u8>(s.nextMemcat++) : detail::kOverflowMemcat;
    s.moduleIndex.emplace(module->name, static_cast<u32>(s.modules.size()));
    s.modules.push_back(std::move(module));
    return {};
}

namespace {
struct ReloadContext {
    VmState* state;
    Module* module;
    BytecodePtr bytecode;
};

int reloadThunk(lua_State* L) {
    auto* ctx = static_cast<ReloadContext*>(lua_tolightuserdata(L, 1));
    VmState& s = *ctx->state;
    Module& m = *ctx->module;
    {
        detail::MemcatScope memcat(s, L, m.memcat);
        s.loadChunk(L, m, ctx->bytecode);
        lua_call(L, 0, 1);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    // Migration hook: new.__reload(oldExports).
    if (lua_istable(L, -1)) {
        lua_rawgetfield(L, -1, "__reload");
        if (lua_isfunction(L, -1)) {
            lua_getref(L, m.exportsRef);
            lua_call(L, 1, 0);
        } else {
            lua_pop(L, 1);
        }
    }
    return 1;
}
} // namespace

Result<void> ScriptVm::reloadModule(std::string_view name, std::string_view source) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    if (s.run) return Error{ErrorCode::InvalidState, "modules reload only between resumes (tick boundary)"};
    Module* m = s.findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    auto bytecode = s.cache->getOrCompile(source, m->compileOptions, name);
    if (!bytecode) {
        ScriptError err;
        err.code = ScriptErrorCode::Compile;
        err.module = m->name;
        err.message = bytecode.error().message;
        s.setLastError(err);
        return bytecode.error(); // the old version stays active
    }

    auto commit = [&] {
        m->bytecode = *bytecode;
        ++m->version;
        m->disabled = false;
        m->killTimes.clear();
        ScriptEvent ev;
        ev.kind = ScriptEventKind::ModuleReloaded;
        ev.tick = s.tickIndex;
        ev.module = m->name;
        s.emit(ev);
    };
    if (!m->instantiated) {
        commit();
        return {};
    }

    const u32 index = s.moduleIndex.at(m->name);
    RunScope scope(s, index);
    ScriptError err;
    bool failed = false;
    int newRef = LUA_NOREF;
    ReloadContext ctx{&s, m, *bytecode};
    auto body = [&](lua_State* L) {
        lua_checkstack(L, 4);
        lua_pushcfunction(L, &detail::tracebackHandler, "traceback");
        const int errIdx = lua_gettop(L);
        s.capturedFrames.clear();
        lua_pushcfunction(L, &reloadThunk, "reload");
        lua_pushlightuserdata(L, &ctx);
        const int status = lua_pcall(L, 1, 1, errIdx);
        if (status != LUA_OK) {
            failed = true;
            detail::readErrorObject(L, status, err);
            err.stack = s.capturedFrames;
            return;
        }
        newRef = lua_ref(L, -1);
    };
    if (s.protect(body, &err) != LUA_OK) failed = true;
    if (Result<void> finished = s.finishTopLevel(scope.context(), index, failed, err); !finished) {
        // A run killed after the chunk returned (no safepoint left to raise at) still fails.
        if (newRef != LUA_NOREF) lua_unref(s.mainL, newRef);
        return finished.error();
    }
    lua_unref(s.mainL, m->exportsRef);
    m->exportsRef = newRef;
    commit();
    return {};
}

Result<void> ScriptVm::instantiateModule(std::string_view name) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    if (s.run)
        return Error{ErrorCode::InvalidState, "instantiateModule cannot run inside a resume; use require"};
    Module* m = s.findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    return s.instantiateTopLevel(*m);
}

bool ScriptVm::hasModule(std::string_view name) const { return m_state->findModule(name) != nullptr; }

std::optional<ModuleInfo> ScriptVm::moduleInfo(std::string_view name) const {
    const Module* m = m_state->findModule(name);
    if (!m) return std::nullopt;
    ModuleInfo info;
    info.name = m->name;
    info.version = m->version;
    info.memoryCategory = m->memcat;
    info.instantiated = m->instantiated;
    info.disabled = m->disabled;
    info.native = m->options.native && m_state->codegen;
    info.recentKills = static_cast<u32>(m->killTimes.size());
    info.heapBytes = lua_totalbytes(m_state->mainL, m->memcat);
    info.sourceHash = m->bytecode->sourceHash;
    return info;
}

Result<void> ScriptVm::enableModule(std::string_view name) {
    OwnerGuard guard(*m_state);
    Module* m = m_state->findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    m->disabled = false;
    m->killTimes.clear();
    return {};
}

// ---------------------------------------------------------------------------------------------
// ScriptVm: diagnostics
// ---------------------------------------------------------------------------------------------
VmStats ScriptVm::stats() const {
    const VmState& s = *m_state;
    VmStats out = s.stats;
    out.liveTasks = s.tasks.size();
    out.queuedEntries = s.timers.size() + s.ready.size();
    out.heapBytes = s.heapBytes;
    out.heapPeakBytes = s.heapPeak;
    out.heapLimitBytes = s.config.heapLimitBytes;
    out.allocationFailures = s.allocFailures;
    return out;
}

const ScriptError& ScriptVm::lastError() const noexcept { return m_state->lastError; }
u64 ScriptVm::tickIndex() const noexcept { return m_state->tickIndex; }
u64 ScriptVm::zoneTimeNanos() const noexcept { return m_state->now; }
std::vector<std::string> ScriptVm::apiManifest() const { return m_state->manifest; }
bool ScriptVm::nativeCodegenActive() const noexcept { return m_state->codegen; }
const VmConfig& ScriptVm::config() const noexcept { return m_state->config; }
lua_State* ScriptVm::state() const noexcept { return m_state->mainL; }

void ScriptVm::collectGarbage() {
    OwnerGuard guard(*m_state);
    HELIOS_ASSERT(m_state->run == nullptr, "collectGarbage inside a resume");
    lua_gc(m_state->mainL, LUA_GCCOLLECT, 0);
}

} // namespace helios::script
