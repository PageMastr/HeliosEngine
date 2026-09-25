#pragma once
// Binding helper layer (hand-written Phase 0 glue; WP-1.6 generates the registration code from
// `@script` schema functions but keeps these runtime helpers).
//
// * Binder registers Luau-callable C++ functions, yielding (async) functions and object types
//   while the VM is being created — before luaL_sandbox freezes the globals. Every registered
//   function runs behind a charging trampoline that takes its declared FuelCost first, so no C++
//   path reachable from Luau is uncharged (02 §7.4).
// * Object types expose host objects as tagged userdata holding a 64-bit generational handle.
//   Every dereference resolves the handle; a stale one raises a typed `StaleHandle` error instead
//   of touching a destroyed or recycled object (06 §11 rule 6).
// * WorldPos is a double-precision userdata (frame id + f64 xyz), never a float `vector` (ADR-005).
// * callLuau() is the protected C++→Luau call wrapper for bindings that call back into Luau: an
//   ordinary error is returned to the binding, a kill is re-raised after the pcall so binding
//   frames unwind through RAII and the kill stays sticky.
// * beginAsync()/yieldAsync() turn a binding into an asynchronous host call: the task yields
//   (an explicit, visible yield) and resumes when the host calls ScriptVm::completeAsync.
//
// Threading: every function here runs on the thread that currently owns the VM (see vm.h). All
// helpers that take a lua_State may raise Luau errors (C++ exceptions thrown by Luau, since Helios
// builds Luau with LUA_USE_LONGJMP=0); call them only from bindings, never from engine code
// outside a Luau call.

#include <string_view>

#include "lua.h"
#include "lualib.h"

#include "helios/core/handle.h"
#include "helios/math/frame.h"
#include "helios/script/types.h"

namespace helios::script {

class ScriptVm;
namespace detail {
struct VmState;
}

/// Object type id returned by Binder::objectType (it is the Luau userdata tag of the type).
using ObjectType = int;

/// Resolves a generational handle to a live host object, or nullptr when the handle is stale.
/// Called on every dereference, so it must be cheap (the plan budgets ≤ 20 ns for entities).
using ResolveFn = void* (*)(void* context, u64 handleBits);

/// Registers the Helios Luau API of a VM. Only valid inside the ScriptVm::create() registrar.
class Binder {
public:
    lua_State* state() const noexcept;

    /// Registers a global function (`library` empty) or `library.name` (the library table is
    /// created on first use and frozen by the sandbox). `userdata` is returned by
    /// bindingUserdata() while the function runs.
    Binder& function(std::string_view library, std::string_view name, lua_CFunction fn, FuelCost cost = {},
                     void* userdata = nullptr);

    /// Registers a yielding binding for an asynchronous host call. `fn` validates its arguments,
    /// calls beginAsync(), hands the token to the host operation and ends with
    /// `return yieldAsync(L);`. The values later passed to ScriptVm::completeAsync become the call's
    /// results; ScriptVm::failAsync raises a `HostError` in the task. Async bindings must not call
    /// back into Luau.
    Binder& asyncFunction(std::string_view library, std::string_view name, lua_CFunction fn,
                          FuelCost cost = {}, void* userdata = nullptr);

    /// Declares an object type exposed as tagged userdata over generational handles. Returns its
    /// id, or -1 when the 96 object-type tags are exhausted.
    ObjectType objectType(std::string_view name, ResolveFn resolve, void* context);

    /// Object type over a core HandlePool (the pool must outlive the VM).
    template <class T, class Tag>
    ObjectType poolType(std::string_view name, HandlePool<T, Tag>& pool) {
        return objectType(
            name,
            [](void* context, u64 bits) -> void* {
                return static_cast<HandlePool<T, Tag>*>(context)->get(Handle<Tag>::fromBits(bits));
            },
            &pool);
    }

    /// Adds `obj:name(...)`; `fn` receives the userdata at index 1 (use checkObject).
    Binder& method(ObjectType type, std::string_view name, lua_CFunction fn, FuelCost cost = {},
                   void* userdata = nullptr);
    /// Adds a read-only property `obj.name`; the handle is resolved (StaleHandle on failure) before
    /// `getter` runs with the userdata at index 1. The getter pushes one value and returns 1.
    Binder& property(ObjectType type, std::string_view name, lua_CFunction getter, FuelCost cost = {},
                     void* userdata = nullptr);

private:
    friend class ScriptVm;
    friend struct detail::VmState;
    explicit Binder(detail::VmState& state) noexcept : m_state(&state) {}
    detail::VmState* m_state;
};

/// The ScriptVm owning `L` (any coroutine of the VM).
ScriptVm& vmFromState(lua_State* L) noexcept;

/// Userdata registered with the currently running binding (nullptr if none).
void* bindingUserdata(lua_State* L) noexcept;

/// Charges `fuel` to the running resume (for bindings whose cost depends on work done). Raises the
/// sticky kill when the hard budget is reached. No-op outside a resume.
void chargeFuel(lua_State* L, u64 fuel);

/// Fuel used so far by the running resume (0 outside a resume).
u64 currentFuel(lua_State* L) noexcept;

/// Task whose resume is running (invalid inside top-level callbacks and module loads).
TaskId currentTask(lua_State* L) noexcept;

/// Raises a typed `ScriptError` (a frozen table with `code` and `message`) in the calling coroutine.
[[noreturn]] void raiseError(lua_State* L, ScriptErrorCode code, std::string_view message);

/// Pushes a userdata of object type `type` holding `handleBits`.
void pushObject(lua_State* L, ObjectType type, u64 handleBits);
/// Resolves the object at `idx`: raises a type error if it is not a `type` userdata and a typed
/// StaleHandle error if the handle no longer resolves.
void* checkObject(lua_State* L, int idx, ObjectType type);
template <class T>
T* checkObjectAs(lua_State* L, int idx, ObjectType type) {
    return static_cast<T*>(checkObject(L, idx, type));
}
/// Reads the handle bits of a `type` userdata without resolving it (false if not one).
bool toObjectHandle(lua_State* L, int idx, ObjectType type, u64* outBits) noexcept;

/// Pushes a WorldPos userdata (frame-local f64 position).
void pushWorldPos(lua_State* L, const FramePos& pos);
/// WorldPos at `idx` or a type error.
FramePos checkWorldPos(lua_State* L, int idx);
/// WorldPos at `idx` or nullptr.
const FramePos* toWorldPos(lua_State* L, int idx) noexcept;

/// Protected call for bindings that call back into Luau (function and `nargs` arguments on top of
/// the stack). Returns LUA_OK with `nresults` results, or an error status with the error object on
/// the stack for the binding to handle. If the resume was killed inside the callback the kill is
/// re-raised here after the callee has unwound, so it cannot be swallowed.
int callLuau(lua_State* L, int nargs, int nresults);

/// Starts an asynchronous host call from an async binding. Raises InvalidState when the binding
/// cannot yield (inside a metamethod, callback or nested coroutine, or outside a task).
AsyncToken beginAsync(lua_State* L);
/// Suspends the task until ScriptVm::completeAsync/failAsync; `return yieldAsync(L);`.
int yieldAsync(lua_State* L);

} // namespace helios::script
