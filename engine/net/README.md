# engine/net — HTP transport (WP-0.13)

`helios::net` (target `helios_net`, HEADLESS, deps `helios::core`, `helios::math`, private
`helios::tp::netcode`) is the Helios Transport Protocol of 04 §2: vendored **netcode 1.4.8**
(connect tokens, encrypted UDP) and **reliable 1.4.5** (packet acks, fragmentation) wrapped with
Helios channels, message reliability, budgets and statistics. The same stack serves client links
and private server trunks. Helios adds **no cryptography of its own**.

```
L0  udp_socket.h   UdpSocket: Winsock2 / BSD, IPv4 + dual-stack IPv6, non-blocking,
                   sendmmsg/recvmmsg batches of 64 (Linux), SO_*BUF(FORCE) sizing, SIO_UDP_CONNRESET off
    transport.h    IDatagramTransport; SocketTransport (batched socket); VirtualNetwork (in-process)
    netsim.h       NetSim: latency, jitter, Bernoulli + Gilbert-Elliott loss, duplication,
                   reordering, bandwidth cap; profiles lan/good/mobile/awful
L1  endpoint.h     Server / Client: netcode instances over any transport (override_send_and_receive),
                   Helios allocators ("Net" memory tag) and logging, L0 pre-filter, session handles
    connect_token.h  mint (netcode API), parse public part, open private part (netcode's AEAD)
L2  connection.h   one reliable endpoint per session
L3  channel.h      the nine HTP channels;  wire.h message framing;  congestion.h budgets/AIMD/RTT
```

Everything takes the caller's clock (`f64` seconds) — `helios::monotonicSeconds()` in production,
a simulated clock in tests — so the whole stack is deterministic under NetSim.

## Quick start

```cpp
using namespace helios::net;
ServerConfig sc;                                   // gateway or cell
sc.protocolId = kProtocolId;  sc.privateKey = shardKey;
sc.bindAddress = *Address::parse("0.0.0.0:7777");
sc.publicAddresses = {*Address::parse("203.0.113.7:7777")};
auto server = Server::create(std::move(sc), monotonicSeconds()).value();

auto client = Client::create(ClientConfig{}, monotonicSeconds()).value();
client->connect(tokenFromSessionService, monotonicSeconds());       // 2,048-byte netcode token

struct Handler : IEndpointHandler {
    void onConnected(SessionHandle s) override;
    void onDisconnected(SessionHandle s, DisconnectReason r) override;
    void onMessage(SessionHandle s, Channel c, std::span<const u8> payload) override;
    void onDeliveryNotify(SessionHandle s, std::span<const PacketNotify> n) override;  // STATE
};
// I/O loop: every poll            // tick: once per simulation tick
server->update(now, handler);      server->send(session, Channel::EventReliable, bytes);
                                   u64 id; server->sendState(session, chunk, &id);
                                   server->flush(now);
```

`update()` receives, decrypts, acks, delivers messages and notifications and runs timers (call it
often); `flush()` packs queued messages into packets (once per tick). Both advance the transport's
clock, so NetSim times a tick's packets from the flush, not from the last poll. Budget for the replication
writer: `server->connection(s)->stateBudgetBytes(tickHz)`.

## Wire format (04 §2.1)

| Layer | Bytes | Content |
|---|---|---|
| netcode | 1 + 1..8 + ≤ 1,200 + 16 | prefix (type, sequence size), sequence, ChaCha20-Poly1305 ciphertext + MAC |
| reliable | 3–9 (+ 5 per fragment) | prefix, sequence u16, ack, ack bits |
| HTP | ≤ 1,191 per packet | messages: `hdr u8` = channel (bits 0–3) \| slice (4) \| sequenced (5) \| reserved (6–7, zero); `msg_seq u16` (sequenced channels); `slice_index u8, slice_count−1 u8` (BULK); `len` canonical LEB128; payload. `0x0F` + zeros = padding / **ack-only** packet |

`wire::parsePacket` validates a whole packet (flags vs. channel, canonical varints, lengths,
per-channel size limits, reserved bits, padding) before anything is delivered and never asserts
on input. Trunk messages add a varint stream id (session or AG) via `wire::writeStreamMessage`.

