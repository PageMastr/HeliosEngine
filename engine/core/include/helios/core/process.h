#pragma once
// Child processes and anonymous pipes (02 §2.1 "process spawn", WP-0.5).
//
// Process::spawn starts a program with CreateProcessW (Windows) or posix_spawn (POSIX) with:
// * UTF-8 arguments, quoted for Windows by quoteWindowsArgument() so the child's CRT (or
//   CommandLineToArgvW) splits them back exactly;
// * an optional working directory and environment overrides (inherit + set/unset, or a clean
//   environment);
// * per-stream stdin/stdout/stderr handling: inherit, the null device, or a pipe to the parent;
// * an inherit-handle whitelist: besides its standard streams, the child inherits exactly the
//   handles listed in ProcessDesc::inheritHandles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST on Windows;
//   every Helios descriptor is close-on-exec on POSIX, and listed ones are re-enabled in the child
//   only). This is how the launcher hands the client its launch-code pipe (08 §2, WP-1.21): create
//   a Pipe, list the read end, pass its value on the command line, and the child adopts it with
//   PipeEnd::adopt().
// wait() with a timeout, kill() and exit codes work the same on both platforms: a process killed
// by a signal (POSIX) or by kill() reports 128 + signal number (kill() -> 137 everywhere).
//
// Threading: Process and PipeEnd objects belong to one thread at a time; different pipe ends may
// be used from different threads concurrently (communicate() does this internally). spawn() and
// the free functions are thread-safe. Handles created by Helios are never inheritable outside a
// spawn() call, so concurrent spawns cannot leak each other's pipes.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios {

/// OS handle as an integer: a HANDLE on Windows, a file descriptor on POSIX.
using NativeHandle = std::intptr_t;
inline constexpr NativeHandle kInvalidNativeHandle = -1;

/// Exit code reported for a process terminated by Process::kill() on every platform.
inline constexpr i32 kKilledExitCode = 137;

/// One end of an anonymous pipe (or an adopted inherited handle). Move-only; closes on destruction.
/// Reads and writes block. Not inheritable unless listed in ProcessDesc::inheritHandles.
class PipeEnd {
public:
    PipeEnd() noexcept = default;
    ~PipeEnd();
    PipeEnd(PipeEnd&& other) noexcept;
    PipeEnd& operator=(PipeEnd&& other) noexcept;
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;

    /// Takes ownership of an existing handle, e.g. a pipe the parent passed on the command line.
    static PipeEnd adopt(NativeHandle handle) noexcept;

    bool isOpen() const noexcept { return m_handle != kInvalidNativeHandle; }
    NativeHandle native() const noexcept { return m_handle; }
    /// Gives up ownership without closing.
    NativeHandle release() noexcept;
    void close() noexcept;

    /// Reads up to `size` bytes; 0 means the other end was closed (end of stream).
    Result<usize> read(void* buffer, usize size);
    /// Reads until end of stream.
    Result<std::string> readAll();
    /// Writes every byte (loops over partial writes). Fails with a broken-pipe error if the reader
    /// is gone (never raises SIGPIPE).
    Result<void> write(const void* data, usize size);
    Result<void> write(std::string_view text) { return write(text.data(), text.size()); }

private:
    NativeHandle m_handle = kInvalidNativeHandle;
};

/// A connected pair: bytes written to `write` come out of `read`.
struct Pipe {
    PipeEnd read;
    PipeEnd write;
    static Result<Pipe> create();
};

enum class StdioMode : u8 {
    Inherit, ///< Share the parent's stream.
    Null,    ///< /dev/null or NUL.
    Pipe,    ///< Connect to a pipe; the parent end is Process::stdinPipe() etc.
};

