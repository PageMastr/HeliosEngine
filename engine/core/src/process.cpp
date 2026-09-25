// Portable part of process.h: pipe ends, the Process object, communicate(), runProcess() and
// Windows command-line quoting. OS calls go through platform/os.h (posix_process.cpp,
// win32_process.cpp).
#include "helios/core/process.h"

#include <algorithm>
#include <thread>

#include "platform/os.h"

namespace helios {

// ---------------------------------------------------------------------------------------------
// PipeEnd / Pipe
// ---------------------------------------------------------------------------------------------

PipeEnd::~PipeEnd() { close(); }

PipeEnd::PipeEnd(PipeEnd&& other) noexcept : m_handle(other.release()) {}

PipeEnd& PipeEnd::operator=(PipeEnd&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = other.release();
    }
    return *this;
}

PipeEnd PipeEnd::adopt(NativeHandle handle) noexcept {
    PipeEnd end;
    end.m_handle = handle;
    return end;
}

NativeHandle PipeEnd::release() noexcept {
    const NativeHandle h = m_handle;
    m_handle = kInvalidNativeHandle;
    return h;
}

void PipeEnd::close() noexcept {
    if (m_handle != kInvalidNativeHandle) {
        os::handleClose(m_handle);
        m_handle = kInvalidNativeHandle;
    }
}

Result<usize> PipeEnd::read(void* buffer, usize size) {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "PipeEnd::read on a closed pipe"};
    if (size == 0) return usize{0};
    return os::pipeRead(m_handle, buffer, size);
}

Result<std::string> PipeEnd::readAll() {
    std::string out;
    char chunk[16 * 1024];
    for (;;) {
        Result<usize> n = read(chunk, sizeof(chunk));
        if (!n) return n.error();
        if (*n == 0) return out;
        out.append(chunk, *n);
    }
}

Result<void> PipeEnd::write(const void* data, usize size) {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "PipeEnd::write on a closed pipe"};
    const auto* bytes = static_cast<const u8*>(data);
    while (size > 0) {
        Result<usize> n = os::pipeWrite(m_handle, bytes, size);
        if (!n) return n.error();
        if (*n == 0) return Error{ErrorCode::IoError, "PipeEnd::write: no progress"};
        bytes += *n;
        size -= *n;
    }
    return {};
}

Result<Pipe> Pipe::create() {
    NativeHandle r = kInvalidNativeHandle, w = kInvalidNativeHandle;
    if (auto res = os::pipeCreate(r, w); !res) return res.error();
    Pipe pipe;
    pipe.read = PipeEnd::adopt(r);
    pipe.write = PipeEnd::adopt(w);
    return pipe;
}

// ---------------------------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------------------------

Process::~Process() {
    if (m_process != kInvalidNativeHandle) os::processClose(m_process);
}

Process::Process(Process&& other) noexcept
    : m_process(other.m_process), m_pid(other.m_pid), m_exitCode(other.m_exitCode), m_stdin(std::move(other.m_stdin)),
      m_stdout(std::move(other.m_stdout)), m_stderr(std::move(other.m_stderr)) {
    other.m_process = kInvalidNativeHandle;
    other.m_pid = 0;
    other.m_exitCode.reset();
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        if (m_process != kInvalidNativeHandle) os::processClose(m_process);
        m_process = other.m_process;
        m_pid = other.m_pid;
        m_exitCode = other.m_exitCode;
        m_stdin = std::move(other.m_stdin);
        m_stdout = std::move(other.m_stdout);
        m_stderr = std::move(other.m_stderr);
        other.m_process = kInvalidNativeHandle;
        other.m_pid = 0;
        other.m_exitCode.reset();
    }
    return *this;
}

namespace {

// Strings that would be silently truncated (an embedded NUL ends a C string, an argv entry and a
// Windows command line) or reinterpreted (a '=' in a variable name makes "NAME=x=value" set a
// different variable) are rejected instead, so a caller passing untrusted text cannot smuggle a
// second variable or drop the rest of an argument.
Result<void> validateDesc(const ProcessDesc& desc) {
    auto hasNul = [](std::string_view s) { return s.find('\0') != std::string_view::npos; };
    auto badName = [&](std::string_view name) {
        return name.empty() || name.find('=') != std::string_view::npos || hasNul(name);
    };
    for (const std::string& a : desc.args) {
        if (hasNul(a)) return Error{ErrorCode::InvalidArgument, "Process::spawn: argument contains a NUL byte"};
    }
    for (const auto& [name, value] : desc.environment) {
        if (badName(name)) {
            return Error{ErrorCode::InvalidArgument, "Process::spawn: invalid environment variable name (empty, '=' or NUL)"};
        }
        if (hasNul(value)) {
            return Error{ErrorCode::InvalidArgument, "Process::spawn: environment value contains a NUL byte"};
        }
    }
    for (const std::string& name : desc.unsetEnvironment) {
        if (badName(name)) {
            return Error{ErrorCode::InvalidArgument, "Process::spawn: invalid environment variable name (empty, '=' or NUL)"};
        }
    }
    return {};
}

} // namespace

