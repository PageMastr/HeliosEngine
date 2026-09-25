// Coroutine task scheduler: tasks are Luau threads resumed in (wake tick, owner EntityId, seq)
// order on the zone clock while lane fuel remains; wait/yield/checkpoint/async bindings are
// explicit yields delivered back through a continuation; kills close the coroutine.

#include <algorithm>
#include <format>
#include <optional>

#include "helios/core/assert.h"
#include "vm_state.h"

namespace helios::script {
namespace detail {

// ---------------------------------------------------------------------------------------------
// Task lifecycle
// ---------------------------------------------------------------------------------------------
TaskHandle VmState::createTask(lua_State* thread, int threadRef, u32 module, u64 owner, int nargs) {
    const TaskHandle h = tasks.create();
    Task* t = tasks.get(h);
    t->thread = thread;
    t->threadRef = threadRef;
    t->module = module;
    t->owner = owner;
    t->pendingArgs = nargs;
    t->state = TaskState::Ready;
    return h;
}

void VmState::makeReady(TaskHandle h, Task& task, u64 wakeTick) {
    task.state = TaskState::Ready;
    task.seq = ++seqCounter;
    task.wakeTick = wakeTick;
    ready.push(QueueEntry{wakeTick, task.owner, task.seq, h.toBits()});
}

void VmState::addTimer(TaskHandle h, Task& task, u64 wakeNanos) {
    task.state = TaskState::Waiting;
    task.seq = ++seqCounter;
    task.wakeNanos = wakeNanos;
    timers.push(QueueEntry{wakeNanos, task.owner, task.seq, h.toBits()});
}

void VmState::finishTask(TaskHandle h, ScriptEventKind kind, const ScriptError* error, u64 fuel) {
    Task* t = tasks.get(h);
    if (!t) return;
    lua_State* co = t->thread;
    const int ref = t->threadRef;
    const u32 module = t->module;
    const u64 owner = t->owner;
    if (t->awaiting.isValid()) asyncOps.destroy(t->awaiting);
    tasks.destroy(h);            // gone before any Luau work, so re-entrant queries see a stale id
    if (co) lua_resetthread(co); // closes upvalues and drops the stack; never runs Luau code
    lua_unref(mainL, ref);
    switch (kind) {
    case ScriptEventKind::TaskFinished: ++stats.tasksFinished; break;
    case ScriptEventKind::TaskFailed: ++stats.tasksFailed; break;
    case ScriptEventKind::TaskKilled: ++stats.tasksKilled; break;
    case ScriptEventKind::TaskCancelled: ++stats.tasksCancelled; break;
    default: break;
    }
    ScriptEvent ev;
    ev.kind = kind;
    ev.tick = tickIndex;
    ev.task = TaskId{h.toBits()};
    ev.owner = owner;
    ev.module = module < modules.size() ? std::string_view(modules[module]->name) : std::string_view();
    ev.fuel = fuel;
    ev.killReason = error ? error->killReason : KillReason::None;
    ev.error = error;
    emit(ev);
    compactQueues();
}

void VmState::compactQueues() {
    // Amortized O(1): a compaction leaves at most one entry per live task, and the next one waits
    // for another kQueueSlack + live-task-count stale entries.
    constexpr usize kQueueSlack = 256;
    const usize live = tasks.size();
    auto staleIn = [this](TaskState expected) {
        return [this, expected](const QueueEntry& e) {
            const Task* t = tasks.get(TaskHandle::fromBits(e.task));
            return !t || t->seq != e.seq || t->state != expected;
        };
    };
    if (timers.size() > 2 * live + kQueueSlack) timers.compact(staleIn(TaskState::Waiting));
    if (ready.size() > 2 * live + kQueueSlack) ready.compact(staleIn(TaskState::Ready));
}

bool VmState::cancelTaskInternal(TaskHandle h) {
    Task* t = tasks.get(h);
    if (!t || t->state == TaskState::Running) return false;
    finishTask(h, ScriptEventKind::TaskCancelled, nullptr, 0);
    return true;
}

void VmState::disableModule(Module& m, std::string_view reason) {
    m.disabled = true;
    const u32 index = moduleIndex.at(m.name);
    std::vector<TaskHandle> victims;
    tasks.forEach([&](TaskHandle h, Task& t) {
        if (t.module == index && t.state != TaskState::Running) victims.push_back(h);
    });
    for (const TaskHandle h : victims) finishTask(h, ScriptEventKind::TaskCancelled, nullptr, 0);
    HELIOS_LOG_WARN(LogScript, "[{}] module '{}' disabled: {}", config.name, m.name, reason);
    ScriptEvent ev;
    ev.kind = ScriptEventKind::ModuleDisabled;
    ev.tick = tickIndex;
    ev.module = m.name;
    emit(ev);
}

// ---------------------------------------------------------------------------------------------
// Yields and continuations
// ---------------------------------------------------------------------------------------------
bool VmState::requireYieldableTask(lua_State* state, const char* what, bool raise) {
    const bool ok = runningTask() != nullptr && state == run->thread && lua_isyieldable(state);
    if (!ok && raise) {
        raiseTyped(
            state, ScriptErrorCode::InvalidState,
            std::format("{} can only yield in a task's own coroutine (not in a metamethod, C call, callback, "
                        "module load or nested coroutine)",
                        what));
    }
    return ok;
}

int VmState::yieldTask(lua_State* state, YieldReason reason, ResumeKind kind) {
    Task* t = runningTask();
    t->resumeKind = kind;
    run->yield = reason;
    ++stats.yieldsByReason[static_cast<usize>(reason)];
    lua_settop(state, 0); // the continuation sees only what it pushes on resume
    return lua_yield(state, 0);
}

// Runs inside the protected lua_resume, so results pushed here (async payloads) can never throw
// into host code.
int taskContinuation(lua_State* L, int status) {
    VmState* s = stateOf(L);
    Task* t = s->runningTask();
    if (!t || status != LUA_OK) return lua_gettop(L);
    const ResumeKind kind = t->resumeKind;
    t->resumeKind = ResumeKind::None;
    switch (kind) {
    case ResumeKind::Wait:
        lua_pushnumber(L, static_cast<double>(s->now - t->waitStartNanos) * 1e-9);
        return 1;
    case ResumeKind::Checkpoint: lua_pushboolean(L, 1); return 1;
    case ResumeKind::TaskYield: return 0;
    case ResumeKind::Async: {
        AsyncOp* op = s->asyncOps.get(t->awaiting);
        if (!op) s->raiseTyped(L, ScriptErrorCode::InvalidState, "async call resumed without a completion");
        PushArgs results = std::move(op->results);
        const bool failed = op->failed;
        std::string error = std::move(op->error);
        s->asyncOps.destroy(t->awaiting);
        t->awaiting = {};
        if (failed) s->raiseTyped(L, ScriptErrorCode::HostError, error);
        return results ? results(L) : 0;
    }
    case ResumeKind::None: break;
    }
    return lua_gettop(L);
}

// ---------------------------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------------------------
ScriptError VmState::errorFromThread(lua_State* thread, int status, const RunContext& r) {
    ScriptError e;
    captureStack(thread, 0, e.stack);
    if (lua_gettop(thread) > 0) {
        auto body = [&](lua_State* state) {
            lua_checkstack(state, 4);
            lua_xmove(thread, state, 1);
            readErrorObject(state, status, e);
        };
        if (protect(body) != LUA_OK) {
            e.code = ScriptErrorCode::Runtime;
            e.message = "(error object unavailable)";
        }
    } else {
        e.code = status == LUA_ERRMEM ? ScriptErrorCode::OutOfMemory : ScriptErrorCode::Runtime;
    }
    if (r.killed) {
        e.code = ScriptErrorCode::Killed;
        e.killReason = r.killReason;
    }
    normalizeKill(e, r);
    if (r.module < modules.size()) e.module = modules[r.module]->name;
    return e;
}

void VmState::resumeTask(TaskHandle h, TickStats& ts) {
    Task* t = tasks.get(h);
    Module& module = *modules[t->module];
    if (module.disabled) {
        finishTask(h, ScriptEventKind::TaskCancelled, nullptr, 0);
        return;
    }
    RunContext r;
    beginRun(r, h, t->module, t->owner, t->thread);
    t->state = TaskState::Running;
    const int nargs = t->started ? 0 : t->pendingArgs;
    t->started = true;
    t->pendingArgs = 0;

    HELIOS_ASSERT(run == nullptr, "nested task resume");
    run = &r;
    RunContext* const prevActive = t_activeRun;
    t_activeRun = &r;
    const u8 prevMemcat = activeMemcat;
    activeMemcat = module.memcat; // the task thread carries the module's category
    const int status = lua_resume(t->thread, nullptr, nargs);
    run = nullptr;
    t_activeRun = prevActive;
    activeMemcat = prevMemcat;

    ++stats.resumes;
    ++ts.resumed;
    stats.fuelTotal += r.fuel;
    ts.fuel += r.fuel;
    ts.maxResumeFuel = std::max(ts.maxResumeFuel, r.fuel);
    t = tasks.get(h); // HandlePool addresses are stable, but re-fetch for clarity
    if (allocFailures != r.allocFailuresAtStart) collectAfterOom();

    if (r.killed) {
        const ScriptError e = errorFromThread(t->thread, status, r);
        ++ts.killed;
        setLastError(e);
        finishTask(h, ScriptEventKind::TaskKilled, &e, r.fuel);
        recordKill(module); // may disable the module (three kills within the window)
        return;
    }
    switch (status) {
    case LUA_YIELD:
        if (r.yield == YieldReason::None) {
            // Not produced by an explicit call. Cannot happen by construction (the interrupt never
            // yields); counted so RT-13's instrumented-yield check can assert it stays zero.
            ++stats.involuntaryYields;
            makeReady(h, *t, tickIndex + 1);
        } else if (r.yield == YieldReason::Coroutine) {
            makeReady(h, *t, tickIndex + 1);
        }
        break;
    case LUA_OK:
        ++ts.finished;
        finishTask(h, ScriptEventKind::TaskFinished, nullptr, r.fuel);
        break;
    default: {
        const ScriptError e = errorFromThread(t->thread, status, r);
        ++ts.failed;
        setLastError(e);
        finishTask(h, ScriptEventKind::TaskFailed, &e, r.fuel);
        break;
    }
    }
}

TickStats VmState::runTick(u64 zoneNanos) {
    TickStats ts;
    now = std::max(now, zoneNanos);
    ++tickIndex;
    ++stats.ticks;
    ts.tick = tickIndex;
    const u64 wallStart = monotonicNanos();

    // Expired waits become ready at this tick, in (deadline, owner, seq) order.
    while (!timers.empty() && timers.top().key <= now) {
        const QueueEntry e = timers.pop();
        const TaskHandle h = TaskHandle::fromBits(e.task);
        Task* t = tasks.get(h);
        if (!t || t->seq != e.seq || t->state != TaskState::Waiting) continue;
        makeReady(h, *t, tickIndex);
    }

    const FuelBudget& b = config.budget;
    while (!ready.empty()) {
        const QueueEntry& top = ready.top();
        if (top.key > tickIndex) break; // became ready during this tick: next tick
        const TaskHandle h = TaskHandle::fromBits(top.task);
        const Task* t = tasks.get(h);
        if (!t || t->seq != top.seq || t->state != TaskState::Ready) {
            ready.pop();
            continue;
        }
        // Lane budget: a resume only starts while lane fuel remains, so the lane spends at most
        // fuelPerTick + fuelKill (RT-13 bound).
        if (b.fuelPerTick != 0 && ts.fuel >= b.fuelPerTick) break;
        if (b.wallPerTickNanos != 0 && monotonicNanos() - wallStart >= b.wallPerTickNanos) break;
        ready.pop();
        resumeTask(h, ts);
    }
    for (const QueueEntry& e : ready.entries()) {
        const Task* t = tasks.get(TaskHandle::fromBits(e.task));
        if (e.key <= tickIndex && t && t->seq == e.seq && t->state == TaskState::Ready) ++ts.deferred;
    }
    ts.wallNanos = monotonicNanos() - wallStart;
    return ts;
}

} // namespace detail

using detail::Module;
using detail::OwnerGuard;
using detail::RunScope;
using detail::TaskHandle;
using detail::VmState;

// ---------------------------------------------------------------------------------------------
// ScriptVm: tasks
// ---------------------------------------------------------------------------------------------
Result<TaskId> ScriptVm::spawnScript(std::string_view name, u64 owner) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    Module* m = s.findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    if (m->disabled) return Error{ErrorCode::PermissionDenied, std::format("module '{}' is disabled", name)};
    const u32 index = s.moduleIndex.at(m->name);
    lua_State* thread = s.run ? s.bindingThread : s.mainL;
    if (!thread) return Error{ErrorCode::InvalidState, "spawnScript inside a resume but outside a binding"};
    TaskHandle handle;
    ScriptError err;
    auto body = [&](lua_State* L) {
        lua_checkstack(L, 4);
        s.loadChunk(L, *m, m->bytecode);  // [fn] with a fresh environment
        lua_State* co = lua_newthread(L); // [fn, co]
        lua_setthreaddata(co, &s.taskThreadMarker);
        lua_setmemcat(co, m->memcat);
        lua_insert(L, -2); // [co, fn]
        lua_xmove(L, co, 1);
        const int ref = lua_ref(L, -1);
        lua_pop(L, 1);
        handle = s.createTask(co, ref, index, owner, 0);
    };
    if (s.protect(body, &err, thread) != LUA_OK) return detail::scriptErrorToResult(err).error();
    s.makeReady(handle, *s.taskOf(handle), s.tickIndex + 1);
    ++s.stats.tasksSpawned;
    return TaskId{handle.toBits()};
}

