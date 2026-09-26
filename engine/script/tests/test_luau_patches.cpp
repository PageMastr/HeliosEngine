// The vendored Luau patches fuel metering relies on (third_party/luau/patches, listed in
// third_party/MANIFEST.md; 02 §7.4, 04 §10.2), checked at the Luau API level below ScriptVm:
//  - fuel-counter: every gc < 0 safepoint (calls, returns, loop back edges, pattern-matcher steps)
//    decrements the VM's inline counter exactly once, in the interpreter and in native code, and
//    `interrupt` runs only when the counter reaches zero;
//  - codegen-fornloop-fuel: native code reaches the same safepoints as the interpreter, including
//    numeric for loops left by `break` or `return`, and at the same program points, so a kill at
//    the k-th safepoint stops both at the same place.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "lua.h"
#include "luacodegen.h"
#include "lualib.h"

#include "helios/script/compiler.h"

using namespace helios;
using namespace helios::script;

namespace {

// Every kind of safepoint, including numeric loops left early and a backtracking pattern match.
constexpr const char* kSafepointMix = R"(
    local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
    local acc = fib(12)
    for i = 1, 40 do
        for j = 1, 10 do
            if j == 4 then break end
            acc += j
        end
    end
    local function firstOver(limit)
        for k = 1, 100 do
            if k * k > limit then return k end
        end
        return 0
    end
    for i = 1, 25 do acc += firstOver(i) end
    local n = 0
    while n < 300 do n += 1 end
    repeat n -= 3 until n <= 0
    for _, v in ipairs({1, 2, 3, 4, 5}) do acc += v end
    for _, v in pairs({a = 1, b = 2}) do acc += v end
    local s = string.rep("ab", 200) .. "c"
    local first, last = string.find(s, "a.-b.-c")
    acc += first + last + select("#", string.gsub(s, "a", "x"))
    return acc
)";

u64 g_calls = 0;
u64 g_unreset = 0; ///< interrupt calls that found the counter not reset to 0
i64 g_rearm = 0;

void countEverySafepoint(lua_State* L, int gc) {
    if (gc < 0) {
        ++g_calls;
        // The VM, the pattern matcher and the native interrupt helper reset the counter to 0 before
        // they call the host (so a host that re-arms it starts from a known value).
        if (*lua_fuelcounter(L) != 0) ++g_unreset;
    }
}

// Re-arms the counter like a host with a decision point every g_rearm safepoints.
void rearmingInterrupt(lua_State* L, int gc) {
    if (gc < 0) {
        ++g_calls;
        *lua_fuelcounter(L) = g_rearm;
    }
}

struct RawRun {
    double result = 0;
    u64 calls = 0;    ///< interrupt(L, gc < 0) calls
    i64 counter = 0;  ///< the counter after the run
};

// Runs `bc` in a plain Luau state (no Helios host) with the counter armed at `armed`.
RawRun runRaw(const Bytecode& bc, bool native, i64 armed, void (*interrupt)(lua_State*, int)) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (native) luau_codegen_create(L);
    lua_callbacks(L)->interrupt = interrupt;
    REQUIRE(luau_load(L, "mix", bc.data.data(), bc.data.size(), 0) == 0);
    if (native) luau_codegen_compile(L, -1);
    *lua_fuelcounter(L) = armed;
    g_calls = 0;
    g_unreset = 0;
    const int status = lua_pcall(L, 0, 1, 0);
    REQUIRE_MESSAGE(status == LUA_OK, (status == LUA_OK ? "" : lua_tostring(L, -1)));
    RawRun r;
    r.result = lua_tonumber(L, -1);
    r.calls = g_calls;
    r.counter = *lua_fuelcounter(L);
    lua_close(L);
    return r;
}

std::vector<bool> modes() {
    std::vector<bool> m{false};
    if (luau_codegen_supported()) m.push_back(true);
    return m;
}

} // namespace

