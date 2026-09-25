#pragma once
// Datagram transports underneath netcode (04 §2, L0). netcode runs with
// `override_send_and_receive`, so every datagram an endpoint sends or receives goes through an
// IDatagramTransport:
//
//   SocketTransport   a real UdpSocket with batched I/O (sendmmsg/recvmmsg batches of 64)
//   VirtualNetwork    an in-process datagram network (PIE, --embedded-gateway, deterministic tests)
//   NetSimTransport   netsim.h: latency/jitter/loss/duplication/reordering/bandwidth around either
//
// Threading: a transport is owned by the thread that updates its endpoint. VirtualNetwork itself
// is thread-safe (each virtual socket's queue is locked), so endpoints on different threads may
// share one network.

#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/address.h"
#include "helios/net/udp_socket.h"

namespace helios::net {

/// Largest datagram any transport carries: netcode's NETCODE_MAX_PACKET_BYTES.
inline constexpr usize kMaxDatagramBytes = 1300;

class IDatagramTransport {
public:
    virtual ~IDatagramTransport() = default;

    /// Sends (or queues until flush()) one datagram. Never blocks; may drop, as UDP does.
    virtual void send(const Address& to, std::span<const u8> data) = 0;
    /// Pops one received datagram into `buffer` and returns its size, or 0 when none is pending.
    /// Datagrams larger than `buffer` are dropped.
    virtual usize receive(Address& from, std::span<u8> buffer) = 0;
    /// Called at the start of every endpoint update() and flush() with the endpoint's clock
    /// (seconds, never decreasing). Time-driven transports (NetSim) release due datagrams here and
    /// timestamp later send()s with this time.
    virtual void update(f64 now) { (void)now; }
    /// Pushes queued datagrams to the network. Called at the end of each endpoint update/flush.
    virtual void flush() {}
    /// The address peers see as this transport's source.
    virtual Address localAddress() const = 0;
};

struct SocketTransportConfig {
    UdpSocketConfig socket;
    /// Datagrams per sendmmsg/recvmmsg call (04 §2.6: 64 on trunks).
    u32 batchSize = 64;

    /// Trunk sockets (04 §2.6): 32 MB buffers, batches of 64. The bind address stays as given.
    static SocketTransportConfig trunk(const Address& bind = Address::anyV4(0)) {
        SocketTransportConfig c;
        c.socket.bindAddress = bind;
        c.socket.sendBufferBytes = 32u * 1024 * 1024;
        c.socket.receiveBufferBytes = 32u * 1024 * 1024;
        c.batchSize = 64;
        return c;
    }
};

/// IDatagramTransport over a UdpSocket. send() queues into a batch that is written with one
/// sendBatch() call when full or on flush(); receive() refills a local batch with receiveBatch().
class SocketTransport final : public IDatagramTransport {
public:
    static Result<std::unique_ptr<SocketTransport>> open(const SocketTransportConfig& config);
    ~SocketTransport() override;

    void send(const Address& to, std::span<const u8> data) override;
    usize receive(Address& from, std::span<u8> buffer) override;
    void flush() override;
    Address localAddress() const override { return m_socket.localAddress(); }

    UdpSocket& socket() noexcept { return m_socket; }
    const UdpSocket& socket() const noexcept { return m_socket; }

private:
    explicit SocketTransport(UdpSocket socket, u32 batchSize);

    UdpSocket m_socket;
    u32 m_batchSize;
    // Outgoing batch: payloads live in fixed arena slots, datagram views point into them.
    std::vector<u8> m_sendArena;
    std::vector<OutDatagram> m_sendBatch;
    // Incoming batch.
    std::vector<u8> m_recvArena;
    std::vector<InDatagram> m_recvSlots;
    usize m_recvCount = 0;
    usize m_recvNext = 0;
};

/// Counters for one virtual socket.
struct VirtualSocketStats {
    u64 sent = 0;
    u64 received = 0;
    u64 droppedNoRoute = 0;   ///< Destination not bound.
    u64 droppedQueueFull = 0; ///< Destination queue at capacity.
};

/// An in-process datagram network: sockets bound to arbitrary IPv4/IPv6 addresses exchange
/// datagrams through per-socket queues, with no OS sockets involved. Delivery is immediate and
/// in order (wrap sockets in NetSimTransport for impairments), which makes tests deterministic.
class VirtualNetwork {
public:
    VirtualNetwork();
    ~VirtualNetwork();
    VirtualNetwork(const VirtualNetwork&) = delete;
    VirtualNetwork& operator=(const VirtualNetwork&) = delete;

    class Socket;

    /// Binds a socket. Port 0 assigns an ephemeral port (from 49152 up). Fails with AlreadyExists
    /// when the address is taken. The socket may outlive the network object.
    Result<std::unique_ptr<Socket>> bind(const Address& address, usize queueCapacity = 65536);

    /// Injects a datagram as if it came from `from` (fuzzing, malicious-peer tests). Returns
    /// false when nothing is bound at `to`.
    bool inject(const Address& from, const Address& to, std::span<const u8> data);

    struct State;

private:
    std::shared_ptr<State> m_state;
};

class VirtualNetwork::Socket final : public IDatagramTransport {
public:
    ~Socket() override;
    void send(const Address& to, std::span<const u8> data) override;
    usize receive(Address& from, std::span<u8> buffer) override;
    Address localAddress() const override { return m_address; }
    VirtualSocketStats stats() const;
    /// Number of datagrams waiting to be received.
    usize pending() const;

    struct Queue;

private:
    friend class VirtualNetwork;
    Socket(std::shared_ptr<State> state, Address address, std::shared_ptr<Queue> queue);

    std::shared_ptr<State> m_state;
    Address m_address;
    std::shared_ptr<Queue> m_queue;
    u64 m_sent = 0;
    u64 m_droppedNoRoute = 0;
    u64 m_droppedQueueFull = 0;
};

} // namespace helios::net
