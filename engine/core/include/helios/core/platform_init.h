#pragma once
// Process start-up checks (02 §1.1 "Proof that it ran"; WP-0.5r).
//
// Every gated executable (helios_executable() roles client, cell, gateway, voice, editor, bot and tool)
// calls core::platformInit() first in main(). Today it checks that the CPU gate's pre-initializer ran and
// passed in this process, in every build configuration: a dropped or misplaced gate hook therefore fails the
// first smoke test on an AVX2 machine ("CPU gate did not run") instead of shipping unnoticed.
//
// Threading: platformInit() runs once, on the main thread, before any other engine call. checkCpuGateVerdict()
// is pure and callable from any thread.

#include "helios/core/cpu.h"
#include "helios/core/result.h"

namespace helios::core {

/// Exit code of platformInit() when a start-up check fails (EX_SOFTWARE). kCpuGateExitCode (78) stays the
/// gate's own refusal.
inline constexpr int kPlatformInitExitCode = 70;

/// The CPU-gate check of platformInit(): Ok when `verdict` is Pass, otherwise an InvalidState error whose
/// message starts with "CPU gate did not run". Pure; any thread.
Result<void> checkCpuGateVerdict(CpuGateVerdict verdict);

/// Runs the start-up checks for this process (today: checkCpuGateVerdict(cpuGateVerdict())). On failure it
/// writes "Helios: <message>" to stderr, flushes the log and ends the process with kPlatformInitExitCode,
/// without running atexit handlers or static destructors. Main thread, once, before other engine calls.
void platformInit() noexcept;

} // namespace helios::core