TEST_CASE("luau patches: fuel-counter counts every gc < 0 safepoint once, in the VM and native code") {
    const auto bc = compile(kSafepointMix, {}, "mix");
    REQUIRE(bc.ok());
    for (const bool native : modes()) {
        CAPTURE(native);
        // Never armed (0): the interrupt runs at every safepoint, as in stock Luau.
        const RawRun each = runRaw(**bc, native, 0, &countEverySafepoint);
        CHECK(each.calls > 2'000);
        CHECK(each.counter == 0);
        CHECK(g_unreset == 0);
        // Armed far away: the host is never called and the counter took exactly one per safepoint.
        constexpr i64 kFar = i64(1) << 40;
        const RawRun counted = runRaw(**bc, native, kFar, &countEverySafepoint);
        CHECK(counted.calls == 0);
        CHECK(static_cast<u64>(kFar - counted.counter) == each.calls);
        CHECK(counted.result == each.result);
    }
}

TEST_CASE("luau patches: fuel-counter calls the host only when the counter reaches zero") {
    const auto bc = compile(kSafepointMix, {}, "mix");
    REQUIRE(bc.ok());
    for (const bool native : modes()) {
        CAPTURE(native);
        const RawRun each = runRaw(**bc, native, 0, &countEverySafepoint);
        for (const i64 interval : {i64(1), i64(7), i64(64), i64(1000)}) {
            CAPTURE(interval);
            g_rearm = interval;
            const RawRun r = runRaw(**bc, native, interval, &rearmingInterrupt);
            // Every call is the interval-th safepoint since the previous one; the rest is left over.
            CHECK(r.calls == each.calls / static_cast<u64>(interval));
            CHECK(r.calls * static_cast<u64>(interval) + static_cast<u64>(interval - r.counter) == each.calls);
        }
    }
}

TEST_CASE("luau patches: codegen-fornloop-fuel — native code reaches the interpreter's safepoints") {
    if (!luau_codegen_supported()) {
        MESSAGE("native codegen not supported on this target; skipped");
        return;
    }
    // 40 loops left by break and 25 by return: stock Luau 0.739 reaches 65 more safepoints natively.
    const auto bc = compile(kSafepointMix, {}, "mix");
    REQUIRE(bc.ok());
    const RawRun interp = runRaw(**bc, false, 0, &countEverySafepoint);
    const RawRun native = runRaw(**bc, true, 0, &countEverySafepoint);
    CHECK(native.result == interp.result);
    CHECK(native.calls == interp.calls);
}

namespace {

// Numeric loops left by continue, run with negative, constant and variable steps, nested and left by
// break or return, generic loops left by break, and while loops. W records progress, so a kill at a
// different program point leaves a different W.
constexpr const char* kKillPositions = R"(
    W = 0
    for i = 1, 12 do
        if i % 3 == 0 then continue end
        W += 1
    end
    for i = 10, 1, -2 do W += 1 end
    local function stepped(step) for i = 1, 30, step do W += 1 if i > 20 then break end end end
    stepped(3)
    stepped(-1)
    for i = 1, 5 do
        for j = 1, 5 do
            if j == i then break end
            W += 1
        end
        W += 1
    end
    for _, v in ipairs({1, 2, 3, 4, 5, 6}) do if v == 4 then break end W += 1 end
    for _, v in pairs({a = 1, b = 2, c = 3}) do W += v break end
    local function firstAt(n) for i = 1, n do if i == 3 then return i end W += 1 end return 0 end
    for i = 1, 5 do W += firstAt(i) end
    local k = 0
    while k < 10 do k += 1 W += 1 end
)";

bool g_stopped = false;

// Stops the chunk at the safepoint where the counter (armed at k) reaches zero: the k-th.
void stopAtZero(lua_State* L, int gc) {
    if (gc >= 0 || g_stopped) return;
    g_stopped = true;
    lua_pushstring(L, "stop");
    lua_error(L);
}

// W after a run stopped at safepoint k (or completed), in the interpreter or native code.
double wAfterStopAt(const Bytecode& bc, bool native, i64 k, bool& stopped) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (native) luau_codegen_create(L);
    lua_callbacks(L)->interrupt = &stopAtZero;
    REQUIRE(luau_load(L, "kill", bc.data.data(), bc.data.size(), 0) == 0);
    if (native) luau_codegen_compile(L, -1);
    *lua_fuelcounter(L) = k;
    g_stopped = false;
    const int status = lua_pcall(L, 0, 0, 0);
    stopped = g_stopped;
    CHECK(status == (stopped ? LUA_ERRRUN : LUA_OK));
    lua_getglobal(L, "W");
    const double w = lua_tonumber(L, -1);
    lua_close(L);
    return w;
}

} // namespace

TEST_CASE("luau patches: codegen-fornloop-fuel — a kill at safepoint k stops both modes at one point") {
    if (!luau_codegen_supported()) {
        MESSAGE("native codegen not supported on this target; skipped");
        return;
    }
    // Stock 0.739 placed every numeric-loop safepoint at the top of the body, one body earlier than
    // the interpreter's FORNLOOP, so even loops that run to completion were killed at another point.
    const auto bc = compile(kKillPositions, {}, "kill");
    REQUIRE(bc.ok());
    const RawRun all = runRaw(**bc, false, 0, &countEverySafepoint);
    const i64 total = static_cast<i64>(all.calls);
    REQUIRE(total > 50);
    std::vector<std::string> mismatches;
    for (i64 k = 1; k <= total + 1; ++k) {
        bool interpStopped = false;
        bool nativeStopped = false;
        const double interp = wAfterStopAt(**bc, false, k, interpStopped);
        const double native = wAfterStopAt(**bc, true, k, nativeStopped);
        CHECK(interpStopped == (k <= total));
        if (interp != native || interpStopped != nativeStopped) {
            mismatches.push_back("k=" + std::to_string(k) + ": interpreter W=" + std::to_string(interp) +
                                 ", native W=" + std::to_string(native));
        }
    }
    INFO(mismatches.size(), " of ", total + 1, " stop points differ; first: ",
         mismatches.empty() ? std::string("none") : mismatches.front());
    CHECK(mismatches.empty());
}
