#pragma once
// Future<T> / Promise<T>: the completion of an asynchronous control-plane call (fence operations,
// 04 §6.1). The simulation never waits on one: there is deliberately no blocking wait. A tick
// either polls isReady() when it drains its inbox, or registers then() to post the result into
// the zone's inbox, so replies are applied at a tick boundary in a recorded order (04 §10.2).
//
//   Promise<FenceResult> p;
//   Future<FenceResult> f = p.future();
//   f.then([&](const FenceResult& r) { inbox.post(r); });   // runs on the completing thread
//   p.set(FenceResult{...});
//
// Threading: Promise::set() and Future::then()/isReady()/get() may be called from any threads;
// the shared state is locked. then() continuations run on the thread that calls set(), or on the
// calling thread when the value is already there; they run outside the lock. A Promise is set at
// most once (a second set() is ignored and asserted in development builds).

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "helios/core/assert.h"

namespace helios::authority {

namespace detail {
template <class T>
struct FutureState {
    std::mutex mutex;
    std::optional<T> value;
    std::vector<std::function<void(const T&)>> continuations;
};
} // namespace detail

template <class T>
class Future {
public:
    Future() = default;

    /// False for a default-constructed Future (no Promise behind it).
    bool isValid() const noexcept { return m_state != nullptr; }
    bool isReady() const {
        if (!m_state) return false;
        std::lock_guard lock(m_state->mutex);
        return m_state->value.has_value();
    }
    /// The value; the Future must be ready (asserted). Never blocks.
    const T& get() const {
        HELIOS_ASSERT(isReady(), "Future::get() before the value is ready (futures are never waited on)");
        return *m_state->value;
    }
    /// Calls `fn(value)` once the value is set: now (on this thread) if it already is, otherwise on
    /// the thread that sets it.
    void then(std::function<void(const T&)> fn) const {
        HELIOS_ASSERT(m_state != nullptr, "then() on an empty Future");
        if (!m_state) return;
        std::unique_lock lock(m_state->mutex);
        if (m_state->value) {
            lock.unlock();
            fn(*m_state->value);
            return;
        }
        m_state->continuations.push_back(std::move(fn));
    }

private:
    template <class>
    friend class Promise;
    explicit Future(std::shared_ptr<detail::FutureState<T>> state) : m_state(std::move(state)) {}
    std::shared_ptr<detail::FutureState<T>> m_state;
};

template <class T>
class Promise {
public:
    Promise() : m_state(std::make_shared<detail::FutureState<T>>()) {}

    Future<T> future() const { return Future<T>(m_state); }
    /// Stores the value and runs the continuations (on this thread, outside the lock).
    void set(T value) {
        std::vector<std::function<void(const T&)>> run;
        {
            std::lock_guard lock(m_state->mutex);
            HELIOS_ASSERT(!m_state->value.has_value(), "Promise set twice");
            if (m_state->value) return;
            m_state->value.emplace(std::move(value));
            run.swap(m_state->continuations);
        }
        for (auto& fn : run) fn(*m_state->value);
    }
    bool isSet() const {
        std::lock_guard lock(m_state->mutex);
        return m_state->value.has_value();
    }

private:
    std::shared_ptr<detail::FutureState<T>> m_state;
};

/// A Future that is already complete.
template <class T>
Future<T> makeReadyFuture(T value) {
    Promise<T> p;
    p.set(std::move(value));
    return p.future();
}

} // namespace helios::authority
