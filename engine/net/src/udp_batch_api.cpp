// UdpBatchApi names and the batch-API test hooks, shared by both platform implementations of UdpSocket.

#include <atomic>

#include "helios/net/udp_socket.h"
#include "net_internal.h"

namespace helios::net {

std::string_view udpBatchApiName(UdpBatchApi api) noexcept {
    switch (api) {
    case UdpBatchApi::Auto: return "auto";
    case UdpBatchApi::Registered: return "registered";
    case UdpBatchApi::MultiMessage: return "multi-message";
    case UdpBatchApi::Message: return "message";
    }
    return "?";
}

namespace detail {
namespace {
std::atomic<bool> g_registeredIoUnavailable{false};
std::atomic<bool> g_registeredIoSetUpFailure{false};
std::atomic<bool> g_registeredIoFallbackWarned{false};
} // namespace

void forceRegisteredIoUnavailable(bool force) noexcept { g_registeredIoUnavailable.store(force); }
void forceRegisteredIoSetUpFailure(bool force) noexcept { g_registeredIoSetUpFailure.store(force); }
bool registeredIoUnavailableForced() noexcept { return g_registeredIoUnavailable.load(); }
bool registeredIoSetUpFailureForced() noexcept { return g_registeredIoSetUpFailure.load(); }
bool firstRegisteredIoFallback() noexcept { return !g_registeredIoFallbackWarned.exchange(true); }
void resetRegisteredIoFallbackWarning() noexcept { g_registeredIoFallbackWarned.store(false); }
} // namespace detail

} // namespace helios::net
