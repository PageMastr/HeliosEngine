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

    jobs::Counter counter;
    jobs::BackgroundPool* pool = nullptr;
    std::atomic<u8> phase{Queued};
    std::atomic<bool> ready{false};
    std::function<void(AsyncRead&)> onComplete;
    // Written by the IO thread before `ready` is released; read after it is acquired.
    Result<std::vector<u8>> bytes;
    Result<usize> count{usize{0}};
    std::mutex takeMutex;
    bool taken = false;
};

} // namespace detail

namespace {

using State = detail::AsyncReadState;

// Returns false if the request was cancelled before it started (the result is then stored).
bool begin(State& st) {
    u8 expected = State::Queued;
    if (st.phase.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) return true;
    st.bytes = Error{ErrorCode::Cancelled, "async read cancelled"};
    st.count = Error{ErrorCode::Cancelled, "async read cancelled"};
    return false;
}

// Publishes the result, runs onComplete and only then releases the counter. Runs inside the IO job,
// whose lambda owns a reference to `st`: the counter is released while the state is certainly
// alive. (Handing &st->counter to BackgroundPool::run would let the pool decrement it after the
// job, and with it possibly the last reference to the state, had been destroyed: a use-after-free
// for fire-and-forget requests whose caller dropped every AsyncRead.)
void finish(const std::shared_ptr<State>& st) {
    st->ready.store(true, std::memory_order_release);
    // Moved out so a callback that captures an AsyncRead of its own request cannot keep the state
    // alive through a reference cycle.
    std::function<void(AsyncRead&)> onComplete = std::move(st->onComplete);
    st->onComplete = nullptr;
    if (onComplete) {
        AsyncRead handle(st);
        onComplete(handle);
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

bool AsyncRead::isReady() const noexcept { return m_state && m_state->ready.load(std::memory_order_acquire); }

const jobs::Counter& AsyncRead::counter() const noexcept {
    static const jobs::Counter kDone{0};
    return m_state ? m_state->counter : kDone;
}

void AsyncRead::wait() const {
    // `ready` is set before onComplete runs, so this returns inside onComplete too (take() and
    // bytesRead() from the callback do not wait for the callback itself to finish).
    if (!m_state || m_state->ready.load(std::memory_order_acquire)) return;
    m_state->pool->wait(m_state->counter); // sleeps (never helps) until the request's job is done
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