Result<TaskId> ScriptVm::spawnExport(std::string_view name, std::string_view function, u64 owner,
                                     const PushArgs& args) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    Module* m = s.findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    if (m->disabled) return Error{ErrorCode::PermissionDenied, std::format("module '{}' is disabled", name)};
    if (!m->instantiated) {
        if (s.run)
            return Error{ErrorCode::InvalidState, "spawnExport of an uninstantiated module inside a resume"};
        HELIOS_TRY(s.instantiateTopLevel(*m));
    }
    const u32 index = s.moduleIndex.at(m->name);
    lua_State* thread = s.run ? s.bindingThread : s.mainL;
    if (!thread) return Error{ErrorCode::InvalidState, "spawnExport inside a resume but outside a binding"};
    const std::string fnName(function);
    TaskHandle handle;
    ScriptError err;
    auto body = [&](lua_State* L) {
        lua_checkstack(L, 4);
        lua_getref(L, m->exportsRef);
        if (!lua_istable(L, -1)) s.raiseTyped(L, ScriptErrorCode::NotFound, "module exports are not a table");
        lua_rawgetfield(L, -1, fnName.c_str());
        if (!lua_isfunction(L, -1)) {
            s.raiseTyped(L, ScriptErrorCode::NotFound,
                         std::format("module '{}' has no function '{}'", m->name, fnName));
        }
        lua_remove(L, -2);                    // [fn]
        const int nargs = args ? args(L) : 0; // [fn, args...]
        lua_State* co = lua_newthread(L);     // [fn, args..., co]
        lua_setthreaddata(co, &s.taskThreadMarker);
        lua_setmemcat(co, m->memcat);
        lua_insert(L, -(nargs + 2)); // [co, fn, args...]
        lua_checkstack(co, nargs + 1);
        lua_xmove(L, co, nargs + 1);
        const int ref = lua_ref(L, -1);
        lua_pop(L, 1);
        handle = s.createTask(co, ref, index, owner, nargs);
    };
    if (s.protect(body, &err, thread) != LUA_OK) return detail::scriptErrorToResult(err).error();
    s.makeReady(handle, *s.taskOf(handle), s.tickIndex + 1);
    ++s.stats.tasksSpawned;
    return TaskId{handle.toBits()};
}