## Channels (04 §2.2)

| ID | Channel | Semantics | Limits |
|---|---|---|---|
| 0 | CONTROL | reliable-ordered, packed first | ≤ 16 KB (one reliable-fragmented packet); trunk ≤ 256 KB |
| 1 | INPUT | unreliable, sequenced; last 4 inputs repeated in every packet until acked | one datagram |
| 2 | STATE | unreliable + exactly one `PacketNotify{seq, delivered}` per `sendState()` | one datagram (≥ 1,100 B chunks fit) |
| 3 | EVENT_R | reliable-ordered | one datagram |
| 4 | EVENT_U | unreliable | one datagram |
| 5 | LATEST | unreliable-sequenced (anything older than the newest delivered is dropped) | one datagram |
| 6 | BULK | reliable-ordered blobs ≤ 256 KB in 1 KB acked slices, ≤ 64 slices in flight, lowest priority | 256 KB |
| 7 | DEBUG | reliable-ordered; `HELIOS_NET_DEBUG_CHANNEL=0` (default under `HELIOS_SHIPPING`) removes it | 16 KB |
| 8 | VOICE | unreliable, not tick-packed: sent on arrival in its own packet or appended to a game packet leaving within 5 ms; own 40 kbit/s bucket | one datagram |

Packing order: CONTROL > EVENT_R > INPUT/STATE > EVENT_U > LATEST > DEBUG > BULK. Channels 9–14
are reserved; 15 is the padding marker. Reliable windows hold ≤ 1,024 messages per channel
(`send()` returns `WouldBlock` beyond that).

**Reliability model.** Every packet records the reliable messages, INPUTs and STATE chunks it
carried. reliable's acks mark them delivered. A packet is declared lost when one ≥ 3 sequences
newer is acked, or after `1.25 × srtt + peer ack delay` (never below `srtt + 4 rttvar`, so an ack
that is late because an ack-only packet was lost is not reported as a loss). Reliable messages are
resent after `RTO = srtt + 4 rttvar` (30 ms–1 s) or at once when their packet is declared lost.
STATE notifications never report a lost chunk as delivered; a delivered chunk whose ack was lost
may be reported lost (replication repairs it).

