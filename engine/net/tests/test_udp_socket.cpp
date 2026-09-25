// L0 UdpSocket and SocketTransport on real loopback sockets, including the NS-0.2 loopback
// packet-rate measurement.

#include <doctest/doctest.h>

#include <vector>

#include "helios/core/time.h"
#include "helios/net/transport.h"
#include "helios/net/udp_socket.h"
#include "platform/net_os.h"

using namespace helios;
using namespace helios::net;

namespace {

UdpSocket openLoopback(u32 bufferBytes = 8u * 1024 * 1024) {
    UdpSocketConfig c;
    c.bindAddress = Address::loopbackV4(0);
    c.sendBufferBytes = bufferBytes;
    c.receiveBufferBytes = bufferBytes;
    auto s = UdpSocket::open(c);
    REQUIRE(s);
    return std::move(s).value();
}

/// Receives until `count` datagrams arrived or `timeout` seconds passed.
usize drain(UdpSocket& s, usize count, f64 timeout = 2.0) {
    std::vector<u8> buf(2048);
    usize got = 0;
    const f64 end = monotonicSeconds() + timeout;
    Address from;
    while (got < count && monotonicSeconds() < end) {
        if (s.receiveFrom(from, buf) > 0) ++got;
        else sleepMillis(1);
    }
    return got;
}

} // namespace

