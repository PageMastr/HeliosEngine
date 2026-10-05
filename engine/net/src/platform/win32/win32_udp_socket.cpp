// Win32 (MSVC, clang-cl, MinGW-w64) implementation of UdpSocket over Winsock2 (04 §2.6).
//
// Batches use Registered I/O (RIO, Windows 8 and later; UdpBatchApi::Registered) when the provider
// offers it. Each socket registers one buffer region of 2 KB slots (128 for receives, 256 for sends,
// each with room for its peer address), keeps every receive slot posted, and polls its two completion
// queues from the owner thread (no event, no completion port). Requests are queued with RIO_MSG_DEFER
// and committed once per batch, so a batch of sends, or of re-posted receives, costs one kernel entry
// and reading completions costs none. A receive copies the datagram out of its slot into the caller's
// buffer (recvfrom makes the same copy in the kernel); a send copies it into a slot.
//
// Where RIO is unavailable, or UdpSocketConfig::batchApi asks for UdpBatchApi::Message (the fallback
// tests force), every datagram takes one WSASendMsg/WSARecvMsg call on the non-blocking socket (recvfrom
// if the provider has no WSARecvMsg). 04 §2.6 names IOCP with WSARecvMsg as the fallback of the trunk IO
// threads, which block between bursts (Phase 2); a socket that its owner polls, as here, needs no
// completion port.
//
// Both APIs keep SIO_UDP_CONNRESET off, the requested buffer sizes, IPv6 dual-stack and the semantics of
// the POSIX sendmmsg/recvmmsg path (partial batches, full buffers counted and skipped, truncated
// datagrams dropped). The RIO declarations are written out from the documented ABI instead of taken
// from <mswsock.h>, because MinGW-w64 11 declares none; where the Windows SDK declares them, the
// static_asserts below check this copy against it.

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
#include <mswsock.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "helios/net/udp_socket.h"
#include "net_internal.h"
#include "platform/net_os.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace helios::net {
namespace {

// ---------------------------------------------------------------------------------------------
// Registered I/O ABI (mswsock.h / mswsockdef.h, Windows 8 and later)
// ---------------------------------------------------------------------------------------------
namespace rio {

struct BufferIdTag;
struct CqTag;
struct RqTag;
using BufferId = BufferIdTag*;
using Cq = CqTag*;
using Rq = RqTag*;

/// RIO_BUF: a slice of a registered buffer.
struct Buf {
    BufferId bufferId;
    ULONG offset;
    ULONG length;
};

/// RIORESULT: one completion.
struct Completion {
    LONG status;
    ULONG bytesTransferred;
    ULONGLONG socketContext;
    ULONGLONG requestContext;
};

using ReceiveFn = BOOL(WINAPI*)(Rq, Buf*, ULONG, DWORD, PVOID);
using ReceiveExFn = int(WINAPI*)(Rq, Buf*, ULONG, Buf*, Buf*, Buf*, Buf*, DWORD, PVOID);
using SendFn = BOOL(WINAPI*)(Rq, Buf*, ULONG, DWORD, PVOID);
using SendExFn = BOOL(WINAPI*)(Rq, Buf*, ULONG, Buf*, Buf*, Buf*, Buf*, DWORD, PVOID);
using CloseCompletionQueueFn = void(WINAPI*)(Cq);
// The second argument is a RIO_NOTIFICATION_COMPLETION*; null makes a polled queue.
using CreateCompletionQueueFn = Cq(WINAPI*)(DWORD, void*);
using CreateRequestQueueFn = Rq(WINAPI*)(SOCKET, ULONG, ULONG, ULONG, ULONG, Cq, Cq, PVOID);
using DequeueCompletionFn = ULONG(WINAPI*)(Cq, Completion*, ULONG);
using DeregisterBufferFn = void(WINAPI*)(BufferId);
using NotifyFn = int(WINAPI*)(Cq);
using RegisterBufferFn = BufferId(WINAPI*)(PCHAR, DWORD);
using ResizeCompletionQueueFn = BOOL(WINAPI*)(Cq, DWORD);
using ResizeRequestQueueFn = BOOL(WINAPI*)(Rq, ULONG, ULONG);

/// RIO_EXTENSION_FUNCTION_TABLE, as SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER fills it.
struct FunctionTable {
    DWORD cbSize;
    ReceiveFn receive;
    ReceiveExFn receiveEx;
    SendFn send;
    SendExFn sendEx;
    CloseCompletionQueueFn closeCompletionQueue;
    CreateCompletionQueueFn createCompletionQueue;
    CreateRequestQueueFn createRequestQueue;
    DequeueCompletionFn dequeueCompletion;
    DeregisterBufferFn deregisterBuffer;
    NotifyFn notify;
    RegisterBufferFn registerBuffer;
    ResizeCompletionQueueFn resizeCompletionQueue;
    ResizeRequestQueueFn resizeRequestQueue;
};

constexpr DWORD kMsgDefer = 0x00000002;                  // RIO_MSG_DEFER
constexpr DWORD kMsgCommitOnly = 0x00000008;             // RIO_MSG_COMMIT_ONLY
constexpr ULONG kCorruptCq = 0xFFFFFFFFu;                // RIO_CORRUPT_CQ
constexpr ULONG_PTR kInvalidBufferIdValue = 0xFFFFFFFFu; // RIO_INVALID_BUFFERID
constexpr DWORD kGetMultipleExtensionFunctionPointer = _WSAIORW(IOC_WS2, 36);
/// WSAID_MULTIPLE_RIO {8509e081-96dd-4005-b165-9e2ee8c79e3f}.
constexpr GUID kMultipleRio = {0x8509e081, 0x96dd, 0x4005, {0xb1, 0x65, 0x9e, 0x2e, 0xe8, 0xc7, 0x9e, 0x3f}};

constexpr bool sameGuid(const GUID& a, const GUID& b) noexcept {
    if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3) return false;
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) return false;
    }
    return true;
}

