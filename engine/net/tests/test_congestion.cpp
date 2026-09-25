// Token buckets, RTT estimator, loss window and the AIMD budget controller (04 §2.5).

#include <doctest/doctest.h>

#include "helios/net/congestion.h"

using namespace helios;
using namespace helios::net;

TEST_SUITE("net.congestion") {
    TEST_CASE("token bucket refills, caps and borrows") {
        TokenBucket b(1000.0, 500.0, 0.0);
        CHECK(b.tokens() == doctest::Approx(500.0));
        CHECK(b.tryConsume(400.0));
        CHECK_FALSE(b.tryConsume(200.0));
        b.refill(0.1); // +100
        CHECK(b.tokens() == doctest::Approx(200.0));
        b.refill(10.0);
        CHECK(b.tokens() == doctest::Approx(500.0)); // capped
        b.consume(1500.0);                           // an oversized packet borrows
        CHECK(b.tokens() == doctest::Approx(-1000.0));
        b.refill(10.5);
        CHECK(b.tokens() == doctest::Approx(-500.0));
        b.refill(10.4); // time going backwards is ignored
        CHECK(b.tokens() == doctest::Approx(-500.0));
        b.setRate(10.0, 5.0);
        CHECK(b.capacity() == doctest::Approx(5.0));
    }

    TEST_CASE("RTT estimator follows RFC 6298") {
        RttEstimator r;
        CHECK_FALSE(r.hasSample());
        CHECK(r.rto() == doctest::Approx(0.25));
        r.addSample(0.100, 0.0);
        CHECK(r.srtt() == doctest::Approx(0.100));
        CHECK(r.rttvar() == doctest::Approx(0.050));
        CHECK(r.rto() == doctest::Approx(0.300));
        for (int i = 0; i < 200; ++i) r.addSample(0.100, 0.01 * i);
        CHECK(r.srtt() == doctest::Approx(0.100).epsilon(0.001));
        CHECK(r.rttvar() < 0.001);
        CHECK(r.rto() == doctest::Approx(0.100).epsilon(0.02)); // srtt + 4 * ~0
        // Clamps.
        RttEstimator fast;
        for (int i = 0; i < 50; ++i) fast.addSample(0.001, i * 0.01);
        CHECK(fast.rto() == doctest::Approx(0.030)); // 30 ms floor
        RttEstimator slow;
        slow.addSample(5.0, 0.0);
        CHECK(slow.rto() == doctest::Approx(1.0)); // 1 s ceiling
        r.addSample(-1.0, 3.0);                    // invalid samples are ignored
        CHECK(r.latest() == doctest::Approx(0.100));
    }

    TEST_CASE("min RTT is windowed and jitter tracks variation") {
        RttEstimator r;
        r.addSample(0.050, 0.0);
        for (int i = 1; i <= 100; ++i) r.addSample(i % 2 ? 0.080 : 0.120, i * 0.05); // 5 s
        CHECK(r.minRtt() == doctest::Approx(0.050));
        CHECK(r.jitter() > 0.02);
        // Two windows (10 s each) later the old 50 ms minimum is forgotten.
        for (int i = 0; i < 500; ++i) r.addSample(0.200, 5.0 + i * 0.05);
        CHECK(r.minRtt() == doctest::Approx(0.200));
    }

    TEST_CASE("loss window covers the last second") {
        LossWindow w;
        for (int i = 0; i < 100; ++i) w.record(i % 10 == 0, i * 0.01); // 10 % over 1 s
        CHECK(w.lossRate(0.99) == doctest::Approx(0.10).epsilon(0.05));
        CHECK(w.samples(0.99) == 100);
        CHECK(w.lossRate(3.0) == doctest::Approx(0.0));
        CHECK(w.samples(3.0) == 0);
        for (int i = 0; i < 40; ++i) w.record(false, 3.0 + i * 0.025);
        CHECK(w.lossRate(3.99) == doctest::Approx(0.0));
    }

    TEST_CASE("AIMD: multiplicative decrease under loss, floor, additive increase") {
        AimdController::Config cfg;
        AimdController aimd(cfg);
        RttEstimator rtt;
        rtt.addSample(0.05, 0.0);
        CHECK(aimd.budgetBps() == 256'000);
        // Updates every 50 ms; t = i * 0.05 (integer steps keep the timeline exact).
        int i = 0;
        const auto t = [&] { return i * 0.05; };
        // 10 % loss sustained: one decrease per second of congestion.
        for (; i <= 20; ++i) aimd.update(t(), 0.10, 100, rtt);
        CHECK(aimd.budgetBps() == 179'200); // x0.7 after 1 s
        for (; i <= 40; ++i) aimd.update(t(), 0.10, 100, rtt);
        CHECK(aimd.budgetBps() == 125'440);
        CHECK(aimd.decreases() == 2);
        // Keeps falling to the 64 kbit/s floor and stays there.
        for (; i <= 200; ++i) aimd.update(t(), 0.10, 100, rtt);
        CHECK(aimd.budgetBps() == 64'000);
        // Loss below the threshold, or too few samples, is not congestion.
        const int cleanStart = i;
        for (; i < cleanStart + 100; ++i) aimd.update(t(), 0.02, 100, rtt);
        CHECK(aimd.budgetBps() == 64'000); // 5 s hold before increasing
        for (; i <= cleanStart + 140; ++i) aimd.update(t(), 0.20, 3, rtt);
        CHECK(aimd.budgetBps() == doctest::Approx(96'000).epsilon(0.01)); // +16 kbit/s per second
        for (; i < cleanStart + 600; ++i) aimd.update(t(), 0.0, 100, rtt);
        CHECK(aimd.budgetBps() == 256'000); // capped
    }

    TEST_CASE("AIMD: queueing delay counts as congestion; battle ceiling") {
        AimdController aimd;
        RttEstimator rtt;
        rtt.addSample(0.040, 0.0);
        int i = 0;
        const auto t = [&] { return i * 0.05; };
        for (; i < 10; ++i) aimd.update(t(), 0.0, 100, rtt);
        CHECK_FALSE(aimd.congested());
        for (int k = 0; k < 60; ++k) rtt.addSample(0.200, t()); // srtt > min RTT + 100 ms
        for (; i <= 35; ++i) aimd.update(t(), 0.0, 100, rtt);
        CHECK(aimd.congested());
        CHECK(aimd.budgetBps() == 179'200);
        aimd.setMaxBps(512'000);
        aimd.setBudget(1'000'000, t());
        CHECK(aimd.budgetBps() == 512'000);
        aimd.setMaxBps(128'000);
        CHECK(aimd.budgetBps() == 128'000);
    }
}
