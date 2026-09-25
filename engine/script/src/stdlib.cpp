// Helios built-in Luau API and sandbox policy: ScriptError, kill errors, removal of unsafe or
// nondeterministic globals, print/require, the task library, WorldPos, deterministic math.random,
// coroutine guards, and fuel-charging wrappers for builtins whose C work grows with their input.

#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#include "helios/core/assert.h"
#include "vm_state.h"

namespace helios::script::detail {
namespace {

// ---------------------------------------------------------------------------------------------
// ScriptError / kill errors
// ---------------------------------------------------------------------------------------------
struct ErrorHeader {
    u8 code;
    u8 reason;
    u32 length;
};

// ScriptError fields: code, message, reason (kills only).
int errorIndex(lua_State* L) {
    ScriptError e;
    if (!readScriptError(L, 1, e)) luaL_typeerrorL(L, 1, "ScriptError");
    const char* key = luaL_checkstring(L, 2);
    std::string_view value;
    if (std::strcmp(key, "code") == 0) {
        value = scriptErrorCodeName(e.code);
    } else if (std::strcmp(key, "message") == 0) {
        value = e.message;
    } else if (std::strcmp(key, "reason") == 0) {
        if (e.killReason == KillReason::None) {
            lua_pushnil(L);
            return 1;
        }
        value = killReasonName(e.killReason);
    } else {
        luaL_error(L, "'%s' is not a member of ScriptError", key);
    }
    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

int errorToString(lua_State* L) {
    ScriptError e;
    if (!readScriptError(L, 1, e)) luaL_typeerrorL(L, 1, "ScriptError");
    const std::string text = std::format("{}: {}", scriptErrorCodeName(e.code), e.message);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// ---------------------------------------------------------------------------------------------
// print / require
// ---------------------------------------------------------------------------------------------
int l_print(lua_State* L) {
    VmState* s = stateOf(L);
    const int n = lua_gettop(L);
    std::string text;
    u64 total = 0;
    u64 chargedBytes = 0;
    for (int i = 1; i <= n; ++i) {
        // Charged per byte of string arguments only: tostring() of a table or userdata embeds a
        // heap address whose printed length differs between runs, which must not change fuel.
        if (lua_type(L, i) == LUA_TSTRING) chargedBytes += static_cast<u64>(lua_objlen(L, i));
        usize len = 0;
        const char* part = luaL_tolstring(L, i, &len);
        total += len + (i > 1 ? 1 : 0);
        // Bounded: the line lives on the host heap (outside the VM cap) and goes to the log.
        if (text.size() < kMaxPrintBytes) {
            if (i > 1) text += '\t';
            text.append(part, std::min<usize>(len, kMaxPrintBytes - std::min(text.size(), kMaxPrintBytes)));
        }
        lua_pop(L, 1);
    }
    if (total > text.size()) text += std::format("... ({} bytes truncated)", total - text.size());
    chargeFuel(L, chargedBytes / 64); // untruncated: truncation never makes printing cheaper
    const std::string_view module = s->run && s->run->module < s->modules.size()
                                        ? std::string_view(s->modules[s->run->module]->name)
                                        : "?";
    HELIOS_LOG_INFO(LogScript, "[{}:{}] {}", s->config.name, module, text);
    if (s->config.onPrint) s->config.onPrint(module, text);
    return 0;
}

int l_require(lua_State* L) {
    VmState* s = stateOf(L);
    usize len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    Module* m = s->findModule(std::string_view(name, len));
    if (!m)
        s->raiseTyped(L, ScriptErrorCode::NotFound,
                      std::format("module '{}' not found", std::string_view(name, len)));
    lua_settop(L, 0);
    s->instantiateInline(L, *m);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// task library
// ---------------------------------------------------------------------------------------------
int l_wait(lua_State* L) {
    VmState* s = stateOf(L);
    const double seconds = luaL_optnumber(L, 1, 0.0);
    if (!(seconds >= 0.0 && seconds <= 1e9)) luaL_argerrorL(L, 1, "expected seconds in [0, 1e9]");
    s->requireYieldableTask(L, "wait()", true);
    Task* task = s->runningTask();
    task->waitStartNanos = s->now;
    const u64 delta = static_cast<u64>(std::llround(seconds * 1e9));
    s->addTimer(s->run->task, *task, s->now + delta);
    return s->yieldTask(L, YieldReason::Wait, ResumeKind::Wait);
}

int l_taskYield(lua_State* L) {
    VmState* s = stateOf(L);
    s->requireYieldableTask(L, "task.yield()", true);
    s->makeReady(s->run->task, *s->runningTask(), s->tickIndex + 1);
    return s->yieldTask(L, YieldReason::TaskYield, ResumeKind::TaskYield);
}

int l_checkpoint(lua_State* L) {
    VmState* s = stateOf(L);
    RunContext* r = s->run;
    // Below the soft budget, or where yielding is impossible, the checkpoint returns false at once.
    if (!r || !r->overBudget || !s->requireYieldableTask(L, "task.checkpoint()", false)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    s->makeReady(r->task, *s->runningTask(), s->tickIndex + 1);
    return s->yieldTask(L, YieldReason::Checkpoint, ResumeKind::Checkpoint);
}

int l_spawn(lua_State* L) {
    VmState* s = stateOf(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    RunContext* r = s->run;
    if (!r || r->module >= s->modules.size())
        s->raiseTyped(L, ScriptErrorCode::InvalidState, "task.spawn outside a run");
    const int n = lua_gettop(L); // function + arguments
    lua_State* co = lua_newthread(L);
    lua_setthreaddata(co, &s->taskThreadMarker);
    // The task belongs to the spawning task's module: its thread must carry that module's memory
    // category (it would otherwise inherit whatever category is active, e.g. a required module's,
    // and the module heap cap would be checked against the wrong category).
    lua_setmemcat(co, s->modules[r->module]->memcat);
    lua_insert(L, 1); // [co, fn, args...]
    if (!lua_checkstack(co, n)) luaL_error(L, "task.spawn: too many arguments");
    lua_xmove(L, co, n);
    const int ref = lua_ref(L, 1);
    const TaskHandle h = s->createTask(co, ref, r->module, r->owner, n - 1);
    s->makeReady(h, *s->taskOf(h), s->tickIndex + 1);
    ++s->stats.tasksSpawned;
    lua_settop(L, 0);
    pushTaskHandle(L, h);
    return 1;
}

TaskHandle checkTask(lua_State* L, int idx) {
    const void* p = lua_touserdatatagged(L, idx, kTagTask);
    if (!p) luaL_typeerrorL(L, idx, "Task");
    u64 bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    return TaskHandle::fromBits(bits);
}

int l_cancel(lua_State* L) {
    VmState* s = stateOf(L);
    const TaskHandle h = checkTask(L, 1);
    lua_pushboolean(L, s->cancelTaskInternal(h));
    return 1;
}

int l_status(lua_State* L) {
    VmState* s = stateOf(L);
    const Task* t = s->taskOf(checkTask(L, 1));
    const std::string_view name = taskStateName(t ? t->state : TaskState::Gone);
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

int l_current(lua_State* L) {
    VmState* s = stateOf(L);
    if (s->runningTask()) {
        pushTaskHandle(L, s->run->task);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int l_now(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(stateOf(L)->now) * 1e-9);
    return 1;
}

int l_fuel(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(currentFuel(L)));
    return 1;
}

int taskToString(lua_State* L) {
    u64 bits = 0;
    if (const void* p = lua_touserdatatagged(L, 1, kTagTask)) std::memcpy(&bits, p, sizeof(bits));
    const std::string text = std::format("Task({}:{})", bits & 0xFFFFFFFFu, bits >> 32);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int taskEq(lua_State* L) {
    const void* a = lua_touserdatatagged(L, 1, kTagTask);
    const void* b = lua_touserdatatagged(L, 2, kTagTask);
    lua_pushboolean(L, a && b && std::memcmp(a, b, sizeof(u64)) == 0);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// WorldPos: frame-local f64 position userdata (never a float vector; ADR-005)
// ---------------------------------------------------------------------------------------------
u32 checkFrameId(lua_State* L, int idx) {
    const double f = luaL_optnumber(L, idx, 0.0);
    if (!(f >= 0.0 && f <= 4294967294.0) || f != std::floor(f))
        luaL_argerrorL(L, idx, "frame id must be an integer in [0, 2^32-2]");
    return static_cast<u32>(f);
}

void sameFrame(lua_State* L, const FramePos& a, const FramePos& b) {
    if (a.frame != b.frame) {
        luaL_error(L, "WorldPos frames differ (%u vs %u): convert through the frame graph first",
                   a.frame.value, b.frame.value);
    }
}

int l_worldPosNew(lua_State* L) {
    FramePos p;
    p.local = DVec3(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    p.frame = FrameId(checkFrameId(L, 4));
    pushWorldPos(L, p);
    return 1;
}

int wpDistance(lua_State* L) {
    chargeFuel(L, 1);
    const FramePos a = checkWorldPos(L, 1);
    const FramePos b = checkWorldPos(L, 2);
    sameFrame(L, a, b);
    lua_pushnumber(L, length(b.local - a.local));
    return 1;
}

int wpLength(lua_State* L) {
    chargeFuel(L, 1);
    lua_pushnumber(L, length(checkWorldPos(L, 1).local));
    return 1;
}

int wpOffset(lua_State* L) {
    chargeFuel(L, 1);
    FramePos p = checkWorldPos(L, 1);
    p.local = p.local + DVec3(luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4));
    pushWorldPos(L, p);
    return 1;
}

int wpLerp(lua_State* L) {
    chargeFuel(L, 1);
    const FramePos a = checkWorldPos(L, 1);
    const FramePos b = checkWorldPos(L, 2);
    sameFrame(L, a, b);
    const double t = luaL_checknumber(L, 3);
    FramePos r = a;
    r.local = a.local + (b.local - a.local) * t;
    pushWorldPos(L, r);
    return 1;
}

int wpIndex(lua_State* L) {
    const FramePos p = checkWorldPos(L, 1);
    usize len = 0;
    const char* key = lua_tolstring(L, 2, &len);
    if (key && len == 1) {
        switch (key[0]) {
        case 'x': lua_pushnumber(L, p.local.x); return 1;
        case 'y': lua_pushnumber(L, p.local.y); return 1;
        case 'z': lua_pushnumber(L, p.local.z); return 1;
        default: break;
        }
    }
    if (key) {
        const std::string_view k(key, len);
        if (k == "frame") {
            lua_pushnumber(L, static_cast<double>(p.frame.value));
            return 1;
        }
        lua_pushvalue(L, 2);
        lua_rawget(L, lua_upvalueindex(1)); // methods
        if (!lua_isnil(L, -1)) return 1;
    }
    luaL_error(L, "'%s' is not a member of WorldPos", key ? key : luaL_typename(L, 2));
}

int wpAdd(lua_State* L) {
    chargeFuel(L, 1);
    const FramePos a = checkWorldPos(L, 1);
    const FramePos b = checkWorldPos(L, 2);
    sameFrame(L, a, b);
    FramePos r = a;
    r.local = a.local + b.local;
    pushWorldPos(L, r);
    return 1;
}

int wpSub(lua_State* L) {
    chargeFuel(L, 1);
    const FramePos a = checkWorldPos(L, 1);
    const FramePos b = checkWorldPos(L, 2);
    sameFrame(L, a, b);
    FramePos r = a;
    r.local = a.local - b.local;
    pushWorldPos(L, r);
    return 1;
}

int wpMul(lua_State* L) {
    chargeFuel(L, 1);
    const bool posFirst = toWorldPos(L, 1) != nullptr;
    const FramePos p = checkWorldPos(L, posFirst ? 1 : 2);
    const double k = luaL_checknumber(L, posFirst ? 2 : 1);
    FramePos r = p;
    r.local = p.local * k;
    pushWorldPos(L, r);
    return 1;
}

int wpDiv(lua_State* L) {
    chargeFuel(L, 1);
    FramePos p = checkWorldPos(L, 1);
    p.local = p.local / luaL_checknumber(L, 2);
    pushWorldPos(L, p);
    return 1;
}

int wpUnm(lua_State* L) {
    chargeFuel(L, 1);
    FramePos p = checkWorldPos(L, 1);
    p.local = -p.local;
    pushWorldPos(L, p);
    return 1;
}

int wpEq(lua_State* L) {
    const FramePos* a = toWorldPos(L, 1);
    const FramePos* b = toWorldPos(L, 2);
    lua_pushboolean(L, a && b && *a == *b);
    return 1;
}

int wpToString(lua_State* L) {
    const FramePos p = checkWorldPos(L, 1);
    const std::string text =
        std::format("WorldPos({}, {}, {} @{})", p.local.x, p.local.y, p.local.z, p.frame.value);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// ---------------------------------------------------------------------------------------------
// math.random: deterministic seeded stream (Luau's default is seeded from time and addresses)
// ---------------------------------------------------------------------------------------------
int l_random(lua_State* L) {
    VmState* s = stateOf(L);
    switch (lua_gettop(L)) {
    case 0: lua_pushnumber(L, s->rng.nextDouble()); return 1;
    case 1: {
        const int u = luaL_checkinteger(L, 1);
        luaL_argcheck(L, 1 <= u, 1, "interval is empty");
        lua_pushnumber(L, static_cast<double>(s->rng.range<i64>(1, u)));
        return 1;
    }
    case 2: {
        const int lo = luaL_checkinteger(L, 1);
        const int hi = luaL_checkinteger(L, 2);
        luaL_argcheck(L, lo <= hi, 2, "interval is empty");
        lua_pushnumber(L, static_cast<double>(s->rng.range<i64>(lo, hi)));
        return 1;
    }
    default: luaL_error(L, "wrong number of arguments");
    }
}

// ---------------------------------------------------------------------------------------------
// coroutine guards
// ---------------------------------------------------------------------------------------------
int callOriginal(lua_State* L) {
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

int l_coGuarded(lua_State* L) {
    VmState* s = stateOf(L);
    if (lua_State* co = lua_tothread(L, 1); co && lua_getthreaddata(co) == &s->taskThreadMarker) {
        s->raiseTyped(
            L, ScriptErrorCode::InvalidState,
            "task coroutines are driven by the scheduler; use task.cancel instead of coroutine.resume/close");
    }
    return callOriginal(L);
}

int l_coResume(lua_State* L) {
    VmState* s = stateOf(L);
    if (lua_State* co = lua_tothread(L, 1)) {
        if (lua_getthreaddata(co) == &s->taskThreadMarker) {
            s->raiseTyped(L, ScriptErrorCode::InvalidState,
                          "task coroutines are driven by the scheduler; use task.cancel instead of "
                          "coroutine.resume/close");
        }
        // A nested coroutine allocates on behalf of whoever resumes it: keeps Luau's per-category
        // accounting equal to the category the allocator checks against the module cap
        // (VmState::activeMemcat), even for coroutines created in another module's context.
        lua_setmemcat(co, s->activeMemcat);
    }
    return callOriginal(L);
}

int l_coYield(lua_State* L) {
    VmState* s = stateOf(L);
    const int n = lua_gettop(L);
    if (s->run && L == s->run->thread && lua_isyieldable(L)) {
        // Yield of a task's own coroutine to the scheduler: explicit, treated like task.yield().
        if (Task* task = s->runningTask()) task->resumeKind = ResumeKind::None;
        s->run->yield = YieldReason::Coroutine;
        ++s->stats.yieldsByReason[static_cast<usize>(YieldReason::Coroutine)];
    }
    return lua_yield(L, n);
}

// ---------------------------------------------------------------------------------------------
// setmetatable (cells reject weak tables) and fuel-charging builtin wrappers
// ---------------------------------------------------------------------------------------------
int l_setmetatable(lua_State* L) {
    VmState* s = stateOf(L);
    if (s->deterministic && lua_istable(L, 2)) {
        lua_rawgetfield(L, 2, "__mode");
        const bool weak = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (weak) {
            s->raiseTyped(L, ScriptErrorCode::Runtime,
                          "weak tables (__mode) are not allowed on cells: GC timing must stay unobservable");
        }
    }
    return callOriginal(L);
}

u64 saturatingMul(u64 a, u64 b) noexcept { return (a != 0 && b > ~u64(0) / a) ? ~u64(0) : a * b; }

u64 argLength(lua_State* L, int idx) noexcept {
    const int t = lua_type(L, idx);
    if (t == LUA_TSTRING || t == LUA_TTABLE) return static_cast<u64>(lua_objlen(L, idx));
    if (t == LUA_TBUFFER) {
        usize len = 0;
        lua_tobuffer(L, idx, &len);
        return len;
    }
    return 0;
}

u64 argCount(lua_State* L, int idx) noexcept {
    if (lua_type(L, idx) != LUA_TNUMBER) return 0;
    const double n = lua_tonumber(L, idx);
    if (!(n > 0.0)) return 0;
    return n < 9007199254740992.0 ? static_cast<u64>(n) : 9007199254740992ull;
}

u64 itemsArg1(lua_State* L) noexcept { return argLength(L, 1); }
u64 itemsRep(lua_State* L) noexcept { return saturatingMul(argLength(L, 1), argCount(L, 2)); }
u64 itemsCreate(lua_State* L) noexcept { return argCount(L, 1); }
u64 itemsFormat(lua_State* L) noexcept {
    u64 total = 0;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
        if (lua_type(L, i) == LUA_TSTRING)
            total += static_cast<u64>(lua_objlen(L, i)); // never converts numbers
    }
    return total;
}
u64 itemsSort(lua_State* L) noexcept {
    // Charged n * ceil(log2 n) before any comparator runs (02 §7.4).
    const u64 n = argLength(L, 1);
    if (n < 2) return 0;
    const u64 lg = 64u - static_cast<u64>(std::countl_zero(n - 1));
    return saturatingMul(n, lg);
}
u64 itemsMove(lua_State* L) noexcept {
    if (lua_type(L, 2) != LUA_TNUMBER || lua_type(L, 3) != LUA_TNUMBER) return 0;
    const double f = lua_tonumber(L, 2);
    const double e = lua_tonumber(L, 3);
    return e >= f && e - f < 9007199254740992.0 ? static_cast<u64>(e - f) + 1 : 0;
}
u64 itemsBufferFill(lua_State* L) noexcept {
    if (lua_type(L, 4) == LUA_TNUMBER) return argCount(L, 4);
    const u64 len = argLength(L, 1);
    const u64 off = argCount(L, 2);
    return len > off ? len - off : 0;
}
u64 itemsBufferCopy(lua_State* L) noexcept {
    if (lua_type(L, 5) == LUA_TNUMBER) return argCount(L, 5);
    const u64 len = argLength(L, 3);
    const u64 off = argCount(L, 4);
    return len > off ? len - off : 0;
}

u64 itemsNone(lua_State*) noexcept { return 0; }
// table.insert(t, pos, v) shifts t[pos..#t] up; the two-argument append is O(1).
u64 itemsInsert(lua_State* L) noexcept {
    if (lua_gettop(L) != 3 || lua_type(L, 1) != LUA_TTABLE || lua_type(L, 2) != LUA_TNUMBER) return 0;
    const double n = static_cast<double>(lua_objlen(L, 1));
    const double pos = lua_tonumber(L, 2);
    return pos >= 1.0 && pos <= n ? static_cast<u64>(n - pos) + 1 : 0;
}
// table.remove(t, pos) shifts t[pos+1..#t] down; pos defaults to #t (O(1)).
u64 itemsRemove(lua_State* L) noexcept {
    if (lua_type(L, 1) != LUA_TTABLE) return 0;
    const double n = static_cast<double>(lua_objlen(L, 1));
    const double pos = lua_type(L, 2) == LUA_TNUMBER ? lua_tonumber(L, 2) : n;
    return pos >= 1.0 && pos <= n ? static_cast<u64>(n - pos) + 1 : 0;
}
u64 itemsBufferCreate(lua_State* L) noexcept { return argCount(L, 1); }
u64 itemsWriteString(lua_State* L) noexcept {
    return lua_type(L, 4) == LUA_TNUMBER ? argCount(L, 4) : argLength(L, 3);
}

/// Bytes of the string and buffer values in [first, last] (results of a pure builtin).
u64 resultBytes(lua_State* L, int first, int last) noexcept {
    u64 total = 0;
    for (int i = first; i <= last; ++i) {
        const int t = lua_type(L, i);
        if (t == LUA_TSTRING || t == LUA_TBUFFER) total += argLength(L, i);
    }
    return total;
}

struct BuiltinWrap {
    const char* library;
    const char* name;
    FuelCost cost; ///< Taken before the call; itemsArg unused: `items` computes the count.
    u64 (*items)(lua_State*) noexcept;
    bool patternSubject; ///< cap the subject at 64 KiB on cells
    /// Fuel per 1000 bytes of string/buffer results, charged right after the call. Only for pure
    /// builtins (02 §7.4 `of=result`): the call has no side effect, so a kill there is as clean as
    /// one before it, and the charge depends only on arguments and results (deterministic).
    u32 resultMilli;
};

// Placeholder costs until `--calibrate-fuel` (WP-1.6) measures them; perItemMilli is fuel per
// 1000 items (bytes or elements). Beyond 02 §7.4's list, every other builtin whose C work grows with
// its input or output is charged too (string.sub/upper/lower/reverse/pack/unpack, positional
// table.insert/remove, table.clear/maxn, buffer.create/fromstring/tostring/readstring/writestring,
// utf8.len/offset), and string.format/gsub and table.concat also pay for the bytes they produce:
// an uncharged O(n) builtin in a loop would otherwise run far past fuel_kill in wall time.
constexpr BuiltinWrap kBuiltinWraps[] = {
    {"string", "rep", {1, 16, 0}, &itemsRep, false, 0},
    // No result charge: `%s` of a table or userdata embeds a heap address whose printed length
    // differs between runs and platforms, so fuel would stop being a function of the inputs. The
    // pre-charge covers every string argument, including the format itself.
    {"string", "format", {1, 63, 0}, &itemsFormat, false, 0},
    {"string", "gsub", {2, 16, 0}, &itemsArg1, true, 16},
    {"string", "find", {1, 16, 0}, &itemsArg1, true, 0},
    {"string", "match", {1, 16, 0}, &itemsArg1, true, 0},
    {"string", "gmatch", {1, 16, 0}, &itemsArg1, true, 0},
    {"string", "split", {1, 32, 0}, &itemsArg1, false, 0},
    {"string", "sub", {1, 0, 0}, &itemsNone, false, 16},
    {"string", "upper", {1, 16, 0}, &itemsArg1, false, 0},
    {"string", "lower", {1, 16, 0}, &itemsArg1, false, 0},
    {"string", "reverse", {1, 16, 0}, &itemsArg1, false, 0},
    {"string", "pack", {1, 0, 0}, &itemsNone, false, 16},
    {"string", "unpack", {1, 0, 0}, &itemsNone, false, 16},
    {"table", "concat", {1, 250, 0}, &itemsArg1, false, 16},
    {"table", "sort", {1, 500, 0}, &itemsSort, false, 0},
    {"table", "move", {1, 125, 0}, &itemsMove, false, 0},
    {"table", "create", {1, 63, 0}, &itemsCreate, false, 0},
    {"table", "clone", {1, 125, 0}, &itemsArg1, false, 0},
    {"table", "find", {1, 250, 0}, &itemsArg1, false, 0},
    {"table", "insert", {1, 125, 0}, &itemsInsert, false, 0},
    {"table", "remove", {1, 125, 0}, &itemsRemove, false, 0},
    {"table", "clear", {1, 63, 0}, &itemsArg1, false, 0},
    {"table", "maxn", {1, 63, 0}, &itemsArg1, false, 0},
    {"buffer", "fill", {1, 4, 0}, &itemsBufferFill, false, 0},
    {"buffer", "copy", {1, 4, 0}, &itemsBufferCopy, false, 0},
    {"buffer", "create", {1, 4, 0}, &itemsBufferCreate, false, 0},
    {"buffer", "fromstring", {1, 16, 0}, &itemsArg1, false, 0},
    {"buffer", "tostring", {1, 0, 0}, &itemsNone, false, 16},
    {"buffer", "readstring", {1, 0, 0}, &itemsNone, false, 16},
    {"buffer", "writestring", {1, 16, 0}, &itemsWriteString, false, 0},
    {"utf8", "len", {1, 16, 0}, &itemsArg1, false, 0},
    {"utf8", "offset", {1, 16, 0}, &itemsArg1, false, 0},
};

int builtinWrapper(lua_State* L) {
    const auto* w = static_cast<const BuiltinWrap*>(lua_tolightuserdata(L, lua_upvalueindex(2)));
    VmState* s = stateOf(L);
    if (w->patternSubject && s->deterministic && lua_type(L, 1) == LUA_TSTRING &&
        static_cast<usize>(lua_objlen(L, 1)) > kCellPatternSubjectLimit) {
        s->raiseTyped(L, ScriptErrorCode::Runtime,
                      std::format("string.{}: subject exceeds 64 KiB on cells", w->name));
    }
    if (RunContext* r = s->run) s->chargeRun(L, *r, w->cost.charge(w->items(L)));
    const int n = callOriginal(L);
    if (w->resultMilli != 0) {
        if (RunContext* r = s->run) {
            s->chargeRun(L, *r, FuelCost{0, w->resultMilli, 0}.charge(resultBytes(L, 1, n)));
        }
    }
    return n;
}

// Replaces lib.name with a C closure (upvalue 1 = original function, upvalue 2 = `extra`).
void wrapField(lua_State* L, const char* library, const char* name, lua_CFunction wrapper, void* extra) {
    if (library) {
        lua_getglobal(L, library);
    } else {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
    }
    lua_getfield(L, -1, name);
    HELIOS_ASSERT(lua_isfunction(L, -1), "builtin {}.{} missing", library ? library : "_G", name);
    if (extra) {
        lua_pushlightuserdata(L, extra);
        lua_pushcclosurek(L, wrapper, name, 2, nullptr);
    } else {
        lua_pushcclosurek(L, wrapper, name, 1, nullptr);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void setNil(lua_State* L, const char* library, const char* name) {
    if (library) {
        lua_getglobal(L, library);
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    } else {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
}

void addManifest(VmState& s, std::initializer_list<const char*> names) {
    for (const char* n : names) s.manifest.emplace_back(n);
}

} // namespace

void pushScriptError(lua_State* L, ScriptErrorCode code, KillReason reason, std::string_view message) {
    const ErrorHeader header{static_cast<u8>(code), static_cast<u8>(reason),
                             static_cast<u32>(message.size())};
    auto* p = static_cast<unsigned char*>(
        lua_newuserdatataggedwithmetatable(L, sizeof(ErrorHeader) + message.size(), kTagScriptError));
    std::memcpy(p, &header, sizeof(header));
    if (!message.empty()) std::memcpy(p + sizeof(header), message.data(), message.size());
}

bool readScriptError(lua_State* L, int idx, ScriptError& out) noexcept {
    const auto* p = static_cast<const unsigned char*>(lua_touserdatatagged(L, idx, kTagScriptError));
    if (!p) return false;
    ErrorHeader header;
    std::memcpy(&header, p, sizeof(header));
    out.code = static_cast<ScriptErrorCode>(header.code);
    out.killReason = static_cast<KillReason>(header.reason);
    out.message.assign(reinterpret_cast<const char*>(p + sizeof(header)), header.length);
    return true;
}

std::vector<std::string> sandboxWrappedBuiltins() {
    std::vector<std::string> out;
    for (const BuiltinWrap& w : kBuiltinWraps) out.push_back(std::format("{}.{}", w.library, w.name));
    out.emplace_back("setmetatable");
    out.emplace_back("math.random");
    return out;
}

void VmState::installStdlib() {
    lua_State* state = mainL;
    lua_checkstack(state, 8);

    // ScriptError: typed errors are immutable userdata that scripts cannot construct or forge.
    lua_createtable(state, 0, 4);
    lua_pushcfunction(state, &errorIndex, "__index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, &errorToString, "__tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushliteral(state, "ScriptError");
    lua_setfield(state, -2, "__type");
    lua_pushliteral(state, "ScriptError");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_setuserdatametatable(state, kTagScriptError);

    // Pre-built kill errors: raising a kill never allocates (it may happen at the heap cap).
    const struct {
        KillReason reason;
        const char* message;
    } kills[] = {
        {KillReason::Fuel, "script killed: fuel budget exhausted (fuel_kill)"},
        {KillReason::WallBudget, "script killed: wall-time budget exhausted"},
        {KillReason::WallBackstop, "script killed: wall-clock backstop (binding far over its fuel charge)"},
    };
    for (const auto& k : kills) {
        pushScriptError(state, ScriptErrorCode::Killed, k.reason, k.message);
        killErrorRefs[static_cast<usize>(k.reason)] = lua_ref(state, -1);
        lua_pop(state, 1);
    }

    // Sandbox removals (02 §7.4, 06 §11): no io/os/debug/loadstring, no environment tricks.
    for (const char* name : {"os", "debug", "io", "loadstring", "load", "dofile", "getfenv", "setfenv"}) {
        setNil(state, nullptr, name);
    }
    setNil(state, "math", "randomseed");
    if (deterministic) {
        // GC timing must stay unobservable on cells (04 §10.2).
        setNil(state, nullptr, "collectgarbage");
        setNil(state, nullptr, "gcinfo");
    }

    // print logs a line (~1 us): placeholder cost until calibration; its text is charged per byte.
    registerCoreFunction(*this, "", "print", &l_print, FuelCost{100, 0, 0}, false);
    registerCoreFunction(*this, "", "require", &l_require, FuelCost{5, 0, 0}, false);
    registerCoreFunction(*this, "", "wait", &l_wait, FuelCost{1, 0, 0}, true);
    registerCoreFunction(*this, "task", "wait", &l_wait, FuelCost{1, 0, 0}, true);
    registerCoreFunction(*this, "task", "yield", &l_taskYield, FuelCost{1, 0, 0}, true);
    registerCoreFunction(*this, "task", "checkpoint", &l_checkpoint, FuelCost{1, 0, 0}, true);
    registerCoreFunction(*this, "task", "spawn", &l_spawn, FuelCost{10, 0, 0}, false);
    registerCoreFunction(*this, "task", "cancel", &l_cancel, FuelCost{5, 0, 0}, false);
    registerCoreFunction(*this, "task", "status", &l_status, FuelCost{1, 0, 0}, false);
    registerCoreFunction(*this, "task", "current", &l_current, FuelCost{1, 0, 0}, false);
    registerCoreFunction(*this, "task", "now", &l_now, FuelCost{1, 0, 0}, false);
    registerCoreFunction(*this, "task", "fuel", &l_fuel, FuelCost{1, 0, 0}, false);
    registerCoreFunction(*this, "WorldPos", "new", &l_worldPosNew, FuelCost{1, 0, 0}, false);

    // Task userdata.
    lua_createtable(state, 0, 4);
    lua_pushcfunction(state, &taskToString, "__tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, &taskEq, "__eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "Task");
    lua_setfield(state, -2, "__type");
    lua_pushliteral(state, "Task");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_setuserdatametatable(state, kTagTask);

    // WorldPos userdata.
    lua_createtable(state, 0, 12);
    lua_createtable(state, 0, 4); // methods
    lua_pushcfunction(state, &wpDistance, "distance");
    lua_setfield(state, -2, "distance");
    lua_pushcfunction(state, &wpLength, "length");
    lua_setfield(state, -2, "length");
    lua_pushcfunction(state, &wpOffset, "offset");
    lua_setfield(state, -2, "offset");
    lua_pushcfunction(state, &wpLerp, "lerp");
    lua_setfield(state, -2, "lerp");
    lua_setreadonly(state, -1, true);
    lua_pushcclosurek(state, &wpIndex, "__index", 1, nullptr);
    lua_setfield(state, -2, "__index");
    const struct {
        const char* name;
        lua_CFunction fn;
    } wpMeta[] = {{"__add", &wpAdd}, {"__sub", &wpSub}, {"__mul", &wpMul},          {"__div", &wpDiv},
                  {"__unm", &wpUnm}, {"__eq", &wpEq},   {"__tostring", &wpToString}};
    for (const auto& mm : wpMeta) {
        lua_pushcfunction(state, mm.fn, mm.name);
        lua_setfield(state, -2, mm.name);
    }
    lua_pushliteral(state, "WorldPos");
    lua_setfield(state, -2, "__type");
    lua_pushliteral(state, "WorldPos");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_setuserdatametatable(state, kTagWorldPos);
    addManifest(*this, {"WorldPos", "WorldPos.x", "WorldPos.y", "WorldPos.z", "WorldPos.frame",
                        "WorldPos:distance", "WorldPos:length", "WorldPos:offset", "WorldPos:lerp", "Task",
                        "ScriptError", "ScriptError.code", "ScriptError.message", "math.random"});

    // Deterministic math.random (a VM-wide seeded stream; named streams arrive with Rand, 06 §11).
    lua_getglobal(state, "math");
    lua_pushcfunction(state, &l_random, "random");
    lua_setfield(state, -2, "random");
    lua_pop(state, 1);

    // Scripts must not drive task coroutines themselves.
    wrapField(state, "coroutine", "resume", &l_coResume, nullptr);
    wrapField(state, "coroutine", "close", &l_coGuarded, nullptr);
    lua_getglobal(state, "coroutine");
    lua_pushcfunction(state, &l_coYield, "yield");
    lua_setfield(state, -2, "yield");
    lua_pop(state, 1);
}

void VmState::installBuiltinWrappers() {
    for (const BuiltinWrap& w : kBuiltinWraps) {
        wrapField(mainL, w.library, w.name, &builtinWrapper, const_cast<BuiltinWrap*>(&w));
    }
    wrapField(mainL, nullptr, "setmetatable", &l_setmetatable, nullptr);
}

} // namespace helios::script::detail
