// POSIX half of ToolsFramework's OS services: process ids, host name and the Unix-domain-socket
// transport of the remote-control endpoint (ipc.h).

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#include "helios/toolsfw/ipc.h"

#include "../tf_os.h"

namespace helios::tf::os {

u32 currentProcessId() noexcept {
    return static_cast<u32>(::getpid());
}

std::string hostName() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return std::string(buf);
}

bool processIsRunning(u32 pid) noexcept {
    if (pid == 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
}

std::string environment(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string runtimeDirectory() {
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR"}) {
        const char* v = std::getenv(var);
        struct stat st {};
        if (v && *v && ::stat(v, &st) == 0 && S_ISDIR(st.st_mode)) return std::string(v);
    }
    return "/tmp";
}

} // namespace helios::tf::os

namespace helios::tf::ipc {

namespace {

Error sysError(std::string_view what) {
    const int e = errno;
    ErrorCode code = ErrorCode::IoError;
    if (e == ENOENT) code = ErrorCode::NotFound;
    if (e == EACCES || e == EPERM) code = ErrorCode::PermissionDenied;
    if (e == EADDRINUSE) code = ErrorCode::AlreadyExists;
    return Error{code, std::format("{}: {}", what, std::strerror(e))};
}

bool makeWakePipe(int fds[2]) {
    if (::pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[i], F_SETFL, O_NONBLOCK);
    }
    return true;
}

void closeFd(int& fd) noexcept {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

Result<sockaddr_un> socketAddress(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return Error{ErrorCode::InvalidArgument, std::format("socket path too long: {}", path)};
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

class PosixConnection final : public Connection {
public:
    explicit PosixConnection(int fd) : m_fd(fd) {
        if (!makeWakePipe(m_wake)) m_wake[0] = m_wake[1] = -1;
    }
    ~PosixConnection() override {
        closeFd(m_fd);
        closeFd(m_wake[0]);
        closeFd(m_wake[1]);
    }

    Result<usize> read(void* buffer, usize size, i32 timeoutMs) override {
        if (m_shut.load()) return usize{0};
        pollfd fds[2] = {{m_fd, POLLIN, 0}, {m_wake[0], POLLIN, 0}};
        const nfds_t n = m_wake[0] >= 0 ? 2 : 1;
        for (;;) {
            const int r = ::poll(fds, n, timeoutMs < 0 ? -1 : timeoutMs);
            if (r < 0) {
                if (errno == EINTR) continue;
                return sysError("poll");
            }
            if (r == 0) return Error{ErrorCode::Timeout, "read timed out"};
            break;
        }
        if (n == 2 && (fds[1].revents & POLLIN)) return usize{0};
        for (;;) {
            const ssize_t got = ::recv(m_fd, buffer, size, 0);
            if (got >= 0) return static_cast<usize>(got);
            if (errno == EINTR) continue;
            if (errno == ECONNRESET) return usize{0};
            return sysError("recv");
        }
    }

    Result<void> write(const void* data, usize size) override {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            const ssize_t sent = ::send(m_fd, p, size, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) continue;
                return sysError("send");
            }
            p += sent;
            size -= static_cast<usize>(sent);
        }
        return {};
    }

    void shutdown() noexcept override {
        if (m_shut.exchange(true)) return;
        if (m_wake[1] >= 0) {
            const char c = 1;
            [[maybe_unused]] const ssize_t w = ::write(m_wake[1], &c, 1);
        }
        ::shutdown(m_fd, SHUT_RDWR);
    }

private:
    int m_fd;
    int m_wake[2] = {-1, -1};
    std::atomic<bool> m_shut{false};
};

class PosixListener final : public Listener {
public:
    PosixListener(int fd, std::string path) : m_fd(fd), m_path(std::move(path)) {
        if (!makeWakePipe(m_wake)) m_wake[0] = m_wake[1] = -1;
    }
    ~PosixListener() override {
        closeFd(m_fd);
        closeFd(m_wake[0]);
        closeFd(m_wake[1]);
        ::unlink(m_path.c_str());
    }

    Result<std::unique_ptr<Connection>> accept() override {
        for (;;) {
            if (m_shut.load()) return Error{ErrorCode::Cancelled, "listener shut down"};
            pollfd fds[2] = {{m_fd, POLLIN, 0}, {m_wake[0], POLLIN, 0}};
            const int r = ::poll(fds, m_wake[0] >= 0 ? 2 : 1, -1);
            if (r < 0) {
                if (errno == EINTR) continue;
                return sysError("poll");
            }
            if (m_wake[0] >= 0 && (fds[1].revents & POLLIN)) return Error{ErrorCode::Cancelled, "listener shut down"};
            const int c = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
                return sysError("accept");
            }
            return std::unique_ptr<Connection>(new PosixConnection(c));
        }
    }

    void shutdown() noexcept override {
        if (m_shut.exchange(true)) return;
        if (m_wake[1] >= 0) {
            const char c = 1;
            [[maybe_unused]] const ssize_t w = ::write(m_wake[1], &c, 1);
        }
    }

    const std::string& path() const noexcept override { return m_path; }

private:
    int m_fd;
    std::string m_path;
    int m_wake[2] = {-1, -1};
    std::atomic<bool> m_shut{false};
};

/// True when a live server accepts connections on `path`.
bool endpointLive(const sockaddr_un& addr) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const bool live = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return live;
}

} // namespace

std::string endpointPath(std::string_view name) {
    return os::runtimeDirectory() + "/" + std::string(name) + ".sock";
}

Result<std::unique_ptr<Listener>> Listener::listen(std::string_view name) {
    if (!isValidEndpointName(name)) return Error{ErrorCode::InvalidArgument, std::format("invalid endpoint name '{}'", name)};
    const std::string path = endpointPath(name);
    HELIOS_TRY_ASSIGN(const sockaddr_un addr, socketAddress(path));
    if (::access(path.c_str(), F_OK) == 0) {
        if (endpointLive(addr)) return Error{ErrorCode::AlreadyExists, std::format("endpoint {} is in use", path)};
        ::unlink(path.c_str());  // stale socket of a crashed process
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return sysError("socket");
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        Error e = sysError(std::format("bind {}", path));
        ::close(fd);
        return e;
    }
    // Owner-only access: the socket drives the editor.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    if (::listen(fd, 16) != 0) {
        Error e = sysError("listen");
        ::close(fd);
        ::unlink(path.c_str());
        return e;
    }
    return std::unique_ptr<Listener>(new PosixListener(fd, path));
}

Result<std::unique_ptr<Connection>> connect(std::string_view name, i32 timeoutMs) {
    if (!isValidEndpointName(name)) return Error{ErrorCode::InvalidArgument, std::format("invalid endpoint name '{}'", name)};
    const std::string path = endpointPath(name);
    HELIOS_TRY_ASSIGN(const sockaddr_un addr, socketAddress(path));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    for (;;) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return sysError("socket");
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            return std::unique_ptr<Connection>(new PosixConnection(fd));
        }
        const int e = errno;
        ::close(fd);
        if (e != ENOENT && e != ECONNREFUSED && e != EAGAIN) {
            errno = e;
            return sysError(std::format("connect {}", path));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error{ErrorCode::Timeout, std::format("no server at {}", path)};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace helios::tf::ipc
