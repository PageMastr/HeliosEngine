#pragma once
// L0 of HTP (04 §2): a non-blocking UDP socket over Winsock2 (Windows) or BSD sockets (POSIX).
//
// * IPv4 and IPv6; an IPv6 socket is dual-stack by default (IPV6_V6ONLY off), receives IPv4
//   traffic as ::ffff:a.b.c.d and reports it as plain IPv4 (Address::unmapped), and maps IPv4
//   destinations on send, so callers only ever see the addresses that appear in connect tokens.
// * Batched I/O: sendBatch/receiveBatch use sendmmsg/recvmmsg on Linux (one syscall per batch),
//   and loop sendto/recvfrom elsewhere (WSARecvMsg/RIO is the Phase 2 option on Windows, 04 §2.6).
// * Socket buffers are sized on open (8 MB for gateways/clients, 32 MB for trunks); when the OS
//   caps SO_RCVBUF/SO_SNDBUF (Linux rmem_max) the socket retries with SO_*BUFFORCE, which works
//   for privileged processes, and reports the size it actually got.
// * Windows: SIO_UDP_CONNRESET is switched off so an ICMP port-unreachable from one peer cannot
//   make recvfrom fail for everyone else.
//
// Threading: a socket is owned by one thread (the endpoint's I/O thread). isIpv6Supported() and
// the Winsock start-up it performs are thread-safe.

#include <span>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/address.h"

namespace helios::net {

struct UdpSocketConfig {
    /// Local address to bind. Its family selects IPv4 or IPv6; port 0 picks an ephemeral port.
    Address bindAddress = Address::anyV4(0);
    /// IPv6 sockets also accept IPv4 (IPV6_V6ONLY = 0). Ignored for IPv4 sockets.
    bool dualStack = true;
    u32 sendBufferBytes = 8u * 1024 * 1024;
    u32 receiveBufferBytes = 8u * 1024 * 1024;
    /// SO_REUSEADDR (tests that rebind a port quickly). Off by default.
    bool reuseAddress = false;
};

/// One datagram to send.
struct OutDatagram {
    Address to;
    std::span<const u8> data;
};

/// One receive slot: fill `buffer` before calling receiveBatch; `size` and `from` are outputs.
struct InDatagram {
    std::span<u8> buffer;
    usize size = 0;
    Address from;
};

struct UdpSocketStats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 bytesSent = 0;
    u64 bytesReceived = 0;
    u64 sendWouldBlock = 0; ///< Datagrams dropped because the send buffer was full.
    u64 sendErrors = 0;
    u64 receiveErrors = 0;
    u64 truncated = 0;      ///< Datagrams larger than the receive buffer (dropped).
    u64 sendSyscalls = 0;
    u64 receiveSyscalls = 0;
};

class UdpSocket {
public:
    UdpSocket() noexcept = default;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket();

    /// Creates, configures (non-blocking, buffers, dual-stack) and binds a socket.
    static Result<UdpSocket> open(const UdpSocketConfig& config);

    /// False when this process cannot create IPv6 sockets (IPv6 disabled in the kernel/container).
    static bool isIpv6Supported() noexcept;

    bool isOpen() const noexcept { return m_handle != kInvalidHandle; }
    void close() noexcept;

    /// Sends one datagram. ErrorCode::Busy when the socket buffer is full (the datagram is
    /// dropped, as UDP would), IoError otherwise.
    Result<void> sendTo(const Address& to, std::span<const u8> data) noexcept;
    /// Sends as many datagrams as the socket accepts; returns how many were handed to the OS.
    /// Datagrams refused with a full buffer are counted in stats().sendWouldBlock and skipped.
    usize sendBatch(std::span<const OutDatagram> datagrams) noexcept;

    /// Receives one datagram; returns its size, 0 when nothing is pending. Truncated datagrams and
    /// ICMP-induced errors (connection reset/refused) are skipped.
    usize receiveFrom(Address& from, std::span<u8> buffer) noexcept;
    /// Fills up to slots.size() slots; returns how many were filled.
    usize receiveBatch(std::span<InDatagram> slots) noexcept;

    /// The bound address (with the actual port when bound to port 0).
    const Address& localAddress() const noexcept { return m_local; }
    /// Buffer sizes the OS actually granted (may be below the request, see the header comment).
    u32 sendBufferBytes() const noexcept { return m_sendBuffer; }
    u32 receiveBufferBytes() const noexcept { return m_receiveBuffer; }
    AddressFamily family() const noexcept { return m_local.family(); }
    const UdpSocketStats& stats() const noexcept { return m_stats; }

private:
    static constexpr std::intptr_t kInvalidHandle = -1;

    std::intptr_t m_handle = kInvalidHandle;
    Address m_local;
    u32 m_sendBuffer = 0;
    u32 m_receiveBuffer = 0;
    UdpSocketStats m_stats;
};

} // namespace helios::net
