#pragma once
// Job system (ADR-011).
//
// * JobSystem: worker pool (default hardwareThreads-1, min 1). Each worker owns one deque per
//   priority (LIFO for the owner, FIFO steals for others); non-worker threads submit into global
//   per-priority injection queues. Three priorities; higher priorities are always searched first.
// * Job: type-erased callable with 48 bytes of inline storage, so typical lambdas never allocate.
// * Counter: completion counter. JobSystem::wait(counter) *helps*: the waiting thread executes
//   other jobs until the counter reaches zero, so waiting inside a job never deadlocks the pool.
// * parallelFor(begin, end, grain, fn): chunked data-parallel loop; the caller participates.
// * TaskGraph: named nodes + dependency edges, validated acyclic, executed with correct ordering.
// * BackgroundPool: separate threads for long, blocking work (IO, shader compiles, pathfinding) so
//   frame jobs never starve behind them.
//
// Rules:
// * Jobs must not block on OS primitives waiting for other jobs; use counters + wait().
// * A job that waits must not depend on work that only a job *lower on the same thread's stack*
//   would produce after its own wait returns (inherent to help-while-waiting without fibers).
// * Helping nests (a waiting thread runs other jobs on its own stack, which may wait in turn).
//   To keep that from overflowing the stack (Windows threads default to 1 MiB), once a thread's
//   nested waits use more than ~256 KiB of stack (or 256 levels), wait() only runs jobs spawned by
//   the job it is waiting in and otherwise just waits (TBB-style stack-limited stealing). Work
//   submitted from outside the pool is then picked up by other workers or waiting non-workers.
// * Jobs must not throw; an escaping exception terminates the process.
// * Counters must outlive every job that references them (wait before destroying).
// * Destroying a JobSystem drains all queued work, then joins the workers. No new work may be
//   submitted from other threads concurrently with destruction.
//
// Budget (4-core dev box): ~0.2-0.3 us per tiny job submitted from a worker, ~0.5 us per job
// submitted from an external thread (global-queue path). The 1,000,000-job stress tests report
// their timings via doctest MESSAGE and must stay well inside the ctest timeout.

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "helios/core/platform.h"
#include "helios/core/result.h"
#include "helios/core/thread.h"
#include "helios/core/types.h"

namespace helios::jobs {

enum class Priority : u8 { High = 0, Normal = 1, Low = 2 };
inline constexpr u32 kPriorityCount = 3;

/// Completion counter: incremented when a job is submitted with it, decremented (release) when
/// that job finishes. Zero means "all associated jobs are done". Thread-safe.
class Counter {
public:
    explicit Counter(u32 initial = 0) noexcept : m_value(initial) {}
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;

    u32 value() const noexcept { return m_value.load(std::memory_order_acquire); }
    bool isDone() const noexcept { return value() == 0; }
    void add(u32 n = 1) noexcept { m_value.fetch_add(n, std::memory_order_relaxed); }
    void decrement(u32 n = 1) noexcept { m_value.fetch_sub(n, std::memory_order_acq_rel); }

private:
    std::atomic<u32> m_value;
};

/// Move-only, one-shot type-erased callable with small-buffer storage. Callables up to
/// kInlineSize bytes (alignment <= 16, nothrow-movable) are stored inline; larger ones fall back to
/// a tracked heap allocation (counted by jobHeapAllocationCount()).
class Job {
public:
    static constexpr usize kInlineSize = 48;
    static constexpr usize kInlineAlign = 16;

    Job() noexcept = default;

    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, Job> && std::is_invocable_v<std::decay_t<F>&>)
    explicit Job(F&& fn, Counter* counter = nullptr) : m_counter(counter) {
        using Fn = std::decay_t<F>;
        if constexpr (fitsInline<Fn>()) {
            ::new (static_cast<void*>(m_storage)) Fn(std::forward<F>(fn));
            m_ops = &kInlineOps<Fn>;
        } else {
            void* mem = allocateHeapCallable(sizeof(Fn), alignof(Fn));
            Fn* heapFn = ::new (mem) Fn(std::forward<F>(fn));
            std::memcpy(m_storage, &heapFn, sizeof(heapFn));
            m_ops = &kHeapOps<Fn>;
        }
    }

