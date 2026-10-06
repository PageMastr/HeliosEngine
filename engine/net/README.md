# engine/net — HTP transport (WP-0.13)

`helios::net` (target `helios_net`, HEADLESS, deps `helios::core`, `helios::math`, private
`helios::tp::netcode`) is the Helios Transport Protocol of 04 §2: vendored **netcode 1.4.8**
(connect tokens, encrypted UDP) and **reliable 1.4.5** (packet acks, fragmentation) wrapped with
Helios channels, message reliability, budgets and statistics. The same stack serves client links
and private server trunks. Helios adds **no cryptography of its own**.

```
L0  udp_socket.h   UdpSocket: Winsock2 / BSD, IPv4 + dual-stack IPv6, non-blocking, batch APIs:
                   sendmmsg/recvmmsg batches of 64 (Linux), Registered I/O with polled completion
                   queues (Windows), WSASendMsg/WSARecvMsg or sendto/recvmsg per datagram as the
                   fallback; SO_*BUF(FORCE) sizing, SIO_UDP_CONNRESET off
    transport.h    IDatagramTransport; SocketTransport (batched socket); VirtualNetwork (in-process)
    netsim.h       NetSim: latency, jitter, Bernoulli + Gilbert-Elliott loss, duplication,
                   reordering, bandwidth cap; profiles lan/good/mobile/awful
    address.h      Address: IPv4/IPv6 endpoints, strict parser/formatter (RFC 5952), mapped-address handling
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
client learns it at 2 RTT (measured 2026-09-25: 75.6 ms / 100.7 ms at 50 ms RTT, 0.1 ms simulation step;
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

`net_tests` (doctest, 99 test cases on Linux, 4 of them `perf:`; 101 on Windows, which adds two Registered I/O
cases; `ctest -R net_`):

| Suite | Covers |
|---|---|
| `net.address` | parser/formatter (RFC 5952), rejects, mapped addresses, agreement with `netcode_parse_address` |
| `net.wire` | varints (canonical), every channel round trip, ack-only/padding, every parse error, stream framing |
| `net.congestion` | token bucket, RFC 6298 RTT/RTO clamps, windowed min RTT, jitter, loss window, AIMD decrease/floor/increase/cap/RTT trigger |
| `net.netsim` | latency, Bernoulli loss, determinism by seed, jitter/reorder/duplication, Gilbert-Elliott bursts, bandwidth cap + tail drop, transport decorator, profiles, VirtualNetwork |
| `net.connection` | reliable-ordered exactly-once in-order under 10 % loss + reorder + duplication (both directions), EVENT_U at-most-once, LATEST monotonic, INPUT redundancy, STATE notify invariants, BULK 256 KB, 16 KB CONTROL fragmentation, WouldBlock/TooLarge, window-bytes flow control, reorder-buffer pinning defence, malicious fragments, malformed strikes, budget cap and state budget, 2 ms pacing, AIMD against a 128 kbit/s bottleneck and recovery, RTT/min RTT/jitter/loss stats, ack-only, VOICE, STATE expiry, inbound policing (120 pps; 64 kbit/s game + separate 40 kbit/s VOICE); every arrival order within a 1,024 window and a reversed 16,384 window at O(log n) per message; one gateway packet per tick with reliable acks still ≤ 10 ms; client RTT/loss detection unaffected by tick-paced acks; 60 pps upstream cap at 144 Hz; memory an authenticated peer can pin |
| `net.endpoint` | token handshake + session info, NS-0.1, 16 clients × all channels under 10 % loss/reorder/dup, timeouts + reconnect, graceful disconnects, stale handles, token validation (protocol, key, address, expiry, full, reuse, junk), pre-filter, malformed authenticated peer, server and client handler re-entrancy (send/disconnect from callbacks), real UDP loopback, keyed pre-filter buckets (a precomputed colliding source cannot starve a victim), IPv6 /64 rate limiting, NetSim timing of flush()-time sends, NAT-rebinding reconnect via `findSession` + `disconnect` |
| `net.connect_token` | round trip, tamper detection, **Go golden vectors** (`services/testdata/vectors`): public + private parts byte-exact, a Go-issued token completes the handshake, an expired one is refused |
| `net.bench` | NS-0.2's gate verdict (`bench/ns02_gate.h`): the stack rate gates unless `--advisory ns02-stack`, and only the rate can be advisory; a stack that failed is never also reported as not gated. The machine-wide cross-check (`bench::machineCrossCheck`): work outside the measuring thread is reported and flagged above 25 %, preemption is not, and a busy host makes it invalid rather than flagged; `os::machineCpuTimes()` sees a second thread's CPU that the measuring thread's counter does not |
| `net.trunk` | profile, 256 KB CONTROL + 200 KB BULK under loss, coalescing, 2,000-packet bursts with acks, short NS-0.7 run with full 1,200 B datagrams |
| `net.udp` | loopback send/receive, batches, truncation, IPv6/dual-stack (skipped when the OS lacks IPv6), 32 MB buffers, bind errors, SocketTransport batching, NS-0.2; **batch APIs**: what Auto and each forced API resolve to (Registered on Windows, MultiMessage on Linux, Message otherwise), the same semantics under every API (nothing pending, empty batches, an unreachable destination skipped and counted, partial batches, sender addresses, truncation through receiveBatch and receiveFrom, byte counters), interop between APIs with a 400-datagram burst received only after it is all sent, SocketTransport over every API; the fallback when Registered I/O is unavailable or its set-up fails, forced by test hooks (`detail::forceRegisteredIo*` in `src/net_internal.h`; Windows: Message on the same fixed port, non-blocking, a round trip each way, one warning per process naming the failing step and its error code; elsewhere the hooks change nothing); and on Windows RIO's 2 KB slot limits and its slot counts (reported as buffer sizes: 15,887 receive and 2,048 send slots for 32 MB, 128 each for 64 KB, where a 400-datagram burst is committed in steps of at most 128, would-block counted) |
| `net.netcode_patches` | `0001-write-bytes-memcpy`: bytes and pointer advance unchanged for 0–1,200 bytes and negative counts; `perf:` a 1,200 B payload in ≤ 0.5 µs (1.28 µs unpatched, 0.018 µs patched here) |
| `net.netcode_crypto` | the bundled libsodium (owner decision 2026-10-05): its CPU probe sees AVX2 and ChaCha20 dispatches to the AVX2 kernel (observed by swapping a spy into each kernel's table for one call); every compiled kernel (reference, SSSE3, AVX2) matches RFC 8439 §2.4.2 and the reference kernel at 17 lengths around the block sizes; `sodium_memzero` clears exactly its range; `perf:` it wipes 64 KB at least 4× faster than a volatile byte loop (1.4 µs against 27 µs here). Without `HAVE_AVX_ASM` the first case fails (SSSE3 dispatched), without `HAVE_EXPLICIT_BZERO` the `perf:` one does |

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

**Gates** (`net_bench`; `--gate` runs the Phase 0 versions and exits non-zero on failure). The hosted
Linux nightly (`linux-gcc`) runs `net_bench --gate --advisory ns02-stack`: the encrypted stack's 100k
packets per core is printed with an `NS-0.2 advisory:` line instead of failing, on the repository owner's
approval of 2026-09-30
([`docs/evidence/ns-0.2-owner-approval-2026-09-30.md`](../../docs/evidence/ns-0.2-owner-approval-2026-09-30.md),
09 §5.6), because hosted Linux measured it at 88k–132k with no code change. Nothing else relaxes: raw
datagrams, loss at either level, a stack that never sent, and NS-0.7 still fail the run, and `--advisory`
takes no other name and needs `--gate` (exit 2 otherwise; `bench/ns02_gate.h`, the `net.bench` doctests
and the `net_bench_advisory_*` CTests). A stack that fails gets only its failure line, never also the
advisory one. Hosted Windows is not covered (its stack measured below 100k on 2 of 3 runs and 125,611 on
2026-10-03, and the owner has not confirmed the approval for it), so its nightly step runs `net_bench --gate`
without the flag, as do local runs and fixed hardware. **The approval lapsed on 2026-10-04** (09 §5.6), when
the fixed-hardware re-test it owed measured the stack below 100k on the `win-gpu` runner (82,842 packets per
core); after WP-0.13r the runner passed strictly (118,253). The Linux nightly keeps the flag until the owner
answers whether to re-confirm the approval (09 §8.1, WP-0.13 row).

| Criterion | Measurement (WP-0.13, 2026-09-25; this container, GCC 13 RelWithDebInfo, shared 4-core VM) |
|---|---|
| NS-0.1 handshake 1.5 RTT | server 75.6 ms, client 100.7 ms at 50 ms RTT (`net.endpoint`) |
| NS-0.2 loopback 100k pps/core without loss | the owner's approval for hosted Linux lapsed on 2026-10-04 (above); gated at both levels by `net_bench --gate`. Medians of five 10-minute gate runs on 2026-10-05 (WP-0.13r): **170k encrypted HTP packets/s per core** through the full stack with both endpoints on one core, 5.9 µs per packet (156,803–174,430; every packet delivered; load average 2.7–6.7), against 113,516 (104,734–119,751, load 0.6–2.7) on main the same day; and ≈ 540k datagrams/s per core at L0 (send + receive on one core, 1,000,000/1,000,000; also `net.udp`), 474k on main the same day, a path WP-0.13r does not change on Linux |
| NS-0.4 fuzzers 1 h clean | harnesses + seeds + CTest smoke; long runs under ASan/UBSan clean (see WP report); the 1 h libFuzzer nightly needs Clang's compiler-rt (absent in this container) |
| NS-0.7 trunk 20k pps × 1,200 B, < 0.1 % drops, ≤ 1 core | `net_bench --gate`, 600 s, full datagrams (1,188 B STATE payload → 1,200 B netcode payload): 11,999,999 of 11,999,999 delivered (20,000 pps, 190.1 Mbit/s payload, 199.7 Mbit/s wire), 0 drops, cell thread 0.26 cores, gateway thread 0.29 cores; `net.trunk` repeats it for 1 s on every test run |

### NS-0.2 per-packet budget (WP-0.13r)

NS-0.2's 100k packets per core is a CPU budget of **≤ 10 µs per encrypted packet, send and receive together,
on one core** (`bench::kNs02BudgetMicrosPerPacket`; `net_bench --stack` prints the measured µs per packet
beside it). The gate counts the measuring thread's CPU. Its wall time shows only preemption: the loop never
sleeps or blocks, so the thread accrues CPU at the wall-clock rate even if the OS does part of each packet's
work elsewhere. So `net_bench --socket` and `--stack` also print a **cross-check** line: the whole machine's
busy CPU per datagram or packet over the run (`GetSystemTimes` on Windows, `/proc/stat` on Linux), less the
background load measured for 0.5 s on either side of it, beside the thread's figure. It warns when the machine
spent more than 25 % more than the thread (`bench::kOffThreadLimitPercent`), as it would if Registered I/O
completed sends or receives on other CPUs, and says the figure means nothing when the background exceeds a
quarter of the CPUs. It never changes a verdict: whether NS-0.2 should count that work is the owner's call
(see the PR). In this container, on Linux, all the work is on the thread: at idle moments (background ≤ 0.08
cores) it read −6.8 % to +3.1 % for raw datagrams and −6.2 % to +1.4 % for the stack (2026-10-05, GCC; Clang
+0.7 %), and under load (background ≈ 2 of 4 CPUs) it says "not meaningful". The Windows readings come from
the hosted Windows nightly and `win-gpu`. The win-gpu runner (AMD Ryzen 5 5500, Windows 11) measured 12.1–12.2
µs (82,842 and 82,134 packets per core, 2026-10-04/05) against 6.2 µs for a raw datagram, so both the Windows
socket path and the stack's own work had to shrink.

Where a packet's CPU goes (`perf record -e cpu-clock` of `net_bench --stack 8`, GCC 13 RelWithDebInfo, this
container; 700-byte EVENT_U messages, one per ≈ 730-byte datagram, received on the same thread):

| Component | main (aa80a1d) | WP-0.13r |
|---|---|---|
| ChaCha20, encrypt + decrypt | 24.7 % (SSSE3 kernel) | 30.6 % (AVX2 kernel) |
| Poly1305 (SSE2) and the AEAD wrappers | 10.1 % | 13.7 % |
| `sodium_memzero` | 12.0 % (volatile byte loop) | 0.2 % (`explicit_bzero`) |
| `netcode_write_bytes` | 11.5 % (a call per payload byte) | 0.2 % (patch 0001) |
| netcode, other (framing, queues; replay protection 0.04 %) | 2.7 % | 3.4 % |
| reliable (acks, sequence buffers) | 3.5 % | 4.2 % |
| Helios (channels, packing, transport, socket wrappers, allocation tracking) | 5.7 % | 7.2 % |
| libc copies and allocations, mimalloc | 1.8 % | 2.6 % |
| Kernel: `sendmmsg` with loopback delivery (≈ 21 % on main), `recvmmsg` (≈ 5 %), page faults | 28.2 % | 37.7 % |
| The bench harness (loop, clock reads, handler) | 0.2 % | 0.2 % |

Helios' own code is about 6 % and allocations about 1 % (mimalloc plus the "Net" tag accounting), with no
Helios function above 0.6 %, so the cuts are in vendored code, each a reviewed vendoring change:
- **netcode patch `0001-write-bytes-memcpy`** (third_party/MANIFEST.md, "Patches"): `netcode_write_bytes`
  copied every payload byte through a function call;
- **the bundled libsodium's own faster settings**, by the repository owner's decision of 2026-10-05
  ([`docs/evidence/netcode-crypto-owner-decision-2026-10-05.md`](../../docs/evidence/netcode-crypto-owner-decision-2026-10-05.md)):
  `tp_netcode` is compiled with `HAVE_AVX_ASM` (GCC/Clang, x86-64), without which the library's CPU probe
  never read XCR0, never saw AVX2 and kept the SSSE3 ChaCha20, and `HAVE_EXPLICIT_BZERO` (glibc), without
  which `sodium_memzero` stored one byte at a time. No vendored source changes; `net.netcode_crypto` shows the
  probe, the dispatch and that every compiled ChaCha20 kernel matches RFC 8439.

Together they take the stack from a median of 113,516 to 169,653 packets per core in this container, 8.8 to
5.9 µs per packet (five 10-minute `net_bench --gate` runs each; the NS-0.2 row of the table above).

Not changed: MSVC builds compile neither the donna64 nor the SSE2 Poly1305 (no `__int128`) and run the
portable donna32 kernel, which costs about 0.6 µs more per packet here (forced donna32: 149k against 164k
packets per core); that would need a source patch or another Windows compiler.

**Windows.** `UdpSocket` now batches with Registered I/O (04 §2.6): one kernel entry per batch of sends or
re-posted receives instead of one `sendto`/`recvfrom` per datagram, and none to read completions. MSVC builds
should, by the code, already select the AVX2 ChaCha20 (`/arch:AVX2` and `_xgetbv`) and wipe with
`SecureZeroMemory`, so on Windows the cuts are the socket path and patch 0001. Windows numbers come only from
the hosted Windows nightly and the owner's `win-gpu` runner.

## Known limitations

* The Win32 batch path (Registered I/O, and the WSASendMsg/WSARecvMsg fallback) is compiled with MinGW
  here and runs only on the Windows CI runners and the owner's `win-gpu` runner. Registered I/O copies
  each datagram through a registered 2 KB slot, so it refuses sends of 2,048 bytes or more and drops larger
  datagrams as truncated; HTP datagrams are at most 1,300 bytes. A RIO socket buffers nothing beyond its
  posted receives (Windows CI: 128 posted receives took 128 of a 400-datagram burst), so the receive slot
  count follows `receiveBufferBytes` (128–16,384) and the send slot count `sendBufferBytes` (128–2,048;
  sends complete within microseconds), and `receiveBufferBytes()`/`sendBufferBytes()` report that slot
  capacity, not `SO_RCVBUF`/`SO_SNDBUF` (so NS-0.7's `rcvbuf` figure on Windows is the receive slots).
  The slot counts assume full 2 KB datagrams: a RIO socket holds 3,971 queued datagrams of any size at the
  8 MB default, where a plain 8 MB socket holds tens of thousands of 100-byte ones. The region is locked
  memory: (3,971 + 2,048) × 2,112 B ≈ 12.7 MB per socket at the 8 MB defaults and (15,887 + 2,048) × 2,112 B
  ≈ 37.9 MB per 32 MB trunk socket (67.1 MB before send slots were capped). Today's gateway opens one trunk
  socket per cell (`GatewayServer::openTrunk`), so a Windows gateway locks ≈ 37.9 MB per cell it serves; 04 §2.6's trunk
  budget (4 trunk IO threads per gateway box, each with one 32 MB socket: "4 × 32 MB of kernel socket
  buffers") would be ≈ 152 MB locked under RIO. When RIO is unavailable or its set-up fails (for example,
  `RIORegisterBuffer` refusing that much locked memory), `open()` falls back to the per-datagram path and
  logs it once per process as a warning. That fallback polls a non-blocking socket rather than using
  IOCP: 04 §2.6's IOCP + `WSARecvMsg` fallback belongs to the Phase 2 trunk IO threads, which block
  between bursts.
* MSVC builds run the bundled libsodium's portable donna32 Poly1305 (no `__int128`); see "NS-0.2 per-packet
  budget".
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

Plan-Rev: 13

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 12 (09 §5.6's owner approval)
on 2026-10-03: the approval adds `net_bench --advisory ns02-stack` and changes nothing in `helios_net`.
Re-checked at revision 13 on 2026-10-05 (WP-0.13r): revision 13 changes 02 §5.5–5.6 and 07 §1.7.1 only.
WP-0.13r implements 04 §2.6's Windows batch I/O (Registered I/O with a polled completion queue per
socket). **One declared deviation from 04 §2.6:** the fallback where RIO is unavailable is a polled
non-blocking socket with `WSARecvMsg`/`WSASendMsg`, not IOCP + `WSARecvMsg` (the WP-0.13r brief allows either;
reason under Known limitations; recorded in PR #62's description and 09 §8.1's WP-0.13 row). It is not a
conformance delta in 09 §5.10's sense (no plan text is superseded by code that keeps an older decision); no
such delta is open (§5.10.4 (c)).
