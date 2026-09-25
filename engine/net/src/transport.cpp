// SocketTransport (batched UdpSocket) and VirtualNetwork (in-process datagram network).

#include "helios/net/transport.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "helios/core/assert.h"

namespace helios::net {

// ---------------------------------------------------------------------------------------------
// SocketTransport
// ---------------------------------------------------------------------------------------------

Result<std::unique_ptr<SocketTransport>> SocketTransport::open(const SocketTransportConfig& config) {
    HELIOS_TRY_ASSIGN(UdpSocket socket, UdpSocket::open(config.socket));
    const u32 batch = std::clamp<u32>(config.batchSize, 1, 1024);
    return std::unique_ptr<SocketTransport>(new SocketTransport(std::move(socket), batch));
}

SocketTransport::SocketTransport(UdpSocket socket, u32 batchSize)
    : m_socket(std::move(socket)), m_batchSize(batchSize) {
    // Fixed slots of kMaxDatagramBytes so queued views never move.
    m_sendArena.resize(static_cast<usize>(batchSize) * kMaxDatagramBytes);
    m_sendBatch.reserve(batchSize);
    m_recvArena.resize(static_cast<usize>(batchSize) * kMaxDatagramBytes);
    m_recvSlots.resize(batchSize);
    for (usize i = 0; i < batchSize; ++i) {
        m_recvSlots[i].buffer = std::span<u8>(m_recvArena.data() + i * kMaxDatagramBytes, kMaxDatagramBytes);
    }
}

SocketTransport::~SocketTransport() { flush(); }

void SocketTransport::send(const Address& to, std::span<const u8> data) {
    if (data.empty() || data.size() > kMaxDatagramBytes) return;
    if (m_sendBatch.size() >= m_batchSize) flush();
    u8* slot = m_sendArena.data() + m_sendBatch.size() * kMaxDatagramBytes;
    std::memcpy(slot, data.data(), data.size());
    m_sendBatch.push_back(OutDatagram{to, std::span<const u8>(slot, data.size())});
}

void SocketTransport::flush() {
    if (m_sendBatch.empty()) return;
    m_socket.sendBatch(m_sendBatch);
    m_sendBatch.clear();
}

usize SocketTransport::receive(Address& from, std::span<u8> buffer) {
    for (;;) {
        if (m_recvNext >= m_recvCount) {
            // Slot buffers may be permuted by receiveBatch; restore the canonical mapping first.
            for (usize i = 0; i < m_recvSlots.size(); ++i) {
                m_recvSlots[i].buffer = std::span<u8>(m_recvArena.data() + i * kMaxDatagramBytes, kMaxDatagramBytes);
                m_recvSlots[i].size = 0;
            }
            m_recvCount = m_socket.receiveBatch(m_recvSlots);
            m_recvNext = 0;
            if (m_recvCount == 0) return 0;
        }
        const InDatagram& d = m_recvSlots[m_recvNext++];
        if (d.size > buffer.size()) continue; // cannot be delivered; drop
        std::memcpy(buffer.data(), d.buffer.data(), d.size);
        from = d.from;
        return d.size;
    }
}

// ---------------------------------------------------------------------------------------------
// VirtualNetwork
// ---------------------------------------------------------------------------------------------

struct VirtualNetwork::Socket::Queue {
    struct Item {
        Address from;
        u16 size = 0;
        std::array<u8, kMaxDatagramBytes> data;
    };
    mutable std::mutex mutex;
    std::deque<Item> items;
    usize capacity = 65536;
    u64 received = 0;
};

struct VirtualNetwork::State {
    std::mutex mutex;
    std::unordered_map<Address, std::weak_ptr<Socket::Queue>> sockets;
    u16 nextEphemeral = 49152;

    std::shared_ptr<Socket::Queue> find(const Address& to) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sockets.find(to);
        if (it == sockets.end()) return nullptr;
        auto q = it->second.lock();
        if (!q) sockets.erase(it);
        return q;
    }

