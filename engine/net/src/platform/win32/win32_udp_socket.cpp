// Win32 (MSVC, clang-cl, MinGW-w64) implementation of UdpSocket over Winsock2. Batches loop
// sendto/recvfrom; WSARecvMsg loops and RIO are the Phase 2 options (04 §2.6).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "helios/net/udp_socket.h"
#include "platform/net_os.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace helios::net {
namespace {

/// WSAStartup once per process; WSACleanup at exit. Thread-safe (static initialisation).
struct WinsockInit {
    bool ok = false;
    WinsockInit() noexcept {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};

bool ensureWinsock() noexcept {
    static WinsockInit init;
    return init.ok;
}

SOCKET toSocket(std::intptr_t h) noexcept { return static_cast<SOCKET>(h); }

int toSockaddr(const Address& to, AddressFamily socketFamily, sockaddr_storage& ss) noexcept {
    std::memset(&ss, 0, sizeof(ss));
    const Address a = (socketFamily == AddressFamily::IPv6) ? to.toV4Mapped() : to.unmapped();
    if (a.isIpv4() && socketFamily == AddressFamily::IPv4) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(a.port());
        std::memcpy(&sin->sin_addr, a.bytes().data(), 4);
        return static_cast<int>(sizeof(sockaddr_in));
    }
    if (a.isIpv6() && socketFamily == AddressFamily::IPv6) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(a.port());
        std::memcpy(&sin6->sin6_addr, a.bytes().data(), 16);
        return static_cast<int>(sizeof(sockaddr_in6));
    }
    return 0;
}

Address fromSockaddr(const sockaddr_storage& ss, bool unmap) noexcept {
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
        const Address a = Address::ipv6Bytes(b, ntohs(sin6->sin6_port));
        return unmap ? a.unmapped() : a;
    }
    return {};
}

u32 setBufferSize(SOCKET s, int option, u32 requested) noexcept {
    int value = static_cast<int>(std::min<u32>(requested, 0x7FFFFFFFu));
    (void)setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
    int actual = 0;
    int len = sizeof(actual);
    getsockopt(s, SOL_SOCKET, option, reinterpret_cast<char*>(&actual), &len);
    return actual > 0 ? static_cast<u32>(actual) : 0;
}

bool isWouldBlock(int e) noexcept { return e == WSAEWOULDBLOCK || e == WSAENOBUFS; }

/// ICMP-driven errors that must never stall the receive loop.
bool isTransientReceiveError(int e) noexcept {
    return e == WSAECONNRESET || e == WSAENETRESET || e == WSAECONNREFUSED || e == WSAEHOSTUNREACH ||
           e == WSAENETUNREACH;
}

} // namespace

bool UdpSocket::isIpv6Supported() noexcept {
    static const bool supported = [] {
        if (!ensureWinsock()) return false;
        const SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return false;
        closesocket(s);
        return true;
    }();
    return supported;
}

Result<UdpSocket> UdpSocket::open(const UdpSocketConfig& config) {
    if (!ensureWinsock()) return Error{ErrorCode::Unsupported, "UdpSocket: WSAStartup failed"};
    const AddressFamily family = config.bindAddress.family();
    if (family == AddressFamily::None) return Error{ErrorCode::InvalidArgument, "UdpSocket: invalid bind address"};
    const SOCKET s = ::socket(family == AddressFamily::IPv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        const int e = WSAGetLastError();
        return makeError(e == WSAEAFNOSUPPORT ? ErrorCode::Unsupported : ErrorCode::IoError,
                         "UdpSocket: socket() failed ({})", e);
    }
    UdpSocket sock;
    sock.m_handle = static_cast<std::intptr_t>(s);

    (void)SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    if (family == AddressFamily::IPv6) {
        const DWORD v6only = config.dualStack ? 0 : 1;
        if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0) {
            return makeError(ErrorCode::IoError, "UdpSocket: IPV6_V6ONLY failed ({})", WSAGetLastError());
        }
    }
    if (config.reuseAddress) {
        const BOOL one = TRUE;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    }
    // An ICMP port-unreachable for one peer must not make recvfrom fail for all others.
    {
        BOOL newBehavior = FALSE;
        DWORD bytesReturned = 0;
        (void)WSAIoctl(s, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytesReturned,
                       nullptr, nullptr);
    }
    sock.m_sendBuffer = setBufferSize(s, SO_SNDBUF, config.sendBufferBytes);
    sock.m_receiveBuffer = setBufferSize(s, SO_RCVBUF, config.receiveBufferBytes);

    u_long nonBlocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonBlocking) != 0) {
        return makeError(ErrorCode::IoError, "UdpSocket: FIONBIO failed ({})", WSAGetLastError());
    }

    sockaddr_storage ss{};
    const int len = toSockaddr(config.bindAddress, family, ss);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&ss), len) != 0) {
        const int e = WSAGetLastError();
        return makeError(e == WSAEADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::IoError,
                         "UdpSocket: bind {} failed ({})", config.bindAddress, e);
    }
    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        return makeError(ErrorCode::IoError, "UdpSocket: getsockname failed ({})", WSAGetLastError());
    }
    sock.m_local = fromSockaddr(bound, /*unmap=*/false);
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
        closesocket(toSocket(m_handle));
        m_handle = kInvalidHandle;
    }
}

