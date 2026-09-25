// POSIX (Linux, macOS, other Unix) implementation of UdpSocket. Linux batches with
// sendmmsg/recvmmsg; other systems loop sendto/recvmsg.

#include "helios/net/udp_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>

#include "platform/net_os.h"

namespace helios::net {
namespace {

constexpr usize kMaxBatch = 64;

/// Fills `ss` for sending to `to` from a socket of `socketFamily` (IPv4 destinations are mapped
/// for IPv6 sockets). Returns 0 when the destination is unreachable from this socket family.
socklen_t toSockaddr(const Address& to, AddressFamily socketFamily, sockaddr_storage& ss) noexcept {
    std::memset(&ss, 0, sizeof(ss));
    const Address a = (socketFamily == AddressFamily::IPv6) ? to.toV4Mapped() : to.unmapped();
    if (a.isIpv4() && socketFamily == AddressFamily::IPv4) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(a.port());
        std::memcpy(&sin->sin_addr, a.bytes().data(), 4);
        return sizeof(sockaddr_in);
    }
    if (a.isIpv6() && socketFamily == AddressFamily::IPv6) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(a.port());
        std::memcpy(&sin6->sin6_addr, a.bytes().data(), 16);
        return sizeof(sockaddr_in6);
    }
    return 0;
}

Address fromSockaddr(const sockaddr_storage& ss) noexcept {
    if (ss.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
        std::array<u8, 4> b{};
        std::memcpy(b.data(), &sin->sin_addr, 4);
        return Address::ipv4(b, ntohs(sin->sin_port));
    }
    if (ss.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        std::array<u8, 16> b{};
        std::memcpy(b.data(), &sin6->sin6_addr, 16);
        return Address::ipv6Bytes(b, ntohs(sin6->sin6_port)).unmapped();
    }
    return {};
}

bool isWouldBlock(int e) noexcept { return e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS; }

/// Sets a socket buffer size, retrying with the privileged *BUFFORCE option when the kernel cap
/// (rmem_max/wmem_max) clamps the request. Returns the size the kernel reports.
u32 setBufferSize(int fd, int option, int forceOption, u32 requested) noexcept {
    int value = static_cast<int>(std::min<u32>(requested, 0x7FFFFFFFu));
    (void)setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value));
    int actual = 0;
    socklen_t len = sizeof(actual);
    getsockopt(fd, SOL_SOCKET, option, &actual, &len);
    if (forceOption != 0 && actual >= 0 && static_cast<u32>(actual) < requested) {
        if (setsockopt(fd, SOL_SOCKET, forceOption, &value, sizeof(value)) == 0) {
            len = sizeof(actual);
            getsockopt(fd, SOL_SOCKET, option, &actual, &len);
        }
    }
    return actual > 0 ? static_cast<u32>(actual) : 0;
}

} // namespace

bool UdpSocket::isIpv6Supported() noexcept {
    static const bool supported = [] {
        const int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return false;
        ::close(fd);
        return true;
    }();
    return supported;
}

Result<UdpSocket> UdpSocket::open(const UdpSocketConfig& config) {
    const AddressFamily family = config.bindAddress.family();
    if (family == AddressFamily::None) return Error{ErrorCode::InvalidArgument, "UdpSocket: invalid bind address"};
    const int fd = ::socket(family == AddressFamily::IPv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        const int e = errno;
        return makeError(e == EAFNOSUPPORT ? ErrorCode::Unsupported : ErrorCode::IoError, "UdpSocket: socket(): {}",
                         std::strerror(e));
    }
    UdpSocket sock;
    sock.m_handle = fd;

    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (family == AddressFamily::IPv6) {
        const int v6only = config.dualStack ? 0 : 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
            return makeError(ErrorCode::IoError, "UdpSocket: IPV6_V6ONLY: {}", std::strerror(errno));
        }
    }
    if (config.reuseAddress) {
        const int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#if defined(__linux__)
    constexpr int kSndForce = SO_SNDBUFFORCE;
    constexpr int kRcvForce = SO_RCVBUFFORCE;
#else
    constexpr int kSndForce = 0;
    constexpr int kRcvForce = 0;
#endif
    sock.m_sendBuffer = setBufferSize(fd, SO_SNDBUF, kSndForce, config.sendBufferBytes);
    sock.m_receiveBuffer = setBufferSize(fd, SO_RCVBUF, kRcvForce, config.receiveBufferBytes);

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return makeError(ErrorCode::IoError, "UdpSocket: O_NONBLOCK: {}", std::strerror(errno));
    }

    sockaddr_storage ss{};
    const socklen_t len = toSockaddr(config.bindAddress, family, ss);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&ss), len) != 0) {
        const int e = errno;
        return makeError(e == EADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::IoError, "UdpSocket: bind {}: {}",
                         config.bindAddress, std::strerror(e));
    }
    sockaddr_storage bound{};
    socklen_t boundLen = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        return makeError(ErrorCode::IoError, "UdpSocket: getsockname: {}", std::strerror(errno));
    }
    // Keep the socket's own family for the local address (a dual-stack [::] bind stays IPv6).
    if (bound.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&bound);
        std::array<u8, 16> b{};
        std::memcpy(b.data(), &sin6->sin6_addr, 16);
        sock.m_local = Address::ipv6Bytes(b, ntohs(sin6->sin6_port));
    } else {
        sock.m_local = fromSockaddr(bound);
    }
    return sock;
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kInvalidHandle)), m_local(other.m_local),
      m_sendBuffer(other.m_sendBuffer), m_receiveBuffer(other.m_receiveBuffer), m_stats(other.m_stats) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, kInvalidHandle);
        m_local = other.m_local;
        m_sendBuffer = other.m_sendBuffer;
        m_receiveBuffer = other.m_receiveBuffer;
        m_stats = other.m_stats;
    }
    return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() noexcept {
    if (m_handle != kInvalidHandle) {
        ::close(static_cast<int>(m_handle));
        m_handle = kInvalidHandle;
    }
}

