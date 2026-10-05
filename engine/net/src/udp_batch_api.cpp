// UdpBatchApi names, shared by both platform implementations of UdpSocket.

#include "helios/net/udp_socket.h"

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

} // namespace helios::net
