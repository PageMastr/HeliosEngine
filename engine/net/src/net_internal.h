#pragma once
// Internal helpers shared by engine/net sources: log channel, allocator hooks for netcode and
// reliable, library reference counting and small sequence-number helpers. Not a public header.

#include <cstddef>

#include "helios/core/log.h"
#include "helios/core/memory.h"
#include "helios/core/types.h"

namespace helios::net {

HELIOS_LOG_CHANNEL(LogNet, "Net");

namespace detail {

/// Memory tag "Net" (registered on first use).
MemoryTag netMemoryTag() noexcept;
/// netcode/reliable allocator hooks: tracked alignedAlloc under the "Net" tag.
void* netAllocate(void* context, std::size_t bytes);
void netFree(void* context, void* pointer);

/// Reference-counted netcode_init/reliable_init plus log and assert hooks. acquire() fails only
/// if netcode_init fails (Winsock start-up or libsodium self-test). Thread-safe.
bool acquireLibrary() noexcept;
void releaseLibrary() noexcept;

/// RAII holder used by Server and Client.
class LibraryRef {
public:
    LibraryRef() noexcept : m_ok(acquireLibrary()) {}
    ~LibraryRef() {
        if (m_ok) releaseLibrary();
    }
    LibraryRef(const LibraryRef&) = delete;
    LibraryRef& operator=(const LibraryRef&) = delete;
    bool ok() const noexcept { return m_ok; }

private:
    bool m_ok;
};

/// Test hooks for UdpSocket's choice of batch API (Windows Registered I/O; no effect elsewhere). While
/// forceRegisteredIoUnavailable(true) is in force, open() treats RIO as absent, as on a provider without it;
/// while forceRegisteredIoSetUpFailure(true) is, setting up a RIO socket fails after its registered region
/// and completion queues exist, as when RIORegisterBuffer or the queues are refused, so open() releases them
/// and re-opens a plain socket on the same address. Thread-safe (atomics); tests set a hook around open()
/// and clear it after.
void forceRegisteredIoUnavailable(bool force) noexcept;
void forceRegisteredIoSetUpFailure(bool force) noexcept;
bool registeredIoUnavailableForced() noexcept;
bool registeredIoSetUpFailureForced() noexcept;
/// True for the first fallback from Registered I/O in the process, which is logged as a warning (a server on
/// the slower path matters for NS-0.2 and NS-0.7); later ones are logged at debug level. Thread-safe.
/// resetRegisteredIoFallbackWarning() is a test hook that makes the next fallback the first again.
bool firstRegisteredIoFallback() noexcept;
void resetRegisteredIoFallbackWarning() noexcept;

/// Signed distance a - b on the 16-bit sequence circle.
constexpr i32 seqDiff(u16 a, u16 b) noexcept { return static_cast<i16>(static_cast<u16>(a - b)); }
constexpr bool seqGreater(u16 a, u16 b) noexcept { return seqDiff(a, b) > 0; }

} // namespace detail
} // namespace helios::net
