// Scheduler: (wake tick, EntityId, seq) ordering, wait() on the dilatable zone clock, the task
// library, asynchronous host calls with continuations, cancellation, yields across pcall, refusal
// to yield where it would be involuntary, and 10k coroutines per zone.

#include <algorithm>
#include <string>

#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

struct AsyncFixture {
    std::vector<AsyncToken> pending;
    std::vector<std::string> keys;
};

int tFetch(lua_State* L) {
    auto* f = static_cast<AsyncFixture*>(bindingUserdata(L));
    const char* key = luaL_checkstring(L, 1);
    const AsyncToken token = beginAsync(L);
    f->pending.push_back(token);
    f->keys.emplace_back(key);
    return yieldAsync(L);
}

// Test.cached(fail): an async call whose host operation completes (or fails) synchronously, before
// the binding yields (a cache hit, an immediately rejected request).
struct SyncFixture {
    ScriptVm* vm = nullptr;
    int completions = 0;
};

int tCached(lua_State* L) {
    auto* f = static_cast<SyncFixture*>(bindingUserdata(L));
    const bool fail = lua_toboolean(L, 1) != 0;
    const AsyncToken token = beginAsync(L);
    const bool ok = fail ? f->vm->failAsync(token, "rejected at once")
                         : f->vm->completeAsync(token, [](lua_State* S) {
                               lua_pushstring(S, "cached");
                               return 1;
                           });
    f->completions += ok ? 1 : 0;
    CHECK_FALSE(f->vm->completeAsync(token)); // a token completes at most once
    return yieldAsync(L);
}

// Test.visit(fn): an async binding that (wrongly) calls back into Luau before yielding. The callback
// must not be able to yield the task through the binding's C frame.
int tVisit(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    const int status = callLuau(L, 0, 0);
    lua_pushinteger(L, status);
    if (status != LUA_OK) {
        lua_insert(L, -2); // [status, error]
        return 2;
    }
    return 1;
}

std::string callString(Harness& h, const char* module, const char* fn) {
    std::string out;
    const auto r = h.vm->callExport(
        module, fn, {},
        [&](lua_State* L, int base, int) { out = lua_tostring(L, base) ? lua_tostring(L, base) : ""; }, 1);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    return out;
}

void loadOrderer(Harness& h) {
    h.load("orderlog", "return { log = {} }");
    h.load("orderer", R"(
        local shared = require("orderlog")
        local M = {}
        function M.run(name, delay)
            if delay > 0 then wait(delay) end
            table.insert(shared.log, name)
        end
        function M.dump() return table.concat(shared.log, ",") end
        return M
    )");
}

void spawnNamed(Harness& h, u64 owner, const char* name, double delay) {
    const auto r = h.vm->spawnExport("orderer", "run", owner, [=](lua_State* L) {
        lua_pushstring(L, name);
        lua_pushnumber(L, delay);
        return 2;
    });
    REQUIRE(r.ok());
}

} // namespace

TEST_CASE("scheduler: tasks resume in (wake tick, owner EntityId, sequence) order") {
    Harness h;
    loadOrderer(h);
    spawnNamed(h, 3, "a3", 0);
    spawnNamed(h, 1, "b1", 0);
    spawnNamed(h, 2, "c2", 0);
    spawnNamed(h, 1, "d1", 0);
    h.step();
    CHECK(callString(h, "orderer", "dump") == "b1,d1,c2,a3");

    // Timers: the earlier deadline wakes on an earlier tick regardless of owner; equal wake ticks
    // are ordered by owner.
    spawnNamed(h, 1, "late1", 0.2);
    spawnNamed(h, 9, "early9", 0.05);
    spawnNamed(h, 5, "late5", 0.2);
    h.steps(20);
    CHECK(callString(h, "orderer", "dump") == "b1,d1,c2,a3,early9,late1,late5");
}

