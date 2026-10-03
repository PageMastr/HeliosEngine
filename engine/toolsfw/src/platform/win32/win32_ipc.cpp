// Win32 half of ToolsFramework's OS services: process ids, host name and the named-pipe transport
// of the remote-control endpoint (ipc.h). Overlapped I/O throughout, so shutdown() can wake a
// thread blocked in accept() or read() with an event.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/utf.h"
#include "helios/toolsfw/ipc.h"

#include "../tf_os.h"

namespace helios::tf::os {

u32 currentProcessId() noexcept {
    return static_cast<u32>(::GetCurrentProcessId());
}

std::string hostName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!::GetComputerNameW(buf, &n)) return {};
    return wideToUtf8(std::wstring_view(buf, n));
}

bool processIsRunning(u32 pid) noexcept {
    if (pid == 0) return false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

std::string environment(const char* name) {
    const std::wstring wname = utf8ToWide(name);
    const DWORD needed = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD len = ::GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    value.resize(len < needed ? len : 0);
    return wideToUtf8(value);
}

std::string runtimeDirectory() {
    return {};
}

} // namespace helios::tf::os

namespace helios::tf::ipc {

namespace {

Error winError(std::string_view what, DWORD code = ::GetLastError()) {
    ErrorCode ec = ErrorCode::IoError;
    if (code == ERROR_FILE_NOT_FOUND) ec = ErrorCode::NotFound;
    if (code == ERROR_ACCESS_DENIED) ec = ErrorCode::PermissionDenied;
    if (code == ERROR_PIPE_BUSY) ec = ErrorCode::Busy;
    return Error{ec, std::format("{}: Win32 error {}", what, static_cast<u32>(code))};
}

/// A security descriptor whose protected DACL grants the pipe to this process's user and to
/// LocalSystem only. The default named-pipe DACL also gives Everyone and Anonymous read access, which
/// would let another local user open the endpoint and receive notifications.
class PipeSecurity {
public:
    PipeSecurity() = default;
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    ~PipeSecurity() {
        if (m_descriptor) ::LocalFree(m_descriptor);
    }

    Result<void> init() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return winError("OpenProcessToken");
        DWORD bytes = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> user(bytes);
        const BOOL got = bytes > 0 && ::GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
        const DWORD err = ::GetLastError();
        ::CloseHandle(token);
        if (!got) return winError("GetTokenInformation(TokenUser)", err);
        LPWSTR sid = nullptr;
        if (!::ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &sid)) {
            return winError("ConvertSidToStringSidW");
        }
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
        ::LocalFree(sid);
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr)) {
            return winError("ConvertStringSecurityDescriptorToSecurityDescriptorW");
        }
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = m_descriptor;
        m_attributes.bInheritHandle = FALSE;
        return {};
    }

    SECURITY_ATTRIBUTES* attributes() noexcept { return &m_attributes; }

private:
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES m_attributes{};
};

HANDLE createPipeInstance(const std::wstring& path, bool first, PipeSecurity& security) {
    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    return ::CreateNamedPipeW(path.c_str(), openMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, security.attributes());
}

bool isDisconnect(DWORD code) noexcept {
    return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_NO_DATA ||
           code == ERROR_OPERATION_ABORTED;
}

class Win32Connection final : public Connection {
public:
    explicit Win32Connection(HANDLE pipe)
        : m_pipe(pipe), m_stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_readEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_writeEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Win32Connection() override {
        shutdown();
        // No DisconnectNamedPipe: it discards data the client has not read yet (a final error
        // line), and the instance is never reused. Closing the handle lets the client read what
        // is buffered, then see the pipe as broken.
        ::CloseHandle(m_pipe);
        for (HANDLE h : {m_stop, m_readEvent, m_writeEvent}) {
            if (h) ::CloseHandle(h);
        }
    }

    Result<usize> read(void* buffer, usize size, i32 timeoutMs) override {
        if (m_shut.load()) return usize{0};
        OVERLAPPED ov{};
        ov.hEvent = m_readEvent;
        ::ResetEvent(m_readEvent);
        const DWORD want = static_cast<DWORD>(std::min<usize>(size, 1u << 20));
        DWORD got = 0;
        if (::ReadFile(m_pipe, buffer, want, &got, &ov)) return static_cast<usize>(got);
        DWORD err = ::GetLastError();
        if (err != ERROR_IO_PENDING) {
            if (isDisconnect(err)) return usize{0};
            return winError("ReadFile", err);
        }
        const HANDLE waits[2] = {m_readEvent, m_stop};
        const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
        if (r != WAIT_OBJECT_0) ::CancelIoEx(m_pipe, &ov);
        if (::GetOverlappedResult(m_pipe, &ov, &got, TRUE)) {
            if (got > 0 || r == WAIT_OBJECT_0) return static_cast<usize>(got);
        } else {
            err = ::GetLastError();
            if (r == WAIT_OBJECT_0 && !isDisconnect(err)) return winError("ReadFile", err);
        }
        if (r == WAIT_OBJECT_0 + 1 || m_shut.load()) return usize{0};
        if (r == WAIT_TIMEOUT) return Error{ErrorCode::Timeout, "read timed out"};
        return usize{0};
    }

