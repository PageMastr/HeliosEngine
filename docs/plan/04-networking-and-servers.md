# 04 — Networking & Servers

*Helios master plan, section 04. Draft v5 (round-5 minor revisions: load-dependent co-location and handoff
decisions logged as `ColocDecision` and `HandoffDecision` replay events, the overload row corrected and a
multi-cell clause in NS-3.8; replay keyframes cut every 30 min through a script rebase, with a keyframe clause
in NS-2.4; the Luau `codegen-fornloop-fuel` and `fuel-counter` patches named, §10.2, §6.2a, §6.3; from 02's
round-5 fix, Jolt's solver order by stable body keys, §5.3, §6.7 residual, §10.2; round-4 review fixes: co-location protocol with set caps, leash and
location-based presence §6.2a; production `MigrationHold` with a bounded zone-wide hold and hitch counts per
rollout, `sim_abi` portable residuals §6.7; replicant recovery for every persistent world zone §6.4; trunk IO
pool, per-trunk buffer sizing and zone-instance affinity for gateway assignment §2.6; Luau math, hitbox
sampling, sort ties and MXCSR brought under the replay contract §10.2; from 05's round-4 fix, make-before-break
gateway relocation §2.4, §6.6 and the fence gate §6.1; from 02's round-4 fix, stage 3's terrain-fence cost
bound §3.3; round 3: planned region migration with `Drain` and
`PreProvision` §6.7, lag compensation across cell boundaries §5.6, exact effect de-duplication across recovery
§6.2, gateway loss under view composition §6.6, tick-consistent replicant restore §6.4; round 2: v1 view
composition §6.6, AG dormancy §6.1, gateway-box failure sizing §2.6; earlier: replicant tier per ADR-007). Conforms to
ADR-001/002/004/005/007/008/009/010/011–016; phases per
`README.md`. Research cited as `R07-P0-3` (report 07, requirement P0 item 3); R03/R06 use their own IDs. Go
services are specified in 05-backend-services; only touchpoints are defined here.*

## 0. Design rules

1. **Session ≠ simulation.** Clients talk only to gateways; cells are private and replaceable (R07 finding 1).
2. **Single writer.** Every replicated entity is in one *authority group* (AG) owned by one cell at one *epoch*; others hold read-only *ghosts*.
3. **Every seam exists at v0.** One cell per zone still uses handoff messages, epochs, fences and route changes; v1/v2 add capacity, not concepts (R04 §9).
4. **Hot path never enters Luau** (ADR-002, R01-P0-4).
5. **Budgets are explicit and CI-tested:** tick ms, bytes/client, entities/zone, memory/process.
6. **Client is untrusted:** it sends intents; the server decides every outcome of value.
7. **Windows parity:** every server runs on Windows for development (ADR-001).
8. **Replayable by contract:** every scheduling decision that can change simulation state is either a pure
   function of recorded inputs or is itself recorded with the tick it took effect (§10.2). Wall time decides
   *when* work runs, never *what* a tick computes.

## 1. Process topology

```
Client ──HTTPS──► Go backend (05): Login queue · Session/tokens · Orchestrator · Persistence GW+Fence · Ledger · Chat
  │                        ▲ nats.c req/reply: fence, ledger, ckpts   ▲ NATS: leases, load, control, service RPC
  │ HTP (UDP, public)      │                                          │
  ▼                        │                                          │
┌─────────────────────┐ HTP trunk ┌──────────────────────────────────┴──────────────────────────┐
│ Gateway ×N (C++)     │◄────────►│ helios-cell A: ZoneInstance "Sol-Prime/A", "raid-123", …      │
│ crypto·limits·routes │ (private)│ [ECS · Jolt grids · Luau VM · ZoneClock · Replication · AGs]  │
└─────────────────────┘           └──────────────▲ cell mesh: ghosts, handoff, effects ─────────┘
  ▲ HTP trunk (VOICE frames)                     ▼
  ▼                                helios-cell B, C, …  ── VoiceAudience deltas ──┐
┌─────────────────────────────┐                                                   │
│ helios-voice ×N (Phase 3)    │◄──────────────────────────────────────────────────┘
└─────────────────────────────┘
```

