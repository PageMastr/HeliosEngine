// Fuel metering and kills (RT-13): runaway kill at fuel_kill, sticky kills that pcall cannot
// swallow, kills inside a metamethod, a table.sort comparator and C++→Luau callbacks (unwinding
// through RAII with no lock held and the VM usable afterwards), no involuntary yields,
// task.checkpoint, the lane bound, binding and builtin charges, wall budgets and the three-kills
// rule, and the inline fuel counter (the host runs only at decision points, with exact fuel).

#include <algorithm>
#include <mutex>
#include <string>

#include "helios/core/time.h"
#include "luacodegen.h"
#include "script_test_util.h"
#include "vm_state.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;
using helios::script::test::RecordedEvent;

namespace {

struct Fixture {
    int guardsAlive = 0;
    int guardsDestroyed = 0;
    std::mutex mutex;
    int touches = 0;
    int sleeps = 0;
};

// Test.each(n, fn): calls fn(i) n times through callLuau while holding an RAII guard and a mutex
// (deliberately, to prove that a kill unwinds binding frames and releases what they hold).
int tEach(lua_State* L) {
    auto* f = static_cast<Fixture*>(bindingUserdata(L));
    const int n = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct Guard {
        Fixture& f;
        explicit Guard(Fixture& fx) : f(fx) { ++f.guardsAlive; }
        ~Guard() {
            --f.guardsAlive;
            ++f.guardsDestroyed;
        }
    } guard(*f);
    std::lock_guard lock(f->mutex);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        lua_pushvalue(L, 2);
        lua_pushinteger(L, i);
        if (callLuau(L, 1, 0) == LUA_OK) {
            ++ok;
        } else {
            lua_pop(L, 1); // ordinary errors are the binding's to handle
        }
    }
    lua_pushinteger(L, ok);
    return 1;
}

int tTouch(lua_State* L) {
    ++static_cast<Fixture*>(bindingUserdata(L))->touches;
    return 0;
}

int tItems(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(lua_objlen(L, 1)));
    return 1;
}

int tSleep(lua_State* L) {
    ++static_cast<Fixture*>(bindingUserdata(L))->sleeps;
    sleepMillis(static_cast<u32>(luaL_checkinteger(L, 1)));
    return 0;
}

