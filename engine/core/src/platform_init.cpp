// Process start-up checks (02 §1.1 "Proof that it ran"): see helios/core/platform_init.h.
#include "helios/core/platform_init.h"

#include <cstdlib>
#include <string>

#include "helios/core/log.h"
#include "platform/os.h"

namespace helios::core {

Result<void> checkCpuGateVerdict(CpuGateVerdict verdict) {
    switch (verdict) {
    case CpuGateVerdict::Pass: return {};
    case CpuGateVerdict::Fail:
        return Error{ErrorCode::InvalidState, "CPU gate did not run to completion: its pre-initializer "
                                              "refused this CPU but the process continued (02 §1.1)"};
    case CpuGateVerdict::NotRun: break;
    }
    return Error{ErrorCode::InvalidState,
                 "CPU gate did not run: this executable carries no CPU-gate pre-initializer, or it ran too "
                 "late (link the image with helios_executable() and a gate role; 02 §1.1)"};
}

void platformInit() noexcept {
    const Result<void> gate = checkCpuGateVerdict(cpuGateVerdict());
    if (gate.ok()) return;
    // Straight to the stderr handle: the log may have no console sink, and this must reach a smoke test.
    const std::string line = "Helios: " + gate.error().message + "\n";
    if (const NativeHandle err = os::standardHandle(2); err != kInvalidNativeHandle) {
        (void)os::pipeWrite(err, line.data(), line.size());
    }
    log::flush();
    std::_Exit(kPlatformInitExitCode);
}

} // namespace helios::core
