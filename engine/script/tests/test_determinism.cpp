// Determinism: a pure script produces identical output and identical fuel counts across runs and
// VMs (fuel counts are what replay and the lane budget rely on, 04 §10.2), and native codegen hits
// the same safepoints as the interpreter.

#include "luacodegen.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

constexpr const char* kPureScript = R"(
    local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
    local primes = {}
    for i = 2, 2000 do
        local isPrime, j = true, 2
        while isPrime and j <= math.floor(math.sqrt(i)) do
            if i % j == 0 then isPrime = false end
            j += 1
        end
        if isPrime then table.insert(primes, i) end
    end
    local words = {}
    for i = 1, 200 do words[i] = string.format("%05d", (i * 7919) % 100003) end
    table.sort(words)
    local h = 0
    for _, w in ipairs(words) do
        for c = 1, #w do h = (h * 31 + string.byte(w, c)) % 2147483647 end
    end
    local rolls = {}
    for i = 1, 16 do rolls[i] = math.random(1, 1000) end
    local pos = WorldPos.new(1e9, 0, 0)
    for i = 1, 100 do pos = pos:offset(0.25, -0.5, 1 / 3) end
    print(fib(18), #primes, primes[#primes], h, table.concat(rolls, ","), string.format("%.17g", pos.z))
)";

// Measured once with GCC 13; must match on every toolchain (see the test below). Changes only when
// charges change: 35270 + 116 after print's base cost became 100 (+99), table.concat started paying
// for the bytes it produces (+1) and every resume pays FuelBudget::resumeCost (+16).
constexpr u64 kGoldenPureFuel = 35386;

struct RunResult {
    std::string output;
    u64 fuel = 0;
};

RunResult runSource(const char* source, bool native) {
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 50'000'000;
    c.budget.fuelPerTick = 50'000'000;
    c.randomSeed = 1234;
    c.enableNativeCodegen = native;
    Harness h(c);
    ModuleOptions options;
    options.native = native;
    h.load("pure", source, options);
    const TaskId id = h.spawn("pure");
    h.steps(5);
    const auto* done = h.eventFor(id, ScriptEventKind::TaskFinished);
    REQUIRE(done != nullptr);
    REQUIRE(h.prints.size() == 1);
    return RunResult{h.prints[0], done->fuel};
}

RunResult runPure(bool native) { return runSource(kPureScript, native); }

} // namespace

TEST_CASE("determinism: a pure script gives identical output and fuel across runs") {
    const RunResult a = runPure(false);
    const RunResult b = runPure(false);
    CHECK(a.output == b.output);
    CHECK(a.fuel == b.fuel);
    CHECK(a.output.rfind("2584\t303\t1999\t", 0) == 0);
    MESSAGE("pure script: fuel ", a.fuel, ", output ", a.output);
    // Golden fuel: bytecode and safepoints are compiler-independent, so every toolchain (MSVC,
    // clang-cl, GCC, Clang, MinGW) must count the same fuel (04 §10.2, NS-3.8 cross-compiler replay).
    CHECK(a.fuel == kGoldenPureFuel);
}

TEST_CASE("determinism: native codegen counts the same fuel as the interpreter") {
    if (!luau_codegen_supported()) {
        MESSAGE("native codegen not supported on this target; skipped");
        return;
    }
    const RunResult interp = runPure(false);
    const RunResult native = runPure(true);
    CHECK(native.output == interp.output);
    CHECK(native.fuel == interp.fuel);
}

TEST_CASE("determinism: known codegen divergence — numeric for loops left early") {
    // Luau 0.739's code generator places the numeric-for interrupt at the start of the loop body,
    // the interpreter in FORNLOOP (IrTranslation.cpp translateInstForNPrep). Counts agree for loops
    // that run to completion, but every iteration left by `break`/`return` costs one extra fuel in
    // native code. This pins the divergence so the pending vendored patch (or an upstream fix) is
    // noticed; until then native codegen stays off on cells (VmConfig default).
    if (!luau_codegen_supported()) return;
    constexpr const char* kEarlyExit = R"(
        local exits = 0
        for i = 1, 50 do
            for j = 1, 10 do
                if j == 3 then exits += 1 break end
            end
        end
        print(exits)
    )";
    const RunResult interp = runSource(kEarlyExit, false);
    const RunResult native = runSource(kEarlyExit, true);
    CHECK(interp.output == "50");
    CHECK(native.output == "50");
    CHECK(native.fuel == interp.fuel + 50); // one extra safepoint per early exit
}

TEST_CASE("determinism: charges never depend on the printed length of heap addresses") {
    // tostring() of a table or userdata embeds its address, whose printed length differs between
    // runs and platforms (glibc "0x55d1..." vs MSVC "000055D1..."): print and string.format must
    // charge the same fuel whatever it is, or a replay would kill at a different count.
    Harness h;
    h.expectRuns("addresses", R"(
        local function cost(f) local f0 = task.fuel() f() return task.fuel() - f0 end
        local t, p, w = {}, newproxy(), WorldPos.new(1, 2, 3)
        local a = cost(function() print(t, p, w) end)
        local b = cost(function() print(1, 2, 3) end)
        assert(a == b, "print charged " .. a .. " vs " .. b)
        local c = cost(function() local _ = string.format("%*|%*", t, p) end)
        local d = cost(function() local _ = string.format("%*|%*", 1, 2) end)
        assert(c == d, "string.format charged " .. c .. " vs " .. d)
    )");
}