ScriptVm::ApiRegistrar testApi(Fixture& f) {
    return [&f](Binder& b) {
        b.function("Test", "each", &tEach, FuelCost{1, 0, 0}, &f);
        b.function("Test", "touch", &tTouch, FuelCost{4'000, 0, 0}, &f);
        b.function("Test", "items", &tItems, FuelCost{1, 1'000, 1}, &f);
        b.function("Test", "sleep", &tSleep, FuelCost{1, 0, 0}, &f);
    };
}

const RecordedEvent& requireKilled(const Harness& h, TaskId id) {
    const RecordedEvent* e = h.eventFor(id, ScriptEventKind::TaskKilled);
    INFO("task was not killed");
    REQUIRE(e != nullptr);
    REQUIRE(e->error.has_value());
    CHECK(e->error->code == ScriptErrorCode::Killed);
    return *e;
}

u64 totalYields(const VmStats& s) {
    u64 n = 0;
    for (u64 y : s.yieldsByReason) n += y;
    return n;
}

// Counts the host's gc < 0 interrupt calls by forwarding to the VM's own callback (white-box).
void (*g_hostInterrupt)(lua_State*, int) = nullptr;
u64 g_hostCalls = 0;

void countHostCalls(lua_State* L, int gc) {
    if (gc < 0) ++g_hostCalls;
    g_hostInterrupt(L, gc);
}

void instrumentInterrupt(ScriptVm& vm) {
    lua_Callbacks* cb = lua_callbacks(vm.state());
    g_hostInterrupt = cb->interrupt;
    cb->interrupt = &countHostCalls;
    g_hostCalls = 0;
}

u64 g_safepoints = 0;

void countSafepoints(lua_State*, int gc) {
    if (gc < 0) ++g_safepoints;
}

// Safepoints (gc < 0 interrupts) of `source` in a plain Luau state, counted one call at a time.
u64 rawSafepoints(const char* source, bool native) {
    const auto bc = compile(source, {}, "raw");
    REQUIRE(bc.ok());
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (native) luau_codegen_create(L);
    lua_callbacks(L)->interrupt = &countSafepoints; // counter left at 0: called at every safepoint
    REQUIRE(luau_load(L, "raw", (*bc)->data.data(), (*bc)->data.size(), 0) == 0);
    if (native) luau_codegen_compile(L, -1);
    g_safepoints = 0;
    REQUIRE(lua_pcall(L, 0, 0, 0) == LUA_OK);
    lua_close(L);
    return g_safepoints;
}

// Only safepoints (no library calls, so no binding or builtin charges and no allocation).
constexpr const char* kSafepointsOnly = R"(
    local function f(n) if n < 2 then return n end return f(n - 1) + f(n - 2) end
    local acc = f(14)
    for i = 1, 300 do
        for j = 1, 10 do
            if j == 6 then break end
            acc += j
        end
    end
    local n = 0
    while n < 20000 do n += 1 end
    acc += n
)";

} // namespace

TEST_CASE("fuel: a runaway loop is killed at exactly fuel_kill and the VM stays usable") {
    Harness h;
    const TaskId id = h.run("runaway", "local x = 0\nwhile true do x += 1 end");
    const TickStats ts = h.step();
    CHECK(ts.killed == 1);
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.killReason == KillReason::Fuel);
    CHECK(e.fuel == 10'000); // the resume is charged exactly fuel_kill
    CHECK(e.error->module == "runaway");
    REQUIRE_FALSE(e.error->stack.empty());
    CHECK(e.error->stack.front().source == "runaway");
    CHECK(e.error->stack.front().line == 2);
    CHECK(h.vm->taskState(id) == TaskState::Gone);
    CHECK(h.vm->stats().killsByReason[static_cast<usize>(KillReason::Fuel)] == 1);
    h.expectRuns("after", "local s = 0 for i = 1, 100 do s += i end assert(s == 5050)");
}

TEST_CASE("fuel: pcall, xpcall and nested coroutines cannot swallow a kill (sticky)") {
    Harness h;
    const TaskId a = h.run("pcall", R"(
        local caught = 0
        for i = 1, 100 do
            local ok = pcall(function() while true do end end)
            if not ok then caught += 1 end
        end
        print("survived", caught)
    )");
    const TaskId b = h.run("xpcall", R"(
        xpcall(function() while true do end end, function(e) while true do end end)
        print("survived xpcall")
    )");
    const TaskId c = h.run("nested", R"(
        local co = coroutine.create(function() while true do end end)
        local ok, err = coroutine.resume(co)
        print("survived coroutine", ok)
    )");
    const TaskId d = h.run("errorloop", R"(
        while true do
            local ok = pcall(function() for i = 1, 1e9 do end end)
            print("retry")
        end
    )");
    h.steps(20);
    for (const TaskId id : {a, b, c, d}) requireKilled(h, id);
    CHECK(h.prints.empty()); // nothing ran after any kill
}

TEST_CASE("fuel: a stored kill error rethrown later is an ordinary error, not a kill") {
    Harness h;
    h.load("stash", "return {}");
    const TaskId a = h.run("stasher", R"(
        local stash = require("stash")
        local co = coroutine.create(function() while true do end end)
        local ok, e = coroutine.resume(co)
        stash.e = e -- no safepoint before the store; the kill re-raises right after
    )");
    const TaskId b = h.run("rethrower", "wait(0.1)\nerror(require('stash').e)");
    h.steps(10);
    requireKilled(h, a);
    const RecordedEvent* e = h.eventFor(b, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    CHECK(e->error->code == ScriptErrorCode::Runtime);
    CHECK(e->error->killReason == KillReason::None);
    CHECK(h.eventFor(b, ScriptEventKind::TaskKilled) == nullptr);
}

TEST_CASE("fuel: kill inside a metamethod unwinds cleanly") {
    Harness h;
    const TaskId index = h.run("index", R"(
        local t = setmetatable({}, {__index = function(_, k) while true do end end})
        local v = t.anything
        print("unreachable")
    )");
    const TaskId lt = h.run("lt", R"(
        local mt = {__lt = function(a, b) while true do end end}
        local arr = {setmetatable({}, mt), setmetatable({}, mt), setmetatable({}, mt)}
        table.sort(arr) -- __lt runs inside table.sort's C frame
        print("unreachable")
    )");
    const TaskId add = h.run("add", R"(
        local v = setmetatable({}, {__add = function() local n = 0 while true do n += 1 end end})
        local ok = pcall(function() return v + 1 end)
        print("unreachable", ok)
    )");
    h.steps(20);
    for (const TaskId id : {index, lt, add}) requireKilled(h, id);
    CHECK(h.prints.empty());
    CHECK(h.vm->stats().involuntaryYields == 0);
    h.expectRuns("after", "assert(setmetatable({}, {__index = function() return 7 end}).x == 7)");
}

TEST_CASE("fuel: kill inside a table.sort comparator unwinds cleanly") {
    Harness h;
    const TaskId id = h.run("sort", R"(
        local arr = {}
        for i = 1, 64 do arr[i] = 65 - i end
        table.sort(arr, function(a, b) while true do end end)
        print("unreachable")
    )");
    h.steps(5);
    requireKilled(h, id);
    CHECK(h.prints.empty());
    h.expectRuns("after", R"(
        local arr = {5, 3, 1, 4, 2}
        table.sort(arr, function(a, b) return a < b end)
        assert(table.concat(arr, ",") == "1,2,3,4,5")
    )");
}

TEST_CASE("fuel: kill inside a Luau callback invoked from C++ releases RAII state and locks") {
    Fixture f;
    Harness h(Harness::defaultConfig(), testApi(f));
    // Ordinary callback errors are returned to the binding, which handles them.
    h.expectRuns("errors", R"(
        local ok = Test.each(3, function(i) error("boom " .. i) end)
        assert(ok == 0)
        assert(Test.each(4, function(i) end) == 4)
    )");
    CHECK(f.guardsAlive == 0);
    CHECK(f.guardsDestroyed == 2);

    const TaskId id = h.run("killcb", R"(
        local n = Test.each(10, function(i) while true do end end)
        print("unreachable", n)
    )");
    h.steps(5);
    requireKilled(h, id);
    CHECK(h.prints.empty());
    CHECK(f.guardsAlive == 0); // the binding frame unwound through RAII
    CHECK(f.guardsDestroyed == 3);
    const bool lockFree = f.mutex.try_lock(); // no lock left held
    CHECK(lockFree);
    if (lockFree) f.mutex.unlock();

    // A binding cannot swallow the kill either: the trampoline re-raises it after the binding returns.
    const TaskId pc = h.run("pcallcb", R"(
        local ok = pcall(Test.each, 10, function(i) while true do end end)
        print("unreachable", ok)
    )");
    h.steps(5);
    requireKilled(h, pc);
    CHECK(h.prints.empty());
    h.expectRuns("after", "assert(Test.each(2, function() end) == 2)");
    CHECK(h.vm->stats().involuntaryYields == 0);
}

TEST_CASE("fuel: engine→Luau callbacks are metered like a resume and can be killed") {
    Harness h;
    h.load("cb", R"(
        local M = {}
        function M.spin() while true do end end
        function M.add(a, b) return a + b end
        function M.sleepy() wait(1) end
        return M
    )");
    const auto killed = h.vm->callExport("cb", "spin");
    REQUIRE_FALSE(killed.ok());
    CHECK(killed.error().code == ErrorCode::LimitExceeded);
    CHECK(h.vm->lastError().code == ScriptErrorCode::Killed);
    CHECK(h.vm->lastError().killReason == KillReason::Fuel);
    REQUIRE(h.last(ScriptEventKind::TaskKilled) != nullptr);
    CHECK(h.last(ScriptEventKind::TaskKilled)->fuel == 10'000);

    double sum = 0;
    const auto ok = h.vm->callExport(
        "cb", "add",
        [](lua_State* L) {
            lua_pushnumber(L, 2);
            lua_pushnumber(L, 3);
            return 2;
        },
        [&](lua_State* L, int base, int count) {
            REQUIRE(count == 1);
            sum = lua_tonumber(L, base);
        },
        1);
    CHECK(ok.ok());
    CHECK(sum == 5);

    // Callbacks cannot yield: wait() inside one is an InvalidState error, not a hidden yield.
    const auto noYield = h.vm->callExport("cb", "sleepy");
    REQUIRE_FALSE(noYield.ok());
    CHECK(h.vm->lastError().code == ScriptErrorCode::InvalidState);
    CHECK(h.vm->stats().involuntaryYields == 0);
}

TEST_CASE("fuel: no involuntary yields — every yield is an explicit call (instrumented)") {
    Fixture f;
    Harness h(Harness::defaultConfig(), testApi(f));
    const TaskId id = h.run("yields", R"(
        for i = 1, 3 do wait(0) end
        task.yield()
        -- Budgets now run out inside the three hard places; none of them may yield.
        local mt = {__index = function() while true do end end}
        local ok = pcall(function() return setmetatable({}, mt).x end)
    )");
    const TaskId sortTask = h.run("sortyield", R"(
        wait(0)
        table.sort({3, 2, 1}, function(a, b) while true do end end)
    )");
    const TaskId cbTask = h.run("cbyield", R"(
        wait(0)
        Test.each(1, function() while true do end end)
    )");
    h.steps(50);
    requireKilled(h, id);
    requireKilled(h, sortTask);
    requireKilled(h, cbTask);
    const VmStats s = h.vm->stats();
    CHECK(s.involuntaryYields == 0);
    CHECK(s.yieldsByReason[static_cast<usize>(YieldReason::Wait)] == 5);
    CHECK(s.yieldsByReason[static_cast<usize>(YieldReason::TaskYield)] == 1);
    CHECK(totalYields(s) == 6);
}

TEST_CASE("fuel: task.checkpoint yields past the soft budget so long work is deferred, never killed") {
    Harness h;
    const TaskId id = h.run("longwork", R"(
        assert(task.checkpoint() == false, "checkpoint yielded below the soft budget")
        local yields, total = 0, 0
        for i = 1, 200 do
            for j = 1, 200 do total += 1 end -- ~200 fuel; 40k fuel in total > fuel_kill
            if task.checkpoint() then yields += 1 end
        end
        assert(total == 40000)
        print(yields)
    )");
    h.steps(100);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished) != nullptr);
    CHECK(h.count(ScriptEventKind::TaskKilled) == 0);
    REQUIRE(h.prints.size() == 1);
    CHECK(std::stoi(h.prints[0]) >= 15);
    CHECK(h.vm->stats().yieldsByReason[static_cast<usize>(YieldReason::Checkpoint)] >= 15);
}

TEST_CASE("fuel: task.checkpoint returns false where it cannot yield") {
    Harness h;
    h.expectRuns("noyield", R"(
        for i = 1, 3000 do end -- past the soft budget (2000)
        local inSort, inMeta
        table.sort({2, 1}, function(a, b) inSort = task.checkpoint() return a < b end)
        local t = setmetatable({}, {__index = function() inMeta = task.checkpoint() return 1 end})
        local _ = t.x
        assert(inSort == false and inMeta == false)
        assert(task.checkpoint() == true) -- at the task's own level it yields
    )");
}

TEST_CASE("fuel: the soft budget raises ScriptOverBudget once per resume") {
    Harness h;
    const TaskId id = h.run("heavy", "for i = 1, 5000 do end\nwait(0)\nfor i = 1, 5000 do end");
    h.steps(10);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished));
    CHECK(h.count(ScriptEventKind::OverBudget) == 2);
    CHECK(h.vm->stats().overBudget == 2);
}