inline BufferId invalidBufferId() noexcept { return reinterpret_cast<BufferId>(kInvalidBufferIdValue); }

} // namespace rio

#if defined(RIO_MSG_DEFER) && defined(RIO_MSG_COMMIT_ONLY) && defined(RIO_CORRUPT_CQ) && defined(WSAID_MULTIPLE_RIO)
// The Windows SDK declares RIO: this copy of the ABI must match it field for field.
static_assert(sizeof(rio::FunctionTable) == sizeof(RIO_EXTENSION_FUNCTION_TABLE));
static_assert(offsetof(rio::FunctionTable, receiveEx) == offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOReceiveEx));
static_assert(offsetof(rio::FunctionTable, sendEx) == offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOSendEx));
static_assert(offsetof(rio::FunctionTable, closeCompletionQueue) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOCloseCompletionQueue));
static_assert(offsetof(rio::FunctionTable, createCompletionQueue) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOCreateCompletionQueue));
static_assert(offsetof(rio::FunctionTable, createRequestQueue) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOCreateRequestQueue));
static_assert(offsetof(rio::FunctionTable, dequeueCompletion) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIODequeueCompletion));
static_assert(offsetof(rio::FunctionTable, deregisterBuffer) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIODeregisterBuffer));
static_assert(offsetof(rio::FunctionTable, registerBuffer) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIORegisterBuffer));
static_assert(offsetof(rio::FunctionTable, resizeRequestQueue) ==
              offsetof(RIO_EXTENSION_FUNCTION_TABLE, RIOResizeRequestQueue));
static_assert(sizeof(rio::Buf) == sizeof(RIO_BUF) && offsetof(rio::Buf, offset) == offsetof(RIO_BUF, Offset) &&
              offsetof(rio::Buf, length) == offsetof(RIO_BUF, Length));
