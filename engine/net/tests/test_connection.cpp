// Channel guarantees, fragmentation, notify, budgets, AIMD and stats on back-to-back Connections
// under NetSim (no netcode in between; endpoint tests cover the full stack).

#include <doctest/doctest.h>

#include <map>
#include <set>

#include <reliable.h>

#include "helios/core/memory.h"
#include "helios/core/random.h"
#include "net_test_util.h"
#include "platform/net_os.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::test;

namespace {

/// Large budget, no AIMD/policing/pacing: isolates channel semantics from rate control.
ConnectionConfig fastConfig() {
    ConnectionConfig c = ConnectionConfig::server();
    c.budgetBps = 50'000'000;
    c.minBudgetBps = 50'000'000;
    c.maxBudgetBps = 50'000'000;
    c.aimd = false;
    c.inboundPacketsPerSecond = 0.0;
    c.inboundBytesPerSecond = 0.0;
    c.maxPacketsPerFlush = 64;
    c.pacingInterval = 0.0;
    return c;
}

NetSimLink hostileLink() {
    NetSimLink l;
    l.latencyMs = 20.0;
    l.jitterMs = 5.0;
    l.lossPercent = 10.0;
    l.duplicatePercent = 5.0;
    l.reorderPercent = 5.0;
    l.reorderDelayMs = 15.0;
    return l;
}

using Received = std::vector<std::pair<Channel, std::vector<u8>>>;

/// Captures everything a raw reliable endpoint transmits (for crafting packets).
struct RawCapture {
    std::vector<std::vector<u8>> packets;
    static void transmit(void* ctx, uint64_t, uint16_t, uint8_t* data, int bytes) {
        static_cast<RawCapture*>(ctx)->packets.emplace_back(data, data + bytes);
    }
    static int process(void*, uint64_t, uint16_t, uint8_t*, int) { return 1; }
};

void nullSend(void*, std::span<const u8>) {}

/// Packs HTP messages into raw reliable packets (<= ~1,150 B each), as a hostile peer would.
class RawPacker {
public:
    RawPacker() {
        reliable_config_t rc;
        reliable_default_config(&rc);
        rc.context = &m_cap;
        rc.transmit_packet_function = &RawCapture::transmit;
        rc.process_packet_function = &RawCapture::process;
        m_endpoint = reliable_endpoint_create(&rc, 0.0);
        REQUIRE(m_endpoint);
    }
    ~RawPacker() { reliable_endpoint_destroy(m_endpoint); }
    void add(Channel ch, u16 seq, std::span<const u8> payload) {
        u8 msg[64];
        const usize n = wire::writeMessage(msg, ch, seq, 0, 1, payload);
        REQUIRE(n > 0);
        if (m_pending.size() + n > 1150) seal();
        m_pending.insert(m_pending.end(), msg, msg + n);
    }
    std::vector<std::vector<u8>>& packets() {
        seal();
        return m_cap.packets;
    }

private:
    void seal() {
        if (m_pending.empty()) return;
        reliable_endpoint_send_packet(m_endpoint, m_pending.data(), static_cast<int>(m_pending.size()));
        m_pending.clear();
    }
    RawCapture m_cap;
    reliable_endpoint_t* m_endpoint = nullptr;
    std::vector<u8> m_pending;
};

std::array<u8, 2> indexBytes(u32 i) { return {static_cast<u8>(i & 0xFF), static_cast<u8>(i >> 8)}; }
u32 indexOf(std::span<const u8> p) { return p.size() == 2 ? static_cast<u32>(p[0] | (p[1] << 8)) : 0xFFFFFFFFu; }

/// Drives a gateway (A, server profile) and a client (B, client profile) with separate tick rates
/// in 1 ms I/O steps: the client sends an INPUT every client tick, the gateway a STATE chunk
/// every gateway tick. `extra(now)` runs every step.
template <class Extra>
void runTicks(LinkedPair& pair, f64 seconds, f64 clientHz, f64 serverHz, u32 inputBytes, Extra&& extra) {
    f64 nextClient = pair.now(), nextServer = pair.now();
    const f64 end = pair.now() + seconds;
    u32 i = 0;
    while (pair.now() < end - 1e-9) {
        pair.advance(0.001);
        extra(pair.now());
        if (pair.now() >= nextClient - 1e-9) {
            (void)pair.b().send(Channel::Input, makePayload(i++, inputBytes));
            pair.b().flush(pair.now());
            nextClient += 1.0 / clientHz;
        }
        if (pair.now() >= nextServer - 1e-9) {
            (void)pair.a().sendState(makePayload(i, 300));
            pair.a().flush(pair.now());
            nextServer += 1.0 / serverHz;
        }
    }
}

} // namespace

