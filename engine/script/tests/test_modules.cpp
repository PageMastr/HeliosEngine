// Modules: require caching and errors, hot reload with re-require semantics and __reload(old),
// error reporting with stack traces mapped to module names, callbacks nested in bindings.

#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

std::string callString(Harness& h, const char* module, const char* fn) {
    std::string out;
    const auto r = h.vm->callExport(
        module, fn, {},
        [&](lua_State* L, int base, int) { out = lua_tostring(L, base) ? lua_tostring(L, base) : ""; }, 1);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    return out;
}

} // namespace

TEST_CASE("modules: require runs a chunk once and caches its exports") {
    Harness h;
    h.load("lib", R"(
        local loads = (loads or 0) + 1
        return { value = 7, loads = loads, twice = function(x) return 2 * x end }
    )");
    h.load("cycleA", "return require('cycleB')");
    h.load("cycleB", "return require('cycleA')");
    h.load("broken", "error('module failed to load')");
    h.expectRuns("user", R"(
        local a = require("lib")
        local b = require("lib")
        assert(a == b and a.value == 7 and a.twice(21) == 42)
        local ok, err = pcall(require, "missing")
        assert(not ok and err.code == "NotFound")
        local ok2, err2 = pcall(require, "cycleA")
        assert(not ok2 and string.find(tostring(err2), "cyclic require", 1, true))
        local ok3, err3 = pcall(require, "broken")
        assert(not ok3 and string.find(err3, "broken:1: module failed to load", 1, true))
        -- A failed load is not cached as success: the next require retries (and fails again).
        assert(not pcall(require, "broken"))
    )");
    const auto info = h.vm->moduleInfo("lib");
    REQUIRE(info);
    CHECK(info->instantiated);
    CHECK(info->version == 1);
    CHECK_FALSE(h.vm->moduleInfo("broken")->instantiated);
}

TEST_CASE("modules: loadModule reports compile errors with the module name and line") {
    Harness h;
    const auto r = h.vm->loadModule("bad", "local x = \nfunction(");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::ParseError);
    CHECK(r.error().message.rfind("bad:", 0) == 0);
    CHECK(h.vm->lastError().code == ScriptErrorCode::Compile);
    CHECK_FALSE(h.vm->hasModule("bad"));
    h.load("dup", "return 1");
    CHECK(h.vm->loadModule("dup", "return 2").error().code == ErrorCode::AlreadyExists);
    CHECK(h.vm->spawnScript("nope").error().code == ErrorCode::NotFound);
}

