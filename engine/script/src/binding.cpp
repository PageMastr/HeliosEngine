// Binding helper layer: registration (Binder), the charging trampoline, object types over
// generational handles, WorldPos userdata access, typed errors, the C++→Luau callback wrapper and
// asynchronous host calls.

#include "helios/script/binding.h"

#include <bit>
#include <cstring>
#include <format>

#include "helios/core/assert.h"
#include "vm_state.h"

namespace helios::script {
namespace detail {

namespace {

// Stores the closure on top of the stack as `library.name` (library empty = global) and pops it.
void setLibraryField(lua_State* L, std::string_view library, std::string_view name) {
    const std::string field(name);
    if (library.empty()) {
        lua_setglobal(L, field.c_str());
        return;
    }
    const std::string lib(library);
    lua_getglobal(L, lib.c_str());
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 4);
        lua_pushvalue(L, -1);
        lua_setglobal(L, lib.c_str());
    }
    lua_insert(L, -2); // [library, closure]
    lua_setfield(L, -2, field.c_str());
    lua_pop(L, 1);
}

ObjectTypeInfo* typeInfo(VmState& s, int type) noexcept {
    const int index = type - kFirstObjectTag;
    if (index < 0 || index >= static_cast<int>(s.objectTypes.size())) return nullptr;
    return &s.objectTypes[static_cast<usize>(index)];
}

int objectIndex(lua_State* L) {
    auto* t = static_cast<ObjectTypeInfo*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    VmState* s = stateOf(L);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_getref(L, t->methodsRef);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 2);
        lua_getref(L, t->propertiesRef);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if (lua_islightuserdata(L, -1)) {
            auto* info = static_cast<BindingInfo*>(lua_tolightuserdata(L, -1));
            checkObject(L, 1, t->tag); // every dereference is checked: StaleHandle before the getter
            lua_settop(L, 1);
            return s->invokeBinding(L, *info);
        }
        luaL_error(L, "'%s' is not a member of %s", lua_tostring(L, 2), t->name.c_str());
    }
    luaL_error(L, "attempt to index %s with a %s key", t->name.c_str(), luaL_typename(L, 2));
}

int objectEq(lua_State* L) {
    const int tag = lua_userdatatag(L, 1);
    u64 a = 0;
    u64 b = 0;
    const bool same =
        tag == lua_userdatatag(L, 2) && toObjectHandle(L, 1, tag, &a) && toObjectHandle(L, 2, tag, &b);
    lua_pushboolean(L, same && a == b);
    return 1;
}

int objectToString(lua_State* L) {
    auto* t = static_cast<ObjectTypeInfo*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    u64 bits = 0;
    toObjectHandle(L, 1, t->tag, &bits);
    const std::string text = std::format("{}({}:{})", t->name, bits & 0xFFFFFFFFu, bits >> 32);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int objectIsValid(lua_State* L) {
    auto* t = static_cast<ObjectTypeInfo*>(bindingUserdata(L));
    u64 bits = 0;
    if (!toObjectHandle(L, 1, t->tag, &bits)) luaL_typeerrorL(L, 1, t->name.c_str());
    lua_pushboolean(L, t->resolve(t->context, bits) != nullptr);
    return 1;
}

} // namespace

BindingInfo& addBinding(VmState& s, std::string_view qualifiedName, lua_CFunction fn, FuelCost cost,
                        void* userdata, bool async) {
    BindingInfo& info = s.bindings.emplace_back();
    info.qualifiedName = std::string(qualifiedName);
    info.fn = fn;
    info.cost = cost;
    info.userdata = userdata;
    info.async = async;
    return info;
}

int bindingTrampoline(lua_State* L) {
    auto* info = static_cast<BindingInfo*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    return stateOf(L)->invokeBinding(L, *info);
}