TEST_CASE("fuel: lane fuel per tick stays within fuel_per_tick + fuel_kill") {
    Harness h;
    h.load("worker", R"(
        for k = 1, 5 do
            for i = 1, 4000 do end
            task.yield()
        end
    )");
    for (u64 owner = 1; owner <= 12; ++owner) h.spawn("worker", owner);
    const FuelBudget budget = h.vm->config().budget;
    bool deferred = false;
    for (int i = 0; i < 100 && h.vm->liveTaskCount() > 0; ++i) {
        const TickStats ts = h.step();
        CHECK(ts.fuel <= budget.fuelPerTick + budget.fuelKill);
        CHECK(ts.maxResumeFuel <= budget.fuelKill);
        deferred = deferred || ts.deferred > 0;
    }
    CHECK(deferred);
    CHECK(h.vm->liveTaskCount() == 0);
    CHECK(h.count(ScriptEventKind::TaskFinished) == 12);
    CHECK(h.count(ScriptEventKind::TaskKilled) == 0);
}

TEST_CASE("fuel: binding charges are taken before the call; a tripping charge has no side effect") {
    Fixture f;
    Harness h(Harness::defaultConfig(), testApi(f));
    const TaskId id = h.run("touch", "for i = 1, 5 do Test.touch() end");
    h.steps(5);
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(f.touches == 2); // 3 x 4000 > 10000: the third call never ran
    CHECK(e.fuel == 10'000);

    h.expectRuns("items", R"(
        local t = table.create(500, 0)
        local f0 = task.fuel()
        assert(Test.items(t) == 500)
        local charged = task.fuel() - f0
        assert(charged >= 501 and charged <= 510, "charged " .. charged)
    )");
}

TEST_CASE("fuel: wrapped builtins charge per item and cannot be bypassed") {
    Harness h;
    h.expectRuns("rep", R"(
        local f0 = task.fuel()
        local s = string.rep("x", 100000)
        local d = task.fuel() - f0
        assert(#s == 100000)
        assert(d >= 1600 and d < 1700, "string.rep charged " .. d)
    )");
    h.expectRuns("sort", R"(
        local arr = table.create(1000, 0)
        for i = 1, 1000 do arr[i] = 1001 - i end
        local f0 = task.fuel()
        table.sort(arr)
        local d = task.fuel() - f0
        assert(arr[1] == 1 and arr[1000] == 1000)
        assert(d >= 5000 and d < 5100, "table.sort charged " .. d) -- 1000 * ceil(log2 1000) / 2
    )");
    h.expectRuns("create", R"(
        local f0 = task.fuel()
        local t = table.create(100000, true)
        local d = task.fuel() - f0
        assert(d >= 6300 and d < 6400, "table.create charged " .. d)
    )");
    h.expectRuns("concat", R"(
        local t = table.create(4000, "a")
        local f0 = task.fuel()
        local s = table.concat(t)
        local d = task.fuel() - f0
        assert(#s == 4000 and d >= 1000 and d < 1100, "table.concat charged " .. d)
    )");
    // Cells cap pattern subjects at 64 KiB.
    h.expectRuns("pattern", R"(
        local big = string.rep("a", 70000)
        local ok, err = pcall(string.find, big, "b")
        assert(not ok and string.find(tostring(err), "64 KiB", 1, true))
        assert(string.find(string.rep("a", 1000) .. "b", "b") == 1001)
    )");
    // A huge string.rep trips the kill before allocating anything.
    const TaskId big = h.run("bigrep", "local s = string.rep('x', 1e9)");
    h.steps(3);
    requireKilled(h, big);
}

TEST_CASE("fuel: the wall-clock backstop kills a binding far over its calibrated charge") {
    Fixture f;
    VmConfig c = Harness::defaultConfig();
    c.budget.wallBackstopNanos = 20'000'000;
    Harness h(c, testApi(f));
    const TaskId id = h.run("slow", "for i = 1, 100 do Test.sleep(5) end");
    h.steps(3);
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.killReason == KillReason::WallBackstop);
    CHECK(f.sleeps >= 1);  // >= 4 on an idle machine (5 ms each against a 20 ms backstop)
    CHECK(f.sleeps < 100); // killed long before the loop could finish
    CHECK(h.vm->stats().killsByReason[static_cast<usize>(KillReason::WallBackstop)] == 1);
}

TEST_CASE("fuel: client profile kills at the wall-time budget and still counts fuel") {
    VmConfig c = Harness::defaultConfig();
    c.profile = HostProfile::Client;
    c.budget = FuelBudget::client();
    Harness h(c);
    const TaskId id = h.run("clientloop", "while true do end");
    const TickStats ts = h.step();
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.killReason == KillReason::WallBudget);
    CHECK(e.fuel > 0);
    // RT-13: a runaway dies within ~5 ms of wall time on the client (loose bound for shared CI).
    MESSAGE("client runaway killed after ", static_cast<double>(ts.wallNanos) / 1e6, " ms, fuel ", e.fuel);
    CHECK(ts.wallNanos >= 5'000'000);
    CHECK(ts.wallNanos < 250'000'000); // generous: shared CI machines preempt
}

TEST_CASE("fuel: three kills of a module within the window disable it") {
    Harness h;
    h.load("bad", "wait(tonumber(0))\nwhile true do end");
    std::vector<TaskId> ids;
    for (int i = 0; i < 4; ++i) ids.push_back(h.spawn("bad", static_cast<u64>(i + 1)));
    h.steps(10, false);
    CHECK(h.count(ScriptEventKind::TaskKilled) >= 3);
    CHECK(h.count(ScriptEventKind::ModuleDisabled) == 1);
    const auto info = h.vm->moduleInfo("bad");
    REQUIRE(info);
    CHECK(info->disabled);
    const auto refused = h.vm->spawnScript("bad");
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::PermissionDenied);
    CHECK(h.vm->liveTaskCount() == 0);

    REQUIRE(h.vm->enableModule("bad").ok());
    CHECK(h.vm->spawnScript("bad").ok());
}