struct ProcessDesc {
    /// Program to run. Without searchPath it must be a path (absolute, or relative to the parent's
    /// working directory, also when workingDirectory is set); with searchPath a bare name is looked
    /// up in PATH. Windows batch files (.bat, .cmd) are refused (InvalidArgument): CreateProcessW
    /// runs them through cmd.exe, whose parsing no argument quoting can make safe.
    fs::Path executable;
    bool searchPath = false;
    /// Arguments after argv[0] (UTF-8, no NUL bytes). argv[0] is the executable as given.
    std::vector<std::string> args;
    /// Working directory of the child; empty = the parent's.
    fs::Path workingDirectory;
    /// Start from the parent's environment (true) or from an empty one.
    bool inheritEnvironment = true;
    /// Variables to set or override (UTF-8). Names compare case-insensitively on Windows. A name
    /// must be non-empty and contain no '=' or NUL, a value no NUL (else InvalidArgument).
    std::vector<std::pair<std::string, std::string>> environment;
    /// Variables to remove from the inherited environment.
    std::vector<std::string> unsetEnvironment;
    StdioMode stdinMode = StdioMode::Inherit;
    StdioMode stdoutMode = StdioMode::Inherit;
    StdioMode stderrMode = StdioMode::Inherit;
    /// Child's stderr goes wherever its stdout goes (stderrMode is ignored).
    bool mergeStderrIntoStdout = false;
    /// Extra handles the child inherits (the whitelist; nothing else is inherited). Each keeps its
    /// value in the child, so pass it on the command line. The parent keeps ownership. On POSIX the
    /// child also closes every other descriptor above 2, including ones a third-party library
    /// opened without close-on-exec (glibc >= 2.34 and macOS; elsewhere only close-on-exec
    /// descriptors are kept out).
    std::vector<NativeHandle> inheritHandles;
};

/// Captured result of Process::communicate() / runProcess().
struct ProcessOutput {
    i32 exitCode = -1;
    std::string out;
    std::string err;
};

/// A spawned child process. Move-only. Destroying a Process closes the parent's pipe ends and
/// handles but does not kill the child (call kill() first if that is wanted); on POSIX a child
/// that is never waited for stays a zombie until the parent exits.
class Process {
public:
    Process() noexcept = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    /// Starts the child. Fails (NotFound, AccessDenied, InvalidArgument, ...) if it cannot be
    /// started; a started child that fails later reports it through its exit code.
    static Result<Process> spawn(const ProcessDesc& desc);

    bool isValid() const noexcept { return m_pid != 0; }
    u32 pid() const noexcept { return m_pid; }

    /// Waits until the child exits or `timeout` passes. Returns the exit code, or an empty optional
    /// on timeout. Without a timeout it waits forever. Idempotent after the child exited.
    Result<std::optional<i32>> wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    /// Non-blocking wait().
    std::optional<i32> tryWait();
    bool isRunning() { return !tryWait().has_value(); }
    /// Forcefully terminates the child (SIGKILL / TerminateProcess); its exit code becomes
    /// kKilledExitCode. A child that already exited is not an error and keeps its own exit code.
    Result<void> kill();

    /// Parent ends of the pipes requested with StdioMode::Pipe (closed ends otherwise).
    PipeEnd& stdinPipe() noexcept { return m_stdin; }
    PipeEnd& stdoutPipe() noexcept { return m_stdout; }
    PipeEnd& stderrPipe() noexcept { return m_stderr; }

    /// Writes `input` to the stdin pipe (if any) and closes it, reads stdout and stderr (if piped)
    /// to the end on separate threads so neither can fill up and deadlock, then waits for exit.
    Result<ProcessOutput> communicate(std::string_view input = {});

private:
    NativeHandle m_process = kInvalidNativeHandle; // process HANDLE (Windows); unused on POSIX
    u32 m_pid = 0;
    std::optional<i32> m_exitCode;
    PipeEnd m_stdin;
    PipeEnd m_stdout;
    PipeEnd m_stderr;
};

/// spawn() + communicate(): runs a program to completion and captures its piped output. Streams
/// the desc leaves at Inherit are switched to Pipe (stdin gets `input`).
Result<ProcessOutput> runProcess(ProcessDesc desc, std::string_view input = {});

/// Quotes one argument for a Windows command line so that the MSVC CRT and CommandLineToArgvW
/// parse it back unchanged (backslashes are doubled only in front of quotes). Exposed for tests
/// and for tools that write command lines to files.
std::string quoteWindowsArgument(std::string_view arg);
/// Builds a full Windows command line: the program (quoted, no escapes) followed by the arguments.
std::string buildWindowsCommandLine(std::string_view program, const std::vector<std::string>& args);

/// The calling process's standard streams as native handles, for binary I/O through PipeEnd with no
/// C runtime text-mode translation. Not owned: wrap with PipeEnd::adopt() and release() it again
/// unless the stream should be closed. kInvalidNativeHandle if the process has no such stream.
NativeHandle standardInputHandle() noexcept;
NativeHandle standardOutputHandle() noexcept;
NativeHandle standardErrorHandle() noexcept;

/// True if the process's ANSI code page is UTF-8 (the Helios manifest sets it on Windows 10
/// 1903+). Always true on POSIX, where Helios treats paths and text as UTF-8 bytes.
bool activeCodePageIsUtf8() noexcept;

} // namespace helios
