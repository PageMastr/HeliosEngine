// L0 UdpSocket and SocketTransport on real loopback sockets, including the NS-0.2 loopback
// packet-rate measurement.

#include <doctest/doctest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/net/transport.h"
#include "helios/net/udp_socket.h"
#include "net_internal.h"
#include "platform/net_os.h"

using namespace helios;
using namespace helios::net;

namespace {

UdpSocket openLoopback(u32 bufferBytes = 8u * 1024 * 1024, UdpBatchApi api = UdpBatchApi::Auto) {
    UdpSocketConfig c;
    c.bindAddress = Address::loopbackV4(0);
    c.sendBufferBytes = bufferBytes;
    c.receiveBufferBytes = bufferBytes;
    c.batchApi = api;
    auto s = UdpSocket::open(c);
    REQUIRE(s);
    return std::move(s).value();
}

UdpSocket openLoopbackWith(UdpBatchApi api) { return openLoopback(8u * 1024 * 1024, api); }

/// What each request resolves to on this platform (04 §2.6): the fastest API it offers for Auto, the
/// API itself where the OS has it, Message otherwise.
UdpBatchApi expectedApi(UdpBatchApi requested) {
#if defined(_WIN32)
    if (requested == UdpBatchApi::Auto || requested == UdpBatchApi::Registered) return UdpBatchApi::Registered;
#elif defined(__linux__)
    if (requested == UdpBatchApi::Auto || requested == UdpBatchApi::MultiMessage) return UdpBatchApi::MultiMessage;
#endif
    (void)requested;
    return UdpBatchApi::Message;
}

constexpr UdpBatchApi kAllApis[] = {UdpBatchApi::Auto, UdpBatchApi::Registered, UdpBatchApi::MultiMessage,
                                    UdpBatchApi::Message};

/// Receives with receiveBatch() into 64 slots of `slotBytes` until `count` datagrams arrived or
/// `timeout` passed; returns the datagrams' payloads in arrival order.
std::vector<std::vector<u8>> receiveAll(UdpSocket& s, usize count, usize slotBytes = 2048, f64 timeout = 2.0) {
    std::vector<u8> arena(64 * slotBytes);
    std::vector<InDatagram> slots(64);
    std::vector<std::vector<u8>> got;
    const f64 end = monotonicSeconds() + timeout;
    while (got.size() < count && monotonicSeconds() < end) {
        for (usize i = 0; i < slots.size(); ++i) slots[i].buffer = std::span<u8>(arena.data() + i * slotBytes, slotBytes);
        const usize n = s.receiveBatch(slots);
        for (usize i = 0; i < n; ++i) got.emplace_back(slots[i].buffer.begin(), slots[i].buffer.begin() + slots[i].size);
        if (n == 0) sleepMillis(1);
    }
    return got;
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

    TEST_CASE("batch APIs: Auto picks the platform's fastest and an API the OS lacks falls back to Message") {
        for (const UdpBatchApi requested : kAllApis) {
            CAPTURE(udpBatchApiName(requested));
            UdpSocket s = openLoopbackWith(requested);
            CHECK(s.batchApi() != UdpBatchApi::Auto);
            CHECK(s.batchApi() == expectedApi(requested));
            MESSAGE(udpBatchApiName(requested) << " -> " << udpBatchApiName(s.batchApi()));
        }
        // Moving a socket keeps its API (and, with Registered I/O, its queues and buffers).
        UdpSocket a = openLoopbackWith(UdpBatchApi::Auto);
        const UdpBatchApi api = a.batchApi();
        UdpSocket b = std::move(a);
        CHECK(b.batchApi() == api);
        UdpSocket c = openLoopbackWith(UdpBatchApi::Message);
        c = std::move(b);
        CHECK(c.batchApi() == api);
        const u8 x[] = {9};
        UdpSocket r = openLoopbackWith(UdpBatchApi::Message);
        REQUIRE(c.sendTo(r.localAddress(), x));
        CHECK(receiveAll(r, 1).size() == 1);
        CHECK(udpBatchApiName(UdpBatchApi::Registered) == "registered");
        CHECK(udpBatchApiName(UdpBatchApi::MultiMessage) == "multi-message");
    }

    TEST_CASE("batch APIs: every API has the same batch semantics") {
        for (const UdpBatchApi requested : kAllApis) {
            CAPTURE(udpBatchApiName(requested));
            UdpSocket a = openLoopbackWith(requested);
            UdpSocket b = openLoopbackWith(requested);
            CAPTURE(udpBatchApiName(a.batchApi()));
            std::vector<u8> slot(2048);
            Address from;
            // Nothing pending: both receive calls return at once with nothing.
            InDatagram one;
            one.buffer = slot;
            CHECK(b.receiveBatch(std::span<InDatagram>(&one, 1)) == 0);
            CHECK(b.receiveFrom(from, slot) == 0);
            CHECK(b.receiveBatch({}) == 0);
            CHECK(a.sendBatch({}) == 0);

            // A batch with a destination this socket cannot reach (IPv6 from an IPv4 socket): it is
            // skipped and counted, and the rest go out.
            std::vector<std::vector<u8>> payloads(100);
            std::vector<OutDatagram> out;
            for (usize i = 0; i < payloads.size(); ++i) {
                payloads[i].assign(10 + i * 7, static_cast<u8>(i));
                out.push_back(OutDatagram{b.localAddress(), payloads[i]});
            }
            const std::vector<u8> stray{1, 2, 3};
            out.insert(out.begin() + 50, OutDatagram{Address::loopbackV6(b.localAddress().port()), stray});
            CHECK(a.sendBatch(out) == 100);
            CHECK(a.stats().sendErrors == 1);
            CHECK(a.stats().datagramsSent == 100);
            CHECK(a.stats().sendSyscalls >= 1);
            CHECK(a.sendTo(Address::loopbackV6(b.localAddress().port()), stray).errorCode() ==
                  ErrorCode::InvalidArgument);

            // Received in partial batches (64 + 36), each datagram whole and from the sender.
            const auto got = receiveAll(b, 100);
            REQUIRE(got.size() == 100);
            std::vector<bool> seen(100, false);
            for (const auto& g : got) {
                REQUIRE(g.size() >= 10);
                const usize idx = (g.size() - 10) / 7;
                REQUIRE(idx < 100);
                CHECK(g == payloads[idx]);
                seen[idx] = true;
            }
            for (bool x : seen) CHECK(x);
            CHECK(b.stats().datagramsReceived == 100);
            u64 bytes = 0;
            for (const auto& p : payloads) bytes += p.size();
            CHECK(b.stats().bytesReceived == bytes);
            CHECK(a.stats().bytesSent == bytes);

            // The addresses come back as the sender's.
            const u8 hello[] = {42, 43};
            REQUIRE(a.sendTo(b.localAddress(), hello));
            usize n = 0;
            for (int i = 0; i < 1000 && n == 0; ++i) {
                n = b.receiveFrom(from, slot);
                if (n == 0) sleepMillis(1);
            }
            CHECK(n == 2);
            CHECK(from == a.localAddress());

            // A datagram larger than the slot it lands in is dropped and counted, never cut, and the
            // next one still arrives, whether it comes through receiveBatch or receiveFrom.
            const std::vector<u8> big(1500, 0xAB);
            const u8 small[] = {7};
            REQUIRE(a.sendTo(b.localAddress(), big));
            REQUIRE(a.sendTo(b.localAddress(), small));
            const auto afterBig = receiveAll(b, 1, /*slotBytes=*/100);
            REQUIRE(afterBig.size() == 1);
            CHECK(afterBig[0] == std::vector<u8>{7});
            CHECK(b.stats().truncated == 1);
            REQUIRE(a.sendTo(b.localAddress(), big));
            REQUIRE(a.sendTo(b.localAddress(), small));
            std::vector<u8> tiny(100);
            n = 0;
            for (int i = 0; i < 1000 && n == 0; ++i) {
                n = b.receiveFrom(from, tiny);
                if (n == 0) sleepMillis(1);
            }
            CHECK(n == 1);
            CHECK(tiny[0] == 7);
            CHECK(b.stats().truncated == 2);
            CHECK(b.stats().receiveErrors == 0);
        }
    }

    TEST_CASE("batch APIs: interoperate, and a burst larger than the in-flight limits is accounted for") {
        for (const UdpBatchApi senderApi : kAllApis) {
            for (const UdpBatchApi receiverApi : {UdpBatchApi::Auto, UdpBatchApi::Message}) {
                CAPTURE(udpBatchApiName(senderApi));
                CAPTURE(udpBatchApiName(receiverApi));
                UdpSocket tx = openLoopbackWith(senderApi);
                UdpSocket rx = openLoopbackWith(receiverApi);
                // 400 datagrams in one call, more than one sendmmsg batch of 64, received only after the
                // whole burst is out: the receive buffer must hold them all. A RIO socket buffers nothing
                // beyond its posted receives (128 of 400 arrived before they followed the 8 MB buffer
                // size), so this checks that its slots do. Small, so that a buffer capped by rmem_max
                // (unprivileged Linux runners) holds them too.
                std::vector<u8> payload(16, 0x33);
                std::vector<OutDatagram> out(400, OutDatagram{rx.localAddress(), payload});
                const usize sent = tx.sendBatch(out);
                CHECK(sent + tx.stats().sendWouldBlock == 400);
                CHECK(sent >= 128); // at least the slots that were free
                CHECK(tx.stats().sendErrors == 0);
                const auto got = receiveAll(rx, sent);
                CHECK(got.size() == sent);
                CHECK(rx.stats().datagramsReceived == sent);
            }
        }
    }

#if defined(_WIN32)
    TEST_CASE("batch APIs: Registered I/O refuses datagrams that do not fit its 2 KB slots") {
        UdpSocket rio = openLoopbackWith(UdpBatchApi::Registered);
        UdpSocket msg = openLoopbackWith(UdpBatchApi::Message);
        REQUIRE(rio.batchApi() == UdpBatchApi::Registered);
        const std::vector<u8> fits(2047, 0x11);
        const std::vector<u8> tooBig(2048, 0x22);
        CHECK(rio.sendTo(msg.localAddress(), fits));
        CHECK(rio.sendTo(msg.localAddress(), tooBig).errorCode() == ErrorCode::InvalidArgument);
        const std::vector<OutDatagram> batch{{msg.localAddress(), tooBig}, {msg.localAddress(), fits}};
        CHECK(rio.sendBatch(batch) == 1);
        CHECK(rio.stats().sendErrors == 2);
        const auto got = receiveAll(msg, 2, /*slotBytes=*/4096);
        REQUIRE(got.size() == 2);
        CHECK(got[0].size() == 2047);
        // The other way: a 3,000-byte datagram does not fit a receive slot, so it is counted as truncated
        // even though the caller's buffer would hold it; the next one arrives.
        const std::vector<u8> large(3000, 0x44);
        REQUIRE(msg.sendTo(rio.localAddress(), large));
        REQUIRE(msg.sendTo(rio.localAddress(), fits));
        const auto back = receiveAll(rio, 1, /*slotBytes=*/4096);
        REQUIRE(back.size() == 1);
        CHECK(back[0].size() == 2047);
        CHECK(rio.stats().truncated == 1);
    }

    TEST_CASE("batch APIs: Registered I/O sizes its slots from the buffer sizes (send slots capped at 2,048) and "
              "commits a burst larger than its send slots in slot-sized steps") {
        // A RIO socket reports its slot capacity (slots x 2,048 B) as its buffer sizes. Receive slots follow
        // the receive buffer (32 MB / 2,112 B per slot = 15,887) up to 16,384; send slots follow the send buffer
        // up to 2,048, so a 32 MB trunk socket locks 37.9 MB rather than 67.1 MB.
        const UdpSocket trunk = openLoopback(32u * 1024 * 1024, UdpBatchApi::Registered);
        REQUIRE(trunk.batchApi() == UdpBatchApi::Registered);
        CHECK(trunk.receiveBufferBytes() == 15887u * 2048);
        CHECK(trunk.sendBufferBytes() == 2048u * 2048);
        const UdpSocket defaults = openLoopbackWith(UdpBatchApi::Registered);
        CHECK(defaults.receiveBufferBytes() == 3971u * 2048); // 8 MB / (2,048 B + 64 B of address)
        CHECK(defaults.sendBufferBytes() == 2048u * 2048);

        // 64 KB buffers give the minimum of 128 slots each way. A 400-datagram burst then needs every send slot
        // more than once: each commit submits at most 128 sends, and what finds no free slot is refused as
        // would-block (counted, not lost silently), as a full socket buffer refuses it.
        UdpSocket tx = openLoopback(64u * 1024, UdpBatchApi::Registered);
        UdpSocket rx = openLoopbackWith(UdpBatchApi::Registered);
        REQUIRE(tx.batchApi() == UdpBatchApi::Registered);
        CHECK(tx.sendBufferBytes() == 128u * 2048);
        CHECK(tx.receiveBufferBytes() == 128u * 2048);
        std::vector<u8> payload(16, 0x55);
        std::vector<OutDatagram> out(400, OutDatagram{rx.localAddress(), payload});
        const usize sent = tx.sendBatch(out);
        MESSAGE("RIO with 128 send slots: " << sent << " of 400 sent in " << tx.stats().sendSyscalls << " commits, "
                                            << tx.stats().sendWouldBlock << " would-block");
        CHECK(sent >= 128);
        CHECK(sent + tx.stats().sendWouldBlock == 400);
        CHECK(tx.stats().sendSyscalls >= (sent + 127) / 128);
        CHECK(tx.stats().sendErrors == 0);
        CHECK(receiveAll(rx, sent).size() == sent);
    }
#endif

    TEST_CASE("batch APIs: when Registered I/O is unavailable or cannot be set up, open() falls back to Message "
              "on the same address (test hooks)") {
        // On Windows the hooks force each fallback a machine can take: no RIO from the provider, and a RIO
        // socket whose registration or queues are refused (open() then closes it and binds a plain socket to
        // the same address). Elsewhere they change nothing.
#if defined(_WIN32)
        const UdpBatchApi fallback = UdpBatchApi::Message;
#else
        const UdpBatchApi fallback = expectedApi(UdpBatchApi::Auto);
#endif
        // Every fallback is logged with its cause: the first in the process as a warning, later ones at debug.
        struct Captured {
            std::mutex mutex;
            std::vector<std::pair<log::Level, std::string>> records;
        };
        auto captured = std::make_shared<Captured>();
        auto sink = std::make_shared<log::CallbackSink>([captured](const log::Record& r) {
            if (r.message.find("Registered I/O") == std::string_view::npos) return;
            std::lock_guard<std::mutex> lock(captured->mutex);
            captured->records.emplace_back(r.level, std::string(r.message));
        });
        log::addSink(sink);
        log::setChannelLevel("Net", log::Level::Debug);
        net::detail::resetRegisteredIoFallbackWarning();

        // A message each way, and nothing pending returns at once (the plain socket is non-blocking).
        const auto checkRoundTrip = [](UdpSocket& s) {
            Address from;
            std::vector<u8> buf(2048);
            CHECK(s.receiveFrom(from, buf) == 0);
            InDatagram one;
            one.buffer = buf;
            CHECK(s.receiveBatch(std::span<InDatagram>(&one, 1)) == 0);
            UdpSocket peer = openLoopbackWith(UdpBatchApi::Auto);
            const u8 x[] = {1, 2, 3};
            REQUIRE(peer.sendTo(s.localAddress(), x));
            const auto in = receiveAll(s, 1);
            REQUIRE(in.size() == 1);
            CHECK(in[0] == std::vector<u8>{1, 2, 3});
            REQUIRE(s.sendTo(peer.localAddress(), x));
            CHECK(receiveAll(peer, 1).size() == 1);
        };

        // 1. The provider offers no Registered I/O.
        UdpSocketConfig auto0;
        auto0.bindAddress = Address::loopbackV4(0);
        net::detail::forceRegisteredIoUnavailable(true);
        auto unavailable = UdpSocket::open(auto0);
        net::detail::forceRegisteredIoUnavailable(false);
        REQUIRE(unavailable);
        CHECK(unavailable.value().batchApi() == fallback);
        checkRoundTrip(unavailable.value());

        // 2. Registered I/O set-up fails on a fixed port, twice (Auto, then Registered requested explicitly):
        // the re-opened socket must bind that same port again.
        for (const UdpBatchApi requested : {UdpBatchApi::Auto, UdpBatchApi::Registered}) {
            CAPTURE(udpBatchApiName(requested));
            UdpSocket probe = openLoopbackWith(UdpBatchApi::Message);
            const u16 port = probe.localAddress().port();
            probe.close();
            net::detail::forceRegisteredIoSetUpFailure(true);
            UdpSocketConfig c;
            c.bindAddress = Address::loopbackV4(port);
            c.batchApi = requested;
            auto s = UdpSocket::open(c);
            net::detail::forceRegisteredIoSetUpFailure(false);
            REQUIRE(s);
            CHECK(s.value().batchApi() == (requested == UdpBatchApi::Auto ? fallback : UdpBatchApi::Message));
            CHECK(s.value().localAddress().port() == port);
            checkRoundTrip(s.value());
            // A socket opened after the hooks are cleared gets the platform's fastest API again.
            CHECK(openLoopbackWith(UdpBatchApi::Auto).batchApi() == expectedApi(UdpBatchApi::Auto));
        }

        log::clearChannelLevel("Net");
        log::removeSink(sink.get());
        std::vector<std::pair<log::Level, std::string>> records;
        {
            std::lock_guard<std::mutex> lock(captured->mutex);
            records = captured->records;
        }
        for (const auto& r : records) MESSAGE(r.second);
#if defined(_WIN32)
        REQUIRE(records.size() == 3);
        CHECK(records[0].first == log::Level::Warn);
        CHECK(records[0].second.find("unavailable") != std::string::npos);
        for (usize i = 1; i < records.size(); ++i) {
            CHECK(records[i].first == log::Level::Debug);
            // The cause is the failing step and its own error code, captured before the clean-up.
            CHECK(records[i].second.find("RIOCreateRequestQueue") != std::string::npos);
            CHECK(records[i].second.find("(10045)") != std::string::npos); // WSAEOPNOTSUPP
        }
#else
        CHECK(records.empty());
#endif
    }

    TEST_CASE("SocketTransport runs over every batch API") {
        for (const UdpBatchApi requested : kAllApis) {
            CAPTURE(udpBatchApiName(requested));
            SocketTransportConfig c;
            c.socket.bindAddress = Address::loopbackV4(0);
            c.socket.batchApi = requested;
            c.batchSize = 16;
            auto a = SocketTransport::open(c).value();
            auto b = SocketTransport::open(c).value();
            CHECK(a->socket().batchApi() == expectedApi(requested));
            for (u32 i = 0; i < 200; ++i) {
                const u8 d[] = {static_cast<u8>(i), static_cast<u8>(i >> 8)};
                a->send(b->localAddress(), d);
            }
            a->flush();
            u8 buf[8];
            Address from;
            usize got = 0;
            const f64 end = monotonicSeconds() + 2.0;
            while (got < 200 && monotonicSeconds() < end) {
                if (b->receive(from, buf) == 2) {
                    CHECK(from == a->localAddress());
                    ++got;
                } else {
                    sleepMillis(1);
                }
            }
            CHECK(got == 200);
        }
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

} // namespace