TEST_CASE("scheduler: spawned tasks first run on the next tick") {
    Harness h;
    const TaskId id = h.run("later", "wait(0)");
    CHECK(h.vm->taskState(id) == TaskState::Ready);
    h.step();
    CHECK(h.vm->taskState(id) == TaskState::Waiting);
    h.step();
    CHECK(h.vm->taskState(id) == TaskState::Gone);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished));
}

TEST_CASE("scheduler: wait() runs on zone time, so time dilation stretches it in wall time") {
    auto stepsFor = [](double scale, std::string& printed) {
        Harness h;
        h.clock.setScale(scale);
        const TaskId id = h.run("sleeper", "local e = wait(1)\nprint(e, task.now())");
        int n = 0;
        while (h.vm->taskState(id) != TaskState::Gone && n < 1000) {
            h.step();
            ++n;
        }
        REQUIRE(h.prints.size() == 1);
        printed = h.prints[0];
        return n;
    };
    std::string normal;
    std::string dilated;
    const int n1 = stepsFor(1.0, normal);
    const int n2 = stepsFor(0.5, dilated);
    CHECK(n1 == 21);         // 1 s of zone time = 20 steps of 50 ms, plus the start tick
    CHECK(n2 >= 2 * n1 - 2); // at 50% dilation each zone second takes two wall seconds
    CHECK(n2 <= 2 * n1 + 1);
    CHECK(normal.rfind("1\t", 0) == 0); // wait returns the elapsed zone seconds (exactly 1)
    CHECK(dilated.rfind("1\t", 0) == 0);
}

TEST_CASE("scheduler: task library — spawn, status, cancel, current, yield across pcall") {
    Harness h;
    h.expectRuns("tasklib", R"(
        local log = {}
        local t = task.spawn(function(a, b)
            table.insert(log, a + b)
            wait(10)
            table.insert(log, "never")
        end, 1, 2)
        assert(typeof(t) == "Task")
        assert(task.status(t) == "ready")
        task.yield() -- t was scheduled first, so it runs before we resume
        assert(log[1] == 3)
        assert(task.status(t) == "waiting")
        assert(task.cancel(t) == true)
        assert(task.status(t) == "dead")
        assert(task.cancel(t) == false)
        local me = task.current()
        assert(me ~= nil and me ~= t and task.status(me) == "running")
        assert(task.cancel(me) == false, "a running task cannot be cancelled")
        -- wait() inside pcall yields the task (pcall is yieldable).
        local ok, e = pcall(function() return wait(0.1) end)
        assert(ok and math.abs(e - 0.1) < 1e-9)
        assert(#log == 1)
    )");
    CHECK(h.count(ScriptEventKind::TaskCancelled) == 1);
}

TEST_CASE("scheduler: yields are refused where they would be involuntary") {
    Harness h;
    h.load("waiter", "wait(0)\nreturn {}");
    h.expectRuns("refuse", R"(
        local ok, err = pcall(function()
            table.sort({2, 1}, function(a, b) wait(0) return a < b end)
        end)
        assert(not ok and err.code == "InvalidState", "yield inside a sort comparator")
        local ok2, err2 = pcall(function()
            return setmetatable({}, {__index = function() wait(0) end}).x
        end)
        assert(not ok2 and err2.code == "InvalidState", "yield inside a metamethod")
        local ok3, err3 = pcall(coroutine.wrap(function() wait(0) end))
        assert(not ok3, "wait inside a nested coroutine")
        local ok4, err4 = pcall(require, "waiter")
        assert(not ok4 and err4.code == "InvalidState", "wait at a module's top level")
    )");
    CHECK(h.vm->stats().involuntaryYields == 0);
}

TEST_CASE("scheduler: asynchronous host calls resume with results or raise HostError") {
    AsyncFixture f;
    Harness h(Harness::defaultConfig(),
              [&f](Binder& b) { b.asyncFunction("Test", "fetch", &tFetch, FuelCost{2, 0, 0}, &f); });
    const TaskId id = h.run("fetcher", R"(
        local v, n = Test.fetch("alpha")
        assert(v == "alpha!" and n == 42)
        local ok, err = pcall(Test.fetch, "fail")
        assert(not ok and err.code == "HostError" and string.find(err.message, "backend down", 1, true))
        print("done")
    )");
    h.step();
    REQUIRE(f.pending.size() == 1);
    CHECK(f.keys[0] == "alpha");
    CHECK(h.vm->taskState(id) == TaskState::Awaiting);
    h.step(); // nothing completes by itself
    CHECK(h.vm->taskState(id) == TaskState::Awaiting);
    CHECK(h.vm->completeAsync(f.pending[0], [](lua_State* L) {
        lua_pushstring(L, "alpha!");
        lua_pushnumber(L, 42);
        return 2;
    }));
    CHECK_FALSE(h.vm->completeAsync(f.pending[0])); // double completion is refused
    h.step();
    REQUIRE(f.pending.size() == 2);
    CHECK(h.vm->failAsync(f.pending[1], "backend down"));
    h.step();
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished));
    REQUIRE(h.prints.size() == 1);
    CHECK(h.vm->stats().yieldsByReason[static_cast<usize>(YieldReason::Async)] == 2);

    // Cancelling an awaiting task invalidates its token.
    const TaskId c = h.run("cancelled", "Test.fetch('x')\nprint('unreachable')");
    h.step();
    REQUIRE(f.pending.size() == 3);
    CHECK(h.vm->cancelTask(c));
    CHECK(h.vm->taskState(c) == TaskState::Gone);
    CHECK_FALSE(h.vm->completeAsync(f.pending[2]));
    CHECK_FALSE(h.vm->cancelTask(c));
    h.steps(3);
    CHECK(h.prints.size() == 1);
}