**Acks.** Every outgoing packet carries reliable's acks. A received packet with reliable messages
is acked within `maxAckDelay` (gateway 10 ms, client 20 ms); a packet with only unreliable data
(INPUT, STATE, EVENT_U, LATEST) is acked by the next outgoing packet, at the latest after
`maxLazyAckDelay` (gateway 100 ms, client 20 ms); after 16 unacked packets an ack goes out at once
(reliable's ack window is 33 packets). Each flush sends an ack-only packet if nothing else carried
the acks, and ack-only packets are not themselves acked. So a gateway sends **one packet per tick**
(04 §2.5) instead of an ack-only packet per 60 Hz client input, and clients' acks ride their tick
packets. RTT samples come only from packets whose peer ack delay is short: reliable packets always,
unreliable-only packets when `peerMaxLazyAckDelay <= peerMaxAckDelay` (gateways sample the client's
tick-paced acks; clients sample only their reliable traffic, e.g. RPCs and CONTROL time sync, so the
gateway's tick does not inflate their RTT). Loss detection adds the peer's lazy ack delay for
unreliable-only packets.

**Fragmentation.** Messages larger than one datagram (CONTROL/DEBUG bursts) travel as one packet
that reliable fragments (1 KB fragments; 17 for 16 KB, 256 on trunks) and reassembles; a lost
fragment costs a retransmission of the packet. Large data belongs on BULK, whose slices are acked
individually. Malicious fragments (bad ids/counts/sizes/headers) are rejected by reliable's
validating reader; see `test_connection.cpp`.

## Rate control (04 §2.5, §9)

| | client → gateway | gateway → client | trunk |
|---|---|---|---|
| Budget | fixed 64 kbit/s bucket | 256 kbit/s, AIMD 64..256 (`setMaxBudgetBps(512k)` in battles) | 2 Gbit/s |
| AIMD | off | loss > 5 % or srtt > min RTT + 100 ms for 1 s → ×0.7; 5 s clean → +16 kbit/s/s | off |
| Packets per flush | 1, at most 60 pps (`min(tick_hz, 60)`) | ≤ 4, paced 2 ms | unlimited, unpaced |
| Acks | ride tick packets (≤ 20 ms) | reliable ≤ 10 ms; unreliable-only ride the next tick (≤ 100 ms) | ≤ 2 ms |
| Inbound policing | — | 120 pps (240 burst); 64 kbit/s game data + a separate 40 kbit/s VOICE bucket | — |
| reliable buffers | 256 | 256 | 4,096 |
| Reliable window / reorder buffer / reassemblies | 64 KB / 320 KB / 8 | 64 KB / 320 KB / 8 | 16 MB / 65 MB / 64 |

Budgets count wire bytes (payload + reliable header + 48 B netcode/UDP/IP overhead per
datagram); a packet may borrow from the bucket (negative balance), so an oversized CONTROL burst
delays later sends instead of stalling. VOICE is charged to its own bucket only.

**L0 pre-filter** (server and client): the prefix byte's type must be one the role accepts, the
sequence size 1..8, and the size exactly what that type encrypts to (requests: exactly 1,078
unencrypted bytes). Connection requests are limited to 10/s per source (an IPv4 address or an IPv6
/64) in 4,096 buckets indexed by a hash keyed with a random per-server secret, so an attacker cannot
precompute a spoofed source that shares a player's bucket and starve its requests.
**Inbound policing** (gateway side of client links): datagrams are counted (120 pps, 240 burst)
and their bytes charged to a combined bucket on arrival (which also bounds fragments of packets
that never complete); after parsing, VOICE messages are charged to the 40 kbit/s voice bucket and
everything else to the 64 kbit/s game bucket (04 §9). Policed packets are dropped unacked.
**Flow control and memory bounds.** Each reliable channel's window span (payload sent but not yet
cumulatively acked) is bounded by `maxWindowBytes` (client links 64 KB, which is 2 s of the 256
kbit/s budget; trunks 16 MB), so a well-behaved peer can never make a receiver buffer more than
`4 × (maxWindowBytes + maxJumboMessageBytes)` (320 KB) out of order; a packet that would exceed
`maxReorderBytes` is refused unacked and counts as malformed, so an authenticated peer withholding
one sequence number cannot pin server memory. Out-of-order messages sit in an ordered map (O(log
n) per message even when a peer sends its window in reverse). reliable allocates a whole packet
(≤ 17 KB) on a sequence's first fragment, so client links keep 8 reassemblies (≤ 140 KB per session
pinned by stray fragments; 64 would allow 1.1 MB). With a 256 KB BULK blob in progress an
authenticated peer can hold at most ≈ 0.7 MB of a gateway's memory.
**Malformed packets** (HTP parse errors, reliable sequences outside the window, reorder-buffer
overflow, unreadable reliable headers, bad BULK slice sequences) are strikes; 3 strikes disconnect the session with
`DisconnectReason::Malformed`. Vendor logging is off unless the `Net.netcode` log channel is at
Debug, because netcode/reliable log peer-triggerable conditions at error level.

## Handshake, keep-alive, timeouts, reconnect (04 §2.3–2.4)

netcode validates protocol id, expiry, the server's own address in the token and token reuse,
challenges statelessly, and sends keep-alives at 10 Hz when idle; the token's timeout (10 s by
default) ends a silent session (`DisconnectReason::TimedOut`, linkdead). A client reconnects by
calling `connect()` again with a fresh token (the reconnect-ticket path); the `SessionHandle`
generation changes so stale handles never reach the new session. Servers use
`maxConnectTokenLifetimeSeconds = 45` (the Session service's token lifetime); a token must not
predate the server's start by more than that (netcode's nonce-reuse guard).