namespace {
struct CallExportContext {
    VmState* state;
    Module* module;
    std::string function;
    const PushArgs* args;
    int nresults;
};

int callExportThunk(lua_State* L) {
    auto* c = static_cast<CallExportContext*>(lua_tolightuserdata(L, 1));
    lua_settop(L, 0);
    c->state->instantiateInline(L, *c->module);
    if (!lua_istable(L, -1))
        c->state->raiseTyped(L, ScriptErrorCode::NotFound, "module exports are not a table");
    lua_rawgetfield(L, -1, c->function.c_str());
    if (!lua_isfunction(L, -1)) {
        c->state->raiseTyped(L, ScriptErrorCode::NotFound,
                             std::format("module '{}' has no function '{}'", c->module->name, c->function));
    }
    lua_remove(L, -2);
    const int nargs = (c->args && *c->args) ? (*c->args)(L) : 0;
    lua_call(L, nargs, c->nresults); // not yieldable: callbacks cannot wait (use spawnExport)
    return c->nresults;
}
} // namespace

Result<void> ScriptVm::callExport(std::string_view name, std::string_view function, const PushArgs& args,
                                  const ReadResults& results, int nresults) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    if (nresults < 0) return Error{ErrorCode::InvalidArgument, "nresults must be >= 0"};
    Module* m = s.findModule(name);
    if (!m) return Error{ErrorCode::NotFound, std::format("module '{}' not loaded", name)};
    if (m->disabled) return Error{ErrorCode::PermissionDenied, std::format("module '{}' is disabled", name)};
    const u32 index = s.moduleIndex.at(m->name);
    // Inside a binding the callback runs nested in the current resume, on the binding's thread,
    // and shares its budget; otherwise it is its own top-level run (it counts as a resume).
    const bool nested = s.run != nullptr;
    lua_State* thread = nested ? s.bindingThread : s.mainL;
    if (!thread) return Error{ErrorCode::InvalidState, "callExport inside a resume but outside a binding"};
    std::optional<RunScope> scope;
    if (!nested) scope.emplace(s, index);
    detail::RunContext& run = nested ? *s.run : scope->context();

    CallExportContext ctx{&s, m, std::string(function), &args, nresults};
    ScriptError err;
    bool failed = false;
    auto body = [&](lua_State* L) {
        lua_checkstack(L, 4 + nresults);
        lua_pushcfunction(L, &detail::tracebackHandler, "traceback");
        const int errIdx = lua_gettop(L);
        s.capturedFrames.clear();
        lua_pushcfunction(L, &callExportThunk, "callExport");
        lua_pushlightuserdata(L, &ctx);
        const int status = lua_pcall(L, 1, nresults, errIdx);
        if (status != LUA_OK) {
            failed = true;
            detail::readErrorObject(L, status, err);
            err.stack = s.capturedFrames;
            return;
        }
        if (results) results(L, errIdx + 1, nresults);
    };
    if (s.protect(body, &err, thread) != LUA_OK) failed = true;
    if (!nested) return s.finishTopLevel(run, index, failed, err);
    err.module = m->name;
    detail::normalizeKill(err, run);
    if (run.killed) {
        // The outer resume is killed; the binding that called us re-raises it when it returns.
        err.code = ScriptErrorCode::Killed;
        err.killReason = run.killReason;
        return detail::scriptErrorToResult(err);
    }
    if (failed) {
        s.setLastError(err);
        return detail::scriptErrorToResult(err);
    }
    return {};
}