Result<Process> Process::spawn(const ProcessDesc& desc) {
    if (desc.executable.empty()) return Error{ErrorCode::InvalidArgument, "Process::spawn: no executable"};
    if (Result<void> valid = validateDesc(desc); !valid) return valid.error();
    Result<os::SpawnedProcess> spawned = os::processSpawn(desc);
    if (!spawned) return spawned.error();
    Process p;
    p.m_process = spawned->process;
    p.m_pid = spawned->pid;
    p.m_stdin = PipeEnd::adopt(spawned->stdinWrite);
    p.m_stdout = PipeEnd::adopt(spawned->stdoutRead);
    p.m_stderr = PipeEnd::adopt(spawned->stderrRead);
    return p;
}

Result<std::optional<i32>> Process::wait(std::optional<std::chrono::milliseconds> timeout) {
    if (!isValid()) return Error{ErrorCode::InvalidState, "Process::wait on an empty Process"};
    if (m_exitCode) return m_exitCode;
    const i64 ms = timeout ? std::max<i64>(0, static_cast<i64>(timeout->count())) : -1;
    Result<std::optional<i32>> r = os::processWait(m_process, m_pid, ms);
    if (r && r->has_value()) m_exitCode = **r;
    return r;
}

std::optional<i32> Process::tryWait() {
    Result<std::optional<i32>> r = wait(std::chrono::milliseconds(0));
    if (!r) return std::nullopt;
    return *r;
}

Result<void> Process::kill() {
    if (!isValid()) return Error{ErrorCode::InvalidState, "Process::kill on an empty Process"};
    if (m_exitCode) return {};
    return os::processKill(m_process, m_pid);
}

Result<ProcessOutput> Process::communicate(std::string_view input) {
    if (!isValid()) return Error{ErrorCode::InvalidState, "Process::communicate on an empty Process"};
    ProcessOutput output;
    Result<void> writeResult;
    Result<std::string> errResult{std::string()};
    std::thread writer;
    std::thread errReader;
    if (m_stdin.isOpen()) {
        writer = std::thread([&] {
            if (!input.empty()) writeResult = m_stdin.write(input.data(), input.size());
            m_stdin.close();
        });
    }
    if (m_stderr.isOpen()) {
        errReader = std::thread([&] { errResult = m_stderr.readAll(); });
    }
    Result<std::string> outResult{std::string()};
    if (m_stdout.isOpen()) outResult = m_stdout.readAll();
    if (writer.joinable()) writer.join();
    if (errReader.joinable()) errReader.join();
    m_stdout.close();
    m_stderr.close();

    Result<std::optional<i32>> code = wait();
    if (!code) return code.error();
    if (!outResult) return outResult.error();
    if (!errResult) return errResult.error();
    // A child that exits without reading all of its input breaks the pipe; that is its business,
    // not a failure of communicate().
    (void)writeResult;
    output.exitCode = code->value_or(-1);
    output.out = std::move(*outResult);
    output.err = std::move(*errResult);
    return output;
}

Result<ProcessOutput> runProcess(ProcessDesc desc, std::string_view input) {
    if (desc.stdinMode == StdioMode::Inherit) desc.stdinMode = StdioMode::Pipe;
    if (desc.stdoutMode == StdioMode::Inherit) desc.stdoutMode = StdioMode::Pipe;
    if (desc.stderrMode == StdioMode::Inherit) desc.stderrMode = StdioMode::Pipe;
    Result<Process> p = Process::spawn(desc);
    if (!p) return p.error();
    return p->communicate(input);
}

// ---------------------------------------------------------------------------------------------
// Windows command lines
// ---------------------------------------------------------------------------------------------

std::string quoteWindowsArgument(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(arg);
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (usize i = 0;; ++i) {
        usize backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            // Double trailing backslashes so the closing quote stays a delimiter.
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

std::string buildWindowsCommandLine(std::string_view program, const std::vector<std::string>& args) {
    std::string line;
    // The program token takes no backslash escapes (a path cannot contain '"'): quote it whole.
    if (program.empty() || program.find_first_of(" \t") != std::string_view::npos) {
        line.push_back('"');
        line.append(program);
        line.push_back('"');
    } else {
        line.append(program);
    }
    for (const std::string& a : args) {
        line.push_back(' ');
        line += quoteWindowsArgument(a);
    }
    return line;
}

bool activeCodePageIsUtf8() noexcept { return os::activeCodePageIsUtf8(); }

NativeHandle standardInputHandle() noexcept { return os::standardHandle(0); }
NativeHandle standardOutputHandle() noexcept { return os::standardHandle(1); }
NativeHandle standardErrorHandle() noexcept { return os::standardHandle(2); }

} // namespace helios
