// Process start-up checks (02 §1.1 "Proof that it ran"): see helios/core/platform_init.h.
#include "helios/core/platform_init.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include "helios/core/log.h"
#include "platform/os.h"

namespace helios::core {
namespace {

// What is wrong with `verdict`, or empty for Pass. Static text, so platformInit() allocates nothing to
// report it while its writes succeed. A failed write (stderr a closed pipe, say) returns an Error that
// holds a std::string; if that threw bad_alloc, noexcept would std::terminate instead of exiting with
// kPlatformInitExitCode. That takes an out-of-memory process at start-up, which ends either way.
std::string_view cpuGateProblem(CpuGateVerdict verdict) noexcept {
    switch (verdict) {
    case CpuGateVerdict::Pass: return {};
    case CpuGateVerdict::Fail:
        return "CPU gate did not run to completion: its pre-initializer refused this CPU but the process "
               "continued (02 §1.1)";
    case CpuGateVerdict::NotRun: break;
    }
    return "CPU gate did not run: this executable carries no CPU-gate pre-initializer, or it ran too late "
           "(link the image with helios_executable() and a gate role; 02 §1.1)";
}

} // namespace

Result<void> checkCpuGateVerdict(CpuGateVerdict verdict) {
    const std::string_view problem = cpuGateProblem(verdict);
    if (problem.empty()) return {};
    return Error{ErrorCode::InvalidState, std::string(problem)};
}

void platformInit() noexcept {
    const std::string_view problem = cpuGateProblem(cpuGateVerdict());
    if (problem.empty()) return;
    // Straight to the stderr handle: the log may have no console sink, and this must reach a smoke test.
    if (const NativeHandle err = os::standardHandle(2); err != kInvalidNativeHandle) {
        constexpr std::string_view kPrefix = "Helios: ";
        (void)os::pipeWrite(err, kPrefix.data(), kPrefix.size());
        (void)os::pipeWrite(err, problem.data(), problem.size());
        (void)os::pipeWrite(err, "\n", 1);
    }
    log::flush();
    std::_Exit(kPlatformInitExitCode);
}

} // namespace helios::core
