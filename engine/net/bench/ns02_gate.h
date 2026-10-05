#pragma once
// NS-0.2 verdicts for `net_bench --gate` (04 §11.4: "Loopback 100k pps/core without loss"), shared with
// net_tests so that the advisory mode is tested. Header-only; test/bench code, not part of helios_net.
// Pure functions of their arguments: safe from any thread.

#include <string_view>

#include "helios/core/types.h"

namespace helios::net::bench {

/// NS-0.2's rate, for raw datagrams and for encrypted HTP packets alike.
inline constexpr f64 kNs02PerCore = 100'000.0;

/// The same threshold as a CPU budget: 10 us of one core per packet, send and receive together, for the
/// whole encrypted stack (netcode's AEAD, reliable, HTP channels, the transport and its system calls).
inline constexpr f64 kNs02BudgetMicrosPerPacket = 1e6 / kNs02PerCore;

/// Cross-check of a per-thread CPU figure against the whole machine. A thread's CPU time misses work the OS
/// does for it elsewhere: interrupts, deferred procedure calls, system threads completing its I/O on other
/// CPUs. Wall time cannot show that: the bench loops never sleep or block, so their thread accrues CPU at the
/// wall-clock rate whether or not such work exists, and CPU below wall shows only preemption. The machine's
/// busy CPU over the run, less the background load measured on either side of it, does include that work;
/// its excess over the thread's CPU is reported, and a run is flagged (a warning, not a gate) when the excess
/// is above kOffThreadLimitPercent of the thread's CPU on a host idle enough for the figure to mean anything.
inline constexpr f64 kOffThreadLimitPercent = 25.0;
/// The background load the cross-check accepts: at most a quarter of the logical CPUs busy without the run.
inline constexpr f64 kCrossCheckIdleFraction = 0.25;

struct MachineCrossCheck {
    bool valid = false;          ///< both samples available and the host idle enough
    f64 machineCpuSeconds = 0.0; ///< the machine's busy CPU during the run, background removed
    f64 offThreadPercent = 0.0;  ///< (machine - thread) / thread x 100; around 0 when all work is on the thread
    bool flagged = false;        ///< valid and offThreadPercent above kOffThreadLimitPercent
};

/// The cross-check of one run: `threadCpuSeconds` of the measuring thread, `machineBusySeconds` of every CPU
/// over the same `wallSeconds`, and `backgroundCores` busy without the run (measured just before and after
/// it), on a machine of `cpus` logical CPUs. Pure: safe from any thread.
constexpr MachineCrossCheck machineCrossCheck(f64 threadCpuSeconds, f64 machineBusySeconds, f64 wallSeconds,
                                              f64 backgroundCores, u32 cpus) {
    MachineCrossCheck c;
    if (cpus == 0 || threadCpuSeconds <= 0.0 || wallSeconds <= 0.0 || backgroundCores < 0.0) return c;
    c.machineCpuSeconds = machineBusySeconds - backgroundCores * wallSeconds;
    c.offThreadPercent = (c.machineCpuSeconds - threadCpuSeconds) * 100.0 / threadCpuSeconds;
    c.valid = backgroundCores <= kCrossCheckIdleFraction * static_cast<f64>(cpus);
    c.flagged = c.valid && c.offThreadPercent > kOffThreadLimitPercent;
    return c;
}

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
