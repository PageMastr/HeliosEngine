#pragma once
// Internal OS helpers for engine/net that are not part of UdpSocket. Implemented once per platform
// in src/platform/{posix,win32}/. Not a public header.

#include "helios/core/types.h"

namespace helios::net::os {

/// CPU time consumed by the calling thread, in seconds (GetThreadTimes /
/// CLOCK_THREAD_CPUTIME_ID). Used by the throughput gates to report cores used. Thread-safe.
f64 threadCpuSeconds() noexcept;

/// The whole machine's busy CPU time, summed over every logical CPU since boot.
struct MachineCpuTimes {
    f64 busySeconds = 0.0; ///< CPU time not spent idle, all CPUs together
    u32 cpus = 0;          ///< logical CPUs the sum covers
    bool ok = false;       ///< false where the OS offers no such counter (POSIX other than Linux)
};

/// Samples the machine's busy CPU time: GetSystemTimes on Windows (kernel + user - idle, which includes
/// interrupt and DPC time), /proc/stat on Linux (user + nice + system + irq + softirq; tick-sampled, so
/// 10 ms per CPU at 100 Hz). The difference of two samples is the CPU that every thread and the OS itself
/// (interrupts, deferred procedure calls, system threads completing I/O on other CPUs) spent between them,
/// which a per-thread figure cannot see. Thread-safe.
MachineCpuTimes machineCpuTimes() noexcept;

} // namespace helios::net::os
