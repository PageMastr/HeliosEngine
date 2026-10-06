// net_bench's NS-0.2 encrypted-stack verdict (bench/ns02_gate.h): `--advisory ns02-stack` makes only the
// 100k-per-core rate advisory, never "without loss", and without it every level still gates. The advisory
// line that follows the verdict is checked too, and so is the machine-wide CPU cross-check of a run
// (bench::machineCrossCheck and the os::machineCpuTimes() counter it reads).

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "helios/core/time.h"
#include "ns02_gate.h"
#include "platform/net_os.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::bench;

namespace {

TEST_SUITE("net.bench") {
    TEST_CASE("NS-0.2 stack verdict: the rate gates by default and only the rate can be advisory") {
        // Gated (a local or lab run): below 100k fails, at or above passes.
        CHECK(ns02StackVerdict(882'432, 882'432, 88'248.0, false) == Ns02Verdict::Fail);
        CHECK(ns02StackVerdict(1'320'320, 1'320'320, 132'039.0, false) == Ns02Verdict::Pass);
        CHECK(ns02StackVerdict(1'000'000, 1'000'000, kNs02PerCore, false) == Ns02Verdict::Pass);
        // Advisory (the hosted nightly): below 100k is reported, at or above still passes.
        CHECK(ns02StackVerdict(882'432, 882'432, 88'248.0, true) == Ns02Verdict::AdvisoryBelow);
        CHECK(ns02StackVerdict(1'320'320, 1'320'320, 132'039.0, true) == Ns02Verdict::Pass);
        // Loss, and a stack that never sent (it did not connect), fail in both modes and at any rate.
        for (const bool advisory : {false, true}) {
            CHECK(ns02StackVerdict(882'432, 882'431, 88'248.0, advisory) == Ns02Verdict::Fail);
            CHECK(ns02StackVerdict(1'320'320, 1'320'319, 132'039.0, advisory) == Ns02Verdict::Fail);
            CHECK(ns02StackVerdict(0, 0, 0.0, advisory) == Ns02Verdict::Fail);
        }
        CHECK(kNs02StackAdvisoryName == "ns02-stack");
        CHECK(kNs02StackAdvisoryReason.find("docs/evidence/ns-0.2-owner-approval-2026-09-30.md") !=
              std::string_view::npos);
    }

    TEST_CASE("NS-0.2 advisory note: a failed stack is never also reported as not gated") {
        // Advisory run: below 100k warns, at or above says the rate was not gated, a failure says neither.
        CHECK(ns02AdvisoryNote(Ns02Verdict::AdvisoryBelow, true) == Ns02AdvisoryNote::Below);
        CHECK(ns02AdvisoryNote(Ns02Verdict::Pass, true) == Ns02AdvisoryNote::NotGated);
        CHECK(ns02AdvisoryNote(Ns02Verdict::Fail, true) == Ns02AdvisoryNote::None);
        const Ns02Verdict lost = ns02StackVerdict(882'432, 882'431, 88'248.0, true);
        const Ns02Verdict neverSent = ns02StackVerdict(0, 0, 0.0, true);
        CHECK(ns02AdvisoryNote(lost, true) == Ns02AdvisoryNote::None);
        CHECK(ns02AdvisoryNote(neverSent, true) == Ns02AdvisoryNote::None);
        // Gated run: no advisory line at all.
        for (const Ns02Verdict v : {Ns02Verdict::Pass, Ns02Verdict::Fail, Ns02Verdict::AdvisoryBelow}) {
            CHECK(ns02AdvisoryNote(v, false) == Ns02AdvisoryNote::None);
        }
    }

    TEST_CASE("machine cross-check: work outside the measuring thread is reported and flagged above 25 %") {
        // 10 s of thread CPU over 10 s of wall time on a 12-CPU host with 0.1 cores of background (1 s).
        // All of the run's work on the thread: the machine spent 11 s, so nothing is outside it.
        MachineCrossCheck c = machineCrossCheck(10.0, 11.0, 10.0, 0.1, 12);
        CHECK(c.valid);
        CHECK(c.machineCpuSeconds == doctest::Approx(10.0));
        CHECK(c.offThreadPercent == doctest::Approx(0.0));
        CHECK_FALSE(c.flagged);
        // A system worker completing the thread's I/O on another CPU for 4 s: the thread's figure and the wall
        // time look exactly the same, the machine's does not.
        c = machineCrossCheck(10.0, 15.0, 10.0, 0.1, 12);
        CHECK(c.machineCpuSeconds == doctest::Approx(14.0));
        CHECK(c.offThreadPercent == doctest::Approx(40.0));
        CHECK(c.flagged);
        // At the limit: reported, not flagged; just above it: flagged.
        CHECK_FALSE(machineCrossCheck(10.0, 13.5, 10.0, 0.1, 12).flagged);
        CHECK(machineCrossCheck(10.0, 13.6, 10.0, 0.1, 12).flagged);
        // Preemption (CPU below wall) is not off-thread work.
        c = machineCrossCheck(8.0, 9.0, 10.0, 0.1, 12);
        CHECK(c.offThreadPercent == doctest::Approx(0.0));
        CHECK_FALSE(c.flagged);
        // A busy host (background above a quarter of the CPUs): the figures are printed but mean nothing,
        // so nothing is flagged.
        c = machineCrossCheck(10.0, 40.0, 10.0, 2.9, 4);
        CHECK_FALSE(c.valid);
        CHECK_FALSE(c.flagged);
        CHECK(machineCrossCheck(10.0, 40.0, 10.0, 1.0, 4).valid);
        // No counter, no CPU or no time: invalid.
        CHECK_FALSE(machineCrossCheck(10.0, 11.0, 10.0, 0.1, 0).valid);
        CHECK_FALSE(machineCrossCheck(0.0, 11.0, 10.0, 0.1, 12).valid);
        CHECK_FALSE(machineCrossCheck(10.0, 11.0, 0.0, 0.1, 12).valid);
        CHECK_FALSE(machineCrossCheck(10.0, 11.0, 10.0, -1.0, 12).valid);
    }

    TEST_CASE("machine CPU counter: sees CPU that another thread spends, which the measuring thread's does not") {
        const os::MachineCpuTimes probe = os::machineCpuTimes();
#if defined(_WIN32) || defined(__linux__)
        REQUIRE(probe.ok);
#else
        if (!probe.ok) return; // no machine-wide counter on this OS; net_bench says so
#endif
        CHECK(probe.cpus >= 1);
        CHECK(probe.busySeconds > 0.0);
        // This thread and a worker each burn 0.3 s of their own CPU. The thread's counter sees only its own
        // share; the machine's sees both (and any other load, so it is checked from below only).
        constexpr f64 kBurn = 0.3;
        const auto burn = [] {
            const f64 start = os::threadCpuSeconds();
            const f64 deadline = monotonicSeconds() + 10.0;
            volatile u64 sink = 0;
            while (os::threadCpuSeconds() - start < kBurn && monotonicSeconds() < deadline) sink = sink + 1;
            return os::threadCpuSeconds() - start;
        };
        std::atomic<f64> workerCpu{0.0};
        const os::MachineCpuTimes before = os::machineCpuTimes();
        const f64 cpu0 = os::threadCpuSeconds();
        std::thread worker([&] { workerCpu.store(burn()); });
        (void)burn();
        worker.join();
        const f64 threadCpu = os::threadCpuSeconds() - cpu0;
        const os::MachineCpuTimes after = os::machineCpuTimes();
        const f64 machine = after.busySeconds - before.busySeconds;
        MESSAGE("thread " << threadCpu << " s, worker " << workerCpu.load() << " s, machine " << machine << " s on "
                          << after.cpus << " CPUs");
        REQUIRE(workerCpu.load() >= 0.8 * kBurn);
        // Tick-sampled counters (10 ms per CPU at 100 Hz on Linux, 15.6 ms on Windows) need some slack.
        CHECK(machine >= 0.8 * (threadCpu + workerCpu.load()));
        CHECK(machine >= threadCpu + 0.5 * workerCpu.load());
    }
}

} // namespace
