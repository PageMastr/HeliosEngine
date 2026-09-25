#include "helios/core/jobs.h"

#include <algorithm>
#include <format>
#include <queue>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/log.h"
#include "helios/core/memory.h"
#include "helios/core/time.h"

#if defined(HELIOS_COMPILER_MSVC)
#pragma warning(disable : 4324) // structure padded due to alignment specifier (intended: false sharing)
#endif

namespace helios::jobs {

namespace {

constinit std::atomic<u64> g_heapJobCount{0};

/// Lock-protected ring deque of Jobs. The owner pushes/pops at the back (LIFO, cache-warm);
/// thieves and the global queue consumers pop at the front (FIFO). A seq_cst size hint lets
/// scanners skip empty queues without taking the lock and pairs with the sleeper count in the
/// wake-up protocol (see JobSystem::Impl::workerMain).
class JobQueue {
public:
    JobQueue() : m_slots(std::make_unique<Job[]>(kInitialCapacity)), m_mask(kInitialCapacity - 1) {}

    void push(Job&& job) {
        std::lock_guard lock(m_lock);
        if (m_tail - m_head == m_mask + 1) grow();
        m_slots[m_tail & m_mask] = std::move(job);
        ++m_tail;
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
    }

    bool popBack(Job& out) {
        if (m_size.load(std::memory_order_seq_cst) <= 0) return false;
        std::lock_guard lock(m_lock);
        if (m_tail == m_head) return false;
        --m_tail;
        out = std::move(m_slots[m_tail & m_mask]);
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
        return true;
    }

    /// Owner only: pops the newest job if it was pushed at or above index `floor` (i.e. after the
    /// owner's current job started), so a deeply nested waiter only runs its own descendants.
    bool popBackAbove(u64 floor, Job& out) {
        if (m_size.load(std::memory_order_seq_cst) <= 0) return false;
        std::lock_guard lock(m_lock);
        if (m_tail == m_head || m_tail <= floor) return false;
        --m_tail;
        out = std::move(m_slots[m_tail & m_mask]);
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
        return true;
    }

    /// Owner only: absolute index of the next push. Only the owner moves the tail (thieves move
    /// the head), so the owner may read it without the lock.
    u64 ownerTail() const noexcept { return m_tail; }

    bool popFront(Job& out) {
        if (m_size.load(std::memory_order_seq_cst) <= 0) return false;
        std::lock_guard lock(m_lock);
        if (m_tail == m_head) return false;
        out = std::move(m_slots[m_head & m_mask]);
        ++m_head;
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
        return true;
    }

    /// Pops up to `max` jobs from the front under a single lock acquisition.
    usize popFrontMany(Job* out, usize max) {
        if (m_size.load(std::memory_order_seq_cst) <= 0) return 0;
        std::lock_guard lock(m_lock);
        usize n = 0;
        while (n < max && m_head != m_tail) {
            out[n++] = std::move(m_slots[m_head & m_mask]);
            ++m_head;
        }
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
        return n;
    }

    /// Pushes several jobs at the back under a single lock acquisition.
    void pushMany(Job* jobs, usize count) {
        if (count == 0) return;
        std::lock_guard lock(m_lock);
        for (usize i = 0; i < count; ++i) {
            if (m_tail - m_head == m_mask + 1) grow();
            m_slots[m_tail & m_mask] = std::move(jobs[i]);
            ++m_tail;
        }
        m_size.store(static_cast<i64>(m_tail - m_head), std::memory_order_seq_cst);
    }

    i64 sizeHint() const noexcept { return m_size.load(std::memory_order_seq_cst); }

private:
    static constexpr u64 kInitialCapacity = 256;

    void grow() {
        const u64 oldCapacity = m_mask + 1;
        const u64 newCapacity = oldCapacity * 2;
        auto slots = std::make_unique<Job[]>(newCapacity);
        for (u64 i = m_head; i != m_tail; ++i) slots[i & (newCapacity - 1)] = std::move(m_slots[i & m_mask]);
        m_slots = std::move(slots);
        m_mask = newCapacity - 1;
    }

