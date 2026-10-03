// net_bench's NS-0.2 encrypted-stack verdict (bench/ns02_gate.h): `--advisory ns02-stack` makes only the
// 100k-per-core rate advisory, never "without loss", and without it every level still gates. The advisory
// line that follows the verdict is checked too.

#include <doctest/doctest.h>

#include "ns02_gate.h"

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
}

} // namespace
