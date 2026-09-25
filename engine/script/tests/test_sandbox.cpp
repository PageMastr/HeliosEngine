// Sandbox: removed globals, frozen libraries, per-module environments, escape attempts, weak-table
// and coroutine guards, deterministic math.random, print routing.

#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

TEST_CASE("sandbox: unsafe and nondeterministic globals are absent on cells") {
    Harness h;
    h.expectRuns("absent", R"(
        assert(io == nil, "io")
        assert(os == nil, "os")
        assert(debug == nil, "debug")
        assert(loadstring == nil, "loadstring")
        assert(load == nil, "load")
        assert(dofile == nil, "dofile")
        assert(getfenv == nil, "getfenv")
        assert(setfenv == nil, "setfenv")
        assert(math.randomseed == nil, "math.randomseed")
        assert(collectgarbage == nil, "collectgarbage")
        assert(gcinfo == nil, "gcinfo")
        assert(type(require) == "function")
        assert(type(task.wait) == "function")
    )");
}

TEST_CASE("sandbox: client profile keeps gcinfo and weak tables but still drops io/os/debug") {
    VmConfig c = Harness::defaultConfig();
    c.profile = HostProfile::Client;
    c.budget = FuelBudget::client();
    Harness h(c);
    h.expectRuns("client", R"(
        assert(os == nil and debug == nil and loadstring == nil and getfenv == nil)
        assert(type(gcinfo) == "function" and gcinfo() > 0)
        local weak = setmetatable({}, {__mode = "k"}) -- weak tables are fine off-cell
        assert(getmetatable(weak).__mode == "k")
    )");
}

TEST_CASE("sandbox: escape attempts fail") {
    Harness h;
    struct Attempt {
        const char* name;
        const char* code;
    };
    const Attempt attempts[] = {
        {"write_global_lib", "string.rep = nil"},
        {"write_string_meta", "getmetatable('').__index.rep = function() end"},
        {"write_G", "_G.x = 1"},
        {"rawset_G", "rawset(_G, 'x', 1)"},
        {"setmetatable_G", "setmetatable(_G, {})"},
        {"write_math", "math.floor = print"},
        {"write_task_lib", "task.wait = nil"},
        {"write_worldpos_lib", "WorldPos.new = nil"},
        {"call_os", "os.execute('echo hi')"},
        {"call_loadstring", "loadstring('return 1')()"},
        {"call_getfenv", "getfenv(1).x = 1"},
        {"debug_registry", "debug.getregistry()"},
        {"io_open", "io.open('/etc/passwd')"},
        {"require_missing", "require('io')"},
        {"freeze_escape", "table.freeze(string); string.rep = nil"},
    };
    for (const Attempt& a : attempts) {
        const std::string wrapped = std::string("local ok, err = pcall(function() ") + a.code +
                                    " end)\nassert(not ok, 'escape succeeded')\nprint(tostring(err))";
        h.expectRuns(a.name, wrapped);
    }
    // The shared libraries are untouched afterwards.
    h.expectRuns("after",
                 "assert(type(string.rep) == 'function' and _G.x == nil and type(math.floor) == 'function')");
}

TEST_CASE("sandbox: every module has its own environment") {
    Harness h;
    h.expectRuns("writer", "leaked = 42\nassert(leaked == 42)");
    h.expectRuns("reader", "assert(leaked == nil, 'global leaked between modules')");
    // Two instances of the same script do not share globals either.
    h.load("counter", "count = (count or 0) + 1\nassert(count == 1)");
    const TaskId a = h.spawn("counter");
    const TaskId b = h.spawn("counter");
    h.steps(5);
    CHECK(h.eventFor(a, ScriptEventKind::TaskFinished));
    CHECK(h.eventFor(b, ScriptEventKind::TaskFinished));
}

