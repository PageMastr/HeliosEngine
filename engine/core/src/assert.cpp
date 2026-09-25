#include "helios/core/assert.h"

#include <atomic>
#include <cstdlib>
#include <format>
#include <string_view>

#include "helios/core/log.h"
#include "platform/os.h"

namespace helios {

namespace {
constinit std::atomic<AssertHandler> g_handler{nullptr};
constinit std::atomic<u64> g_failureCount{0};
thread_local int t_assertDepth = 0;
} // namespace

AssertHandler setAssertHandler(AssertHandler handler) noexcept {
    const AssertHandler previous = g_handler.exchange(handler, std::memory_order_acq_rel);
    return previous ? previous : &defaultAssertHandler;
}

AssertHandler assertHandler() noexcept {
    const AssertHandler handler = g_handler.load(std::memory_order_acquire);
    return handler ? handler : &defaultAssertHandler;
}

u64 assertFailureCount() noexcept { return g_failureCount.load(std::memory_order_relaxed); }

AssertAction defaultAssertHandler(const AssertInfo& info) {
    const bool isVerify = std::string_view(info.kind) == "VERIFY";
    const bool continuable = isVerify && !HELIOS_ENABLE_ASSERTS;
    std::string text = info.message.empty()
                           ? std::format("{} failed: {} in {}", info.kind, info.expression, info.function)
                           : std::format("{} failed: {} in {}: {}", info.kind, info.expression, info.function,
                                         info.message);
    log::writeMessage(continuable ? log::Level::Error : log::Level::Fatal, LogCore,
                      log::SourceLocation{info.file, info.line, info.function}, text);
    log::flush();
    if (os::isDebuggerPresent()) return AssertAction::Break;
    return continuable ? AssertAction::Continue : AssertAction::Abort;
}

namespace detail {

AssertAction reportAssertFailure(const char* kind, const char* expression, const char* file, u32 line,
                                 const char* function, std::string_view message) {
    g_failureCount.fetch_add(1, std::memory_order_relaxed);
    if (t_assertDepth > 0) {
        // An assert fired inside the assert handler (or the logger it uses): report minimally.
        os::consoleWrite(os::ConsoleStream::Err, "Helios: recursive assertion failure: ");
        os::consoleWrite(os::ConsoleStream::Err, expression ? expression : "?");
        os::consoleWrite(os::ConsoleStream::Err, "\n");
        return AssertAction::Abort;
    }
    ++t_assertDepth;
    AssertInfo info;
    info.kind = kind;
    info.expression = expression;
    info.file = file;
    info.line = line;
    info.function = function;
    info.message = message;
    const AssertAction action = assertHandler()(info);
    --t_assertDepth;
    return action;
}

void assertAbort() noexcept {
    log::flush();
    std::abort();
}

} // namespace detail
} // namespace helios
