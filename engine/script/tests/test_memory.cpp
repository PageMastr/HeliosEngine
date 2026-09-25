// Heap caps: the per-VM hard cap, per-module caps through memory categories, memory-tag
// attribution, and the VM staying usable after an allocation failure.

#include "helios/core/memory.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

TEST_CASE("memory: exceeding the VM heap cap raises a memory error and the VM recovers") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 2u << 20;
    c.budget.fuelKill = 0; // let the allocation loop run into the cap, not the budget
    c.budget.fuelPerResume = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    const TaskId hog = h.run("hog", R"(
        local keep = {}
        for i = 1, 1e7 do keep[i] = string.rep("x", 64) .. i end
    )");
    h.steps(3);
    const auto* e = h.eventFor(hog, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    REQUIRE(e->error.has_value());
    CHECK(e->error->code == ScriptErrorCode::OutOfMemory);
    VmStats s = h.vm->stats();
    CHECK(s.allocationFailures > 0);
    CHECK(s.heapPeakBytes <= c.heapLimitBytes);

    // pcall catches the memory error inside the script; the task then continues.
    h.expectRuns("catcher", R"(
        local ok, err = pcall(function()
            local keep = {}
            for i = 1, 1e7 do keep[i] = table.create(64, i) end
        end)
        assert(not ok)
        assert(string.find(tostring(err), "not enough memory", 1, true))
    )");
    h.vm->collectGarbage();
    s = h.vm->stats();
    CHECK(s.heapBytes < c.heapLimitBytes / 2);
    h.expectRuns("after", "local t = table.create(1000, 1) assert(#t == 1000)");
}

TEST_CASE("memory: per-module caps isolate a greedy module") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 32u << 20;
    c.moduleHeapLimitBytes = 1u << 20;
    c.budget.fuelKill = 0;
    c.budget.fuelPerResume = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    h.load("greedy", R"(
        GREEDY = {}
        for i = 1, 1e6 do GREEDY[i] = table.create(16, i) end
    )");
    h.load("modest", R"(
        local keep = {}
        for i = 1, 1000 do keep[i] = table.create(16, i) end
        assert(#keep == 1000)
        wait(0)
        assert(#keep == 1000)
    )");
    const TaskId g = h.spawn("greedy");
    const TaskId m = h.spawn("modest");
    h.steps(10);
    const auto* ge = h.eventFor(g, ScriptEventKind::TaskFailed);
    REQUIRE(ge != nullptr);
    CHECK(ge->error->code == ScriptErrorCode::OutOfMemory);
    CHECK(h.eventFor(m, ScriptEventKind::TaskFinished) != nullptr);
    const auto greedy = h.vm->moduleInfo("greedy");
    const auto modest = h.vm->moduleInfo("modest");
    REQUIRE(greedy);
    REQUIRE(modest);
    CHECK(greedy->memoryCategory != modest->memoryCategory);
    CHECK(h.vm->stats().heapPeakBytes < 8u << 20); // far below the VM cap: the module cap tripped
}

TEST_CASE("memory: the VM heap is attributed to its memory tag and fully released") {
    const MemoryTag tag = registerMemoryTag("ScriptTestTag");
    const i64 baseline = memoryTagStats(tag).liveBytes;
    {
        VmConfig c = Harness::defaultConfig();
        c.memoryTag = tag;
        Harness h(c);
        h.expectRuns("alloc", "local t = table.create(10000, 'x') assert(#t == 10000)");
        const i64 live = memoryTagStats(tag).liveBytes - baseline;
        CHECK(live > 0);
        CHECK(static_cast<usize>(live) >= h.vm->stats().heapBytes); // the tag also counts headers
    }
    CHECK(memoryTagStats(tag).liveBytes == baseline);
}

TEST_CASE("memory: VM creation validates the heap cap") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 1024;
    const auto tooSmall = ScriptVm::create(c);
    REQUIRE_FALSE(tooSmall.ok());
    CHECK(tooSmall.error().code == ErrorCode::InvalidArgument);

    c.heapLimitBytes = 256u << 10;
    auto vm = ScriptVm::create(c);
    if (!vm.ok()) {
        // Too small for the standard libraries: a clean error, never a crash.
        CHECK(vm.error().code == ErrorCode::OutOfMemory);
    } else {
        CHECK((*vm)->stats().heapBytes <= c.heapLimitBytes);
    }
}

TEST_CASE("memory: tasks and coroutines allocate in the right module category") {
    VmConfig c = Harness::defaultConfig();
    c.heapLimitBytes = 32u << 20;
    c.moduleHeapLimitBytes = 1u << 20;
    c.budget.fuelKill = 0;
    c.budget.fuelPerResume = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    // A task spawned while another module's chunk runs (inside require) belongs to the spawning
    // task's module; its thread used to inherit the required module's category, so the cap was
    // checked against a category that never grew.
    h.load("lib", R"(
        task.spawn(function()
            local keep = {}
            for i = 1, 1e6 do keep[i] = table.create(16, i) end
        end)
        return {}
    )");
    const TaskId host = h.run("host", "require('lib')\nwait(1)");
    h.steps(5, false);
    const auto* failed = h.last(ScriptEventKind::TaskFailed);
    REQUIRE(failed != nullptr);
    CHECK(failed->error->code == ScriptErrorCode::OutOfMemory);
    CHECK(h.vm->stats().heapPeakBytes < (8u << 20));
    CHECK(h.vm->taskState(host) != TaskState::Gone); // the host's own task is unaffected

    // A coroutine created by one module and resumed by another allocates on the resumer's behalf.
    h.load("gen", R"(
        local keep = {}
        local co = coroutine.create(function()
            while true do
                for i = 1, 1000 do keep[#keep + 1] = table.create(16, i) end
                coroutine.yield()
            end
        end)
        return { co = co }
    )");
    const TaskId driver = h.run("driver", R"(
        local co = require("gen").co
        while true do
            local ok, err = coroutine.resume(co)
            if not ok then error(err) end
        end
    )");
    h.steps(5, false);
    const auto* e = h.eventFor(driver, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    // Either the rethrown "not enough memory" message or the driver's own allocation failing.
    CHECK((e->error->code == ScriptErrorCode::OutOfMemory || e->error->code == ScriptErrorCode::Runtime));
    CHECK(e->error->message.find("not enough memory") != std::string::npos);
    CHECK(h.vm->stats().heapPeakBytes < (8u << 20));
}