    Job(Job&& other) noexcept { moveFrom(other); }
    Job& operator=(Job&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() { reset(); }

    bool isValid() const noexcept { return m_ops != nullptr; }
    explicit operator bool() const noexcept { return isValid(); }
    Counter* counter() const noexcept { return m_counter; }
    void setCounter(Counter* counter) noexcept { m_counter = counter; }
    /// True if the callable lives in the inline buffer (no heap allocation).
    bool isInline() const noexcept { return m_ops && m_ops->isInline; }

    /// Invokes the callable and destroys it; the Job becomes empty. Does not touch the counter.
    void invoke() {
        const Ops* ops = m_ops;
        m_ops = nullptr;
        ops->invokeAndDestroy(m_storage);
    }

    /// Destroys the callable without running it.
    void reset() noexcept {
        if (m_ops) {
            m_ops->destroy(m_storage);
            m_ops = nullptr;
        }
    }

private:
    struct Ops {
        void (*invokeAndDestroy)(void* storage);
        void (*relocate)(void* dst, void* src) noexcept;
        void (*destroy)(void* storage) noexcept;
        bool isInline;
    };

    template <class Fn>
    static constexpr bool fitsInline() {
        return sizeof(Fn) <= kInlineSize && alignof(Fn) <= kInlineAlign && std::is_nothrow_move_constructible_v<Fn>;
    }

    static void* allocateHeapCallable(usize size, usize alignment);
    static void freeHeapCallable(void* ptr) noexcept;

    template <class Fn>
    static constexpr Ops kInlineOps{
        [](void* s) {
            Fn* fn = std::launder(static_cast<Fn*>(s));
            (*fn)();
            std::destroy_at(fn);
        },
        [](void* dst, void* src) noexcept {
            Fn* from = std::launder(static_cast<Fn*>(src));
            ::new (dst) Fn(std::move(*from));
            std::destroy_at(from);
        },
        [](void* s) noexcept { std::destroy_at(std::launder(static_cast<Fn*>(s))); },
        true,
    };

    template <class Fn>
    static Fn* heapPtr(void* s) noexcept {
        Fn* fn;
        std::memcpy(&fn, s, sizeof(fn));
        return fn;
    }

    template <class Fn>
    static constexpr Ops kHeapOps{
        [](void* s) {
            Fn* fn = heapPtr<Fn>(s);
            (*fn)();
            std::destroy_at(fn);
            freeHeapCallable(fn);
        },
        [](void* dst, void* src) noexcept { std::memcpy(dst, src, sizeof(Fn*)); },
        [](void* s) noexcept {
            Fn* fn = heapPtr<Fn>(s);
            std::destroy_at(fn);
            freeHeapCallable(fn);
        },
        false,
    };

    void moveFrom(Job& other) noexcept {
        m_counter = other.m_counter;
        if (other.m_ops) {
            other.m_ops->relocate(m_storage, other.m_storage);
            m_ops = std::exchange(other.m_ops, nullptr);
        }
    }

    const Ops* m_ops = nullptr;
    Counter* m_counter = nullptr;
    alignas(kInlineAlign) unsigned char m_storage[kInlineSize];
};

static_assert(sizeof(Job) == 64, "Job is expected to fill one cache line");

/// Total number of Jobs whose callable did not fit inline (process-wide).
u64 jobHeapAllocationCount() noexcept;

class BackgroundPool;

struct JobSystemDesc {
    u32 workerCount = 0;           ///< 0 = max(1, hardwareThreadCount() - 1).
    u32 backgroundThreadCount = 0; ///< >0 creates an owned BackgroundPool.
    std::string name = "Job";      ///< Thread name prefix ("Job Worker 0").
    bool pinWorkers = false;       ///< Pin worker i to logical CPU (i+1) % hw (first 64 CPUs).
};

struct JobSystemStats {
    u64 executed = 0;     ///< Jobs executed (workers + helping threads).
    u64 stolen = 0;       ///< Jobs taken from another worker's deque.
    u64 sleeps = 0;       ///< Times a worker went to sleep for lack of work.
    u64 heapJobs = 0;     ///< Process-wide jobs whose callable needed the heap.
    u64 limitedWaits = 0; ///< wait() calls nested too deeply to help with unrelated jobs.
};

class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc = {});
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Queues `fn`. If `counter` is given it is incremented now and decremented when fn returns.
    /// From a worker of this system the job goes to that worker's deque, otherwise to the global
    /// queue. Thread-safe. The counter is decremented after `fn` has been destroyed, so it must not
    /// live in memory that only `fn` keeps alive (e.g. inside a shared state `fn` holds the last
    /// reference to): release such a counter from inside `fn` instead.
    template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    void run(F&& fn, Counter* counter = nullptr, Priority priority = Priority::Normal) {
        submit(Job(std::forward<F>(fn), counter), priority);
    }

    /// Queues a prepared Job (its counter, if any, is incremented here).
    void submit(Job job, Priority priority = Priority::Normal);

    /// Blocks until `counter` reaches zero, executing other queued jobs meanwhile. Safe to call
    /// from workers (nested waits) and from any other thread.
    void wait(const Counter& counter);

    /// Blocks (helping) until every job submitted to this system has finished. Call from outside
    /// the job system only (never from a job).
    void waitIdle();

    /// Executes at most one queued job on the calling thread. Returns false if none was found.
    bool tryRunOne();

    /// Runs fn over [begin, end) split into chunks of `grain` (0 = automatic) and waits (helping).
    /// fn is either fn(u64 index) or fn(u64 chunkBegin, u64 chunkEnd). Chunk order is unspecified;
    /// fn must be safe to call concurrently for different indices.
    template <class F>
    void parallelFor(u64 begin, u64 end, u64 grain, F&& fn, Priority priority = Priority::Normal);

    u32 workerCount() const noexcept;
    /// True if the calling thread is one of this system's workers.
    bool isWorkerThread() const noexcept;
    /// Index of the calling worker in [0, workerCount()), or -1.
    i32 currentWorkerIndex() const noexcept;
    /// Owned background pool (nullptr if JobSystemDesc::backgroundThreadCount was 0).
    BackgroundPool* background() noexcept;
    JobSystemStats stats() const noexcept;

    struct Impl;