Result<void> UdpSocket::sendTo(const Address& to, std::span<const u8> data) noexcept {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "socket closed"};
    sockaddr_storage ss{};
    const int len = toSockaddr(to, m_local.family(), ss);
    if (len == 0) {
        ++m_stats.sendErrors;
        return Error{ErrorCode::InvalidArgument, "address family not reachable from this socket"};
    }
    ++m_stats.sendSyscalls;
    const int r = ::sendto(toSocket(m_handle), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                           0, reinterpret_cast<const sockaddr*>(&ss), len);
    if (r != SOCKET_ERROR) {
        ++m_stats.datagramsSent;
        m_stats.bytesSent += data.size();
        return {};
    }
    const int e = WSAGetLastError();
    if (isWouldBlock(e)) {
        ++m_stats.sendWouldBlock;
        return Error{ErrorCode::Busy, "send buffer full"};
    }
    ++m_stats.sendErrors;
    return makeError(ErrorCode::IoError, "sendto failed ({})", e);
}

usize UdpSocket::sendBatch(std::span<const OutDatagram> datagrams) noexcept {
    usize sent = 0;
    for (const OutDatagram& d : datagrams) {
        if (sendTo(d.to, d.data)) ++sent;
    }
    return sent;
}

usize UdpSocket::receiveFrom(Address& from, std::span<u8> buffer) noexcept {
    if (!isOpen()) return 0;
    for (;;) {
        sockaddr_storage ss{};
        int ssLen = sizeof(ss);
        ++m_stats.receiveSyscalls;
        const int r = ::recvfrom(toSocket(m_handle), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                 0, reinterpret_cast<sockaddr*>(&ss), &ssLen);
        if (r == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            if (e == WSAEMSGSIZE) {
                ++m_stats.truncated;
                continue;
            }
            if (isTransientReceiveError(e)) continue;
            ++m_stats.receiveErrors;
            return 0;
        }
        if (r == 0) continue;
        from = fromSockaddr(ss, /*unmap=*/true);
        ++m_stats.datagramsReceived;
        m_stats.bytesReceived += static_cast<u64>(r);
        return static_cast<usize>(r);
    }
}

usize UdpSocket::receiveBatch(std::span<InDatagram> slots) noexcept {
    usize filled = 0;
    while (filled < slots.size()) {
        InDatagram& s = slots[filled];
        const usize size = receiveFrom(s.from, s.buffer);
        if (size == 0) break;
        s.size = size;
        ++filled;
    }
    return filled;
}

namespace os {
f64 threadCpuSeconds() noexcept {
    FILETIME creation, exitTime, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exitTime, &kernel, &user)) return 0.0;
    const auto toU64 = [](const FILETIME& ft) {
        return (static_cast<u64>(ft.dwHighDateTime) << 32) | static_cast<u64>(ft.dwLowDateTime);
    };
    return static_cast<f64>(toU64(kernel) + toU64(user)) * 1e-7; // 100 ns units
}
} // namespace os

} // namespace helios::net