Result<void> UdpSocket::sendTo(const Address& to, std::span<const u8> data) noexcept {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "socket closed"};
    sockaddr_storage ss{};
    const socklen_t len = toSockaddr(to, m_local.family(), ss);
    if (len == 0) {
        ++m_stats.sendErrors;
        return Error{ErrorCode::InvalidArgument, "address family not reachable from this socket"};
    }
    ++m_stats.sendSyscalls;
    for (;;) {
        const ssize_t r = ::sendto(static_cast<int>(m_handle), data.data(), data.size(), 0,
                                   reinterpret_cast<const sockaddr*>(&ss), len);
        if (r >= 0) {
            ++m_stats.datagramsSent;
            m_stats.bytesSent += data.size();
            return {};
        }
        const int e = errno;
        if (e == EINTR) continue;
        if (isWouldBlock(e)) {
            ++m_stats.sendWouldBlock;
            return Error{ErrorCode::Busy, "send buffer full"};
        }
        ++m_stats.sendErrors;
        return makeError(ErrorCode::IoError, "sendto: {}", std::strerror(e));
    }
}

usize UdpSocket::sendBatch(std::span<const OutDatagram> datagrams) noexcept {
    if (!isOpen()) return 0;
    usize sent = 0;
#if defined(__linux__)
    std::array<mmsghdr, kMaxBatch> hdrs;
    std::array<iovec, kMaxBatch> iovs;
    std::array<sockaddr_storage, kMaxBatch> addrs;
    usize i = 0;
    while (i < datagrams.size()) {
        // Build one batch, skipping datagrams whose family this socket cannot reach.
        usize n = 0;
        std::array<usize, kMaxBatch> sourceIndex{};
        while (i < datagrams.size() && n < kMaxBatch) {
            const OutDatagram& d = datagrams[i];
            const socklen_t len = toSockaddr(d.to, m_local.family(), addrs[n]);
            if (len == 0) {
                ++m_stats.sendErrors;
                ++i;
                continue;
            }
            iovs[n].iov_base = const_cast<u8*>(d.data.data());
            iovs[n].iov_len = d.data.size();
            std::memset(&hdrs[n], 0, sizeof(mmsghdr));
            hdrs[n].msg_hdr.msg_name = &addrs[n];
            hdrs[n].msg_hdr.msg_namelen = len;
            hdrs[n].msg_hdr.msg_iov = &iovs[n];
            hdrs[n].msg_hdr.msg_iovlen = 1;
            sourceIndex[n] = i;
            ++n;
            ++i;
        }
        usize done = 0;
        while (done < n) {
            ++m_stats.sendSyscalls;
            const int r = ::sendmmsg(static_cast<int>(m_handle), hdrs.data() + done, static_cast<unsigned>(n - done), 0);
            if (r > 0) {
                for (usize k = 0; k < static_cast<usize>(r); ++k) m_stats.bytesSent += datagrams[sourceIndex[done + k]].data.size();
                m_stats.datagramsSent += static_cast<u64>(r);
                sent += static_cast<usize>(r);
                done += static_cast<usize>(r);
                continue;
            }
            const int e = errno;
            if (r < 0 && e == EINTR) continue;
            // The first remaining datagram failed: drop it (UDP semantics) and carry on.
            if (r < 0 && isWouldBlock(e)) ++m_stats.sendWouldBlock;
            else ++m_stats.sendErrors;
            ++done;
        }
    }
#else
    for (const OutDatagram& d : datagrams) {
        if (sendTo(d.to, d.data)) ++sent;
    }
#endif
    return sent;
}

