#pragma once
// Internal OS helpers for engine/net that are not part of UdpSocket. Implemented once per platform
// in src/platform/{posix,win32}/. Not a public header.

#include "helios/core/types.h"

namespace helios::net::os {

/// CPU time consumed by the calling thread, in seconds (GetThreadTimes /
/// CLOCK_THREAD_CPUTIME_ID). Used by the throughput gates to report cores used.
f64 threadCpuSeconds() noexcept;

} // namespace helios::net::os