TEST_CASE("scheduler: the host cancels waiting tasks and ids go stale") {
    Harness h;
    const TaskId id = h.run("sleepy", "wait(100)\nprint('unreachable')");
    h.step();
    CHECK(h.vm->taskState(id) == TaskState::Waiting);
    CHECK(h.vm->cancelTask(id));
    CHECK(h.vm->taskState(id) == TaskState::Gone);
    CHECK_FALSE(h.vm->cancelTask(id));
    const auto* e = h.eventFor(id, ScriptEventKind::TaskCancelled);
    REQUIRE(e != nullptr);
    CHECK(e->module == "sleepy");
    h.steps(5);
    CHECK(h.prints.empty());
    CHECK(h.vm->stats().tasksCancelled == 1);
}

TEST_CASE("scheduler: spawned tasks inherit the owner EntityId of their parent") {
    Harness h;
    const TaskId parent = h.run("parent", "task.spawn(function() wait(0) end)", 777);
    h.steps(10);
    CHECK(h.eventFor(parent, ScriptEventKind::TaskFinished)->owner == 777);
    CHECK(h.count(ScriptEventKind::TaskFinished) == 2);
    for (const auto& e : h.events) CHECK(e.owner == 777);
}

TEST_CASE("scheduler: 10k coroutines per zone fit a 16 MB VM heap") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 16u << 20;
    c.budget.fuelPerTick = 1'000'000;
    Harness h(c);
    h.load("counter", "return { n = 0 }");
    h.load("many", R"(
        local counter = require("counter")
        return { run = function(i) wait(0.1) counter.n += 1 end }
    )");
    REQUIRE(h.vm->instantiateModule("many").ok());
    const usize before = h.vm->stats().heapBytes;
    constexpr int kTasks = 10'000;
    for (int i = 0; i < kTasks; ++i) {
        const auto r = h.vm->spawnExport("many", "run", static_cast<u64>(i), [i](lua_State* L) {
            lua_pushnumber(L, i);
            return 1;
        });
        REQUIRE(r.ok());
    }
    CHECK(h.vm->liveTaskCount() == kTasks);
    const usize perTask = (h.vm->stats().heapBytes - before) / kTasks;
    MESSAGE("heap per idle task: ", perTask, " bytes");
    CHECK(perTask < 1536);
    h.steps(50);
    CHECK(h.vm->liveTaskCount() == 0);
    CHECK(h.count(ScriptEventKind::TaskFinished) == kTasks);
    std::string n;
    h.load("reader", "return { get = function() return tostring(require('counter').n) end }");
    n = callString(h, "reader", "get");
    CHECK(n == "10000");
    const VmStats s = h.vm->stats();
    MESSAGE("heap peak with 10k tasks: ", s.heapPeakBytes / 1024, " KiB of ", s.heapLimitBytes / 1024,
            " KiB");
    CHECK(s.heapPeakBytes <= s.heapLimitBytes);
    CHECK(s.allocationFailures == 0);
}

