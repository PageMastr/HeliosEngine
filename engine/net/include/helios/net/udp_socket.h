#pragma once
// L0 of HTP (04 §2): a non-blocking UDP socket over Winsock2 (Windows) or BSD sockets (POSIX).
//
// * IPv4 and IPv6; an IPv6 socket is dual-stack by default (IPV6_V6ONLY off), receives IPv4
//   traffic as ::ffff:a.b.c.d and reports it as plain IPv4 (Address::unmapped), and maps IPv4
//   destinations on send, so callers only ever see the addresses that appear in connect tokens.
// * Batched I/O (04 §2.6; UdpBatchApi): sendBatch/receiveBatch use sendmmsg/recvmmsg on Linux (one
//   syscall per batch of up to 64) and Registered I/O on Windows (RIO: registered buffers, polled
//   completion queues, one kernel entry per batch), falling back to one WSASendMsg/WSARecvMsg (Windows)
//   or sendto/recvmsg (POSIX) per datagram where those are unavailable. Every API has the same
//   semantics: partial batches, full buffers counted and skipped, truncated datagrams dropped.
// * Socket buffers are sized on open (8 MB for gateways/clients, 32 MB for trunks); when the OS
//   caps SO_RCVBUF/SO_SNDBUF (Linux rmem_max) the socket retries with SO_*BUFFORCE, which works
//   for privileged processes, and reports the size it actually got.
// * Windows: SIO_UDP_CONNRESET is switched off so an ICMP port-unreachable from one peer cannot
//   make a receive fail for everyone else.
//
// Budget (NS-0.2, 04 §11.4): the full encrypted stack must cost <= 10 us of one core per packet, send
// and receive together (100k packets per core); this layer's share is measured by `net_bench --socket`.
//
// Threading: a socket is owned by one thread (the endpoint's I/O thread). isIpv6Supported() and
// the Winsock start-up it performs are thread-safe.

#include <span>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/address.h"

namespace helios::net {

/// How sendBatch/receiveBatch reach the OS (04 §2.6). A socket asks for one in
/// UdpSocketConfig::batchApi and reports the one it got in UdpSocket::batchApi().
enum class UdpBatchApi : u8 {
    /// The fastest the platform offers: Registered on Windows, MultiMessage on Linux, else Message.
    Auto,
    /// Windows Registered I/O (Windows 8 and later): datagrams are copied through registered 2 KB
    /// slots and completions are polled, so a batch costs one kernel entry. A RIO socket buffers
    /// nothing beyond its posted receives, so the receive slot count follows receiveBufferBytes (128 to
    /// 16,384 slots) and the send slot count sendBufferBytes (128 to 2,048: sends complete within
    /// microseconds); the region they take is locked in memory. A slot holds one datagram of any size,
    /// so the slots hold that many datagrams, not that many bytes of small ones. Datagrams of 2,048
    /// bytes or more are refused on send and dropped as truncated on receive (HTP datagrams are at most
    /// 1,300 bytes). A send counts as sent once it is posted; a failure its completion reports later is
    /// counted in sendErrors. When RIO is unavailable or its set-up fails, open() falls back to Message
    /// (logged once per process at warning level).
    Registered,
    /// Linux sendmmsg/recvmmsg: one system call per batch of up to 64 datagrams.
    MultiMessage,
    /// One call per datagram: WSASendMsg/WSARecvMsg on Windows, sendto/recvmsg on POSIX. Every platform
    /// has it, so a request for an API the OS lacks falls back to it.
    Message,
};

/// "auto", "registered", "multi-message" or "message" (logs and bench output). A pure function of its
/// argument, returning a static string: safe from any thread.
std::string_view udpBatchApiName(UdpBatchApi api) noexcept;

struct UdpSocketConfig {
    /// Local address to bind. Its family selects IPv4 or IPv6; port 0 picks an ephemeral port.
    Address bindAddress = Address::anyV4(0);
    /// IPv6 sockets also accept IPv4 (IPV6_V6ONLY = 0). Ignored for IPv4 sockets.
    bool dualStack = true;
    u32 sendBufferBytes = 8u * 1024 * 1024;
    u32 receiveBufferBytes = 8u * 1024 * 1024;
    /// SO_REUSEADDR (tests that rebind a port quickly). Off by default.
    bool reuseAddress = false;
    /// The batch API to use. Auto picks the fastest; tests and diagnostics force a fallback with
    /// Message. An API the OS lacks (Registered off Windows or when RIO cannot be set up,
    /// MultiMessage off Linux) falls back to Message.
    UdpBatchApi batchApi = UdpBatchApi::Auto;
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
    /// Calls that enter the kernel: one per datagram (Message), per sendmmsg/recvmmsg (MultiMessage) or
    /// per request-queue commit (Registered; polling a completion queue does not enter the kernel).
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

