#include "helios/core/thread.h"

#include "platform/os.h"

namespace helios {

namespace {
thread_local ThreadId t_threadId = 0;
thread_local std::string t_threadName;
} // namespace

ThreadId currentThreadId() noexcept {
    if (t_threadId == 0) t_threadId = os::currentThreadId();
    return t_threadId;
}

u32 hardwareThreadCount() noexcept {
    static const u32 count = [] {
        const u32 n = os::processorCount();
        return n == 0 ? 1u : n;
    }();
    return count;
}

void setCurrentThreadName(std::string_view name) {
    t_threadName.assign(name);
    os::setCurrentThreadName(t_threadName);
}

std::string_view currentThreadName() noexcept { return t_threadName; }

bool setCurrentThreadAffinity(u64 mask) noexcept { return mask != 0 && os::setCurrentThreadAffinity(mask); }

bool setCurrentThreadPriority(ThreadPriority priority) noexcept { return os::setCurrentThreadPriority(priority); }

// ---------------------------------------------------------------------------------------------
// Semaphore / ManualResetEvent
// ---------------------------------------------------------------------------------------------

// Notifications are issued while holding the mutex: a woken waiter may destroy the primitive as
// soon as it can observe the new state (e.g. Thread's startup event lives on the creator's stack),
// so the notifier must not touch the condition variable after releasing the lock.
void Semaphore::release(u32 count) {
    if (count == 0) return;
    std::lock_guard lock(m_mutex);
    m_count += count;
    if (count == 1) {
        m_cv.notify_one();
    } else {
        m_cv.notify_all();
    }
}

void Semaphore::acquire() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_count > 0; });
    --m_count;
}

bool Semaphore::tryAcquire() {
    std::lock_guard lock(m_mutex);
    if (m_count == 0) return false;
    --m_count;
    return true;
}

bool Semaphore::tryAcquireFor(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(m_mutex);
    if (!m_cv.wait_for(lock, timeout, [this] { return m_count > 0; })) return false;
    --m_count;
    return true;
}

void ManualResetEvent::set() {
    std::lock_guard lock(m_mutex);
    m_set = true;
    m_cv.notify_all();
}

void ManualResetEvent::reset() {
    std::lock_guard lock(m_mutex);
    m_set = false;
}

bool ManualResetEvent::isSet() const {
    std::lock_guard lock(m_mutex);
    return m_set;
}

void ManualResetEvent::wait() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_set; });
}

bool ManualResetEvent::waitFor(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_cv.wait_for(lock, timeout, [this] { return m_set; });
}

namespace detail {

void threadStarted(ThreadStartup& startup) noexcept {
    if (!startup.name.empty()) setCurrentThreadName(startup.name);
    if (startup.affinityMask != 0) setCurrentThreadAffinity(startup.affinityMask);
    if (startup.priority != ThreadPriority::Normal) setCurrentThreadPriority(startup.priority);
    startup.id.store(currentThreadId(), std::memory_order_release);
    startup.started.set(); // last touch of `startup`
}

} // namespace detail
} // namespace helios