TEST_CASE("scheduler: an async call completed synchronously resumes on the next tick") {
    SyncFixture f;
    Harness h(Harness::defaultConfig(),
              [&f](Binder& b) { b.asyncFunction("Test", "cached", &tCached, FuelCost{1, 0, 0}, &f); });
    f.vm = h.vm.get();
    const TaskId id = h.run("sync", R"(
        local v = Test.cached(false)
        assert(v == "cached", "result of a synchronous completion")
        local ok, err = pcall(Test.cached, true)
        assert(not ok and err.code == "HostError" and string.find(err.message, "rejected at once", 1, true))
        print("done")
    )");
    h.step();
    CHECK(h.vm->taskState(id) == TaskState::Ready); // yielded (explicitly), not left awaiting forever
    h.steps(5);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished) != nullptr);
    CHECK(h.prints == std::vector<std::string>{"done"});
    CHECK(f.completions == 2);
    CHECK(h.vm->stats().yieldsByReason[static_cast<usize>(YieldReason::Async)] == 2);
    CHECK(h.vm->stats().involuntaryYields == 0);
}

TEST_CASE("scheduler: a callback made from an async binding cannot yield the task") {
    Harness h(Harness::defaultConfig(),
              [](Binder& b) { b.asyncFunction("Test", "visit", &tVisit, FuelCost{1, 0, 0}); });
    const TaskId id = h.run("visitor", R"(
        local after = 0
        local status, err = Test.visit(function() wait(0.1) print("inner resumed") end)
        after += 1
        assert(status ~= 0 and err.code == "InvalidState", "wait() inside the callback must be refused")
        assert(Test.visit(function() end) == 0)
        print("after", after)
    )");
    h.steps(10);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished) != nullptr);
    // The code after the binding ran exactly once (a yield through the binding's C frame made it run
    // twice and dropped the callback's continuation).
    CHECK(h.prints == std::vector<std::string>{"after\t1"});
    CHECK(h.vm->stats().involuntaryYields == 0);
}

TEST_CASE("scheduler: task.spawn forwards thousands of arguments") {
    Harness h;
    h.expectRuns("manyargs", R"(
        local args = table.create(7900, 3)
        task.spawn(function(...)
            assert(select("#", ...) == 7900 and select(7900, ...) == 3)
            print("child ran")
        end, table.unpack(args))
        wait(0)
    )");
    h.steps(3);
    CHECK(h.prints == std::vector<std::string>{"child ran"});
    CHECK(h.count(ScriptEventKind::TaskFailed) == 0);
}

TEST_CASE("scheduler: cancelled waits do not accumulate queue entries") {
    // A cancelled wait(1e9) used to keep its timer entry until the deadline: a script spawning and
    // cancelling waiters grew the host-side timer heap without bound.
    Harness h;
    h.run("churn", R"(
        local batch = {}
        while true do
            for i = 1, 200 do batch[i] = task.spawn(function() wait(1e9) end) end
            wait(0)
            for i = 1, 200 do task.cancel(batch[i]) end
        end
    )");
    usize maxQueued = 0;
    for (int i = 0; i < 300; ++i) {
        h.step();
        maxQueued = std::max(maxQueued, h.vm->stats().queuedEntries);
    }
    const VmStats s = h.vm->stats();
    CHECK(s.tasksCancelled > 20'000);
    MESSAGE("max queued entries ", maxQueued, " for ", s.tasksCancelled, " cancelled waiters");
    CHECK(maxQueued <= 2 * 201 + 256 + 200);
}
