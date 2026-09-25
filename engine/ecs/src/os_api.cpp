#include "helios/ecs/os_api.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

#include "flecs_internal.h"
#include "helios/core/assert.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/world.h"

namespace helios::ecs {

namespace {

std::atomic<bool> g_installed{false};
std::atomic<u64> g_mallocs{0};
std::atomic<u64> g_reallocs{0};
std::atomic<u64> g_frees{0};
std::atomic<u64> g_tasksStarted{0};
std::atomic<u64> g_tasksJoined{0};
std::atomic<u64> g_threadsStarted{0};
std::atomic<u64> g_warnings{0};
std::atomic<u64> g_errors{0};

// ---- memory ---------------------------------------------------------------------------------

void* osMalloc(ecs_size_t size) {
    g_mallocs.fetch_add(1, std::memory_order_relaxed);
    return size > 0 ? ecsHeap().allocate(static_cast<usize>(size)) : nullptr;
}

void* osCalloc(ecs_size_t size) {
    g_mallocs.fetch_add(1, std::memory_order_relaxed);
    return size > 0 ? ecsHeap().allocateZeroed(static_cast<usize>(size)) : nullptr;
}

void* osRealloc(void* ptr, ecs_size_t size) {
    g_reallocs.fetch_add(1, std::memory_order_relaxed);
    return ecsHeap().reallocate(ptr, size > 0 ? static_cast<usize>(size) : 0);
}

void osFree(void* ptr) {
    if (!ptr) return;
    g_frees.fetch_add(1, std::memory_order_relaxed);
    ecsHeap().free(ptr);
}

char* osStrdup(const char* str) {
    if (!str) return nullptr;
    const usize n = std::strlen(str) + 1;
    char* out = static_cast<char*>(ecsHeap().allocate(n));
    if (out) std::memcpy(out, str, n);
    g_mallocs.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// ---- logging / abort ------------------------------------------------------------------------

void osLog(int32_t level, const char* file, int32_t line, const char* msg) {
    const std::string_view text = msg ? std::string_view(msg) : std::string_view();
    const log::SourceLocation loc{file ? file : "flecs", static_cast<u32>(line > 0 ? line : 0), "flecs"};
    log::Level lvl = log::Level::Trace;
    if (level <= -3) {
        lvl = log::Level::Error; // -4 (fatal) is followed by ecs_os_abort()
        g_errors.fetch_add(1, std::memory_order_relaxed);
    } else if (level == -2) {
        lvl = log::Level::Warn;
        g_warnings.fetch_add(1, std::memory_order_relaxed);
    } else if (level == 0 || level == -1) {
        lvl = log::Level::Debug;
    }
    if (!log::isEnabled(lvl, LogEcs)) return;
    log::writeMessage(lvl, LogEcs, loc, text);
}

void osAbort() {
    // Route through the assert handler (tests/editor may intercept), then terminate: flecs does not
    // expect ecs_os_abort to return.
    const AssertAction action =
        ::helios::detail::reportAssertFailure("FLECS", "ecs_os_abort()", "flecs", 0, "flecs", "flecs aborted (see log)");
    if (action == AssertAction::Break) HELIOS_DEBUG_BREAK();
    ::helios::detail::assertAbort();
}

// ---- time -----------------------------------------------------------------------------------

uint64_t osNow() { return monotonicNanos(); }

void osGetTime(ecs_time_t* out) {
    const u64 ns = monotonicNanos();
    out->sec = static_cast<uint32_t>(ns / 1'000'000'000ull);
    out->nanosec = static_cast<uint32_t>(ns % 1'000'000'000ull);
}

void osSleep(int32_t sec, int32_t nanosec) {
    const i64 total = static_cast<i64>(sec) * 1'000'000'000ll + nanosec;
    if (total > 0) sleepNanos(static_cast<u64>(total));
}

// ---- threads (long-lived; used only by ecs_set_threads) ----------------------------------------

struct OsThread {
    Thread thread;
    void* result = nullptr;
};

ecs_os_thread_t osThreadNew(ecs_os_thread_callback_t callback, void* param) {
    g_threadsStarted.fetch_add(1, std::memory_order_relaxed);
    auto* t = new OsThread();
    t->thread = Thread("flecs worker", [t, callback, param] { t->result = callback(param); });
    return reinterpret_cast<ecs_os_thread_t>(t);
}

void* osThreadJoin(ecs_os_thread_t handle) {
    auto* t = reinterpret_cast<OsThread*>(handle);
    if (!t) return nullptr;
    t->thread.join();
    void* result = t->result;
    delete t;
    return result;
}

ecs_os_thread_id_t osThreadSelf() { return static_cast<ecs_os_thread_id_t>(currentThreadId()); }

// ---- tasks (ecs_set_task_threads) -> JobSystem jobs ---------------------------------------------

struct OsTask {
    jobs::Counter counter;
    jobs::JobSystem* jobs = nullptr;
    void* result = nullptr;
};

ecs_os_thread_t osTaskNew(ecs_os_thread_callback_t callback, void* param) {
    // `param` is the worker's ecs_stage_t; its world's ctx is the owning helios::ecs::World.
    const ecs_world_t* world = ecs_get_world(param);
    auto* owner = world ? static_cast<World*>(ecs_get_ctx(world)) : nullptr;
    jobs::JobSystem* js = owner ? owner->jobs() : nullptr;
    auto* task = new OsTask();
    task->jobs = js;
    g_tasksStarted.fetch_add(1, std::memory_order_relaxed);
    if (!js) {
        // No job system (should not happen: World refuses task threads without one): run inline
        // on a helper thread so flecs' handshake still completes.
        HELIOS_LOG_ERROR(LogEcs, "flecs task requested for a world without a JobSystem; using an OS thread");
        auto* t = reinterpret_cast<OsThread*>(osThreadNew(callback, param));
        task->result = t;
        return reinterpret_cast<ecs_os_thread_t>(task);
    }
    js->run([task, callback, param] { task->result = callback(param); }, &task->counter, jobs::Priority::High);
    return reinterpret_cast<ecs_os_thread_t>(task);
}

void* osTaskJoin(ecs_os_thread_t handle) {
    auto* task = reinterpret_cast<OsTask*>(handle);
    if (!task) return nullptr;
    void* result = nullptr;
    if (task->jobs) {
        task->jobs->wait(task->counter);
        result = task->result;
    } else {
        result = osThreadJoin(reinterpret_cast<ecs_os_thread_t>(task->result));
    }
    delete task;
    g_tasksJoined.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::mutex& installMutex() {
    static std::mutex m;
    return m;
}

} // namespace

bool installFlecsOsApi() noexcept {
    if (g_installed.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(installMutex());
    if (g_installed.load(std::memory_order_relaxed)) return true;

    // flecs' default allocator counts its calls: if anything was allocated before us, switching the
    // allocator now would free foreign blocks through mimalloc. Refuse and keep the defaults.
    if (ecs_os_api_malloc_count != 0 || ecs_os_api_calloc_count != 0 || ecs_os_api_realloc_count != 0) {
        HELIOS_LOG_WARN(LogEcs, "flecs allocated memory before installFlecsOsApi(); keeping flecs defaults");
        return false;
    }
    // Populate flecs' defaults first (mutex/cond/dl/fopen implementations we keep), then override.
    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_get_api();
    api.malloc_ = osMalloc;
    api.calloc_ = osCalloc;
    api.realloc_ = osRealloc;
    api.free_ = osFree;
    api.strdup_ = osStrdup;
    api.log_ = osLog;
    api.abort_ = osAbort;
    api.now_ = osNow;
    api.get_time_ = osGetTime;
    api.sleep_ = osSleep;
    api.thread_new_ = osThreadNew;
    api.thread_join_ = osThreadJoin;
    api.thread_self_ = osThreadSelf;
    api.task_new_ = osTaskNew;
    api.task_join_ = osTaskJoin;
    api.flags_ &= ~static_cast<ecs_flags32_t>(EcsOsApiLogWithColors);
    ecs_os_set_api(&api);

    const bool ok = ecs_os_api.malloc_ == osMalloc;
    if (!ok) {
        HELIOS_LOG_WARN(LogEcs, "flecs OS API was initialized before installFlecsOsApi(); keeping flecs defaults");
    }
    g_installed.store(ok, std::memory_order_release);
    return ok;
}

bool flecsOsApiInstalled() noexcept { return g_installed.load(std::memory_order_acquire); }

FlecsOsApiStats flecsOsApiStats() noexcept {
    FlecsOsApiStats s;
    s.mallocs = g_mallocs.load(std::memory_order_relaxed);
    s.reallocs = g_reallocs.load(std::memory_order_relaxed);
    s.frees = g_frees.load(std::memory_order_relaxed);
    s.tasksStarted = g_tasksStarted.load(std::memory_order_relaxed);
    s.tasksJoined = g_tasksJoined.load(std::memory_order_relaxed);
    s.threadsStarted = g_threadsStarted.load(std::memory_order_relaxed);
    s.logWarnings = g_warnings.load(std::memory_order_relaxed);
    s.logErrors = g_errors.load(std::memory_order_relaxed);
    return s;
}

} // namespace helios::ecs