bool ScriptVm::cancelTask(TaskId task) {
    OwnerGuard guard(*m_state);
    return m_state->cancelTaskInternal(TaskHandle::fromBits(task.bits));
}

TaskState ScriptVm::taskState(TaskId task) const {
    const detail::Task* t = m_state->tasks.get(TaskHandle::fromBits(task.bits));
    return t ? t->state : TaskState::Gone;
}

u32 ScriptVm::liveTaskCount() const { return m_state->tasks.size(); }

TickStats ScriptVm::tick() {
    const VmState& s = *m_state;
    return tickAt(s.config.clock ? s.config.clock->gameTimeNanos() : s.now);
}

TickStats ScriptVm::tickAt(u64 zoneNanos) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    HELIOS_ASSERT(s.run == nullptr, "ScriptVm::tick called from inside a script resume");
    if (s.run) return TickStats{};
    return s.runTick(zoneNanos);
}

namespace detail {
// Shared by completeAsync/failAsync. A token completes at most once. The task is either suspended
// in the call (Awaiting: it becomes ready for the next tick) or still inside the async binding that
// issued the token (Running: a synchronous completion; yieldAsync then makes it ready itself).
AsyncOp* claimAsync(VmState& s, AsyncHandle h) {
    AsyncOp* op = s.asyncOps.get(h);
    if (!op || op->completed) return nullptr;
    Task* t = s.tasks.get(op->task);
    const bool live = t && t->awaiting == h &&
                      (t->state == TaskState::Awaiting ||
                       (t->state == TaskState::Running && s.run != nullptr && s.run->task == op->task));
    if (!live) {
        s.asyncOps.destroy(h);
        return nullptr;
    }
    op->completed = true;
    if (t->state == TaskState::Awaiting) s.makeReady(op->task, *t, s.tickIndex + 1);
    return op;
}
} // namespace detail

bool ScriptVm::completeAsync(AsyncToken token, PushArgs results) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    detail::AsyncOp* op = detail::claimAsync(s, detail::AsyncHandle::fromBits(token.bits));
    if (!op) return false;
    op->results = std::move(results);
    return true;
}

bool ScriptVm::failAsync(AsyncToken token, std::string message) {
    VmState& s = *m_state;
    OwnerGuard guard(s);
    detail::AsyncOp* op = detail::claimAsync(s, detail::AsyncHandle::fromBits(token.bits));
    if (!op) return false;
    op->failed = true;
    op->error = std::move(message);
    return true;
}

} // namespace helios::script
