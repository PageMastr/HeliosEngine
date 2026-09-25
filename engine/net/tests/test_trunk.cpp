// Trunk profile (04 §2.6): 256 KB reassembly, 4,096-entry packet buffers, coalescing of small
// chunks, stream framing, and a short NS-0.7 throughput run over real UDP loopback (the full
// 10-minute gate is `net_bench --gate`).

#include <doctest/doctest.h>

#include "net_test_util.h"
#include "trunk_gate.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::test;

TEST_SUITE("net.trunk") {
    TEST_CASE("trunk profile values") {
        const ConnectionConfig t = ConnectionConfig::trunk();
        CHECK(t.sentPacketsBufferSize == 4096);
        CHECK(t.receivedPacketsBufferSize == 4096);
        CHECK(t.maxJumboMessageBytes + 16 == 256 * 1024);
        CHECK_FALSE(t.aimd);
        CHECK(t.pacingInterval == 0.0);
        CHECK(t.inboundPacketsPerSecond == 0.0);
        const SocketTransportConfig sock = SocketTransportConfig::trunk();
        CHECK(sock.socket.receiveBufferBytes == 32u * 1024 * 1024);
        CHECK(sock.socket.sendBufferBytes == 32u * 1024 * 1024);
        CHECK(sock.batchSize == 64);
    }

    TEST_CASE("256 KB CONTROL messages reassemble over a trunk under loss") {
        NetSimLink link;
        link.latencyMs = 1.0;
        link.lossPercent = 1.0;
        LinkedPair pair(ConnectionConfig::trunk(), ConnectionConfig::trunk(), link, link, 31);
        std::vector<u8> blob(ConnectionConfig::trunk().maxJumboMessageBytes);
        for (usize i = 0; i < blob.size(); ++i) blob[i] = static_cast<u8>(i * 13 + (i >> 8));
        REQUIRE(pair.a().send(Channel::Control, blob) == SendResult::Ok);
        std::vector<u8> handoff(200 * 1024, 0x42); // handoff blobs ride BULK slices
        REQUIRE(pair.a().send(Channel::Bulk, handoff) == SendResult::Ok);
        std::vector<std::pair<Channel, std::vector<u8>>> got;
        for (int i = 0; i < 3000 && got.size() < 2; ++i) {
            pair.tick(0.002);
            LinkedPair::drain(pair.b(), got);
        }
        REQUIRE(got.size() == 2);
        // Channels are independent: the sliced BULK blob usually completes first, because one lost
        // fragment of the 256-fragment CONTROL packet costs a whole retransmission.
        for (const auto& [ch, data] : got) {
            if (ch == Channel::Control) CHECK(data == blob);
            else if (ch == Channel::Bulk) CHECK(data == handoff);
            else FAIL("unexpected channel");
        }
        CHECK(got[0].first != got[1].first);
        CHECK(pair.a().stats().fragmentsSent >= 256);
    }

    TEST_CASE("small chunks for many sessions coalesce into full trunk datagrams") {
        LinkedPair pair(ConnectionConfig::trunk(), ConnectionConfig::trunk());
        // 400 chunks of 40 B, each prefixed with its stream id (session), sent in one tick.
        for (u64 session = 0; session < 400; ++session) {
            u8 buf[64];
            const auto payload = makePayload(static_cast<u32>(session), 40);
            const usize n = wire::writeStreamMessage(buf, session * 1'000'003ull, payload);
            REQUIRE(pair.a().send(Channel::State, std::span<const u8>(buf, n)) == SendResult::Ok);
        }
        pair.a().flush(0.0);
        // 400 x (1 hdr + 1 len + 3-4 stream id + 40) bytes ~= 18.4 KB -> ~16 datagrams, not 400.
        CHECK(pair.datagramsAB() <= 17);
        pair.advance(0.001);
        std::vector<std::pair<Channel, std::vector<u8>>> got;
        LinkedPair::drain(pair.b(), got);
        REQUIRE(got.size() == 400);
        for (usize i = 0; i < got.size(); ++i) {
            u64 stream = 0;
            std::span<const u8> payload;
            REQUIRE(wire::readStreamMessage(got[i].second, stream, payload));
            CHECK(stream == i * 1'000'003ull);
            CHECK(payloadIndex(payload) == i);
            CHECK(payloadValid(payload));
        }
    }

    TEST_CASE("4,096-entry buffers keep acks flowing at high packet rates") {
        LinkedPair pair(ConnectionConfig::trunk(), ConnectionConfig::trunk());
        // 2,000 datagrams in one burst: far more than reliable's 33-packet ack window, so the
        // receiver must ack every 16 packets while the burst is still arriving.
        const std::vector<u8> chunk(1100, 0x11);
        u64 delivered = 0;
        for (int i = 0; i < 2000; ++i) REQUIRE(pair.a().sendState(chunk) == SendResult::Ok);
        pair.a().flush(0.0);
        CHECK(pair.datagramsAB() == 2000);
        pair.advance(0.001);
        for (const PacketNotify& n : pair.a().notifications()) delivered += n.delivered ? 1 : 0;
        CHECK(pair.b().inboxSize() == 2000);
        CHECK(delivered == 2000);
        CHECK(pair.a().stats().packetsLost == 0);
        CHECK(pair.b().stats().ackOnlyPackets >= 2000 / 16 - 1);
    }

    TEST_CASE("perf: NS-0.7 (short run): trunk throughput over UDP loopback") {
        bench::TrunkGateConfig cfg;
        // Full datagrams: the STATE message plus reliable's largest header fill netcode's 1,200 B.
        CHECK(wire::encodedSize(Channel::State, cfg.payloadBytes) + wire::kReliableMaxHeader == wire::kNetcodeMaxPayload);
        cfg.seconds = 1.0;
        cfg.targetPps = 20'000;
        const bench::TrunkGateResult r = bench::runTrunkGate(cfg);
        REQUIRE(r.connected);
        MESSAGE("NS-0.7 short run: sent " << r.sent << ", delivered " << r.delivered << " (" << r.deliveredPps
                                          << " pps, " << r.payloadMbps << " Mbit/s payload, " << r.wireMbps
                                          << " Mbit/s wire), drops " << r.dropPercent << " %, cell thread "
                                          << r.senderCores << " cores, gateway thread " << r.receiverCores
                                          << " cores, rcvbuf " << r.grantedReceiveBuffer / 1024 << " KB");
        // Targets: 20k pps, < 0.1 % drops, <= 1 core. This machine is shared with other builds, so
        // the unit test asserts a margin below the gate; net_bench --gate enforces the real one.
        CHECK(r.deliveredPps >= 15'000.0);
        CHECK(r.payloadMbps >= 140.0); // 190 Mbit/s at the full 20k pps
        CHECK(r.dropPercent < 1.0);
        CHECK(r.senderCores <= 1.0);
        CHECK(r.receiverCores <= 1.0);
    }
}