TEST_CASE("fuel: every builtin whose work grows with its input or output is charged") {
    // Beyond 02 §7.4's list: an uncharged O(n) builtin costs one safepoint however large its input,
    // so a loop over it runs far past fuel_kill in wall time. Each check below measures the charge of
    // one call on a large input; the uncharged baseline is 1-3 fuel.
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    h.expectRuns("charges", R"(
        local big = string.rep("a", 64000)
        local list = table.create(40000, 1)
        local buf = buffer.create(64000)
        local function cost(label, min, f)
            local f0 = task.fuel()
            f()
            local d = task.fuel() - f0
            assert(d >= min, label .. " charged only " .. d)
        end
        cost("string.upper", 1000, function() local _ = big:upper() end)
        cost("string.lower", 1000, function() local _ = string.lower(big) end)
        cost("string.reverse", 1000, function() local _ = big:reverse() end)
        cost("string.sub", 1000, function() local _ = big:sub(2) end)
        cost("string.pack", 1000, function() local _ = string.pack("c64000", big) end)
        cost("string.unpack", 1000, function() local _ = string.unpack("c64000", big) end)
        cost("string.format", 1000, function() local _ = string.format("%s%s", big, big) end)
        cost("string.gsub output", 1000, function() local _ = string.gsub(big, "a", "bb") end)
        local parts = table.create(100, "x")
        cost("table.concat separator", 1000, function() local _ = table.concat(parts, big) end)
        cost("table.insert at front", 4000, function() table.insert(list, 1, 0) end)
        cost("table.remove at front", 4000, function() table.remove(list, 1) end)
        local toClear = table.create(40000, 1)
        cost("table.clear", 2000, function() table.clear(toClear) end)
        cost("table.maxn", 2000, function() local _ = table.maxn(list) end)
        cost("buffer.create", 250, function() local _ = buffer.create(64000) end)
        cost("buffer.fromstring", 1000, function() local _ = buffer.fromstring(big) end)
        cost("buffer.tostring", 1000, function() local _ = buffer.tostring(buf) end)
        cost("buffer.readstring", 1000, function() local _ = buffer.readstring(buf, 0, 64000) end)
        cost("buffer.writestring", 1000, function() buffer.writestring(buf, 0, big) end)
        cost("utf8.len", 1000, function() local _ = utf8.len(big) end)
        -- The O(1) cases stay cheap: an append keeps its FASTCALL, a short sub is charged per byte.
        local f0 = task.fuel()
        for i = 1, 100 do table.insert(list, i) end
        local appendCost = task.fuel() - f0
        assert(appendCost < 400, "table.insert append charged " .. appendCost)
        f0 = task.fuel()
        local _ = big:sub(1, 10)
        assert(task.fuel() - f0 < 10, "short string.sub overcharged")
    )");
}