    /// Creates, configures (non-blocking, buffers, dual-stack, batch API) and binds a socket.
    static Result<UdpSocket> open(const UdpSocketConfig& config);

    /// False when this process cannot create IPv6 sockets (IPv6 disabled in the kernel/container).
    static bool isIpv6Supported() noexcept;

    bool isOpen() const noexcept { return m_handle != kInvalidHandle; }
    void close() noexcept;

    /// Sends one datagram. ErrorCode::Busy when the socket buffer is full (with Registered I/O: when
    /// every send slot is in flight); the datagram is dropped, as UDP would. InvalidArgument for a
    /// destination of the wrong family or, with Registered I/O, a datagram of 2,048 bytes or more;
    /// IoError otherwise.
    Result<void> sendTo(const Address& to, std::span<const u8> data) noexcept;
    /// Sends as many datagrams as the socket accepts; returns how many were handed to the OS.
    /// Datagrams refused with a full buffer are counted in stats().sendWouldBlock and skipped, others
    /// that fail in stats().sendErrors.
    usize sendBatch(std::span<const OutDatagram> datagrams) noexcept;

    /// Receives one datagram; returns its size, 0 when nothing is pending. Truncated datagrams and
    /// ICMP-induced errors (connection reset/refused) are skipped.
    usize receiveFrom(Address& from, std::span<u8> buffer) noexcept;
    /// Fills up to slots.size() slots; returns how many were filled.
    usize receiveBatch(std::span<InDatagram> slots) noexcept;

    /// The bound address (with the actual port when bound to port 0).
    const Address& localAddress() const noexcept { return m_local; }
    /// Buffer sizes the OS actually granted (may be below the request, see the header comment). A
    /// Registered I/O socket does not use SO_SNDBUF/SO_RCVBUF, so it reports its slot capacity instead
    /// (slots × 2,048 bytes).
    u32 sendBufferBytes() const noexcept { return m_sendBuffer; }
    u32 receiveBufferBytes() const noexcept { return m_receiveBuffer; }
    AddressFamily family() const noexcept { return m_local.family(); }
    const UdpSocketStats& stats() const noexcept { return m_stats; }
    /// The batch API in use (never Auto; see UdpSocketConfig::batchApi).
    UdpBatchApi batchApi() const noexcept { return m_batchApi; }

private:
    static constexpr std::intptr_t kInvalidHandle = -1;
    /// Per-socket OS state beyond the handle (Windows: the WSARecvMsg entry point and the Registered I/O
    /// queues and buffers). Defined by the platform source that needs it; always null elsewhere.
    struct PlatformState;

    /// Fills slots with one receiveFrom() each (the Message API).
    usize receiveEach(std::span<InDatagram> slots) noexcept;

    std::intptr_t m_handle = kInvalidHandle;
    PlatformState* m_platform = nullptr;
    Address m_local;
    u32 m_sendBuffer = 0;
    u32 m_receiveBuffer = 0;
    UdpBatchApi m_batchApi = UdpBatchApi::Message;
    UdpSocketStats m_stats;
};

} // namespace helios::net
