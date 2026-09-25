// Handles and WorldPos: host objects as tagged userdata over generational handles (StaleHandle
// instead of UB after a wait, slot reuse detected), and double-precision WorldPos userdata.

#include <cmath>

#include "helios/core/handle.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

struct Probe {
    double health = 100;
    int pings = 0;
};

struct ProbeWorld {
    HandlePool<Probe> pool;
    ObjectType type = -1;
    std::vector<Handle<Probe>> spawned;
};

int tSpawnProbe(lua_State* L) {
    auto* w = static_cast<ProbeWorld*>(bindingUserdata(L));
    const Handle<Probe> h = w->pool.create();
    w->pool.get(h)->health = luaL_optnumber(L, 1, 100);
    w->spawned.push_back(h);
    pushObject(L, w->type, h.toBits());
    return 1;
}

int tHealth(lua_State* L) {
    auto* w = static_cast<ProbeWorld*>(bindingUserdata(L));
    lua_pushnumber(L, checkObjectAs<Probe>(L, 1, w->type)->health);
    return 1;
}

int tPing(lua_State* L) {
    auto* w = static_cast<ProbeWorld*>(bindingUserdata(L));
    Probe* p = checkObjectAs<Probe>(L, 1, w->type);
    ++p->pings;
    lua_pushinteger(L, p->pings);
    return 1;
}

ScriptVm::ApiRegistrar probeApi(ProbeWorld& w) {
    return [&w](Binder& b) {
        w.type = b.poolType("Probe", w.pool);
        REQUIRE(w.type >= 0);
        b.function("Probes", "spawn", &tSpawnProbe, FuelCost{3, 0, 0}, &w);
        b.property(w.type, "health", &tHealth, FuelCost{1, 0, 0}, &w);
        b.method(w.type, "ping", &tPing, FuelCost{1, 0, 0}, &w);
    };
}

} // namespace

TEST_CASE("handles: a handle invalidated across a wait raises StaleHandle") {
    ProbeWorld w;
    Harness h(Harness::defaultConfig(), probeApi(w));
    const TaskId id = h.run("revalidate", R"(
        local p = Probes.spawn(75)
        assert(typeof(p) == "Probe" and type(p) == "userdata")
        assert(p.health == 75 and p:ping() == 1 and p:isValid())
        wait(0.1) -- the host destroys the probe meanwhile
        assert(not p:isValid(), "isValid must not raise and must report the stale handle")
        local ok, err = pcall(function() return p.health end)
        assert(not ok and typeof(err) == "ScriptError" and err.code == "StaleHandle", "property")
        local ok2, err2 = pcall(function() return p:ping() end)
        assert(not ok2 and err2.code == "StaleHandle", "method")
        print(err.message)
        local _ = p.health -- unprotected: the task fails with a typed StaleHandle error
    )");
    h.step();
    REQUIRE(w.spawned.size() == 1);
    CHECK(w.pool.destroy(w.spawned[0]));
    h.steps(10);
    const auto* e = h.eventFor(id, ScriptEventKind::TaskFailed);
    REQUIRE(e != nullptr);
    REQUIRE(e->error.has_value());
    INFO(e->error->toString());
    CHECK(e->error->code == ScriptErrorCode::StaleHandle);
    CHECK(e->error->message.find("revalidate:12:") != std::string::npos);
    REQUIRE(h.prints.size() == 1);
    CHECK(h.prints[0].find("Probe handle") != std::string::npos);
}

