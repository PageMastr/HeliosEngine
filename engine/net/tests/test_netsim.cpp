// NetSim: deterministic latency, jitter, loss (Bernoulli + Gilbert-Elliott), duplication,
// reordering and bandwidth caps (04 §10.1).

#include <doctest/doctest.h>

#include <vector>

#include "net_test_util.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::test;

namespace {

struct Delivered {
    f64 time;
    u32 index;
};

/// Pushes `count` datagrams (one every `interval`), drains until empty, returns deliveries.
std::vector<Delivered> run(NetSimPipe& pipe, u32 count, f64 interval, usize size = 100) {
    std::vector<Delivered> out;
    u8 buf[kMaxDatagramBytes];
    Address peer;
    f64 t = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const auto p = makePayload(i, size);
        pipe.push(t, Address::loopbackV4(1), p);
        while (usize n = pipe.pop(t, peer, buf)) out.push_back({t, payloadIndex(std::span<const u8>(buf, n))});
        t += interval;
    }
    for (int k = 0; k < 100000 && pipe.pending() > 0; ++k) {
        t += 0.001;
        while (usize n = pipe.pop(t, peer, buf)) out.push_back({t, payloadIndex(std::span<const u8>(buf, n))});
    }
    return out;
}

} // namespace

TEST_SUITE("net.netsim") {
    TEST_CASE("latency delays every datagram; order is kept without jitter") {
        NetSimLink link;
        link.latencyMs = 50.0;
        NetSimPipe pipe(link, 1);
        const auto d = run(pipe, 100, 0.01);
        REQUIRE(d.size() == 100);
        for (usize i = 0; i < d.size(); ++i) {
            CHECK(d[i].index == i);
            CHECK(d[i].time == doctest::Approx(static_cast<f64>(i) * 0.01 + 0.050).epsilon(0.02));
        }
        CHECK(pipe.stats().delivered == 100);
    }

    TEST_CASE("Bernoulli loss rate and determinism") {
        NetSimLink link;
        link.lossPercent = 10.0;
        NetSimPipe a(link, 42), b(link, 42), c(link, 43);
        const auto da = run(a, 20000, 0.0005);
        const auto db = run(b, 20000, 0.0005);
        const auto dc = run(c, 20000, 0.0005);
        CHECK(da.size() == doctest::Approx(18000).epsilon(0.03));
        CHECK(a.stats().lost == 20000 - da.size());
        REQUIRE(da.size() == db.size()); // same seed: identical run
        for (usize i = 0; i < da.size(); ++i) CHECK(da[i].index == db[i].index);
        CHECK(da.size() != dc.size()); // different seed: different run (overwhelmingly likely)
    }

    TEST_CASE("jitter and explicit reordering reorder; duplication duplicates") {
        NetSimLink link;
        link.latencyMs = 20.0;
        link.jitterMs = 10.0;
        NetSimPipe jittered(link, 7);
        const auto d = run(jittered, 1000, 0.002);
        CHECK(d.size() == 1000);
        usize inversions = 0;
        for (usize i = 1; i < d.size(); ++i) inversions += d[i].index < d[i - 1].index ? 1 : 0;
        CHECK(inversions > 50);
        for (const auto& x : d) CHECK(x.time >= x.index * 0.002 + 0.0099); // never below latency - jitter

        NetSimLink reorder;
        reorder.reorderPercent = 20.0;
        reorder.reorderDelayMs = 15.0;
        NetSimPipe r(reorder, 9);
        const auto dr = run(r, 1000, 0.001);
        CHECK(dr.size() == 1000);
        CHECK(r.stats().reordered == doctest::Approx(200).epsilon(0.25));
        inversions = 0;
        for (usize i = 1; i < dr.size(); ++i) inversions += dr[i].index < dr[i - 1].index ? 1 : 0;
        CHECK(inversions > 100);

        NetSimLink dup;
        dup.duplicatePercent = 25.0;
        NetSimPipe du(dup, 11);
        const auto dd = run(du, 2000, 0.001);
        CHECK(dd.size() == 2000 + du.stats().duplicated);
        CHECK(du.stats().duplicated == doctest::Approx(500).epsilon(0.15));
    }

    TEST_CASE("Gilbert-Elliott produces bursts") {
        NetSimLink link;
        link.burstLoss = true;
        link.burstEnterPercent = 1.0;
        link.burstExitPercent = 20.0; // mean burst length 5
        link.burstLossPercent = 100.0;
        NetSimPipe pipe(link, 5);
        const auto d = run(pipe, 50000, 0.0001);
        std::vector<bool> got(50000, false);
        for (const auto& x : d) got[x.index] = true;
        usize bursts = 0, lostTotal = 0, run2plus = 0, current = 0;
        for (bool g : got) {
            if (!g) {
                ++current;
                ++lostTotal;
            } else if (current > 0) {
                ++bursts;
                if (current >= 2) ++run2plus;
                current = 0;
            }
        }
        REQUIRE(bursts > 0);
        const f64 meanBurst = static_cast<f64>(lostTotal) / static_cast<f64>(bursts);
        CHECK(meanBurst > 3.0); // Bernoulli loss at the same rate would give ~1.05
        CHECK(pipe.stats().burstLost == lostTotal);
        CHECK(run2plus > bursts / 2);
    }

    TEST_CASE("bandwidth cap serialises and tail-drops") {
        NetSimLink link;
        link.bandwidthBitsPerSecond = 800'000; // 100 KB/s
        link.queueLimitBytes = 10'000;
        NetSimPipe pipe(link, 3);
        // Offer 1,000-byte datagrams at 200 KB/s for 1 s: about half must be dropped.
        const auto d = run(pipe, 200, 0.005, 1000);
        const u64 drops = pipe.stats().queueDrops;
        CHECK(d.size() + drops == 200);
        CHECK(d.size() == doctest::Approx(110).epsilon(0.1)); // 100 in 1 s + the queue
        // Delivery rate never exceeds the cap.
        const f64 span = d.back().time - d.front().time;
        CHECK(static_cast<f64>(d.size() - 1) * 1000.0 * 8.0 / span <= 800'000.0 * 1.02);
    }

    TEST_CASE("NetSimTransport: pass-through, profiles and runtime changes") {
        VirtualNetwork net;
        auto a = net.bind(Address::ipv4(1, 1, 1, 1, 1)).value();
        auto bSock = net.bind(Address::ipv4(2, 2, 2, 2, 2)).value();
        VirtualNetwork::Socket* b = bSock.get();
        NetSimTransport sim(std::move(a), NetSimConfig{});
        const u8 data[] = {1, 2, 3};
        sim.update(0.0);
        sim.send(b->localAddress(), data);
        CHECK(b->pending() == 1); // inactive link: immediate
        u8 buf[16];
        Address from;
        CHECK(b->receive(from, buf) == 3);
        CHECK(from == Address::ipv4(1, 1, 1, 1, 1));

        NetSimConfig slow;
        slow.outbound.latencyMs = 100.0;
        sim.setConfig(slow);
        sim.send(b->localAddress(), data);
        sim.flush();
        CHECK(b->pending() == 0);
        sim.update(0.099);
        CHECK(b->pending() == 0);
        sim.update(0.1001);
        CHECK(b->pending() == 1);

        // Inbound impairment delays what the socket already received.
        NetSimConfig in;
        in.inbound.latencyMs = 30.0;
        sim.setConfig(in);
        b->send(sim.localAddress(), data);
        sim.update(0.2);
        CHECK(sim.receive(from, buf) == 0);
        sim.update(0.2301);
        CHECK(sim.receive(from, buf) == 3);

        for (auto p : {NetSimProfile::Lan, NetSimProfile::Good, NetSimProfile::Mobile, NetSimProfile::Awful}) {
            CHECK(parseNetSimProfile(netSimProfileName(p)) == p);
        }
        CHECK_FALSE(parseNetSimProfile("bogus"));
        const NetSimConfig good = makeNetSimProfile(NetSimProfile::Good);
        CHECK(good.outbound.latencyMs + good.inbound.latencyMs == doctest::Approx(40.0)); // 40 ms RTT
        CHECK(good.outbound.lossPercent == doctest::Approx(0.1));
        CHECK_FALSE(makeNetSimProfile(NetSimProfile::Lan).outbound.active());
        const NetSimConfig mobile = makeNetSimProfile(NetSimProfile::Mobile);
        CHECK(mobile.outbound.latencyMs * 2 == doctest::Approx(120.0));
        CHECK(mobile.outbound.jitterMs * 2 == doctest::Approx(30.0));
        CHECK(makeNetSimProfile(NetSimProfile::Awful).outbound.lossPercent == doctest::Approx(5.0));
    }

    TEST_CASE("VirtualNetwork routing, ephemeral ports, queue limits and inject") {
        VirtualNetwork net;
        auto s1 = net.bind(Address::ipv4(10, 0, 0, 1, 0)).value();
        auto s2 = net.bind(Address::ipv4(10, 0, 0, 1, 0)).value();
        CHECK(s1->localAddress().port() != 0);
        CHECK(s1->localAddress() != s2->localAddress());
        CHECK_FALSE(net.bind(s1->localAddress()).hasValue()); // taken
        const u8 d[] = {9};
        s1->send(Address::ipv4(9, 9, 9, 9, 9), d);
        CHECK(s1->stats().droppedNoRoute == 1);
        auto tiny = net.bind(Address::ipv6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}, 5), 2).value();
        for (int i = 0; i < 3; ++i) s1->send(tiny->localAddress(), d);
        CHECK(s1->stats().droppedQueueFull == 1);
        CHECK(tiny->pending() == 2);
        CHECK(net.inject(Address::ipv4(6, 6, 6, 6, 6), s2->localAddress(), d));
        u8 buf[4];
        Address from;
        CHECK(s2->receive(from, buf) == 1);
        CHECK(from == Address::ipv4(6, 6, 6, 6, 6));
        const Address freed = s2->localAddress();
        s2.reset();
        CHECK(net.bind(freed).hasValue()); // unbound on destruction
    }
}
