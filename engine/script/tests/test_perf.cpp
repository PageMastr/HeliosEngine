// Fuel metering cost (04 §10.2 budget: the interrupt hook costs <= 10 % of script time) and the
// ns-per-fuel figure that `--calibrate-fuel` (WP-1.6) will measure on the reference SERVER core.
// Informational on shared CI machines: the assertion is deliberately loose (min of several runs).

#include <algorithm>

#include "helios/core/time.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

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

// Plain Luau state (no Helios sandbox): the baseline, optionally with a minimal interrupt.
u64 runRaw(const Bytecode& bc, bool interrupt) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (interrupt) lua_callbacks(L)->interrupt = &countingInterrupt;
    REQUIRE(luau_load(L, "bench", bc.data.data(), bc.data.size(), 0) == 0);
    const Stopwatch sw;
    const int status = lua_pcall(L, 0, 0, 0);
    const u64 ns = sw.elapsedNanos();
    CHECK(status == LUA_OK);
    lua_close(L);
    return ns;
}

} // namespace

TEST_CASE("perf: fuel metering overhead and ns per fuel") {
    const auto bc = compile(kWorkload, {}, "bench");
    REQUIRE(bc.ok());
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    h.load("bench", kWorkload);

    u64 raw = ~u64(0);
    u64 rawInterrupt = ~u64(0);
    u64 metered = ~u64(0);
    u64 fuel = 0;
    for (int i = 0; i < 5; ++i) {
        raw = std::min(raw, runRaw(**bc, false));
        g_rawSafepoints = 0;
        rawInterrupt = std::min(rawInterrupt, runRaw(**bc, true));
        const TaskId id = h.spawn("bench");
        const TickStats ts = h.step();
        REQUIRE(ts.finished == 1);
        metered = std::min(metered, ts.wallNanos);
        fuel = h.eventFor(id, ScriptEventKind::TaskFinished)->fuel;
    }
    const double overheadHook = static_cast<double>(rawInterrupt) / static_cast<double>(raw) - 1.0;
    const double overheadHelios = static_cast<double>(metered) / static_cast<double>(raw) - 1.0;
    MESSAGE("workload: ", fuel, " fuel (", g_rawSafepoints, " raw safepoints); raw ", raw / 1000,
            " us, raw+hook ", rawInterrupt / 1000, " us, Helios ", metered / 1000, " us; hook overhead ",
            overheadHook * 100.0, " %, Helios metering overhead ", overheadHelios * 100.0, " %, ns/fuel ",
            static_cast<double>(metered) / static_cast<double>(fuel));
    CHECK(overheadHelios < 1.0); // loose CI guard; the 10 % budget is tracked by the nightly bench
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