TEST_SUITE("net.udp") {
    TEST_CASE("bind, send and receive on IPv4 loopback") {
        UdpSocket a = openLoopback();
        UdpSocket b = openLoopback();
        CHECK(a.isOpen());
        CHECK(a.localAddress().isIpv4());
        CHECK(a.localAddress().port() != 0);
        CHECK(a.localAddress() != b.localAddress());
        CHECK(a.receiveBufferBytes() > 0);
        CHECK(a.sendBufferBytes() > 0);
        const u8 msg[] = {1, 2, 3, 4, 5};
        REQUIRE(a.sendTo(b.localAddress(), msg));
        std::vector<u8> buf(64);
        Address from;
        usize n = 0;
        for (int i = 0; i < 1000 && n == 0; ++i) {
            n = b.receiveFrom(from, buf);
            if (n == 0) sleepMillis(1);
        }
        REQUIRE(n == 5);
        CHECK(from == a.localAddress());
        CHECK(buf[4] == 5);
        CHECK(b.receiveFrom(from, buf) == 0); // non-blocking: nothing pending
        CHECK(a.stats().datagramsSent == 1);
        CHECK(b.stats().datagramsReceived == 1);
    }

    TEST_CASE("batched send/receive and truncation") {
        UdpSocket a = openLoopback();
        UdpSocket b = openLoopback();
        std::vector<std::vector<u8>> payloads(100);
        std::vector<OutDatagram> out;
        for (usize i = 0; i < payloads.size(); ++i) {
            payloads[i].assign(10 + i, static_cast<u8>(i));
            out.push_back(OutDatagram{b.localAddress(), payloads[i]});
        }
        CHECK(a.sendBatch(out) == 100);
        std::vector<u8> arena(64 * 2048);
        std::vector<InDatagram> slots(64);
        usize total = 0;
        std::vector<bool> seen(100, false);
        const f64 end = monotonicSeconds() + 2.0;
        while (total < 100 && monotonicSeconds() < end) {
            for (usize i = 0; i < slots.size(); ++i) slots[i].buffer = std::span<u8>(arena.data() + i * 2048, 2048);
            const usize n = b.receiveBatch(slots);
            for (usize i = 0; i < n; ++i) {
                const usize idx = slots[i].size - 10;
                REQUIRE(idx < 100);
                CHECK(slots[i].buffer[0] == static_cast<u8>(idx));
                CHECK(slots[i].from == a.localAddress());
                seen[idx] = true;
            }
            total += n;
            if (n == 0) sleepMillis(1);
        }
        CHECK(total == 100);
        for (bool s : seen) CHECK(s);
        // A datagram larger than the receive buffer is dropped and counted, not returned partially.
        std::vector<u8> big(1500, 0xAB);
        REQUIRE(a.sendTo(b.localAddress(), big));
        const u8 small[] = {7};
        REQUIRE(a.sendTo(b.localAddress(), small));
        std::vector<u8> tiny(100);
        Address from;
        usize n = 0;
        for (int i = 0; i < 1000 && n == 0; ++i) {
            n = b.receiveFrom(from, tiny);
            if (n == 0) sleepMillis(1);
        }
        CHECK(n == 1);
        CHECK(tiny[0] == 7);
        CHECK(b.stats().truncated == 1);
    }

    TEST_CASE("IPv6 and dual-stack sockets (when the OS supports IPv6)") {
        if (!UdpSocket::isIpv6Supported()) {
            MESSAGE("IPv6 sockets unavailable in this environment");
            UdpSocketConfig c;
            c.bindAddress = Address::loopbackV6(0);
            CHECK(UdpSocket::open(c).errorCode() == ErrorCode::Unsupported);
            return;
        }
        UdpSocketConfig c6;
        c6.bindAddress = Address::anyV6(0);
        c6.dualStack = true;
        UdpSocket dual = UdpSocket::open(c6).value();
        CHECK(dual.localAddress().isIpv6());
        UdpSocket v4 = openLoopback();
        const u8 msg[] = {42};
        REQUIRE(v4.sendTo(Address::loopbackV4(dual.localAddress().port()), msg));
        std::vector<u8> buf(16);
        Address from;
        usize n = 0;
        for (int i = 0; i < 1000 && n == 0; ++i) {
            n = dual.receiveFrom(from, buf);
            if (n == 0) sleepMillis(1);
        }
        REQUIRE(n == 1);
        CHECK(from == v4.localAddress()); // reported as plain IPv4, not ::ffff:127.0.0.1
        REQUIRE(dual.sendTo(v4.localAddress(), msg)); // IPv4 destination through the IPv6 socket
        CHECK(drain(v4, 1) == 1);
    }

    TEST_CASE("socket buffers honour large requests (trunk profile: 32 MB)") {
        UdpSocketConfig c;
        c.bindAddress = Address::loopbackV4(0);
        c.receiveBufferBytes = 32u * 1024 * 1024;
        c.sendBufferBytes = 32u * 1024 * 1024;
        UdpSocket s = UdpSocket::open(c).value();
        MESSAGE("requested 32 MB, OS granted rcv " << s.receiveBufferBytes() / 1024 << " KB, snd "
                                                   << s.sendBufferBytes() / 1024 << " KB");
        CHECK(s.receiveBufferBytes() >= 256u * 1024);
    }

    TEST_CASE("bind conflicts and invalid addresses are reported") {
        UdpSocket a = openLoopback();
        UdpSocketConfig c;
        c.bindAddress = a.localAddress();
        CHECK(UdpSocket::open(c).errorCode() == ErrorCode::AlreadyExists);
        c.bindAddress = Address();
        CHECK(UdpSocket::open(c).errorCode() == ErrorCode::InvalidArgument);
        UdpSocket moved = std::move(a);
        CHECK(moved.isOpen());
        CHECK_FALSE(a.isOpen()); // NOLINT(bugprone-use-after-move): moved-from state is defined
        moved.close();
        CHECK_FALSE(moved.isOpen());
        const u8 x[] = {1};
        CHECK(moved.sendTo(Address::loopbackV4(9), x).errorCode() == ErrorCode::InvalidState);
    }

    TEST_CASE("SocketTransport batches through the IDatagramTransport interface") {
        SocketTransportConfig c;
        c.socket.bindAddress = Address::loopbackV4(0);
        c.batchSize = 16;
        auto a = SocketTransport::open(c).value();
        auto b = SocketTransport::open(c).value();
        for (u32 i = 0; i < 40; ++i) {
            const u8 d[] = {static_cast<u8>(i)};
            a->send(b->localAddress(), d);
        }
        CHECK(a->socket().stats().datagramsSent == 32); // two full batches went out on their own
        a->flush();
        CHECK(a->socket().stats().datagramsSent == 40);
        u8 buf[8];
        Address from;
        usize got = 0;
        const f64 end = monotonicSeconds() + 2.0;
        while (got < 40 && monotonicSeconds() < end) {
            if (b->receive(from, buf) == 1) {
                CHECK(buf[0] == got);
                ++got;
            } else {
                sleepMillis(1);
            }
        }
        CHECK(got == 40);
    }

    TEST_CASE("perf: NS-0.2: loopback 100k pps per core without loss") {
        // One thread sends and receives (both ends on one core) in batches of 64 small datagrams.
        UdpSocket tx = openLoopback();
        UdpSocket rx = openLoopback();
        constexpr usize kBatch = 64;
        constexpr usize kTotal = 200'000;
        std::vector<u8> payload(100, 0x5A);
        std::vector<OutDatagram> out(kBatch, OutDatagram{rx.localAddress(), payload});
        std::vector<u8> arena(kBatch * 2048);
        std::vector<InDatagram> slots(kBatch);
        usize sent = 0, received = 0;
        const f64 cpu0 = os::threadCpuSeconds();
        const f64 t0 = monotonicSeconds();
        while (sent < kTotal) {
            sent += tx.sendBatch(out);
            // Drain what arrived (loopback delivers synchronously into the receive buffer).
            for (;;) {
                for (usize i = 0; i < kBatch; ++i) slots[i].buffer = std::span<u8>(arena.data() + i * 2048, 2048);
                const usize n = rx.receiveBatch(slots);
                received += n;
                if (n < kBatch) break;
            }
        }
        const f64 end = monotonicSeconds() + 2.0;
        while (received < sent && monotonicSeconds() < end) {
            for (usize i = 0; i < kBatch; ++i) slots[i].buffer = std::span<u8>(arena.data() + i * 2048, 2048);
            received += rx.receiveBatch(slots);
        }
        const f64 wall = monotonicSeconds() - t0;
        const f64 cpu = os::threadCpuSeconds() - cpu0;
        const f64 ppsPerCore = static_cast<f64>(received) / std::max(cpu, 1e-6);
        MESSAGE("NS-0.2: " << received << "/" << sent << " datagrams in " << wall * 1000.0 << " ms wall, " << cpu * 1000.0
                           << " ms CPU -> " << static_cast<u64>(ppsPerCore) << " pps per core (send+receive on one core); "
                           << "syscalls tx " << tx.stats().sendSyscalls << " rx " << rx.stats().receiveSyscalls);
        CHECK(sent == kTotal);
        CHECK(received == sent); // without loss
        CHECK(tx.stats().sendWouldBlock == 0);
        CHECK(ppsPerCore >= 100'000.0);
    }
}