TEST_CASE("sandbox: cells reject weak tables, also through local aliases (no FASTCALL bypass)") {
    Harness h;
    h.expectRuns("weak", R"(
        local ok, err = pcall(setmetatable, {}, {__mode = "k"})
        assert(not ok and err.code == "Runtime", "weak table accepted")
        local sm = setmetatable
        local ok2 = pcall(function() return sm({}, {__mode = "v"}) end)
        assert(not ok2, "aliased setmetatable bypassed the wrapper")
        -- Ordinary metatables still work.
        local t = setmetatable({}, {__index = function(_, k) return k .. "!" end})
        assert(t.hi == "hi!")
    )");
}

TEST_CASE("sandbox: typed error and userdata metatables are locked") {
    Harness h;
    h.expectRuns("locked", R"(
        local ok, e = pcall(require, "nope")
        assert(not ok and typeof(e) == "ScriptError" and e.code == "NotFound")
        assert(getmetatable(e) == "ScriptError")
        assert(not pcall(function() e.code = "Killed" end), "ScriptError is frozen")
        local p = WorldPos.new(1, 2, 3)
        assert(getmetatable(p) == "WorldPos")
        assert(string.find(tostring(e), "NotFound", 1, true))
    )");
}

TEST_CASE("sandbox: scripts cannot resume or close task coroutines") {
    Harness h;
    h.load("shared", "return {}");
    h.run("victim", R"(
        local shared = require("shared")
        shared.co = coroutine.running()
        wait(1)
        shared.done = true
    )");
    h.run("attacker", R"(
        local shared = require("shared")
        task.yield()
        assert(shared.co ~= nil)
        local ok, err = pcall(coroutine.resume, shared.co)
        assert(not ok and err.code == "InvalidState", "resumed another task's coroutine")
        local ok2 = pcall(coroutine.close, shared.co)
        assert(not ok2, "closed another task's coroutine")
        -- Nested coroutines owned by the script work normally.
        local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)
        assert(gen() == 1 and gen() == 2 and gen() == 3)
    )");
    h.steps(100);
    CHECK(h.count(ScriptEventKind::TaskFinished) == 2);
    CHECK(h.count(ScriptEventKind::TaskFailed) == 0);
}

TEST_CASE("sandbox: math.random is a deterministic seeded stream") {
    auto draw = [](u64 seed) {
        VmConfig c = Harness::defaultConfig();
        c.randomSeed = seed;
        Harness h(c);
        h.expectRuns("rng", R"(
            local out = {}
            for i = 1, 8 do out[#out + 1] = tostring(math.random(1, 1000000)) end
            local f = math.random()
            assert(f >= 0 and f < 1)
            local one = math.random(5)
            assert(one >= 1 and one <= 5)
            assert(not pcall(math.random, 3, 1), "empty interval accepted")
            print(table.concat(out, ","))
        )");
        REQUIRE(h.prints.size() == 1);
        return h.prints[0];
    };
    CHECK(draw(1) == draw(1));
    CHECK(draw(1) != draw(2));
}

TEST_CASE("sandbox: print goes to the log sink with tostring semantics") {
    Harness h;
    h.expectRuns("printer", "print('a', 1, true, nil, WorldPos.new(1, 2, 3, 7))");
    REQUIRE(h.prints.size() == 1);
    CHECK(h.prints[0] == "a\t1\ttrue\tnil\tWorldPos(1, 2, 3 @7)");
}

TEST_CASE("sandbox: print output is bounded but charged on its full length") {
    Harness h;
    h.expectRuns("flood", R"(
        local line = string.rep("x", 100000)
        local f0 = task.fuel()
        print(line, line)
        local d = task.fuel() - f0
        assert(d >= 200000 / 64, "print charged only " .. d)
    )");
    REQUIRE(h.prints.size() == 1);
    CHECK(h.prints[0].size() < 4200);
    CHECK(h.prints[0].rfind(std::string(4096, 'x'), 0) == 0);
    CHECK(h.prints[0].find("(195905 bytes truncated)") != std::string::npos);
}