static_assert(sizeof(rio::Completion) == sizeof(RIORESULT) &&
              offsetof(rio::Completion, bytesTransferred) == offsetof(RIORESULT, BytesTransferred) &&
              offsetof(rio::Completion, requestContext) == offsetof(RIORESULT, RequestContext));
static_assert(rio::kMsgDefer == RIO_MSG_DEFER && rio::kMsgCommitOnly == RIO_MSG_COMMIT_ONLY);
static_assert(rio::kCorruptCq == RIO_CORRUPT_CQ);
static_assert(rio::sameGuid(rio::kMultipleRio, GUID WSAID_MULTIPLE_RIO));
#endif
#if defined(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER)
static_assert(rio::kGetMultipleExtensionFunctionPointer == SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER);
#endif

// Registered I/O slots: one datagram (kSlotBytes) plus its peer address (a SOCKADDR_INET) each. Receive
// slots come first and send slots after them, in one registered region per socket (≈ 0.8 MB).
constexpr ULONG kSlotBytes = 2048;
constexpr ULONG kAddressBytes = 64; // >= sizeof(SOCKADDR_INET); keeps every slot 64-byte aligned
constexpr ULONG kSlotStride = kSlotBytes + kAddressBytes;
constexpr u32 kReceiveSlots = 128;
constexpr u32 kSendSlots = 256;
constexpr ULONG kRegionBytes = kSlotStride * (kReceiveSlots + kSendSlots);
/// Completions read per RIODequeueCompletion call.
constexpr ULONG kDequeueBatch = 64;
static_assert(sizeof(SOCKADDR_INET) <= kAddressBytes);

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

/// Loads the RIO function table for `s` (a socket created with WSA_FLAG_REGISTERED_IO).
bool loadRioTable(SOCKET s, rio::FunctionTable& table) noexcept {
    GUID id = rio::kMultipleRio;
    DWORD bytes = 0;
    std::memset(&table, 0, sizeof(table));
    if (WSAIoctl(s, rio::kGetMultipleExtensionFunctionPointer, &id, sizeof(id), &table, sizeof(table), &bytes, nullptr,
                 nullptr) != 0) {
        return false;
    }
    return table.receiveEx && table.sendEx && table.closeCompletionQueue && table.createCompletionQueue &&
           table.createRequestQueue && table.dequeueCompletion && table.deregisterBuffer && table.registerBuffer;
}

/// Whether this process's UDP provider offers Registered I/O (probed once; thread-safe).
bool registeredIoAvailable() noexcept {
    static const bool available = [] {
        const SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                    WSA_FLAG_REGISTERED_IO | WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET) return false;
        rio::FunctionTable table;
        const bool ok = loadRioTable(s, table);
        closesocket(s);
        return ok;
    }();
    return available;
}

/// Outcome of handing one datagram to Registered I/O.
enum class SendOutcome { Posted, TooLarge, Full, Failed };

} // namespace

// ---------------------------------------------------------------------------------------------
// Per-socket state
// ---------------------------------------------------------------------------------------------

struct UdpSocket::PlatformState {
    /// WSARecvMsg (Message API); null: recvfrom.
    LPFN_WSARECVMSG recvMsg = nullptr;

    // Registered I/O (UdpBatchApi::Registered only).
    rio::FunctionTable api{};
    u8* region = nullptr; // kRegionBytes from VirtualAlloc: receive slots, then send slots
    rio::BufferId bufferId = nullptr;
    rio::Cq receiveCq = nullptr;
    rio::Cq sendCq = nullptr;
    rio::Rq requestQueue = nullptr;
    std::array<u16, kSendSlots> freeSend{}; // send slots not in flight (indices 0..kSendSlots-1)
    u32 freeSendCount = 0;
    u32 deferredSends = 0;                  // posted with RIO_MSG_DEFER and not committed yet
    std::array<u16, kReceiveSlots> unposted{}; // receive slots whose re-post failed (retried later)
    u32 unpostedCount = 0;

