#pragma once
// Crash handling.
//
// Windows: SetUnhandledExceptionFilter + MiniDumpWriteDump -> "<dumpDir>/<app>-<utc>-<pid>.dmp".
//          abort() from any thread (failed asserts, HELIOS_LOG_FATAL, std::terminate) and MSVC CRT
//          pure-call / invalid-parameter failures also write a dump (SIGABRT + CRT hooks), since
//          they would otherwise fast-fail past the exception filter.
// POSIX:   sigaction for SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT on an alternate signal stack, writing
//          "<dumpDir>/<app>-<utc>-<pid>.txt" with the signal and a backtrace (glibc
//          <execinfo.h>; other libcs get the signal info only), then re-raising the signal so the
//          default action (core dump / exit status) still happens.
//
// The handler only uses async-signal-safe calls on POSIX; paths are prepared at install time.
// Threading: install/uninstall from the main thread during startup/shutdown. Crashes on any
// thread are handled. Replaced later by sentry-native/crashpad (ADR-013), behind this API.

#include <filesystem>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios {

struct CrashHandlerOptions {
    std::string appName = "helios"; ///< Dump file name prefix (ASCII recommended).
    bool fullMemoryDump = false;    ///< Windows: include all process memory (large).
};

/// Installs the process-wide crash handler writing reports to `dumpDir` (created if missing).
/// Calling it again replaces the directory/options.
Result<void> installCrashHandler(const std::filesystem::path& dumpDir, const CrashHandlerOptions& options = {});
/// Restores the previous handlers.
void uninstallCrashHandler() noexcept;
bool isCrashHandlerInstalled() noexcept;

/// Writes a report for the *current* state without crashing (minidump on Windows, backtrace text
/// on POSIX) — e.g. before a fatal error exit. Requires an installed handler (its directory).
Result<std::filesystem::path> writeCrashReport(std::string_view reason);

} // namespace helios