    SpinLock m_lock;
    std::unique_ptr<Job[]> m_slots;
    u64 m_mask;
    u64 m_head = 0;
    u64 m_tail = 0;
    std::atomic<i64> m_size{0};
};

struct alignas(HELIOS_CACHE_LINE_SIZE) WorkerData {
    JobQueue queues[kPriorityCount];
    // Written only by the owning worker (plain load+store, no locked RMW); read by stats().
    std::atomic<u64> executed{0};
    std::atomic<u64> stolen{0};
    std::atomic<u64> sleeps{0};
};

/// Sleep/wake signal for idle workers. Tokens are capped at the number of sleepers so a burst of
/// submissions cannot bank thousands of spurious wake-ups.
class WakeSignal {
public:
    void notify(u32 maxTokens) {
        {
            std::lock_guard lock(m_mutex);
            if (m_tokens >= maxTokens) return;
            ++m_tokens;
        }
        m_cv.notify_one();
    }
    void notifyAll(u32 tokens) {
        {
            std::lock_guard lock(m_mutex);
            m_tokens += tokens;
        }
        m_cv.notify_all();
    }
    void wait() {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_tokens > 0; });
        --m_tokens;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    u32 m_tokens = 0;
};

inline u32 xorshift32(u32& state) noexcept {
    u32 x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

inline void bump(std::atomic<u64>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/// Backoff for threads waiting on a counter when there is nothing to help with.
inline void waitBackoff(u32 idleRounds) noexcept {
    if (idleRounds < 64) {
        cpuPause();
    } else if (idleRounds < 512) {
        yieldThread();
    } else {
        sleepNanos(50'000);
    }
}

// Help-while-waiting runs other jobs on the waiting thread's stack, and those may wait (and help)
// in turn. Unbounded, a burst of jobs waiting on one not-yet-signaled counter nests once per job
// and overflows the stack (10k such jobs already overflow a 1 MiB Windows thread stack). Beyond
// these limits a waiting thread only runs jobs spawned by the job it waits in (TBB-style
// stack-limited stealing), which bounds nesting by the program's own fork/join recursion depth.
constexpr uptr kHelpStackBudget = 256 * 1024;
constexpr u32 kMaxHelpDepth = 256;

/// Approximate stack pointer of the caller (not affected by ASan fake stacks).
HELIOS_FORCEINLINE uptr currentStackAddress() noexcept {
#if defined(HELIOS_COMPILER_MSVC)
    return reinterpret_cast<uptr>(_AddressOfReturnAddress());
#else
    return reinterpret_cast<uptr>(__builtin_frame_address(0));
#endif
}

/// Per-thread job-system state. A thread is a worker of at most one JobSystem.
struct ThreadContext {
    JobSystem::Impl* impl = nullptr; ///< System this thread is a worker of (nullptr otherwise).
    i32 index = -1;                  ///< Worker index in `impl`.
    u32 waitDepth = 0;               ///< Nested wait() calls on this thread (any JobSystem).
    uptr stackBase = 0;              ///< Stack address at worker entry / the outermost wait().
    u64 floor[kPriorityCount] = {};  ///< Own-deque tails when the running job started.
};

thread_local ThreadContext t_context;

} // namespace

void* Job::allocateHeapCallable(usize size, usize alignment) {
    g_heapJobCount.fetch_add(1, std::memory_order_relaxed);
    void* p = alignedAlloc(size, std::max<usize>(alignment, 16), MemoryTag::Jobs);
    if (!p) throw std::bad_alloc();
    return p;
}

void Job::freeHeapCallable(void* ptr) noexcept { alignedFree(ptr); }

u64 jobHeapAllocationCount() noexcept { return g_heapJobCount.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------
// JobSystem
// ---------------------------------------------------------------------------------------------

struct JobSystem::Impl {
    u32 workerCount = 0;
    std::unique_ptr<WorkerData[]> workers;
    JobQueue global[kPriorityCount];
    std::vector<Thread> threads;
    std::atomic<bool> running{true};
    std::atomic<u32> sleepers{0};
    WakeSignal wake;
    std::atomic<i64> pending{0};
    std::atomic<u64> externalExecuted{0};
    std::atomic<u64> externalStolen{0};
    std::atomic<u64> limitedWaits{0};
    std::unique_ptr<BackgroundPool> background;

    // Max jobs a worker moves from a global queue into its own deque per visit. Batching keeps
    // the global queue's lock from becoming the bottleneck when an external thread submits
    // many small jobs; the moved jobs remain stealable by other workers.
    static constexpr usize kGlobalBatch = 32;

    bool popGlobal(u32 p, i32 self, Job& out) {
        if (self < 0) return global[p].popFront(out);
        const i64 queued = global[p].sizeHint();
        if (queued <= 1) return global[p].popFront(out);
        usize want = static_cast<usize>(queued) / (workerCount + 1);
        want = std::clamp<usize>(want, 1, kGlobalBatch);
        Job batch[kGlobalBatch];
        const usize got = global[p].popFrontMany(batch, want);
        if (got == 0) return false;
        out = std::move(batch[0]);
        // Reverse so the owner's LIFO pops preserve submission order within the batch.
        std::reverse(batch + 1, batch + got);
        workers[self].queues[p].pushMany(batch + 1, got - 1);
        return true;
    }

    bool findJob(i32 self, Job& out, u32& rng) {
        for (u32 p = 0; p < kPriorityCount; ++p) {
            if (self >= 0 && workers[self].queues[p].popBack(out)) return true;
            if (popGlobal(p, self, out)) return true;
            const u32 start = xorshift32(rng) % workerCount;
            for (u32 k = 0; k < workerCount; ++k) {
                u32 victim = start + k;
                if (victim >= workerCount) victim -= workerCount;
                if (static_cast<i32>(victim) == self) continue;
                if (workers[victim].queues[p].popFront(out)) {
                    if (self >= 0) {
                        bump(workers[self].stolen);
                    } else {
                        externalStolen.fetch_add(1, std::memory_order_relaxed);
                    }
                    return true;
                }
            }
        }
        return false;
    }

    /// Restricted helping for deeply nested waits: only jobs the current job pushed itself.
    bool popOwnDescendant(i32 self, Job& out) {
        if (self < 0) return false;
        for (u32 p = 0; p < kPriorityCount; ++p) {
            if (workers[self].queues[p].popBackAbove(t_context.floor[p], out)) return true;
        }
        return false;
    }

    void execute(Job& job, i32 self) {
        Counter* counter = job.counter();
        if (self >= 0) {
            // Record where this job's own deque entries start (see popOwnDescendant).
            u64 saved[kPriorityCount];
            for (u32 p = 0; p < kPriorityCount; ++p) {
                saved[p] = t_context.floor[p];
                t_context.floor[p] = workers[self].queues[p].ownerTail();
            }
            job.invoke();
            for (u32 p = 0; p < kPriorityCount; ++p) t_context.floor[p] = saved[p];
        } else {
            job.invoke();
        }
        if (counter) counter->decrement();
        if (self >= 0) {
            bump(workers[self].executed);
        } else {
            externalExecuted.fetch_add(1, std::memory_order_relaxed);
        }
        pending.fetch_sub(1, std::memory_order_acq_rel);
    }

    void wakeOne() {
        const u32 s = sleepers.load(std::memory_order_seq_cst);
        if (s > 0) wake.notify(s);
    }

    void workerMain(u32 index);
};

namespace {

thread_local u32 t_rng = 0;

u32& threadRng() noexcept {
    if (t_rng == 0) t_rng = static_cast<u32>(mix64(currentThreadId())) | 1u;
    return t_rng;
}

} // namespace

void JobSystem::Impl::workerMain(u32 index) {
    t_context.impl = this;
    t_context.index = static_cast<i32>(index);
    t_context.stackBase = currentStackAddress();
    u32& rng = threadRng();
    const i32 self = static_cast<i32>(index);
    Job job;
    u32 idleRounds = 0;
    for (;;) {
        if (findJob(self, job, rng)) {
            execute(job, self);
            idleRounds = 0;
            continue;
        }
        if (!running.load(std::memory_order_acquire)) break;
        if (++idleRounds < 32) {
            for (int i = 0; i < 32; ++i) cpuPause();
            continue;
        }
        if (idleRounds < 40) {
            yieldThread();
            continue;
        }
        // Sleep protocol (Dekker-style with the producer in submit()):
        //   worker:   sleepers++ (seq_cst)  ->  re-scan queues (seq_cst size loads)  ->  wait
        //   producer: push (seq_cst size store)  ->  load sleepers (seq_cst)  ->  notify if > 0
        // In the single total order either the re-scan sees the job or the producer sees us.
        sleepers.fetch_add(1, std::memory_order_seq_cst);
        if (findJob(self, job, rng)) {
            sleepers.fetch_sub(1, std::memory_order_seq_cst);
            execute(job, self);
            idleRounds = 0;
            continue;
        }
        if (!running.load(std::memory_order_seq_cst)) {
            sleepers.fetch_sub(1, std::memory_order_seq_cst);
            break;
        }
        bump(workers[index].sleeps);
        wake.wait();
        sleepers.fetch_sub(1, std::memory_order_seq_cst);
        idleRounds = 0;
    }
    t_context = ThreadContext{};
}

JobSystem::JobSystem(const JobSystemDesc& desc) : m_impl(std::make_unique<Impl>()) {
    Impl& impl = *m_impl;
    const u32 hw = hardwareThreadCount();
    impl.workerCount = desc.workerCount != 0 ? desc.workerCount : std::max<u32>(1, hw - 1);
    impl.workers = std::make_unique<WorkerData[]>(impl.workerCount);
    impl.threads.reserve(impl.workerCount);
    for (u32 i = 0; i < impl.workerCount; ++i) {
        Thread::Options options;
        if (desc.pinWorkers) {
            const u32 cpus = std::min<u32>(hw, 64);
            options.affinityMask = 1ull << ((i + 1) % cpus);
        }
        Impl* implPtr = &impl;
        impl.threads.emplace_back(std::format("{} Worker {}", desc.name, i), options,
                                  [implPtr, i] { implPtr->workerMain(i); });
    }
    if (desc.backgroundThreadCount > 0) {
        impl.background = std::make_unique<BackgroundPool>(desc.backgroundThreadCount, desc.name + " Background");
    }
    HELIOS_LOG_DEBUG(LogJobs, "JobSystem '{}' started: {} workers, {} background threads", desc.name,
                     impl.workerCount, desc.backgroundThreadCount);
}

JobSystem::~JobSystem() {
    Impl& impl = *m_impl;
    // Background tasks may submit frame jobs, so drain them first; then drain our own queues.
    impl.background.reset();
    waitIdle();
    impl.running.store(false, std::memory_order_seq_cst);
    impl.wake.notifyAll(impl.workerCount);
    impl.threads.clear(); // joins
}

void JobSystem::submit(Job job, Priority priority) {
    if (!job) return;
    Impl& impl = *m_impl;
    if (Counter* counter = job.counter()) counter->add(1);
    impl.pending.fetch_add(1, std::memory_order_relaxed);
    const u32 p = static_cast<u32>(priority) < kPriorityCount ? static_cast<u32>(priority) : 1u;
    if (t_context.impl == &impl) {
        impl.workers[t_context.index].queues[p].push(std::move(job));
    } else {
        impl.global[p].push(std::move(job));
    }
    impl.wakeOne();
}

void JobSystem::wait(const Counter& counter) {
    if (counter.isDone()) return;
    Impl& impl = *m_impl;
    const i32 self = t_context.impl == &impl ? t_context.index : -1;
    u32& rng = threadRng();

    // Decide once per wait whether this thread may still help with arbitrary jobs.
    ThreadContext& ctx = t_context;
    const uptr here = currentStackAddress();
    if (ctx.waitDepth == 0 && ctx.impl == nullptr) ctx.stackBase = here; // non-worker: measure from here
    const uptr stackUsed = ctx.stackBase > here ? ctx.stackBase - here : 0;
    const bool limited = ctx.waitDepth >= kMaxHelpDepth || stackUsed > kHelpStackBudget;
    if (limited) impl.limitedWaits.fetch_add(1, std::memory_order_relaxed);
    struct DepthScope {
        u32& depth;
        explicit DepthScope(u32& d) noexcept : depth(d) { ++depth; }
        ~DepthScope() { --depth; }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
    } depthScope(ctx.waitDepth);

    Job job;
    u32 idleRounds = 0;
    while (!counter.isDone()) {
        const bool found = limited ? impl.popOwnDescendant(self, job) : impl.findJob(self, job, rng);
        if (found) {
            impl.execute(job, self);
            idleRounds = 0;
        } else {
            waitBackoff(idleRounds++);
        }
    }
}

void JobSystem::waitIdle() {
    Impl& impl = *m_impl;
    const i32 self = t_context.impl == &impl ? t_context.index : -1;
    u32& rng = threadRng();
    Job job;
    u32 idleRounds = 0;
    // A job calling waitIdle() would wait for itself (it is still pending).
    HELIOS_ASSERT(self < 0, "JobSystem::waitIdle() must not be called from a worker thread");
    while (impl.pending.load(std::memory_order_acquire) > 0) {
        if (impl.findJob(self, job, rng)) {
            impl.execute(job, self);
            idleRounds = 0;
        } else {
            waitBackoff(idleRounds++);
        }
    }
}

bool JobSystem::tryRunOne() {
    Impl& impl = *m_impl;
    const i32 self = t_context.impl == &impl ? t_context.index : -1;
    Job job;
    if (!impl.findJob(self, job, threadRng())) return false;
    impl.execute(job, self);
    return true;
}

u32 JobSystem::workerCount() const noexcept { return m_impl->workerCount; }

bool JobSystem::isWorkerThread() const noexcept { return t_context.impl == m_impl.get(); }

i32 JobSystem::currentWorkerIndex() const noexcept { return t_context.impl == m_impl.get() ? t_context.index : -1; }

BackgroundPool* JobSystem::background() noexcept { return m_impl->background.get(); }

JobSystemStats JobSystem::stats() const noexcept {
    const Impl& impl = *m_impl;
    JobSystemStats s;
    for (u32 i = 0; i < impl.workerCount; ++i) {
        s.executed += impl.workers[i].executed.load(std::memory_order_relaxed);
        s.stolen += impl.workers[i].stolen.load(std::memory_order_relaxed);
        s.sleeps += impl.workers[i].sleeps.load(std::memory_order_relaxed);
    }
    s.executed += impl.externalExecuted.load(std::memory_order_relaxed);
    s.stolen += impl.externalStolen.load(std::memory_order_relaxed);
    s.limitedWaits = impl.limitedWaits.load(std::memory_order_relaxed);
    s.heapJobs = jobHeapAllocationCount();
    return s;
}

// ---------------------------------------------------------------------------------------------
// TaskGraph
// ---------------------------------------------------------------------------------------------

struct TaskGraph::RunState {
    TaskGraph* graph = nullptr;
    JobSystem* jobs = nullptr;
    std::unique_ptr<std::atomic<u32>[]> remaining;
    Counter done;
};

TaskGraph::NodeId TaskGraph::addNode(std::string name, std::function<void()> fn, Priority priority) {
    const auto id = static_cast<NodeId>(m_nodes.size());
    Node node;
    node.name = std::move(name);
    node.fn = std::move(fn);
    node.priority = priority;
    m_nodes.push_back(std::move(node));
    return id;
}

Result<void> TaskGraph::addEdge(NodeId before, NodeId after) {
    if (before >= m_nodes.size() || after >= m_nodes.size()) {
        return makeError(ErrorCode::InvalidArgument, "TaskGraph::addEdge: invalid node id ({} -> {})", before, after);
    }
    if (before == after) {
        return makeError(ErrorCode::InvalidArgument, "TaskGraph::addEdge: self-dependency on '{}'",
                         m_nodes[before].name);
    }
    auto& successors = m_nodes[before].successors;
    if (std::find(successors.begin(), successors.end(), after) != successors.end()) return {};
    successors.push_back(after);
    ++m_nodes[after].predecessorCount;
    return {};
}

Result<std::vector<TaskGraph::NodeId>> TaskGraph::topologicalOrder() const {
    const usize n = m_nodes.size();
    std::vector<u32> indegree(n);
    std::priority_queue<NodeId, std::vector<NodeId>, std::greater<>> ready;
    for (usize i = 0; i < n; ++i) {
        indegree[i] = m_nodes[i].predecessorCount;
        if (indegree[i] == 0) ready.push(static_cast<NodeId>(i));
    }
    std::vector<NodeId> order;
    order.reserve(n);
    while (!ready.empty()) {
        const NodeId id = ready.top();
        ready.pop();
        order.push_back(id);
        for (const NodeId s : m_nodes[id].successors) {
            if (--indegree[s] == 0) ready.push(s);
        }
    }
    if (order.size() != n) {
        for (usize i = 0; i < n; ++i) {
            if (indegree[i] != 0) {
                return makeError(ErrorCode::InvalidState, "TaskGraph has a cycle involving '{}'", m_nodes[i].name);
            }
        }
    }
    return order;
}

Result<void> TaskGraph::validate() const {
    HELIOS_TRY(topologicalOrder());
    return {};
}

void TaskGraph::runNode(RunState& state, NodeId id) {
    const Node& node = state.graph->m_nodes[id];
    if (node.fn) node.fn();
    for (const NodeId s : node.successors) {
        if (state.remaining[s].fetch_sub(1, std::memory_order_acq_rel) == 1) {
            RunState* statePtr = &state;
            state.jobs->run([statePtr, s] { runNode(*statePtr, s); }, nullptr, state.graph->m_nodes[s].priority);
        }
    }
    state.done.decrement(); // last touch of `state`: run() may return right after this
}

Result<void> TaskGraph::run(JobSystem& jobs) {
    HELIOS_TRY(validate());
    const usize n = m_nodes.size();
    if (n == 0) return {};
    RunState state;
    state.graph = this;
    state.jobs = &jobs;
    state.remaining = std::make_unique<std::atomic<u32>[]>(n);
    for (usize i = 0; i < n; ++i) state.remaining[i].store(m_nodes[i].predecessorCount, std::memory_order_relaxed);
    state.done.add(static_cast<u32>(n));
    RunState* statePtr = &state;
    for (usize i = 0; i < n; ++i) {
        if (m_nodes[i].predecessorCount == 0) {
            const auto id = static_cast<NodeId>(i);
            jobs.run([statePtr, id] { runNode(*statePtr, id); }, nullptr, m_nodes[i].priority);
        }
    }
    jobs.wait(state.done);
    return {};
}

void TaskGraph::clear() { m_nodes.clear(); }

// ---------------------------------------------------------------------------------------------
// BackgroundPool
// ---------------------------------------------------------------------------------------------

BackgroundPool::BackgroundPool(u32 threadCount, std::string name, ThreadPriority priority) {
    threadCount = std::max<u32>(1, threadCount);
    m_threads.reserve(threadCount);
    for (u32 i = 0; i < threadCount; ++i) {
        Thread::Options options;
        options.priority = priority;
        m_threads.emplace_back(std::format("{} {}", name, i), options, [this] { workerMain(); });
    }
}

BackgroundPool::~BackgroundPool() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    m_threads.clear(); // joins after the queues drain
}

void BackgroundPool::submit(Job job, Priority priority) {
    if (!job) return;
    const u32 p = static_cast<u32>(priority) < kPriorityCount ? static_cast<u32>(priority) : 1u;
    {
        std::lock_guard lock(m_mutex);
        if (Counter* counter = job.counter()) counter->add(1);
        m_queues[p].push_back(std::move(job));
    }
    m_workCv.notify_one();
}

void BackgroundPool::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_workCv.wait(lock, [this] {
                return m_stop || !m_queues[0].empty() || !m_queues[1].empty() || !m_queues[2].empty();
            });
            bool found = false;
            for (auto& queue : m_queues) {
                if (!queue.empty()) {
                    job = std::move(queue.front());
                    queue.pop_front();
                    found = true;
                    break;
                }
            }
            if (!found) return; // stopping and drained
            ++m_active;
        }
        Counter* counter = job.counter();
        job.invoke();
        {
            std::lock_guard lock(m_mutex);
            --m_active;
            if (counter) counter->decrement();
        }
        m_doneCv.notify_all();
    }
}

void BackgroundPool::wait(const Counter& counter) {
    // Tasks of this pool notify m_doneCv, but the counter may also be decremented elsewhere (e.g.
    // shared with JobSystem jobs, which never touch this condition variable), so never sleep
    // unboundedly on it: re-check the counter at least every millisecond.
    std::unique_lock lock(m_mutex);
    while (!counter.isDone()) m_doneCv.wait_for(lock, std::chrono::milliseconds(1));
}

void BackgroundPool::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_doneCv.wait(lock, [this] {
        return m_active == 0 && m_queues[0].empty() && m_queues[1].empty() && m_queues[2].empty();
    });
}

usize BackgroundPool::pendingCount() const {
    std::lock_guard lock(m_mutex);
    return m_queues[0].size() + m_queues[1].size() + m_queues[2].size();
}

} // namespace helios::jobs