TEST_CASE("fuel: a pure builtin whose result trips fuel_kill is killed right after the call") {
    Harness h;
    const TaskId id = h.run("resultkill", R"(
        local b = buffer.create(1000000) -- ~4000 fuel
        local s = buffer.tostring(b)     -- a 1 MB result: ~16000 fuel, charged after the call
        print("unreachable", #s)
    )");
    h.steps(3);
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.fuel == 10'000);
    CHECK(h.prints.empty());
}

namespace {
// Runs `source` (which must bump `require("probe").n` once per iteration of a loop whose body is one
// large allocation) until the wall limit kills it. Returns how many iterations ran past the limit,
// estimated from the measured time per iteration: machine-speed independent.
double iterationsPastLimit(VmConfig config, u64 limitNanos, KillReason expected) {
    config.budget.fuelKill = 0;
    config.budget.fuelPerResume = 0;
    config.budget.fuelPerTick = 0;
    Harness h(config);
    h.load("probe", "return { n = 0 }");
    const TaskId id = h.run("allocloop", R"(
        local probe = require("probe")
        local big = string.rep("x", 2000000)
        while true do
            local s = big .. "y" -- one 2 MB allocation per iteration, one safepoint
            probe.n += 1
        end
    )");
    const TickStats ts = h.step();
    const RecordedEvent* e = h.eventFor(id, ScriptEventKind::TaskKilled);
    REQUIRE(e != nullptr);
    CHECK(e->killReason == expected);
    double n = 0;
    h.load("reader", "return { n = function() return require('probe').n end }");
    REQUIRE(h.vm
                ->callExport(
                    "reader", "n", {}, [&](lua_State* L, int base, int) { n = lua_tonumber(L, base); }, 1)
                .ok());
    const double wall = static_cast<double>(ts.wallNanos);
    const double limit = static_cast<double>(limitNanos);
    const double past = wall > limit ? n * (wall - limit) / wall : 0.0;
    MESSAGE("killed after ", wall / 1e6, " ms and ", n, " iterations (", past, " past the limit)");
    return past;
}
} // namespace

TEST_CASE("fuel: wall limits fire promptly when single operations are slow (GC steps force a clock read)") {
    // The clock is read every 64 fuel; a loop whose every iteration is one multi-MB allocation used to
    // run ~64 iterations (hundreds of ms) past the 20 ms backstop. GC steps now force a read at the
    // next safepoint.
    // Best of three attempts: the overshoot before the fix was structural (~64 iterations every
    // time), while a descheduled test thread can inflate a single measurement.
    auto best = [](const VmConfig& config, u64 limit, KillReason reason) {
        double past = 1e9;
        for (int attempt = 0; attempt < 3 && past > 8.0; ++attempt)
            past = std::min(past, iterationsPastLimit(config, limit, reason));
        return past;
    };
    VmConfig cell = Harness::defaultConfig();
    cell.budget.wallBackstopNanos = 20'000'000;
    CHECK(best(cell, 20'000'000, KillReason::WallBackstop) <= 8.0);

    VmConfig client = Harness::defaultConfig();
    client.profile = HostProfile::Client;
    client.budget = FuelBudget::client();
    CHECK(best(client, 5'000'000, KillReason::WallBudget) <= 8.0);
}

TEST_CASE("fuel: unbounded recursion through bindings and require fails cleanly (C stack guard)") {
    // Every binding→Luau→binding level adds C++ frames; Luau's LUAI_MAXCCALLS turns the recursion
    // into an ordinary error long before a 1 MiB (Windows default) thread stack runs out.
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelKill = 0;
    c.budget.fuelPerResume = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c, [](Binder& b) {
        b.function(
            "Test", "call",
            [](lua_State* L) -> int {
                luaL_checktype(L, 1, LUA_TFUNCTION);
                lua_pushvalue(L, 1);
                lua_pushboolean(L, callLuau(L, 0, 0) == LUA_OK);
                return 1;
            },
            FuelCost{1, 0, 0});
    });
    h.expectRuns("deep", R"(
        local depth = 0
        local function dive()
            depth += 1
            if not Test.call(dive) then error("callback failed at depth " .. depth, 0) end
        end
        local ok, err = pcall(dive)
        assert(not ok and depth > 10 and depth < 1000, tostring(err))
    )");
    h.load("selfreq", "return require('selfreq2')");
    h.load("selfreq2", "return require('selfreq')");
    h.expectRuns("cycle", "assert(not pcall(require, 'selfreq'))");
}

