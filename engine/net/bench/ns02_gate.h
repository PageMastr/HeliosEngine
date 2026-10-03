#pragma once
// NS-0.2 verdicts for `net_bench --gate` (04 §11.4: "Loopback 100k pps/core without loss"), shared with
// net_tests so that the advisory mode is tested. Header-only; test/bench code, not part of helios_net.
// Pure functions of their arguments: safe from any thread.

#include <string_view>

#include "helios/core/types.h"

namespace helios::net::bench {

/// NS-0.2's rate, for raw datagrams and for encrypted HTP packets alike.
inline constexpr f64 kNs02PerCore = 100'000.0;

/// The only threshold `net_bench --advisory` accepts: the encrypted-stack rate on hosted runners.
inline constexpr std::string_view kNs02StackAdvisoryName = "ns02-stack";

/// Why that rate may be advisory. Hosted runners' resources vary, so the level they measure does too
/// (88k to 132k packets per core on hosted Linux with no code change); the repository owner passed NS-0.2
/// on 2026-09-30 for the hosted Linux nightly, with a re-test on fixed hardware owed (WP-0.4).
inline constexpr std::string_view kNs02StackAdvisoryReason =
    "owner approval 2026-09-30, evidence docs/evidence/ns-0.2-owner-approval-2026-09-30.md";

enum class Ns02Verdict {
    Pass,          ///< at least 100k per core, nothing lost
    Fail,          ///< loss, a stack that never ran, or below 100k with the rate gated
    AdvisoryBelow, ///< below 100k with the rate advisory; nothing lost
};

/// The encrypted-stack verdict. Loss and a stack that sent nothing always fail: only the rate can be
/// advisory (`rateAdvisory`, set by `--advisory ns02-stack`), never "without loss".
constexpr Ns02Verdict ns02StackVerdict(u64 sent, u64 delivered, f64 packetsPerCore, bool rateAdvisory) {
    if (sent == 0 || delivered != sent) return Ns02Verdict::Fail;
    if (packetsPerCore >= kNs02PerCore) return Ns02Verdict::Pass;
    return rateAdvisory ? Ns02Verdict::AdvisoryBelow : Ns02Verdict::Fail;
}

/// The "NS-0.2 advisory:" line an advisory run logs after the stack's verdict.
enum class Ns02AdvisoryNote {
    None,     ///< nothing: the rate is gated, or the stack failed on loss or never sent
    Below,    ///< WARN: below 100k, reported and not failing
    NotGated, ///< INFO: at or above 100k, but the rate was not gated on this run
};

/// Which advisory line follows a verdict. A failed stack gets only its failure line, so a log never says
/// both "FAILED" and "not gated" about the same run.
constexpr Ns02AdvisoryNote ns02AdvisoryNote(Ns02Verdict verdict, bool rateAdvisory) {
    if (!rateAdvisory || verdict == Ns02Verdict::Fail) return Ns02AdvisoryNote::None;
    return verdict == Ns02Verdict::AdvisoryBelow ? Ns02AdvisoryNote::Below : Ns02AdvisoryNote::NotGated;
}

} // namespace helios::net::bench
