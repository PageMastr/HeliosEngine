#include "job_system.h"

#include <thread>

#include "log_channel.h"

namespace helios::physics::jolt {

HeliosJobSystem::HeliosJobSystem(jobs::JobSystem& jobs, u32 maxJobs, u32 maxBarriers) : m_jobs(jobs), m_maxJobs(maxJobs) {
    JobSystemWithBarrier::Init(maxBarriers);
    m_pool.Init(maxJobs, maxJobs);
}

HeliosJobSystem::~HeliosJobSystem() {
    // Queued Helios jobs keep Jolt job references (and point at m_pool): let them drain first.
    m_jobs.wait(m_outstanding);
}

void HeliosJobSystem::drain() { m_jobs.wait(m_outstanding); }

int HeliosJobSystem::GetMaxConcurrency() const { return static_cast<int>(m_jobs.workerCount()) + 1; }

JPH::JobSystem::JobHandle HeliosJobSystem::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                                     JPH::uint32 numDependencies) {
    JPH::uint32 index;
    for (;;) {
        index = m_pool.ConstructObject(name, color, this, function, numDependencies);
        if (index != decltype(m_pool)::cInvalidObjectIndex) break;
        // Pool exhausted: backpressure. Help the queue drain (queued wrappers release their slots)
        // instead of sleeping like Jolt's own pool does (100 us per retry).
        if (!m_warnedExhausted.exchange(true, std::memory_order_relaxed)) {
            HELIOS_LOG_WARN(LogPhysics, "Jolt job pool ({} jobs) exhausted; waiting for queued jobs to drain", m_maxJobs);
        }
        if (!m_jobs.tryRunOne()) std::this_thread::yield();
    }
    const u32 live = m_live.fetch_add(1, std::memory_order_acq_rel) + 1;
    u32 peak = m_peak.load(std::memory_order_relaxed);
    while (live > peak && !m_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    Job* job = &m_pool.Get(index);
    JobHandle handle(job); // keeps a reference: the job may run and finish before we return
    if (numDependencies == 0) QueueJob(job);
    return handle;
}

void HeliosJobSystem::QueueJob(Job* job) {
    job->AddRef();
    m_jobs.run(
        [job] {
            (void)job->Execute(); // returns at once if a barrier's waiting thread already ran it
            job->Release();
        },
        &m_outstanding, jobs::Priority::High);
}

void HeliosJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
}

void HeliosJobSystem::FreeJob(Job* job) {
    m_pool.DestructObject(job);
    m_live.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace helios::physics::jolt