void pushBindingClosure(lua_State* L, BindingInfo& info) {
    // Luau keeps the debug-name pointer: it points into the deque-owned name, stable for the VM's life.
    const usize dot = info.qualifiedName.find_last_of(".:");
    const char* debugName = info.qualifiedName.c_str() + (dot == std::string::npos ? 0 : dot + 1);
    lua_pushlightuserdata(L, &info);
    // Only async bindings get a continuation: a C function with a continuation makes the Luau calls
    // it performs yieldable, which ordinary bindings (using callLuau) must never be.
    lua_pushcclosurek(L, &bindingTrampoline, debugName, 1, info.async ? &taskContinuation : nullptr);
}

void registerFunction(VmState& s, std::string_view library, std::string_view name, lua_CFunction fn,
                      FuelCost cost, void* userdata, bool async) {
    HELIOS_ASSERT(fn != nullptr && !name.empty(), "binding needs a name and a function");
    const std::string qualified = library.empty() ? std::string(name) : std::format("{}.{}", library, name);
    BindingInfo& info = addBinding(s, qualified, fn, cost, userdata, async);
    pushBindingClosure(s.mainL, info);
    setLibraryField(s.mainL, library, name);
    if (s.registeringCore) s.manifest.push_back(qualified);
}

void registerCoreFunction(VmState& s, std::string_view library, std::string_view name, lua_CFunction fn,
                          FuelCost cost, bool async) {
    s.registeringCore = true;
    registerFunction(s, library, name, fn, cost, nullptr, async);
    s.registeringCore = false;
}

u64 itemsOfArg(lua_State* L, int idx) noexcept {
    switch (lua_type(L, idx)) {
    case LUA_TTABLE:
    case LUA_TSTRING: return static_cast<u64>(lua_objlen(L, idx));
    case LUA_TBUFFER: {
        usize len = 0;
        lua_tobuffer(L, idx, &len);
        return len;
    }
    case LUA_TNUMBER: {
        const double n = lua_tonumber(L, idx);
        if (!(n > 0.0)) return 0;
        return n < 9007199254740992.0 ? static_cast<u64>(n) : 9007199254740992ull;
    }
    default: return 0;
    }
}

void pushTaskHandle(lua_State* L, TaskHandle h) {
    const u64 bits = h.toBits();
    void* p = lua_newuserdatataggedwithmetatable(L, sizeof(u64), kTagTask);
    std::memcpy(p, &bits, sizeof(bits));
}

} // namespace detail

using detail::stateOf;
using detail::VmState;

// ---------------------------------------------------------------------------------------------
// Binder
// ---------------------------------------------------------------------------------------------
lua_State* Binder::state() const noexcept { return m_state->mainL; }

Binder& Binder::function(std::string_view library, std::string_view name, lua_CFunction fn, FuelCost cost,
                         void* userdata) {
    detail::registerFunction(*m_state, library, name, fn, cost, userdata, false);
    return *this;
}

Binder& Binder::asyncFunction(std::string_view library, std::string_view name, lua_CFunction fn,
                              FuelCost cost, void* userdata) {
    detail::registerFunction(*m_state, library, name, fn, cost, userdata, true);
    return *this;
}

ObjectType Binder::objectType(std::string_view name, ResolveFn resolve, void* context) {
    VmState& s = *m_state;
    const int tag = detail::kFirstObjectTag + static_cast<int>(s.objectTypes.size());
    if (tag > detail::kLastObjectTag || resolve == nullptr || name.empty()) return -1;
    lua_State* L = s.mainL;
    detail::ObjectTypeInfo& t = s.objectTypes.emplace_back();
    t.name = std::string(name);
    t.resolve = resolve;
    t.context = context;
    t.tag = tag;
    lua_createtable(L, 0, 8);
    t.methodsRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_createtable(L, 0, 8);
    t.propertiesRef = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_createtable(L, 0, 6);
    lua_pushlightuserdata(L, &t);
    lua_pushcclosurek(L, &detail::objectIndex, "__index", 1, nullptr);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &detail::objectEq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushlightuserdata(L, &t);
    lua_pushcclosurek(L, &detail::objectToString, "__tostring", 1, nullptr);
    lua_setfield(L, -2, "__tostring");
    lua_pushlstring(L, t.name.data(), t.name.size());
    lua_setfield(L, -2, "__type");
    lua_pushlstring(L, t.name.data(), t.name.size());
    lua_setfield(L, -2, "__metatable"); // getmetatable() returns the name; the metatable stays private
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, tag);

    // Revalidation after a yield (06 §11 rule 6) must not raise: obj:isValid().
    method(tag, "isValid", &detail::objectIsValid, FuelCost{1, 0, 0}, &t);
    return tag;
}