**NS-0.1 (handshake in 1.5 RTT).** netcode's client paces every handshake packet at 10 Hz, so it
would answer a challenge up to 100 ms late. `ClientConfig::fastHandshake` (default on) advances the
netcode client's clock by 100 ms when the challenge arrives and updates it again, so the response
leaves immediately: the server completes the handshake 1.5 RTT after the first request and the
client learns it at 2 RTT (measured: 75.6 ms / 100.7 ms at 50 ms RTT, 0.1 ms simulation step;
125 ms without the adjustment). A one-line upstream netcode change (reset `last_packet_send_time`
on the challenge) would make this unnecessary.

## Trunk profile (04 §2.6)

`ConnectionConfig::trunk()` + `SocketTransportConfig::trunk()`: 4,096-entry sent/received packet
buffers, fragments reassembling up to 256 KB, 32 MB socket buffers (retried with `SO_RCVBUFFORCE`/`SO_SNDBUFFORCE` when the kernel
caps them), sendmmsg/recvmmsg batches of 64, 2 ms acks, no AIMD, no pacing. Small chunks for many
sessions coalesce into full datagrams automatically (400 × 40 B chunks → 16 datagrams).

## Threading

A `Server`, `Client`, `Connection` or transport is owned by one thread; different instances may run
on different threads (the trunk gate runs a cell and a gateway on two threads). Creation and the
reference-counted netcode/reliable initialisation are thread-safe. `VirtualNetwork` is
thread-safe. Handlers run inside `update()` on the updating thread and may call `send()` and
`disconnect()` (applied after delivery) but not `update()`/`flush()`.

## Tests, fuzzers, gates

`net_tests` (doctest, 84 test cases; `ctest -R net_`):

| Suite | Covers |
|---|---|
| `net.address` | parser/formatter (RFC 5952), rejects, mapped addresses, agreement with `netcode_parse_address` |
| `net.wire` | varints (canonical), every channel round trip, ack-only/padding, every parse error, stream framing |
| `net.congestion` | token bucket, RFC 6298 RTT/RTO clamps, windowed min RTT, jitter, loss window, AIMD decrease/floor/increase/cap/RTT trigger |
| `net.netsim` | latency, Bernoulli loss, determinism by seed, jitter/reorder/duplication, Gilbert-Elliott bursts, bandwidth cap + tail drop, transport decorator, profiles, VirtualNetwork |
| `net.connection` | reliable-ordered exactly-once in-order under 10 % loss + reorder + duplication (both directions), EVENT_U at-most-once, LATEST monotonic, INPUT redundancy, STATE notify invariants, BULK 256 KB, 16 KB CONTROL fragmentation, WouldBlock/TooLarge, window-bytes flow control, reorder-buffer pinning defence, malicious fragments, malformed strikes, budget cap and state budget, 2 ms pacing, AIMD against a 128 kbit/s bottleneck and recovery, RTT/min RTT/jitter/loss stats, ack-only, VOICE, STATE expiry, inbound policing (120 pps; 64 kbit/s game + separate 40 kbit/s VOICE); every arrival order within a 1,024 window and a reversed 16,384 window at O(log n) per message; one gateway packet per tick with reliable acks still ≤ 10 ms; client RTT/loss detection unaffected by tick-paced acks; 60 pps upstream cap at 144 Hz; memory an authenticated peer can pin |
| `net.endpoint` | token handshake + session info, NS-0.1, 16 clients × all channels under 10 % loss/reorder/dup, timeouts + reconnect, graceful disconnects, stale handles, token validation (protocol, key, address, expiry, full, reuse, junk), pre-filter, malformed authenticated peer, server and client handler re-entrancy (send/disconnect from callbacks), real UDP loopback, keyed pre-filter buckets (a precomputed colliding source cannot starve a victim), IPv6 /64 rate limiting, NetSim timing of flush()-time sends, NAT-rebinding reconnect via `findSession` + `disconnect` |
| `net.connect_token` | round trip, tamper detection, **Go golden vectors** (`services/testdata/vectors`): public + private parts byte-exact, a Go-issued token completes the handshake, an expired one is refused |
| `net.trunk` | profile, 256 KB CONTROL + 200 KB BULK under loss, coalescing, 2,000-packet bursts with acks, short NS-0.7 run with full 1,200 B datagrams |
| `net.udp` | loopback send/receive, batches, truncation, IPv6/dual-stack (skipped when the OS lacks IPv6), 32 MB buffers, bind errors, SocketTransport batching, NS-0.2 |