    u8* slotData(u32 slot) noexcept { return region + static_cast<usize>(slot) * kSlotStride; }
    u8* slotAddress(u32 slot) noexcept { return slotData(slot) + kSlotBytes; }
    rio::Buf dataBuf(u32 slot, ULONG length) const noexcept {
        return rio::Buf{bufferId, static_cast<ULONG>(slot * kSlotStride), length};
    }
    rio::Buf addressBuf(u32 slot) const noexcept {
        return rio::Buf{bufferId, static_cast<ULONG>(slot * kSlotStride + kSlotBytes),
                        static_cast<ULONG>(sizeof(SOCKADDR_INET))};
    }

    /// Queues receive slot `slot` (0..kReceiveSlots-1) with RIO_MSG_DEFER; commitReceives() submits it.
    bool postReceive(u32 slot) noexcept {
        rio::Buf data = dataBuf(slot, kSlotBytes);
        rio::Buf address = addressBuf(slot);
        return api.receiveEx(requestQueue, &data, 1, nullptr, &address, nullptr, nullptr, rio::kMsgDefer,
                             reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(slot))) != FALSE;
    }
    bool commitReceives() noexcept {
        return api.receiveEx(requestQueue, nullptr, 0, nullptr, nullptr, nullptr, nullptr, rio::kMsgCommitOnly,
                             nullptr) != FALSE;
    }
    bool commitSends() noexcept {
        return api.sendEx(requestQueue, nullptr, 0, nullptr, nullptr, nullptr, nullptr, rio::kMsgCommitOnly, nullptr) !=
               FALSE;
    }

    /// Returns finished send slots to the free list; a failed completion counts as a send error.
    void reapSends(UdpSocketStats& stats) noexcept {
        std::array<rio::Completion, kDequeueBatch> done;
        for (;;) {
            const ULONG n = api.dequeueCompletion(sendCq, done.data(), kDequeueBatch);
            if (n == rio::kCorruptCq) {
                ++stats.sendErrors;
                return;
            }
            for (ULONG k = 0; k < n; ++k) {
                const auto index = static_cast<u32>(done[k].requestContext);
                if (done[k].status != 0) ++stats.sendErrors;
                if (index < kSendSlots && freeSendCount < kSendSlots) freeSend[freeSendCount++] = static_cast<u16>(index);
            }
            if (n < kDequeueBatch) return;
        }
    }

    /// Releases every Registered I/O resource. The request queue goes with the socket, so the caller
    /// closes the socket first (which also cancels the posted receives).
    void releaseRegistered() noexcept {
        if (receiveCq) api.closeCompletionQueue(receiveCq);
        if (sendCq) api.closeCompletionQueue(sendCq);
        if (bufferId && bufferId != rio::invalidBufferId()) api.deregisterBuffer(bufferId);
        if (region) VirtualFree(region, 0, MEM_RELEASE);
        receiveCq = sendCq = nullptr;
        requestQueue = nullptr;
        bufferId = nullptr;
        region = nullptr;
        freeSendCount = unpostedCount = deferredSends = 0;
    }

    /// Sets up Registered I/O on a bound socket created with WSA_FLAG_REGISTERED_IO and posts every
    /// receive slot. False leaves nothing allocated, and the socket uses the Message API. Once the
    /// request queue exists the socket stays on Registered I/O: it cannot be detached from the queue,
    /// so a receive slot that cannot be posted now is retried by the next receive.
    bool setUpRegistered(SOCKET s) noexcept {
        if (!loadRioTable(s, api)) return false;
        region = static_cast<u8*>(VirtualAlloc(nullptr, kRegionBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (region) bufferId = api.registerBuffer(reinterpret_cast<PCHAR>(region), kRegionBytes);
        if (bufferId && bufferId != rio::invalidBufferId()) receiveCq = api.createCompletionQueue(kReceiveSlots, nullptr);
        if (receiveCq) sendCq = api.createCompletionQueue(kSendSlots, nullptr);
        if (sendCq) requestQueue = api.createRequestQueue(s, kReceiveSlots, 1, kSendSlots, 1, receiveCq, sendCq, nullptr);
        if (!requestQueue) {
            releaseRegistered();
            return false;
        }
        for (u32 i = 0; i < kSendSlots; ++i) freeSend[i] = static_cast<u16>(kSendSlots - 1 - i);
        freeSendCount = kSendSlots;
        for (u32 i = 0; i < kReceiveSlots; ++i) {
            if (!postReceive(i)) unposted[unpostedCount++] = static_cast<u16>(i);
        }
        (void)commitReceives();
        return true;
    }

    /// Re-posts receive slots whose earlier post failed. Returns how many it posted.
    u32 repostUnposted() noexcept {
        u32 posted = 0;
        u32 kept = 0;
        for (u32 i = 0; i < unpostedCount; ++i) {
            if (postReceive(unposted[i])) ++posted;
            else unposted[kept++] = unposted[i];
        }
        unpostedCount = kept;
        if (posted > 0) (void)commitReceives();
        return posted;
    }

    /// Copies one datagram into a free send slot and queues it with RIO_MSG_DEFER; flushSends() submits it.
    SendOutcome postSend(const sockaddr_storage& to, int toLength, std::span<const u8> data,
                         UdpSocketStats& stats) noexcept {
        if (data.size() >= kSlotBytes) return SendOutcome::TooLarge;
        if (freeSendCount == 0) {
            // Submit what this batch queued so far, then collect what has finished since.
            flushSends(stats);
            reapSends(stats);
            if (freeSendCount == 0) return SendOutcome::Full;
        }
        const u32 index = freeSend[--freeSendCount];
        const u32 slot = kReceiveSlots + index;
        if (!data.empty()) std::memcpy(slotData(slot), data.data(), data.size());
        std::memset(slotAddress(slot), 0, sizeof(SOCKADDR_INET));
        std::memcpy(slotAddress(slot), &to, std::min<usize>(static_cast<usize>(toLength), sizeof(SOCKADDR_INET)));
        rio::Buf payload = dataBuf(slot, static_cast<ULONG>(data.size()));
        rio::Buf address = addressBuf(slot);
        if (!api.sendEx(requestQueue, &payload, 1, nullptr, &address, nullptr, nullptr, rio::kMsgDefer,
                        reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(index)))) {
            freeSend[freeSendCount++] = static_cast<u16>(index);
            return isWouldBlock(WSAGetLastError()) ? SendOutcome::Full : SendOutcome::Failed;
        }
        ++deferredSends;
        return SendOutcome::Posted;
    }

    /// Submits the sends postSend() queued (one kernel entry).
    void flushSends(UdpSocketStats& stats) noexcept {
        if (deferredSends == 0) return;
        ++stats.sendSyscalls;
        if (commitSends()) deferredSends = 0;
        else ++stats.sendErrors; // still queued: the next commit submits them
    }

    /// Before the socket closes: submits queued sends and waits up to ~20 ms for every send in flight to
    /// complete, so that closing does not cancel datagrams handed to the OS just before (an endpoint's
    /// disconnect packets), which sendto would have delivered.
    void drainSends(UdpSocketStats& stats) noexcept {
        flushSends(stats);
        const ULONGLONG end = GetTickCount64() + 20;
        for (;;) {
            reapSends(stats);
            if (freeSendCount >= kSendSlots || deferredSends > 0 || GetTickCount64() > end) return;
            (void)SwitchToThread();
        }
    }
};

