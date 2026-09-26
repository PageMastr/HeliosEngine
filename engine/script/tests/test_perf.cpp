// Fuel metering cost and the ns-per-fuel figure that `--calibrate-fuel` (WP-1.6) will measure on the
// reference SERVER core. RT-13 / 04 §10.2 budget: metering costs <= 10 % of script time. It holds
// with the vendored fuel-counter patch (the VM decrements an inline counter and calls the host only
// at decision points); the per-safepoint callback it replaced measured 12-17 % (K39). "perf:" cases
// run serially on an idle nightly runner (ctest label `perf`); each figure is the best of several runs.

#include <algorithm>
#include <limits>

#include "helios/core/time.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

// The timing thresholds hold for optimized, uninstrumented builds (test::kTimingGates): the nightly
// perf job (linux-gcc, RelWithDebInfo). Debug and sanitizer builds (the ASan nightly also runs perf
// cases) keep only the loose guards and report the figures.
using helios::script::test::kTimingGates;

constexpr const char* kWorkload = R"(
    local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
    local acc = 0
    for r = 1, 12 do
        acc += fib(18)
        local t = table.create(2000, 0)
        for i = 1, 2000 do t[i] = (i * 3) % 7 end
        local s = 0
        for i = 1, #t do s += t[i] end
        local parts = {}
        for i = 1, 200 do parts[i] = tostring(i) end
        acc += s + #table.concat(parts)
    end
    return acc
)";

u64 g_rawSafepoints = 0;

void countingInterrupt(lua_State*, int gc) {
    if (gc < 0) ++g_rawSafepoints;
}

// A host whose next decision is always 64 fuel away (Helios' densest cadence: the wall-clock read
// of clients and editors, kWallCheckInterval).
void rearmingInterrupt(lua_State* L, int gc) {
    if (gc < 0) *lua_fuelcounter(L) = 64;
}

// All modes run the patched VM, so every safepoint pays the counter's decrement, the baseline too
// (a standalone A/B against stock 0.739 found the decrement costs no more than stock's null check of
// `interrupt`; engine/script/README.md, "Metering cost").
enum class RawMode {
    Unmetered,      ///< Counter out of reach, no interrupt: the VM's cheapest safepoint (a decrement).
    EverySafepoint, ///< Counter never armed: a minimal callback at every safepoint (WP-0.10's model).
    Counter64,      ///< Counter re-armed to 64 by the callback: the fuel-counter callback cadence.
};

// Plain Luau state (no Helios sandbox) running the same bytecode.
u64 runRaw(const Bytecode& bc, RawMode mode) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    switch (mode) {
    case RawMode::Unmetered: *lua_fuelcounter(L) = std::numeric_limits<i64>::max(); break;
    case RawMode::EverySafepoint: lua_callbacks(L)->interrupt = &countingInterrupt; break;
    case RawMode::Counter64:
        lua_callbacks(L)->interrupt = &rearmingInterrupt;
        *lua_fuelcounter(L) = 64;
        break;
    }
    REQUIRE(luau_load(L, "bench", bc.data.data(), bc.data.size(), 0) == 0);
    const Stopwatch sw;
    const int status = lua_pcall(L, 0, 0, 0);
    const u64 ns = sw.elapsedNanos();
    CHECK(status == LUA_OK);
    lua_close(L);
    return ns;
}

double overhead(u64 metered, u64 base) {
    return static_cast<double>(metered) / static_cast<double>(base) - 1.0;
}

} // namespace

TEST_CASE("perf: fuel metering overhead and ns per fuel") {
    const auto bc = compile(kWorkload, {}, "bench");
    REQUIRE(bc.ok());
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    // A wall limit makes the host read the clock every 64 fuel, as a cell's 20 ms backstop does; 10 s
    // so that a slow (sanitizer) build is never killed.
    c.budget.wallBackstopNanos = 10'000'000'000ull;
    Harness h(c);
    h.load("bench", kWorkload);

    u64 raw = ~u64(0);
    u64 rawEvery = ~u64(0);
    u64 rawCounter = ~u64(0);
    u64 metered = ~u64(0);
    u64 fuel = 0;
    for (int i = 0; i < 9; ++i) {
        raw = std::min(raw, runRaw(**bc, RawMode::Unmetered));
        g_rawSafepoints = 0;
        rawEvery = std::min(rawEvery, runRaw(**bc, RawMode::EverySafepoint));
        rawCounter = std::min(rawCounter, runRaw(**bc, RawMode::Counter64));
        const TaskId id = h.spawn("bench");
        const TickStats ts = h.step();
        REQUIRE(ts.finished == 1);
        metered = std::min(metered, ts.wallNanos);
        fuel = h.eventFor(id, ScriptEventKind::TaskFinished)->fuel;
    }
    MESSAGE("workload: ", fuel, " fuel (", g_rawSafepoints, " raw safepoints); raw ", raw / 1000,
            " us, raw+callback per safepoint ", rawEvery / 1000, " us (", overhead(rawEvery, raw) * 100.0,
            " %), raw+callback every 64 ", rawCounter / 1000, " us (", overhead(rawCounter, raw) * 100.0,
            " %), Helios ", metered / 1000, " us (metering overhead ", overhead(metered, raw) * 100.0,
            " %), ns/fuel ", static_cast<double>(metered) / static_cast<double>(fuel));
    // RT-13's <= 10 %: the callback cadence alone (host called every 64 safepoints), and the whole
    // Helios host (sandbox, charging wrappers, scheduler and resume, fuel counter, clock reads)
    // against unmetered plain Luau.
    CHECK(overhead(metered, raw) < 1.0); // loose guard for every build
    if constexpr (kTimingGates) {
        CHECK(overhead(rawCounter, raw) <= 0.10);
        CHECK(overhead(metered, raw) <= 0.10);
    }
}

TEST_CASE("perf: host cost of a trivial resume versus FuelBudget::resumeCost") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 64u << 20;
    c.budget.fuelPerTick = 0; // resume everything
    Harness h(c);
    h.load("spinner", "while true do task.yield() end");
    constexpr int kTasks = 5000;
    for (int i = 0; i < kTasks; ++i) h.spawn("spinner", static_cast<u64>(i));
    h.step();
    u64 best = ~u64(0);
    u64 fuel = 0;
    for (int i = 0; i < 5; ++i) {
        const TickStats ts = h.step();
        REQUIRE(ts.resumed == kTasks);
        best = std::min(best, ts.wallNanos);
        fuel = ts.fuel;
    }
    const double nsPerResume = static_cast<double>(best) / kTasks;
    const double fuelPerResume = static_cast<double>(fuel) / kTasks;
    MESSAGE("trivial resume: ", nsPerResume, " ns, ", fuelPerResume, " fuel (resumeCost ",
            c.budget.resumeCost, ") -> ", nsPerResume / fuelPerResume, " ns per fuel");
    CHECK(nsPerResume < 100'000.0); // loose CI guard; calibration sets resumeCost from this figure
}
