// Jolt's JobSystemWithBarrier on the Helios job system: the scripted scene gives the same state on
// the caller thread and on a loaded pool, from inside a Helios job, on an oversubscribed pool and
// with grids stepping concurrently (02 §7.1, ADR-011).
#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "helios/core/jobs.h"
#include "scene.h"

using namespace helios;
using namespace helios::physics;
using namespace helios::physics::test;

namespace {
constexpr u32 kSteps = 240;

u64 singleThreadedReference() {
    static const u64 hash = [] {
        Scene s({});
        return s.run(kSteps);
    }();
    return hash;
}
} // namespace

TEST_CASE("jobs: the Helios job system gives the single-threaded result at 1, 2 and 3 workers") {
    PhysicsRuntime runtime;
    const u64 reference = singleThreadedReference();
    for (u32 workers : {1u, 2u, 3u}) {
        jobs::JobSystem js({.workerCount = workers, .name = "PhysJobs"});
        Scene s({.jobs = &js});
        CHECK_MESSAGE(s.run(kSteps) == reference, "workers " << workers);
    }
}

TEST_CASE("jobs: stepping under unrelated load and from inside a job") {
    PhysicsRuntime runtime;
    const u64 reference = singleThreadedReference();
    jobs::JobSystem js({.workerCount = 3, .name = "PhysLoad"});
    // Background load: low-priority busy jobs keep re-queuing themselves while physics steps.
    std::atomic<bool> stop{false};
    std::atomic<u64> spins{0};
    jobs::Counter load;
    struct Spinner {
        jobs::JobSystem* js;
        std::atomic<bool>* stop;
        std::atomic<u64>* spins;
        jobs::Counter* counter;
        void operator()() const {
            u64 x = 0;
            for (int i = 0; i < 2000; ++i) x += static_cast<u64>(i) * 2654435761u;
            spins->fetch_add(1 + (x & 1), std::memory_order_relaxed);
            if (!stop->load(std::memory_order_relaxed)) js->run(Spinner{*this}, counter, jobs::Priority::Low);
        }
    };
    for (int i = 0; i < 8; ++i) js.run(Spinner{&js, &stop, &spins, &load}, &load, jobs::Priority::Low);
    {
        Scene s({.jobs = &js});
        CHECK(s.run(kSteps) == reference);
    }
    // The whole simulation driven from a worker thread (nested barrier waits inside a job).
    u64 insideJob = 0;
    jobs::Counter done;
    js.run(
        [&] {
            Scene s({.jobs = &js});
            insideJob = s.run(kSteps);
        },
        &done);
    js.wait(done);
    CHECK(insideJob == reference);
    stop.store(true);
    js.wait(load);
    CHECK(spins.load() > 0);
}

TEST_CASE("jobs: an oversubscribed pool keeps stepping (regression: Jolt job pool exhaustion)") {
    // Jobs that a barrier already ran on the stepping thread keep a pool slot until a Helios worker
    // dequeues their wrapper. With 32 workers on a few cores those wrappers used to pile up across
    // steps until the 4096-job pool ran out (an assert, ~80 steps in on a 4-core box at 16 workers).
    // step() now drains them, and a full pool is backpressure, not an error.
    // One grid for many steps: the pile-up was per grid (each grid owns its Jolt job pool).
    constexpr u32 kLongRun = 3000;
    PhysicsRuntime runtime;
    Scene single({});
    const u64 reference = single.run(kLongRun);
    jobs::JobSystem js({.workerCount = 32, .name = "PhysOver"});
    Scene s({.jobs = &js});
    // Deterministic form of the regression: step() returns only after every wrapper it queued has
    // run, so no Jolt job holds a pool slot between steps and the peak stays at one step's jobs.
    // (Without the drain, wrappers of already-executed jobs outlive their step.)
    u32 stepsWithJobsLeft = 0;
    for (u32 i = 0; i < kLongRun; ++i) {
        s.step();
        if (s.grid().stats().jobsInFlight != 0) ++stepsWithJobsLeft;
    }
    CHECK(s.grid().stateHash() == reference);
    CHECK(stepsWithJobsLeft == 0);
    const PhysicsGrid::Stats st = s.grid().stats();
    MESSAGE("Jolt job pool peak: " << st.jobPoolPeak << " of 4096");
    CHECK(st.jobPoolPeak > 0);
    CHECK(st.jobPoolPeak < 1024);
}

TEST_CASE("jobs: two grids step concurrently on one job system") {
    PhysicsRuntime runtime;
    const u64 reference = singleThreadedReference();
    jobs::JobSystem js({.workerCount = 2, .name = "PhysPair"});
    u64 a = 0, b = 0;
    std::thread other([&] {
        Scene s({.jobs = &js});
        b = s.run(kSteps);
    });
    {
        Scene s({.jobs = &js});
        a = s.run(kSteps);
    }
    other.join();
    CHECK(a == reference);
    CHECK(b == reference);
}
