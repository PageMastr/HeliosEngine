// Compilation: luau_compile options, error reporting, the shared bytecode cache, the
// disabled-builtin list that keeps FASTCALL from bypassing charging wrappers, and opt-in codegen.

#include <algorithm>
#include <format>
#include <set>

#include "luacodegen.h"
#include "script_test_util.h"
#include "vm_state.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

TEST_CASE("compiler: valid source compiles; errors carry the chunk name and line") {
    const auto ok = compile("local x = 1\nreturn x + 1", {}, "good");
    REQUIRE(ok.ok());
    CHECK_FALSE((*ok)->data.empty());
    CHECK((*ok)->data[0] != 0);
    CHECK((*ok)->sourceHash == hash128(std::string_view("local x = 1\nreturn x + 1")));

    const auto bad = compile("local x = 1\nlocal = 2", {}, "broken");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::ParseError);
    CHECK(bad.error().message.rfind("broken:2:", 0) == 0);

    CompileOptions invalid;
    invalid.optimizationLevel = 3;
    CHECK(compile("return 1", invalid).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("compiler: the bytecode cache is keyed by source hash and options") {
    BytecodeCache cache;
    const auto a = cache.getOrCompile("return 1", {}, "a");
    const auto b = cache.getOrCompile("return 1", {}, "b"); // same source, other name: shared
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a->get() == b->get());
    CHECK(cache.hits() == 1);
    CHECK(cache.misses() == 1);

    CompileOptions o2;
    o2.optimizationLevel = 2;
    CHECK(o2.fingerprint() != CompileOptions{}.fingerprint());
    const auto c = cache.getOrCompile("return 1", o2, "c");
    REQUIRE(c.ok());
    CHECK(c->get() != a->get());
    CHECK(cache.size() == 2);
    CHECK_FALSE(cache.getOrCompile("return (", {}, "d").ok());
    CHECK(cache.size() == 2); // failures are not cached
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("compiler: VMs of one content version share bytecode through a common cache") {
    auto shared = std::make_shared<BytecodeCache>();
    VmConfig c = Harness::defaultConfig();
    c.bytecodeCache = shared;
    Harness h1(c);
    Harness h2(c);
    h1.load("door", "return { open = function() return true end }");
    h2.load("door", "return { open = function() return true end }");
    CHECK(shared->misses() == 1);
    CHECK(shared->hits() == 1);
    CHECK(h1.vm->moduleInfo("door")->sourceHash == h2.vm->moduleInfo("door")->sourceHash);
}

TEST_CASE("compiler: every builtin the sandbox wraps is compiled as a disabled builtin") {
    // 02 §8.3: a unit test asserts every wrapped builtin appears in disabledBuiltins, so no FASTCALL
    // reaches the original function. The only exceptions are the audited fastcallWrappedBuiltins,
    // whose FASTCALL covers an O(1) case and otherwise falls back to calling the wrapper.
    std::set<std::string> disabled;
    for (const char* name : disabledBuiltins()) CHECK(disabled.emplace(name).second); // no duplicates
    std::set<std::string> declared;
    for (const char* name : wrappedBuiltins()) declared.emplace(name);
    std::set<std::string> fastcall;
    for (const char* name : fastcallWrappedBuiltins()) fastcall.emplace(name);
    CHECK(fastcall == std::set<std::string>{"table.insert"});
    const std::vector<std::string> installed = helios::script::detail::sandboxWrappedBuiltins();
    CHECK(installed.size() == declared.size());
    for (const std::string& name : installed) {
        INFO(name);
        CHECK(declared.count(name) == 1);
        CHECK(disabled.count(name) + fastcall.count(name) == 1);
    }
    for (const char* gcSensitive : {"tostring", "string.char", "string.sub"})
        CHECK(disabled.count(gcSensitive) == 1);
}

TEST_CASE("compiler: table.insert keeps its FASTCALL only for the O(1) append") {
    // The append is compiled as a FASTCALL (no call safepoint, no wrapper); a positional insert
    // falls back to a regular call of the charging wrapper.
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    h.expectRuns("inserts", R"(
        local t = table.create(20000, 0)
        local f0 = task.fuel()
        for i = 1, 1000 do table.insert(t, i) end
        local append = task.fuel() - f0
        f0 = task.fuel()
        for i = 1, 10 do table.insert(t, 1, i) end
        local front = task.fuel() - f0
        assert(append <= 1100, "append cost " .. append) -- ~1 back-edge per iteration
        assert(front >= 10 * 2500, "positional insert cost " .. front)
        assert(t[1] == 10 and #t == 21010)
    )");
}

TEST_CASE("compiler: fuel does not depend on GC pacing (no GC-dependent FASTCALL fallbacks)") {
    // tostring/string.char/string.sub fastcalls fall back to a counted call when a GC step is due;
    // they are compiled as plain calls, so the same script costs the same fuel whatever the heap
    // history of the VM.
    VmConfig c = Harness::defaultConfig();
    c.budget.fuelPerResume = 0;
    c.budget.fuelKill = 0;
    c.budget.fuelPerTick = 0;
    Harness h(c);
    h.load("strings", R"(
        local parts = {}
        for r = 1, 20 do
            for i = 1, 200 do
                parts[i] = tostring(i) .. string.char(65 + i % 26) .. string.sub("abcdef", 2, 4)
            end
        end
    )");
    std::vector<u64> fuel;
    for (int i = 0; i < 6; ++i) {
        if (i % 2 == 1)
            h.expectRuns(std::format("junk{}", i), "local t = {} for i = 1, 5000 do t[i] = {i} end");
        const TaskId id = h.spawn("strings");
        h.step();
        fuel.push_back(h.eventFor(id, ScriptEventKind::TaskFinished)->fuel);
    }
    for (const u64 f : fuel) CHECK(f == fuel.front());
}

TEST_CASE("compiler: optimization and debug levels produce equivalent programs") {
    for (int opt = 0; opt <= 2; ++opt) {
        for (int dbg = 0; dbg <= 2; ++dbg) {
            VmConfig c = Harness::defaultConfig();
            c.compileOptions.optimizationLevel = opt;
            c.compileOptions.debugLevel = dbg;
            Harness h(c);
            h.expectRuns("levels", R"(
                local function add(a, b) return a + b end
                local s = 0
                for i = 1, 100 do s = add(s, i) end
                assert(s == 5050)
                print(s)
            )");
            REQUIRE(h.prints.size() == 1);
            CHECK(h.prints[0] == "5050");
        }
    }
}

TEST_CASE("compiler: native codegen is opt-in and gated by luau_codegen_supported") {
    {
        Harness h; // default: off (and refused on cells, 02 §7.4)
        CHECK_FALSE(h.vm->nativeCodegenActive());
        ModuleOptions native;
        native.native = true;
        h.load("nat", "return { f = function(x) return x * 2 end }", native);
        CHECK_FALSE(h.vm->moduleInfo("nat")->native);
    }
    // Client and editor VMs may opt in (the budgets below are still fuel, which native code counts).
    VmConfig c = Harness::defaultConfig();
    c.profile = HostProfile::Editor;
    c.enableNativeCodegen = true;
    Harness h(c);
    CHECK(h.vm->nativeCodegenActive() == (luau_codegen_supported() != 0));
    ModuleOptions native;
    native.native = true;
    h.load("nat", R"(
        local function mix(n)
            local acc = 0
            for i = 1, n do acc = (acc * 33 + i) % 1000003 end
            return acc
        end
        return { mix = mix }
    )",
           native);
    h.expectRuns("user", "assert(require('nat').mix(1000) == require('nat').mix(1000))");
    CHECK(h.vm->moduleInfo("nat")->native == h.vm->nativeCodegenActive());

    // Kills raised from native code unwind through JIT frames (registered unwind info).
    h.load("natspin", "return { spin = function() local n = 0 while true do n += 1 end end }", native);
    const TaskId id = h.run("natuser", "require('natspin').spin()");
    h.steps(3);
    const auto* killed = h.eventFor(id, ScriptEventKind::TaskKilled);
    REQUIRE(killed != nullptr);
    CHECK(killed->fuel == 10'000);
    h.expectRuns("natafter", "assert(require('nat').mix(10) > 0)");
}

TEST_CASE("compiler: a cell VmConfig with native codegen fails create()") {
    // 02 §7.4 / 04 §10.2: cells and world-script hosts (which run the cell profile) are
    // interpreter-only; the config is refused on every target, not just where codegen is supported.
    VmConfig cell = Harness::defaultConfig();
    cell.enableNativeCodegen = true;
    const auto refused = ScriptVm::create(cell);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(refused.error().message.find("native codegen is refused on cells") != std::string::npos);

    VmConfig interpreted = Harness::defaultConfig(); // a cell without codegen is fine
    CHECK(ScriptVm::create(interpreted).ok());
    for (const HostProfile profile : {HostProfile::Client, HostProfile::Editor}) {
        VmConfig c = Harness::defaultConfig();
        c.profile = profile;
        c.enableNativeCodegen = true;
        const auto created = ScriptVm::create(c);
        REQUIRE(created.ok());
        CHECK((*created)->nativeCodegenActive() == (luau_codegen_supported() != 0));
    }
}