private:
    template <class F>
    static void invokeRange(F& fn, u64 b, u64 e) {
        if constexpr (std::is_invocable_v<F&, u64, u64>) {
            fn(b, e);
        } else {
            for (u64 i = b; i < e; ++i) fn(i);
        }
    }

    std::unique_ptr<Impl> m_impl;
};

template <class F>
void JobSystem::parallelFor(u64 begin, u64 end, u64 grain, F&& fn, Priority priority) {
    if (end <= begin) return;
    const u64 count = end - begin;
    if (grain == 0) {
        const u64 targetChunks = static_cast<u64>(workerCount() + 1) * 4;
        grain = count / targetChunks;
        if (grain == 0) grain = 1;
    }
    // Not (count + grain - 1) / grain: that overflows for huge grains (e.g. ~0 for "one chunk").
    const u64 chunks = count / grain + (count % grain != 0 ? 1 : 0);
    auto& f = fn;
    if (chunks == 1) {
        invokeRange(f, begin, end);
        return;
    }
    Counter counter;
    auto* fp = &f;
    for (u64 c = 1; c < chunks; ++c) {
        const u64 b = begin + c * grain;
        const u64 e = (end - b > grain) ? b + grain : end;
        run([fp, b, e] { invokeRange(*fp, b, e); }, &counter, priority);
    }
    invokeRange(f, begin, begin + grain);
    wait(counter);
}

/// Dependency graph of named tasks. Build once (single thread), run many times.
class TaskGraph {
public:
    using NodeId = u32;

    NodeId addNode(std::string name, std::function<void()> fn, Priority priority = Priority::Normal);
    /// `after` will not start before `before` has finished. Duplicate edges are ignored.
    Result<void> addEdge(NodeId before, NodeId after);
    /// Checks that the graph is acyclic; the error names a node on a cycle.
    Result<void> validate() const;
    /// A valid execution order (Kahn's algorithm, ties broken by node id).
    Result<std::vector<NodeId>> topologicalOrder() const;
    /// Validates, then executes all nodes on `jobs` respecting edges; blocks (helping) until done.
    /// Not reentrant: one run() of a given graph at a time.
    Result<void> run(JobSystem& jobs);

    usize nodeCount() const noexcept { return m_nodes.size(); }
    const std::string& nodeName(NodeId id) const { return m_nodes[id].name; }
    void clear();

private:
    struct Node {
        std::string name;
        std::function<void()> fn;
        Priority priority = Priority::Normal;
        std::vector<NodeId> successors;
        u32 predecessorCount = 0;
    };
    struct RunState;
    static void runNode(RunState& state, NodeId id);

    std::vector<Node> m_nodes;
};

/// Pool of threads for long-running, blocking work. FIFO within each priority. Thread-safe.
/// The destructor finishes all queued work, then joins.
class BackgroundPool {
public:
    explicit BackgroundPool(u32 threadCount = 2, std::string name = "Background",
                            ThreadPriority priority = ThreadPriority::Low);
    ~BackgroundPool();
    BackgroundPool(const BackgroundPool&) = delete;
    BackgroundPool& operator=(const BackgroundPool&) = delete;

    /// Queues `fn` for a pool thread; `counter` as for JobSystem::run (incremented now, decremented
    /// after `fn` returned and was destroyed — never let `fn` own the counter's memory).
    template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    void run(F&& fn, Counter* counter = nullptr, Priority priority = Priority::Normal) {
        submit(Job(std::forward<F>(fn), counter), priority);
    }
    void submit(Job job, Priority priority = Priority::Normal);

    /// Blocks (sleeping, not helping) until `counter` reaches zero. The counter may also be shared
    /// with JobSystem jobs (their completion is noticed within ~1 ms). Do not call from a task of
    /// this pool for work queued behind it.
    void wait(const Counter& counter);
    /// Blocks until the queue is empty and no task is running.
    void waitIdle();

    u32 threadCount() const noexcept { return static_cast<u32>(m_threads.size()); }
    usize pendingCount() const;

private:
    void workerMain();

    mutable std::mutex m_mutex;
    std::condition_variable m_workCv;
    std::condition_variable m_doneCv;
    std::deque<Job> m_queues[kPriorityCount];
    usize m_active = 0;
    bool m_stop = false;
    std::vector<Thread> m_threads;
};

} // namespace helios::jobs
