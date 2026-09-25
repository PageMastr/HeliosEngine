#pragma once
// Internal OS abstraction for engine/core. Implemented once per platform in
// src/platform/win32/*.cpp and src/platform/posix/*.cpp; the portable code in src/*.cpp only
// talks to the OS through these functions. Not a public header.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/crash.h"
#include "helios/core/fs.h"
#include "helios/core/process.h"
#include "helios/core/result.h"
#include "helios/core/thread.h"
#include "helios/core/types.h"

namespace helios::os {

// ---- time -----------------------------------------------------------------------------------
u64 monotonicTicks() noexcept;
u64 monotonicFrequency() noexcept;
u64 monotonicNanos() noexcept;
/// Coarse OS sleep.
void sleepNanos(u64 nanos) noexcept;
/// Best available high-resolution sleep (may still overshoot slightly; caller spins the rest).
void highResolutionSleepNanos(u64 nanos) noexcept;
/// Typical overshoot of highResolutionSleepNanos; sleepPrecise spins for the last stretch.
u64 sleepSpinMarginNanos() noexcept;

// ---- threads / process ------------------------------------------------------------------------
u64 currentThreadId() noexcept;
void setCurrentThreadName(const std::string& name) noexcept;
bool setCurrentThreadAffinity(u64 mask) noexcept;
bool setCurrentThreadPriority(ThreadPriority priority) noexcept;
u32 processorCount() noexcept;
u32 currentProcessId() noexcept;
/// UTF-8 arguments of the current process including argv[0].
std::vector<std::string> processArguments();

// ---- memory ---------------------------------------------------------------------------------
usize pageSize() noexcept;
usize allocationGranularity() noexcept;
void* vmReserve(usize size) noexcept;
bool vmCommit(void* ptr, usize size) noexcept;
bool vmDecommit(void* ptr, usize size) noexcept;
bool vmRelease(void* ptr, usize size) noexcept;

// ---- console / debugger ---------------------------------------------------------------------
enum class ConsoleStream : u8 { Out, Err };
/// Enables ANSI escape processing if the stream is an interactive console; returns whether
/// colored output should be used.
bool consoleEnableColors(ConsoleStream stream) noexcept;
/// Writes UTF-8 text (unbuffered, one OS call where possible).
void consoleWrite(ConsoleStream stream, std::string_view utf8) noexcept;
bool isDebuggerPresent() noexcept;
void debugOutput(std::string_view utf8) noexcept;
/// Environment variable (UTF-8), empty optional if unset.
bool getEnv(const char* name, std::string& out);

// ---- files ----------------------------------------------------------------------------------
using NativeFile = std::intptr_t;
inline constexpr NativeFile kInvalidFile = -1;

Result<NativeFile> fileOpen(const std::filesystem::path& path, fs::OpenMode mode);
void fileClose(NativeFile file) noexcept;
Result<usize> fileRead(NativeFile file, void* buffer, usize size);
Result<usize> fileReadAt(NativeFile file, u64 offset, void* buffer, usize size);
Result<usize> fileWrite(NativeFile file, const void* data, usize size);
Result<u64> fileSeek(NativeFile file, i64 offset, fs::SeekOrigin origin);
Result<u64> fileSize(NativeFile file);
Result<void> fileSync(NativeFile file);
/// Atomically replaces `target` with `source` (same volume). Target may or may not exist.
Result<void> atomicReplace(const std::filesystem::path& source, const std::filesystem::path& target);

struct Mapping {
    const void* data = nullptr;
    usize size = 0;
    std::intptr_t file = -1;
    std::intptr_t mapping = -1;
};
Result<Mapping> mapFileReadOnly(const std::filesystem::path& path);
void unmapFile(Mapping& mapping) noexcept;

Result<std::filesystem::path> executablePath();

/// Native directory watcher, or an error if unavailable.
Result<std::unique_ptr<fs::detail::WatcherBackend>> createNativeWatcher(const std::filesystem::path& dir,
                                                                        bool recursive);

// ---- dynamic libraries ----------------------------------------------------------------------
void* libraryOpen(const std::filesystem::path& path, std::string& error) noexcept;
void* librarySymbol(void* handle, const char* name) noexcept;
void libraryClose(void* handle) noexcept;
std::string_view libraryExtension() noexcept;
std::string_view libraryPrefix() noexcept;

// ---- randomness -----------------------------------------------------------------------------
bool secureRandomBytes(void* buffer, usize size) noexcept;

// ---- crash handling -------------------------------------------------------------------------
Result<void> installCrashHandler(const std::filesystem::path& dumpDir, const CrashHandlerOptions& options);
void uninstallCrashHandler() noexcept;
Result<std::filesystem::path> writeCrashReport(std::string_view reason);

// ---- processes and pipes --------------------------------------------------------------------
/// Anonymous pipe; both ends non-inheritable (close-on-exec).
Result<void> pipeCreate(NativeHandle& readEnd, NativeHandle& writeEnd);
void handleClose(NativeHandle handle) noexcept;
/// Blocking read; 0 at end of stream (writer closed).
Result<usize> pipeRead(NativeHandle handle, void* buffer, usize size);
/// Blocking write of at most `size` bytes; returns bytes written. Never raises SIGPIPE.
Result<usize> pipeWrite(NativeHandle handle, const void* data, usize size);

struct SpawnedProcess {
    NativeHandle process = kInvalidNativeHandle; // Windows process HANDLE; unused on POSIX
    u32 pid = 0;
    NativeHandle stdinWrite = kInvalidNativeHandle;
    NativeHandle stdoutRead = kInvalidNativeHandle;
    NativeHandle stderrRead = kInvalidNativeHandle;
};
Result<SpawnedProcess> processSpawn(const ProcessDesc& desc);
/// timeoutMs < 0 waits forever. Returns the exit code, or an empty optional on timeout.
Result<std::optional<i32>> processWait(NativeHandle process, u32 pid, i64 timeoutMs);
Result<void> processKill(NativeHandle process, u32 pid);
void processClose(NativeHandle process) noexcept;
bool activeCodePageIsUtf8() noexcept;
/// 0 = stdin, 1 = stdout, 2 = stderr.
NativeHandle standardHandle(int which) noexcept;

// ---- errors ---------------------------------------------------------------------------------
/// Error from the calling thread's last OS error (errno / GetLastError), with `context` prefixed.
Error lastError(std::string_view context);

} // namespace helios::os
