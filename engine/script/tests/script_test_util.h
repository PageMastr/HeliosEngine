#pragma once
// Shared harness for engine/script tests: a VM on a 20 Hz DilatableClock with recorded events and
// print output.

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "helios/core/time.h"
#include "helios/script/script.h"

namespace helios::script::test {

struct RecordedEvent {
    ScriptEventKind kind = ScriptEventKind::TaskFinished;
    u64 tick = 0;
    TaskId task;
    u64 owner = 0;
    std::string module;
    u64 fuel = 0;
    KillReason killReason = KillReason::None;
    std::optional<ScriptError> error;
};

struct Harness {
    DilatableClock clock{DilatableClock::Config{50'000'000, 100'000, 1'000'000}};
    std::vector<RecordedEvent> events;
    std::vector<std::string> prints;
    std::unique_ptr<ScriptVm> vm;

    /// Budgets small enough for fast tests: soft 2k, kill 10k, lane 30k fuel.
    static VmConfig defaultConfig() {
        VmConfig c;
        c.name = "test";
        c.budget.fuelPerResume = 2'000;
        c.budget.fuelKill = 10'000;
        c.budget.fuelPerTick = 30'000;
        c.budget.wallBackstopNanos = 0; // deterministic tests: fuel only unless a test opts in
        c.heapLimitBytes = 64u << 20;
        return c;
    }

    explicit Harness(VmConfig config = defaultConfig(), const ScriptVm::ApiRegistrar& api = {}) {
        config.clock = &clock;
        config.onEvent = [this](const ScriptEvent& e) {
            RecordedEvent r;
            r.kind = e.kind;
            r.tick = e.tick;
            r.task = e.task;
            r.owner = e.owner;
            r.module = std::string(e.module);
            r.fuel = e.fuel;
            r.killReason = e.killReason;
            if (e.error) r.error = *e.error;
            events.push_back(std::move(r));
        };
        config.onPrint = [this](std::string_view, std::string_view text) { prints.emplace_back(text); };
        auto created = ScriptVm::create(config, api);
        REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string() : created.error().toString()));
        vm = std::move(*created);
    }

    void load(std::string_view name, std::string_view source, const ModuleOptions& options = {}) {
        const auto r = vm->loadModule(name, source, options);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    }

    TaskId spawn(std::string_view name, u64 owner = 0) {
        auto r = vm->spawnScript(name, owner);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
        return *r;
    }

    /// Loads `source` as module `name` and spawns it as a task.
    TaskId run(std::string_view name, std::string_view source, u64 owner = 0) {
        load(name, source);
        return spawn(name, owner);
    }

    /// Advances the zone clock by one 50 ms step and runs one scheduler tick.
    TickStats step() {
        clock.advance(clock.stepNanos());
        while (clock.consumeStep()) {}
        return vm->tick();
    }

    /// Runs `n` ticks (stops early once no task is live when `untilIdle`).
    void steps(int n, bool untilIdle = true) {
        for (int i = 0; i < n; ++i) {
            step();
            if (untilIdle && vm->liveTaskCount() == 0) return;
        }
    }

    usize count(ScriptEventKind kind) const {
        usize n = 0;
        for (const RecordedEvent& e : events) n += e.kind == kind ? 1 : 0;
        return n;
    }

    const RecordedEvent* last(ScriptEventKind kind) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->kind == kind) return &*it;
        }
        return nullptr;
    }

    const RecordedEvent* eventFor(TaskId id, ScriptEventKind kind) const {
        for (const RecordedEvent& e : events) {
            if (e.task == id && e.kind == kind) return &e;
        }
        return nullptr;
    }

    /// Runs `source` as a task to completion (<= 200 ticks) and requires it finished cleanly.
    void expectRuns(std::string_view name, std::string_view source) {
        const TaskId id = run(name, source);
        steps(200);
        const RecordedEvent* failed = eventFor(id, ScriptEventKind::TaskFailed);
        const RecordedEvent* killed = eventFor(id, ScriptEventKind::TaskKilled);
        INFO("module ", name, ": ", failed && failed->error ? failed->error->toString() : std::string(),
             killed && killed->error ? killed->error->toString() : std::string());
        CHECK(eventFor(id, ScriptEventKind::TaskFinished) != nullptr);
    }
};

} // namespace helios::script::test
