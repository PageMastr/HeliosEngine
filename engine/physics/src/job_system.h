// Internal: Jolt's JobSystemWithBarrier on the Helios job system (02 §7.1 "HeliosJoltJobSystem",
// ADR-011 "Jolt's JobSystemWithBarrier ... run on it").
//
// Jolt jobs are allocated from a fixed free list and queued as High-priority Helios jobs. A queued
// job holds a Jolt reference until it has run; a job that a barrier already executed on the waiting
// thread returns at once (Job::Execute runs a job exactly once). Jolt's barrier waits by executing
// the barrier's jobs itself, so a step always progresses, also on a pool with no free worker and
// when step() runs inside a Helios job.
//
// Those already-executed wrappers still occupy pool slots until a Helios thread dequeues them. On an
// oversubscribed pool (16 workers on 4 cores) they piled up across steps faster than the workers
// drained them and exhausted a 4096-job pool within ~80 steps (~50 jobs per step). PhysicsGrid::step()
// therefore calls drain() after every Update, which bounds the live jobs to one step's worth; a pool
// that is still exhausted (a single step needing more than maxJobs) is backpressure, not an error:
// CreateJob runs queued Helios jobs until a slot frees and logs one warning per job system.
//
// Threading: Jolt calls these from any thread; everything is lock-free or atomic. The destructor waits
// until every Helios job it queued has finished (they may still hold Jolt job references).
#pragma once

#include <atomic>

#include "helios/core/jobs.h"
#include "jolt.h"

namespace helios::physics::jolt {

class HeliosJobSystem final : public JPH::JobSystemWithBarrier {
public:
    HeliosJobSystem(jobs::JobSystem& jobs, u32 maxJobs = 4096, u32 maxBarriers = 16);
    ~HeliosJobSystem() override;

    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 numDependencies = 0) override;

    /// Waits (helping, so it may run other queued Helios jobs) until every Helios job this adapter
    /// queued has finished. Call between steps only, never while Jolt jobs are in flight.
    void drain();

    /// Jolt jobs currently holding a pool slot, and the most ever held at once (diagnostics).
    u32 liveJobs() const noexcept { return m_live.load(std::memory_order_acquire); }
    u32 peakJobs() const noexcept { return m_peak.load(std::memory_order_relaxed); }

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    jobs::JobSystem& m_jobs;
    JPH::FixedSizeFreeList<Job> m_pool;
    jobs::Counter m_outstanding;
    u32 m_maxJobs;
    std::atomic<bool> m_warnedExhausted{false};
    std::atomic<u32> m_live{0};
    std::atomic<u32> m_peak{0};
};

} // namespace helios::physics::jolt
