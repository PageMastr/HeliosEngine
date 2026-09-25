// netcode/reliable process-wide initialisation, log and assert routing, allocator hooks.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

#include <netcode.h>
#include <reliable.h>

#include "helios/core/assert.h"
#include "net_internal.h"

namespace helios::net::detail {
namespace {

HELIOS_LOG_CHANNEL(LogNetcode, "Net.netcode");

std::mutex g_libraryMutex;
int g_libraryRefs = 0;

/// Maps a vendor message to a log level: their errors start with "error".
void forward(std::string_view origin, const char* text) {
    std::string_view msg(text ? text : "");
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.remove_suffix(1);
    if (msg.empty()) return;
    const bool isError = msg.find("error") != std::string_view::npos;
    if (isError) {
        HELIOS_LOG_WARN(LogNetcode, "[{}] {}", origin, msg);
    } else {
        HELIOS_LOG_DEBUG(LogNetcode, "[{}] {}", origin, msg);
    }
}

// Both libraries call printf_function("%s", buffer) with an already formatted message.
int netcodePrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const char* text = std::strcmp(format, "%s") == 0 ? va_arg(args, const char*) : format;
    forward("netcode", text);
    va_end(args);
    return 0;
}

int reliablePrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    forward("reliable", buffer);
    return 0;
}

void vendorAssert(const char* condition, const char* function, const char* file, int line) {
    // A failed vendor assert means corrupted state; continuing would be undefined behaviour.
    HELIOS_LOG_FATAL(LogNetcode, "vendored transport assert failed: ({}) in {} at {}:{}", condition ? condition : "?",
                     function ? function : "?", file ? file : "?", line);
}

} // namespace

MemoryTag netMemoryTag() noexcept {
    static const MemoryTag tag = registerMemoryTag("Net");
    return tag;
}

void* netAllocate(void* context, std::size_t bytes) {
    (void)context;
    return alignedAlloc(bytes == 0 ? 1 : bytes, 16, netMemoryTag());
}

void netFree(void* context, void* pointer) {
    (void)context;
    alignedFree(pointer);
}

bool acquireLibrary() noexcept {
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefs == 0) {
        if (netcode_init() != NETCODE_OK) {
            HELIOS_LOG_ERROR(LogNet, "netcode_init failed");
            return false;
        }
        reliable_init();
        netcode_set_printf_function(netcodePrintf);
        netcode_set_assert_function(vendorAssert);
        reliable_set_printf_function(reliablePrintf);
        reliable_set_assert_function(vendorAssert);
        // Vendor logging is off unless the Net.netcode channel is at Debug: both libraries log
        // peer-triggerable conditions (bad tokens, duplicate or invalid fragments) at error level,
        // which on a public gateway would let a peer spam the log. Helios reports those as
        // counters, pre-filter stats and malformed-packet strikes instead.
        const bool debug = log::isEnabled(log::Level::Debug, LogNetcode);
        netcode_log_level(debug ? NETCODE_LOG_LEVEL_DEBUG : NETCODE_LOG_LEVEL_NONE);
        reliable_log_level(debug ? RELIABLE_LOG_LEVEL_DEBUG : RELIABLE_LOG_LEVEL_NONE);
    }
    ++g_libraryRefs;
    return true;
}

void releaseLibrary() noexcept {
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefs <= 0) return;
    if (--g_libraryRefs == 0) {
        reliable_term();
        netcode_term();
    }
}

} // namespace helios::net::detail