// ---------------------------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------------------------

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

    // One attempt at a socket. A Registered I/O socket (WSA_FLAG_REGISTERED_IO) refuses FIONBIO
    // (WSAEOPNOTSUPP): its requests never block, but a plain call on it would, so it is used only once its
    // queues exist. When they cannot be set up, `rioFailed` is set and open() starts over with a plain
    // non-blocking socket for the Message API.
    bool rioFailed = false;
    const auto attempt = [&](bool registered) -> Result<UdpSocket> {
        const SOCKET s = WSASocketW(family == AddressFamily::IPv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                    nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT | (registered ? WSA_FLAG_REGISTERED_IO : 0));
        if (s == INVALID_SOCKET) {
            const int e = WSAGetLastError();
            return makeError(e == WSAEAFNOSUPPORT ? ErrorCode::Unsupported : ErrorCode::IoError,
                             "UdpSocket: socket() failed ({})", e);
        }
        UdpSocket sock;
        sock.m_handle = static_cast<std::intptr_t>(s);
        sock.m_platform = new (std::nothrow) PlatformState();
        if (!sock.m_platform) return Error{ErrorCode::OutOfMemory, "UdpSocket: out of memory"};

        (void)SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
        if (family == AddressFamily::IPv6) {
            const DWORD v6only = config.dualStack ? 0 : 1;
            if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) !=
                0) {
                return makeError(ErrorCode::IoError, "UdpSocket: IPV6_V6ONLY failed ({})", WSAGetLastError());
            }
        }
        if (config.reuseAddress) {
            const BOOL one = TRUE;
            (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        }
        // An ICMP port-unreachable for one peer must not make receives fail for all others.
        {
            BOOL newBehavior = FALSE;
            DWORD bytesReturned = 0;
            (void)WSAIoctl(s, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytesReturned,
                           nullptr, nullptr);
        }
        sock.m_sendBuffer = setBufferSize(s, SO_SNDBUF, config.sendBufferBytes);
        sock.m_receiveBuffer = setBufferSize(s, SO_RCVBUF, config.receiveBufferBytes);

        if (!registered) {
            u_long nonBlocking = 1;
            if (ioctlsocket(s, FIONBIO, &nonBlocking) != 0) {
                return makeError(ErrorCode::IoError, "UdpSocket: FIONBIO failed ({})", WSAGetLastError());
            }
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

        if (registered) {
            if (!sock.m_platform->setUpRegistered(s)) {
                rioFailed = true;
                return makeError(ErrorCode::Unsupported, "UdpSocket {}: Registered I/O set-up failed ({})",
                                 sock.m_local, WSAGetLastError());
            }
            sock.m_batchApi = UdpBatchApi::Registered;
            return sock;
        }
        sock.m_batchApi = UdpBatchApi::Message;
        GUID id = WSAID_WSARECVMSG;
        DWORD bytes = 0;
        LPFN_WSARECVMSG fn = nullptr;
        if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &fn, sizeof(fn), &bytes, nullptr,
                     nullptr) == 0) {
            sock.m_platform->recvMsg = fn;
        }
        return sock;
    };

    if ((config.batchApi == UdpBatchApi::Auto || config.batchApi == UdpBatchApi::Registered) &&
        registeredIoAvailable()) {
        Result<UdpSocket> r = attempt(true);
        if (r || !rioFailed) return r;
        HELIOS_LOG_DEBUG(LogNet, "{}; using WSASendMsg/WSARecvMsg", r.error());
    }
    return attempt(false);
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_handle(std::exchange(other.m_handle, kInvalidHandle)), m_platform(std::exchange(other.m_platform, nullptr)),
      m_local(other.m_local), m_sendBuffer(other.m_sendBuffer), m_receiveBuffer(other.m_receiveBuffer),
      m_batchApi(other.m_batchApi), m_stats(other.m_stats) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, kInvalidHandle);
        m_platform = std::exchange(other.m_platform, nullptr);
        m_local = other.m_local;
        m_sendBuffer = other.m_sendBuffer;
        m_receiveBuffer = other.m_receiveBuffer;
        m_batchApi = other.m_batchApi;
        m_stats = other.m_stats;
    }
    return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() noexcept {
    if (m_handle != kInvalidHandle) {
        if (m_platform && m_batchApi == UdpBatchApi::Registered) m_platform->drainSends(m_stats);
        closesocket(toSocket(m_handle));
        m_handle = kInvalidHandle;
    }
    if (m_platform) {
        m_platform->releaseRegistered();
        delete m_platform;
        m_platform = nullptr;
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
    if (m_batchApi == UdpBatchApi::Registered) {
        PlatformState& p = *m_platform;
        p.reapSends(m_stats);
        const SendOutcome outcome = p.postSend(ss, len, data, m_stats);
        const int e = outcome == SendOutcome::Failed ? WSAGetLastError() : 0;
        p.flushSends(m_stats);
        switch (outcome) {
        case SendOutcome::Posted:
            ++m_stats.datagramsSent;
            m_stats.bytesSent += data.size();
            return {};
        case SendOutcome::Full: ++m_stats.sendWouldBlock; return Error{ErrorCode::Busy, "send slots full"};
        case SendOutcome::TooLarge:
            ++m_stats.sendErrors;
            return Error{ErrorCode::InvalidArgument, "datagram does not fit a registered send slot"};
        case SendOutcome::Failed: break;
        }
        ++m_stats.sendErrors;
        return makeError(ErrorCode::IoError, "RIOSendEx failed ({})", e);
    }
    ++m_stats.sendSyscalls;
    WSABUF buf;
    buf.len = static_cast<ULONG>(data.size());
    buf.buf = reinterpret_cast<CHAR*>(const_cast<u8*>(data.data()));
    WSAMSG msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.name = reinterpret_cast<LPSOCKADDR>(&ss);
    msg.namelen = len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    DWORD sentBytes = 0;
    if (WSASendMsg(toSocket(m_handle), &msg, 0, &sentBytes, nullptr, nullptr) != SOCKET_ERROR) {
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
    return makeError(ErrorCode::IoError, "WSASendMsg failed ({})", e);
}

usize UdpSocket::sendBatch(std::span<const OutDatagram> datagrams) noexcept {
    if (!isOpen()) return 0;
    usize sent = 0;
    if (m_batchApi != UdpBatchApi::Registered) {
        for (const OutDatagram& d : datagrams) {
            if (sendTo(d.to, d.data)) ++sent;
        }
        return sent;
    }
    PlatformState& p = *m_platform;
    p.reapSends(m_stats);
    for (const OutDatagram& d : datagrams) {
        sockaddr_storage ss{};
        const int len = toSockaddr(d.to, m_local.family(), ss);
        if (len == 0) {
            ++m_stats.sendErrors;
            continue;
        }
        switch (p.postSend(ss, len, d.data, m_stats)) {
        case SendOutcome::Posted:
            ++sent;
            ++m_stats.datagramsSent;
            m_stats.bytesSent += d.data.size();
            break;
        case SendOutcome::Full: ++m_stats.sendWouldBlock; break;
        case SendOutcome::TooLarge:
        case SendOutcome::Failed: ++m_stats.sendErrors; break;
        }
    }
    p.flushSends(m_stats);
    return sent;
}

usize UdpSocket::receiveFrom(Address& from, std::span<u8> buffer) noexcept {
    if (!isOpen()) return 0;
    if (m_batchApi == UdpBatchApi::Registered) {
        InDatagram slot;
        slot.buffer = buffer;
        if (receiveBatch(std::span<InDatagram>(&slot, 1)) == 0) return 0;
        from = slot.from;
        return slot.size;
    }
    const LPFN_WSARECVMSG recvMsg = m_platform ? m_platform->recvMsg : nullptr;
    for (;;) {
        sockaddr_storage ss{};
        int r = 0;
        bool truncated = false;
        ++m_stats.receiveSyscalls;
        if (recvMsg) {
            WSABUF buf;
            buf.len = static_cast<ULONG>(std::min<usize>(buffer.size(), 0x7FFFFFFF));
            buf.buf = reinterpret_cast<CHAR*>(buffer.data());
            WSAMSG msg;
            std::memset(&msg, 0, sizeof(msg));
            msg.name = reinterpret_cast<LPSOCKADDR>(&ss);
            msg.namelen = static_cast<INT>(sizeof(ss));
            msg.lpBuffers = &buf;
            msg.dwBufferCount = 1;
            DWORD received = 0;
            if (recvMsg(toSocket(m_handle), &msg, &received, nullptr, nullptr) == SOCKET_ERROR) {
                r = SOCKET_ERROR;
            } else {
                r = static_cast<int>(received);
                truncated = (msg.dwFlags & MSG_TRUNC) != 0;
            }
        } else {
            int ssLen = sizeof(ss);
            r = ::recvfrom(toSocket(m_handle), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                           reinterpret_cast<sockaddr*>(&ss), &ssLen);
        }
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
        if (truncated) {
            ++m_stats.truncated;
            continue;
        }
        if (r == 0) continue;
        from = fromSockaddr(ss, /*unmap=*/true);
        ++m_stats.datagramsReceived;
        m_stats.bytesReceived += static_cast<u64>(r);
        return static_cast<usize>(r);
    }
}

usize UdpSocket::receiveBatch(std::span<InDatagram> slots) noexcept {
    if (!isOpen() || slots.empty()) return 0;
    if (m_batchApi != UdpBatchApi::Registered) return receiveEach(slots);
    PlatformState& p = *m_platform;
    if (p.unpostedCount > 0 && p.repostUnposted() > 0) ++m_stats.receiveSyscalls;
    usize filled = 0;
    std::array<rio::Completion, kDequeueBatch> done;
    while (filled < slots.size()) {
        const auto want = static_cast<ULONG>(std::min<usize>(kDequeueBatch, slots.size() - filled));
        const ULONG n = p.api.dequeueCompletion(p.receiveCq, done.data(), want);
        if (n == rio::kCorruptCq) {
            ++m_stats.receiveErrors;
            break;
        }
        if (n == 0) break;
        u32 reposted = 0;
        for (ULONG k = 0; k < n; ++k) {
            const rio::Completion& c = done[k];
            const auto slot = static_cast<u32>(c.requestContext);
            if (slot >= kReceiveSlots) {
                ++m_stats.receiveErrors;
                continue;
            }
            if (c.status != 0) {
                if (c.status == WSAEMSGSIZE) ++m_stats.truncated;
                else if (!isTransientReceiveError(static_cast<int>(c.status))) ++m_stats.receiveErrors;
            } else if (c.bytesTransferred >= kSlotBytes) {
                ++m_stats.truncated; // filled the whole slot, so it may have been cut
            } else if (c.bytesTransferred > 0) {
                InDatagram& out = slots[filled];
                if (c.bytesTransferred > out.buffer.size()) {
                    ++m_stats.truncated;
                } else {
                    std::memcpy(out.buffer.data(), p.slotData(slot), c.bytesTransferred);
                    out.size = c.bytesTransferred;
                    sockaddr_storage ss{};
                    std::memcpy(&ss, p.slotAddress(slot), sizeof(SOCKADDR_INET));
                    out.from = fromSockaddr(ss, /*unmap=*/true);
                    ++m_stats.datagramsReceived;
                    m_stats.bytesReceived += c.bytesTransferred;
                    ++filled;
                }
            }
            if (p.postReceive(slot)) ++reposted;
            else p.unposted[p.unpostedCount++] = static_cast<u16>(slot);
        }
        if (reposted > 0) {
            ++m_stats.receiveSyscalls;
            if (!p.commitReceives()) ++m_stats.receiveErrors;
        }
        if (n < want) break; // the completion queue is drained
    }
    return filled;
}

usize UdpSocket::receiveEach(std::span<InDatagram> slots) noexcept {
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