    /// 1: delivered, 0: no route, -1: queue full.
    int deliver(const Address& from, const Address& to, std::span<const u8> data) {
        if (data.empty() || data.size() > kMaxDatagramBytes) return 0;
        auto q = find(to);
        if (!q) return 0;
        std::lock_guard<std::mutex> lock(q->mutex);
        if (q->items.size() >= q->capacity) return -1;
        auto& item = q->items.emplace_back();
        item.from = from;
        item.size = static_cast<u16>(data.size());
        std::memcpy(item.data.data(), data.data(), data.size());
        return 1;
    }
};

VirtualNetwork::VirtualNetwork() : m_state(std::make_shared<State>()) {}
VirtualNetwork::~VirtualNetwork() = default;

Result<std::unique_ptr<VirtualNetwork::Socket>> VirtualNetwork::bind(const Address& address, usize queueCapacity) {
    if (!address.isValid()) return Error{ErrorCode::InvalidArgument, "VirtualNetwork: invalid address"};
    auto queue = std::make_shared<Socket::Queue>();
    queue->capacity = std::max<usize>(queueCapacity, 1);
    Address bound = address;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (bound.port() == 0) {
            // Ephemeral: first free port from the dynamic range (wraps once).
            bool found = false;
            for (u32 attempt = 0; attempt < 16384 && !found; ++attempt) {
                const u16 port = m_state->nextEphemeral;
                m_state->nextEphemeral = static_cast<u16>(port == 65535 ? 49152 : port + 1);
                const Address candidate = address.withPort(port);
                auto it = m_state->sockets.find(candidate);
                if (it == m_state->sockets.end() || it->second.expired()) {
                    bound = candidate;
                    found = true;
                }
            }
            if (!found) return Error{ErrorCode::LimitExceeded, "VirtualNetwork: no free ephemeral port"};
        } else {
            auto it = m_state->sockets.find(bound);
            if (it != m_state->sockets.end() && !it->second.expired()) {
                return makeError(ErrorCode::AlreadyExists, "VirtualNetwork: {} already bound", bound);
            }
        }
        m_state->sockets[bound] = queue;
    }
    return std::unique_ptr<Socket>(new Socket(m_state, bound, std::move(queue)));
}

bool VirtualNetwork::inject(const Address& from, const Address& to, std::span<const u8> data) {
    return m_state->deliver(from, to, data) == 1;
}

VirtualNetwork::Socket::Socket(std::shared_ptr<State> state, Address address, std::shared_ptr<Queue> queue)
    : m_state(std::move(state)), m_address(address), m_queue(std::move(queue)) {}

VirtualNetwork::Socket::~Socket() {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    auto it = m_state->sockets.find(m_address);
    if (it != m_state->sockets.end()) {
        auto q = it->second.lock();
        if (!q || q == m_queue) m_state->sockets.erase(it);
    }
}

void VirtualNetwork::Socket::send(const Address& to, std::span<const u8> data) {
    ++m_sent;
    const int r = m_state->deliver(m_address, to, data);
    if (r == 0) ++m_droppedNoRoute;
    if (r < 0) ++m_droppedQueueFull;
}

usize VirtualNetwork::Socket::receive(Address& from, std::span<u8> buffer) {
    std::lock_guard<std::mutex> lock(m_queue->mutex);
    while (!m_queue->items.empty()) {
        const Queue::Item& item = m_queue->items.front();
        const usize size = item.size;
        if (size > buffer.size()) {
            m_queue->items.pop_front();
            continue;
        }
        std::memcpy(buffer.data(), item.data.data(), size);
        from = item.from;
        m_queue->items.pop_front();
        ++m_queue->received;
        return size;
    }
    return 0;
}

VirtualSocketStats VirtualNetwork::Socket::stats() const {
    VirtualSocketStats s;
    s.sent = m_sent;
    s.droppedNoRoute = m_droppedNoRoute;
    s.droppedQueueFull = m_droppedQueueFull;
    std::lock_guard<std::mutex> lock(m_queue->mutex);
    s.received = m_queue->received;
    return s;
}

usize VirtualNetwork::Socket::pending() const {
    std::lock_guard<std::mutex> lock(m_queue->mutex);
    return m_queue->items.size();
}

} // namespace helios::net
