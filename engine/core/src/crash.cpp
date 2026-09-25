#include "helios/core/crash.h"

#include <atomic>
#include <mutex>

#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "platform/os.h"

namespace helios {

namespace {
std::mutex g_crashMutex;
std::atomic<bool> g_installed{false};
} // namespace

Result<void> installCrashHandler(const std::filesystem::path& dumpDir, const CrashHandlerOptions& options) {
    std::lock_guard lock(g_crashMutex);
    HELIOS_TRY(fs::createDirectories(dumpDir));
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(dumpDir, ec);
    HELIOS_TRY(os::installCrashHandler(ec ? dumpDir : absolute, options));
    g_installed.store(true, std::memory_order_release);
    HELIOS_LOG_DEBUG(LogCore, "Crash handler installed; reports go to {}", fs::pathToGenericUtf8(dumpDir));
    return {};
}

void uninstallCrashHandler() noexcept {
    std::lock_guard lock(g_crashMutex);
    if (!g_installed.load(std::memory_order_acquire)) return;
    os::uninstallCrashHandler();
    g_installed.store(false, std::memory_order_release);
}

bool isCrashHandlerInstalled() noexcept { return g_installed.load(std::memory_order_acquire); }

Result<std::filesystem::path> writeCrashReport(std::string_view reason) {
    std::lock_guard lock(g_crashMutex);
    if (!g_installed.load(std::memory_order_acquire)) {
        return Error{ErrorCode::InvalidState, "writeCrashReport: no crash handler installed"};
    }
    return os::writeCrashReport(reason);
}

} // namespace helios