TEST_CASE("handles: a recycled slot never resolves an old handle") {
    ProbeWorld w;
    Harness h(Harness::defaultConfig(), probeApi(w));
    h.load("shared", "return {}");
    const TaskId id = h.run("recycle", R"(
        local shared = require("shared")
        local old = Probes.spawn(1)
        wait(0.1) -- host: destroy old, spawn a new probe that reuses the slot
        local new = Probes.spawn(2)
        assert(new.health == 2)
        assert(old ~= new, "same slot, different generation")
        assert(not old:isValid() and new:isValid())
        assert(not pcall(function() return old.health end))
        assert(tostring(old) ~= tostring(new))
    )");
    h.step();
    REQUIRE(w.spawned.size() == 1);
    const Handle<Probe> old = w.spawned[0];
    CHECK(w.pool.destroy(old));
    h.steps(10);
    CHECK(h.eventFor(id, ScriptEventKind::TaskFinished));
    REQUIRE(w.spawned.size() == 2);
    CHECK(w.spawned[1].index() == old.index()); // the slot was reused
    CHECK(w.spawned[1].generation() != old.generation());
}

TEST_CASE("handles: object userdata rejects the wrong type and unknown members") {
    ProbeWorld w;
    Harness h(Harness::defaultConfig(), probeApi(w));
    h.expectRuns("types", R"(
        local p = Probes.spawn()
        assert(not pcall(function() return p.nonexistent end))
        local fakeSelf = WorldPos.new(0, 0, 0)
        assert(not pcall(p.ping, fakeSelf), "method accepted a WorldPos as self")
        assert(not pcall(p.ping, {}), "method accepted a table as self")
        assert(p == p and getmetatable(p) == "Probe")
        assert(not pcall(function() p.health = 5 end), "properties are read-only")
    )");
}

TEST_CASE("WorldPos: double precision userdata, never a float vector") {
    Harness h;
    h.expectRuns("worldpos", R"(
        local far = WorldPos.new(1e13, 0, 0)
        local step = WorldPos.new(0.001, 0, 0)
        local moved = far + step
        assert(typeof(moved) == "WorldPos" and type(moved) == "userdata" and type(moved) ~= "vector")
        assert(moved.x == 1e13 + 0.001, "f64 precision lost")
        assert(moved.x ~= 1e13, "millimetre step vanished at 10^13 m")
        assert((moved - far).x == (1e13 + 0.001) - 1e13)
        local a = WorldPos.new(1, 2, 3, 4)
        local b = a:offset(3, 4, 0)
        assert(b.frame == 4 and a:distance(b) == 5)
        assert((a * 2).y == 4 and (2 * a).z == 6 and (a / 2).x == 0.5 and (-a).x == -1)
        assert(a:lerp(b, 0.5).x == 2.5)
        assert(a == WorldPos.new(1, 2, 3, 4) and a ~= WorldPos.new(1, 2, 3, 5))
        assert(WorldPos.new(3, 4, 0):length() == 5)
        local ok = pcall(function() return a + WorldPos.new(0, 0, 0, 9) end)
        assert(not ok, "adding positions from different frames must fail")
        assert(not pcall(WorldPos.new, 1, 2), "missing coordinate accepted")
        assert(not pcall(WorldPos.new, 1, 2, 3, -1), "negative frame accepted")
        assert(not pcall(function() a.x = 5 end), "WorldPos is immutable")
    )");
}

TEST_CASE("WorldPos: C++ push/check round trip keeps every bit") {
    FramePos seen{};
    Harness h(Harness::defaultConfig(), [&seen](Binder& b) {
        b.function(
            "Test", "echo",
            [](lua_State* L) -> int {
                const FramePos p = checkWorldPos(L, 1);
                *static_cast<FramePos*>(bindingUserdata(L)) = p;
                pushWorldPos(L, p);
                return 1;
            },
            FuelCost{1, 0, 0}, &seen);
    });
    h.expectRuns("roundtrip", R"(
        local p = WorldPos.new(123456789012.345678, -0.1, 6.02e23, 42)
        local q = Test.echo(p)
        assert(q == p and q.x == 123456789012.345678 and q.z == 6.02e23)
        assert(not pcall(Test.echo, vector.create(1, 2, 3)), "a float vector is not a WorldPos")
    )");
    CHECK(seen.frame.value == 42);
    CHECK(seen.local.x == 123456789012.345678);
    CHECK(seen.local.y == -0.1);
}