TEST_CASE("fuel: every resume pays resumeCost, so trivial resumes cannot flood the lane") {
    // A task that only yields costs ~3 safepoints per resume but a few hundred ns of scheduler and
    // lua_resume work; without the fixed charge a lane of such tasks spent far more wall time than
    // its fuel said.
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 64u << 20;
    Harness h(c);
    h.load("spinner", "while true do task.yield() end");
    for (u64 owner = 1; owner <= 3000; ++owner) h.spawn("spinner", owner);
    h.step(); // first resumes
    const TickStats ts = h.step();
    const FuelBudget& b = h.vm->config().budget;
    CHECK(ts.fuel <= b.fuelPerTick + b.fuelKill);
    CHECK(ts.resumed <= b.fuelPerTick / b.resumeCost + 1);
    CHECK(ts.deferred > 0);
    CHECK(ts.fuel >= static_cast<u64>(ts.resumed) * b.resumeCost);
}

TEST_CASE("fuel: the inline counter counts exactly what per-safepoint counting did") {
    // Fuel = gc < 0 safepoints + resumeCost, as when the host was called at every safepoint (the
    // golden counts in test_determinism pin the same for charged builtins). The cell interpreter,
    // and the editor profile (cells refuse codegen) with the interpreter and native code.
    struct Mode {
        HostProfile profile;
        bool native;
    };
    for (const Mode mode : {Mode{HostProfile::Cell, false}, Mode{HostProfile::Editor, false},
                            Mode{HostProfile::Editor, true}}) {
        const bool native = mode.native;
        if (native && !luau_codegen_supported()) continue;
        CAPTURE(native);
        CAPTURE(static_cast<int>(mode.profile));
        const u64 safepoints = rawSafepoints(kSafepointsOnly, native);
        VmConfig c = Harness::defaultConfig();
        c.budget.fuelPerResume = 0;
        c.budget.fuelKill = 0;
        c.profile = mode.profile;
        c.enableNativeCodegen = native;
        Harness h(c);
        ModuleOptions options;
        options.native = native;
        h.load("safepoints", kSafepointsOnly, options);
        const TaskId id = h.spawn("safepoints");
        h.step();
        const RecordedEvent* done = h.eventFor(id, ScriptEventKind::TaskFinished);
        REQUIRE(done != nullptr);
        CHECK(done->fuel == safepoints + c.budget.resumeCost);
    }
}