TEST_SUITE("net.connection") {
    TEST_CASE("reliable-ordered channels: exactly once, in order, under 10% loss + reorder + dup") {
        LinkedPair pair(fastConfig(), fastConfig(), hostileLink(), hostileLink(), 101);
        Random rng(5);
        std::vector<Channel> channels = {Channel::Control, Channel::EventReliable};
        if constexpr (HELIOS_NET_DEBUG_CHANNEL != 0) channels.push_back(Channel::Debug);
        std::map<Channel, u32> sentA, sentB;
        Received gotB, gotA;
        const u32 kPerChannel = 600;
        for (int tick = 0; tick < 3000; ++tick) {
            for (Channel ch : channels) {
                for (int k = 0; k < 4 && sentA[ch] < kPerChannel; ++k) {
                    // Mostly small; every 50th CONTROL message is a 5 KB burst (reliable fragments it).
                    usize size = 4 + rng.nextU32() % 300;
                    if (ch == Channel::Control && sentA[ch] % 50 == 49) size = 5000;
                    REQUIRE(pair.a().send(ch, makePayload(sentA[ch], size)) == SendResult::Ok);
                    ++sentA[ch];
                    REQUIRE(pair.b().send(ch, makePayload(sentB[ch], 8)) == SendResult::Ok);
                    ++sentB[ch];
                }
            }
            pair.tick(0.02);
            LinkedPair::drain(pair.b(), gotB);
            LinkedPair::drain(pair.a(), gotA);
            if (gotB.size() == kPerChannel * channels.size() && gotA.size() == kPerChannel * channels.size()) break;
        }
        for (const Received* got : {&gotB, &gotA}) {
            std::map<Channel, u32> next;
            for (const auto& [ch, data] : *got) {
                CHECK(payloadValid(data));
                CHECK(payloadIndex(data) == next[ch]); // in order, no gaps, no duplicates
                ++next[ch];
            }
            for (Channel ch : channels) CHECK(next[ch] == kPerChannel);
        }
        const ConnectionStats sa = pair.a().stats();
        CHECK(sa.channels[channelId(Channel::Control)].resends > 0);
        CHECK(sa.jumboPackets > 0);
        CHECK(sa.packetsLost > 0);
        CHECK(pair.pipeAB().stats().duplicated > 0);
        CHECK(pair.pipeAB().stats().reordered > 0);
        CHECK(pair.b().stats().packetsDuplicate > 0); // NetSim duplicates dropped by reliable
        CHECK(pair.a().pendingReliableBytes() == 0);
        CHECK(pair.a().strikes() == 0);
        CHECK(pair.b().strikes() == 0);
    }

    TEST_CASE("unreliable channels: at most once; LATEST never goes backwards") {
        LinkedPair pair(fastConfig(), fastConfig(), hostileLink(), {}, 202);
        Received got;
        const u32 kTicks = 2000;
        for (u32 i = 0; i < kTicks; ++i) {
            REQUIRE(pair.a().send(Channel::EventUnreliable, makePayload(i, 40)) == SendResult::Ok);
            REQUIRE(pair.a().send(Channel::Latest, makePayload(i, 12)) == SendResult::Ok);
            pair.tick(0.016);
            LinkedPair::drain(pair.b(), got);
        }
        pair.tick(0.5);
        LinkedPair::drain(pair.b(), got);
        std::set<u32> eventsSeen;
        u32 events = 0, latest = 0;
        i64 lastLatest = -1;
        for (const auto& [ch, data] : got) {
            REQUIRE(payloadValid(data));
            const u32 idx = payloadIndex(data);
            if (ch == Channel::EventUnreliable) {
                CHECK(eventsSeen.insert(idx).second); // never twice, despite 5 % duplication
                ++events;
            } else if (ch == Channel::Latest) {
                CHECK(static_cast<i64>(idx) > lastLatest); // strictly newer every time
                lastLatest = idx;
                ++latest;
            }
        }
        CHECK(events == doctest::Approx(kTicks * 0.9).epsilon(0.05));
        CHECK(latest > kTicks * 0.8);
        CHECK(latest < events + 1); // reordered stale LATEST updates were dropped
        CHECK(pair.b().stats().channels[channelId(Channel::Latest)].dropped > 0);
    }

    TEST_CASE("INPUT: last 4 inputs ride redundantly; almost nothing is lost at 10 %") {
        LinkedPair pair(fastConfig(), ConnectionConfig::client(), {}, hostileLink(), 303);
        Received got;
        const u32 kInputs = 3000;
        for (u32 i = 0; i < kInputs; ++i) {
            // 12-byte command frames: 4 redundant copies stay inside the 64 kbit/s upstream bucket.
            REQUIRE(pair.b().send(Channel::Input, makePayload(i, 12)) == SendResult::Ok);
            pair.tick(1.0 / 60.0, 2);
            LinkedPair::drain(pair.a(), got);
        }
        pair.tick(0.5);
        LinkedPair::drain(pair.a(), got);
        i64 last = -1;
        u32 count = 0;
        for (const auto& [ch, data] : got) {
            REQUIRE(ch == Channel::Input);
            const u32 idx = payloadIndex(data);
            CHECK(static_cast<i64>(idx) > last); // in order, each at most once
            last = idx;
            ++count;
        }
        // P(input lost) ~ P(4 packets in a row lost) = 1e-4 at 10 % loss.
        CHECK(count >= kInputs - 5);
        CHECK(pair.b().stats().channels[channelId(Channel::Input)].resends > 0);
        CHECK(pair.a().stats().channels[channelId(Channel::Input)].dropped > 0); // redundant copies discarded
    }

    TEST_CASE("STATE: exactly one delivery notification per chunk, never a false 'delivered'") {
        LinkedPair pair(fastConfig(), fastConfig(), hostileLink(), hostileLink(), 404);
        std::map<u64, u32> seqToIndex;
        std::set<u32> received;
        Received got;
        std::map<u64, int> notified;
        std::set<u64> delivered;
        const u32 kChunks = 2000;
        for (u32 i = 0; i < kChunks; ++i) {
            u64 seq = 0;
            REQUIRE(pair.a().sendState(makePayload(i, 900), &seq) == SendResult::Ok);
            seqToIndex[seq] = i;
            pair.tick(0.05, 2);
            LinkedPair::drain(pair.b(), got);
            for (const PacketNotify& n : pair.a().notifications()) {
                ++notified[n.seq];
                if (n.delivered) delivered.insert(n.seq);
            }
            pair.a().clearNotifications();
        }
        for (int k = 0; k < 40; ++k) {
            pair.tick(0.05);
            LinkedPair::drain(pair.b(), got);
            for (const PacketNotify& n : pair.a().notifications()) {
                ++notified[n.seq];
                if (n.delivered) delivered.insert(n.seq);
            }
            pair.a().clearNotifications();
        }
        for (const auto& [ch, data] : got) {
            REQUIRE(ch == Channel::State);
            CHECK(received.insert(payloadIndex(data)).second);
        }
        CHECK(notified.size() == kChunks);
        for (const auto& [seq, count] : notified) CHECK(count == 1);
        u32 falseNegatives = 0;
        for (const auto& [seq, index] : seqToIndex) {
            if (delivered.count(seq)) CHECK(received.count(index) == 1); // delivered => really received
            else if (received.count(index)) ++falseNegatives;            // ack lost: repaired by replication
        }
        INFO("delivered " << delivered.size() << ", false negatives " << falseNegatives);
        CHECK(delivered.size() >= kChunks * 84 / 100);
        CHECK(delivered.size() <= kChunks * 94 / 100);
        CHECK(falseNegatives < kChunks / 20);
        const ConnectionStats s = pair.a().stats();
        CHECK(s.stateDelivered + s.stateLost == kChunks);
    }

    TEST_CASE("BULK: sliced blobs up to 256 KB reassemble exactly under loss") {
        ConnectionConfig cfg = fastConfig();
        LinkedPair pair(cfg, cfg, hostileLink(), hostileLink(), 505);
        const std::vector<usize> sizes = {256 * 1024, 100 * 1000, 1, 0, 1024, 1025, 70 * 1024};
        std::vector<std::vector<u8>> blobs;
        for (usize i = 0; i < sizes.size(); ++i) {
            std::vector<u8> b(sizes[i]);
            for (usize k = 0; k < b.size(); ++k) b[k] = static_cast<u8>((k * 131 + i * 7) & 0xFF);
            blobs.push_back(b);
            REQUIRE(pair.a().send(Channel::Bulk, b) == SendResult::Ok);
        }
        // Higher-priority traffic keeps flowing while BULK drains in the background.
        Received got;
        u32 controls = 0;
        for (int tick = 0; tick < 4000; ++tick) {
            if (tick < 200) REQUIRE(pair.a().send(Channel::Control, makePayload(static_cast<u32>(tick), 16)) == SendResult::Ok);
            pair.tick(0.02);
            LinkedPair::drain(pair.b(), got);
            usize bulks = 0;
            for (const auto& g : got) bulks += g.first == Channel::Bulk ? 1 : 0;
            if (bulks == blobs.size() && got.size() == blobs.size() + 200) break;
        }
        usize b = 0;
        for (const auto& [ch, data] : got) {
            if (ch == Channel::Control) {
                ++controls;
                continue;
            }
            REQUIRE(ch == Channel::Bulk);
            REQUIRE(b < blobs.size());
            CHECK(data == blobs[b]);
            ++b;
        }
        CHECK(b == blobs.size());
        CHECK(controls == 200);
        CHECK(pair.b().strikes() == 0);
    }

    TEST_CASE("CONTROL bursts up to 16 KB travel as one reliable-fragmented packet") {
        LinkedPair pair(fastConfig(), fastConfig(), hostileLink(), hostileLink(), 606);
        std::vector<u8> big(16 * 1024);
        for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 7);
        REQUIRE(pair.a().send(Channel::Control, big) == SendResult::Ok);
        REQUIRE(pair.a().send(Channel::Control, bytesOf("after")) == SendResult::Ok);
        Received got;
        for (int i = 0; i < 500 && got.size() < 2; ++i) {
            pair.tick(0.02);
            LinkedPair::drain(pair.b(), got);
        }
        REQUIRE(got.size() == 2);
        CHECK(got[0].second == big);
        CHECK(got[1].second == bytesOf("after"));
        const ConnectionStats s = pair.a().stats();
        CHECK(s.jumboPackets >= 1);
        CHECK(s.fragmentsSent >= 17);
        CHECK(pair.b().stats().fragmentsReceived >= 17);
        CHECK(pair.a().send(Channel::Control, std::vector<u8>(16 * 1024 + 1)) == SendResult::TooLarge);
    }

    TEST_CASE("send limits: WouldBlock at 1,024 in flight, TooLarge per channel") {
        LinkedPair pair(fastConfig(), fastConfig());
        for (int i = 0; i < 1024; ++i) REQUIRE(pair.a().send(Channel::EventReliable, makePayload(i, 8)) == SendResult::Ok);
        CHECK(pair.a().send(Channel::EventReliable, makePayload(1024, 8)) == SendResult::WouldBlock);
        CHECK(pair.a().stats().channels[channelId(Channel::EventReliable)].wouldBlock == 1);
        const std::vector<u8> blob(256 * 1024);
        for (int i = 0; i < 4; ++i) REQUIRE(pair.a().send(Channel::Bulk, blob) == SendResult::Ok); // 4 x 256 slices
        CHECK(pair.a().send(Channel::Bulk, std::vector<u8>(1)) == SendResult::WouldBlock);
        CHECK(pair.a().send(Channel::Bulk, std::vector<u8>(256 * 1024 + 1)) == SendResult::TooLarge);
        const u32 single = wire::maxPayloadFor(Channel::EventUnreliable, wire::kMaxPacketPayload);
        CHECK(pair.a().send(Channel::EventUnreliable, std::vector<u8>(single)) == SendResult::Ok);
        CHECK(pair.a().send(Channel::EventUnreliable, std::vector<u8>(single + 1)) == SendResult::TooLarge);
        CHECK(pair.a().send(Channel::State, std::vector<u8>(1100)) == SendResult::Ok); // replication chunk
        CHECK(pair.a().send(Channel::Input, std::vector<u8>(2000)) == SendResult::TooLarge);
        // Once the peer acks, the window drains and sends succeed again.
        for (int i = 0; i < 200; ++i) pair.tick(0.02);
        CHECK(pair.a().send(Channel::EventReliable, makePayload(1, 8)) == SendResult::Ok);
    }

    TEST_CASE("malicious fragments are rejected without harm; the real packet still reassembles") {
        // Capture the fragments of one 16 KB CONTROL message.
        struct Sink {
            std::vector<std::vector<u8>> packets;
            static void send(void* ctx, std::span<const u8> p) {
                static_cast<Sink*>(ctx)->packets.emplace_back(p.begin(), p.end());
            }
        } sink;
        auto a = Connection::create(fastConfig(), &Sink::send, &sink, 0.0, "A").value();
        std::vector<u8> big(16 * 1024);
        for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i ^ 0x5A);
        REQUIRE(a->send(Channel::Control, big) == SendResult::Ok);
        a->flush(0.0);
        REQUIRE(sink.packets.size() == 17);
        for (const auto& f : sink.packets) REQUIRE((f[0] & 1) == 1); // reliable fragment prefix

        struct Null {
            static void send(void*, std::span<const u8>) {}
        };
        auto b = Connection::create(fastConfig(), &Null::send, nullptr, 0.0, "B").value();
        const auto feed = [&](const std::vector<u8>& p) { b->receivePacket(p, 0.01); };
        Random rng(77);
        // Header-level garbage.
        feed({1});                          // shorter than a fragment header
        feed({1, 0, 0, 5, 3});              // fragment id 5 of 4
        feed({1, 0, 0, 0, 255, 1, 2, 3});   // 256 fragments > max_fragments (17)
        auto oversized = sink.packets[3];
        oversized.resize(oversized.size() + 100, 0xEE); // longer than fragment_size
        feed(oversized);
        auto wrongCount = sink.packets[4];
        wrongCount[4] = 3; // claims 4 fragments while others say 17
        feed(wrongCount);
        auto shortMiddle = sink.packets[5];
        shortMiddle.resize(shortMiddle.size() - 10); // non-final fragment must be full size
        feed(shortMiddle);
        CHECK(b->inboxSize() == 0);
        CHECK(b->stats().fragmentsInvalid >= 6);
        // Random fragment-shaped noise and a corrupted header go to a separate connection: a peer
        // can only disturb reassembly of its own packets (netcode authenticated every byte), so
        // this checks robustness, not isolation.
        auto c = Connection::create(fastConfig(), &Null::send, nullptr, 0.0, "C").value();
        auto badHeader = sink.packets[0];
        badHeader[5] ^= 0x3E; // corrupt the embedded packet header in fragment 0
        c->receivePacket(badHeader, 0.01);
        for (int i = 0; i < 5000; ++i) {
            std::vector<u8> p(5 + rng.nextU32() % 1100);
            for (u8& x : p) x = static_cast<u8>(rng.nextU32());
            p[0] = 1;
            c->receivePacket(p, 0.01 + i * 1e-4);
        }
        CHECK(c->inboxSize() == 0);
        CHECK(c->stats().fragmentsInvalid > 0);
        // Duplicates of real fragments and then the full, genuine set: delivered exactly once.
        feed(sink.packets[2]);
        feed(sink.packets[2]);
        for (const auto& f : sink.packets) feed(f);
        for (const auto& f : sink.packets) feed(f); // replayed after completion: ignored
        REQUIRE(b->inboxSize() == 1);
        CHECK(b->inboxAt(0).channel == Channel::Control);
        CHECK(std::equal(b->inboxAt(0).payload.begin(), b->inboxAt(0).payload.end(), big.begin(), big.end()));
    }

    TEST_CASE("malformed Helios payloads earn strikes; three mark the connection malformed") {
        RawCapture cap;
        reliable_config_t rc;
        reliable_default_config(&rc);
        rc.context = &cap;
        rc.transmit_packet_function = &RawCapture::transmit;
        rc.process_packet_function = &RawCapture::process;
        reliable_endpoint_t* raw = reliable_endpoint_create(&rc, 0.0);
        REQUIRE(raw);
        u8 garbage1[] = {0x09, 0x00};                // reserved channel 9
        u8 garbage2[] = {0x04, 0x10, 0x01};          // truncated payload
        u8 garbage3[] = {0x20, 0xD0, 0x07, 0x00};    // CONTROL seq 2000: outside the 1,024 window
        u8 fine[] = {0x04, 0x02, 0xAB, 0xCD};        // valid EVENT_U
        reliable_endpoint_send_packet(raw, fine, sizeof(fine));
        reliable_endpoint_send_packet(raw, garbage1, sizeof(garbage1));
        reliable_endpoint_send_packet(raw, garbage2, sizeof(garbage2));
        reliable_endpoint_send_packet(raw, garbage3, sizeof(garbage3));
        reliable_endpoint_destroy(raw);
        REQUIRE(cap.packets.size() == 4);

        struct Null {
            static void send(void*, std::span<const u8>) {}
        };
        auto b = Connection::create(fastConfig(), &Null::send, nullptr, 0.0, "B").value();
        b->receivePacket(cap.packets[0], 0.0);
        CHECK(b->inboxSize() == 1);
        CHECK(b->strikes() == 0);
        b->receivePacket(cap.packets[1], 0.0);
        CHECK(b->strikes() == 1);
        CHECK_FALSE(b->isMalformed());
        b->receivePacket(cap.packets[2], 0.0);
        b->receivePacket(cap.packets[3], 0.0);
        CHECK(b->strikes() == 3);
        CHECK(b->isMalformed());
        CHECK(b->inboxSize() == 1); // nothing from the bad packets was delivered
        // A reliable header that does not parse is a strike too.
        b->receivePacket(std::vector<u8>{0x00}, 0.0);
        CHECK(b->strikes() == 4);
    }

    TEST_CASE("flow control: a reliable channel's unacked span is bounded in bytes") {
        ConnectionConfig cfg = fastConfig();
        cfg.maxWindowBytes = 4096;
        cfg.maxReorderBytes = 4 * (cfg.maxWindowBytes + cfg.maxJumboMessageBytes);
        NetSimLink blackhole;
        blackhole.lossPercent = 100.0; // nothing gets acked
        LinkedPair pair(cfg, fastConfig(), blackhole, {}, 3);
        for (int i = 0; i < 50; ++i) REQUIRE(pair.a().send(Channel::EventReliable, makePayload(i, 1000)) == SendResult::Ok);
        pair.tick(0.01);
        // 4 x 1,000 B fit the 4 KB span; the rest waits (resends of those 4 continue).
        CHECK(pair.a().stats().channels[channelId(Channel::EventReliable)].resends == 0);
        CHECK(pair.a().pendingReliableBytes() == 46 * 1000);
        pair.pipeAB().setLink({});
        std::vector<std::pair<Channel, std::vector<u8>>> got;
        for (int i = 0; i < 200 && got.size() < 50; ++i) {
            pair.tick(0.02);
            LinkedPair::drain(pair.b(), got);
        }
        CHECK(got.size() == 50);
        for (usize i = 0; i < got.size(); ++i) CHECK(payloadIndex(got[i].second) == i);
        ConnectionConfig bad = cfg;
        bad.maxReorderBytes = 1000;
        CHECK_FALSE(Connection::create(bad, [](void*, std::span<const u8>) {}, nullptr, 0.0));
    }

    TEST_CASE("a peer withholding one sequence cannot pin receiver memory") {
        // Receiver sized for peers with 2 KB windows and 2 KB messages: at most 16 KB buffered.
        ConnectionConfig cfg = fastConfig();
        cfg.maxWindowBytes = 2048;
        cfg.maxJumboMessageBytes = 2048;
        cfg.maxReorderBytes = 4 * (2048 + 2048);
        struct Null {
            static void send(void*, std::span<const u8>) {}
        };
        auto b = Connection::create(cfg, &Null::send, nullptr, 0.0, "B").value();
        RawCapture cap;
        reliable_config_t rc;
        reliable_default_config(&rc);
        rc.context = &cap;
        rc.transmit_packet_function = &RawCapture::transmit;
        rc.process_packet_function = &RawCapture::process;
        reliable_endpoint_t* raw = reliable_endpoint_create(&rc, 0.0);
        REQUIRE(raw);
        // EVENT_R messages 1..40 of 1,000 B each; message 0 is never sent.
        for (u16 seq = 1; seq <= 40; ++seq) {
            std::vector<u8> pkt(1100);
            const usize n = wire::writeMessage(pkt, Channel::EventReliable, seq, 0, 1, std::vector<u8>(1000, 0x77));
            pkt.resize(n);
            reliable_endpoint_send_packet(raw, pkt.data(), static_cast<int>(pkt.size()));
        }
        reliable_endpoint_destroy(raw);
        for (const auto& p : cap.packets) b->receivePacket(p, 0.0);
        CHECK(b->stats().reorderOverflows >= 3);
        CHECK(b->isMalformed());
        CHECK(b->inboxSize() == 0);
    }

    TEST_CASE("send budget caps wire bandwidth; stateBudgetBytes follows budget and backlog") {
        ConnectionConfig cfg = ConnectionConfig::server();
        cfg.aimd = false; // fixed 256 kbit/s
        cfg.unreliableMaxAge = 0.2;
        LinkedPair pair(cfg, fastConfig());
        CHECK(pair.a().stateBudgetBytes(20) == 256'000 / 8 / 20);
        // Offer 8x the budget of EVENT_U for 10 simulated seconds at 20 Hz.
        for (int tick = 0; tick < 200; ++tick) {
            for (int k = 0; k < 10; ++k) (void)pair.a().send(Channel::EventUnreliable, makePayload(tick * 10 + k, 1000));
            pair.tick(0.05, 5);
        }
        const ConnectionStats s = pair.a().stats();
        const f64 bitsPerSecond = static_cast<f64>(s.wireBytesSent) * 8.0 / 10.0;
        CHECK(bitsPerSecond <= 256'000 * 1.05);
        CHECK(bitsPerSecond >= 256'000 * 0.85);
        CHECK(s.channels[channelId(Channel::EventUnreliable)].dropped > 1000); // expired unsent
        // Reliable backlog reduces the replication allowance.
        for (int i = 0; i < 10; ++i) REQUIRE(pair.a().send(Channel::EventReliable, std::vector<u8>(100)) == SendResult::Ok);
        CHECK(pair.a().stateBudgetBytes(20) == 256'000 / 8 / 20 - 1000);
        pair.a().setBudgetBps(128'000);
        CHECK(pair.a().budgetBps() == 128'000);
    }

    TEST_CASE("packets of one flush are paced 2 ms apart (<= 4 per tick)") {
        ConnectionConfig cfg = ConnectionConfig::server(); // 4 packets per flush, 2 ms pacing
        cfg.aimd = false;
        cfg.budgetBps = 2'000'000;
        cfg.maxBudgetBps = 2'000'000;
        LinkedPair pair(cfg, fastConfig());
        for (int k = 0; k < 8; ++k) REQUIRE(pair.a().send(Channel::EventUnreliable, makePayload(k, 1100)) == SendResult::Ok);
        pair.a().flush(0.0);
        CHECK(pair.datagramsAB() == 1);
        pair.advance(0.0019);
        CHECK(pair.datagramsAB() == 1);
        pair.advance(0.0002);
        CHECK(pair.datagramsAB() == 2);
        pair.advance(0.002);
        CHECK(pair.datagramsAB() == 3);
        pair.advance(0.002);
        CHECK(pair.datagramsAB() == 4);
        pair.advance(0.010);
        CHECK(pair.datagramsAB() == 4); // the rest waits for the next tick
        pair.a().flush(pair.now());
        CHECK(pair.datagramsAB() == 5);
    }

    TEST_CASE("AIMD adapts the budget to a 128 kbit/s bottleneck and recovers") {
        NetSimLink bottleneck;
        bottleneck.latencyMs = 30.0;
        bottleneck.bandwidthBitsPerSecond = 128'000;
        bottleneck.queueLimitBytes = 6000;
        NetSimLink back;
        back.latencyMs = 30.0;
        LinkedPair pair(ConnectionConfig::server(), ConnectionConfig::client(), bottleneck, back, 909);
        u32 index = 0;
        const auto tick = [&] {
            // The replication writer fills exactly what the budget allows.
            usize budget = pair.a().stateBudgetBytes(20);
            while (budget >= 200) {
                (void)pair.a().sendState(makePayload(index++, 200));
                budget -= 200;
            }
            pair.tick(0.05, 5);
            pair.a().clearNotifications();
            pair.b().clearInbox();
        };
        for (int t = 0; t < 400; ++t) tick(); // 20 s
        const u32 congestedBudget = pair.a().budgetBps();
        INFO("budget under the bottleneck: " << congestedBudget);
        CHECK(congestedBudget < 200'000);
        CHECK(congestedBudget >= 64'000);
        CHECK(pair.a().stats().aimdDecreases >= 1);
        NetSimLink open;
        open.latencyMs = 30.0;
        pair.pipeAB().setLink(open);
        for (int t = 0; t < 600; ++t) tick(); // 30 s: 5 s hold + 16 kbit/s per second
        CHECK(pair.a().budgetBps() == 256'000);
    }

    TEST_CASE("stats: RTT, min RTT, jitter and loss track the link") {
        NetSimLink link;
        link.latencyMs = 50.0;
        link.lossPercent = 10.0;
        LinkedPair pair(fastConfig(), fastConfig(), link, link, 111);
        for (int i = 0; i < 1500; ++i) {
            (void)pair.a().send(Channel::EventUnreliable, makePayload(i, 50));
            (void)pair.b().send(Channel::EventUnreliable, makePayload(i, 50));
            pair.tick(0.02, 4);
            pair.a().clearInbox();
            pair.b().clearInbox();
        }
        const ConnectionStats s = pair.a().stats();
        CHECK(s.hasRtt);
        // 100 ms of path plus up to one ack delay (10 ms) and one update step (5 ms).
        CHECK(s.rttMs >= 95.0);
        CHECK(s.rttMs <= 125.0);
        CHECK(s.minRttMs >= 99.0);
        CHECK(s.minRttMs <= 116.0);
        CHECK(s.rtoMs >= 100.0);
        const f64 lossFraction = static_cast<f64>(s.packetsLost) / static_cast<f64>(s.packetsLost + s.packetsAcked);
        CHECK(lossFraction == doctest::Approx(0.10).epsilon(0.25));
        CHECK(s.lossPercent > 0.0);
        CHECK(s.lossPercent < 40.0);
        CHECK(s.reliableRttMs > 50.0);
        CHECK(s.sentKbps > 0.0f);

        NetSimLink jittery;
        jittery.latencyMs = 40.0;
        jittery.jitterMs = 15.0;
        LinkedPair jp(fastConfig(), fastConfig(), jittery, jittery, 112);
        for (int i = 0; i < 500; ++i) {
            (void)jp.a().send(Channel::EventUnreliable, makePayload(i, 50));
            jp.tick(0.02, 4);
            jp.b().clearInbox();
        }
        CHECK(jp.a().stats().jitterMs > 3.0);
        CHECK(pair.a().stats().jitterMs < jp.a().stats().jitterMs);
    }

    TEST_CASE("one-way traffic is acked by ack-only packets") {
        LinkedPair pair(fastConfig(), fastConfig());
        for (int i = 0; i < 100; ++i) {
            REQUIRE(pair.a().send(Channel::EventReliable, makePayload(i, 200)) == SendResult::Ok);
            pair.a().flush(pair.now());
            pair.advance(0.02, 4); // only A flushes; B sends nothing but acks
            pair.b().clearInbox();
        }
        CHECK(pair.a().pendingReliableBytes() == 0);
        CHECK(pair.a().stats().packetsAcked >= 90);
        CHECK(pair.b().stats().ackOnlyPackets > 0);
        CHECK(pair.a().stats().channels[channelId(Channel::EventReliable)].resends == 0);
        // Ack-only packets are not themselves acked (no ping-pong).
        CHECK(pair.a().stats().ackOnlyPackets == 0);
    }

    TEST_CASE("VOICE is sent on arrival with its own 40 kbit/s budget") {
        LinkedPair pair(fastConfig(), fastConfig());
        pair.advance(0.01);
        const std::vector<u8> frame(160, 0x33);
        REQUIRE(pair.a().send(Channel::Voice, frame) == SendResult::Ok);
        const u64 before = pair.datagramsAB();
        pair.advance(0.001); // update() only, no flush
        CHECK(pair.datagramsAB() == before + 1);
        Received got;
        LinkedPair::drain(pair.b(), got);
        REQUIRE(got.size() == 1);
        CHECK(got[0].first == Channel::Voice);
        // Token bucket: a burst beyond ~2.4 KB is refused.
        int ok = 0, limited = 0;
        for (int i = 0; i < 40; ++i) {
            const SendResult r = pair.a().send(Channel::Voice, frame);
            ok += r == SendResult::Ok;
            limited += r == SendResult::RateLimited;
        }
        CHECK(limited > 0);
        CHECK(ok < 15);
        CHECK(pair.a().stats().voiceRateLimited == static_cast<u64>(limited));
        pair.advance(0.001);
        CHECK(pair.a().stats().voicePackets >= 2);
    }

    TEST_CASE("unsent STATE expires with a 'lost' notification") {
        LinkedPair pair(fastConfig(), fastConfig());
        u64 seq = 0;
        REQUIRE(pair.a().sendState(makePayload(1, 100), &seq) == SendResult::Ok);
        pair.a().update(0.05);
        CHECK(pair.a().notifications().empty());
        pair.a().update(0.2); // > unreliableMaxAge (100 ms) without a flush
        REQUIRE(pair.a().notifications().size() == 1);
        CHECK(pair.a().notifications()[0].seq == seq);
        CHECK_FALSE(pair.a().notifications()[0].delivered);
    }

    TEST_CASE("inbound policing: 120 pps with a 240 burst") {
        ConnectionConfig sender = fastConfig();
        sender.maxPacketsPerFlush = 1;
        LinkedPair pair(sender, ConnectionConfig::server());
        // 1,000 pps for 1 s: 240 burst + ~120 refill pass, the rest is policed.
        for (int i = 0; i < 1000; ++i) {
            (void)pair.a().send(Channel::EventUnreliable, makePayload(i, 20));
            pair.a().flush(pair.now());
            pair.advance(0.001);
            pair.b().clearInbox();
        }
        const u64 policed = pair.b().stats().inboundRateLimited;
        CHECK(policed == doctest::Approx(1000 - 240 - 120).epsilon(0.05));
        CHECK(pair.b().strikes() == 0);
    }

    TEST_CASE("reorder buffer: messages arriving in any order within the window are delivered once, in order") {
        // A peer may legally send its window in any order (and repeat messages). Every permutation
        // must come out exactly once and in order.
        ConnectionConfig cfg = fastConfig();
        Random rng(4242);
        for (int round = 0; round < 3; ++round) {
            std::vector<u16> order(cfg.maxReliableInFlight);
            for (usize i = 0; i < order.size(); ++i) order[i] = static_cast<u16>(i);
            if (round == 0) std::reverse(order.begin(), order.end());
            else for (usize i = order.size() - 1; i > 0; --i) std::swap(order[i], order[rng.nextU32() % (i + 1)]);
            RawPacker packer;
            for (u16 seq : order) {
                packer.add(Channel::EventReliable, seq, indexBytes(seq));
                if (rng.nextU32() % 8 == 0) packer.add(Channel::EventReliable, seq, indexBytes(seq)); // duplicate
            }
            auto b = Connection::create(cfg, &nullSend, nullptr, 0.0, "B").value();
            for (const auto& p : packer.packets()) b->receivePacket(p, 0.0);
            REQUIRE(b->inboxSize() == order.size());
            for (usize i = 0; i < b->inboxSize(); ++i) REQUIRE(indexOf(b->inboxAt(i).payload) == i);
            CHECK(b->strikes() == 0);
        }
    }

    TEST_CASE("reorder buffer: a window sent in reverse costs O(log n) per message, not O(n)") {
        // Regression: out-of-order messages lived in a sorted vector, so a peer sending its window
        // in descending order paid O(n) moves per message (~200 us per packet at a 1,024 window,
        // unpenalised). Compare against in-order delivery of the same messages: an ordered map
        // stays within a small factor; the vector was >50x slower at this window size.
        ConnectionConfig cfg = fastConfig();
        cfg.maxReliableInFlight = 16384;
        const auto craft = [&](bool reverse) {
            RawPacker packer;
            const u32 n = cfg.maxReliableInFlight;
            for (u32 i = 0; i < n; ++i) {
                // Reverse: n-1, n-2, ..., 1 buffered, then 0 releases everything.
                const u16 seq = static_cast<u16>(reverse ? (i + 1 < n ? n - 1 - i : 0) : i);
                packer.add(Channel::EventReliable, seq, indexBytes(seq));
            }
            return packer.packets();
        };
        const auto cpuFor = [&](const std::vector<std::vector<u8>>& packets) {
            auto b = Connection::create(cfg, &nullSend, nullptr, 0.0, "B").value();
            const f64 cpu0 = os::threadCpuSeconds();
            for (const auto& p : packets) b->receivePacket(p, 0.0);
            const f64 cpu = os::threadCpuSeconds() - cpu0;
            REQUIRE(b->inboxSize() == cfg.maxReliableInFlight);
            for (usize i = 0; i < b->inboxSize(); ++i) REQUIRE(indexOf(b->inboxAt(i).payload) == i);
            return cpu;
        };
        const f64 inOrder = cpuFor(craft(false));
        const f64 reversed = cpuFor(craft(true));
        MESSAGE("16,384 reliable messages: in order " << inOrder * 1e3 << " ms CPU, reversed " << reversed * 1e3 << " ms CPU");
        CHECK(reversed < 25.0 * std::max(inOrder, 0.004)); // coarse clocks (Windows: 15.6 ms) read 0
    }

    TEST_CASE("acks: a gateway sends one packet per tick; reliable client data is still acked within 10 ms") {
        // Regression: every client packet (60 pps of INPUT) was acked by its own ack-only packet
        // 10 ms later, tripling the gateway's packet rate (80 pps instead of one per tick, 04 §2.5
        // and §2.6's 20-30 pps down) and spending ~28 kbit/s of the 256 kbit/s budget on acks.
        LinkedPair pair(ConnectionConfig::server(), ConnectionConfig::client());
        runTicks(pair, 5.0, 60.0, 20.0, 8, [](f64) {});
        const ConnectionStats sa = pair.a().stats();
        const ConnectionStats sb = pair.b().stats();
        INFO("gateway packets " << sa.packetsSent << ", ack-only " << sa.ackOnlyPackets << "; client packets "
                                << sb.packetsSent << ", INPUT resends " << sb.channels[channelId(Channel::Input)].resends);
        CHECK(sa.ackOnlyPackets == 0);
        CHECK(sa.packetsSent <= 5 * 20 + 2);
        CHECK(sb.ackOnlyPackets == 0); // the client's acks ride its 60 Hz tick packets
        // Tick-paced acks still trim INPUT redundancy: at most the 3 extra copies per input.
        CHECK(sb.channels[channelId(Channel::Input)].resends <= 3 * sb.channels[channelId(Channel::Input)].messagesSent);
        CHECK(sb.packetsLost == 0);
        CHECK(sa.packetsLost == 0);

        // A reliable message (RPC) right after a gateway tick is acked within 10 ms, not at the
        // next tick 50 ms later. The client takes RTT samples only from promptly acked (reliable)
        // packets, so its first sample is exactly this ack.
        REQUIRE_FALSE(sb.hasRtt);
        pair.advance(0.017); // one client tick later (the 60 pps cap allows the next packet)
        const u64 ackOnlyBefore = pair.a().stats().ackOnlyPackets;
        REQUIRE(pair.b().send(Channel::EventReliable, bytesOf("use ability")) == SendResult::Ok);
        pair.b().flush(pair.now());
        REQUIRE(pair.b().stats().packetRateDeferred == sb.packetRateDeferred);
        for (int step = 0; step < 40 && !pair.b().stats().hasRtt; ++step) pair.advance(0.001);
        REQUIRE(pair.b().stats().hasRtt);
        CHECK(pair.b().stats().rttMs <= 12.0);
        CHECK(pair.a().stats().ackOnlyPackets == ackOnlyBefore + 1);
    }

    TEST_CASE("acks: client RTT and loss detection are not distorted by the gateway's tick-paced acks") {
        NetSimLink link;
        link.latencyMs = 25.0; // RTT 50 ms
        LinkedPair pair(ConnectionConfig::server(), ConnectionConfig::client(), link, link, 17);
        // Gateway at 10 Hz: acks for INPUT-only packets wait up to 100 ms. The client also sends
        // an RPC every 250 ms (reliable, acked within 10 ms).
        f64 nextRpc = 0.0;
        runTicks(pair, 10.0, 60.0, 10.0, 8, [&](f64 now) {
            if (now >= nextRpc) {
                REQUIRE(pair.b().send(Channel::EventReliable, bytesOf("rpc")) == SendResult::Ok);
                nextRpc = now + 0.25;
            }
        });
        const ConnectionStats sb = pair.b().stats();
        const ConnectionStats sa = pair.a().stats();
        INFO("client srtt " << sb.rttMs << " ms, gateway srtt " << sa.rttMs << " ms");
        REQUIRE(sb.hasRtt);
        CHECK(sb.rttMs >= 49.0);
        CHECK(sb.rttMs <= 64.0); // path + <= 10 ms reliable ack delay + step granularity
        CHECK(sb.packetsLost == 0); // INPUT packets waiting for a lazy ack are not declared lost
        CHECK(sb.spuriousLosses == 0);
        CHECK(sa.rttMs <= 75.0);   // client acks ride its 60 Hz ticks (<= 20 ms)
        CHECK(sa.packetsLost == 0);
        CHECK(sa.ackOnlyPackets <= 40); // at most one per RPC (40 in 10 s); none for INPUT
    }

    TEST_CASE("client upstream is capped at 60 pps even when ticking at 144 Hz (04 §2.5)") {
        // Regression: the client sent one packet per flush, so a 144 Hz client exceeded 60 pps and,
        // with small inputs, the gateway's 120 pps policing.
        LinkedPair pair(ConnectionConfig::server(), ConnectionConfig::client());
        std::vector<std::pair<Channel, std::vector<u8>>> got;
        runTicks(pair, 5.0, 144.0, 20.0, 8, [&](f64) { LinkedPair::drain(pair.a(), got); });
        pair.advance(0.02); // the last tick's inputs wait for the next permitted packet
        pair.b().flush(pair.now());
        pair.advance(0.001);
        LinkedPair::drain(pair.a(), got);
        const ConnectionStats sb = pair.b().stats();
        INFO("client packets " << sb.packetsSent << ", deferred flushes " << sb.packetRateDeferred);
        CHECK(sb.packetsSent <= 60 * 5 + 4);
        CHECK(sb.packetsSent >= 60 * 5 * 95 / 100);
        CHECK(sb.packetRateDeferred > 0);
        CHECK(pair.a().stats().inboundRateLimited == 0);
        // Each packet carries the last 4 unacked inputs, so every one of the 720 inputs arrives.
        usize inputs = 0;
        for (const auto& g : got) inputs += g.first == Channel::Input ? 1 : 0;
        CHECK(inputs == sb.channels[channelId(Channel::Input)].messagesSent);
    }

    TEST_CASE("client-link memory an authenticated peer can pin is bounded") {
        // Regression: reliable allocates a whole packet's reassembly buffer on a sequence's first
        // fragment. With 64 reassembly slots a peer pinned ~1.1 MB per session with 64 tiny
        // fragments (1,800 bytes on the wire); client links now keep 8 (<= 8 x 17 KB).
        for (const ConnectionConfig& cfg : {ConnectionConfig::server(), ConnectionConfig::client()}) {
            CHECK(cfg.fragmentReassemblyBufferSize == 8);
            CHECK(cfg.maxWindowBytes == 64u * 1024);
            CHECK(cfg.maxReorderBytes == 4 * (64u * 1024 + 16u * 1024));
        }
        const MemoryTag tag = registerMemoryTag("Net");
        auto b = Connection::create(ConnectionConfig::server(), &nullSend, nullptr, 0.0, "B").value();
        const i64 before = memoryTagStats(tag).liveBytes;
        for (u16 seq = 0; seq < 200; ++seq) {
            // Last fragment (16 of 17) of packet `seq`, 4 data bytes.
            const std::vector<u8> f = {1, static_cast<u8>(seq & 0xFF), static_cast<u8>(seq >> 8), 16, 16, 1, 2, 3, 4};
            b->receivePacket(f, 0.001 * seq);
        }
        const i64 pinned = memoryTagStats(tag).liveBytes - before;
        const i64 perReassembly = 9 + 17 * 1024 + 8;
        MESSAGE("200 stray fragments pinned " << pinned / 1024 << " KB");
        CHECK(pinned <= 8 * perReassembly + 4096);
        CHECK(b->inboxSize() == 0);
    }

    TEST_CASE("inbound policing: 64 kbit/s of game data and a separate 40 kbit/s VOICE bucket (04 §9)") {
        // Regression: one combined 104 kbit/s byte bucket let a client that sends no voice push
        // 104 kbit/s of game traffic upstream.
        ConnectionConfig sender = fastConfig();
        sender.voiceBudgetBps = 1'000'000; // the sender's own voice limit is not under test
        struct Delivered {
            usize game = 0;
            usize voice = 0;
            u64 policed = 0;
        };
        const auto run = [&](u32 gameBytesPerTick, u32 voiceBytesPerTick) {
            LinkedPair pair(sender, ConnectionConfig::server());
            usize game = 0, voice = 0;
            for (int tick = 0; tick < 500; ++tick) { // 10 s at 50 Hz
                if (gameBytesPerTick) REQUIRE(pair.a().send(Channel::EventUnreliable, makePayload(tick, gameBytesPerTick)) == SendResult::Ok);
                if (voiceBytesPerTick) REQUIRE(pair.a().send(Channel::Voice, makePayload(tick, voiceBytesPerTick)) == SendResult::Ok);
                pair.a().flush(pair.now());
                pair.advance(0.02);
                pair.b().forEachMessage([&](const Connection::ReceivedMessage& m) {
                    (m.channel == Channel::Voice ? voice : game) += m.payload.size();
                });
                pair.b().clearInbox();
            }
            return Delivered{game, voice, pair.b().stats().inboundRateLimited};
        };
        // 100 kbit/s of game data, no voice: policed down to 64 kbit/s (+ the 32 KB burst).
        const Delivered gameOnly = run(250, 0);
        INFO("game only: delivered " << gameOnly.game << " B in 10 s, policed " << gameOnly.policed);
        CHECK(gameOnly.policed > 0);
        CHECK(gameOnly.game <= 10 * 8000 + 32 * 1024);
        CHECK(gameOnly.game >= 10 * 8000 * 85 / 100);
        CHECK(gameOnly.voice == 0);
        // 56 kbit/s of game data plus 36 kbit/s of voice: both inside their own buckets.
        const Delivered both = run(140, 90);
        INFO("game + voice: delivered " << both.game << " + " << both.voice << " B, policed " << both.policed);
        CHECK(both.policed == 0);
        CHECK(both.game == 500u * 140);
        CHECK(both.voice == 500u * 90);
    }
}