    Result<void> write(const void* data, usize size) override {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            if (m_shut.load()) return Error{ErrorCode::IoError, "connection shut down"};
            OVERLAPPED ov{};
            ov.hEvent = m_writeEvent;
            ::ResetEvent(m_writeEvent);
            const DWORD chunk = static_cast<DWORD>(std::min<usize>(size, 1u << 20));
            DWORD done = 0;
            if (!::WriteFile(m_pipe, p, chunk, &done, &ov)) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING) return winError("WriteFile", err);
                const HANDLE waits[2] = {m_writeEvent, m_stop};
                const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (r != WAIT_OBJECT_0) ::CancelIoEx(m_pipe, &ov);
                if (!::GetOverlappedResult(m_pipe, &ov, &done, TRUE)) return winError("WriteFile", ::GetLastError());
            }
            p += done;
            size -= done;
        }
        return {};
    }

    void shutdown() noexcept override {
        if (m_shut.exchange(true)) return;
        ::SetEvent(m_stop);
        ::CancelIoEx(m_pipe, nullptr);
    }

    Result<void> shutdownWrite() override {
        return Error{ErrorCode::Unsupported, "a named pipe has no half-close"};
    }

private:
    HANDLE m_pipe;
    HANDLE m_stop;
    HANDLE m_readEvent;
    HANDLE m_writeEvent;
    std::atomic<bool> m_shut{false};
};

class Win32Listener final : public Listener {
public:
    Win32Listener(std::wstring widePath, std::string path, HANDLE first, std::unique_ptr<PipeSecurity> security)
        : m_widePath(std::move(widePath)), m_path(std::move(path)), m_security(std::move(security)), m_next(first),
          m_stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_connectEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Win32Listener() override {
        shutdown();
        if (m_next != INVALID_HANDLE_VALUE) ::CloseHandle(m_next);
        for (HANDLE h : {m_stop, m_connectEvent}) {
            if (h) ::CloseHandle(h);
        }
    }

    Result<std::unique_ptr<Connection>> accept() override {
        for (;;) {
            if (m_shut.load()) return Error{ErrorCode::Cancelled, "listener shut down"};
            if (m_next == INVALID_HANDLE_VALUE) {
                m_next = createPipeInstance(m_widePath, false, *m_security);
                if (m_next == INVALID_HANDLE_VALUE) return winError("CreateNamedPipeW");
            }
            OVERLAPPED ov{};
            ov.hEvent = m_connectEvent;
            ::ResetEvent(m_connectEvent);
            bool connected = ::ConnectNamedPipe(m_next, &ov) != FALSE;
            if (!connected) {
                const DWORD err = ::GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (err == ERROR_IO_PENDING) {
                    const HANDLE waits[2] = {m_connectEvent, m_stop};
                    const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    DWORD unused = 0;
                    if (r != WAIT_OBJECT_0) {
                        ::CancelIoEx(m_next, &ov);
                        ::GetOverlappedResult(m_next, &ov, &unused, TRUE);
                        return Error{ErrorCode::Cancelled, "listener shut down"};
                    }
                    connected = ::GetOverlappedResult(m_next, &ov, &unused, FALSE) != FALSE;
                    if (!connected) {
                        // The client went away before we saw it; recycle the instance.
                        ::DisconnectNamedPipe(m_next);
                        continue;
                    }
                } else if (err == ERROR_NO_DATA) {
                    ::DisconnectNamedPipe(m_next);
                    continue;
                } else {
                    return winError("ConnectNamedPipe", err);
                }
            }
            HANDLE pipe = m_next;
            m_next = INVALID_HANDLE_VALUE;
            return std::unique_ptr<Connection>(new Win32Connection(pipe));
        }
    }

    void shutdown() noexcept override {
        if (m_shut.exchange(true)) return;
        ::SetEvent(m_stop);
    }

    const std::string& path() const noexcept override { return m_path; }

private:
    std::wstring m_widePath;
    std::string m_path;
    std::unique_ptr<PipeSecurity> m_security;
    HANDLE m_next;
    HANDLE m_stop;
    HANDLE m_connectEvent;
    std::atomic<bool> m_shut{false};
};

} // namespace

std::string endpointPath(std::string_view name) {
    return "\\\\.\\pipe\\" + std::string(name);
}

Result<std::unique_ptr<Listener>> Listener::listen(std::string_view name) {
    if (!isValidEndpointName(name)) return Error{ErrorCode::InvalidArgument, std::format("invalid endpoint name '{}'", name)};
    const std::string path = endpointPath(name);
    const std::wstring wide = utf8ToWide(path);
    auto security = std::make_unique<PipeSecurity>();
    HELIOS_TRY(security->init());
    HANDLE first = createPipeInstance(wide, true, *security);
    if (first == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) {
            return Error{ErrorCode::AlreadyExists, std::format("endpoint {} is in use", path)};
        }
        return winError("CreateNamedPipeW", err);
    }
    return std::unique_ptr<Listener>(new Win32Listener(wide, path, first, std::move(security)));
}

Result<std::unique_ptr<Connection>> connect(std::string_view name, i32 timeoutMs) {
    if (!isValidEndpointName(name)) return Error{ErrorCode::InvalidArgument, std::format("invalid endpoint name '{}'", name)};
    const std::string path = endpointPath(name);
    const std::wstring wide = utf8ToWide(path);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    for (;;) {
        HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) return std::unique_ptr<Connection>(new Win32Connection(h));
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY) return winError(std::format("connect {}", path), err);
        if (std::chrono::steady_clock::now() >= deadline) return Error{ErrorCode::Timeout, std::format("no server at {}", path)};
        if (err == ERROR_PIPE_BUSY) {
            ::WaitNamedPipeW(wide.c_str(), 50);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

} // namespace helios::tf::ipc