TEST_CASE("fuel: the host runs only at decision points (inline fuel counter)") {
    const u64 safepoints = rawSafepoints(kSafepointsOnly, false);
    REQUIRE(safepoints > 20'000);
    SUBCASE("no limits: the host is never called") {
        VmConfig c = Harness::defaultConfig();
        c.budget.fuelPerResume = 0;
        c.budget.fuelKill = 0;
        Harness h(c);
        instrumentInterrupt(*h.vm);
        h.expectRuns("free", kSafepointsOnly);
        CHECK(g_hostCalls == 0);
        // No run is active: the counter is parked out of reach.
        CHECK(*lua_fuelcounter(h.vm->state()) ==
              static_cast<i64>(helios::script::detail::kMaxCounterDistance));
    }
    SUBCASE("soft budget and kill: one call each, at the exact fuel") {
        VmConfig c = Harness::defaultConfig();
        c.budget.fuelPerResume = 1'000;
        c.budget.fuelKill = 5'000;
        Harness h(c);
        instrumentInterrupt(*h.vm);
        const TaskId id = h.run("capped", kSafepointsOnly);
        h.step();
        const RecordedEvent& e = requireKilled(h, id);
        CHECK(e.fuel == 5'000);
        const RecordedEvent* over = h.last(ScriptEventKind::OverBudget);
        REQUIRE(over != nullptr);
        CHECK(over->fuel == 1'000);
        CHECK(g_hostCalls == 2);
    }
    SUBCASE("wall limits: one call per clock read (every 64 fuel)") {
        VmConfig c = Harness::defaultConfig();
        c.profile = HostProfile::Client;
        c.budget = FuelBudget::client();
        c.budget.wallKillNanos = 0; // a slow CI machine must not kill the run
        c.budget.wallBackstopNanos = 0;
        c.budget.wallPerTickNanos = 0;
        Harness h(c);
        instrumentInterrupt(*h.vm);
        const TaskId id = h.run("walled", kSafepointsOnly);
        h.step();
        const RecordedEvent* done = h.eventFor(id, ScriptEventKind::TaskFinished);
        REQUIRE(done != nullptr);
        CHECK(done->fuel == safepoints + c.budget.resumeCost);
        // One call per clock read; a GC step may force an early read, which restarts the cadence.
        const u64 reads = safepoints / helios::script::detail::kWallCheckInterval;
        CHECK(g_hostCalls + 8 >= reads);
        CHECK(g_hostCalls <= reads + 8);
    }
}

// ---------------------------------------------------------------------------------------------
// Counter bookkeeping (review round 1 of WP-0.10r): every place the host commits or re-arms the
// inline counter must leave fuel exactly as per-safepoint counting did. The cases run in wall mode
// (a wall limit makes the host read the clock every 64 fuel, before every binding and after every
// GC step, as a cell's 20 ms backstop does) and around decision points.
// ---------------------------------------------------------------------------------------------
namespace {

u64 g_probeFuel = 0;
int g_probeCalls = 0;

int tProbe(lua_State* L) {
    ++g_probeCalls;
    g_probeFuel = currentFuel(L); // includes this call's charge
    return 0;
}

int pCharge(lua_State* L) {
    chargeFuel(L, static_cast<u64>(luaL_checkinteger(L, 1)));
    return 0;
}

int pItems(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(lua_objlen(L, 1)));
    return 1;
}

int pHuge(lua_State* L) {
    chargeFuel(L, ~u64(0)); // saturates the run's fuel
    return 0;
}

ScriptVm::ApiRegistrar counterApi() {
    return [](Binder& b) {
        b.function("P", "charge", &pCharge, FuelCost{1, 0, 0});
        b.function("P", "items", &pItems, FuelCost{1, 1'000, 1});
        b.function("P", "huge", &pHuge, FuelCost{0, 0, 0});
    };
}

// Safepoints, allocation (GC steps), per-item binding charges and host charges of varying size.
constexpr const char* kMixed = R"lua(
    local keep = {}
    for i = 1, 300 do
        keep[i % 50 + 1] = {i, tostring(i), string.rep("z", i % 40)}
        P.items(keep)
        P.charge(i % 7)
    end
    local s = "" for i = 1, 100 do s = s .. "x" end
    print(task.fuel())
)lua";

u64 finishedFuel(const VmConfig& c, std::string* printed = nullptr) {
    Harness h(c, counterApi());
    const TaskId id = h.run("mixed", kMixed);
    h.steps(5);
    const RecordedEvent* done = h.eventFor(id, ScriptEventKind::TaskFinished);
    REQUIRE(done != nullptr);
    REQUIRE(h.prints.size() == 1);
    if (printed) *printed = h.prints[0];
    return done->fuel;
}

VmConfig unlimited() {
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    return c;
}

} // namespace

TEST_CASE("fuel: GC-forced clock reads do not change the fuel count (counter armed at 0)") {
    constexpr const char* kAllocating = R"(
        local keep = {}
        for i = 1, 3000 do
            keep[i % 16 + 1] = { i, i + 1, i + 2, { i } }
        end
    )";
    auto fuelWith = [&](u64 backstop) {
        VmConfig c = unlimited();
        c.budget.wallBackstopNanos = backstop;
        Harness h(c);
        const TaskId id = h.run("alloc", kAllocating);
        h.step();
        const RecordedEvent* done = h.eventFor(id, ScriptEventKind::TaskFinished);
        REQUIRE(done != nullptr);
        return done->fuel;
    };
    CHECK(fuelWith(10'000'000'000ull) == fuelWith(0));
}

TEST_CASE("fuel: a charge landing exactly on fuel_kill kills before the call (counter fast path)") {
    VmConfig c = unlimited();
    const ScriptVm::ApiRegistrar api = [](Binder& b) {
        b.function("Test", "probe", &tProbe, FuelCost{500, 0, 0}, nullptr);
    };
    const char* src = "for i = 1, 10 do end\nTest.probe()\nfor i = 1, 10 do end";
    u64 atCall = 0;
    {
        Harness h(c, api);
        g_probeCalls = 0;
        h.expectRuns("probe", src);
        REQUIRE(g_probeCalls == 1);
        atCall = g_probeFuel;
    }
    c.budget.fuelKill = atCall; // the probe's own charge reaches fuel_kill exactly
    Harness h(c, api);
    g_probeCalls = 0;
    const TaskId id = h.run("probe", src);
    h.steps(3);
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.fuel == atCall);
    CHECK(g_probeCalls == 0); // the tripping charge has no side effect
}

TEST_CASE("fuel: cheap binding calls count the same fuel with and without clock reads") {
    Fixture f;
    auto fuelWith = [&](u64 backstop) {
        VmConfig c = unlimited();
        c.budget.wallBackstopNanos = backstop;
        Harness h(c, testApi(f));
        const TaskId id = h.run("cheap", "local t = {} for i = 1, 500 do Test.items(t) end");
        h.step();
        const RecordedEvent* done = h.eventFor(id, ScriptEventKind::TaskFinished);
        REQUIRE(done != nullptr);
        return done->fuel;
    };
    const u64 walled = fuelWith(10'000'000'000ull);
    const u64 free = fuelWith(0);
    CHECK(free > 1'000);
    CHECK(walled == free);
}

TEST_CASE("fuel: a top-level run (callExport) reports the fuel the counter counted") {
    Harness h(unlimited());
    h.load("m", "return { f = function() local n = 0 while n < 5000 do n += 1 end error('boom') end }");
    REQUIRE(h.vm->instantiateModule("m").ok());
    const u64 before = h.vm->stats().fuelTotal;
    CHECK_FALSE(h.vm->callExport("m", "f", {}, {}, 0).ok());
    const RecordedEvent* failed = h.last(ScriptEventKind::TaskFailed);
    REQUIRE(failed != nullptr);
    CHECK(failed->fuel >= 5'000);
    CHECK(h.vm->stats().fuelTotal - before == failed->fuel);
}

TEST_CASE("fuel: a kill swallowed by pcall is re-raised at exactly fuel_kill") {
    Harness h;
    const TaskId id = h.run("pk", "for i = 1, 100 do pcall(function() while true do end end) end");
    h.step();
    const RecordedEvent& e = requireKilled(h, id);
    CHECK(e.fuel == Harness::defaultConfig().budget.fuelKill);
}

TEST_CASE("fuel: wall-clock reads and budget decision points never change fuel") {
    const VmConfig base = unlimited();
    std::string refPrint;
    const u64 ref = finishedFuel(base, &refPrint);
    VmConfig wall = base; // clock read every 64 fuel, before every binding, after every GC step
    wall.budget.wallSoftNanos = 1'000'000'000'000ull;
    wall.budget.wallKillNanos = 2'000'000'000'000ull;
    wall.budget.wallBackstopNanos = 3'000'000'000'000ull;
    std::string wallPrint;
    CHECK(finishedFuel(wall, &wallPrint) == ref);
    CHECK(wallPrint == refPrint);
    for (u64 soft = 1; soft <= 200; ++soft) { // some charge lands exactly on the decision point
        CAPTURE(soft);
        VmConfig c = base;
        c.budget.fuelPerResume = soft;
        CHECK(finishedFuel(c) == ref);
    }
}

TEST_CASE("fuel: a saturated charge on a host without fuel_kill never wraps the fuel count") {
    // A charge that saturates the run's fuel trips the kill even with fuel_kill disabled. Later
    // safepoints of the killed run (a pcall caught the first raise) must not wrap the count to 0,
    // and neither may the lane and VM totals.
    Harness h(unlimited(), counterApi());
    h.load("huge", "pcall(P.huge) for i = 1, 10 do end");
    const TaskId a = h.spawn("huge", 1);
    const TaskId b = h.spawn("huge", 2);
    const TickStats ts = h.step();
    for (const TaskId id : {a, b}) {
        const RecordedEvent& e = requireKilled(h, id);
        CHECK(e.killReason == KillReason::Fuel);
        CHECK(e.fuel == ~u64(0));
    }
    CHECK(ts.fuel == ~u64(0));
    CHECK(h.vm->stats().fuelTotal == ~u64(0));
}