Binder& Binder::method(ObjectType type, std::string_view name, lua_CFunction fn, FuelCost cost,
                       void* userdata) {
    VmState& s = *m_state;
    detail::ObjectTypeInfo* t = detail::typeInfo(s, type);
    HELIOS_ASSERT(t != nullptr && fn != nullptr, "unknown object type {}", type);
    if (!t || !fn) return *this;
    detail::BindingInfo& info =
        detail::addBinding(s, std::format("{}:{}", t->name, name), fn, cost, userdata, false);
    lua_State* L = s.mainL;
    lua_getref(L, t->methodsRef);
    detail::pushBindingClosure(L, info);
    const std::string field(name);
    lua_setfield(L, -2, field.c_str());
    lua_pop(L, 1);
    return *this;
}

Binder& Binder::property(ObjectType type, std::string_view name, lua_CFunction getter, FuelCost cost,
                         void* userdata) {
    VmState& s = *m_state;
    detail::ObjectTypeInfo* t = detail::typeInfo(s, type);
    HELIOS_ASSERT(t != nullptr && getter != nullptr, "unknown object type {}", type);
    if (!t || !getter) return *this;
    detail::BindingInfo& info =
        detail::addBinding(s, std::format("{}.{}", t->name, name), getter, cost, userdata, false);
    info.objectType = type;
    lua_State* L = s.mainL;
    lua_getref(L, t->propertiesRef);
    lua_pushlightuserdata(L, &info);
    const std::string field(name);
    lua_setfield(L, -2, field.c_str());
    lua_pop(L, 1);
    return *this;
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
ScriptVm& vmFromState(lua_State* L) noexcept { return *stateOf(L)->scriptVm; }

void* bindingUserdata(lua_State* L) noexcept {
    const detail::BindingInfo* info = stateOf(L)->currentBinding;
    return info ? info->userdata : nullptr;
}

void chargeFuel(lua_State* L, u64 fuel) {
    VmState* s = stateOf(L);
    if (s->run) s->chargeRun(L, *s->run, fuel);
}

u64 currentFuel(lua_State* L) noexcept {
    const VmState* s = stateOf(L);
    return s->run ? s->run->fuel : 0;
}

TaskId currentTask(lua_State* L) noexcept {
    const VmState* s = stateOf(L);
    return s->run ? TaskId{s->run->task.toBits()} : TaskId{};
}

void raiseError(lua_State* L, ScriptErrorCode code, std::string_view message) {
    stateOf(L)->raiseTyped(L, code, message);
}

void pushObject(lua_State* L, ObjectType type, u64 handleBits) {
    if (!detail::typeInfo(*stateOf(L), type)) luaL_error(L, "pushObject: unknown object type %d", type);
    void* p = lua_newuserdatataggedwithmetatable(L, sizeof(u64), type);
    std::memcpy(p, &handleBits, sizeof(handleBits));
}

void* checkObject(lua_State* L, int idx, ObjectType type) {
    VmState* s = stateOf(L);
    detail::ObjectTypeInfo* t = detail::typeInfo(*s, type);
    if (!t) luaL_error(L, "checkObject: unknown object type %d", type);
    const void* p = lua_touserdatatagged(L, idx, type);
    if (!p) luaL_typeerrorL(L, idx, t->name.c_str());
    u64 bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    void* object = t->resolve(t->context, bits);
    if (!object) {
        s->raiseTyped(L, ScriptErrorCode::StaleHandle,
                      std::format("{} handle {}:{} is stale (the object was destroyed or its slot recycled)",
                                  t->name, bits & 0xFFFFFFFFu, bits >> 32));
    }
    return object;
}

bool toObjectHandle(lua_State* L, int idx, ObjectType type, u64* outBits) noexcept {
    if (type < detail::kFirstObjectTag || type > detail::kLastObjectTag) return false;
    const void* p = lua_touserdatatagged(L, idx, type);
    if (!p) return false;
    if (outBits) std::memcpy(outBits, p, sizeof(u64));
    return true;
}

void pushWorldPos(lua_State* L, const FramePos& pos) {
    void* p = lua_newuserdatataggedwithmetatable(L, sizeof(FramePos), detail::kTagWorldPos);
    std::memcpy(p, &pos, sizeof(pos));
}

const FramePos* toWorldPos(lua_State* L, int idx) noexcept {
    return static_cast<const FramePos*>(lua_touserdatatagged(L, idx, detail::kTagWorldPos));
}

FramePos checkWorldPos(lua_State* L, int idx) {
    const FramePos* p = toWorldPos(L, idx);
    if (!p) luaL_typeerrorL(L, idx, "WorldPos");
    FramePos out;
    std::memcpy(&out, p, sizeof(out));
    return out;
}

namespace {
// Calls the function below the arguments from a C frame WITHOUT a continuation. Luau makes a call
// yieldable when the calling C function has a continuation (luaD_call bumps baseCcalls), and every
// async binding has one; a direct lua_pcall from such a binding would let the callee's wait() yield
// the whole task through the binding's C++ frame, which then keeps running and returns twice. This
// frame keeps the callee non-yieldable, so wait()/async calls raise InvalidState there instead.
int callLuauTrampoline(lua_State* L) {
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}
} // namespace

int callLuau(lua_State* L, int nargs, int nresults) {
    VmState* s = stateOf(L);
    int status = LUA_ERRMEM;
    if (detail::ensureStack(L, 1)) {
        lua_pushcfunction(L, &callLuauTrampoline, "callLuau");
        lua_insert(L, -(nargs + 2)); // [trampoline, fn, args...]
        status = lua_pcall(L, nargs + 1, nresults, 0);
    } else {
        lua_pop(L, nargs + 1);
        lua_pushliteral(L, "callLuau: stack overflow"); // uses the frame's LUA_MINSTACK reserve
        status = LUA_ERRRUN;
    }
    // The callee has fully unwound; a kill must not be swallowed by the binding's error handling.
    if (s->run && s->run->killed) s->raiseKill(L, *s->run);
    return status;
}

AsyncToken beginAsync(lua_State* L) {
    VmState* s = stateOf(L);
    s->requireYieldableTask(L, "an async host call", true);
    detail::Task* task = s->runningTask();
    if (task->awaiting.isValid()) s->asyncOps.destroy(task->awaiting); // abandoned earlier attempt
    const detail::AsyncHandle h = s->asyncOps.create();
    s->asyncOps.get(h)->task = s->run->task;
    task->awaiting = h;
    return AsyncToken{h.toBits()};
}

int yieldAsync(lua_State* L) {
    VmState* s = stateOf(L);
    detail::Task* task = s->runningTask();
    if (!task || !task->awaiting.isValid() || L != s->run->thread) {
        s->raiseTyped(L, ScriptErrorCode::InvalidState, "yieldAsync() without a matching beginAsync()");
    }
    const detail::AsyncOp* op = s->asyncOps.get(task->awaiting);
    if (!op) {
        // Never suspend on a token nobody can complete any more (the task would await forever).
        s->raiseTyped(L, ScriptErrorCode::InvalidState,
                      "yieldAsync(): the async call's token is no longer valid");
    }
    if (op->completed) {
        // Completed synchronously (cache hit, immediate failure) before this yield: still an
        // explicit yield, resumed on the next tick with the stored results.
        s->makeReady(s->run->task, *task, s->tickIndex + 1);
    } else {
        task->state = TaskState::Awaiting;
    }
    return s->yieldTask(L, YieldReason::Async, detail::ResumeKind::Async);
}

} // namespace helios::script