TEST_CASE("modules: runtime errors carry stack traces mapped to module names") {
    Harness h;
    h.load("inner", R"(
        local M = {}
        function M.explode(x)
            local y = x.field -- line 4: index a number
            return y
        end
        return M
    )");
    const TaskId id = h.run("outer", R"(
        local inner = require("inner")
        local function relay(v)
            return inner.explode(v) -- line 4
        end
        relay(5)
    )");
    h.steps(5);
    const auto* e = h.eventFor(id, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    REQUIRE(e->error.has_value());
    const ScriptError& err = *e->error;
    CHECK(err.code == ScriptErrorCode::Runtime);
    CHECK(err.module == "outer");
    CHECK(err.message.rfind("inner:4:", 0) == 0);
    REQUIRE(err.stack.size() >= 3);
    CHECK(err.stack[0].source == "inner");
    CHECK(err.stack[0].line == 4);
    CHECK(err.stack[0].function == "explode");
    CHECK(err.stack[1].source == "outer");
    CHECK(err.stack[1].line == 4);
    CHECK(err.stack[1].function == "relay");
    CHECK(err.stack[2].source == "outer");
    const std::string text = err.toString();
    CHECK(text.find("at inner:4 in explode") != std::string::npos);
    CHECK(text.find("at outer:4 in relay") != std::string::npos);
    CHECK(h.vm->lastError().message == err.message);

    // Engine→Luau callbacks capture the trace at the error point too.
    const auto r = h.vm->callExport("inner", "explode", [](lua_State* L) {
        lua_pushnumber(L, 1);
        return 1;
    });
    REQUIRE_FALSE(r.ok());
    const ScriptError& cb = h.vm->lastError();
    REQUIRE_FALSE(cb.stack.empty());
    bool sawExplode = false;
    for (const StackFrame& f : cb.stack) sawExplode = sawExplode || (f.source == "inner" && f.line == 4);
    CHECK(sawExplode);
}

TEST_CASE("modules: hot reload with re-require semantics and __reload(old)") {
    Harness h;
    h.load("config", R"(
        local M = { version = 1, hits = 0 }
        function M.describe() return "v1:" .. M.hits end
        function M.hit() M.hits += 1 end
        return M
    )");
    h.load("holder", R"(
        local captured = require("config") -- captured before the reload
        return {
            captured = function() return captured.describe() end,
            fresh = function() return require("config").describe() end,
        }
    )");
    REQUIRE(h.vm->instantiateModule("holder").ok());
    REQUIRE(h.vm->callExport("config", "hit").ok());
    REQUIRE(h.vm->callExport("config", "hit").ok());
    CHECK(callString(h, "holder", "fresh") == "v1:2");

    // A task already running keeps the old code until it finishes.
    const TaskId runner = h.run("longrunner", R"(
        local cfg = require("config")
        wait(0.2)
        print(cfg.describe())
    )");
    h.step();

    const auto reloaded = h.vm->reloadModule("config", R"(
        local M = { version = 2, hits = 0 }
        function M.describe() return "v2:" .. M.hits end
        function M.hit() M.hits += 10 end
        function M.__reload(old) M.hits = old.hits end -- migrate state
        return M
    )");
    REQUIRE_MESSAGE(reloaded.ok(), (reloaded.ok() ? std::string() : reloaded.error().toString()));
    CHECK(h.vm->moduleInfo("config")->version == 2);
    CHECK(h.last(ScriptEventKind::ModuleReloaded) != nullptr);
    CHECK(callString(h, "holder", "fresh") == "v2:2");    // require() now returns the new exports
    CHECK(callString(h, "holder", "captured") == "v1:2"); // captured tables keep the old code
    REQUIRE(h.vm->callExport("config", "hit").ok());      // callExport resolves the new exports
    CHECK(callString(h, "holder", "fresh") == "v2:12");
    h.steps(10);
    CHECK(h.eventFor(runner, ScriptEventKind::TaskFinished));
    REQUIRE(h.prints.size() == 1);
    CHECK(h.prints[0] == "v1:2");

    // A reload that fails to compile or to run keeps the current version.
    CHECK_FALSE(h.vm->reloadModule("config", "return {").ok());
    CHECK_FALSE(h.vm->reloadModule("config", "error('bad reload')").ok());
    CHECK_FALSE(
        h.vm->reloadModule("config", "return { __reload = function(old) error('migration failed') end }")
            .ok());
    CHECK(h.vm->moduleInfo("config")->version == 2);
    CHECK(callString(h, "holder", "fresh") == "v2:12");

    // Reloading a module that was never required just swaps the code.
    h.load("lazy", "return { v = function() return 'a' end }");
    REQUIRE(h.vm->reloadModule("lazy", "return { v = function() return 'b' end }").ok());
    CHECK(callString(h, "lazy", "v") == "b");
    CHECK(h.vm->moduleInfo("lazy")->version == 2);
}

TEST_CASE("modules: reload re-enables a module disabled by the three-kills rule") {
    Harness h;
    h.load("flaky", "while true do end");
    for (int i = 0; i < 3; ++i) h.spawn("flaky");
    h.steps(5, false);
    REQUIRE(h.vm->moduleInfo("flaky")->disabled);
    REQUIRE(h.vm->reloadModule("flaky", "print('fixed')").ok());
    CHECK_FALSE(h.vm->moduleInfo("flaky")->disabled);
    CHECK(h.vm->moduleInfo("flaky")->recentKills == 0);
    h.spawn("flaky");
    h.steps(3);
    CHECK(h.prints == std::vector<std::string>{"fixed"});
}

TEST_CASE("modules: callbacks from bindings run nested in the current resume") {
    Harness* self = nullptr;
    Harness h(Harness::defaultConfig(), [&](Binder& b) {
        b.function(
            "Test", "dispatch",
            [](lua_State* L) -> int {
                // An engine event raised while a script runs: the handler shares the caller's budget.
                auto* hp = static_cast<Harness**>(bindingUserdata(L));
                const u64 before = currentFuel(L);
                const auto r = (*hp)->vm->callExport("handlers", "onEvent", [](lua_State* S) {
                    lua_pushnumber(S, 41);
                    return 1;
                });
                lua_pushboolean(L, r.ok());
                lua_pushnumber(L, static_cast<double>(currentFuel(L) - before));
                return 2;
            },
            FuelCost{1, 0, 0}, &self);
    });
    self = &h;
    h.load("handlers", R"(
        local M = { seen = 0 }
        function M.onEvent(v) M.seen = v + 1 for i = 1, 100 do end end
        function M.spin() while true do end end
        return M
    )");
    h.expectRuns("dispatcher", R"(
        local ok, fuel = Test.dispatch()
        assert(ok and require("handlers").seen == 42)
        assert(fuel >= 100, "nested callback fuel was not charged to the caller: " .. fuel)
    )");
}

TEST_CASE("modules: a disabled module cannot be required, even after an earlier require") {
    Harness h;
    h.load("lib", R"(
        local M = {}
        function M.spin() while true do end end
        function M.ok() return 1 end
        return M
    )");
    h.expectRuns("early", "assert(require('lib').ok() == 1)");
    for (int i = 0; i < 3; ++i) CHECK_FALSE(h.vm->callExport("lib", "spin").ok());
    REQUIRE(h.vm->moduleInfo("lib")->disabled);
    h.expectRuns("late", R"(
        local ok, err = pcall(require, "lib")
        assert(not ok and err.code == "ModuleDisabled", "cached exports of a disabled module were returned")
    )");
    REQUIRE(h.vm->enableModule("lib").ok());
    h.expectRuns("again", "assert(require('lib').ok() == 1)");
}

TEST_CASE("modules: huge error messages are truncated when extracted to the host") {
    Harness h;
    const TaskId id = h.run("bigerror", "error(string.rep('e', 200000), 0)");
    h.steps(3);
    const auto* e = h.eventFor(id, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    CHECK(e->error->message.size() < 4200);
    CHECK(e->error->message.rfind(std::string(4096, 'e'), 0) == 0);
    CHECK(e->error->message.find("(195904 bytes truncated)") != std::string::npos);
    // Scripts still see the full error value.
    h.expectRuns("catcher", R"(
        local ok, err = pcall(error, string.rep("x", 100000), 0)
        assert(not ok and #err == 100000)
    )");
}
