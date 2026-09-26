// Asynchronous file reads on a BackgroundPool (async_io.h). Phase 0 backend: one blocking
// positional read per request on an IO thread; see the header for the IORing / io_uring plan.
#include "helios/core/async_io.h"

#include <algorithm>
#include <mutex>
#include <new>

namespace helios::fs {

namespace detail {

struct AsyncReadState {
    enum Phase : u8 { Queued = 0, Running = 1, Cancelled = 2 };

    // The one completion signal: released once the result is stored and onComplete has returned.
    // isReady(), wait(), take(), bytesRead() and JobSystem::wait all go by it (see finish()).
    jobs::Counter counter;
    jobs::BackgroundPool* pool = nullptr;
    std::atomic<u8> phase{Queued};
    std::function<void(AsyncRead&)> onComplete;
    // Written by the IO thread before onComplete runs and `counter` is released; other threads read
    // them only after observing the counter at zero (acquire).
    Result<std::vector<u8>> bytes;
    Result<usize> count{usize{0}};
    std::mutex takeMutex;
    bool taken = false;
};

} // namespace detail

namespace {

using State = detail::AsyncReadState;

// The request whose onComplete is running (or being destroyed) on this thread. Its result is already
// stored, so here (and only here) it counts as ready before its counter is released: take()/bytesRead()
// inside the callback must not wait for the callback itself.
constinit thread_local const State* t_completing = nullptr;

// True once the result may be read on this thread: the request is complete, or we are inside its
// onComplete.
bool isComplete(const State& st) noexcept { return st.counter.isDone() || t_completing == &st; }

// Returns false if the request was cancelled before it started (the result is then stored).
bool begin(State& st) {
    u8 expected = State::Queued;
    if (st.phase.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) return true;
    st.bytes = Error{ErrorCode::Cancelled, "async read cancelled"};
    st.count = Error{ErrorCode::Cancelled, "async read cancelled"};
    return false;
}

// Runs onComplete, destroys it, and only then releases the counter, which is what makes the request
// complete for every other thread: whoever sees it ready (by any of isReady, wait, take, bytesRead or
// the counter) also sees everything the callback did, and may destroy what it captured. (Publishing
// readiness before the callback let a waiter return while the callback was still running.)
// Runs inside the IO job, whose lambda owns a reference to `st`: the counter is released while the
// state is certainly alive. (Handing &st->counter to BackgroundPool::run would let the pool decrement
// it after the job, and with it possibly the last reference to the state, had been destroyed: a
// use-after-free for fire-and-forget requests whose caller dropped every AsyncRead.)
void finish(const std::shared_ptr<State>& st) {
    // Moved out so a callback that captures an AsyncRead of its own request cannot keep the state
    // alive through a reference cycle.
    std::function<void(AsyncRead&)> onComplete = std::move(st->onComplete);
    st->onComplete = nullptr;
    if (onComplete) {
        const State* const outer = t_completing;
        t_completing = st.get();
        {
            AsyncRead handle(st);
            onComplete(handle);
        }
        onComplete = nullptr; // captures die before the request completes (still "inside" it)
        t_completing = outer;
    }
    st->counter.decrement();
}

// Sizes `data` to `bytes` (a file range, so possibly larger than memory); false instead of an
// exception escaping the IO job.
bool reserveBytes(std::vector<u8>& data, u64 bytes) {
    if (bytes > static_cast<u64>(data.max_size())) return false;
    try {
        data.resize(static_cast<usize>(bytes));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

Result<usize> readRange(const File& file, u64 offset, u8* dst, usize size) {
    usize total = 0;
    while (total < size) {
        Result<usize> got = file.readAt(offset + total, dst + total, size - total);
        if (!got) return got.error();
        if (*got == 0) break; // end of file
        total += *got;
    }
    return total;
}

} // namespace

bool AsyncRead::isReady() const noexcept { return m_state && isComplete(*m_state); }

const jobs::Counter& AsyncRead::counter() const noexcept {
    static const jobs::Counter kDone{0};
    return m_state ? m_state->counter : kDone;
}

void AsyncRead::wait() const {
    // Checked before touching the pool, which may already be gone once the request is complete.
    if (!m_state || isComplete(*m_state)) return;
    m_state->pool->wait(m_state->counter); // sleeps (never helps) until the request is complete
}

bool AsyncRead::cancel() noexcept {
    if (!m_state) return false;
    u8 expected = State::Queued;
    return m_state->phase.compare_exchange_strong(expected, State::Cancelled, std::memory_order_acq_rel) ||
           expected == State::Cancelled;
}

Result<std::vector<u8>> AsyncRead::take() {
    if (!m_state) return Error{ErrorCode::InvalidState, "AsyncRead::take on an empty handle"};
    wait();
    std::lock_guard lock(m_state->takeMutex);
    if (m_state->taken) return Error{ErrorCode::InvalidState, "AsyncRead::take: bytes already taken"};
    m_state->taken = true;
    return std::move(m_state->bytes);
}

Result<usize> AsyncRead::bytesRead() const {
    if (!m_state) return Error{ErrorCode::InvalidState, "AsyncRead::bytesRead on an empty handle"};
    wait();
    return m_state->count;
}

AsyncRead readFileAsync(jobs::BackgroundPool& pool, const Path& path, AsyncReadOptions options) {
    auto st = std::make_shared<State>();
    st->pool = &pool;
    st->onComplete = std::move(options.onComplete);
    st->counter.add(1); // released by finish(), inside the job (see there)
    AsyncRead handle(st);
    const u64 offset = options.offset;
    const u64 size = options.size;
    pool.run(
        [st, path, offset, size] {
            if (begin(*st)) {
                Result<File> file = File::open(path, OpenMode::Read);
                if (!file) {
                    st->count = file.error();
                    st->bytes = std::move(file).error();
                } else if (Result<u64> fileSize = file->size(); !fileSize) {
                    st->count = fileSize.error();
                    st->bytes = std::move(fileSize).error();
                } else {
                    const u64 start = std::min(offset, *fileSize);
                    const u64 avail = *fileSize - start;
                    const u64 want = size == kReadToEnd ? avail : std::min(size, avail);
                    std::vector<u8> data;
                    if (!reserveBytes(data, want)) {
                        const Error e{ErrorCode::OutOfMemory, "readFileAsync: cannot allocate the read buffer"};
                        st->count = e;
                        st->bytes = e;
                    } else if (Result<usize> got = readRange(*file, start, data.data(), data.size()); !got) {
                        st->count = got.error();
                        st->bytes = got.error();
                    } else {
                        data.resize(*got);
                        st->count = *got;
                        st->bytes = std::move(data);
                    }
                }
            }
            finish(st);
        },
        nullptr, options.priority);
    return handle;
}

AsyncRead readAsync(jobs::BackgroundPool& pool, const File& file, u64 offset, std::span<u8> dst,
                    jobs::Priority priority, std::function<void(AsyncRead&)> onComplete) {
    auto st = std::make_shared<State>();
    st->pool = &pool;
    st->onComplete = std::move(onComplete);
    st->counter.add(1); // released by finish(), inside the job
    AsyncRead handle(st);
    const File* filePtr = &file;
    pool.run(
        [st, filePtr, offset, dst] {
            if (begin(*st)) {
                if (!filePtr->isOpen()) {
                    st->count = Error{ErrorCode::InvalidState, "readAsync: file is not open"};
                } else {
                    st->count = readRange(*filePtr, offset, dst.data(), dst.size());
                }
                st->bytes = Error{ErrorCode::InvalidState, "readAsync reads into caller memory; use bytesRead()"};
            }
            finish(st);
        },
        nullptr, priority);
    return handle;
}

} // namespace helios::fs