usize UdpSocket::receiveFrom(Address& from, std::span<u8> buffer) noexcept {
    if (!isOpen()) return 0;
    for (;;) {
        sockaddr_storage ss{};
        iovec iov{buffer.data(), buffer.size()};
        msghdr msg{};
        msg.msg_name = &ss;
        msg.msg_namelen = sizeof(ss);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ++m_stats.receiveSyscalls;
        const ssize_t r = ::recvmsg(static_cast<int>(m_handle), &msg, 0);
        if (r < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            if (isWouldBlock(e)) return 0;
            if (e == ECONNREFUSED || e == ECONNRESET || e == EHOSTUNREACH || e == ENETUNREACH) continue;
            ++m_stats.receiveErrors;
            return 0;
        }
        if (msg.msg_flags & MSG_TRUNC) {
            ++m_stats.truncated;
            continue;
        }
        if (r == 0) continue; // empty datagrams carry nothing for us
        from = fromSockaddr(ss);
        ++m_stats.datagramsReceived;
        m_stats.bytesReceived += static_cast<u64>(r);
        return static_cast<usize>(r);
    }
}

usize UdpSocket::receiveBatch(std::span<InDatagram> slots) noexcept {
    if (!isOpen() || slots.empty()) return 0;
#if defined(__linux__)
    usize filled = 0;
    std::array<mmsghdr, kMaxBatch> hdrs;
    std::array<iovec, kMaxBatch> iovs;
    std::array<sockaddr_storage, kMaxBatch> addrs;
    while (filled < slots.size()) {
        const usize n = std::min(kMaxBatch, slots.size() - filled);
        for (usize k = 0; k < n; ++k) {
            InDatagram& s = slots[filled + k];
            iovs[k].iov_base = s.buffer.data();
            iovs[k].iov_len = s.buffer.size();
            std::memset(&hdrs[k], 0, sizeof(mmsghdr));
            hdrs[k].msg_hdr.msg_name = &addrs[k];
            hdrs[k].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
            hdrs[k].msg_hdr.msg_iov = &iovs[k];
            hdrs[k].msg_hdr.msg_iovlen = 1;
        }
        ++m_stats.receiveSyscalls;
        const int r = ::recvmmsg(static_cast<int>(m_handle), hdrs.data(), static_cast<unsigned>(n), MSG_DONTWAIT, nullptr);
        if (r < 0) {
            const int e = errno;
            if (e == EINTR) continue;
            if (e == ECONNREFUSED || e == ECONNRESET || e == EHOSTUNREACH || e == ENETUNREACH) continue;
            if (!isWouldBlock(e)) ++m_stats.receiveErrors;
            break;
        }
        if (r == 0) break;
        // Compact: truncated/empty datagrams are dropped and later slots move up (their buffer
        // spans move with them, so slot buffers may be permuted).
        usize out = filled;
        for (usize k = 0; k < static_cast<usize>(r); ++k) {
            InDatagram& s = slots[filled + k];
            const unsigned len = hdrs[k].msg_len;
            if ((hdrs[k].msg_hdr.msg_flags & MSG_TRUNC) || len == 0) {
                if (len != 0) ++m_stats.truncated;
                continue;
            }
            s.size = len;
            s.from = fromSockaddr(addrs[k]);
            ++m_stats.datagramsReceived;
            m_stats.bytesReceived += len;
            if (out != filled + k) std::swap(slots[out], s);
            ++out;
        }
        filled = out;
        if (static_cast<usize>(r) < n) break; // socket drained
    }
    return filled;
#else
    usize filled = 0;
    while (filled < slots.size()) {
        InDatagram& s = slots[filled];
        const usize size = receiveFrom(s.from, s.buffer);
        if (size == 0) break;
        s.size = size;
        ++filled;
    }
    return filled;
#endif
}

namespace os {
f64 threadCpuSeconds() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
    return static_cast<f64>(ts.tv_sec) + static_cast<f64>(ts.tv_nsec) * 1e-9;
}
} // namespace os

} // namespace helios::net
