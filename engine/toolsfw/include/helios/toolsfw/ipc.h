#pragma once
// Local IPC stream transport for the remote-control socket (07 §1.2): a Windows named pipe
// `\\.\pipe\<name>` (remote clients rejected; a protected DACL grants it to the creating user and
// LocalSystem only) or a Unix-domain socket `<runtime dir>/<name>.sock` (mode 0600, runtime dir
// = $XDG_RUNTIME_DIR, else $TMPDIR, else /tmp) on Linux.
//
// Blocking calls; shutdown() wakes every thread blocked in accept() or read() of the object.
//
// Threading: a Listener's accept() runs on one thread while shutdown() may be called from any.
// A Connection may be read on one thread and written on another concurrently; writes from
// several threads must be serialized by the caller.

#include <memory>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::tf::ipc {

/// Endpoint names are 1..64 characters of [A-Za-z0-9._-].
bool isValidEndpointName(std::string_view name) noexcept;
/// The OS path of an endpoint (`\\.\pipe\name` or `/run/user/1000/name.sock`).
std::string endpointPath(std::string_view name);

class Connection {
public:
    virtual ~Connection() = default;
    /// Reads up to `size` bytes. 0 = the peer closed the stream. `timeoutMs` < 0 waits forever;
    /// on timeout returns ErrorCode::Timeout.
    virtual Result<usize> read(void* buffer, usize size, i32 timeoutMs = -1) = 0;
    /// Writes every byte (fails once the peer is gone).
    virtual Result<void> write(const void* data, usize size) = 0;
    /// Wakes blocked reads (they return 0) and disconnects.
    virtual void shutdown() noexcept = 0;
};

class Listener {
public:
    /// Creates the endpoint. Fails with AlreadyExists when another live process owns the name.
    static Result<std::unique_ptr<Listener>> listen(std::string_view name);
    virtual ~Listener() = default;
    /// Waits for the next client. Fails with Cancelled after shutdown().
    virtual Result<std::unique_ptr<Connection>> accept() = 0;
    virtual void shutdown() noexcept = 0;
    virtual const std::string& path() const noexcept = 0;
};

/// Connects to an endpoint, retrying until `timeoutMs` passes (the server may still be starting).
Result<std::unique_ptr<Connection>> connect(std::string_view name, i32 timeoutMs = 0);

} // namespace helios::tf::ipc
