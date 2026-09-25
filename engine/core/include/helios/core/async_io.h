#pragma once
// Asynchronous file reads on a jobs::BackgroundPool (02 §2.1 "async reads", WP-0.5).
//
// readFileAsync() / readAsync() queue a blocking positional read (pread / ReadFile with an
// OVERLAPPED offset, via fs::File::readAt) on an IO BackgroundPool and return an AsyncRead handle
// immediately. Completion is observable three ways:
//   * poll AsyncRead::isReady() (lock-free), e.g. once per frame from the streaming system;
//   * wait on AsyncRead::counter() with JobSystem::wait(), which runs other jobs meanwhile;
//   * an optional onComplete callback, which runs on the IO thread right after the result is
//     stored (keep it short: take() the bytes and hand them to a job, do not decode on the IO
//     thread). take() and bytesRead() do not block inside it.
// Fire-and-forget is allowed: the caller may drop every AsyncRead at once; the request keeps its
// own state alive until its job (and onComplete) has finished.
// Requests not yet started can be cancelled (their result is ErrorCode::Cancelled).
//
// Backends. Phase 0 uses one blocking read per request on the pool's threads (2 on MIN, 3 on REF,
// 02 §2.3). The request shape (file, offset, size, destination, completion counter) is what the
// Phase 3 backends need, so they slot in behind the same API without touching callers:
//   * Windows 11 22H2+: IORing (CreateIoRing, BuildIoRingReadFile, SubmitIoRing) with registered
//     file handles and buffers, one ring per IO thread; DirectStorage stays out of scope for CPU
//     reads. Windows 10 falls back to this pool (or overlapped IO with an IOCP).
//   * Linux 5.6+: io_uring (IORING_OP_READ with fixed files/buffers, IOSQE_ASYNC for buffered
//     reads), falling back to this pool when the kernel or seccomp policy (containers) refuses it.
//   * Both batch submissions per streaming tick and complete into the same Counter, so
//     JobSystem::wait keeps helping instead of blocking.
//
// Threading: all functions are thread-safe. An AsyncRead may be copied and polled from any
// thread; take() hands the bytes to exactly one caller. Memory passed to readAsync() and the File
// it reads from must stay alive until the read is ready.

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/jobs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::fs {

/// Read from `offset` to the end of the file.
inline constexpr u64 kReadToEnd = ~u64(0);

namespace detail {
struct AsyncReadState;
} // namespace detail

class AsyncRead;

struct AsyncReadOptions {
    u64 offset = 0;
    u64 size = kReadToEnd; ///< Bytes to read; a range past the end of the file is truncated.
    jobs::Priority priority = jobs::Priority::Normal;
    /// Runs on the IO thread once the result is stored (isReady() is already true).
    std::function<void(AsyncRead&)> onComplete;
};

/// Handle to one asynchronous read. Copies share the same request.
class AsyncRead {
public:
    AsyncRead() noexcept = default;

    bool isValid() const noexcept { return m_state != nullptr; }
    /// True once the read has finished, failed or been cancelled.
    bool isReady() const noexcept;
    /// Reaches zero when the request (including onComplete) has finished. JobSystem::wait() on it
    /// helps with other jobs; BackgroundPool::wait() sleeps.
    const jobs::Counter& counter() const noexcept;
    /// Blocks (sleeping) until ready. Returns at once from this request's onComplete. Must not be
    /// called from another task of the pool doing the read (it could wait for itself).
    void wait() const;
    /// Cancels the request if it has not started yet. Returns true if it will not run.
    bool cancel() noexcept;

    /// Waits, then returns the bytes (readFileAsync) or the error. The bytes are moved out: later
    /// calls return InvalidState.
    Result<std::vector<u8>> take();
    /// Waits, then returns how many bytes were read (both kinds of request) or the error.
    Result<usize> bytesRead() const;

    /// Internal: wraps a request's shared state (created by readFileAsync / readAsync).
    explicit AsyncRead(std::shared_ptr<detail::AsyncReadState> state) noexcept : m_state(std::move(state)) {}

private:
    std::shared_ptr<detail::AsyncReadState> m_state;
};

/// Opens `path` on an IO thread and reads options.size bytes from options.offset into a new buffer.
AsyncRead readFileAsync(jobs::BackgroundPool& pool, const Path& path, AsyncReadOptions options = {});

/// Reads up to dst.size() bytes at `offset` of an open file into caller memory. `file` and `dst`
/// must outlive the request. Several requests may target the same File concurrently.
AsyncRead readAsync(jobs::BackgroundPool& pool, const File& file, u64 offset, std::span<u8> dst,
                    jobs::Priority priority = jobs::Priority::Normal,
                    std::function<void(AsyncRead&)> onComplete = {});

} // namespace helios::fs