**Fuzz targets** (`fuzz/`, `net_fuzz` target): `packet_parser` (HTP framing; property:
canonical re-serialisation), `connection_receive` (reliable headers, fragment reassembly, parser,
ordering buffers, BULK slices, acks), `connect_token` (public parser + netcode AEAD),
`server_datagram` (pre-filter + netcode's read path, seeded with a genuine never-expiring
request), `address_parse`. Each exports `LLVMFuzzerTestOneInput`; with
`-DHELIOS_NET_LIBFUZZER=ON` (Clang) they link libFuzzer (configure the build with
`CMAKE_C_FLAGS/CMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined` for coverage of
helios_net and the vendored read paths). Otherwise they are standalone replayers registered as
CTests (`--smoke 20000` over the committed seeds in `fuzz/corpus/<target>/`; regenerate seeds with
`--make-seeds DIR`).

**Gates** (`net_bench`; `--gate` runs the Phase 0 versions and exits non-zero on failure):

| Criterion | Measurement (this container, GCC 13 RelWithDebInfo, shared 4-core VM) |
|---|---|
| NS-0.1 handshake 1.5 RTT | server 75.6 ms, client 100.7 ms at 50 ms RTT (`net.endpoint`) |
| NS-0.2 loopback 100k pps/core without loss | gated at both levels by `net_bench --gate`: ≈ 480k datagrams/s per core at L0 (send + receive on one core, 1,000,000/1,000,000; also `net.udp`), and 117k encrypted HTP packets/s per core through the full stack with both endpoints on one core (1,162,752/1,162,752 delivered, 10 s, load average 3.5) |
| NS-0.4 fuzzers 1 h clean | harnesses + seeds + CTest smoke; long runs under ASan/UBSan clean (see WP report); the 1 h libFuzzer nightly needs Clang's compiler-rt (absent in this container) |
| NS-0.7 trunk 20k pps × 1,200 B, < 0.1 % drops, ≤ 1 core | `net_bench --gate`, 600 s, full datagrams (1,188 B STATE payload → 1,200 B netcode payload): 11,999,999 of 11,999,999 delivered (20,000 pps, 190.1 Mbit/s payload, 199.7 Mbit/s wire), 0 drops, cell thread 0.26 cores, gateway thread 0.29 cores; `net.trunk` repeats it for 1 s on every test run |

## Known limitations

* Windows batching loops `sendto`/`recvfrom` (WSARecvMsg loops / RIO are the Phase 2 options);
  the Win32 path is compile/link-checked with MinGW here and needs the Windows CI runner.
* IPv6 sockets are exercised only where the OS provides them (this container has no IPv6);
  IPv6 addressing is covered through VirtualNetwork and the Go vectors.
* A peer can disturb reassembly of its *own* fragmented packets (e.g. by sending bogus fragments
  for a future sequence); this cannot affect other sessions, and the reliable layer retransmits.
* reliable's own statistics (`reliableRttMs`, `sentKbps`, …) are sampled every 10 ms; Helios'
  RTT/loss/jitter come from its own per-packet bookkeeping.
* netcode's loopback-client mode is not wrapped; PIE uses VirtualNetwork (full handshake and
  encryption, no sockets) instead.
* A client's `ConnectionStats::rttMs` is sampled from its reliable packets only (RPCs, CONTROL
  time sync), because the gateway acks INPUT-only packets at its tick; until the client has sent
  reliable data `hasRtt` is false and loss detection uses the initial RTO plus the gateway's lazy
  ack delay.
* netcode ignores a connection request while a session with the same client id is connected, so
  a reconnect from a new address (NAT rebinding) waits for the old session's timeout unless the
  gateway drops it first (`Server::findSession` + `disconnect`, driven by the Session service's
  `session_epoch` signal in WP-0.14 / Phase 1).
* The client's 60 pps cap defers a flush's data to the next flush (the next tick); `update()`
  does not send it early.
* `openPrivateConnectToken` calls netcode's non-API (but external) `netcode_decrypt_connect_token_private`;
  a netcode update that makes it `static` fails at link time.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
