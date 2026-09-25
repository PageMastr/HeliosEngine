#pragma once
// Threads and synchronization primitives.
//
// * Thread: std::thread wrapper that names the OS thread (SetThreadDescription /
//   pthread_setname_np), optionally pins it (affinity) and joins on destruction.
// * SpinLock (short critical sections only), Mutex/RWLock aliases, Semaphore, ManualResetEvent.
// * currentThreadId(), hardwareThreadCount(), cpuPause().
//
// Threading: all functions are thread-safe; Thread objects themselves are owned by one thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "helios/core/platform.h"
#include "helios/core/types.h"

namespace helios {

using ThreadId = u64;

/// OS thread id of the calling thread (GetCurrentThreadId / gettid). Cached per thread.
ThreadId currentThreadId() noexcept;
/// Logical processor count (>= 1).
u32 hardwareThreadCount() noexcept;
/// Names the calling thread for debuggers/profilers/logs (Linux truncates the OS name to 15 bytes;
/// currentThreadName() keeps the full name).
void setCurrentThreadName(std::string_view name);
/// Name set with setCurrentThreadName (empty if never set). Valid until the name changes.
std::string_view currentThreadName() noexcept;
/// Pins the calling thread to the CPUs in `mask` (bit i = logical CPU i; first 64 CPUs only).
bool setCurrentThreadAffinity(u64 mask) noexcept;

enum class ThreadPriority : i8 { Low = -1, Normal = 0, High = 1 };
/// Best effort; raising priority may require privileges and silently fail.
bool setCurrentThreadPriority(ThreadPriority priority) noexcept;

/// Gives up the rest of the time slice.
inline void yieldThread() noexcept { std::this_thread::yield(); }

/// CPU spin-wait hint (PAUSE / YIELD).
HELIOS_FORCEINLINE void cpuPause() noexcept {
#if defined(HELIOS_ARCH_X64)
#if defined(HELIOS_COMPILER_MSVC)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(HELIOS_ARCH_ARM64)
#if defined(HELIOS_COMPILER_MSVC)
    __yield();
#else
    __asm__ volatile("yield");
#endif
#endif
}

using Mutex = std::mutex;
using RecursiveMutex = std::recursive_mutex;
using RWLock = std::shared_mutex;
using LockGuard = std::lock_guard<std::mutex>;
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;

/// Test-and-test-and-set spin lock with PAUSE backoff that yields after a short spin, so it
/// degrades gracefully when the holder is descheduled. Satisfies Lockable (std::lock_guard etc.;
/// hence the std-style method names). Use only for very short critical sections.
class SpinLock {
public:
    void lock() noexcept {
        for (;;) {
            if (!m_flag.exchange(true, std::memory_order_acquire)) return;
            u32 spins = 0;
            while (m_flag.load(std::memory_order_relaxed)) {
                if (++spins < 64) {
                    cpuPause();
                } else {
                    yieldThread();
                }
            }
        }
    }
    bool try_lock() noexcept {
        return !m_flag.load(std::memory_order_relaxed) && !m_flag.exchange(true, std::memory_order_acquire);
    }
    void unlock() noexcept { m_flag.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_flag{false};
};

/// Counting semaphore (mutex + condition variable; portable across all toolchains).
class Semaphore {
public:
    explicit Semaphore(u32 initial = 0) noexcept : m_count(initial) {}
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void release(u32 count = 1);
    void acquire();
    bool tryAcquire();
    bool tryAcquireFor(std::chrono::nanoseconds timeout);

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    u64 m_count;
};

/// Event that stays signaled until reset(); wakes all waiters.
class ManualResetEvent {
public:
    explicit ManualResetEvent(bool initiallySet = false) noexcept : m_set(initiallySet) {}
    ManualResetEvent(const ManualResetEvent&) = delete;
    ManualResetEvent& operator=(const ManualResetEvent&) = delete;

    void set();
    void reset();
    bool isSet() const;
    void wait();
    /// Returns true if the event was set within the timeout.
    bool waitFor(std::chrono::nanoseconds timeout);

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_set;
};

namespace detail {
struct ThreadStartup {
    std::string name;
    u64 affinityMask = 0;
    ThreadPriority priority = ThreadPriority::Normal;
    ManualResetEvent started;
    std::atomic<ThreadId> id{0};
};
/// Runs on the new thread: applies name/affinity/priority, publishes the id, signals `started`.
/// `startup` must not be touched after this returns (the creator may have destroyed it).
void threadStarted(ThreadStartup& startup) noexcept;
} // namespace detail

/// Named, joinable OS thread. The constructor returns once the thread is running and named.
/// The destructor joins (never std::terminate on a joinable thread).
class Thread {
public:
    struct Options {
        u64 affinityMask = 0; ///< 0 = no pinning.
        ThreadPriority priority = ThreadPriority::Normal;
    };

    Thread() noexcept = default;

    template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    Thread(std::string name, F&& fn) : Thread(std::move(name), Options{}, std::forward<F>(fn)) {}

    template <class F>
        requires std::is_invocable_v<std::decay_t<F>&>
    Thread(std::string name, const Options& options, F&& fn) : m_name(std::move(name)) {
        detail::ThreadStartup startup;
        startup.name = m_name;
        startup.affinityMask = options.affinityMask;
        startup.priority = options.priority;
        m_thread = std::thread([&startup, f = std::decay_t<F>(std::forward<F>(fn))]() mutable {
            detail::threadStarted(startup);
            f();
        });
        startup.started.wait();
        m_id = startup.id.load(std::memory_order_acquire);
    }

    ~Thread() { join(); }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept
        : m_thread(std::move(other.m_thread)), m_id(std::exchange(other.m_id, 0)), m_name(std::move(other.m_name)) {}
    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            join();
            m_thread = std::move(other.m_thread);
            m_id = std::exchange(other.m_id, 0);
            m_name = std::move(other.m_name);
        }
        return *this;
    }

    bool joinable() const noexcept { return m_thread.joinable(); }
    void join() {
        if (m_thread.joinable()) m_thread.join();
    }
    /// OS thread id (0 for a default-constructed Thread).
    ThreadId id() const noexcept { return m_id; }
    const std::string& name() const noexcept { return m_name; }

private:
    std::thread m_thread;
    ThreadId m_id = 0;
    std::string m_name;
};

} // namespace helios