| Process | Owns | Never |
|---|---|---|
| **Client** (C++) | Input; prediction of own avatar/ship/abilities; interpolation; content streaming | Decides outcomes |
| **Gateway** (C++) | netcode instances, channels, rate limits, routes, chunk packing, ack translation, v1 view composition (viewer registration at every contributing cell, per-session budget arbitration, §6.6), non-spatial RPC (chat, social, market) → Go via NATS | Game logic, interest, priority |
| **Cell** (C++) | Authoritative simulation: ECS, Jolt, AI, Luau, interest, replication, AGs, ghosts, handoff, lag compensation | Public traffic; SQL; value writes except via Ledger |
| **Replicant** (C++, `helios-cell --replicant`, Phase 4) | Latest state of every AG owned by ≤ 4 cells of persistent world zones (one zone or several), in RAM, streamed from their serialize-once gather; serves it to a standby after a cell crash (§6.4, ADR-007) | Simulation, authority, clients, persistence writes |
| **World-script host** (C++, `helios-cell --role world-script`, Phase 3) | The studio's world-script partitions (05 §1.23): one Luau VM per partition, each partition an orchestrator region with a lease and `lease_gen`; RPCs from cells over the NATS service lane, never per tick | Zone simulation, clients, SQL (commits go through the Go `worldscript` service), debiting a character |
| **Voice forwarder** (C++, `helios-voice`, Phase 3) | SFU-style Opus forwarding for party, fleet, org, ship-intercom and proximity channels; speaker selection; abuse-evidence ring (§2.7, ADR-015) | Decoding, mixing, game logic |
| **Orchestrator** (Go) | Registry, region leases (generations and leadership in PG; liveness from 1 Hz heartbeats plus two-signal failure detection, 05 §1.4), placement, instances, routes, zone-leader assignment and handle blocks (§3.5), TiDi policy, split/merge (v2) | Per-tick traffic |
| **Session svc** (Go) | Connect tokens after login queue; `session_epoch` (Valkey CAS); reconnect tickets | — |
| **Persistence GW** (Go) | Batched epoch-fenced checkpoints, validated against a fence cache (§6.1); **Fence table** (owner, epoch, tree root per persistent AG, CAS) | Value (Ledger's job) |
| **Ledger** (Go) | Item/currency moves: synchronous, idempotent, fenced by (AG, epoch) | — |

**Orchestrator touchpoints:** `RegisterProcess`, 1 Hz lease heartbeat, `AssignRegion/ReleaseRegion(instance,
region, lease_gen)`, `AssignZoneLeader(instance, cell, leader_gen)`, `LeaseHandleBlocks(instance, cell, n)`,
`Create/DestroyInstance`, `ReportLoad` (1 Hz: tick p99, players, egress, dilation, overload stage),
`ResolveRoute`, `TransferPlayer`, `Drain(process, reason, deadline)` and `PreProvision(zone, time, size)`
(both planned region migrations, §6.7), `Split/Merge` (v2). **Persistence** (all NATS
request/reply, async to the tick, §6.1): `Fence.Advance(root, expected, next, owner)` (CAS on an AG tree,
~1 ms), `Fence.Join/Leave` (boarding, docking), `Fence.AdvanceMany` (bundle handoff),
`Fence.Park(root, e, cell, ckpt_seq)` (logout, linkdead expiry, instance or structure unload; §6.1),
`Fence.AdvanceOwnedBy(region, lease_gen, owner)` (recovery of *active* rows, §6.4),
`Fence.MigrateRegion(region, lease_gen, owner, manifest)` (its cooperative variant for planned migration,
§6.7), `Checkpoint.Write` (async
1 s batches; per AG every 30 s while dirty and on logout/zone exit), `Checkpoint.Load`. **Control from the
fence:** `ctl.<shard>.cell.<id>.released{ag, epoch}` tells a superseded owner to drop its copy (§6.1).

**Links:** client↔gateway HTP on UDP 7777 (the only public game port, also carrying voice); gateway↔cell,
cell↔cell, gateway↔voice and cell↔voice HTP trunks (§2.6, §2.7); cell/gateway→Go via **nats.c v3.14**
request/reply and JetStream with schema-codegen binary payloads, never per tick, no protobuf/gRPC in C++
(ADR-013). Handoff, replication and voice never cross NATS (ADR-008).

| | Windows dev box | Linux production |
|---|---|---|
| Go backend | `helios-backend.exe`: all services in-process, embedded NATS, embedded-postgres, miniredis (ADR-014) | Per-service Deployments, Postgres HA, Valkey, NATS cluster |
| Gateway | `helios-gateway.exe` on `127.0.0.1:7777` (no firewall prompt), voice forwarder in-process (`--embedded-voice`); PIE may use `helios-cell --embedded-gateway` (in-process trunk) | bare metal, anycast behind scrubbing, spread evenly over the shard's 3 availability zones, sized N+1 with 20 % headroom for a box loss (§2.6) and so that two zones hold every session (05 §6.7): 6 boxes at Ph3, 12 at Ph4; `helios-voice` ≥ 2 per shard |
| Cells | One `helios-cell.exe` for all dev zones; PIE spawns it + clients/bots; `helios-dev up` starts everything | Agones Fleets per zone class, warm pool; replicants for every persistent world zone (Phase 4, §6.4) |

Windows specifics: ticks paced by high-resolution waitable timers; server suites run in Windows CI.

## 2. Transport: HTP = Helios channels on reliable on netcode

**Decision (ADR-013, R10 §7):** vendored **netcode v1.4.8** (connect tokens, encrypted UDP, replay protection,
via its libsodium subset) and **reliable v1.4.5** (packet acks, RTT/loss/bandwidth, fragmentation), built by our
CMake (`/MT`, allocators on tagged mimalloc heaps). Helios adds channels, message reliability, budgets and
replication, and **no cryptography of its own**. The same stack serves clients and private trunks.

Layers: **L0** `helios::net::UdpSocket` (Winsock2/BSD, plugged in via netcode's `override_send_and_receive`:
`recvmmsg/sendmmsg` batching, `SIO_UDP_CONNRESET` off, 8 MB buffers, dual-stack IPv6, per-IP pre-filter, NetSim)
→ **L1** netcode (tokens, challenge, ChaCha20-Poly1305, 256-entry replay window, keep-alive, timeout, slots,
loopback clients for PIE) → **L2** reliable (sequence, acks, RTT/loss/bandwidth) → **L3** Helios channels
(`engine/net`) → **L4** replication/RPC (§4).

### 2.1 Packet layout

netcode payloads ≤ 1,200 B → datagrams ≤ 1,273 B with IPv6+UDP, inside the 1,280 B IPv6 minimum MTU.

| Field | Size | Layer |
|---|---|---|
| Prefix: type in low 4 bits (0 request, 1 denied, 2 challenge, 3 response, 4 keep-alive, 5 payload, 6 disconnect), sequence byte count in high 4 bits | 1 B | netcode |
| Packet sequence (AEAD nonce + replay window) | 1–8 B | netcode |
| Ciphertext (AD = version ‖ `protocol_id` ‖ prefix) + MAC | ≤ 1,200 + 16 B | netcode |
| ↳ reliable header: flags, sequence u16, ack 1–2 B, ack bits 0–4 B | 3–9 B | reliable |
| ↳ Helios messages: `hdr` u8 (channel 4 bits, slice bit, sequenced bit, 2 reserved), `msg_seq` u16 (ordered/sequenced channels), `len` varint, payload | ≤ ~1,190 B | Helios |

The connection request (type 0, unencrypted, 1,078 B: version, `protocol_id`, expiry, nonce, sealed 1,024 B
private token) is larger than any reply, so it cannot amplify. Challenge/response carry a sealed 300 B token.

### 2.2 Channels

| ID | Channel | Semantics | Use |
|---|---|---|---|
| 0 | CONTROL | reliable-ordered, first | time sync, TiDi, route/zone change, reconnect tickets, kick |
| 1 | INPUT | unreliable, last 4 inputs redundant | command frames c→s |
| 2 | STATE | unreliable + **delivery notify** | replication chunks; loss repaired by replication (§4.2) |
| 3 | EVENT_R | reliable-ordered | gameplay RPCs, inventory results, UI, chat |
| 4 | EVENT_U | unreliable | VFX/audio cues, hit markers |
| 5 | LATEST | unreliable-sequenced | aim/camera look, voice activity |
| 6 | BULK | reliable-ordered, 1 KB acked slices (window 64), lowest priority | blobs ≤ 256 KB |
| 7 | DEBUG | reliable; compiled out of shipping | inspector, dev cheats |
| 8 | VOICE | unreliable, per-stream `u16` sequence, receiver jitter buffer, never retransmitted; **not tick-packed** | Opus frames c↔s (§2.7, Phase 3) |

A reliable message rides in packets until reliable acks one of them (`RTO = srtt + 4·rttvar`, 30 ms–1 s; ≤ 1,024
in flight per channel, else `send()` returns `WouldBlock`). A packet is declared lost when one ≥ 3 sequences
newer is acked or after 1.25 × srtt; losses drive STATE repair. STATE and INPUT never exceed one datagram;
reliable's fragmentation serves only rare CONTROL bursts (≤ 16 KB). Packing order: CONTROL > EVENT_R >
INPUT/STATE > EVENT_U > LATEST > BULK. VOICE is sent on arrival in its own datagram, or appended to a game
packet leaving within 5 ms, and is charged to its own budget (§2.5). Channels 9–15 are reserved.

### 2.3 Connect tokens and handshake

The **Session service** mints netcode-format tokens in Go after the login queue (`x/crypto`
XChaCha20-Poly1305; CI interop test against vendored netcode) and returns them over HTTPS. **Public part:**
`protocol_id` (build-compatibility gate, derived from the protocol version and the content compat epoch,
05 §1.14.1), expiry **≤ 45 s**, timeout 10 s, 2–4 gateway instance addresses,
per-session keys, and the 1,024-byte private part sealed under the shard key (Session service and gateways
only). **Private part:** `client_id` = session ID, addresses, keys, **256 B user data** (character ID,
content pin `(compat_epoch, manifest_hash)`, placement ticket, entitlements, attestation hash, §9).

Handshake: the gateway validates protocol, expiry, decryption, its own address and token reuse from another
address (failures dropped silently); challenges without per-client state; the response proves address
ownership and assigns a slot; the connect callback then calls `ResolveRoute` and attaches the placement cell.
Policy denials (ban, version, queue) happen at issuance; gateways only deny "full". The shard key rotates
daily: instances on the new key take new tokens while old-key instances drain (tokens live ≤ 45 s).
**Forward secrecy:** netcode has none (a leaked shard key exposes recorded sessions); the key
lives in the secret store, and a Monocypher X25519 in-band re-key (small vendored patch) is an option for the
Phase 4 security review.

### 2.4 Keep-alive, timeouts, reconnect

netcode keep-alives run at 10 Hz when idle; timeout is the token's 10 s (5–15 s). A timed-out session is
*linkdead*: routes held, avatar **stays in the world and vulnerable** (R05 §7.4). netcode has no resume, so every
60 s the gateway sends a **reconnect ticket** on CONTROL (sealed by the Session service, valid 5 min: session,
character, zone, `session_epoch`). After a timeout, gateway crash or NAT rebinding the client trades it over
HTTPS for a fresh token, bypassing the login queue (R07-P0-3); the Session service CAS-increments
`session_epoch` (the old gateway drops its copy), and the new gateway sends `ClientRebind` to the home cell
and, in a multi-cell zone, to **every** contributing cell it registers (§6.6). Baselines are invalidated, and
the client keeps its entity table and sends it as `HeldEntities`, so the resync is full-mask updates in
priority order, not creates. Target 1–3 s.

- **Linkdead ends** after 5 min (the ticket's validity) or at an explicit logout. The cell then drains the
  AG's in-flight ledger requests, writes the final checkpoint and **parks** the AG (`Fence.Park`, §6.1).
- **Relogging while linkdead.** A fresh login (not a ticket) for a character whose fence row is *active* is
  routed to the owning cell and rebinds like a reconnect. Only if that owner is suspect, or runs another
  content version, does the new placement take the row over; the old owner then receives `released`
  (§6.1).
- **Reconnect storms.** A gateway box holds up to 8k sessions (§2.6), and all of them lose it at once. Clients
  therefore use full jitter: the first ticket redemption after a lost gateway waits a uniform 0–2 s, then
  backs off 0.5, 1, 2, 4 s, each ±50 % (08 §1.2). The Session service redeems a burst of **10k tickets in
  ≤ 5 s at p99 < 250 ms per call** per shard (NS-4.7), and reconnects never consume login-admission slots
  (AAA-SRV-7), so new logins keep flowing during a storm. Time budget per session: 2 s to detect the loss
  (08 §1.2), ≤ 2 s of jitter, ≤ 0.25 s to redeem, ≤ 0.5 s for handshake and `ClientRebind`, and ≤ 1 s for the
  near-tier resync, ≈ 6 s in all against NS-4.7's 10 s.
- **Planned relocation (Phase 4).** A gateway drained for a deploy or host maintenance moves its sessions
  make-before-break instead of dropping them (05 §6.3.1).
  - It sends `Relocate{token}` on CONTROL. The token is minted by the Session service for another box.
  - The client connects there while the old connection keeps carrying its input and state, and it sends each
    input on both connections until the move completes.
  - The new gateway sends `ClientRebind{s, session_epoch + 1, mode = relocate}` to every contributor (§6.6).
  - The old gateway forwards until every contributor has fenced its path, then sends `PathClosed`.

  No session goes linkdead, baselines survive, and nothing is re-created. 05 §6.3.1 specifies the steps, the
  pacing (≤ 500 relocations/s per box), the failure cases and the acceptance (BE-A14 Ph4).

### 2.5 Congestion and bandwidth

Per-connection **budget** from reliable's estimates: 256 kbit/s by default; in battles 512 kbit/s through Phase 4
and up to 1,000 kbit/s from Phase 5 (AAA-SRV-6, R07 §8.3). AIMD: loss > 5 % or
`srtt > min_rtt + 100 ms` for 1 s → ×0.7 (floor 64 kbit/s); 5 s clean → +16 kbit/s per second. One packet per
tick (≤ 4 when budget allows, paced 2 ms). Upstream `min(tick_hz, 60)` pps under a **64 kbit/s token bucket**.
The replication writer gets `budget/8/tick_hz − reliable backlog` bytes per tick; in a multi-cell zone the
gateway divides that among contributing cells as byte grants (§6.6). Replication is bit-packed,
never compressed; BULK uses zstd with a trained dictionary.

**Voice** (Phase 3, §2.7) has its own allowance outside the STATE budget: **≤ 40 kbit/s up** while
transmitting (a separate token bucket, not the 64 kbit/s input bucket) and **≤ 104 kbit/s down** (≤ 3
concurrent streams at ≈ 34 kbit/s each on the wire). It stays inside the AIMD envelope: when AIMD pushes the
connection below its default budget, the gateway cuts a listener's voice to one stream (the channel's
priority speaker) before STATE loses more than 10 %. Voice adds ≤ 25 pps up (inside §9's 120 pps upstream cap)
and ≤ 75 pps down; gateway sizing (§2.6) assumes an average of ≤ 15 voice pps per session, which NS-3.9
measures.

### 2.6 Gateway scaling and trunks

**Gateway instances.** A netcode server is single-threaded and sized for hundreds of clients (R07 §3.1), so a
gateway runs **one netcode instance per UDP port**, each pinned to one worker thread with ≤ 256 slots; the
Session service fills instances by **zone-instance affinity**, then by reported free slots (below). Per session
the load is ≈ 20–30 pps down, 20–30 pps up and ≤ 15 voice pps on average, so one instance handles ≈ 20k pps.
Capacity steps, each gated by a criterion: **1 instance per core, 2,048 sessions per 8-core box** in Phase 2 (NS-2.6), then **4 instances per core, 8k
sessions per 8-core box** in Phase 4 (NS-4.4) after profiling.

**Trunks** (gateway↔cell, cell↔cell, gateway/cell↔voice) are netcode connections with orchestrator-minted
trunk tokens. Messages carry a varint stream ID (session or AG), and handoff blobs use BULK slices.
- **Cells never build client packets.** They emit per-connection **replication chunks** (≤ 1,100 B). The
  gateway packs them (1:1 in v0) and maps client acks and losses back to per-cell notifications. In v1 it also
  registers each session as a viewer at every cell whose regions its view reaches, and splits the session's
  budget across those contributing cells by weighted fair share (§6.6). v2 then relocates replication into
  the replication layer without a wire change.
- **Trunk IO: a fixed thread pool, not a thread per trunk.** Every trunk endpoint runs a fixed pool of trunk
  IO threads: **4 per 8-core gateway box, 2 per cell process, 1 per replicant or voice forwarder**. Each thread
  owns one UDP socket (32 MB buffers) with one netcode server endpoint for inbound trunks (≤ 256 slots), and it
  multiplexes the outbound trunk connections it opened on the same socket through L0's
  `override_send_and_receive`, demultiplexed by peer address. On Linux each thread runs one `epoll` set with
  `recvmmsg/sendmmsg` batches of 64 and a 50 µs busy-poll before sleeping under load. On Windows it uses
  Registered I/O with one polled completion queue per thread (`RIODequeueCompletion`), and falls back to IOCP
  with `WSARecvMsg` where RIO is unavailable. A new trunk goes to the thread with the lowest expected pps, so
  thread count never grows with trunk count.
- **Trunk profile, sized by traffic.** reliable's sent/received packet tables hold
  `next_pow2(peak_pps × 0.2 s)` entries, at least 256 and at most 4,096; 4,096 covers 200 ms at 20k pps, where
  the default 256 would cover only 13 ms. A trunk's state is ≈ 48 B per entry pair plus ≈ 4 KB of netcode
  state: ≈ 16 KB for a sparse trunk and ≈ 200 KB for a 20k-pps one. Fragments reassemble up to 256 KB in an
  arena taken only while a fragmented message is in flight. Small chunks for sessions on the same gateway are
  **coalesced** into one ≤ 1,200 B trunk datagram.
- **Trunk budget per 8-core gateway box (Phase 4):** ≤ **1,024 trunk connections**, ≤ **64 MB of trunk
  state** (plus 4 × 32 MB of kernel socket buffers) and ≤ 1.5 cores of trunk IO at 8k sessions.
  `gw_trunk_conns` alarms above 768. A cell holds ≈ 25 trunks (12 gateway boxes, ≤ 8 neighbours, its
  replicant, voice and world-script hosts) on its 2 IO threads.
- **Load.** A 500-player cell emits ≈ 128 Mbit/s (§4.7), or ≈ 13.3k datagrams/s if every session sat on one
  gateway. Per datagram the cost is ≈ 0.8 µs of ChaCha20-Poly1305 for 1.2 KB, ≈ 0.3 µs of reliable bookkeeping
  and ≈ 0.5 µs of batched syscalls: ≈ 2 µs, or ≈ 4 % of a core at 20k pps. That leaves ~20× headroom for
  netcode's copies, and NS-0.7 measures it.
- **Sharding.** Each cell↔gateway pair opens `K = ceil(pair_peak_pps / 8,000)` trunk connections (K ≤ 8),
  from the pair's measured peak, re-evaluated every 60 s: connections are added at once and retired after
  10 min below the threshold. The K connections of one pair sit on different IO threads. With affinity a
  500-player cell's sessions sit on 2–3 boxes, so K = 1 per box (≈ 4.4–6.7k pps each); K = 2 is the case of a
  cell whose sessions all sit on one box. Sessions map to trunks by `session_id mod K`, so per-session order
  holds. Cell↔cell trunks shard by `hash(root AG) mod K`, and a handoff blob travels on its AG's trunk, ordered
  with that AG's effects.
- **Zone-instance affinity (Session service, 05 §1.3).** "Zone" here is the game zone instance, not an
  availability zone. A token lists instances from the target zone instance's **affinity set**, the boxes
  already carrying its sessions, while the chosen box (a) keeps the box-loss
  headroom below, (b) keeps the availability-zone rule below, and (c) holds ≤ 50 % of that zone instance's
  sessions once it has > 200, so one box loss never takes most of a zone. Otherwise the service picks the box
  with the most free slots and adds it to the set; a set with > 200 sessions spans ≥ 2 boxes in ≥ 2
  availability zones. Reconnect tokens use the session's current zone. Zone transitions are route changes
  (§7) and keep the gateway, so affinity decays over a play session, and the trunk budget above is sized for
  the worst case with no affinity at all. From Phase 4, a box above 512 trunk connections repairs drift with
  make-before-break relocation (05 §6.3.1, reason `rebalance`): it moves sessions whose zone instance's
  affinity set excludes it, at ≤ 50 relocations/s per box, skipping sessions in combat or in zones at overload
  stage ≥ 4. A relocation keeps baselines and never goes linkdead, so the repair costs a player at most one
  ≤ 250 ms input gap (BE-A14).
- **Mesh model (Phase 4: 50k CCU, ≈ 200 populated cells, 12 boxes).** With no affinity every box talks to every
  populated cell: ≈ 200 cell trunks plus voice and world-script trunks, ≈ 250 per box, all K = 1, ≈ 4 MB of
  trunk state. With affinity it is ≈ 60–100. Both are far inside the budget. Datagram count follows sessions,
  not trunks, so AEAD and syscall cost do not change; scattering costs only coalescing. At worst a cell sends
  one extra part-filled datagram per trunk per tick (≈ 250 × 20 Hz = 5k datagrams/s per box, ≈ 1 % of a core),
  and a 10-cell zone's small remote chunks still coalesce ≈ 8 to a datagram per (cell, box) pair. NS-4.4(c)
  measures the scattered case.
- **Fallback.** If NS-0.7 or NS-2.6 fails after profiling, the order is: (1) patch the vendored
  netcode/reliable (zero-copy receive, per-instance packet pools); (2) put **Valve GameNetworkingSockets**
  (BSD-3; multi-threaded service thread, lanes, reliable messages) behind `Endpoint`, first for trunks only,
  since the public path keeps netcode connect tokens; (3) the same for gateways. The decision gates are the end
  of Phase 0 for trunks and the end of Phase 2 for gateways. GNS brings its own crypto dependency (bcrypt on
  Windows, libsodium on Linux), which is a 09 §3.2 licence and security sign-off.
- **Headroom for a box loss (Phase 4).** A gateway box is one failure domain of up to 8k sessions (NS-4.4).
  The orchestrator keeps the free slots on a shard's *other* boxes at **≥ 1.2 × the sessions on its most loaded
  box**, and at least half of those free slots in a different availability zone from that box. It adds a warm
  spare box when the rule fails for 60 s. At 50k CCU this rule alone needs 8 boxes over 2 zones: 64k slots,
  14k free against a requirement of 1.2 × 6.25k = 7.5k. The zone rule below raises the deployment to 12 boxes
  over 3 zones, and the 8 boxes are what survive a zone loss. The Session service spreads new and reconnect
  tokens by zone-instance affinity within these rules, then by free slots, and every token lists instances on
  ≥ 2 boxes. When a box dies, its sessions redeem reconnect tickets under §2.4's jitter and burst target and land on the survivors with no "full" denial. NS-4.7 kills a full
  box at 50k CCU; STB-5 covers the Phase 3 case at 5k CCU.
- **Load under view composition (Phase 3–4).** A session homed in a 10-cell zone has 4–10 contributing
  cells (§6.6). Per box of 8k such sessions the gateway also handles ≈ 960k chunks/s in (6 per session per
  tick, ≈ 5 per coalesced trunk datagram, so ≈ 190k datagrams/s ≈ 0.4 core of AEAD and bookkeeping),
  `RemoteViewer` fan-out of ≈ 11 MB/s out (48 B × 4 Hz × ≈ 7 contributors per session), and water-filling
  over ≤ 10 pairs per session-tick (≈ 1 µs, ≈ 0.2 core). Client egress is unchanged, because the budget is
  per session. NS-4.4(b) measures the 8k-session step with every session homed in a 10-cell zone, and
  NS-4.4(c) with 8k sessions scattered over every populated cell of the shard.
- **Headroom for an availability-zone loss.** A zone holds a third of the shard's sessions, far more than one
  box, so 05 §6.7 adds a **zone rule**: the gateways outside any one zone must hold every session and still meet
  the box rule. Boxes are spread evenly over 3 zones: 6 at Phase 3 and 12 at Phase 4 (after a zone loss the
  Phase 4 shard is exactly the 8-box configuration above). The login queue caps admission so the rule holds,
  and reconnects keep priority over new logins. 05 A19 drills it at 50k CCU.

### 2.7 Voice chat (ADR-015, Phase 3)

**Decision.** Opus over HTP, with no second transport. Clients send **Opus 20 kbit/s VBR** (20 ms frames, two
per datagram, in-band FEC tuned for 5 % loss, DTX) on the VOICE channel to their gateway. The gateway relays
to the **`helios-voice` forwarder** that owns the channel, and the forwarder fans out one copy per listener
gateway, which delivers to each listener. The forwarder does no decoding, mixing or transcoding. The rejected
alternative is a self-hosted WebRTC SFU such as LiveKit (Apache-2.0): the native client would need libwebrtc,
which is large and hard to build `/MT`, plus a second crypto/ICE/TURN stack beside UDP 7777, and proximity
membership would still have to come from cells. It stays an option for web companion apps (Phase 5).

| Channel | Members (source) | Default | Notes |
|---|---|---|---|
| `party:<id>` | party service (05 §1.12) | on, push-to-talk | 2D, radio filter optional |
| `fleet:<id>` | party service; fleet → wings (≤ 5×50) → squads (≤ 10) | on, push-to-talk | members transmit to their squad; squad, wing and fleet commanders transmit to their level (EVE model) |
| `org:<id>` | social service ops channels (05 §1.11), role-gated | opt-in | ≤ 1 per org active per member |
| `ship:<root AG>` | the owning cell: sessions whose AG root is the ship (§6.1) | on, voice-activated | intercom, auto-joined on boarding |
| `prox:<instance>` | the owning cell: hearers within 40 m on the same grid or through open portals (02 §7.3) | **opt-in per player**; designers can disable per zone | spatialized with the speaker's `NetHandle` |

**Routing.**
1. **Ownership.** Channels map to forwarders by rendezvous hashing over live forwarders (KV `DIRECTORY`). A
   forwarder crash rehashes its channels. Membership is rebuilt from services and cells in ≤ 1 s, so listeners
   hear a gap of ≤ 1 s.
2. **Membership.** Party, fleet and org rosters arrive as NATS events and are cached by forwarders and
   gateways, which check talk rights. For `ship` and `prox` channels the owning cell streams
   `VoiceAudience{speaker, +hearers, −hearers, speaker_handle}` deltas to the channel's forwarder on a cell↔voice
   trunk. The deltas are computed on the 4 Hz interest refresh (§4.3), so hearers are always a subset of the
   replicated set (40 m < 150 m), cost ≤ 20 KB/s per cell, and multi-cell zones send them per owning cell.
3. **Speaker selection.** Frames carry a 7-bit audio level. Each listener gets **≤ 3 streams**, in the order
   channel priority speaker, fleet commander, wing commander, then the loudest (300 ms hangover).
4. **Blocks are server-side.** A blocked speaker is never forwarded to the blocker, and a sanctioned session's
   VOICE is dropped at its gateway. Both take effect in ≤ 1 s.
5. **Latency budget (mouth to ear).** 10 ms capture + 40 ms packetization + 2 ms encode + uplink + ≤ 5 ms
   gateway→forwarder→gateway + downlink + 40–100 ms adaptive jitter buffer (Opus PLC/FEC) + 20 ms playout. At
   80 ms RTT per leg and a typical 60 ms jitter buffer this is ≈ 220 ms, against a **p95 ≤ 250 ms** target
   (NS-3.9).

**Wire.** A voice message is `{channel alias u16 (session-scoped, assigned on CONTROL), seq u16, level u7 +
flags, Opus TOC + frames}`, ≈ 168 B per datagram with all headers, or ≈ 34 kbit/s per stream. Gateways
validate the TOC (20 ms frames, SILK or hybrid mode, bandwidth ≤ super-wideband), the 40 kbit/s bucket and
talk rights. Three malformed frames count as one strike toward §9's malformed-message disconnect.

**Moderation and abuse policy.**
- **Defaults.** Party, fleet and ship voice are on. Proximity voice is opt-in, and a "mute strangers" toggle is
  on by default for new accounts. Voice is disabled for accounts under 24 h played, for accounts under
  parental controls (an entitlement claim, 05 §1.20), and for chat-sanctioned accounts.
- **Player tools.** Per-player mute (client-side), block (social service; enforced by the forwarder), and
  per-channel volume. Fleet and org commanders can mute members in their channel. A continuous-transmit cap of
  60 s applies in `prox`, which catches hot mics.
- **Evidence.** HTP encrypts every hop, but voice is not end-to-end encrypted, and the privacy notice says so.
  Each forwarder keeps an **in-memory 60 s ring** of every active speaker's forwarded frames. A voice report
  snapshots the reported speaker's ring to the Trust service (05 §1.16): encrypted at rest, access-logged,
  deleted after 30 days unless actioned. There is no continuous recording and no transcription.
- **GM actions.** Voice mute and ban are Trust sanctions, pushed on `ctl.<shard>.gateway.all` and applied in
  ≤ 1 s.
- **Later.** Automated toxicity detection is a Phase 4 decision behind an `IVoiceModeration` seam. It needs
  legal review first (GDPR, COPPA, two-party-consent jurisdictions), as does the first-use consent dialog.

**Client side** (02 §7.3 audio, 08 §1.4 settings): libopus (approved, 09 §3.2 #6), the Helios jitter buffer
and a push-to-talk default. Echo cancellation and noise suppression candidates are SpeexDSP and RNNoise
(both BSD-3); they need 09 §3.2 approval, and push-to-talk plus headsets are the fallback.

## 3. Cell server architecture

### 3.1 Process model

`helios-cell` is the headless engine build (`HELIOS_BUILD_GRAPHICS=OFF`, Null RHI; R06-ENG-02). A `ZoneHost`
runs N **ZoneInstances**, each with its own flecs world (ADR-004), one Jolt `PhysicsSystem` per grid (ADR-005), **one Luau
VM**, ZoneClock (1–60 Hz), replication context and AG table; instances share nothing mutable and talk via
mailboxes. Small instances are packed EVE-style (≤ 64 instances or 200 players per process); hot zones get
dedicated processes. Zones **live-migrate** between processes by **planned region migration** (§6.7; R01-P0-1,
Phase 3): the target pre-copies the region from the source's replication stream, and one freeze of ≤ 1 s
(p99) moves the rest, including transients, spawners, the region lease, handle blocks and the zone-leader
role. Rolling restarts, `Drain` and `PreProvision` all use it.

Luau runs only in the gameplay stage under a deterministic **fuel** budget: interrupt safepoints are counted,
the budget is sized to ≈ 15 % of the tick, and once it is spent the lane stops resuming coroutines (the rest
resume next tick). A running resume is never preempted, because there are no involuntary yields: it is killed at
`fuel_kill` (02 §7.4). Wall time is only a logged 20 ms backstop (§10.2). Heap caps are 16 MB (small) and 256 MB (large). Service
calls yield the coroutine, and results arrive as messages through the sim inbox (§10.2). **Rule:** per-entity
script state lives in schema components (`ScriptState`), never VM locals, so it survives handoff,
checkpoints and the script rebase that cuts a replay keyframe every 30 min (§10.2).

### 3.2 Zone clock and time dilation (TiDi)

Game step is fixed (`tick_dt = 1/tick_hz`); dilation `d ∈ [floor, 1]` (floor 0.1, R01-P0-3) stretches only wall
time: `wall_interval = tick_dt / d`, so results are identical under TiDi. Gameplay timers are in ticks;
wall-clock services (industry, market) are not dilated. Controller, `L = tick_cpu_ms / (1000/tick_hz)`: **fast
attack** `L > 0.9 → d ← max(floor, d·0.85/L)`; **slow release** +0.05/s after 2 s of `L < 0.7`. `d` rides in
snapshot headers and CONTROL; clients scale prediction, interpolation and animation and show an indicator. In
multi-cell zones the **zone leader** sets `d = min(demands)` for every cell from a common future tick, and cells
tick in phase with skew ≤ 1 ms p99 (§3.5).

### 3.3 Per-tick job graph

Job system per ADR-011 (flecs systems and Jolt run on it); 1–2 IO threads fill per-instance mailboxes.

| # | Stage | Parallelism | Budget @20 Hz |
|---|---|---|---|
| 1 | **Input**: `SimInbox` drain (inputs, service replies, trunk messages, count-capped and recorded, §10.2); one input per controlled entity (§5.5) | per AG | 2 ms |
| 2 | **Pre-physics**: ability SMs, flight controllers, movers, AI results | ECS scheduler | 6 ms |
| 3 | **Physics**: `TerrainFence` builds any missing contact tile (02 §5.8a: ≤ 8 core-ms of builds, which is ≤ 8 full or ≤ 16 shape-only tiles; ≤ 1.2 ms p99 wall at that cap, ≤ 0.2 ms when nothing is built), then a Jolt step per grid; space and bubbles holding a ground vehicle substep at 60 Hz (06 §8.1a) | per grid + Jolt jobs | 10 ms |
| 4 | **Post-physics**: contacts, damage, effects inbox, Luau events (fuel-sliced, §10.2) | ECS; Luau single-thread | 8 ms |
| 5 | **Authority flush**: effects, ghost updates, handoff offers | per neighbour | 1 ms |
| 6 | **Replication gather**: shadow + serialize once (§4.2) | per ECS table | 3 ms |
| 7 | **Connection write**: interest, priority, chunks for local sessions and, in v1, remote viewers (§6.6) | per (session, cell) | 3 ms |
| 8 | **Send + record**: trunk send, hitbox history, metrics | IO + jobs | 2 ms |

Total **35 ms = p99 target (70 % of 50 ms)**, leaving headroom before overload (§8); 11.7 ms at 60 Hz.

**Luau allowance (02 §7.4).** Stage 4's Luau share is `fuel_per_tick`. Every binding and length-dependent
builtin call is charged calibrated fuel, so fuel tracks CPU time. A tick can therefore overrun the share by at
most one resume, which is ≤ `fuel_kill` = min(≈ 5 ms, 25 % of the tick).
- At 20 Hz that is ≤ 5 ms, inside the 15 ms between the 35 ms p99 target and the 50 ms tick.
- At 60 Hz it is ≤ 4.2 ms, and 11.7 + 4.2 < 16.7 ms.

The 20 ms wall backstop is a fault path, not a budget.

**Stage 7 cost model** (reference SERVER core; one row per kind of `(session, cell)` pair; NS-2.1 and NS-3.10
measure it):

| Pair kind | Relevant entities / writes per tick | CPU per tick | Interest refresh (4 Hz, staggered) |
|---|---|---|---|
| Local session (home cell) | ≈ 2,000 / ≈ 60 | ≈ 12 µs | ≈ 20 µs per refresh |
| Remote viewer, L3 only (capitals, stations, zone globals) | ≤ 20 / ≤ 2 | ≈ 1 µs | ≈ 3 µs |
| Remote viewer, L2 (small craft within 25 km) | ≤ 300 / ≈ 15 | ≈ 4 µs | ≈ 3 µs |
| Remote viewer, L0–L1 (characters, props near a boundary) | ≤ 100 / ≈ 10 | ≈ 3 µs | ≈ 3 µs |

| Per cell with 500 local players | v0 or 1 cell | 4-cell Harrow orbit, 2,000 players (Ph3) | 10-cell zone, 5,000 players (Ph4, SRV-2) |
|---|---|---|---|
| Remote registrations (worst case: every cell holds a capital or station): L3 only / L2 / L0–L1 | 0 | 1,500 / 300 / 50 | 4,500 / 600 / 100 |
| CPU: local writes + local interest | 6.0 + 2.0 ms | 6.0 + 2.0 ms | 6.0 + 2.0 ms |
| CPU: remote writes + remote interest | 0 | 2.9 + 1.1 ms | 7.2 + 3.1 ms |
| **Stage 7 wall on 8 cores** (budget 3 ms) | **≈ 1.0 ms** | **≈ 1.5 ms** | **≈ 2.3 ms** |

The Ph4 column sits at 96 % of the 2.4 ms (80 %) trigger for the **far-broadcast fallback** (§6.6), so WP-4.3
builds that path and NS-3.10's Phase 4 rerun decides whether it is on by default.

### 3.4 Budgets per zone profile

| Profile | Tick | Players | Replicated entities | Cores | RAM | Egress |
|---|---|---|---|---|---|---|
| Ground hub / station | 20 Hz | 500 | 20k | 8 | 6 GB | 128 Mbit/s |
| Open space (system) | 20 Hz (+60 Hz substeps) | 500 | 20k | 8 | 6 GB | 128 Mbit/s |
| Fleet battle (Ph3 500 ships, Ph4 2,000) | 2 Hz × `d`; every ship `CommandKinematic` (§5.4) | 2,000 | 50k | 16–32 | 16 GB | ≤ 1 Gbit/s |
| Activity (raid/FPS/PvP) | 30–60 Hz | 6–64 | 2k | 1–2 | 512 MB | 32 Mbit/s |
| Phase / housing | 10 Hz | 1–16 | 1k | 0.25 | 128 MB | 4 Mbit/s |

500-player memory: static data ≤ 3 GB, ghost records 32 MB, hitbox history ≤ 16 MB. Per-profile entity caps
are enforced with telemetry (R04-P1-16). **Fleet-battle zones** are the one battle profile (01 §3.4). A zone
record selects the profile. Ships there run no 6-DoF prediction, and replication never exceeds the 2 Hz tick, so
the §4.3 near rates cap at 2 Hz. At `d` = 0.1 a tick lasts 5 s of wall time, so module response is gated in
ticks of game time, not wall seconds (AAA-SRV-10, NS-4.2).

### 3.5 Multi-cell zone coordination: the zone leader (v1, Phase 3)

One cell of each zone instance is the **zone leader**. It owns only small, reconstructible state: the zone
schedule, the TiDi demand table and the handle-block table. It simulates nothing extra, and its duties cost
≤ 0.2 ms per tick. In v0 the zone's only cell is its own leader, running the same code path.

**Assignment, not election.** The orchestrator calls `AssignZoneLeader(instance, cell, leader_gen)`. As with
region leases (05 §1.4), `leader_gen` is allocated in PG (`zone_leader(instance, cell, gen)`) before anyone is
told. The leader role rides on the cell's own lease and heartbeat. The orchestrator also names a **standby
leader**: the next live cell by ID, which mirrors leader state from a 1 Hz `LeaderState` snapshot plus every
change, sent on the trunk. Every leader message carries `leader_gen`. Cells ignore lower generations and
answer `STALE_LEADER`.

**Failover.** When the leader cell's failure is confirmed (two signals, ≈ 3 s; 05 §1.4.3), the orchestrator
bumps `leader_gen`, promotes the standby and names a new standby. It never does so while the control plane is
degraded (05 §1.4.4). The dead cell's region recovers like any other (§6.4). During the gap
(≤ 3.5 s):
- cells keep ticking on the last schedule, and `d` is frozen, since no cell may change it unilaterally;
- spawns never stall, because every cell holds at least one unused **reserve handle block**;
- handoffs continue, because they never involve the leader.

The new leader rebuilds demands from the next `TiDiDemand` round and handle blocks from PG.

**Handle blocks.** The 24-bit `NetHandle` index space (§4.6) is split:
- indices `[0, 2^22)` belong to content-placed entities through container index tables;
- the rest form **3,072 dynamic blocks of 4,096**.

Blocks are leased through `LeaseHandleBlocks`, which writes `zone_handle_block(instance, block, holder_cell,
leader_gen, state ∈ {active, frozen, free})` to PG synchronously (~2 ms) **before** the cell may allocate from
them. A cell requests a new block when 25 % of its current one remains, so allocation never waits.

Handles survive handoff. When a cell destroys an entity whose handle comes from another cell's block, it sends
`HandleRelease{handle}` to the block holder (ordered, idempotent). The holder bumps the 8-bit generation and
quarantines the index for 2 s. When a cell dies, its blocks become **frozen**: the standby process that takes
the region allocates only from fresh blocks. A frozen block returns to `free` only after a zone-wide
`HandleCensus` (every cell reports its live handles in that block, ≤ 1 s) finds it unused. No handle can
therefore be reused across a failover.

**Planned moves.** When a cell's region migrates (§6.7), its blocks move to the target **still active**: the
residual carries their free lists and quarantine, and the commit transaction rewrites `holder_cell`. Only a
crash freezes blocks. A leader cell that migrates hands over its `LeaderState` in the same residual, and the
commit transaction moves the role to the target at `leader_gen + 1`, so the zone is never leaderless.

**Tick alignment.** All cells run one schedule, `ZoneSchedule{anchor_tick, anchor_wall_ns, tick_dt, d}`:
tick *n* starts at `anchor_wall_ns + (n − anchor_tick) · tick_dt / d`.
- **Changing `d`.** The leader issues a new schedule with `anchor_tick = now + 2`. Fast attack costs 2 ticks,
  and every cell applies the same `d` from the same tick, so gameplay time is identical everywhere.
- **Clocks.** Production hosts run **chrony** with NIC hardware timestamping against the data centre's
  PTP/NTP stratum-1 sources. An offset above 250 µs alarms, and above 1 ms the host is drained from leader and
  multi-cell placement. Each cell also compares a 1 Hz leader `TickMark{tick, wall_ns}` with its own schedule
  and slews by ≤ 0.5 ms/s, never stepping.
- **Timers.** High-resolution waitable timers or `timerfd`, then a busy-wait for the last 200 µs, keep timer
  jitter ≤ 0.25 ms.
- **Bound.** Skew **≤ 1 ms p99** (clock ≤ 0.25 ms + timer ≤ 0.25 ms + margin).
- **Late cells.** A late cell starts its next tick late but never skips a tick number. A cell ≥ 1 tick behind
  the schedule for 1 s reports `L > 1`, and its `TiDiDemand` drives `d` down zone-wide. The one exception is a
  region that rejoins after a crash recovery (§6.4 step 5) or more than 2 s after an expired migration hold
  (§6.7): it starts at the schedule's current tick, and its state stands still for the gap.
- **Holds.** The only production hold is `MigrationHold` (§6.7), bounded at 1 s. The debug hold (§10.3) exists
  only in `--dev` processes.
- **Windows dev.** Multi-cell zones run on one host, so alignment reduces to timer jitter.

`TiDiDemand{tick, d_wanted, L}` goes to the leader every tick. The leader applies §3.2's attack rule to the
minimum, and releases `d` only when every cell's release condition holds. NS-3.7 kills the leader under load.

## 4. Replication design

Pipeline (R06 §6.12, Iris): descriptors → push dirty bits → quantized shadow → filter → prioritize →
serialize once → delta.

### 4.1 Schema-generated descriptors

`helios-schemac` (ADR-004) emits `ComponentRepDesc` tables: offsets, bit widths, quantizers, **audience**
(`all | owner | server`, R02-P0-4), **LOD group** (`core | near`), change-mask indices (syntax owned by 02):

```
component ShipMotion replicate(all) lod(core) {
  pos    : WorldPos @quant(frame_cell, cell=4096m, res=1/256m)   // §4.5
  rot    : quatf    @quant(smallest3, bits=10)                  // 32 bits
  vel    : vec3f    @quant(range=±4096, bits=16)
  angvel : vec3f    @quant(range=±8, bits=10)        lod(near)
}
component CargoHold    replicate(owner)  { ... }
component AiBlackboard replicate(server) { ... }                  // ghosts + checkpoints only
rpc ActivateModule(slot: u8, target: NetHandle) client->server reliable rate(10/s)
```

Record templates carry a **replication profile**: relevance class, base priority, rate limits, command
replication eligibility (§5.4).

### 4.2 Shadow state, dirty tracking, serialize-once, deltas

1. **Gather:** generated mutators (`Mut<C>`) push per-field dirty bits into our `RepState` component (flecs
   table-level change detection is too coarse, ADR-004). Dirty fields are quantized into the SoA shadow and
   compared with the previous quantized value → **change mask** (sub-quantum noise costs nothing); append
   `(change_seq, mask)` to the entity's 64-entry change ring.
2. **Serialize once:** encode the *current* bits of each dirty field into a per-tick field-chunk cache (per
   audience), plus a lazily built full-state chunk for creates.
3. **Per connection:** a 32-byte ghost record holds `net_handle`, `last_sent_change_seq`, `lost_mask`, acked
   motion baseline, priority accumulator. Send mask = `lost_mask | OR(ring masks newer than last_sent)` (ring
   overflow → full). Writing copies pre-encoded bits (handle Δ, mask, fields): **no per-connection
   re-encoding** except motion deltas.
4. **Delta vs acked baseline** (Quake 3/Fiedler): `pos` offset and `rot` encode against the connection's last
   *acked* motion baseline in 3 size classes (8, 12 bits/axis, absolute), cached per `(entity, baseline_tick)`.
5. **Loss repair:** each packet keeps `[(handle, mask)]`; a loss notification ORs the mask into `lost_mask`,
   so the next send carries *current* values, never stale retransmits.

### 4.3 Interest management

**Spatial:** each reference-frame grid (surface, interior, station, open space) owns a sparse **hierarchical
spatial hash**: L0 32 m (props, loot), L1 256 m (characters), L2 8 km (small craft), L3 256 km (capitals,
stations). Entities insert at the level of their relevance radius; frames are entities in their parent's hash;
viewers are transformed into each candidate frame. Interiors are relevant within 300 m of the host or when
flagged visible (open hangar). Candidate lists are per hash cell, shared by its viewers (RepGraph); connection
sets refresh at **4 Hz, staggered**; leave radius = 1.15 × enter, min dwell 1 s. **Always relevant:** self and
own AG, owned drones, party/fleet (core LOD), targets and attackers, zone globals. **Scope filter:**
`(instance, layer, phase_mask:u64, owner/party)` (§7); cloaked or undetected entities are not relevant (§9).
**Caps:** 2,048 relevant entities (ground) / 4,096 (space); overflow drops lowest priorities (fleet proxies in
v2, R07-P2-25). **Multi-cell zones (v1):** each cell runs this interest only over entities whose AG it owns,
for its own sessions and for **remote viewers** homed on other cells, which the gateway registers with a
hash-level mask (§6.6). A session's relevant set is the union of the per-cell sets, and the caps apply to the
union: the gateway splits them across contributing cells in proportion to their byte grants.

| Relevance class | Enter radius | Near / far rate | Base priority |
|---|---|---|---|
| Character / NPC | 150 m (400 m open terrain) | 20 / 5 Hz | 1.0 |
| Dynamic loot / prop | 50 m | 5 / 1 Hz | 0.3 |
| Small craft | 25 km | 20 / 2 Hz | 1.0 |
| Capital / station | 300 km / system | 5 / 0.5 Hz | 0.8 |
| Projectile entity | 3 km | 20 / 5 Hz | 0.6 |
| Content-placed static | client has it from containers | overrides only | — |

### 4.4 Priority accumulator × bandwidth budget

Per tick for each relevant entity: `p = base × falloff(d/r) × screen_size × relation × (1 + change_bonus)`
(relation ×3 target/attacker, ×1.5 party); `acc += p·dt`. Mandatory items (CONTROL, owner state, creates,
destroys) go first, then entities by descending `acc` (partial sort of top K) until the STATE budget is spent;
entity writes are atomic (bit position rolls back if it doesn't fit). Only sent entities reset `acc`. **Max
staleness** (1 s near, 5 s far) forces a send. Far entities send only `core` fields; promotion to near forces
the `near` mask.

### 4.5 Frame-relative position quantization

Transform = `(frame NetHandle, cell index, offset)`: cell index as zigzag varint per axis on 4 km cells (rarely
changes, own dirty bit); offset at 1/256 m over 4,096 m = 20 bits/axis (interiors 1/1024 m over ±512 m).
Rotation: ships smallest-three 3×10 bits; characters yaw16 + pitch14. At-rest bit suppresses velocity. Both
sides quantize (R07 §3.4) so extrapolation matches. Interior contents are frame-local, so a moving ship dirties
only itself. The ADR-005 10^13 m test covers replicated state too.

### 4.6 Entity lifecycle, RPCs and events

**NetHandle** = u32 wire alias (24-bit index + 8-bit generation) of the 64-bit EntityId (ADR-004),
**zone-instance-scoped** and identical for all connections, so payloads stay connection-independent.
Content-placed entities take handles from their container's index table and are never "created" on the wire
(SWG snapshot lesson); dynamic handles come from PG-recorded blocks leased through the zone leader (§3.5); handles survive handoff.
**Create** `{handle, EntityId, record hash, frame, owner bit, full state}` and **Destroy** `{handle, reason}`
are reliable state (kept in `lost_mask` until acked). **Reparent** (board, dock, grid transfer) changes the frame field
atomically with the new local transform; clients `Reparent()` preserving world position (ADR-005). **RPCs** are
schema-generated with direction, reliability and rate limit; handle arguments must be relevant to the sender.
**Events** are entity-scoped with audience `owner | relevant | party`.

| Reliability class | Channel | Example |
|---|---|---|
| Replicated state (eventually consistent) | STATE | transforms, vitals, equipment visuals |
| Reliable ordered | EVENT_R / CONTROL | inventory result, dialogue choice, zone change |
| Unreliable fire-and-forget | EVENT_U | muzzle flash, impact FX |
| Unreliable latest-only | LATEST | others' turret aim |
| Server↔server ordered | trunk | `ApplyDamage`, `RequestDock` (§6.2) |

### 4.7 Bandwidth math (R07 §8.3 targets)

Per-packet overhead: ~48 B (IPv4+UDP+netcode) + ~7 B reliable + ~6 B snapshot header ≈ 61 B → **~10 kbit/s** at 20 pps.
Moving character ≈ 118 bits (handle Δ 8, mask 8, pos Δ 3×12, yaw/pitch 30, vel 3×8, anim/flags 12) ≈ **15 B**;
moving ship ≈ 170 bits (pos Δ 36, rot 32, vel 48, near angvel 30, header 16) ≈ **21 B**; idle entities cost 0.

| Scenario (cap) | Composition, KB/s | Result |
|---|---|---|
| Ground hub (256 kbit/s) | 30 chars @20 Hz 9.0 + 70 @5 Hz 5.3 + 200 @1 Hz 3.0 + events 3.0 + overhead 1.3 | **21.6 KB/s ≈ 173 kbit/s** ✓ |
| Open-space battle, 20 Hz zone (512 kbit/s) | 50 ships @20 Hz 21.0 + 150 @5 Hz 15.8 + 300 @1 Hz 6.3 + launches 1.6 + vitals 2.0 + overhead 2.6 | **49.3 KB/s ≈ 394 kbit/s** ✓ |
| Same, far ships command-replicated (§5.4) | 300 far ships ~2 B/s each | **≈ 345 kbit/s**; ~1,000 ships fit 1 Mbit/s |
| Fleet battle, 2 Hz zone, 2,000 ships at `d` = 1 (512 kbit/s) | commands 2,000 × 20 B / 10 s 4.0 + 100 near ships @2 Hz 4.2 + vitals 300 @2 Hz × 6 B 3.6 + module/volley changes 2,000 × 0.2/s × 8 B 3.2 + launches 1.6 + overhead and resends 1.8 | **18.4 KB/s ≈ 147 kbit/s** ✓; scales with `d` (≈ 20 kbit/s at 0.1) |
| Upstream, 20 Hz | 20 pps × (55 B + 4 inputs, Δ-coded) | **≈ 16 kbit/s** ✓ (≤ 64) |
| Upstream, 60 Hz instance | 30 pps × (2 new + 2 redundant) | **≈ 24 kbit/s** ✓ |

Server side: a 500-player cell emits ≈ **128 Mbit/s**; 5k CCU ≈ 1.3 Gbit/s, 100k CCU ≈ 25.6 Gbit/s (R07 cost
flag → 05). CPU at 500 connections: gather ≈ 2 ms/tick wall; connection writes ≈ 8 ms of CPU, ≈ 1.0 ms wall
over 8 cores (§3.3 cost model).

**Multi-cell zones (v1, §6.6).** Remote viewers change who sends the bytes, not how many reach a client: the
gateway enforces each session's §2.5 budget over all contributing cells, so every row above holds per client.
What they add is trunk and CPU traffic per cell:

| Per cell, 500 local players | 4-cell Harrow orbit, 2,000 players (Ph3) | 10-cell zone, 5,000 players (Ph4) |
|---|---|---|
| `RemoteViewer` in (≈ 48 B × 4 Hz per registration) | ≈ 2.8 Mbit/s | ≈ 8.0 Mbit/s |
| `Demand` out + `Grant` in (9 B × 20 Hz per pair wanting more than the 48 B implicit floor) | ≈ 1.2 Mbit/s | ≈ 1.7 Mbit/s |
| Chunk headers for remote pairs (≈ 4 B per non-empty chunk; L3 pairs ≈ 8 chunks/s, others 20/s) | ≈ 0.6 Mbit/s | ≈ 1.6 Mbit/s |
| Client-bound egress | ≈ 128 Mbit/s, redistributed between cells | same |
| Stage 7 wall (§3.3) | ≈ 1.5 ms | ≈ 2.3 ms |
| Extra per-(session, cell) ghost records (32 B each, worst case) | ≤ 4 MB | ≤ 9 MB |

A client near a boundary splits its budget between two cells; one in Harrow orbit with a capital in each cell
receives four small streams at the far-tier rate. Neither raises its downstream, which NS-3.10 checks against
AAA-SRV-6.

## 5. Movement and combat netcode

### 5.1 Time model

Command frames = zone ticks. The client runs **ahead** of the server by `RTT/2 + input buffer` (target 1.5
ticks); snapshots report server-side input-buffer depth and the client nudges its tick rate ±3 % (Overwatch,
R05-P0-3). CONTROL time sync every 1 s (NTP-style, best 3 of 8). Under TiDi the command clock follows `d`.
**Hard resync.** When the input-buffer error exceeds 4 ticks, as after a migration freeze (§6.7) or a
recovery hitch (§6.4), the server sends `ClockReset{server_tick}` on CONTROL. It keeps the newest buffered
inputs up to the 1.5-tick target and discards the rest; the client re-bases its command clock and replays
prediction from its last acked state. Input sequences continue, and §5.5's cheat counter skips the next 2 s.

### 5.2 Characters

**Input:** tick, move axes 3×8 bits, look yaw16/pitch14, buttons, `device_class` (2 bits: mouse, pad, HOTAS;
selects the server-side `AimAssistDef`, §5.6, 08 §1.5), optional ability intent `{slot, prediction
key, target}`, `view_tick + fraction` for lag compensation; last 4 unacked inputs, delta-coded. Client and
server run the **same C++ character mover** (Jolt `CharacterVirtual`) in frame-local coordinates. The client
keeps 128 ticks of `(input, predicted state)`; owner snapshots carry `last_processed_input_tick` plus
authoritative position, velocity, movement mode and ability-SM state. Compared *quantized*: > 1 cm or any
discrete difference → rewind and replay; visual error decays over 150 ms. Mispredictions < 1 % of ticks at
100 ms RTT / 1 % loss.

### 5.3 Ships, multi-crew, remote entities

The pilot predicts its ship by stepping only the owned body with the flight controller in a client Jolt system
against static colliders, in the owning cell bubble's coordinates (the replicated `BubbleOrigin`, 02 §5.4); with `JPH_CROSS_PLATFORM_DETERMINISTIC` on both sides (ADR-013),
Jolt's contact order keyed by stable body keys rather than `BodyID`s, which differ between the two systems, and
no contact cache carried across ticks on the hull (02 §7.1), the integration matches the server bit-for-bit, so corrections come only from unpredicted contacts, damage and others' effects, which are smoothed. The flight controller and
thruster allocation that drive the hull are held to the same bit-exact bar on every toolchain (06 §8.2, GP-4a), and the `Flight` channel sends six
8-bit axes and an 8-bit throttle in the §5.2 input record. EVA characters are predicted the same way, in the owning grid's coordinates (06 §8.2a). Only the seat
holding the flight input channel predicts the hull; gunners predict turret aim; passengers walk in the ship's
grid frame, unaffected by hull corrections (R04-P0-3). Owner jitter buffer: 4 ticks at 60 Hz.
**Ground vehicles and mounts (Phase 2, 06 §8.1a).** The seat holding the `Drive` channel predicts the vehicle.
`Drive` (steer, throttle, brake, trim and buttons; 6 bytes) takes `Flight`'s place in the §5.2 input record.
Wheeled and hover vehicles go through `SingleBodyPredictor`, which steps the one chassis body with its
`VehicleConstraint` or hover listener at the bubble's collision-step count, with the constraint's `SaveState` in
the rollback snapshot. Mounts go through the shared character mover. Owner snapshots carry a 32-bit hash of the
predicted state, and the full state follows only on a mismatch (06 GP-4d).
**Remote entities:** interpolation delay `clamp(2 × send_interval + jitter_p95, 50, 250 ms)` (≈ 100 ms at
20 Hz); Hermite position with velocities, slerp rotation, frame-local; extrapolation ≤ 250 ms (ships) /
100 ms (characters), then freeze with a lag indicator.

### 5.4 Command-based replication for large-scale space (EVE model)

For ships under navigation commands (approach, orbit, keep-at-range, align, warp, quantum travel, autopilot)
the server replicates the **command** `{type, target, params, start tick, start state}`; both sides run the same
integrator in **64-bit fixed point** (2^-10 m; bit-exact across compilers; exact in f64 to 8.8×10^12 m)
(R01-P1-10). A state hash every 2 s detects drift and falls back to state replication. Used for far-tier ships,
NPC convoys and quantum travel with container-prefetch hints and mid-route authority transfers (R04-P1-15).
Cost: ~20 B per command change.

### 5.5 Server-authoritative movement and speedhack detection

Clients never send positions, so speedhacks reduce to **input-rate abuse**. **Movement time budget:** accrue
`wall_dt × d` per tick; each input costs `tick_dt`; burst credit ≤ 250 ms; excess inputs are dropped and
counted (the decision uses wall time, so the replay recorder logs the inputs actually consumed, §10.2); > 5 % over 30 s emits cheat telemetry and trips an auto-kick threshold. Missing input repeats for 2
ticks, then "no input" (characters stop, ships keep momentum). Defence in depth: mode speed/acceleration caps,
teleport check (Δpos > v_max·dt + ε), grid transfers only through valid grid volumes, range and LOS for
interactions — **flagged, not silently snapped** (R07 §3.5). R05's "PvE client-reported hits" is a
per-activity flag, off by default, never in PvP.

### 5.6 Lag-compensated hit registration and projectiles

**History:** a 1 s ring of `(tick, frame transform, capsule set)` per hitboxed entity, frame-local
(R05-P0-4; ≈ 6 MB for 200 entities at 60 Hz). **Resolve:** rewind `now − view_tick`, **clamped** (default 200 ms, 100–250 ms per activity; high ping leads shots); ray or
sweep against rewound hitboxes in the shooter's frame; occlusion against current static and rewound dynamic
geometry; validate aim vs the server's shooter orientation, fire rate, ammo, weapon SM, and the per-weapon
**aim-assist cone evaluated server-side** (R05-P0-9). Long-range space turrets and targeted abilities use hit
formulas at the current tick with range/LOS tolerance — no rewind (R07 §3.6).
**Projectiles:** fast (< 1 s flight) — shooter spawns a predicted visual keyed by prediction key; server
spawns at the rewound time and fast-forwards against rewound hitboxes; others get one spawn event
`{origin, dir, speed, tick}` and simulate visuals locally; shooter gets confirm/cancel. Slow/guided (missiles)
— server entities on command replication, battle volleys aggregated into one entity with a count (EVE missile
lesson, R01 §4).

**Across a cell boundary (v1, Phase 3).** One rule applies: **the shooter's cell resolves every shot**, against
its own entities and its ghosts alike, and damage to another cell's entity lands as an idempotent
`ApplyDamage` effect. Resolution never forwards a shot and never waits for another cell.
- **Ghost hit history.** A cell ghosts every hitboxed entity (anything with a `HitboxSet`: characters, small
  craft, turrets) that lies within **`M_hit`** of its boundary at the **zone tick rate**, instead of §6.2's
  20 / 5 Hz ghost rates. The ghost update carries a server-audience **`HitPose`**: the capsule set quantized
  relative to the root (≤ 16 capsules × 7 B, ≈ 50 B per tick after change masks; rigid ships send only
  turret yaw and pitch) plus hit-relevant flags (dodge i-frames, cloak). The owner records the same quantized
  values in its own history, so a rewind reads identical numbers on either side of the boundary. The
  receiving cell keeps the same 1 s ring for those ghosts (≈ 0.5 MB for 200 ghosts at 20 Hz), and the trunk
  cost is ≈ 1 KB/s per ghost at 20 Hz. A tick lost on the trunk is interpolated from its neighbours in the
  ring. A `view_tick` older than a new ghost's history uses its oldest entry and counts `lagcomp_ghost_gap`.
- **Coverage by construction (cook check).** For each zone class the cook computes
  `M_hit = max(H, range of every RewindHitscan weapon, speed × lifetime of every fast projectile usable in the
  class) + v_max × rewind cap`, and sets the ghost margin for hitboxed classes to `max(M, M_hit)`, like the
  follower leash check (06 §7.3). The build fails if `M_hit` exceeds 1 km on the ground or 10 km in space: such
  a weapon needs a per-class range cap in its `WeaponDef` or the `Statistical` or `Missile` strategy (06 §8.6).
  The server clamps every ray and fast projectile to the class range, so no shot needs state beyond `M_hit`.
- **Decision.** One query over local and ghost bodies picks the nearest hit, with occlusion against static
  content (identical on every cell) and rewound dynamic bodies (local or ghost). A local target is damaged at
  once. A ghost target gets `ApplyDamage{DamagePacket, idempotency id = (shooter AG, prediction key, shot
  index)}` (06 §8.7), and its owner applies it at its next tick after re-checking only liveness and immunity.
  The shooter's hit marker comes from the resolution in both cases.
- **Target handoff mid-fight.** The new owner already held the target's tick-rate ghost history
  (`M_hit ≥ H`), and the old owner receives it as a ghost from T+1, so either cell can rewind across the
  handoff tick. Damage sent to the old owner is forwarded in its grace window and de-duplicated exactly
  (§6.2), so resolution needs no pause.
- **Fast projectiles** stay on the shooter's cell for their < 1 s flight and sweep the same rewound bodies.
  Guided missiles are entities and hand off like any transient root.
- **Target region suspect or recovering.** Its ghosts stop updating. Shots resolve against the last recorded
  pose, with no extrapolation, and the `ApplyDamage` waits in the sender's outbox until the region's new owner
  applies it exactly once against its restored state (§6.2).
- *Rejected:* forwarding `ResolveShot{shooter, view_tick, ray}` to the target's owner. A ray can cross several
  regions, so nearest-hit needs a coordinator and a second round trip (+2 ticks of hit latency). The owner's
  history is also no more exact than the ghost's, since both hold the same quantized poses.

NS-3.12 tests firefights and dogfights across a boundary.

### 5.7 Predicted abilities (ADR-002)

Ability graphs compile to the native rollback-safe state-machine format, run identically on client and server. SM state `{state id, state tick, charges, cooldown end tick, ≤ 64 B vars}` is predicted owner state,
reconciled like movement. Each activation carries a client **prediction key**; the server confirms or rejects,
and rejection rolls back the SM plus predicted cues and movement modifiers (dash, blink). SMs may only use
timers, input checks, predicted projectile spawns, cosmetic cues, movement modifiers and server-effect *requests*. Damage, loot and value spend are never predicted.

## 6. Authority groups, ghosts and handoff

### 6.1 AGs, epochs and the fence

An **AG** is the unit of authority, persistence and handoff: a root entity plus its attached hierarchy (a ship
and its interior grid). A player on foot is their own AG and nests under the ship's AG on boarding (R04-P0-4).
Each AG records its owner, as `owner_cell` (the process incarnation) plus `owner_region` and
`owner_lease_gen` (the region lease it is simulated under), and a **64-bit epoch**.

The **Fence** (persistence gateway; rows in the shard primary; 05 §1.13) is the linearization point for
ownership. Every fence operation is a conditional update over NATS request/reply (p99 < 5 ms), issued from an
I/O job, so **the tick never waits on it**. Handoff *state* moves cell→cell on the trunk (ADR-008). The Ledger
and Persistence reject writes with a stale `(ag, epoch, owner)` (R07-P0-7), so a crash can roll back position or
XP but never duplicate value.

**Which entities get a fence row.**

| AG kind | Examples | Fence row | Ledger custody | Checkpoint | Fenced by |
|---|---|---|---|---|---|
| **Persistent root** | character, player ship or vehicle, deployable, player structure, housing interior, jettisoned container, player wreck with loot rights | Yes, created by the **service transaction that creates the thing** (character create, asset mint, ledger `CreateAg`), never by a cell mid-tick | Yes | own record, `(epoch, seq)` | its own row |
| **Persistent member** | character aboard a ship; fighter in a carrier hangar; vehicle in a cargo bay | Same row, with `root_ag ≠ ag_id` | Yes, **unchanged**: `item.custody_ag` stays on the member's own stable `ag_id` and is never re-custodied on boarding | own record, stamped with the tree epoch | its row, kept in lockstep with the root |
| **Transient** | NPCs, AI ships, projectiles, missiles, unclaimed loot, spawner state | No | **Never.** Loot is a `reward_token`, minted on pickup straight into the looter's custody (05 §1.6 `Grant`) | only inside the region checkpoint | the cell's region lease generation: a superseded cell's region checkpoint and trunk messages carry the old `lease_gen` and are rejected, and it stops at the first rejection or on seeing the higher generation (05 §1.4.2) |

- **No synchronous creation.** Rows for characters and ships exist before they enter a zone. The load-time
  `Fence.Advance(ag, e, e+1, me)` runs while the player is "loading" (05 §2.4).
- **Jettisoned cargo** creates the container's row inside the same ledger transaction that moves the items
  (`CreateAg` + `MoveItem`), so custody and row appear atomically. The container shows at once as a *pending*
  entity and becomes lootable when the reply arrives.
- **Promotion.** A transient that must become persistent, such as a captured NPC ship, is promoted by a
  `CreateAg` from a coroutine. It holds no custody until the reply arrives.
- **Owned NPCs** (companions, pets, hirelings, launched drones; 06 §7.3) are not AGs. A summoned follower
  is an authority member of its owner's AG, with no fence row of its own. It travels in the owner's
  `HandoffOffer`, is checkpointed in the owner's record, and its gear and drone items stay in the owner's
  custody. An abandoned drone becomes a persistent root through `CreateAg` + `MoveItem`, like jettisoned
  cargo.

**AG trees (boarding, docking, hangars).** Physical parenting (frames, `InFrame`, `DockedTo`, 02 §4.2) is
separate from authority nesting, which exists only between persistent AGs. The fence row is
`authority_fence(ag_id, root_ag, owner_cell, owner_region, owner_lease_gen, epoch, kind, state, parked_seq)`,
with `state ∈ {active, dormant}` (dormancy below). A root has `root_ag = ag_id`, and members point at the root
of their tree (flattened, depth ≤ 3: carrier → fighter → pilot).

**Invariant: every row of a tree carries the root's epoch, owner and state.** The operations keep it true:

| Operation | When | Transaction (one, on the shard primary) |
|---|---|---|
| `Fence.Advance(root, e, e+1, owner)` | handoff, load, abort | `SELECT … WHERE root_ag=$root FOR UPDATE` checks that every row is at `e` (else `FENCE_TREE_MISMATCH` and an alert), then `UPDATE authority_fence SET epoch=$e+1, (owner_cell, owner_region, owner_lease_gen)=$owner, state='active' WHERE root_ag=$root AND epoch=$e`. A 60-member ship is one statement. A **load** accepts a dormant row (NULL owner); it accepts an active row only as a flagged **takeover** (below), and returns the previous owner |
| `Fence.Park(root, e, cell, ckpt_seq)` | logout, linkdead expiry, instance teardown, structure or interior unload, ship stowed (below) | requires every row of the tree at `(e, cell, active)`; sets `epoch = e+1`, owner fields NULL, `state = 'dormant'`, `parked_seq = ckpt_seq`. This one statement is also what returns the tree's custody items to **at rest**: no item row is touched |
| `Fence.Join(member, e_m, root, e_r, cell)` | boarding, docking, landing in a hangar | locks both trees, requires both owned by `cell` at the stated epochs, sets `e' = max(e_m, e_r) + 1` on **both** trees and `root_ag = root` on the member subtree |
| `Fence.Leave(member, e, cell)` | disembark, undock, launch | requires the tree at `(e, cell)`, sets `root_ag = member` on the member subtree and `e + 1` on **both** trees |
| `Fence.AdvanceMany([(ag, e)…], owner)` | bundle handoff: a transient root (NPC ship) with persistent riders, which stay their own roots under a co-location lease; a co-location part or whole co-location set with several roots (§6.2a) | all-or-nothing CAS of each listed root's tree |
| `Fence.AdvanceOwnedBy(region, lease_gen, owner)` | crash recovery (§6.4) | bulk advance of every **active** row the dead region owned (partial index on `owner_region WHERE state = 'active'`); dormant rows have no owner and are never resurrected |

Consequences:
- **The ledger check needs no tree walk.** `Execute` reads the custody AG's own row `FOR SHARE` and requires the
  request's `(epoch, cell)` (05 §4.1). Member rows move with the root, so once a ship is handed off, its old
  owner fails the check for every passenger's items. This closes the BENCH-6 split-brain window.
- **Boarding never waits.** Join and Leave need co-location, which boarding already takes (§6.2a). The cell
  applies the frame change at once, so the player walks aboard, and switches AG-table membership when the reply
  lands, normally the next tick. A handoff of a tree with a pending Join or Leave waits for that reply (§6.3).
- **Stale requests retry safely.** In-flight ledger requests stamped with the pre-Join/Leave epoch fail with
  `FENCE_STALE`. The cell re-stamps them and retries with the same idempotency key, which is safe because the
  stale attempt did not commit.
- **Fence gate.** While a fence operation (`Advance`, `Join`, `Leave`, `Park`) on a tree is in flight, the
  owner cell sends no new ledger request for the tree's AGs. It holds them until the reply (normally ≤ 5 ms)
  and stamps them with the epoch the reply returns.
  - A tree-wide `FOR UPDATE` therefore waits only for requests already in flight, never for a stream of new
    share locks.
  - Only requests already in flight can fail with `FENCE_STALE`.
  - The shard-primary side (lock order, the fence check as the ledger transaction's last statement, lock
    timeouts, the load budget) is in 05 §1.13 and §3.6.

**Dormancy: parking an AG that no cell simulates.** A persistent AG is *active* while a cell simulates it and
*dormant* otherwise. Without dormancy, a logged-out character's row would keep naming its last cell, so
recovery would revive it, its items would sit under a dead cell's fence, and the custody audit would fail.
- **Park sequence.** The cell stops simulating the tree, waits for its in-flight ledger replies (≤ 1 s), writes
  the final checkpoint, waits for the `Checkpoint.Write` reply (durable in `PERSIST`), and then calls
  `Fence.Park(root, e, cell, ckpt_seq)`. The next load passes `parked_seq` to the load barrier
  (`Checkpoint.Load(ag, min_stream_seq)`, 05 §1.13), so it always reads that final state. If Park fails
  because the epoch moved (a takeover won the race), the cell drops its copy.
- **Triggers.**

  | Event | Parks |
  |---|---|
  | Logout, or linkdead expiry after 5 min (§2.4) | the character's tree. A member of a tree that stays active (a passenger on another player's ship) first `Fence.Leave`s, then parks alone. An owner logging out aboard their own ship with no other active member parks the whole tree, ship included |
  | Idle instance destroyed (§7) | every persistent AG of the instance |
  | Housing interior or player structure unloaded (no visitor for its `idle_ttl`) | the structure tree |
  | Ship stowed (hangar, landing pad) with its owner offline | the ship tree |
  | Recovery finds an AG whose logout was in progress (flagged in the region checkpoint and the replicant stream) | the AG, immediately after its `(e+1, 0)` record |

- **At-rest custody.** A dormant row means its tree's custody items are **at rest**. `item.custody_ag` does
  not change on logout or login, so no per-item writes are needed. The ledger's `Execute` (05 §4.1) accepts
  service-issued ops on those items with the precondition `fence = {ag, e, dormant}`: out-of-game mail claims,
  GM restores of an offline character, and erasure step 4 (05 §6.6), which runs only after step 1's session end
  has parked every AG of the account. A concurrent load-time Advance takes the row `FOR UPDATE`, so the service
  write either commits first, and the loading cell sees it through the "ledger wins" `item_refs` reconcile, or
  fails with `FENCE_STALE`. After `FENCE_STALE` the service re-issues through the owning cell
  (`ctl.<shard>.cell.<id>.gm`, or the cell-side claim path) or waits for the next park. Active rows accept only
  their owner cell's fenced writes, as before.
- **Takeover and `released`.** A load normally targets a dormant row. If the row is active, the placement first
  routes the session to the current owner (§2.4). Only when that owner is suspect, or on another content
  version, does the loader issue a takeover Advance. The fence's outbox event is
  `fence.advanced{ag, epoch, prev_owner, owner, cause ∈ handoff | join | leave | load | park | recovery}`. For
  `load` and `recovery`, when `prev_owner` is non-NULL and differs from the new owner, the persistence gateway
  sends **`ctl.<shard>.cell.<prev>.released{ag, epoch}`**, p99 ≤ 1 s after the commit. The superseded cell
  drops the tree without checkpointing (its writes would fail the fence anyway), cancels its pending ledger
  retries for it, sends viewers `Destroy{reason: moved}` and releases its handles (§3.5). A zombie from a
  recovery stops sooner this way too. A handoff needs no `released`, because the old owner started it.

**Checkpoints across two stores.** The fence lives in the shard primary, and checkpoints land in the separate
persistence cluster (05 §3.1).
1. **Fence cache.** The persistence gateway keeps a cache `ag → (epoch, owner, state, prev_owner, cause,
   changed_at)`, fed by the ordered JetStream event `evt.<shard>.fence.advanced` (bulk events carry ≤ 1,000
   AGs).
2. **Validation.** A record whose epoch is below the cached one, or equal with a different owner, is rejected.
   A newer epoch is accepted, since cells stamp only epochs they won by CAS. On a cache miss, the gateway does
   one batched synchronous read of the primary per flush. Rejections are counted by reason, and **only
   owner-changing staleness alerts**:
   - *benign, metrics only* (`ckpt_rejected{reason=stale_same_owner|stale_inflight}`): the writer is the cached
     owner (a Join, Leave or abort re-promotion bumped the epoch under an in-flight batch), or the writer is
     `prev_owner` within 2 s of `changed_at` after a handoff or takeover (its last batch was in flight, and
     the new owner's `(e+1, 0)` record supersedes it anyway);
   - *alert* (`CheckpointZombie`): a writer below the region's current `lease_gen`, a writer that is neither
     the owner nor a recent `prev_owner`, or any write by a parker after its own Park. A parker waits for its
     final checkpoint's reply before parking, so a later write is a bug.
3. **The residual window**, where an advance has committed but its event has not arrived, is closed by the new
   owner. It writes an immediate **`(e+1, 0)` record** for every AG it gains (handoff, Join/Leave, load or
   takeover, recovery) in its next 1 s batch, so the `(epoch, seq)` merge rule (05 §1.13) supersedes anything
   the old owner writes afterwards. The load barrier covers readers.

### 6.2 Ghosts, effects, co-location

A cell C ghosts every AG it does not own whose root lies **inside C's regions or within
`M = max(class interaction range, v_max × 0.25 s) + H` of them**. Distance is measured from the receiving
cell's regions, not from the owner's boundary, so an AG leased deep inside C by co-location (§6.2a) is ghosted
by C, and by any third cell within M of it. Ghosts are fed by the same serialize-once writer (audiences
`all+server`) at 20 Hz inside or within M/2 of C's regions, else 5 Hz. Hitboxed entities within `M_hit` are
ghosted at the zone tick rate with their `HitPose` (§5.6), and their margin is `max(M, M_hit)`. The owner
re-evaluates each AG's ghost set at the 4 Hz interest refresh, and at once when a handoff or co-location
moves it. Gameplay gets
`GhostRef` (no mutators); `World::mutate()` asserts ownership; the only way to change another owner's entity
is **`send_effect()`** (R04 §9). Effects (`ApplyDamage`, `RequestDock`, `TractorAttach`, …) carry `{src AG, src epoch, dst AG, per-pair seq, tick, idempotency id}`, are
ordered per (src, dst), applied at the owner's next tick, and forwarded by an old owner during the grace
window (§6.3) or a migration's (§6.7). Ghosts serve simulation only (collision, AI, targeting, effects); **clients never
receive a ghost**, and what a client sees across a boundary comes from the owning cell (§6.6).

**Exactly-once effects, including across a recovery.** A time-based de-duplication window cannot work here:
a recovery takes ≈ 3 s to confirm plus a hitch of up to 10 s, and a checkpoint restore rolls state back.
- *Receiver.* De-duplication is by sequence, not by time. Each destination AG holds a server-audience
  `EffectInbox`: the highest applied per-pair sequence for each source AG, ≈ 8 B per pair, with entries idle
  for 60 s expiring. It changes in the same tick as the effect's result, so it moves with the AG in every
  handoff, migration, checkpoint and replicant update. A restored AG therefore knows exactly which effects
  its restored state already contains.
- *Sender outbox.* A sender keeps every cross-cell effect in a per-destination-region outbox until the trunk
  acknowledges it, and for **≥ 15 s** in any case (≈ 3 s to confirm a failure + a ≤ 10 s hitch + margin).
  While the destination region is suspect or recovering, new effects for it wait in the outbox (≤ 4,096 per
  region; beyond that the oldest are dropped and counted in `effect_outbox_dropped`).
- *Re-send.* When the region reappears at a higher `lease_gen` (§6.4 step 5) or under a new holder (§6.7), the
  sender re-sends everything it retained for that region in sequence order. The new owner drops any sequence
  at or below its restored `EffectInbox` and applies the rest, after the usual liveness and immunity checks.
- *Result.* Each effect applies exactly once relative to the state that survived. With the replicant tier
  (≤ 1 s of rollback, Phase 4) that means exactly once. A Phase 3 checkpoint restore can roll back further than
  the 15 s retention, and effects older than that are lost with the rest of the rolled-back state (§6.4). Value
  is never involved: loot and currency move through the ledger with its own idempotency keys.

**Co-location** (contact, tractor, docking, boarding, melee) is specified in §6.2a.

### 6.2a Co-location protocol (v1, Phase 3)

Some interactions cannot run as one-tick-late effects: a contact between two bodies, a tractor tow, a docking
clamp, boarding before `Fence.Join` commits, melee and grapples. Both sides must step in one Jolt
`PhysicsSystem`, so one cell must simulate both. **Co-location** moves one side's AGs to the cell that
simulates the other through an ordinary handoff (§6.3), and holds them there under a lease that suppresses the
geometric handoff trigger. It is the mechanism behind cross-cell physics, docking, tows, boarding (BENCH-6)
and melee. Viewers and ghosts never depend on where a leased AG sits (rule 5); the caps and the leash only
bound cost.

1. **Coupled pairs.** Two AG roots owned by different cells are *coupled* while one of these holds. A pair
   uncouples 2 s after its predicate stops holding.

   | Coupling | Starts when | Coupling-distance cap (cook check) |
   |---|---|---|
   | Contact | the swept AABBs of the body and the other's ghost (dead-reckoned to the current tick) overlap within the next `k` ticks while closing; `k` covers §6.3's handoff p99 plus 2 ticks (4 at 20 Hz, 5 at 60 Hz) | `v_rel × k` ticks |
   | Tractor or tow | `TractorAttach` accepted | `TractorDef.range` ≤ H |
   | Docking | `RequestDock` accepted, until `Fence.Join` commits (the ship is then a tree member, §6.1) or the dock aborts | approach corridor ≤ H |
   | Boarding | a character enters a ship's boarding volume, until `Fence.Join` commits | boarding volume ≤ 50 m |
   | Melee, grapple, carry | a melee ability or grapple targets the other within 5 m | 5 m |

   The cook fails a `TractorDef` or docking corridor longer than H, as it fails weapons past `M_hit` (§5.6).
2. **Co-location set.** A set is a connected component of the coupling graph: two AGs belong to one set when
   a chain of coupled pairs joins them, whichever cells own them. One **host cell** simulates the whole set.
   The set is named by `set_id`, the lowest root EntityId in it. Each member carries a server-audience
   `ColocLease{set_id, host, expires_tick}` component, so leases travel in handoffs, checkpoints, the replicant
   stream and migration residuals.
3. **Choosing the host ("cheaper").** When a pair couples across cells A and B, each side's *part* is the
   members of the merged set that it owns. The part that is **cheaper to move** moves. Parts are compared by:
   1. fewer AG rows, members of the trees included;
   2. then lower `ag_tick_us`, the EWMA of the part's measured per-tick cost (bodies stepped, AI, script fuel)
      from per-AG profiling counters;
   3. then the part whose centroid lies farther from its own cell's regions;
   4. then the higher cell ID.

   Both cells compute the same answer from `ColocRequest{pair, rows, tick_us, centroid}`. The mover's cell
   sends one `HandoffOffer` carrying the lease for its whole part: `Fence.Advance` for one tree, or
   `Fence.AdvanceMany` over several roots, all or nothing. A set that spans three cells merges in two steps,
   each by this rule.
4. **Caps and refusal.** The receiving cell answers `ColocNack{reason}` instead of `HandoffAccept` when any
   of these holds:
   - its tick p95 is above 70 % of budget, or it is at overload stage ≥ 2 (§8);
   - the merged set would exceed **32 AG roots or 128 AG rows**, or **5 % of the host's tick budget** in
     `ag_tick_us` (1.75 ms at 20 Hz);
   - its **leased-in** load (members it hosts whose roots lie outside its regions) would exceed **64 roots or
     10 % of its tick budget**;
   - the region is in a migration's quiesce, freeze or catch-up (§6.7), or the control plane is degraded
     (05 §1.4.4).

   If B refuses to host, A hosts instead when A passes the same tests. Otherwise the pair runs in
   **effect-only mode** and counts in `coloc_refused{reason}`:
   - *Contact:* each owner adds the other's ghost as a dynamic **puppet** body with the ghost's real mass and
     inertia, reset each tick to the ghost's state dead-reckoned to the current tick, and keeps only its own
     body's result; the puppet's result is discarded. Both owners solve nearly the same contact, so momentum
     is conserved to within one tick of acceleration (NS-3.12(c) audits it).
   - *Tractor:* the tug's owner sends a per-tick `TractorForce` effect, a spring-damper on the ghost's
     position capped at the beam's force, which the towed AG's owner applies one tick late.
   - *Docking and boarding:* the clamp or airlock holds ("port busy") and retries every 1 s.
   - *Melee:* the attacker's cell resolves it against the target's ghost hit history like a shot (§5.6), and
     damage lands by `ApplyDamage`.

   A pair refused for a set cap stays effect-only until it uncouples, so no chain can grow a set past the cap,
   and a 100-ship furball on a seam splits into many small sets rather than one cell's worth. Each of these
   choices depends on measured load, so every host choice, NACK, fallback, leash handoff, release and overstay
   is logged by the cell that makes it as a `ColocDecision` replay event with its inputs (§10.2).
5. **Location: presence, ghosts and the leash.** A leased AG usually sits inside another cell's region.
   - *Presence follows location (§6.6).* Each cell reports class and party presence **per region its owned
     entities occupy**, not per region it owns. A cell that tentatively accepts a co-located part sends the
     presence delta with its `HandoffAccept`, outside the 1 Hz limit. Gateways therefore register a viewer at
     every cell that owns entities inside the regions within the viewer's radius, and the handoff registry
     (§6.6) covers viewers that already had the part.
   - *Ghosts follow location (§6.2).* A leased AG inside B is ghosted by B at 20 Hz (at the tick rate if
     hitboxed), and by any third cell within M.
   - *Leash.* Admission keeps a set's radius (the largest member distance from its centroid) ≤ H, and every
     tick the host checks the centroid. When the centroid has been more than H past the host's boundary for
     3 ticks, the **whole set hands off together** to the cell owning the centroid's region: one tree
     handoff, or `Fence.AdvanceMany` over every root, with the lease in the offer. Members therefore stay
     within **2H** of the host's regions. A member that ends up beyond 2H anyway (a collision flinging a
     body) has its farthest pair uncoupled into effect-only mode (`coloc_leash_break`). A tow at 1 km/s thus
     changes host once per boundary it crosses, H after the boundary, like a single AG, and no part of it is
     ever outside a nearby viewer's registration or a neighbour's ghost set.
6. **Lease lifecycle.**
   - *Start.* The lease runs 10 s of zone time from the handoff's first authoritative tick (T+1).
   - *Renewal* is host-local and needs no message. At every 1 s boundary, while any pair of the set was
     coupled within the last 2 s, the host sets `expires_tick = now + 10 s`. In degraded mode leases renew
     unconditionally, because handoffs are deferred anyway (05 §1.4.4).
   - *Split.* When the coupling graph splits, each component becomes its own set with the same
     `expires_tick`.
   - *Release.* At `expires_tick` the host drops the lease and applies §6.3's trigger to every member in the
     same tick. A member more than H past the boundary has already dwelled 3 ticks, so it hands off at once,
     grouped per destination (one offer per tree; `AdvanceMany` for a transient bundle). Members within H
     stay. A destination that NACKs for overload leaves the member with its host under §6.3's retry (1, 2,
     4 s) and counts `coloc_overstay`; presence and ghosts stay correct meanwhile, because they follow
     location.
   - *Migration (§6.7).* A region in a migration's quiesce starts no co-location, and neighbours defer
     co-locations into it. Leases travel in the residual. A lease that would expire during the quiesce or the
     hitch is extended to T + 1 s, so no release handoff falls inside the freeze.
   - *Crash.* Leased AGs are owned under the host's region lease (`owner_region`, §6.1), so the host's
     recovery restores them with their leases (§6.4). A crash of the region where they sit changes nothing
     for them: its ghosts freeze and effects for it wait in the outbox (§6.2).
7. **Cost and latency.** Co-location is a handoff, so it commits within §6.3's p99 (< 100 ms at 20 Hz), which is
   why contact prediction looks `k` ticks ahead. A contact that arrives before the commit resolves in
   effect-only mode and counts `coloc_late`. The caps bound a host's extra load at 10 % of its tick budget, and
   any one set at 5 %.

In space, v2 partitions by **interaction clusters** (k-d median splits), not uniform grids (R07 §2.3), which
keeps most sets inside one cell. NS-3.10's tow clause and NS-3.12(c) accept this section.

### 6.3 Handoff protocol (v1)

A owns x at epoch e; B holds a ghost. x is a tree root (§6.1): the offer carries every member, and one
`Fence.Advance` moves the whole tree (a transient root with persistent riders uses `Fence.AdvanceMany`).
Trigger: x's root is **H** past the boundary for 3 ticks with no co-location lease (§6.2a) and no pending
`Fence.Join/Leave` on the tree (H = max(10 % cell size, 50 m ground / 2 km space)). Conversely, no Join or Leave
is issued while the tree has a handoff in flight; a boarding that arrives meanwhile is queued for ≤ 1 handoff.

```
A (owner, e)                                        B (ghost)                          Fence / Gateway
T  end of tick: stop simulating x; buffer its inputs/effects
   HandoffOffer{x, e→e+1, T, server-audience state of all AG members, last
     input seqs, buffered inputs, effect seqs, relevance registry
     with per-session resync records (§6.6)}                   ──►  validate (ghost epoch == e);
                                                                 tentative promote ◄── HandoffAccept
   Fence.Advance(x, e, e+1, owner=B) ─────────────────────────────────────────────► CAS ok
   demote x to ghost; forward buffered inputs/effects; HandoffCommit ──► commit: simulate T+1,
                                                                        replicate, persist
   RouteUpdate{sessions controlling x or a member → B, e+1} ──────────────────────► gateway re-routes input
T+ grace 500 ms: A forwards late inputs/effects (B de-dups by input seq)
```

B neither replicates nor persists x before Commit. Each registry session's record moves to B with a
`resync_mask`, and B sends that session only those fields, never a create or a full state (§6.6), so nothing
pops. A writes ticks ≤ T and B writes ticks ≥ T+1, so there is one source per tick. Input sequences continue,
so prediction is unaffected. Offer → B's first
authoritative tick ≤ 1 tick + fence CAS: **p99 < 100 ms at 20 Hz** (Phase 3), < 35 ms at 60 Hz, < 50 ms in v2.
**Abort:** B NACKs (epoch, overload, decode), no Accept within 250 ms, or CAS fails → A re-promotes x at
**epoch e+2** (`Fence.Advance(e→e+2)`), catches up from buffered inputs, sends `HandoffCancel`; B's tentative
copy is fenced out. If B times out awaiting Commit it reads the fence: owner = B at e+1 (A died after the CAS)
→ self-commit; otherwise drop. Retries back off 1/2/4 s, then alert. NACKs depend on load and retries on wall
time, so each side logs every offer, accept, NACK, deferral, commit, abort, self-commit and drop as a
`HandoffDecision` replay event (§10.2); zone transfers (§7) log the same events.

### 6.4 Crash recovery

**Detection (05 §1.4.3):** gateways mark a cell *suspect* after 500 ms of trunk silence, freeze its sessions
without disconnecting, and report it (`ReportSuspect`, listing silent and live trunk peers so the orchestrator
can judge the gateway's own health). The orchestrator confirms a failure only from two independent signals:
heartbeat silence ≥ 3 s plus all-gateway trunk silence, a placer process exit, or a failed direct probe (the
NATS ping **and** a gateway trunk probe both fail; a cell that answers only on its trunk is NATS-isolated,
not dead). Correlated silence confined to one host (from Phase 4, one rack) is a failure-domain loss and is
recovered at once; only wider or unexplained silence enters degraded mode, during which it never reassigns
(05 §1.4.3–1.4.4). A cell that cannot reach the
control plane keeps simulating; it stops only when it sees a higher region generation or has a write rejected,
and gateways and neighbour cells drop its trunk messages below the current `(region, lease_gen)`. Dumps go to
sentry-native (ADR-013).

**Warm standby** (Phase 2; preloaded Agones Ready processes, one per 8 cells per class; from Phase 3 sized so
that the loss of the largest host, or rack from Phase 4, still leaves enough standbys outside it, 05 §1.4.6).
Hitch budget
≤ 10 s from failure confirmation (≈ 3 s after the crash):

| Step | Action | Budget |
|---|---|---|
| 1 | The orchestrator allocates region lease generation `g+1` in PG and assigns standby S, telling it the region | ≤ 0.5 s |
| 2 | S calls **`Fence.AdvanceOwnedBy(region, gen = g+1, S)`**: one transaction, `UPDATE authority_fence SET epoch = epoch + 1, owner_cell = $S, owner_lease_gen = $gen WHERE owner_region = $region AND state = 'active' AND owner_lease_gen < $gen RETURNING ag_id, root_ag, epoch`, on the partial index `(owner_region) WHERE state = 'active'`. Parked AGs have NULL owners, so however many were parked from the region (NS-2.7 tests ≥ 50k), none is returned or revived. Trees stay consistent because all their rows share one owner. AGs a committed handoff already moved belong to another region and are skipped by construction. A delayed call from a superseded assignment (an older `g`) matches nothing. One bulk `fence.advanced` event feeds the persistence cache | **≤ 1 s p99 at 5k AGs** (NS-2.7) |
| 3 | S loads checkpoints for the returned AGs through the load barrier (64 in flight), writes their `(e+1, 0)` records (§6.1), and reloads the region checkpoint (transients, spawners). Frozen handle blocks stay frozen (§3.5). Checkpoints come from different times, so S runs the **consistency pass** below (steps 2–4) before resuming | ≤ 6 s |
| 4 | Value needs no rebuild (Ledger-owned; `item_refs` reconcile, 05 §1.13); activity progress rehydrates from the activity service (R05-P0-5) | in parallel |
| 5 | S publishes `RouteUpdate`s. Gateways re-attach the region's local sessions and re-register their remote viewers at S (§6.6). S resyncs with full-mask updates, not creates (clients kept the entities, frozen under §5.3's extrapolation rule), in priority order within each session's grant, and sends `ClockReset` (§5.1). Input sequences continue. Neighbours re-send their outboxed effects, which S de-duplicates against the restored `EffectInbox` (§6.2). AGs flagged as logging out are parked (§6.1). In a multi-cell zone S takes the `ZoneSchedule` from the leader and starts at its current tick: the restored state stands still for the gap, tick-stamped timers keep zone time, and the recording logs `ZoneRejoin{from, to}` (§3.5, §10.2) | ≤ 2 s |

Mid-handoff AGs resolve without special cases. If A died after the CAS, the row names B, so S skips it and B
self-commits (§6.3). If A died before the CAS, S takes the AG, and B's timeout read of the fence says to drop its
tentative copy. Target: **≤ 10 s hitch, ≥ 99 % sessions retained, loss ≤ checkpoint window** (30 s Phase 3,
10 s Phase 5), or **≤ 1 s** in a zone served by the replicant tier below.
**Gateway crash:** reconnect ticket → new token → another gateway in 1–3 s for one instance, avatar linkdead
meanwhile. A whole box (≤ 8k sessions) is absorbed by the N+1 headroom rule (§2.6) and the jittered,
burst-sized reconnect path (§2.4): ≥ 99 % of its sessions are back in the world in ≤ 10 s (NS-4.7).

**Replicant tier** (Phase 4, stage v1.5; ADR-007). It brings Helios to Star Citizen's shipped recovery model, in
which a replication layer holds the latest entity state so a replacement server resumes without disconnecting
anyone on any server of the shard (R04 §4.1).
- **Scope.** The zone record's `crashRecovery` is **`replicant` by default for every persistent world-zone
  profile**: ground hubs and stations, open space (systems) and fleet battles (§3.4), single-cell or
  multi-cell. It is mandatory for multi-cell zones. Activity, phase and housing instances default to
  `checkpoint` and may opt in: they are small and short-lived, activity progress rehydrates from the activity
  service (R05-P0-5), and housing is mostly static. So from Phase 4 a cell crash in any world zone, where
  nearly all players are, loses ≤ 1 s of non-value state (AAA-SRV-9 Ph4).
- **Process and placement.** A replicant is `helios-cell --replicant`: the headless binary with ECS and
  replication descriptors, but no physics steps, AI, Luau or tick of its own. The orchestrator places one per
  **≤ 4 world-zone cells, from one zone instance or several** (streams are fenced per region, so mixing zones is
  safe), on a different host (and, while the blast radius is a rack, a different rack) from every one of those
  cells (`AssignReplicant`, 05 §1.4). A replicant is **2 vCPU and 4 GB**: it ingests ≤ 80 Mbit/s of chunks
  (≈ 1 core measured by NS-4.6), and its state is ≈ 1 GB per 4 cells. At Phase 4 that is one replicant per
  4 of the 100–200 world cells, ≈ 6 % more compute (05 §6.4).
- **Stream.** Each cell sends its replicant, on a dedicated HTP trunk, the field chunks that stage 6 already
  serializes once per tick (§4.2) for every audience (`all`, `owner`, `server`), which covers the AG's
  checkpoint state. It sends them for every AG it owns and for its region's transients and spawners, plus
  create, destroy and AG-table deltas `(ag, root, epoch)`. AGs with a controlling session are sent every tick;
  the rest are sent dirty-only at ≥ 5 Hz, with change masks OR-ed between sends; fleet-battle zones send every
  tick. An AG's dirty fields always travel together in one tick's chunks, so each AG is internally
  consistent. Every message carries `(region, lease_gen, tick)`, and each tick's stream ends with
  **`TickFlush{tick, chunk_count, state_hash}`**, the same commit marker gateways use (§6.6). Cost:
  ≤ 20 Mbit/s per 500-player cell (about a sixth of its client egress, §4.7) and ≤ 0.3 ms of tick time.
- **State.** The replicant stages each tick's chunks and **commits them atomically** when that tick's
  `TickFlush` arrives with a matching chunk count. It never holds a partial tick, and a tick whose flush never
  comes (the cell died mid-send) is discarded. It applies committed chunks to an in-RAM shadow of quantized
  state and records, per entity, the epoch and tick of its last committed update. Like a gateway, it drops messages below the `(region, lease_gen)` it knows
  from `DIRECTORY` (05 §1.4.2), and it drops an AG's chunks below the highest epoch it has seen for that AG. On
  a committed handoff the old owner sends `Released{ag, epoch}`, and the new owner's stream takes over from its
  first authoritative tick (§6.3). A replicant holds no authority, never serves clients and never writes
  persistence; checkpoints still flow from cells (§6.1). RAM is ≈ 1 GB per 4 cells.
- **Recovery from RAM.** Step 3 of the table above changes. S pulls from the region's replicant, on a BULK
  trunk, the latest state of every AG that `AdvanceOwnedBy` returned whose replicant epoch equals the returned
  epoch − 1, plus the region's transients at `lease_gen = g`. It writes their `(e+1, 0)` records and resumes
  (≤ 1.5 s at 5k AGs). AGs the replicant lacks, or holds at another epoch, load from checkpoints as before.
  The state lost is the replicant's lag, **≤ 1 s** (AAA-SRV-9 Ph4, NS-4.6), instead of the 30 s checkpoint
  window. The hitch stays inside the ≤ 10 s budget and is typically ≈ 5 s after confirmation.
- **Restore to one tick (consistency pass).** Committed ticks alone do not make the region consistent,
  because dirty-only entities were last sent up to 200 ms before the last committed tick. At 300–1,500 m/s
  that is 60–300 m of relative error, enough for interpenetrating ships, false collisions and AI aiming at
  empty space. S therefore restores every entity at one **restore tick R**, the replicant's last committed
  tick, before its first simulated tick:
  1. entities sent every tick (controlled AGs, fleet-battle zones) are already at R;
  2. `CommandKinematic` ships and other command-replicated entities (§5.4) run their fixed-point integrator
     from the command's start state to R, which is exact. Every other moving body is dead-reckoned,
     frame-local, from its last committed transform with its replicated linear and angular velocity over
     `R − last_tick`. Fast projectiles re-simulate from their spawn event `{origin, dir, speed, tick}`;
  3. a **depenetration pass** runs per grid: one Jolt broad- and narrow-phase query over the restored bodies.
     Each pair overlapping by more than 5 cm is pushed apart along the contact normal. The dynamic or lighter
     body moves, positions change and velocities do not, for up to 4 iterations. A pair still overlapping gets
     its collision disabled until the bodies separate, so the solver never turns an overlap into an impulse.
     Speeds above the class `v_max` are clamped;
  4. bodies flagged at rest restore asleep.

  The pass costs ≤ 50 ms at 20k bodies and is inside the step budgets. Checkpoint restores run steps 2–4 as
  well.
- **Replicant failure.** The orchestrator assigns a new replicant, and its cells resend full state (the lazily
  built full-state chunks) within ≤ 5 s. A cell crash inside that window falls back to checkpoints. Players see
  nothing.
- **Scope.** Clients still reach cells only through gateways, and interest and priority stay in cells. The
  Phase 5 gateway replication layer (§6.5, v2) moves those onto this tier, which makes a crash invisible
  (≤ 1 s hitch, NS-5.3). Counsel reviews Improbable's US 11,792,306 before WP-4.3 (09 K18). If the review
  requires a change, the same stream feeds a per-zone hot standby cell instead, with the same ≤ 1 s RPO.

### 6.5 Staging (ADR-007)

| Stage | Phase | Authority | Gateway |
|---|---|---|---|
| **v0** | 1–2 | One cell per zone instance; AG/epoch/fence/handoff/ghost APIs exist, exercised by zone transitions; AG trees and `Fence.Join/Leave` from Phase 1 (boarding the Kestrel) | Forwards chunks 1:1 |
| **v1** | 3 | Static multi-cell zones, zone leader (§3.5), ghosts, effects, co-location, handoff p99 < 100 ms, planned region migration with hitch p99 ≤ 1 s (§6.7) | **View composition (§6.6):** registers each session's viewpoint at every contributing cell, arbitrates its budget across them and packs their chunks |
| **v1.5** | 4 | As v1, plus the **replicant tier** (§6.4): every persistent world zone, single-cell or multi-cell, streams its AG state to replicants, so a cell crash there loses ≤ 1 s of state with no disconnect | As v1 |
| **v2** | 5 | Dynamic split (> 70 % budget for N s) / merge (< 30 % for minutes), minimum cell size, cluster partitioning, handoff < 50 ms | **Replication layer:** cells publish field chunks once per tick to the replicant tier; gateways and replicants own interest, priority and ghost records, so cell crashes and handoffs become invisible |

R04-P0-7's "replication layer from day one" is met in stages: the v0 gateway seam separates session from
simulation from Phase 1, the stateful replicant tier arrives in Phase 4 (v1.5), and the client-facing layer
arrives in v2 (ADR-007). Star Citizen's shipped Hybrid service already combines the stateful and client-facing
parts: its replicants hold entity state and stream it to clients through gateways (Alpha 3.18, with PES), and
static meshing runs on top (Alpha 4.0). Helios reaches the same player-visible crash recovery in Phase 4 (no
disconnect, ≤ 1 s of state lost) while clients keep receiving replication from cells. Moving client
replication onto the tier waits for v2, together with dynamic meshing, which SC has not shipped either
(R04 §4.1–4.2).

### 6.6 v1 view composition: seeing across cell boundaries (Phase 3)

Ghosts cover only a margin M around a boundary (§6.2), but relevance radii reach 400 m for characters, 25 km
for small craft and the whole system for capitals and stations (§4.3). So in v1 a client's view is assembled
from several cells, under one rule: **each cell replicates only the entities whose AG it owns, to every session
whose view reaches its regions.** Ghosts are never written to clients. The gateway registers viewers,
arbitrates budgets and packs. The wire format is unchanged, and v2 moves the same duties into the replication
layer (§6.5).

**Roles for a session s.**
- **Home cell:** owns the AG that s controls, and is its input route (§6.3). It supplies s's self and
  owner-audience state, CONTROL and s's viewpoint.
- **Contributing cells:** every cell where s is registered, the home cell included. Each keeps a per-(s, cell)
  `ConnectionState`: ghost records, acked motion baselines and priority accumulators for **its own entities
  only**.

**Viewer registration.**
1. **Viewpoint.** At each staggered 4 Hz interest refresh (§4.3), the home cell sends the gateway
   `ViewerState{session, frame handle, cell index, offset, velocity, instance, layer, phase_mask, party_id,
   fleet_id, sensor class, relations[≤ 16 (handle, kind ∈ target | attacker | healer)], hint_cells}`
   (≈ 48 B typical). `hint_cells` names the cells that sent s's AG an effect in the last 10 s, which covers
   attackers outside s's relevance radius.
2. **Region map.** The zone leader publishes the **`RegionMap`** to the zone's gateways: region bounds in the
   zone frame, taken from the build's `ZonePartitionDef` (static in v1, 05 §1.4.2), plus region → cell
   ownership, republished on migration and recovery. Each cell pushes to the gateways, on change and at most
   1 Hz, its **`ClassPresence{region → class mask}`** for **every region its owned entities occupy**: its own
   regions, plus any neighbour region where an AG it owns sits, such as one within H of the boundary or leased
   by co-location (§6.2a). Presence follows location, not ownership of the region. A change caused by a
   handoff or co-location is pushed with the `HandoffAccept`, outside the 1 Hz limit. Each cell also pushes
   its **party presence** (the party and fleet IDs of members it owns).
3. **Registration set.** The gateway registers s at cell c, at hash level L_k, when:
   - c reports class k in some region R with `dist(viewpoint, R) ≤ 1.15 × r_k + v_max(s) × 0.5 s + H`. That
     is the leave radius, plus two refresh periods of viewer motion, plus the handoff distance, so an entity
     that hands off into R is always already covered. Because presence is per occupied region, s registers at
     every cell that owns entities inside the regions within its radius, including the host of a leased AG
     deep inside R;
   - or k is a system-relevance class (capital, station, zone globals) and c reports one anywhere in the zone;
   - or the cell owns a member of s's party or fleet, or is in `hint_cells`. These give the `party` and
     `relation` levels, which admit only those entities (core LOD).
4. **Fan-out.** The gateway forwards `RemoteViewer{ViewerState, level_mask}` to every registered cell. A
   remote cell applies the same scope filter as a local one (instance, layer, `phase_mask`, owner and party,
   cloak and detection against s's sensor class; §4.3, §9) and the same priority formula (§4.4: relation ×3
   from the relation list, party ×1.5). Its query touches only the masked hash levels, so a distant region
   costs one L3 lookup.
5. **Leaving.** A cell leaves the set only after 2 s outside it. The gateway then sends `ViewerLeave`, the
   cell sends reliable Destroys for everything it had sent s, and frees the per-(s, cell) state once they are
   acked.
6. **On demand.** A cell that needs s's registration sooner (for example after adopting a handoff record)
   sends `ViewerRequest{s}`, and the gateway answers from its latest `ViewerState` within one trunk RTT.

**One source per NetHandle.**
- A cell writes entity x only while it owns x's AG at the current epoch, so for any tick at most one cell
  writes x: A writes ticks ≤ T and B writes ticks ≥ T+1 (§6.3).
- Chunks carry their tick. The client applies an entity update only if its tick is newer than the last one
  it applied for that handle, so a late chunk from the old owner cannot overwrite the new owner's state.
- Handles are zone-wide (§4.6), so every source agrees on `(handle, gen) → EntityId`. In test builds each
  entity write carries a 2 B source-cell tag, and the handle-audit bot (§10.3) fails on any `(handle, tick)`
  written by two cells.

**When x hands off (A → B).** The `HandoffOffer`'s relevance registry carries one record per session that has
x: `{session, resync_mask, acked_baseline_tick, acc}`, ≈ 16 B, so a capital seen by 2,000 sessions adds
≈ 32 KB of BULK slices.
- `resync_mask` = lost ∪ in flight ∪ changed since last send, which covers every field the client might lack.
- B adopts the records at commit and sends each session only the masked fields, with motion in the absolute
  size class (≈ 25–35 B per entity). There are no creates and no full-state bursts.
- A drops loss notifications for x that arrive after T, because their masks are already in `resync_mask`.
- If s is not yet registered at B, B issues `ViewerRequest` and holds the record. If s is still unregistered
  after 1 s, B destroys x for s, which NS-3.10 counts as a pop and requires to be 0.

**When the viewer's own group hands off (home A → B).** The per-(s, cell) states stay where they are. A keeps
writing its own entities to s, now as a remote contributor; s is within H of A's region by construction. B was
already registered for s. Only the AG moves: records for its members (self, owned drones, cargo; owner
audience) travel in the `HandoffOffer` like any entity's. B becomes the source of `ViewerState` and CONTROL,
and the gateway switches the home role on the `RouteUpdate`. No baseline is invalidated, so a crossing costs s
only the resync masks of its own AG's entities.

**Budget arbitration at the gateway.** The gateway owns s's STATE budget (§2.5). Each tick:
1. **Demand.** Every (session, cell) pair may write up to a 48 B **implicit floor** per tick without asking.
   That covers far-tier and L3 traffic and guarantees max-staleness sends (§4.4). A pair that wants more
   reports `Demand{session, mandatory_bytes, want_bytes, w}` with its chunk for tick n. Here `w` is the sum
   of the accumulators of its top K = 32 unsent candidates, and `want_bytes` is their encoded size.
2. **Grants for tick n+1** come from weighted water-filling:
   - reserve the home cell's mandatory bytes (CONTROL, owner state), each cell's creates and destroys up to
     25 % of the budget per cell (the rest carries over), and the EWMA of implicit-floor use, which the
     gateway measures from the chunks it packs;
   - split the remainder in proportion to `w`, cap each cell at its `want_bytes`, and redistribute the surplus.

   Grants go back batched per trunk as `Grant{session, bytes}` (3 B). If one tick's chunks still exceed the
   budget, the gateway defers the lowest-`w` cell's chunk to the next packet. A chunk deferred twice is
   dropped and reported lost to its cell, and the cell's `lost_mask` repairs it (§4.2).
3. **Writing.** A cell never writes past its grant (entity writes are atomic, §4.4). Every cell computes
   accumulators with the same formula and the same viewer data, so `w` is comparable across cells. The
   one-tick lag only delays redistribution by 50 ms at 20 Hz, while accumulators keep growing.
4. **Packing.** The gateway packs s's chunks for tick n when every contributor with a grant has flushed tick n
   (`TickFlush{tick}` per trunk), or 2 ms after the first chunk, whichever is first. Cells tick in phase (skew
   ≤ 1 ms, §3.5). A chunk that misses the deadline rides the next packet, where the per-handle tick rule makes
   it harmless. Acks and losses map back per (cell, chunk), as in v0.

**Failures.**
- *A contributor becomes suspect.* Its chunks stop, and its entities stay on the client, frozen under the §5.3
  extrapolation rule; nothing is destroyed. The standby that takes the region gets the registrations from the
  gateways with its `RouteUpdate` and resyncs with full-mask updates (§6.4 step 5).
- *The home cell becomes suspect.* The gateway keeps s's registrations alive at the last `ViewerState`, so the
  rest of the view keeps flowing.
- *A contributor migrates* (§6.7). The gateway re-keys s's per-(s, cell) grant and ack maps to the target on
  the `RouteUpdate`, and the target resyncs from the session records in the residual. If the migration hold
  expires first, the gateway treats the region as a suspect contributor, as above, but does not report it
  (`ReportSuspect`) while its `region_migration` is open, and the rejoining simulator resyncs as after a
  recovery.
- *The gateway is drained* (a deploy or host maintenance, Phase 4). This is a relocation, not a death (§2.4,
  05 §6.3.1). On `ClientRebind{s, e+1, mode = relocate}` from the new gateway, each contributor:
  - re-keys its per-(s, cell) state to the new gateway's trunk and **keeps its acked baselines**, because the
    client's entity table is continuous;
  - ORs every chunk mask in flight via the old gateway into `lost_mask`;
  - sends `RebindFence` on the old trunk as its last message for s;
  - writes to the new trunk from its next tick.

  No `HeldEntities`, full mask, create or destroy is involved. BE-A14 (Ph4) audits the result with NS-4.7's
  field auditors.
- *The gateway dies* (one process, or a whole box of ≤ 8k sessions, §2.6). Three rules keep the view
  correct:
  1. **Stop writing into dead trunks.** After 2 s of silence on a gateway trunk, or at once on the
     orchestrator's `ctl.<shard>.cell.all.gateway_dead{gateway}`, every cell stops writing chunks and `Demand`
     for that gateway's sessions and frees their per-(s, cell) states, local and remote. The home cell keeps
     each avatar linkdead (§2.4). No cell writes to a dead trunk for more than 2 s (`gw_dead_trunk_writes`).
     If the old gateway is alive, as after NAT rebinding, it sends `SessionMoved{s, session_epoch}` instead,
     and contributors keep s's state suspended for ≤ 30 s, awaiting the rebind.
  2. **Rebind everywhere.** The client rejoins on another gateway with a reconnect ticket (§2.4). With the
     handshake it sends **`HeldEntities{session_epoch, handles}`**, its entity table as a sorted delta-varint
     list (≈ 4 KB for 2,000 entities). It goes on BULK and is exempt, once per `session_epoch`, from the
     64 kbit/s input bucket. The new gateway sends `ClientRebind{s, session_epoch}` to the home cell, gets its
     `ViewerState` and computes the registration set. It then sends `ClientRebind` with the held list, plus a
     `RemoteViewer`, to **every** contributing cell it registers, not only the home cell.
  3. **Repair every field.** Each contributor builds a fresh per-(s, cell) state from the held list:
     - a handle it owns that is still relevant gets a ghost record with a **full `lost_mask`**, which means a
       full-mask update, not a create;
     - a handle it owns that is no longer relevant gets a reliable Destroy;
     - a relevant entity missing from the list gets a Create.

     There are no baselines, so motion uses the absolute size class. A contributor that kept suspended state
     (rule 1) instead ORs every in-flight chunk mask for s into `lost_mask` and invalidates its baselines. The
     chunks that died with the gateway are never acked or reported lost, so this step is what repairs them.
     Either way every field the client might lack, including one that changed once (a door, a shield state),
     is in a mask.

  The resync runs in priority order within the session's grants: the near tier within ≤ 1 s (§2.4), the rest
  within max staleness (≤ 5 s). NS-4.7's field-state audit checks the result.

**Costs and fallback.**
- The per-pair CPU model and per-cell totals are in §3.3, and trunk traffic is in §4.7. In 4-cell Harrow orbit
  (2,000 players), stage 7 is ≈ 1.5 ms wall against its 3 ms budget. In a 10-cell, 5,000-player zone (Ph4) it
  is ≈ 2.3 ms. Client downstream is unchanged, because the gateway enforces the §2.5 budget over the union of
  contributors.
- **Far-broadcast fallback.** If stage 7 exceeds 2.4 ms p99 (80 %) for 10 s, a cell switches its L3-only
  remote viewers to far-broadcast chunks: one chunk per (scope group, tick) with absolute motion encoding,
  which the gateway copies to each member session. Creates and destroys stay per session. This cuts the L3
  cost from ≈ 1 µs to ≈ 0.1 µs per viewer.
- Remote viewers take part in §8's stage 2 like local ones: far rates halve and interest drops to 2 Hz.

### 6.7 Planned region migration (Phase 3)

A **planned migration** moves one region of a zone instance, with everything simulated under its lease, from a
live source process P to a target process Q. Nobody is disconnected and no state is lost. It is EVE's model of
moving a solar system between nodes (R01-P0-1), built from primitives that already exist: the replicant stream
(§6.4), handoff's freeze and forward (§6.3), the region lease (05 §1.4.2) and a cooperative bulk fence call.

Handoff cannot do this job, for two reasons:
- it moves one AG tree to a *neighbour* region, and a single-cell zone has no neighbour. Every v0 zone, every
  packed instance and, even from Phase 3, most zones (hubs, stations, systems, instances) are single-cell;
- transients, spawners, the region lease, the region checkpoint, handle blocks and the zone-leader role are
  not AGs.

**Uses.**

| Operation | Trigger | What migrates | Notes |
|---|---|---|---|
| Rolling N/N+1 cell restart (05 §1.14.1, AAA-STB-6) | rollout controller | every region on a build-N process, to a build-N+1 process | append-only schemas make the residual readable across N and N+1 (§7); third-party formats follow `sim_abi` (below) |
| Epoch flip, step 4 (05 §1.14.1) | orchestrator, at the flip | every region, to an E+1 process | the flip has already disconnected sessions, so no session records travel. Q remaps content-placed handles by EntityId through E+1's container index tables, and clients get creates when they rejoin |
| `Drain(process, reason, deadline)` | ops, host maintenance, scale-in, rebalancing, packing and unpacking (§3.1) | every region on the process | below |
| `PreProvision(zone, time, size)` | calendar, structure timers (R01-P1-14) | the zone's regions, to a reinforced host, ahead of the fight | below |

**Protocol.** Region ρ of instance I is held by P at lease generation g. Budgets are for the reference
500-player, 20k-entity single-cell zone.

| Step | Where | Action | Budget |
|---|---|---|---|
| 0. Prepare | orchestrator, Q | The orchestrator refuses while degraded (05 §1.4.4). It picks Q under 05 §1.4.6's placement rules (failure-domain spread; never a host that runs another cell of I), records `region_migration(region, from, to, g, state = preparing)` in PG, and sends `MigrateBegin{ρ, g, P, Q}` to P, Q, I's gateways, its neighbour cells, its replicant and its zone leader, which pre-arm to accept Q at g+1. Q loads the zone template and containers for P's content pin (compat epoch and server part), and opens a dedicated migration trunk to P (K = 4 connections, ≈ 100 MB/s at NS-0.7's per-connection rate) plus trunks to the gateways, neighbours and replicant. P and Q exchange their `sim_abi` (below), which fixes the physics path before anything freezes | ≤ 30 s; not in the hitch |
| 1. Pre-copy | P → Q | P streams ρ to Q in the replicant format (§6.4): lazily built full-state chunks for every AG, transient and spawner in all audiences, then every tick's chunks and `TickFlush`. Q applies only committed ticks, into an inert world that neither simulates, replicates nor persists, like a tentative handoff promotion. Phase 3 builds this stream for migration, and the Phase 4 replicant tier reuses it | initial ≈ 10 MB in ≤ 2 s, then ≤ 25 Mbit/s and ≤ 0.3 ms of P's tick |
| 2. Quiesce | P, neighbours | P announces the freeze tick T to Q, the neighbours and the zone leader. For a quiesce window of 500 ms before T (one tick in a zone at ≤ 2 Hz), P starts no fence operation (`Join`, `Leave`, `Park`, `Advance`), handoff, co-location (§6.2a), zone transfer or instance entry for ρ. Boardings and arrivals queue as during a handoff (§6.3), and neighbours defer handoffs and co-locations into ρ. Fence calls and handoffs already in flight complete (p99 < 5 ms and < 100 ms). In a multi-cell zone the leader issues the `MigrationHold` for T (Clock, below) | 0.5 s; not in the hitch |
| 3. Freeze | P | At the end of tick T, P stops simulating ρ, buffers ρ's inputs and effects, and sends the **residual** (table below) on the migration trunk | ≤ 100 ms |
| 4. Verify | Q | Q applies the residual and recomputes tick T's state hash: §10.2's xxh3 over quantized replicated and server-audience state, over the fields of P's schema version, plus the Jolt state blob when both sides' `sim_abi.physics` match (otherwise the portable physics path, below). It answers `MigrateAck{ρ, T, hash}` or `MigrateNack{reason}` | ≤ 100 ms (+ ≤ 50 ms portable) |
| 5. Commit | orchestrator | On a matching ACK, one term-fenced PG transaction: `region_lease` goes to generation g+1 with holder Q; `zone_leader` moves to Q at `leader_gen + 1` if P leads; `zone_handle_block.holder_cell` goes from P to Q for P's blocks, which stay active (§3.5); `region_migration.state` becomes `committed`. `DIRECTORY` is updated after it, and P and Q are told. **This transaction is the commit point** | ≤ 100 ms p99 |
| 6. Resume | Q, P | Q simulates T+1 (in a multi-cell zone, after its `MigrationReady` and the leader's `MigrationRelease`, Clock below) and sends `RouteUpdate{sessions homed in ρ → Q, (ρ, g+1)}`; the zone leader republishes the `RegionMap`. Q resyncs sessions from their records (below) and sends `ClockReset` (§5.1). For 500 ms P forwards buffered and late inputs and effects, which Q de-duplicates by input sequence and by `EffectInbox` (§6.2). P then sends `RegionReleased{ρ, g}` to the replicant and neighbours and keeps its frozen copy for 10 s | ≤ 50 ms |
| 7. Fence | Q | `Fence.MigrateRegion(ρ, g+1, Q, manifest)` (below). Until it returns, Q holds ledger calls (shown as "pending", as in degraded mode) and checkpoints. It then writes `(e+1, 0)` records for every AG it gained and a region checkpoint at g+1, and re-issues pending service calls under their idempotency keys | ≤ 1 s; not in the hitch |

The **hitch**, from P's last tick to Q's first, is steps 3–6: **budget ≤ 350 ms p99, bound ≤ 1 s p99**
(NS-3.11), typically ≈ 150 ms.

**The residual** (≈ 6 MB for the reference zone; step 1 already moved everything else):

| Part | Content | Size |
|---|---|---|
| Header | P's build, schema hashes and `sim_abi` (below) | < 1 KB |
| Last tick | tick T's chunks and `TickFlush{T, state_hash}` | ≤ 0.3 MB |
| Simulation state outside the stream | Jolt `SaveState` of ρ's awake bodies and constraints per grid, only when `sim_abi.physics` matches (sleeping bodies restore from replicated transforms), with the grid's body table `(BodyID, stable key)`: Jolt restores state into bodies by ID, so Q creates every body of ρ with P's `BodyID` (`CreateBodyWithID`) before `RestoreState` (02 §7.1); RNG stream positions; zone timers; async job requests in flight (nav, AI planning), which Q re-issues | ≤ 2 MB |
| Region control | AG table `(ag, root, epoch)`; handle-block table with free lists and quarantine (§3.5); `LeaderState` if P leads; per-pair effect sequences and outboxes (§6.2); pending service calls `{idem key, request, entity}`; co-location leases | ≤ 0.5 MB |
| Session records | one per local session and remote viewer of ρ (below) | ≈ 3.5 MB |

**Session records and the resync burst.** Per-entity resync records (§6.6) for 500 sessions × ≈ 2,000
relevant entities would be ≈ 1M records. Instead, each session record holds:
- the relevant set, as a bitmap over ρ's live handle index (≈ 2.5 KB at 20k entities);
- a sparse `(handle, pending_mask)` list for entities whose pending mask is non-zero, where pending = lost ∪
  in flight ∪ changed since last send (§6.6's `resync_mask`): ≈ 600 entries × 7 B for a hub client;
- the controlled AG, the last input sequence, the time-sync offset, and budget and grant state.

That is ≈ 7 KB per session. Acked motion baselines do not travel, so Q's first motion send per entity uses the
absolute size class (≈ 25–35 B). Q seeds priority accumulators from §4.4's formula and writes pending fields
in priority order, never a create or full state, within each session's budget (single-cell) or grant (v1).
The burst is bounded by those budgets:
- a hub client's pending set is ≈ 150–300 entities, ≈ 9 KB. The near tier (≈ 30 entities, ≈ 1 KB) goes out in
  the first tick after resume, and the whole set within ≈ 0.4 s at 256 kbit/s;
- downstream never exceeds §2.5's budget, and stage 7 stays near its steady cost, because bytes bound the
  writes per tick;
- gateways drop P's late ack and loss notifications, which the pending masks already cover (as in §6.6).

**Third-party formats across N↔N+1 (`sim_abi`).** Append-only `.hschema` rules govern everything schemac
generates, but not formats owned by third-party libraries. Every server build therefore computes a
**`sim_abi`**: one u32 per third-party format that can cross a process in a migration residual, a handoff blob
or the replicant stream, plus an xxh3 over all of them.

| Component | Covers | When P's and Q's differ |
|---|---|---|
| `physics` | Jolt version, its precision and determinism defines, and the `StateRecorder` layout | **Portable physics path:** P omits every `SaveState` blob. Q creates every body, awake ones included, from the stream's quantized transforms and linear and angular velocities, rebuilds constraints from their schema components (tractor links, vehicle and dock constraints; only solver warm-start state is lost) and runs §6.4's consistency pass (steps 3–4: depenetration, velocity clamp, sleep) before T+1. Step 4's hash then covers quantized state only, which both builds compute identically under the schema rules, so no blob is ever read by a build that cannot parse it. It adds ≤ 50 ms of hitch at 20k bodies |
| `script` | Luau's bytecode version range (`LBC_VERSION_MIN..MAX`) and the `det-math` patch revision (§10.2) | If Q cannot load the bytecode of P's content pin, it refuses at step 0 (`MigrateNack{sim_abi}`), and the build must ship as an epoch change (05 §1.14.1). A patch revision change is otherwise harmless, because no coroutine crosses a process |
| `nav` | Recast/Detour tile and node-pool formats (`DT_NAVMESH_VERSION`, `DT_NAVMESH_STATE_VERSION`) | Tiles built at run time on moving grids (02 §7.3) are rebuilt by Q from their inputs instead of copied; nav queries in flight are re-issued anyway |
| `anim` | ozz archive versions of runtime-built data | Hitbox pose caches are rebuilt from replicated movement and montage state |

- The residual header, every `HandoffOffer` and the replicant stream header carry `sim_abi`, and handoffs
  between N and N+1 cells during a rolling restart apply the same per-component rule per AG.
- `RegisterBuild` (05 §1.14.1) records each server build's `sim_abi`. CI refuses to promote a server build
  whose `sim_abi` differs from the live build's until NS-3.11(e), which drives every differing component down
  its fallback path, is green for that build pair.
- A Jolt change also changes client hull prediction (§5.3), so it enters the compat fingerprint and arrives
  with an epoch flip (05 §1.14.1). The portable path is what makes that flip's step 4 roll the world from E to
  E+1 without reading an unreadable blob.

**Safety.**
- **One simulator.** Before the commit only P holds the lease; after it only Q. P never resumes after the
  freeze unless the migration aborts before the commit, and it stops for good on seeing g+1 (05 §1.4.2's
  holder rule). Gateways, neighbours and the replicant drop P's messages for ρ below g+1. P writes ticks ≤ T
  and Q ticks ≥ T+1, so no `(handle, tick)` is written twice (§6.6).
- **The fence step cannot fail a committed migration.** `Fence.MigrateRegion` is `AdvanceOwnedBy` with cause
  `migrate`, restricted to rows owned by P and countersigned by P's manifest `{count, xxh3 over sorted
  (ag, epoch)}` of ρ's active AGs at T. It moves every active row of ρ owned by P below g+1 to
  `(e+1, Q, g+1)`.
  - The quiesce makes a mismatch impossible except for a takeover load racing the freeze (§6.1). A row taken
    over is absent from the result, and Q drops that AG as on `released`. A row returned but missing from
    the manifest raises `MigrationManifestMismatch` (SEV2).
  - Its `fence.advanced` events carry `cause = migrate` and `prev_owner = P`, so P's last in-flight checkpoint
    batch is benign `stale_inflight` (§6.1). No `released` is sent: P started the move, as in a handoff.
- **Why Q may simulate before the fence commits.** The region lease already fences transients, trunk
  messages and routes. The fence guards only value and checkpoints, and Q holds both until the reply. P's
  in-flight ledger calls at epoch e either commit before the advance or fail with `FENCE_STALE`, and Q
  re-issues them under the same keys (§6.1). A relog in the window routes to the region's holder, Q.
- **Abort before the commit** (Q NACKs, no ACK within 1 s, a hash mismatch, or the orchestrator refuses or
  turns degraded). P resumes ρ at T+1 from its buffered inputs. No fence row or generation changed, so nothing
  is undone, and the hitch counts against the same bound. Retries back off 5, 10 and 20 s, then raise
  `MigrationStuck` (SEV3). Once Q has ACKed, P resumes only on an abort recorded in `region_migration`. If the
  orchestrator cannot answer (a control-plane fault: a leader takeover takes ≤ 10 s, a PG failover < 30 s),
  **only ρ** stays frozen until it can. Liveness is traded for a single simulator in that one region, while the
  rest of a multi-cell zone resumes when the migration hold expires (Clock, below), as 05 principle 8
  requires. NS-3.11 injects the case.
- **Q dies after the commit.** ρ recovers like any crash (§6.4) at g+2. While P holds its frozen copy (10 s),
  it serves as that recovery's replicant: an AG restores from the copy if its returned epoch is P's snapshot
  epoch + 2 (the migrate advance committed) or + 1 (it did not). State lost ≤ the time since the commit.
- **Replay.** Q's recording starts at T+1 with the post-residual world as a `migrate` keyframe (§10.2), and its
  header records `MigratedFrom{P, T, state_hash}`, so P's log to T and Q's from T+1 chain; NS-3.8 checks the
  chain.

**Clock.** In a single-cell zone Q re-anchors its schedule at resume (`anchor_tick = T+1`): game time pauses for
the hitch like a TiDi dip, so no timer or cooldown advances. A multi-cell zone keeps §3.5's lockstep through a
bounded, production **migration hold**:
- **`MigrationHold{ρ, T, deadline}`**, with `deadline = anchor_wall(T+1) + max_ms` and `max_ms = 1,000`, is a
  `ZoneSchedule` message the leader issues at step 2. It is a production primitive, compiled into shipping
  builds, and distinct from §10.3's debug hold, which exists only in `--dev` processes. A cell accepts it only
  for a region named by a `MigrateBegin` it has received. It grants no exemption from failure detection: held
  cells keep sending trunk keep-alives and heartbeats, and gateways, pre-armed by `MigrateBegin`, treat ρ's
  silence as expected only until the deadline. Every cell runs tick T and then waits.
- **Normal end.** When Q is ready to resume (step 6), or P after an abort, it sends `MigrationReady{ρ, gen}`. The leader
  (Q itself if the commit moved the role) answers `MigrationRelease{ρ, anchor_tick = T+1, anchor_wall}`, and
  every cell, ρ included, resumes in lockstep. The hitch therefore applies zone-wide, and a multi-cell zone
  migrates one region at a time.
- **Expiry.** A cell with no release by the deadline resumes on its own with
  `anchor_tick = T+1, anchor_wall = deadline`. Every cell derives the same anchor from the same message, so the
  rest of the zone stays in lockstep with no further message and no leader, which matters because the leader
  may be P, frozen with ρ. The rest of the zone therefore never holds longer than 1 s. The cells treat ρ as a
  **suspect contributor**: its ghosts freeze (shots resolve against their last pose, §5.6), effects for it
  wait in the outbox (§6.2), handoffs and co-locations into it are deferred, and gateways keep its sessions
  frozen without reporting it suspect (§6.6). `migration_hold_expired` counts each case.
- **Rejoin.** ρ's simulator, Q after the commit or P after a recorded abort, resumes behind the schedule:
  - **≤ 2 s behind:** §3.5's late-cell path. It runs ticks back-to-back, with stages 6–7 on every fourth tick
    only (≈ 30 ms per tick at 20 Hz, so ≈ 1.7 ticks per tick period), and reports `L > 1`. The leader's TiDi
    fast attack takes `d` to ≈ 0.7, so the gap closes at ≈ 1 tick per period: a 2 s gap closes in ≈ 2 s. ρ's
    players see a short fast-forward; everyone else sees a brief TiDi dip.
  - **> 2 s behind** (a longer control-plane fault): it rejoins as a recovered region does (§6.4 step 5), at
    the schedule's current tick. ρ's state stands still for the gap, tick-stamped timers keep zone time, its
    sessions get `ClockReset` (§5.1), and the recording logs `ZoneRejoin{from T+1, to N}` (§10.2).

  Either way, handoffs and co-locations involving ρ wait until it is within one tick of the schedule, and only
  ρ's simulator writes ρ's AGs, so no `(handle, tick)` is written twice.
- **Rejected:** (a) letting Q catch up alone at up to 1.4× speed on *every* migration, which breaks lockstep
  and shows players a fast-forward each time; catch-up is kept for the fault path. (b) Migrating all of a
  multi-cell zone's regions under one hold, which would cut a rollout to one hitch per zone: a control-plane
  fault after its ACK would freeze the whole zone rather than one region, against 05 principle 8.

**Scripts.** A Luau thread cannot be serialized, so no coroutine crosses a process, just as none survives a
crash. P keeps ρ's coroutines suspended through the freeze, so an abort resumes them, and ends them at the
commit. Durable progress lives in `ScriptState` (06 §11 rule 7), which travels in the stream. Q raises `Authority.Adopted{entity, cause = migrate}` for every entity with a `ScriptState`, as after
a handoff or a recovery, so modules re-arm their waits; results of re-issued service calls arrive in that
handler. The script analyzer flags a module that waits more than 5 s of zone time with no `Adopted` handler.

**`Drain(process, reason ∈ rollout | maintenance | rebalance | evacuate, deadline)`.**
1. The orchestrator marks the process `draining` in `svc_orch.process`: no new placements or instances.
2. Idle instances (no session and no active persistent AG) are not moved. They park and are destroyed (§7) and
   are recreated on demand. World zones always move, since their transients and spawners live only in the
   region.
3. The rest migrate in **descending CCU order**, populated before empty, as recovery orders regions
   (05 §1.4.6), with **at most 4 migrations in flight per source process**. Each instance may go to a
   different target (bin-packing, §7).
4. A process that holds no region exits (05 §1.4.2), or restarts on the new build as a standby.

A packed process (64 instances, ≤ 200 players) drains in **≤ 60 s**: 16 rounds of 4, each ≈ 2 s of prepare
with the template cached (§7) and ≤ 150 ms of hitch, with prepares overlapping. Past `deadline`,
`DrainOverdue` (SEV3) is raised, and a host that dies mid-drain recovers its remaining regions as crashes
(§6.4). Degraded mode pauses drains (05 §1.4.4), and a migration not yet committed aborts.

**`PreProvision(zone, time, size)`.**
- At `time − lead` (30 min by default, at least 10 min) the orchestrator reserves a host of the size class
  that `size` names (a fleet battle: 16–32 cores, 16 GB, §3.4) and migrates the zone's regions to it, one at a
  time. The zone's profile and partition do not change, because a running instance's regions never do
  (05 §1.14.1); only the host does.
- The reservation lasts until the timer window closes, plus 2 h. After that the rebalancer may drain the zone
  back.
- If the move is not complete by `time − 5 min`, it is abandoned with `PreProvisionMissed` (SEV2), and the fight
  runs where the zone is, under TiDi and admission control (§8).

**Concurrency and rollout.**
- Per shard, at most 24 regions are between freeze and commit at once, at most 8 migrations target one host,
  and a multi-cell zone migrates one region at a time.
- No planned migration touches a zone at overload stage ≥ 4 (TiDi, §8) unless the reason is `evacuate`.
- A rolling restart draws a surge of 10 % of the cell fleet as build-N+1 processes, never the warm pool's
  failure-domain reserve (05 §1.4.6). At Phase 4's 240 cells that is 24 processes per wave, ≈ 1 min per wave,
  and **≈ 10 min for the whole shard**.
- **Hitches per rollout.** A single-cell zone takes **one** hitch. A multi-cell zone of n cells takes **n**, one
  per region, and each holds the whole zone: **4 in Harrow orbit** and **10 in a Phase 4 10-cell zone**. The
  rollout controller moves at most one region of a zone per wave, so a zone's hitches are ≈ 1 min apart, and
  each stays within the multi-cell row below (≤ 400 ms p99, bound ≤ 1 s). The **cumulative hitch budget per
  rollout** for a player who stays in one zone is ≤ 350 ms (single-cell), ≤ 1.6 s (4 cells) and ≤ 4 s
  (10 cells) at p99. A player who changes zone mid-rollout can add one hitch per zone entered that has not yet
  moved. `migration_zone_hitch_total_ms{zone}` reports the cumulative figure per zone and rollout; NS-3.11(c)
  and BE-A14 hold every hitch to its bound.

**Budgets by profile** (NS-3.11 measures each):

| Profile | Residual | Hitch budget (p99) | Bound (p99) | Hitches per rollout |
|---|---|---|---|---|
| Hub, station or open space (500 players, 20k entities) | ≈ 6 MB | ≤ 350 ms | ≤ 1 s | 1 |
| Fleet battle (2,000 players, 50k entities; `PreProvision`) | ≈ 20 MB | ≤ 600 ms | ≤ 1 s | 1 |
| Small or packed instance (≤ 64 players, 2k entities) | ≤ 0.5 MB | ≤ 150 ms | ≤ 1 s | 1 |
| One cell of an n-cell zone (zone-wide `MigrationHold`) | ≈ 6 MB | ≤ 400 ms | ≤ 1 s, and the rest of the zone never holds > 1 s | n (4 in Harrow orbit, 10 in a Ph4 10-cell zone) |
| Any profile on the portable physics path (`sim_abi` differs) | as above, no Jolt blob | above + 50 ms | ≤ 1 s | as above |

## 7. Zones, instances, layers and phasing

A **Zone** is a template (content, frame, profile); a **ZoneInstance** is a runtime copy owned by one or more
cells. Kinds: *world*, *overflow layer*, *activity* (raid/dungeon/arena; checkpoints and lockouts in the
activity service), *phase instance* (personal/group story), *housing/ship interior* (persistent, per owner),
*edit* (ADR-009; the only kind that persists authored changes).

**Lifecycle:** `CreateInstance{template, content_version, profile, cap, owner/group, idle_ttl}` bin-packs onto
a process with the template cached (**ready ≤ 2 s** for small instances, R03-P0-6); idle 5 min → final
checkpoints and `Fence.Park` of every persistent AG (§6.1), then destroy. Housing and ship interiors park on
their own `idle_ttl` when the last visitor leaves.

**Overflow layers** (WoW layering, GW2 megaserver): a world zone past 80 % of cap opens a layer; placement
prefers party > guild > friends > language > least loaded (R03-P0-8). Layers below 30 % for 10 min merge,
moving players at **safe moments** (out of combat) behind a transition effect; hops ≤ 1 per 60 s; joining a
party pulls you to the leader's layer.

**Phasing:** per-entity `phase_mask` (64 bits per zone, allocated by content) driven by quest state and tested
in the interest filter (§4.3); a phase change yields create/destroy diffs for that client only. Class stories
(SWTOR) use a **phase instance** behind a door. Phasing is used sparingly and shown in editor debug views
(R03 §5).

**Zone transitions** (hyperspace, quantum jump, docking, elevator) are **gateway route changes**, not
reconnects (R07-P0-4): source validates (spool complete) → `TransferPlayer` returns a placement ticket; the
destination pre-creates the arriving AG → source sends the same `HandoffOffer` plus a frame change → gateway
`RouteUpdate`s; client gets `ZoneChange{instance, content version, frame}` on CONTROL and streams destination
containers during transit VFX (prefetch starts at spool). Target ≤ 3 s, 0 reconnects.

**Content-version pinning:** instances pin a content manifest; play instances are never edited live
(R03-P0-3); placement matches the token's content **compat epoch**, never its client build, so a client build
staged at a few percent shares every instance with the live one (05 §1.14.1). Same-epoch releases never
duplicate a zone: a server-only hotfix swaps the server part in running cells at a tick boundary, and new
server binaries roll open-world zones in place through planned region migration (§6.7). Only instanced content (activities, phases,
housing) created after a release starts on it. An epoch change is a coordinated flip (05 §1.14.1). Schemas are
append-only (field IDs, defaults), so N and N+1 cells exchange handoff blobs and migration residuals during
rolling restarts (R07-P1-18); formats owned by third-party libraries (Jolt state, Luau bytecode, Recast
tiles) are versioned by `sim_abi` and fall back to portable paths when they differ (§6.7).

## 8. Overload handling

Inputs: `tick_ms` p95/p99 vs budget, STATE demand/budget, egress, RSS vs cap, `d`. Exiting a stage requires 2×
its entry dwell below threshold.

| # | Stage | Trigger | Action |
|---|---|---|---|
| 1 | **Offload** | tick p95 > 60 % for 10 s | AI planning, pathfinding, missile guidance, stat recompute → helper pools, then **offload worker processes** over trunk with tick deadlines (EVE character nodes) |
| 2 | **Degrade replication** | p95 > 70 % for 5 s or demand/budget > 1.5 | halve far rates; interest 2 Hz; cosmetics ≤ 1 Hz; drop EVENT_U beyond 5 km; far ships command-replicated; fleet proxies (v2) |
| 3 | **Split** | v2: p95 > 70 % for 30 s *and* ≥ 2 clusters with < 20 % cross-interaction | split onto a warm cell |
| 4 | **Time dilation** | tick p99 > 90 % (§3.2) | zone-wide `d` down to 0.1 |
| 5 | **Admission control** | `d` < 0.3 for 60 s or RSS > 85 % | queue arrivals at origin gate ("system congested"); overflow layer for non-contested content; refuse placements; **never kick players inside** |

Scheduled fights (sieges, structure timers) call `PreProvision(zone, time, size)` so the orchestrator moves the
zone onto a reinforced host in advance by planned migration (§6.7: ≥ 10 min ahead, hitch ≤ 1 s, abandoned
with an alert 5 min before the fight; R01-P1-14, R07-P2-25). A zone already at stage ≥ 4 is never moved.

## 9. Security and anti-cheat

- **Authority:** the server owns movement results, hits, damage, cooldowns, loot, inventory and currency (R05-P0-11); nothing of value is client-authoritative.
- **Transport:** netcode AEAD on every post-handshake packet, stateless challenge, token-reuse check, request larger than replies; L0 pre-filter drops wrong-size/type datagrams and limits requests to 10/s per IP before netcode sees them; ≤ 120 pps (240 burst) and 64 kbit/s up per session, plus the separate 40 kbit/s VOICE bucket while transmitting (§2.7).
- **Messages:** schema-declared **token bucket per RPC**; generated bounds-checked decoders (`BitReader` returns errors, never asserts); enum/array ranges from schema; NaN/Inf rejected; 3 malformed messages → disconnect + flag.
- **Semantics:** handle relevance (cannot target what you were never sent), range/LOS, server-side ability SM cooldowns, movement budget (§5.5), fire-rate/damage bounds, idempotent ledger operations.
- **Information hiding** (the only robust anti-ESP): interest management and the `server` audience keep secrets server-side; cloaked entities are not replicated; per-interior PVS culling of enemies in FPS instances (Phase 4).
- **Telemetry:** budget violations, strikes, snaps, aim statistics and economy anomalies → NATS → Trust service (05); tick recordings (§10) for review.
- **Fuzzing:** libFuzzer (clang; MSVC `/fsanitize=fuzzer`) on vendored netcode/reliable read paths, the channel parser and every generated decoder, with recorded-session corpora; 1 h nightly per target; crashes block release.
- **DDoS:** only gateways are public (anycast behind scrubbing, IP rotation); cells and services have no public IPs; pre-auth XDP/eBPF fast path drops bad prefix/size/version and rate-limits per IP (Phase 4).
- **Windows client anti-tamper:** code-signed binaries (ADR-010), Ed25519-signed pak manifests (Monocypher), an `IAntiCheatProvider` seam plus attestation hash in token user data; vendor (EAC/BattlEye class) chosen in Phase 3 (WP-3.11; its Linux support constrains the native Linux client) and integrated in Phase 4, never *depended* on; server-side cheat and bot detection is 05 §1.16a. External security review before Phase 4 launch.

## 10. Testing and tooling

### 10.1 Network simulation

- **NetSim** at L0 (client and server): latency + jitter, Bernoulli and Gilbert–Elliott burst loss, duplication, reordering, bandwidth cap. Profiles `lan`, `good` (40 ms/0.1 %), `mobile` (120 ± 30 ms/2 %), `awful` (250 ms/5 %) via `--netsim=` and the PIE toolbar (R06-ENG-15).

### 10.2 Deterministic replay

**Contract (normative, design rule 8).** A zone replays bit-exactly from any **replay keyframe** (below) plus
the log that follows it; the first keyframe is the instance's initial checkpoint.
Every decision that can change simulation state is therefore either (a) a pure function of logged inputs and
state, or (b) itself a logged **replay event** stamped with the tick, and the position within the tick, where it
took effect. Every asynchronous producer (network, services, I/O, job pools) reaches gameplay only through the
zone's **`SimInbox`**, which assigns each item an apply tick and order at the start of a tick. The recorder logs
at that boundary, so arrival timing never matters.

| Source | Where specified | Rule | Logged as |
|---|---|---|---|
| Luau time slice | §3.1; 02 §7.4; 06 §11 | **Fuel, not wall time.** The `interrupt` callback runs at every VM safepoint (loop back-edges and calls; GC-step invocations with `gc ≥ 0` are not counted) and decrements the resume's fuel. Generated binding glue and the sandbox's builtin wrappers also charge `cost + each × items` per call, from a calibrated per-function table (02 §7.4). **The interrupt never yields** (02 §7.4): Luau cannot yield across a metamethod or C-call boundary, and invisible yields would break handle revalidation. Past `fuel_per_resume` the coroutine is flagged (`ScriptOverBudget`) and an explicit `task.checkpoint()` yields. The lane resumes coroutines in `(wake tick, EntityId, seq)` order until `fuel_per_tick` is spent; the rest resume next tick. `fuel_per_resume` (≈ 2 ms), `fuel_kill` (min(≈ 5 ms, 25 % of the tick)), `fuel_per_tick` (≈ 15 % of the tick) and the binding cost table come from `helios-cell --calibrate-fuel` on the reference server core, are stored in the zone profile of the content build, and are copied into the replay header | nothing (deterministic) |
| Luau runaway | same | A resume that reaches **`fuel_kill`** is killed deterministically, with a sticky error that `pcall` cannot swallow. A **20 ms wall-clock backstop** catches a binding whose real cost far exceeds its calibrated fuel charge, and it is the only wall-time script decision | Fuel kills: nothing (deterministic), plus audit telemetry. Backstop kills: `ScriptKilled{tick, coroutine, fuel_used}`, and replay kills at the same fuel count |
| Service replies (ledger, fence, checkpoint load, character) | 05 §2.4 | Drained FIFO at tick start **by count** (≤ 256 replies or 256 KB per tick), not by a time cap | `Reply{tick, idx, payload}` |
| Trunk traffic: effects, ghost updates, handoff blobs, `VoiceAudience` | §6.2–6.3 | Applied at the owner's next tick in `(src, per-pair seq)` order | `Msg{tick, idx, payload}` |
| Client inputs | §5.5 | The movement budget uses wall time, so rate decisions happen before the sim | inputs actually consumed, per entity per tick |
| Container activation and streaming completion (server) | 02 §2.4, §5.7 | Activation is metered in **work units** (≤ 2,000 entity instantiations per zone tick ≈ 2 ms), never wall time. I/O completion time is inherently nondeterministic | `ContainerActivated{tick, container, content hash}`; the replayer blocks at that tick until the container is resident (replay is not real-time) |
| Async jobs with deadlines: nav queries and tiles, AI planning, offload workers (§8) | 02 §7.3; §8 | Consumed at the tick they enter the inbox; late results roll to the next tick | `JobResult{tick, request, payload or hash}`; payloads that are pure functions of logged state are recomputed synchronously on replay and hash-checked |
| Hot reload | 05 §1.14 | Swaps only at tick boundaries | `ContentSwap{tick, build}` |
| Terrain collision tiles | 02 §5.8a | A tile's content is a pure function of the body seed, graph, stamps and `TileKey`. The stage 3 fence builds any missing contact tile synchronously, so contacts never depend on generation timing. Only a hold past the fence's per-tick cap does | Synchronous builds: nothing. Holds: `CollisionHold{tick, entity}` |
| Wall-time effects, durable-timer firings, calendar resets | 06 §11 rule 4; 05 §1.8 | A wall deadline takes effect at the first tick whose start passes it | `WallDeadline{tick, id}` |
| Job-system merge order, physics, RNG, floating point | 02 §2.4 rule 7; 02 §7.1; ADR-013; 06 §11 | Stable merge by `(system, EntityId)`; `JPH_CROSS_PLATFORM_DETERMINISTIC`, bodies added and query results sorted by EntityId; **Jolt's solver order keyed by stable body keys, never `BodyID`** (the vendored `third_party/jolt/patches/stable-order`: contact sort keys, body-pair order and `CharacterVirtual` contact order), and ShipHull and Vehicle bodies start every `PhysicsSystem::Update` with no cached manifold or warm-start impulse, so a predictor or replay whose `BodyID`s differ computes the same bits (02 §7.1); named seeded streams; `/fp:precise`, `-ffp-contract=off`, `det::` transcendentals | seeds and build IDs in the header; per-grid body tables in keyframes |
| **Luau math** | 02 §7.4 | Stock Luau calls the CRT's libm, and MSVC's UCRT and glibc differ in the last ULP (the UCRT also picks FMA3 variants at run time). A vendored patch, `third_party/luau/patches/det-math`, routes every transcendental to `det::` on cells, world-script hosts and clients alike (clients for PIE parity): `luai_numpow` in `lnumutils.h` (the `^` operator); `lmathlib.cpp` (`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log10`, `pow`, `sinh`, `cosh`, `tanh`); the `luauF_*` fastcalls in `lbuiltins.cpp`; the `vector` library; the libm pointers that native codegen calls through `NativeContext`; and the compiler's constant and builtin folding, so cooked bytecode is identical whichever compiler built the cook. `sqrt`, `floor`, `ceil`, `fmod`, `abs`, `ldexp`, `frexp`, `modf` and `round` stay, being exact or correctly rounded. At start each VM host evaluates 256 golden vectors and refuses to start on any mismatch | nothing |
| **Hitbox sampling** (lag compensation) | 02 §7.2; §5.6 | ozz's `SamplingJob` and `BlendingJob` normalize with `RSqrtEst`/`NormalizeEst` (`rsqrtps`), whose bits differ between Intel and AMD. Cells and clients therefore pose the ≤ 24 hitbox joints with **`det::HitboxSampler`**: ozz key decompression, interpolation with an exact normalize (`sqrt` then a divide) and scalar local-to-model without contraction, ≈ 1.5× ozz's cost, inside 02 §7.2's 1 ms for 200 entities at 60 Hz. ozz's SIMD jobs remain for cosmetic client animation only | nothing |
| **Sorting** | 02 §2.4 rule 7 | MSVC's STL and libstdc++ order equal elements differently. SIM modules sort with `det::sort(range, key)`, whose key must end in a unique ID (EntityId, handle or index); debug builds assert that no two keys are equal. Vendored SIM-path code is audited: Jolt and Luau (`table.sort`) use their own sort implementations, and Recast's `qsort` call sites are patched to `det::sort` with an index tiebreak | nothing |
| **Floating-point environment** | 02 §2.1 | Every simulation thread, on cells and in the client's prediction step (§5.3), runs with the default MXCSR, `0x1F80` (round to nearest, FTZ and DAZ off, exceptions masked). Workers set it at start; the zone asserts it at each tick start, the client at each prediction step, and the job system at each SIM job-batch start (`stmxcsr`, ≈ 1 ns). A mismatch, such as a driver or injected DLL changing it, resets it and raises `FpStateCorrupt{thread, value}`, and the recorder logs the event. On Windows, `_set_FMA3_enable(0)` at start keeps any CRT math left outside `det::` on one code path on every CPU | `FpReset{tick, thread, value}` |
| **Region rejoin** | §3.5, §6.4, §6.7 | A recovered region, or one rejoining > 2 s after an expired migration hold, starts at the schedule's current tick | `ZoneRejoin{from, to}` |
| **Co-location decisions** | §6.2a | The host choice reads measured `ag_tick_us`. Refusal reads the receiver's tick p95, its overload stage, the set caps (5 % of the host's tick) and its leased-in load (10 %). All are load-dependent, and each decides which cell simulates an AG and whether a contact is co-simulated or solved against a puppet. Every cell logs each decision it makes. The replayer applies the logged outcome and never reads a load measurement | `ColocDecision{tick, set_id, pair, outcome ∈ host \| move \| nack \| effect_only \| leash \| release \| overstay, reason, inputs{rows, tick_us, centroid, p95, stage, leased_in}}` |
| **Handoff and transfer decisions** | §6.3, §6.7, §7 | Whether an offer is sent, accepted or NACKed (epoch, overload, decode, a region in a migration's quiesce or a suspect contributor, admission at stage 5), deferred, retried after the 1/2/4 s wall-clock back-off, aborted, self-committed or dropped decides which cell simulates an AG tree from which tick | `HandoffDecision{tick, root, role ∈ offer \| accept \| nack \| commit \| abort \| self_commit \| drop \| defer, peer, epoch, reason}`, on each side |
| **Keyframes and script rebases** | below | A rebase ends every coroutine and swaps the VM; the cut tick depends on script state, the cadence and deferrals (overload stage ≥ 4, a migration) | `Keyframe{tick, cause}` |
| Luau GC and table order | 02 §7.4 | GC timing is unobservable: Luau has no table `__gc`, and on cells the sandbox removes `collectgarbage` and rejects `__mode` (weak tables) (02 §7.4). Iterating tables keyed by tables, userdata or functions is rejected (address-ordered); entity maps use `EntityMap`, which iterates in EntityId order | nothing |
| TiDi, overload stage, interest refresh | §3.2, §8, §4.3 | TiDi and the interest refresh change wall pacing or replication only. The overload stage and measured tick times **do** change sim state, but only through decisions logged in their own rows: offloaded jobs (`JobResult`), co-location refusals and fallbacks (`ColocDecision`), and handoff, transfer and admission NACKs (`HandoffDecision`) | `d` and stage per tick, for review |

**Lint (`simdet`, every PR).** A clang-tidy plugin plus rules on modules tagged `SIM` (gameplay, authority
apply paths, server netgame, `engine/pcg`) forbids `std::chrono::*::now`, `QueryPerformanceCounter`,
`clock_gettime`, `rand`, `std::random_device`, thread IDs, pointer-ordered containers and unsorted
`std::unordered_*` iteration. It also forbids, in `SIM` modules:
- libm transcendentals (`std::sin`, `sinf`, `pow`, `exp` and the rest); only `det::` may be called, as HXL
  already requires (06 §1.2);
- reciprocal and reciprocal-square-root estimates: `_mm_rsqrt_ps/ss`, `_mm_rcp_ps/ss`, their `_mm256_` and
  `_mm512_*14` forms, and ozz's `RSqrtEst`, `RcpEst`, `NormalizeEst`, `NLerpEst` and `*EstNR`;
- `std::sort`, `std::partial_sort`, `std::nth_element`, their `std::ranges` forms, `qsort` and
  `std::priority_queue`, unless the comparator is `det::total_order` over a key with a unique tiebreak
  (`std::stable_sort` stays allowed: its output is fully specified by its input order);
- `long double` (80-bit x87 on GCC, 64-bit on MSVC) and any write to MXCSR outside `det::FpEnv`.

The lint has a self-test with one planted violation per rule. Wall time is only available as a `WallTime` type
from `ZoneClock::wall_now()`, and it cannot convert into sim types. A Luau analyzer rule applies the
table-order and removed-API checks to scripts. CI also runs the script corpus through the interpreter and native codegen and asserts **identical fuel
counts**. Stock Luau 0.739 fails that: its code generator emits the numeric-`for` interrupt at the top of the
loop body instead of in `FORNLOOP`, so every numeric `for` left by `break` or `return` costs one extra fuel in
native code (WP-0.10 pins the case in a test). The vendored patch **`third_party/luau/patches/codegen-fornloop-fuel`**
emits it in `FORNLOOP`, as the interpreter does (`CodeGen/src/IrTranslation.cpp`). Until that patch lands,
cells and world-script hosts run the interpreter only, and `VmConfig` refuses native codegen on them (today it
only warns). The `interrupt` hook's overhead must stay ≤ 10 % of script time (RT-13). WP-0.10 measured
12–17 % with the callback, so the second vendored patch, **`third_party/luau/patches/fuel-counter`** (an
inline counter decremented at each `gc < 0` safepoint, which calls the host only when it reaches zero), is
required rather than a fallback. Both patches, and `det-math`, are in `sim_abi.script` (§6.7); 09 names their
owning WP, and RT-13 plus the interpreter-versus-codegen parity test are its acceptance.

**Recording.** The header holds the content build, schema hashes, binary build ID, compiler, CPU model,
`sim_abi` (§6.7), zone profile, fuel constants and the binding fuel-cost table (02 §7.4), RNG seeds and the
keyframe the segment starts from. Each tick then logs consumed inputs, inbox items in apply order, replay events and a 64-bit **state hash** (xxh3 over quantized replicated plus server-audience
state; per-system hashes in debug builds). The log is zstd-compressed: ~2 MB/min at 50 players, ~15 MB/min
at 500. The ring on local NVMe keeps the log back to the newest keyframe that is at least 30 min old, so at
least 30 min is always replayable on its own (30–60 min at the default cadence, ≤ 1 GB at 500 players). A
crash, a report or a killmail flushes it to object storage from the newest keyframe at least 5 min before
the event.

**Replay keyframes.** A keyframe is a tick-boundary snapshot holding everything a bit-exact replay needs, so a
segment of log can start at it. The recorder cuts one:
- at instance start, and at a region's first tick under a new simulator: Q's T+1 after a planned migration
  (§6.7, whose header also carries `MigratedFrom`) and S's first tick after a recovery or a `ZoneRejoin` (§6.4);
- every 30 min of zone time (`replay.keyframeMinutes` in the zone profile, 10–120) through a **script rebase**
  (below), which is what makes a zone that has run for days replayable from a recent point.

| Part | Content |
|---|---|
| Header | The recording header above, plus `{tick K, cause ∈ start \| migrate \| recover \| rebase, xxh3 of the previous keyframe}` |
| ECS | Every entity's components in every audience, server-only included, at full precision in the tagged format (02 §3.7), in EntityId order; the `EntityRegistry` (EntityId ↔ NetHandle); handle blocks with free lists and quarantine (§3.5) |
| Authority | AG table `(ag, root, epoch)`; `EffectInbox`es, and outboxes with their per-pair sequences (§6.2); co-location sets and leases (§6.2a); ghost records with their `HitPose` history, and the cell's own lag-compensation history (§5.6) |
| Physics | Per grid: the bubble box and origin (02 §5.4); a **body table** `(BodyID, stable key, layer, motion type, shape AssetId or TileKey)` in `BodyID` order and the constraint table in `mConstraintIndex` order; `PhysicsSystem::SaveState(EStateRecorderState::All)`, contacts included (manifold and body-pair caches). Jolt restores state into existing bodies by ID, so the replayer re-creates each body with `BodyInterface::CreateBodyWithID` and each constraint in index order, then calls `RestoreState` (02 §7.1) |
| Terrain | The collision-tile bodies instanced per grid, by `TileKey` (their content is a pure function, 02 §5.8a), and the bodies held at K (`CollisionHold`). Tile-cache residency is not state: a fence build never changes a result |
| Nav | Runtime-built tiles on moving grids, by input hash; the replayer rebuilds them from their inputs, as §6.7's `nav` rule does |
| RNG and clock | Every named stream's `(seed, position)`; the `ZoneClock` anchor, schedule and `d`; zone-time timers; durable-timer mirrors and pending `WallDeadline`s |
| Inbox | `SimInbox` items already assigned an apply tick after K (count-capped replies, late job results) and in-flight requests (service calls with their idempotency keys, nav and AI jobs), whose results the log carries |
| Scripts | **No Luau state.** The VM is fresh at every keyframe: at start, after a migration or recovery (§6.7 Scripts) and after a rebase. `ScriptState` components and script timers are ECS data above; the lane's per-module kill history and disabled set (02 §7.4) are C++ state and are included |

A keyframe is ≈ 10 MB (zstd) for the reference 500-player, 20k-entity zone and ≈ 40 MB at 50k entities. The
cut copies ECS columns and Jolt state at the end of tick K, ≤ 3 ms on that one tick (in stage 8), and
serializes the copy on the Background pool.

**Script rebase.** A Luau thread cannot be serialized (§6.7), so a keyframe can be cut only where the VM holds
nothing the snapshot would miss. A rebase creates that point, with the same semantics as a planned migration's
Scripts rule:
1. At the due tick the lane waits for the first tick at which **no coroutine of a module without an
   `Authority.Adopted` handler is suspended**. Such modules wait ≤ 5 s by 06 §11's analyzer rule, so this
   normally arrives within seconds. After 60 s of zone time without one, the keyframe is skipped for one
   interval (`replay_keyframe_deferred`) and the ring keeps the older one. No rebase starts in a migration's
   quiesce, freeze or catch-up, or at overload stage ≥ 4.
2. At the end of that tick K the cell ends every suspended coroutine, swaps in a fresh VM for the instance's
   content pin (a background job has already created it and loaded the bytecode, as Q does at §6.7 step 0)
   and cuts the keyframe. The old VM is closed on the Background pool.
3. From K+1 the lane runs the zone's module chunks in manifest order, as at instance start, and then raises
   `Authority.Adopted{entity, cause = rebase}` for every entity with a `ScriptState`, in EntityId order, under
   the normal fuel budget. Modules re-arm their waits from `ScriptState`, and replies to service calls whose
   coroutine ended arrive in that handler, as after a migration.
4. `Keyframe{K, rebase}` is logged and a new segment starts. A replay started at that keyframe builds the same
   fresh VM and raises the same events, and a replay passing through K performs the same rebase, so the live
   cell and every replay agree bit for bit.

A module that is correct under handoff, migration and recovery is therefore correct under a rebase. Players
see nothing: there is no hitch, no client message and no value movement. Rejected: (a) keyframes only at
natural quiescence (no coroutine suspended anywhere), which a populated zone never reaches; (b) serializing
Luau stacks with an Eris-style persister, a large patch against Luau's internals and native frames on every
update; (c) replay only from instance start, a migration or a recovery, which leaves a world zone that has run
for days with no recent replayable window.

**Replay.** `helios-cell --replay <log> [--from keyframe] [--until tick] [--dap]` runs as fast as possible from
the named keyframe (default: the segment's first). It injects inbox items, kills, activations and logged
decisions (`ColocDecision`, `HandoffDecision`, `Keyframe`) at their logged ticks, never re-deciding them from
load, and compares per-tick hashes. At the first divergence it bisects with per-system hashes and dumps an ECS
diff. Logs stay per cell, so a multi-cell zone replays one cell at a time, with trunk traffic from its
neighbours injected from the log and migrations chained by `MigratedFrom`.
- **Phase 2:** same platform and binary, with budgets binding, from instance start and from a rebase keyframe
  (NS-2.4).
- **Phase 3:** cross-compiler (GCC-record/MSVC-replay and the reverse) and **cross-vendor** (AMD-record/
  Intel-replay and the reverse), and multi-cell zones through handoffs, co-location, migration holds and
  rejoins, NS-3.8. This needs identical fuel counts, which hold because bytecode and
  safepoints are compiler-independent (and, once `codegen-fornloop-fuel` is in, identical in native code), and it is why Luau math, hitbox
  sampling, sorting, MXCSR and Jolt's solver order are covered above: Linux production logs replay on Windows
  dev boxes with either CPU vendor.

It feeds crash repro, anti-cheat review and server-built killcams (R05-P1-19), and the DAP adapter attaches to
a replay at any tick (07).

### 10.3 Bots, metrics and inspection

- **Bots:** `helios-bot` is a headless thin client sharing net/prediction/decode code (collision/nav data only); Luau behaviours (login, fight, fly, dock, zone-hop, trade, boundary ping-pong, relog storm, **boundary observer**: holds station 100 m from a cell boundary and logs, per watched EntityId, creates, destroys, update ticks and source cell; **seam fighter**: fights across a boundary with hitscan, fast projectiles or ship guns and logs its client-side reference hit, hit zone and idempotency id for every shot; **tug**: tows a wreck by tractor along a scripted route through one or more cell boundaries, or rams and grapples on a seam, to drive co-location (§6.2a); **field auditor**: after a rejoin or migration, compares every replicated field it holds with the cell's authoritative quantized state from a dev-build debug query); 500–2,000 bots per process; `helios-swarm` (Go) schedules pods. **16 bots per commit** (ADR-012), **1k-bot 1 h soak nightly**, **10k bots weekly** on staging with chaos: cell kills, gateway-instance and whole-box kills, loss, handoff torture, **migration torture** (repeated `Drain`s and N↔N+1 migrations under load, each checked by state hash and transient census) (R05-P1-20, R06-ENG-27). Test builds add a **truth tap**: each cell streams per-tick transforms to the harness over a separate socket, so a restore can be compared with what the killed cell really held.
- **Metrics:** in-house `engine/telemetry` (Prometheus exposition + sampled OTLP/HTTP-JSON spans, no protobuf in C++): `cell_tick_ms{zone,stage}`, `cell_dilation`, `repl_bytes{class,component}`, `conn_budget_bps`, `conn_loss`, `handoff_latency_ms`, `fence_conflicts`, `fence_tree_mismatch`, `fence_bulk_ms`, `ckpt_rejected{reason}`, `zone_tick_skew_ms`, `leader_gen`, `replicant_lag_ms`, `fence_parked_total`, `fence_released_sent`, `fence_active_orphans`,
`view_contributors{zone}`, `repl_remote_viewers{level}`, `gw_grant_bytes{role}`, `gw_pack_delay_ms`,
`gw_free_slot_ratio`, `session_reconnect_rps`, `trunk_pps{link}`, `gw_trunk_conns`, `gw_trunk_state_bytes`, `gw_affinity_hit_ratio`, `gw_dead_trunk_writes`, `migration_hitch_ms{profile}`, `migration_zone_hitch_total_ms{zone}`, `migration_hold_expired`, `migration_portable_physics`, `migration_residual_bytes`, `migration_aborts{reason}`, `coloc_set_size`, `coloc_refused{reason}`, `coloc_late`, `coloc_leased_in{cell}`, `coloc_overstay`, `coloc_leash_break`, `ghost_audit_missing`, `fp_state_resets`, `effect_outbox_depth`, `effect_outbox_dropped`, `effect_resent_total`, `lagcomp_ghost_gap`, `restore_depenetrations`, `script_fuel_used`, `replay_divergence`, `replay_keyframe_ms`, `replay_keyframe_deferred`, `voice_m2e_ms`, `voice_streams`, `gw_drops{reason}`, `overload_stage`; traces login → token → gateway → cell, handoff phases and migration steps (§6.7).
- **Packet inspector** (editor profiling tool): per-connection bandwidth by channel/class/component (ImPlot), priority tables, relevancy set in the viewport, schema-decoded message log, `.hnetcap` captures (decrypted at L1, dev builds only); schema-generated Wireshark dissector in Phase 3.
- **Audits:** a nightly **custody/fence audit** checks every custody item's `custody_ag` row:
  - *active:* it names a live cell that holds the region lease at `owner_lease_gen` and lists the AG in its
    AG-table census;
  - *dormant:* its owner fields are NULL, no cell lists it, and its newest checkpoint is the one at
    `parked_seq`, at epoch = row epoch − 1. Its items are at rest (§6.1);
  - *every tree:* all rows share one epoch, owner and state;
  - *leaks:* no active row has gone > 60 s without a simulating owner outside a running recovery, and no active
    character row has gone > 6 min without a session (linkdead expiry is 5 min). Such a row is a missed park
    (`fence_active_orphans`).

  A **handle audit** bot records `(handle, gen) → EntityId` on every connection and fails on any conflict
  (§3.5). In test builds it also fails on any `(handle, tick)` written by two cells (§6.6).

  A **ghost audit** (test builds, from the truth tap) checks every tick that each cell whose regions lie within
  an AG's ghost margin of its root (inside them, or within M or `M_hit`, §6.2) holds a ghost of it updated
  within two ghost periods, and that each viewer within an entity's registration distance is registered at its
  owner (§6.6). Misses count in `ghost_audit_missing`. It covers co-located AGs deep inside another region
  (§6.2a).
- **Dev-only multi-cell PIE hooks (07 §1.6.1, Phase 3).** Accepted only by processes started with `--dev` and
  compiled out of shipping builds (ADR-016): `DevForceHandoff{root, toCell}` starts §6.3's protocol immediately,
  bypassing only the H-past-boundary trigger; a zone-leader **debug hold** (a `ZoneSchedule` hold from `now + 2`)
  stops every cell when one stops at a breakpoint; a cell that reports `DebugPaused` is exempt from gateway
  suspect detection and orchestrator failure detection until it resumes, and handoff timers restart on resume;
  NetSim also applies to cell ↔ cell trunks. Replay logs stay per cell (§10.2), so a multi-cell repro replays
  one cell at a time. The debug hold is unbounded and exempts cells from failure detection, so production
  never uses it; planned migrations use the separate, bounded `MigrationHold` (§6.7).

## 11. Layout, interfaces, ladder, acceptance, risks, traceability

### 11.1 Module and directory layout

```
engine/net/          helios::net (HEADLESS): sockets (win32/posix), trunk IO pool (epoll/recvmmsg, RIO), netcode + reliable wrappers, channels, netsim
engine/replication/  descriptor runtime, shadow state, interest hash, prioritizer, ghost records, chunks
engine/netgame/      time sync, prediction/reconciliation, interpolation, lag-comp history, move validation, command nav
engine/authority/    AGs, epochs, GhostRef, effects, handoff FSM, cell mesh, fence client
engine/telemetry/    metrics, spans, cheat events
apps/cellserver/     helios-cell (ZoneHost, profiles, --replay, --embedded-gateway, --replicant, --role world-script)
apps/gateway/        helios-gateway (--embedded-voice)
apps/voice/          helios-voice forwarder (engine/net only; no ECS)
engine/replay/       SimInbox, recorder, keyframes and script rebase, replayer, state hashing
tools/bots/  tools/devcluster/  tools/netinspect/  tools/fuzz/  tools/lint/simdet/
third_party/luau/patches/det-math   Luau transcendentals, ^ and constant folding → det:: (§10.2)
third_party/luau/patches/codegen-fornloop-fuel, fuel-counter   native-code fuel parity; inline fuel counter (§10.2)
third_party/jolt/patches/stable-order   solver order by stable body keys; no cross-Update cache on predicted bodies (02 §7.1)
schemas/net/         *.hschema wire messages (token format shared with Go via schemac)
tests/net/ tests/replication/ tests/authority/   doctest suites, Windows↔Linux and Go-token interop
```

### 11.2 Key interfaces (sketch)

```cpp
namespace helios::net {
struct PacketNotify { uint64_t seq; bool delivered; };
class Endpoint { public:             // wraps netcode client/server + reliable endpoints
  Result<SessionIndex> connect(const ConnectToken&);
  SendResult send(SessionIndex, Channel, std::span<const std::byte>);   // may be WouldBlock
  size_t     state_budget_bytes(SessionIndex) const;
  uint64_t   send_state(SessionIndex, std::span<const std::byte> chunk); // returns packet seq
  void       poll(TimePoint now, IEndpointHandler&);                    // decrypt, ack, deliver, notify
};
}
namespace helios::replication {
class ReplicationSystem {
public:
  void  gather(ecs::World&, Tick);                                 // pushed dirty bits → shadow → chunks
  void  update_interest(ConnectionState&, const InterestIndex&);  // staggered 4 Hz
  Chunk write(ConnectionState&, size_t budget_bytes);             // priority accumulator
  void  on_notify(ConnectionState&, std::span<const PacketNotify>);
  // v1 view composition (§6.6): one ConnectionState per (session, cell), local or remote
  void  on_remote_viewer(SessionId, const RemoteViewer&);         // viewpoint, scope, relations, level mask
  void  on_viewer_leave(SessionId);                               // reliable destroys, then free
  Demand demand(const ConnectionState&) const;                    // mandatory, want, Σ top-K acc
  void  on_grant(SessionId, uint32_t bytes);                      // applies to the next tick
  void  on_rebind(SessionId, SessionEpoch, RebindMode, std::span<const NetHandle> held); // §6.6: loss (held set) or relocate (keeps baselines, §2.4)
  SessionRecord export_session(const ConnectionState&) const;     // §6.7 residual: bitmap + pending
  ResyncRecord export_record(const ConnectionState&, NetHandle);  // HandoffOffer registry entry
  void  adopt_record(SessionId, NetHandle, const ResyncRecord&);  // at HandoffCommit
};
}
namespace helios::gateway {
class ViewComposer {                    // per zone instance, per gateway
public:
  void on_viewer_state(SessionId, const ViewerState&);            // from the home cell, 4 Hz
  void on_region_map(const RegionMap&);                           // from the zone leader
  void on_presence(CellId, const ClassPresence&, const PartyPresence&); // per occupied region (§6.6)
  void on_demand(CellId, std::span<const Demand>);                // with tick n's chunks
  void grant(Tick next, std::span<Grant> out);                    // weighted water-filling
  void pack(SessionId, Tick);                                     // on all TickFlush or +2 ms
};
}
namespace helios::authority {
class Authority {
public:
  template<class C> Mut<C> mutate(ecs::Entity);                   // asserts ownership
  void send_effect(AgId target, const EffectMsg&);                // ordered, idempotent
  HandoffTicket begin_handoff(AgId, CellId to);
};
class Colocation {                      // §6.2a; one per zone instance on each cell
public:
  void couple(AgId mine, GhostRef other, CouplingKind);           // predicate true: request or join a set
  ColocDecision on_request(const ColocRequest&);                   // host rule, caps → accept or ColocNack; logged (§10.2)
  void tick(Tick);                        // renew leases, leash check, set handoff, release at expiry
  bool effect_only(AgId a, AgId b) const; // refused pair: puppet contact, TractorForce, retries
};
struct Owner { CellId cell; RegionId region; LeaseGen lease_gen; };
struct IFence {                                                   // all async; never awaited inside a tick
  virtual Future<FenceResult> advance(AgId root, Epoch expect, Epoch next, Owner,
                                      AdvanceCause) = 0;          // whole tree; load may take over
  virtual Future<FenceResult> park(AgId root, Epoch e, Owner, StreamSeq final_ckpt) = 0;  // → dormant
  virtual Future<FenceResult> join(AgId member, Epoch em, AgId root, Epoch er, Owner) = 0; // → max+1
  virtual Future<FenceResult> leave(AgId member, Epoch e, Owner) = 0;                     // → e+1
  virtual Future<FenceResult> advance_many(std::span<const AgEpoch>, Owner) = 0;          // bundles
  virtual Future<BulkFenceResult> advance_owned_by(RegionId, LeaseGen, Owner) = 0;        // active rows only
  virtual Future<BulkFenceResult> migrate_region(RegionId, LeaseGen next, Owner to,
                                                 const AgManifest& from_source) = 0;     // cause migrate (§6.7)
};
class RegionMigrator {                  // §6.7; source and target sides of one planned migration
public:
  void          begin_precopy(RegionId, CellId target);             // replicant-format stream + TickFlush
  Residual      freeze(RegionId, Tick T);                           // end of tick T; buffers inputs/effects
  MigrateResult adopt(const Residual&);                             // target: apply, hash check, ACK/NACK
  void          resume(RegionId, LeaseGen next);                    // target: T+1, RouteUpdate, ClockReset
  static PhysicsPath physics_path(const SimAbi& p, const SimAbi& q);  // blob or portable (§6.7)
};
class ZoneClock { public: Tick tick() const; float dilation() const; Duration wall_interval() const;
                  WallTime wall_now() const;                      // not convertible to sim types
                  void on_tick_measured(Duration cpu);            // TiDi controller
                  void apply_schedule(const ZoneSchedule&);       // from the zone leader (§3.5)
                  void apply_hold(const MigrationHold&);          // ≤ max_ms; self-release at deadline (§6.7)
                  void rejoin(Tick current);  };                  // recovery or late rejoin (§3.5, §6.4)
}
namespace helios::replay {
class SimInbox {                        // the only path from async producers into the simulation
public:
  void post(InboxItem&&);                                         // any thread
  std::span<const InboxItem> drain(Tick, const DrainLimits&);     // count/bytes caps; recorded
};
class Recorder { public: void event(Tick, const ReplayEvent&); void state_hash(Tick, uint64_t);
                 void keyframe(Tick, KeyframeCause);  };          // start | migrate | recover | rebase (§10.2)
}
```

### 11.3 MVP → AAA feature ladder

| Area | Phase 0 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 |
|---|---|---|---|---|---|---|
| Transport | netcode+reliable, channels, fuzzers, trunk throughput gate | Internet play, reconnect tickets | AIMD, pacing, trunk sharding, trunk IO pool with per-trunk buffer sizing (§2.6) | Gateway instances, key rotation, VOICE channel | XDP, DDoS drill, security review | QUIC/WebTransport (R07-P2-24) |
| Gateway | Forwarder | Tokens, routing, service RPC | Multi-gateway, limits, 256 slots/instance gate | View composition: viewer registration by occupied-region presence, budget arbitration, tick-aligned packing (§6.6) | 8k sessions, zone-affinity token assignment with relocation drift repair and the scattered-mesh trunk budget (§2.6), N+1 box headroom, zone-loss sizing (12 boxes over 3 zones, 05 §6.7), reconnect-storm path, far-broadcast fallback, AC hook | Replication layer (on the replicant tier) |
| Voice | — | — | — | `helios-voice`; party, fleet, ship, proximity; moderation v1 | Automated-moderation decision | Web companion (SFU option) |
| Cell | Skeleton, ZoneClock | Job graph, Luau fuel, 50 players | Multi-instance, 500/zone, TiDi | Multi-cell, zone leader, planned region migration with `Drain` and `PreProvision`, bounded `MigrationHold`, `sim_abi` portable residuals (§6.7) | Offload workers, `--replicant` mode | Split/merge |
| Replication | Descriptors, full state | Interest, masks, priority | Budgets, frame quant, LOD, deltas | Phase/layer scope | Fleet aggregation, PVS, replicant stream | Gateway-side |
| Netcode | — | Prediction, interpolation, basic hitscan lag comp, ability prediction keys (06 §1.4) | Full lag comp, projectiles, predicted abilities at 500/zone | Command replication; lag compensation across cell boundaries (ghost hit history, `M_hit` cook check, §5.6) | 60 Hz tuning | Seamless planet↔space |
| Authority | AG/epoch/fence API | Transitions via handoff; AG trees, `Join/Leave`; `Park`, dormant rows, `released` | Warm standby, `AdvanceOwnedBy` (active rows), fence cache | v1 handoff with resync records, co-location protocol (sets, caps, leash, effect-only fallback; §6.2a), bundles; exactly-once effects with outboxes (§6.2); `Fence.MigrateRegion` | Replicant recovery for every persistent world zone, ≤ 1 s state loss, tick-consistent restore (v1.5) | v2 < 50 ms |
| Instancing | — | — | Instance lifecycle, overflow layers | Activities (05 §1.12), phases, housing, pinning | Placement tuning | Cross-shard |
| Overload | Metrics | Tick budgets in CI | Degrade + TiDi | Admission, pre-provision | Offload | Split |
| Tooling | NetSim, unit tests, `simdet` lint | 16 bots, inspector, recorder, relog storms; `simdet` libm, estimate, sort and MXCSR rules; Luau `det-math`, `fuel-counter` and `codegen-fornloop-fuel` patches; Jolt `stable-order` patch (§10.2) | 1k nightly, same-platform replay, replay keyframes with script rebase, custody audit (active and dormant) | 10k, handoff and migration torture, cross-compiler and cross-vendor replay, multi-cell replay with `ColocDecision` and `HandoffDecision` events, handle and ghost audits, boundary observer, seam fighter, tug | Chaos drills, gateway-box kill, field auditor, truth tap | 100k simulation |

### 11.4 Acceptance criteria (CI or bot swarm)

IDs are stable: 09 cites them as `NS-p.k`, and new clauses are only ever appended.

| ID | Criterion |
|---|---|
| **NS-0.1** | Handshake in 1.5 RTT |
| **NS-0.2** | Loopback 100k pps/core without loss |
| **NS-0.3** | Windows client ↔ Linux gateway interop |
| **NS-0.4** | Fuzzers 1 h clean |
| **NS-0.5** | Go-minted tokens accepted by vendored netcode |
| **NS-0.6** | Empty-zone tick < 0.5 ms |
| **NS-0.7** | **Trunk throughput:** one trunk connection sustains 20k pps of 1,200 B payloads (≈ 200 Mbit/s) for 10 min with < 0.1 % drops on ≤ 1 core, on loopback on Windows and Linux CI; otherwise the §2.6 fallback starts |
| **NS-1.1** | 50 players + 1k NPCs in one system at 20 Hz: tick p99 ≤ 25 ms |
| **NS-1.2** | Downstream p95 ≤ 128 kbit/s |
| **NS-1.3** | Mispredictions < 1 % at 100 ms/1 % |
| **NS-1.4** | Zone transition ≤ 3 s, 0 reconnects |
| **NS-1.5** | **AG trees and dormancy:** 10k board/disembark cycles on the Kestrel, interleaved with relogs and cell restarts (BE-A3a), plus **relog storms**: 500 bots log out and back in within 5 s, 20 times, with 100 of the relogs made while the old copy is still linkdead. Pass: 0 custody/fence mismatches in the §10.3 audit (active and dormant rules); every logout leaves a dormant row with NULL owner ≤ 5 s after its final checkpoint; every takeover's superseded copy gets `released` p99 ≤ 1 s and is gone ≤ 2 s; 0 `CheckpointZombie` alerts from Join, Leave or relogs; 0 active rows without a simulating owner; 0 conservation deltas; 0 tick waits on fence calls |
| **NS-2.1** | 500 bots/zone at 20 Hz: tick p99 ≤ 35 ms, ≤ 256 kbit/s steady |
| **NS-2.2** | Lag comp ≥ 98 % agreement with the local-hit reference at ≤ 150 ms RTT |
| **NS-2.3** | 1k soak, 0 crashes |
| **NS-2.4** | **10 min replay bit-exact** on the same platform, of a 500-bot zone with budgets binding: the lane fuel budget deferring coroutines every tick, ≥ 10 injected fuel kills and ≥ 2 wall-backstop kills, ≥ 100 container activations and ≥ 10k service replies arriving under NetSim jitter. **Keyframe clause:** the same zone runs 70 min at the default 30 min cadence, and the 10 min window that starts at the rebase keyframe cut ≥ 60 min into the run replays bit-exact from that keyframe and the log after it alone, with no earlier log present. At that rebase ≥ 1,000 coroutines are ended and re-armed through `Adopted{cause = rebase}`, ≥ 20 of them waiting in `awaitService`; the keyframe tick costs ≤ 3 ms more than its neighbours; the run's quest and level-script corpus completes with 0 script errors and 0 lost service results across both rebases; and in a variant where a module with no `Adopted` handler is part-way through a 2 s wait at the due tick, the rebase comes only after that wait ends, and the window still replays bit-exact |
| **NS-2.5** | TiDi absorbs 3× overload without desync |
| **NS-2.6** | **Gateway capacity:** one gateway instance serves 256 encrypted bot sessions at 20 pps each way on ≤ 1 core (≤ 70 % busy), adding ≤ 2 ms p99; 8 instances on an 8-core SERVER box serve 2,048 sessions; a 500-player cell's K = 2 trunks carry its full egress between two SERVER hosts with < 0.1 % drops |
| **NS-2.7** | **Recovery fence:** `Fence.AdvanceOwnedBy` over 5k active persistent AGs, in a region from which ≥ 50k AGs have been parked, p99 ≤ 1 s; it returns exactly the 5k active rows, and 0 dormant AGs are loaded or revived; 100 % of zombie checkpoints written after the advance are rejected (fence cache or merge rule) and alerted; hitch ≤ 10 s (BE-A3b) |
| **NS-3.1** | Handoff p99 < 100 ms |
| **NS-3.2** | 0 duplicated or lost AGs over 1M torture handoffs |
| **NS-3.3** | 5k CCU shard |
| **NS-3.4** | Cell kill → ≤ 10 s hitch, ≥ 99 % sessions kept |
| **NS-3.5** | Instance ready ≤ 2 s |
| **NS-3.6** | **AG-tree torture (BENCH-6 class):** 100k board/disembark and dock/undock cycles, interleaved with handoffs of the carrying ship and random cell kills: 0 custody/fence mismatches, 0 tree-epoch mismatches, 0 zombie checkpoints accepted, 0 conservation deltas (05 §4.4) |
| **NS-3.7** | **Zone-leader kill:** killing the leader cell of a 4-cell zone under 2,000 bots gives a new leader in ≤ 3.5 s; 0 handle reuse (handle audit); 0 TiDi desync (every cell reports the same `d` for every tick number); 0 spawn stalls; tick skew p99 ≤ 1 ms throughout |
| **NS-3.8** | **Cross-compiler and cross-vendor replay:** NS-2.4's recordings replay bit-exact GCC→MSVC and MSVC→GCC, and between an AMD host (the EPYC SERVER node) and an Intel host (the H1 Intel box, 09 §4.3.1) in both directions, with identical fuel counts. The recordings include a Luau corpus that calls every `math.*` function and `vector` operation and uses `^` with non-integer exponents in both runtime and constant-folded forms, and ≥ 10k lag-compensated hits posed by `det::HitboxSampler`. Cooked bytecode is byte-identical from MSVC- and GCC-built cooks; every VM host's 256-vector `det-math` self-test passes on both vendors; 0 `FpReset` events; and the `simdet` self-test flags each planted violation (a libm call, an `rsqrt` estimate, `std::sort` on SIM data, `long double`, an MXCSR write). **Multi-cell clause:** every cell's log from one NS-3.12(c) seam-furball run and from NS-3.11(c)'s migrations, including a 30 s control-plane outage whose hold expires, replays bit-exact one cell at a time, both on the recording platform and GCC↔MSVC. The replayer takes co-location and handoff outcomes only from `ColocDecision` and `HandoffDecision` events: a replay run with `ag_tick_us`, tick p95 and the overload stage forced to other values still matches. P's log to T and Q's from T+1 chain through `MigratedFrom{P, T, state_hash}`, with Q's `migrate` keyframe hashing to P's tick-T state hash, and each late ρ's log carries its `ZoneRejoin`. **Physics order:** a replay from a keyframe re-creates the keyframe's bodies with their recorded `BodyID`s, but bodies created after it take other free slots than the live cell's did; the NS-2.4 zone, with ground vehicles and landing ships on multi-tile terrain, still replays bit-exact for 10 min from such a keyframe (02 §7.1) |
| **NS-3.9** | **Voice:** a 250-member fleet channel across ≥ 4 gateways (3 concurrent speakers plus commander) and 50 concurrent proximity speakers in a 500-player hub: mouth-to-ear p95 ≤ 250 ms with both legs at 80 ms RTT and 1 % loss; ≤ 104 kbit/s down and ≤ 40 kbit/s up per client; forwarder ≤ 1 core; block, mute and sanction effective ≤ 1 s; ≤ 15 voice pps per session on average |
| **NS-4.1** | 50k CCU shard, held ≥ 1 h with every Ph4 metric in budget (AAA-SRV-3); the same load runs the 72 h AAA-STB-4 soak. **Failure domains (05 §1.4.3, BE-A15):** during the hour, power off a SERVER host carrying ≥ 8 cells, and separately partition a SERVER rack (≥ 30 processes) at its top-of-rack switch for 5 min. Pass: every affected region served by a standby ≤ 10 s after confirmation; no control-plane degraded episode > 5 s; login admission never pauses; ≥ 99 % of affected sessions kept; 0 value errors; after the rack heals, 100 % of its zombies' writes and trunk messages rejected |
| **NS-4.2** | 2,000 ships fight for 30 min in one 2 Hz fleet-battle zone (§3.4) and `d` never needs to drop below 0.1. Measured at `d` = 0.1: module response p95 ≤ 1 tick of game time + RTT, and no activation waits more than 2 ticks (01 AAA-SRV-10). The bots fight as **8 player fleets of 250** (05 §1.12.1, 06 §8.3a), four per side: bot fleet commanders call targets and positions with broadcasts, fleet-warp their fleets onto the grid mid-fight and run command bursts, and every fleet warp meets 06 GP-15's same-tick, in-formation landing |
| **NS-4.3** | Battle downstream p95 ≤ 512 kbit/s (AAA-SRV-6, via command replication and fleet aggregation) |
| **NS-4.4** | 8k sessions per 8-core gateway, measured three ways (§2.6, §6.6): **(a)** sessions spread over single-cell zones; **(b)** all 8k homed in a 10-cell zone at 5,000+ bots per zone (4–10 contributors per session, `RemoteViewer` fan-out, `Demand`/`Grant` traffic); **(c)** 8k sessions scattered with no zone affinity over every populated cell of a 50k-CCU shard (≥ 200 cells). All three at ≤ 70 % CPU per core, ≤ 2 ms p99 added latency and gateway pack delay p99 ≤ 2 ms. In (c) also: ≤ 1,024 trunk connections, ≤ 64 MB of trunk state, trunk IO threads equal to the pool size (4) and ≤ 1.5 cores of trunk IO. With affinity on, ≥ 80 % of new tokens land in the zone's affinity set (`gw_affinity_hit_ratio`) |
| **NS-4.5** | Security review closed |
| **NS-4.6** | **Replicant recovery (§6.4):** (a) in 4-cell Harrow orbit under 2,000 bots, 20 `kill -9`s of cells and 5 of replicants; (b) in single-cell Harrow High under 500 bots, 10 `kill -9`s of its cell, with its replicant shared with three cells of other zones. Both at the pass bar below. Per-AG state rollback p99 ≤ 1 s against the bots' last observed state (AAA-SRV-9 Ph4); hitch ≤ 10 s and ≥ 99 % of sessions kept; 0 disconnects caused by a replicant kill, whose cells resync it in ≤ 5 s; 0 zombie chunks accepted (stale `lease_gen` or epoch); 0 conservation deltas; replicant stream ≤ 20 Mbit/s per 500-player cell and ≤ 0.3 ms of tick time; a replicant serving 4 cells uses ≤ 1 core and ≤ 4 GB. **Tick-consistent restore:** the replicant commits only ticks with a complete `TickFlush`, and every restore resumes from one restore tick: after each restore, 0 interpenetrations deeper than 5 cm (overlap query at the first resumed tick); 0 physics explosions (over the next 5 s no body exceeds its class `v_max`, and none gains > 20 m/s in one tick without a recorded impulse); every transient's restored position within v × 200 ms + 0.5 m of the truth tap's position extrapolated to the restore tick (§10.3), and every `CommandKinematic` ship exact; re-sent effects applied exactly once relative to the restored `EffectInbox` (0 double-applied damage) |
| **NS-3.10** | **View composition across a boundary (v1, §6.6):** 4-cell Harrow orbit under 2,000 bots, with the BENCH-6 *Mule* crossing a boundary and 10k scripted crossings of one boundary (characters, small craft, 100 by a capital, 1k by the observers' own ships), and a boundary-observer bot 100 m from it on each side. Pass: far-side entities within their near radius reach each observer at the near-tier rate (inter-update p95 ≤ 60 ms at 20 Hz, within 5 % of same-side entities at equal distance); every session receives every capital and station in the system (0 misses); **0 pops** over all crossings (no Destroy→Create of one EntityId on a connection while it stayed within the leave radius, and no update gap beyond max staleness, §4.4); 0 full-state updates or creates caused by a handoff, and resync ≤ 64 B per (entity, viewer) per crossing; 0 `(handle, tick)` written by two cells; per-client downstream p95 ≤ 256 kbit/s steady and ≤ 512 kbit/s in the scripted battle (AAA-SRV-6); stage 7 p99 ≤ 3 ms with remote viewers, their share ≤ 1.2 ms; registration plus demand traffic ≤ 5 Mbit/s per cell; gateway pack delay p99 ≤ 2 ms. **Co-located tow (§6.2a):** a tug bot tows a wreck by tractor for 20 km at 1 km/s, through the boundary and 10 km into the neighbour region, 50 times each way, watched by boundary observers on both sides and by an observer in a third cell: 0 pops; `ghost_audit_missing` = 0 (every cell within the ghost margin of either hull holds a fresh ghost, and every viewer within registration distance is registered at the host); the set hands off as one each time its centroid is H past a boundary; no member is ever more than 2H from its host's regions; and neither side sees a position discontinuity > 0.5 m at a host change. It reruns in Phase 4 in a 10-cell zone at 5,000 bots (SRV-2), with stage 7 p99 ≤ 3 ms, registration plus demand traffic ≤ 12 Mbit/s per cell, and the far-broadcast fallback allowed |
| **NS-3.11** | **Planned region migration (§6.7):** under load, migrate each 100 times, alternating N→N+1 and N+1→N builds: (a) a 500-bot, 20k-entity single-cell hub (Harrow High); (b) a packed process of 64 instances (≤ 200 bots), by `Drain`; (c) one cell of 4-cell Harrow orbit under 2,000 bots. Also (d) `PreProvision` of a Vane system with 500 players aboard ships to a reinforced fleet-battle host, 10 times. Pass: **hitch p99 ≤ 1 s** per zone (P's last tick to Q's first; zone-wide in (c)) and within the §6.7 profile budgets; **0 disconnects** and 0 re-placements; **0 state loss**: every migration's tick-T state hash matches on both sides, **0 lost transients** (EntityId census before and after), **0 conservation deltas** (05 §4.4), **0 `(handle, tick)` pairs written twice** and 0 handle reuse (handle audit); 0 pops and 0 creates caused by a migration (NS-3.10 definitions); per-client downstream within its §2.5 budget during the resync, with the near tier resynced ≤ 250 ms after resume and every pending field ≤ 2 s after; value calls held for the fence step ≤ 1 s p99; the packed-process drain ≤ 60 s; 0 `CheckpointZombie` alerts. Injected faults: a Q kill, a hash mismatch and an orchestrator timeout before the commit each leave P running with no state change and a hitch ≤ 1 s; a Q kill within 10 s after the commit recovers from P's frozen copy, losing ≤ the time since the commit; an orchestrator outage after the ACK never yields two simulators (0 `(handle, tick)` pairs written twice). In (c), outages after the ACK of 10 s (an orchestrator-leader takeover) and 30 s (a PG failover), 10 each: the three other cells resume ≤ 1 s after the freeze through hold expiry, with tick skew p99 ≤ 1 ms; ρ rejoins ≤ 3 s after the control plane returns (late-cell catch-up or rejoin at the current tick); 0 `(handle, tick)` pairs written twice. A full rolling restart of (c)'s zone gives each player there 4 hitches, cumulative ≤ 1.6 s p99. **(e) `sim_abi` change:** with N and N+1 differing in `sim_abi.physics` (a test build on the next Jolt release, or a fork with a changed `StateRecorder` layout), 100 migrations of (a) and 20 of (c) take the portable physics path: hitch within its profile budget + 50 ms; tick T's quantized state hash equal on both sides; 0 interpenetrations deeper than 5 cm and 0 physics explosions over the next 5 s (NS-4.6 definitions); a mismatched blob forced through the normal path is refused at step 0 and never parsed; and a Q whose Luau bytecode range excludes P's content is refused at step 0 |
| **NS-3.12** | **Combat at a cell seam (§5.6):** (a) 32 vs 32 character bots fighting across the boundary of a 2-cell ground test zone with hitscan and fast-projectile weapons, and (b) 50 vs 50 Kestrels dogfighting with ship guns across a boundary of 4-cell Harrow orbit; 30 min each at 50–150 ms RTT and 1 % loss, with ≥ 1,000 handoffs of targets under fire. Pass: ≥ 98 % agreement with the seam fighters' local-hit reference on hit or miss and on hit zone, with the ghost-target rate within 0.5 points of the local-target rate (NS-2.2 method); **0 double-applied damage** (every `ApplyDamage` idempotency id applied once; damage dealt equals damage taken in the audit); resolution hitch-free through the target's handoff (0 shots lost or delayed > 1 tick; tick p99 in budget); `lagcomp_ghost_gap` ≤ 0.1 % of ghost-target shots; the cook rejects a test weapon whose range exceeds its class's `M_hit` cap. **(c) Seam furball (§6.2a):** 100 Kestrels (50 v 50) centred on a seam of 4-cell Harrow orbit for 30 min with collisions on, 20 ramming bots, 10 tug pairs and 10 boardings of disabled ships across the seam. Reported: maximum and p99 co-location set size, `coloc_refused` by reason, `coloc_late`. Pass: every set ≤ 32 roots and ≤ 128 rows; every cell's leased-in load ≤ 64 roots and ≤ 10 % of its tick budget; **every cell's tick p99 ≤ 35 ms**; effect-only contacts conserve momentum within 5 % per contact (pair audit from the truth tap); 0 pops; `ghost_audit_missing` = 0. **Phase 4 rerun:** 10 `kill -9`s of a target's cell mid-fight in (b), with every outboxed effect applied exactly once relative to the restored state |
| **NS-4.7** | **Gateway-box kill at scale:** at 50k CCU (NS-4.1 load), `kill -9` of every gateway process on the most loaded box (≈ 8k sessions; NS-4.4), 5 times, plus one power-off. Pass: ≥ 99 % of its sessions are back in the world ≤ 10 s (input accepted by the home cell and every near-tier entity present), 100 % ≤ 60 s; the Session service redeems ≥ 10k tickets in ≤ 5 s at p99 < 250 ms per call; 0 "full" denials (the §2.6 headroom holds); 0 value errors (conservation audit, AAA-STB-5); login admission holds ≥ 200/s with no pause (AAA-SRV-7 Ph4); cell tick p99 stays in budget throughout. **Under view composition (§6.6):** one of the 5 kills hits a box whose sessions are homed in a 10-cell zone. **Field-state audit:** 10 s after rejoining, 1,000 sampled field-auditor bots compare every replicated field of every entity they hold with the authoritative quantized state at the same tick; any mismatch not explained by a change inside the interpolation delay, any held entity that is not relevant and any relevant entity missing fails. No cell writes to the dead box's trunks > 2 s after their silence (`gw_dead_trunk_writes`) |
| **NS-5.1** | Dynamic split/merge under a moving hotspot |
| **NS-5.2** | Handoff p99 < 50 ms |
| **NS-5.3** | Cell-crash hitch ≤ 1 s via the gateway replication layer |
| **NS-5.4** | 100k-bot shard simulation |

### 11.5 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Protocol/crypto flaw; no forward secrecy | Vendored netcode/reliable, no Helios crypto; Go↔netcode token interop tests; fuzzing; external review; X25519 re-key option |
| Split-authority gameplay bugs (SC, SpatialOS) | Type-enforced `GhostRef`; effects-only mutation; handoff exercised from Phase 1; torture bots |
| Big-battle fan-out | Budgets, LOD groups, command replication, volley aggregation, TiDi, early 1k-ship bots |
| netcode trunk and gateway scale (single-threaded, R07 §3.1 targets ~100 players) | Early gates NS-0.7 (trunk) and NS-2.6 (gateway); trunk profile, coalescing and K-way sharding (§2.6); patch netcode/reliable, then GameNetworkingSockets behind `Endpoint` |
| Cross-compiler determinism (nav, replay) | Fixed-point integrators; cross-compiler hash tests; periodic corrections as fallback |
| Gateway becomes a stateful bottleneck | Sessions sharded across netcode instances; horizontal scale; gateways keep only soft per-session state (routes, latest `ViewerState`, registrations, grants) that a replacement rebuilds from the home cell within one 4 Hz refresh; entity state lives in cells and replicants, never gateways; v2 layer only after measurement |
| Replicant tier cost, lag or patent exposure (Improbable US 11,792,306) | Reuses stage-6 chunks (NS-4.6 caps stream and tick cost); 2-vCPU replicants shared by ≤ 4 world-zone cells across zones (≈ 6 % compute); epoch- and generation-fenced like any receiver; checkpoint fallback per AG; recovery-only scope with no client views; counsel review before WP-4.3 (09 K18), with a per-zone hot-standby cell on the same stream as the design-around |
| Luau stalls ticks | Fuel-metered lane, deterministic fuel kill with a 20 ms wall backstop, heap caps, `ScriptState` rule, script-cost telemetry |
| Replay erodes as features land, or cannot start mid-session | Design rule 8, `SimInbox` as the only async path, `simdet` lint, logged load-dependent decisions (`ColocDecision`, `HandoffDecision`), replay keyframes every 30 min through a script rebase, Jolt order by stable body keys, nightly replay of the 1k soak, NS-2.4 (keyframe clause) and NS-3.8 (multi-cell clause) |
| AG-tree/fence drift (dupe window when a ship with passengers hands off) | Lockstep tree epochs, fenced Join/Leave, custody never re-homed, fence cache + `(e+1, 0)` records, nightly custody audit, NS-1.5/2.7/3.6 |
| Offline AGs revived or orphaned (stale owner on logged-out characters, parked ships, unloaded interiors) | `Fence.Park` to dormant rows with NULL owner; recovery matches active rows only; at-rest custody via dormant-fence ledger preconditions; `released` to superseded owners; orphan and dormant audits; NS-1.5 relog storms, NS-2.7 with ≥ 50k parked rows |
| Seams visible across cells (pop-in, 5 Hz ghosts, full-state bursts) or remote-viewer cost at 2,000–5,000 players per zone | Owner-only replication to registered remote viewers; handoff moves per-session resync records; one source per tick by epoch plus the client's per-handle tick rule; gateway water-filling over one budget; stage 7 cost model with a far-broadcast fallback; NS-3.10 and its Ph4 rerun |
| Gateway-box loss at Phase 4 scale (8k simultaneous reconnects) | N+1 box headroom (+20 %) across availability zones; jittered client reconnect; Session-service burst target; reconnects outside the admission queue; NS-4.7 |
| Availability-zone loss (≈ 17k simultaneous reconnects) | Zone rule: two zones hold every session and still meet the box rule (05 §6.7); Session replicas and Valkey spread over zones; 05 A19's gateway clause |
| Planned moves hitch or lose state (rolling restarts, epoch flips, `Drain`, `PreProvision`) | Planned region migration (§6.7): pre-copy on the replicant stream, a freeze of ≤ 1 s p99 checked by state hash before the commit, the region-lease bump as the single commit point, a cooperative `Fence.MigrateRegion` that cannot fail after it, compact session records bounded by budgets, bounded concurrency and a 10 % surge; NS-3.11 |
| Seam combat bugs (SC-class misses, double damage, dead zones at a boundary) | One rule: the shooter's cell resolves against tick-rate ghost hit history; the `M_hit` cook check and server range clamp; idempotent `ApplyDamage` with sequence-exact de-duplication and outbox re-send across recovery (§5.6, §6.2); NS-3.12 |
| Restores or rejoins leave inconsistent state (mixed-tick replicant restore, stale fields after a gateway loss) | `TickFlush` commits, one restore tick with command replay, dead reckoning and depenetration (§6.4); `HeldEntities` rebind at every contributor (§6.6); NS-4.6 and NS-4.7 clauses |
| Co-location cascades onto one cell, or leased AGs vanish from views and ghost sets (seam furballs, long tows) | Transitive sets capped at 32 roots, 128 rows and 5 % of the host's tick; refusal at 70 % p95 or overload stage ≥ 2, with effect-only fallbacks; leased-in load ≤ 10 % per cell; leash with whole-set handoff by centroid; presence and ghost margins by occupied region, not owned region (§6.2a); ghost audit; NS-3.10 tow and NS-3.12(c) |
| A control-plane fault during a multi-cell migration freezes the zone | Production `MigrationHold` bounded at 1 s with a self-computed release anchor, so only ρ waits; late-cell catch-up or rejoin at the current tick; one region at a time with hitch counts stated per rollout (§6.7); NS-3.11(c) outage cases |
| A third-party serialized format (Jolt, Luau bytecode, Recast) changes between N and N+1 | `sim_abi` per component in residuals, handoffs and replicant streams; portable physics path from quantized state plus the consistency pass; refusal at prepare for unloadable bytecode; CI promotion gate on NS-3.11(e) |
| Third-party math on the sim path breaks replay (UCRT vs glibc libm in Luau, `rsqrtps` in ozz, STL sort ties, MXCSR) | Luau `det-math` patch, `det::HitboxSampler`, `det::sort`, asserted MXCSR, `simdet` rules with planted-violation self-test; cross-compiler and Intel↔AMD replay (NS-3.8), RT-03/04 on both vendors |
| Gateway↔cell trunk full mesh at Phase 4 (hundreds of trunks per box) | Fixed trunk IO pool multiplexing sockets (epoll/`recvmmsg`, RIO), buffers sized by traffic, ≤ 1,024 trunks and ≤ 64 MB per box, zone-affinity token assignment; NS-4.4(c) scattered case |
| Zone leader as a single point of failure | Orchestrator assignment with generations, mirrored standby leader, reserve handle blocks, PG-recorded blocks, NS-3.7 |
| Voice abuse and privacy | Server-side blocks and sanctions, 60 s report ring only, no continuous recording, legal review before automated moderation (§2.7) |
| Egress cost | Per-client budgets; bandwidth regression gates; hosting model in 05 |

### 11.6 Traceability

| Requirement | Section |
|---|---|
| R07-P0-1, R02-P0-3, R07-P1-20 | §1, §2, §9 |
| R07-P0-4, R02-P0-2 | §1, §6.5, §7 |
| R07-P0-5, R02-P0-4, R06-ENG-18, R04-P0-6, R05-P1-13, R01-P0-6 | §4 |
| R07-P0-6, R05-P0-3, R05-P0-11 | §5.1–5.5, §9 |
| R05-P0-4, R05-P0-9, R07-P1-16 | §5.6 (including across cell boundaries), §3.4 |
| R05-P1-12, R01-P1-10, R04-P1-15 | §5.7, §5.4 |
| R07-P1-13, R04-P1-11, R02-P0-1, R04-P0-4, R04-P0-7 | §6 |
| R07-P2-21, R07-P2-22, R04-P2-19 | §6.5, §8 |
| R01-P0-1/3/4/5, R04-P1-16 | §3, §6.7 |
| R07-P1-14, R01-P1-14, R07-P2-25, R03-P0-10 | §8, §6.7 |
| R03-P0-3/6/8, R05-P0-6, R07-P1-18 | §7 |
| R07-P0-10, R05-P1-19/20, R06-ENG-02/15/27, R01-P2-22 | §10 |
| ADR-015 (voice), 02 §7.3 Phase 3 voice | §2.5, §2.7 |
