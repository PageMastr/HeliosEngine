# 04 — Networking & Servers

*Helios master plan, section 04. Draft. Conforms to ADR-001/002/004/005/007/008/011–014; phases per
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
                                                 ▼
                                   helios-cell B, C, …
```

| Process | Owns | Never |
|---|---|---|
| **Client** (C++) | Input; prediction of own avatar/ship/abilities; interpolation; content streaming | Decides outcomes |
| **Gateway** (C++) | netcode instances, channels, rate limits, routes, chunk packing, ack translation, multi-cell merge (v1), non-spatial RPC (chat, social, market) → Go via NATS | Game logic |
| **Cell** (C++) | Authoritative simulation: ECS, Jolt, AI, Luau, interest, replication, AGs, ghosts, handoff, lag compensation | Public traffic; SQL; value writes except via Ledger |
| **Orchestrator** (Go) | Registry, region leases (NATS KV, TTL 3 s), placement, instances, routes, TiDi policy, split/merge (v2) | Per-tick traffic |
| **Session svc** (Go) | Connect tokens after login queue; `session_epoch` (Valkey CAS); reconnect tickets | — |
| **Persistence GW** (Go) | Batched epoch-fenced checkpoints; **Fence table** (owner + epoch per AG, CAS) | Value (Ledger's job) |
| **Ledger** (Go) | Item/currency moves: synchronous, idempotent, fenced by (AG, epoch) | — |

**Orchestrator touchpoints:** `RegisterProcess`, 1 Hz lease heartbeat, `AssignRegion/ReleaseRegion(instance,
region, lease_gen)`, `Create/DestroyInstance`, `ReportLoad` (1 Hz: tick p99, players, egress, dilation, overload
stage), `ResolveRoute`, `TransferPlayer`, `PreProvision`, `Drain`, `Split/Merge` (v2). **Persistence:** `Fence.Advance(ag, expected,
next, owner)` (sync CAS, NATS request/reply, ~1 ms), `Checkpoint.Write` (async 1 s batches; per AG every 30 s
while dirty and on logout/zone exit), `Checkpoint.Load`.

**Links:** client↔gateway HTP on UDP 7777 (the only public game port); gateway↔cell and cell↔cell HTP trunks
(§2.6); cell/gateway→Go via **nats.c v3.14** request/reply and JetStream with schema-codegen binary payloads,
never per tick, no protobuf/gRPC in C++ (ADR-013). Handoff and replication never cross NATS (ADR-008).

| | Windows dev box | Linux production |
|---|---|---|
| Go backend | `helios-backend.exe`: all services in-process, embedded NATS, embedded-postgres, miniredis (ADR-014) | Per-service Deployments, Postgres HA, Valkey, NATS cluster |
| Gateway | `helios-gateway.exe` on `127.0.0.1:7777` (no firewall prompt); PIE may use `helios-cell --embedded-gateway` (in-process trunk) | ≥ 2 per shard, bare metal, anycast behind scrubbing |
| Cells | One `helios-cell.exe` for all dev zones; PIE spawns it + clients/bots; `helios-dev up` starts everything | Agones Fleets per zone class, warm pool, hot standby |

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
| ↳ Helios messages: `hdr` u8 (channel 3 bits, slice bit, sequenced bit), `msg_seq` u16 (ordered/sequenced channels), `len` varint, payload | ≤ ~1,190 B | Helios |

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

A reliable message rides in packets until reliable acks one of them (`RTO = srtt + 4·rttvar`, 30 ms–1 s; ≤ 1,024
in flight per channel, else `send()` returns `WouldBlock`). A packet is declared lost when one ≥ 3 sequences
newer is acked or after 1.25 × srtt; losses drive STATE repair. STATE and INPUT never exceed one datagram;
reliable's fragmentation serves only rare CONTROL bursts (≤ 16 KB). Packing order: CONTROL > EVENT_R >
INPUT/STATE > EVENT_U > LATEST > BULK.

### 2.3 Connect tokens and handshake

The **Session service** mints netcode-format tokens in Go after the login queue (`x/crypto`
XChaCha20-Poly1305; CI interop test against vendored netcode) and returns them over HTTPS. **Public part:**
`protocol_id` (build-compatibility gate), expiry **≤ 45 s**, timeout 10 s, 2–4 gateway instance addresses,
per-session keys, and the 1,024-byte private part sealed under the shard key (Session service and gateways
only). **Private part:** `client_id` = session ID, addresses, keys, **256 B user data** (character ID,
content-version pin, placement ticket, entitlements, attestation hash, §9).

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
`session_epoch` (the old gateway drops its copy), and the new gateway sends the cell `ClientRebind`
(baselines invalidated, full resync). Target 1–3 s.

### 2.5 Congestion and bandwidth

Per-connection **budget** from reliable's estimates: 256 kbit/s by default; in battles 512 kbit/s through Phase 4
and up to 1,000 kbit/s from Phase 5 (AAA-SRV-6, R07 §8.3). AIMD: loss > 5 % or
`srtt > min_rtt + 100 ms` for 1 s → ×0.7 (floor 64 kbit/s); 5 s clean → +16 kbit/s per second. One packet per
tick (≤ 4 when budget allows, paced 2 ms). Upstream `min(tick_hz, 60)` pps under a **64 kbit/s token bucket**.
The replication writer gets `budget/8/tick_hz − reliable backlog` bytes per tick. Replication is bit-packed,
never compressed; BULK uses zstd with a trained dictionary.

### 2.6 Gateway scaling and trunks

A netcode server is single-threaded and sized for hundreds of clients (R07 §3.1), so a gateway runs **one
instance per worker thread and UDP port** (e.g. 32 × 256 slots ≈ 8k sessions); the Session service fills
instances by reported free slots. **Trunks** (gateway↔cell, cell↔cell) are netcode connections with
orchestrator-minted trunk tokens; messages carry a varint stream ID (session or AG); handoff blobs use BULK
slices. **Cells never build client packets:** they emit per-connection **replication chunks** (≤ 1,100 B); the
gateway packs them (1:1 in v0), maps client acks/losses back to per-cell notifications, and in v1 splits the
client budget across contributing cells. v2 then relocates replication into the gateway without a rewrite.

## 3. Cell server architecture

### 3.1 Process model

`helios-cell` is the headless engine build (`HELIOS_BUILD_GRAPHICS=OFF`, Null RHI; R06-ENG-02). A `ZoneHost`
runs N **ZoneInstances**, each with its own flecs world (ADR-004), one Jolt `PhysicsSystem` per grid (ADR-005), **one Luau
VM**, ZoneClock (1–60 Hz), replication context and AG table; instances share nothing mutable and talk via
mailboxes. Small instances are packed EVE-style (≤ 64 instances or 200 players per process); hot zones get
dedicated processes. Zones **live-migrate** between processes by bulk AG handoff (R01-P0-1, Phase 3).

Luau runs only in the gameplay stage under a 15 % time slice (`interrupt` callback; overrunning coroutines
resume next tick) with heap caps (16 MB small, 256 MB large). Service calls yield the
coroutine; results arrive as messages. **Rule:** per-entity script state lives in schema components
(`ScriptState`), never VM locals, so it survives handoff and checkpoints.

### 3.2 Zone clock and time dilation (TiDi)

Game step is fixed (`tick_dt = 1/tick_hz`); dilation `d ∈ [floor, 1]` (floor 0.1, R01-P0-3) stretches only wall
time: `wall_interval = tick_dt / d`, so results are identical under TiDi. Gameplay timers are in ticks;
wall-clock services (industry, market) are not dilated. Controller, `L = tick_cpu_ms / (1000/tick_hz)`: **fast
attack** `L > 0.9 → d ← max(floor, d·0.85/L)`; **slow release** +0.05/s after 2 s of `L < 0.7`. `d` rides in
snapshot headers and CONTROL; clients scale prediction, interpolation and animation and show an indicator. In
multi-cell zones the **zone leader** broadcasts `d = min(demands)`; cells tick in phase (skew ≤ 1 ms).

### 3.3 Per-tick job graph

Job system per ADR-011 (flecs systems and Jolt run on it); 1–2 IO threads fill per-instance mailboxes.

| # | Stage | Parallelism | Budget @20 Hz |
|---|---|---|---|
| 1 | **Input**: mailboxes; one input per controlled entity (§5.5) | per AG | 2 ms |
| 2 | **Pre-physics**: ability SMs, flight controllers, movers, AI results | ECS scheduler | 6 ms |
| 3 | **Physics**: Jolt step per grid; space substeps at 60 Hz | per grid + Jolt jobs | 10 ms |
| 4 | **Post-physics**: contacts, damage, effects inbox, Luau events (time-sliced) | ECS; Luau single-thread | 8 ms |
| 5 | **Authority flush**: effects, ghost updates, handoff offers | per neighbour | 1 ms |
| 6 | **Replication gather**: shadow + serialize once (§4.2) | per ECS table | 3 ms |
| 7 | **Connection write**: interest, priority, chunks | per connection | 3 ms |
| 8 | **Send + record**: trunk send, hitbox history, metrics | IO + jobs | 2 ms |

Total **35 ms = p99 target (70 % of 50 ms)**, leaving headroom before overload (§8); 11.7 ms at 60 Hz.

### 3.4 Budgets per zone profile

| Profile | Tick | Players | Replicated entities | Cores | RAM | Egress |
|---|---|---|---|---|---|---|
| Ground hub / station | 20 Hz | 500 | 20k | 8 | 6 GB | 128 Mbit/s |
| Open space (system) | 20 Hz (+60 Hz substeps) | 500 | 20k | 8 | 6 GB | 128 Mbit/s |
| Fleet battle (Phase 4) | 10 Hz × TiDi | 2,000 | 50k | 16–32 | 16 GB | ≤ 1 Gbit/s |
| Activity (raid/FPS/PvP) | 30–60 Hz | 6–64 | 2k | 1–2 | 512 MB | 32 Mbit/s |
| Phase / housing | 10 Hz | 1–16 | 1k | 0.25 | 128 MB | 4 Mbit/s |

500-player memory: static data ≤ 3 GB, ghost records 32 MB, hitbox history ≤ 16 MB. Per-profile entity caps
are enforced with telemetry (R04-P1-16).

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
v2, R07-P2-25).

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
(SWG snapshot lesson); multi-cell zones lease handle blocks from the zone leader; handles survive handoff.
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
| Space battle (512 kbit/s) | 50 ships @20 Hz 21.0 + 150 @5 Hz 15.8 + 300 @1 Hz 6.3 + launches 1.6 + vitals 2.0 + overhead 2.6 | **49.3 KB/s ≈ 394 kbit/s** ✓ |
| Same, far ships command-replicated (§5.4) | 300 far ships ~2 B/s each | **≈ 345 kbit/s**; ~1,000 ships fit 1 Mbit/s |
| Upstream, 20 Hz | 20 pps × (55 B + 4 inputs, Δ-coded) | **≈ 16 kbit/s** ✓ (≤ 64) |
| Upstream, 60 Hz instance | 30 pps × (2 new + 2 redundant) | **≈ 24 kbit/s** ✓ |

Server side: a 500-player cell emits ≈ **128 Mbit/s**; 5k CCU ≈ 1.3 Gbit/s, 100k CCU ≈ 25.6 Gbit/s (R07 cost
flag → 05). CPU at 500 connections: gather ≈ 2 ms/tick, writes ≈ 4 ms over 8 cores.

## 5. Movement and combat netcode

### 5.1 Time model

Command frames = zone ticks. The client runs **ahead** of the server by `RTT/2 + input buffer` (target 1.5
ticks); snapshots report server-side input-buffer depth and the client nudges its tick rate ±3 % (Overwatch,
R05-P0-3). CONTROL time sync every 1 s (NTP-style, best 3 of 8). Under TiDi the command clock follows `d`.

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
against static colliders; with `JPH_CROSS_PLATFORM_DETERMINISTIC` on both sides (ADR-013) the integration
matches the server bit-for-bit, so corrections come only from unpredicted contacts, which are smoothed. Only the seat
holding the flight input channel predicts the hull; gunners predict turret aim; passengers walk in the ship's
grid frame, unaffected by hull corrections (R04-P0-3). Owner jitter buffer: 4 ticks at 60 Hz.
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
counted; > 5 % over 30 s emits cheat telemetry and trips an auto-kick threshold. Missing input repeats for 2
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

### 5.7 Predicted abilities (ADR-002)

Ability graphs compile to the native rollback-safe state-machine format, run identically on client and server. SM state `{state id, state tick, charges, cooldown end tick, ≤ 64 B vars}` is predicted owner state,
reconciled like movement. Each activation carries a client **prediction key**; the server confirms or rejects,
and rejection rolls back the SM plus predicted cues and movement modifiers (dash, blink). SMs may only use
timers, input checks, predicted projectile spawns, cosmetic cues, movement modifiers and server-effect *requests*. Damage, loot and value spend are never predicted.

## 6. Authority groups, ghosts and handoff

### 6.1 AGs, epochs and the fence

An **AG** is a root entity plus its attached hierarchy (a ship and its interior grid); a player on foot is
their own AG and joins the ship's AG on boarding (R04-P0-4). Each AG records `owner_cell` and a **64-bit
epoch**. The **Fence** (persistence gateway, one row per AG in the shard primary; 05 §1.13) is the
linearization point: `Fence.Advance` is a conditional update (p99 < 5 ms) over NATS request/reply, while handoff
state moves cell→cell on the trunk (ADR-008). Persistence and Ledger reject writes with a stale `(ag, epoch)` (R07-P0-7), so a
crash can roll back position or XP but never duplicate value.

### 6.2 Ghosts, effects, co-location

Cells ghost neighbour AGs within `M = max(class interaction range, v_max × 0.25 s) + H`, fed by the same
serialize-once writer (audiences `all+server`) at 20 Hz within M/2 of the boundary, else 5 Hz. Gameplay gets
`GhostRef` (no mutators); `World::mutate()` asserts ownership; the only way to change another owner's entity
is **`send_effect()`** (R04 §9). Effects (`ApplyDamage`, `RequestDock`, `TractorAttach`, …) carry `{src AG, src epoch, dst AG, per-pair seq, tick, idempotency id}`, are
ordered per (src, dst), applied at the owner's next tick, de-duplicated for 10 s, and forwarded by an old owner
during the grace window. **Co-location:** tightly coupled interactions (predicted contact within 2 ticks,
docking, tractor, boarding, melee) hand the cheaper AG to the other cell regardless of boundary, under a
≥ 10 s lease. In space, v2 partitions by **interaction clusters** (k-d median splits), not uniform grids
(R07 §2.3).

### 6.3 Handoff protocol (v1)

A owns x at epoch e; B holds a ghost. Trigger: x's root is **H** past the boundary for 3 ticks with no
co-location lease (H = max(10 % cell size, 50 m ground / 2 km space)).

```
A (owner, e)                                        B (ghost)                          Fence / Gateway
T  end of tick: stop simulating x; buffer its inputs/effects
   HandoffOffer{x, e→e+1, T, server-audience state of all AG members, last
     input seqs, buffered inputs, effect seqs, relevance registry}  ──►  validate (ghost epoch == e);
                                                                 tentative promote ◄── HandoffAccept
   Fence.Advance(x, e, e+1, owner=B) ─────────────────────────────────────────────► CAS ok
   demote x to ghost; forward buffered inputs/effects; HandoffCommit ──► commit: simulate T+1,
                                                                        replicate, persist
   RouteUpdate{sessions controlling x → B, e+1} ─────────────────────────────────► gateway re-routes input
T+ grace 500 ms: A forwards late inputs/effects (B de-dups by input seq)
```

B neither replicates nor persists x before Commit; registry clients get a full-state update from B (not a
create), so nothing pops; input sequences continue, so prediction is unaffected. Offer → B's first
authoritative tick ≤ 1 tick + fence CAS: **p99 < 100 ms at 20 Hz** (Phase 3), < 35 ms at 60 Hz, < 50 ms in v2.
**Abort:** B NACKs (epoch, overload, decode), no Accept within 250 ms, or CAS fails → A re-promotes x at
**epoch e+2** (`Fence.Advance(e→e+2)`), catches up from buffered inputs, sends `HandoffCancel`; B's tentative
copy is fenced out. If B times out awaiting Commit it reads the fence: owner = B at e+1 (A died after the CAS)
→ self-commit; otherwise drop. Retries back off 1/2/4 s, then alert.

### 6.4 Crash recovery

**Detection:** lease TTL (3 s); gateways mark a cell *suspect* after 500 ms of trunk silence and freeze its
sessions without disconnecting; dumps go to sentry-native (ADR-013). **Warm standby** (Phase 2; preloaded Agones
Ready processes, one per 8 cells per class): (1) orchestrator bumps the region lease generation and assigns
standby S; (2) S loads checkpoints for AGs fenced to the dead cell and CAS-advances each to `epoch+1`, skipping
AGs a committed handoff already moved; (3) value needs no rebuild (Ledger-owned); activity progress rehydrates
from the activity service (R05-P0-5); (4) S publishes `RouteUpdate`s, gateways re-attach, S sends full resyncs;
input sequences continue. Target: **≤ 10 s hitch, ≥ 99 % sessions retained, loss ≤ checkpoint window** (30 s
Phase 3, 10 s Phase 5). **Hot standby** (Phase 4, hot zones) streams dirty AG state at 2 Hz: RPO ≤ 1 s.
**Gateway crash:** reconnect ticket → new token → another gateway in 1–3 s; avatar linkdead meanwhile.

### 6.5 Staging (ADR-007)

| Stage | Phase | Authority | Gateway |
|---|---|---|---|
| **v0** | 1–2 | One cell per zone instance; AG/epoch/fence/handoff/ghost APIs exist, exercised by zone transitions | Forwards chunks 1:1 |
| **v1** | 3 | Static multi-cell zones, ghosts, effects, co-location, handoff p99 < 100 ms, zone migration | Merges multi-cell streams |
| **v2** | 5 | Dynamic split (> 70 % budget for N s) / merge (< 30 % for minutes), minimum cell size, cluster partitioning, handoff < 50 ms | **Replication layer:** cells publish field chunks once per tick; gateways own interest, priority and ghost records, so cell crashes and handoffs become invisible |

R04-P0-7's "replication layer from day one" is met by the v0 gateway seam; the stateful tier waits for v2
(ADR-007).

## 7. Zones, instances, layers and phasing

A **Zone** is a template (content, frame, profile); a **ZoneInstance** is a runtime copy owned by one or more
cells. Kinds: *world*, *overflow layer*, *activity* (raid/dungeon/arena; checkpoints and lockouts in the
activity service), *phase instance* (personal/group story), *housing/ship interior* (persistent, per owner),
*edit* (ADR-009; the only kind that persists authored changes).

**Lifecycle:** `CreateInstance{template, content_version, profile, cap, owner/group, idle_ttl}` bin-packs onto
a process with the template cached (**ready ≤ 2 s** for small instances, R03-P0-6); idle 5 min → checkpoint if
persistent, destroy.

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
(R03-P0-3); placement matches the token's client content version. Rollouts start new instances on N+1 while
old ones drain. Schemas are append-only (field IDs, defaults), so N and N+1 cells exchange handoff blobs
during rolling restarts (R07-P1-18).

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
zone onto a reinforced host in advance (R01-P1-14, R07-P2-25).

## 9. Security and anti-cheat

- **Authority:** the server owns movement results, hits, damage, cooldowns, loot, inventory and currency (R05-P0-11); nothing of value is client-authoritative.
- **Transport:** netcode AEAD on every post-handshake packet, stateless challenge, token-reuse check, request larger than replies; L0 pre-filter drops wrong-size/type datagrams and limits requests to 10/s per IP before netcode sees them; ≤ 120 pps (240 burst) and 64 kbit/s up per session.
- **Messages:** schema-declared **token bucket per RPC**; generated bounds-checked decoders (`BitReader` returns errors, never asserts); enum/array ranges from schema; NaN/Inf rejected; 3 malformed messages → disconnect + flag.
- **Semantics:** handle relevance (cannot target what you were never sent), range/LOS, server-side ability SM cooldowns, movement budget (§5.5), fire-rate/damage bounds, idempotent ledger operations.
- **Information hiding** (the only robust anti-ESP): interest management and the `server` audience keep secrets server-side; cloaked entities are not replicated; per-interior PVS culling of enemies in FPS instances (Phase 4).
- **Telemetry:** budget violations, strikes, snaps, aim statistics and economy anomalies → NATS → Trust service (05); tick recordings (§10) for review.
- **Fuzzing:** libFuzzer (clang; MSVC `/fsanitize=fuzzer`) on vendored netcode/reliable read paths, the channel parser and every generated decoder, with recorded-session corpora; 1 h nightly per target; crashes block release.
- **DDoS:** only gateways are public (anycast behind scrubbing, IP rotation); cells and services have no public IPs; pre-auth XDP/eBPF fast path drops bad prefix/size/version and rate-limits per IP (Phase 4).
- **Windows client anti-tamper:** code-signed binaries (ADR-010), Ed25519-signed pak manifests (Monocypher), an `IAntiCheatProvider` seam plus attestation hash in token user data; vendor (EAC/BattlEye class) chosen in Phase 4, never *depended* on. External security review before Phase 4 launch.

## 10. Testing and tooling

- **NetSim** at L0 (client and server): latency + jitter, Bernoulli and Gilbert–Elliott burst loss, duplication, reordering, bandwidth cap. Profiles `lan`, `good` (40 ms/0.1 %), `mobile` (120 ± 30 ms/2 %), `awful` (250 ms/5 %) via `--netsim=` and the PIE toolbar (R06-ENG-15).
- **Deterministic tick replay:** a zone records its initial checkpoint plus every inbound message (inputs, effects, service responses, RNG seeds, versions) as zstd logs (~2 MB/min at 50 players); `helios-cell --replay` compares per-tick state hashes. Requires fixed ticks, per-zone RNG streams, no wall-clock reads in gameplay (lint), stable job merge order, `JPH_CROSS_PLATFORM_DETERMINISTIC` (ADR-013). Same-platform replay in Phase 2; cross-compiler (Linux-record/Windows-replay) in Phase 3. Feeds crash repro, anti-cheat review and killcams (R05-P1-19).
- **Bots:** `helios-bot` is a headless thin client sharing net/prediction/decode code (collision/nav data only); Luau behaviours (login, fight, fly, dock, zone-hop, trade, boundary ping-pong); 500–2,000 bots per process; `helios-swarm` (Go) schedules pods. **16 bots per commit** (ADR-012), **1k-bot 1 h soak nightly**, **10k bots weekly** on staging with chaos: cell/gateway kills, loss, handoff torture (R05-P1-20, R06-ENG-27).
- **Metrics:** in-house `engine/telemetry` (Prometheus exposition + sampled OTLP/HTTP-JSON spans, no protobuf in C++): `cell_tick_ms{zone,stage}`, `cell_dilation`, `repl_bytes{class,component}`, `conn_budget_bps`, `conn_loss`, `handoff_latency_ms`, `fence_conflicts`, `gw_drops{reason}`, `overload_stage`; traces login → token → gateway → cell and handoff phases.
- **Packet inspector** (editor profiling tool): per-connection bandwidth by channel/class/component (ImPlot), priority tables, relevancy set in the viewport, schema-decoded message log, `.hnetcap` captures (decrypted at L1, dev builds only); schema-generated Wireshark dissector in Phase 3.

## 11. Layout, interfaces, ladder, acceptance, risks, traceability

### 11.1 Module and directory layout

```
engine/net/          helios::net (HEADLESS): sockets (win32/posix), netcode + reliable wrappers, channels, netsim
engine/replication/  descriptor runtime, shadow state, interest hash, prioritizer, ghost records, chunks
engine/netgame/      time sync, prediction/reconciliation, interpolation, lag-comp history, move validation, command nav
engine/authority/    AGs, epochs, GhostRef, effects, handoff FSM, cell mesh, fence client
engine/telemetry/    metrics, spans, cheat events
apps/cellserver/     helios-cell (ZoneHost, profiles, --replay, --embedded-gateway)
apps/gateway/        helios-gateway
tools/bots/  tools/devcluster/  tools/netinspect/  tools/fuzz/
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
};
}
namespace helios::authority {
class Authority {
public:
  template<class C> Mut<C> mutate(ecs::Entity);                   // asserts ownership
  void send_effect(AgId target, const EffectMsg&);                // ordered, idempotent
  HandoffTicket begin_handoff(AgId, CellId to);
};
struct IFence { virtual Future<FenceResult> advance(AgId, Epoch expect, Epoch next, CellId owner) = 0; };
class ZoneClock { public: Tick tick() const; float dilation() const; Duration wall_interval() const;
                  void on_tick_measured(Duration cpu); };         // TiDi controller
}
```

### 11.3 MVP → AAA feature ladder

| Area | Phase 0 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 |
|---|---|---|---|---|---|---|
| Transport | netcode+reliable, channels, fuzzers | Internet play, reconnect tickets | AIMD, pacing | Gateway instances, key rotation | XDP, DDoS drill, security review | QUIC/WebTransport (R07-P2-24) |
| Gateway | Forwarder | Tokens, routing, service RPC | Multi-gateway, limits | Multi-cell merge | 8k sessions, AC hook | Replication layer |
| Cell | Skeleton, ZoneClock | Job graph, Luau, 50 players | Multi-instance, 500/zone, TiDi | Multi-cell, zone migration | Offload workers, hot standby | Split/merge |
| Replication | Descriptors, full state | Interest, masks, priority | Budgets, frame quant, LOD, deltas | Phase/layer scope | Fleet aggregation, PVS | Gateway-side |
| Netcode | — | Prediction, interpolation, basic hitscan lag comp, ability prediction keys (06 §1.4) | Full lag comp, projectiles, predicted abilities at 500/zone | Command replication | 60 Hz tuning | Seamless planet↔space |
| Authority | AG/epoch/fence API | Transitions via handoff | Warm standby | v1 handoff, co-location | Hot standby | v2 < 50 ms |
| Instancing | — | — | Instance lifecycle, overflow layers | Activities (05 §1.12), phases, housing, pinning | Placement tuning | Cross-shard |
| Overload | Metrics | Tick budgets in CI | Degrade + TiDi | Admission, pre-provision | Offload | Split |
| Tooling | NetSim, unit tests | 16 bots, inspector | 1k nightly, replay | 10k, handoff torture, cross-compiler replay | Chaos drills | 100k simulation |

### 11.4 Acceptance criteria (CI or bot swarm)

| Phase | Criteria |
|---|---|
| 0 | Handshake in 1.5 RTT; loopback 100k pps/core without loss; Windows client ↔ Linux gateway interop; fuzzers 1 h clean; Go-minted tokens accepted by vendored netcode; empty-zone tick < 0.5 ms |
| 1 | 50 players + 1k NPCs in one system at 20 Hz: tick p99 ≤ 25 ms; downstream p95 ≤ 128 kbit/s; mispredictions < 1 % at 100 ms/1 %; zone transition ≤ 3 s, 0 reconnects |
| 2 | 500 bots/zone at 20 Hz: tick p99 ≤ 35 ms, ≤ 256 kbit/s steady; lag comp ≥ 98 % agreement with local-hit reference at ≤ 150 ms RTT; 1k soak 0 crashes; 10 min replay bit-exact; TiDi absorbs 3× overload without desync |
| 3 | Handoff p99 < 100 ms; 0 duplicated/lost AGs over 1M torture handoffs; 5k CCU shard; cell kill → ≤ 10 s hitch, ≥ 99 % sessions kept; instance ready ≤ 2 s |
| 4 | 20–50k CCU shard; 2,000 ships in one system at d = 0.1, module response < 1 s wall; battle downstream p95 ≤ 512 kbit/s (AAA-SRV-6, via command replication and fleet aggregation); 8k sessions per 8-core gateway; security review closed |
| 5 | Dynamic split/merge under a moving hotspot; handoff p99 < 50 ms; cell-crash hitch ≤ 1 s via gateway replication layer; 100k-bot shard simulation |

### 11.5 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Protocol/crypto flaw; no forward secrecy | Vendored netcode/reliable, no Helios crypto; Go↔netcode token interop tests; fuzzing; external review; X25519 re-key option |
| Split-authority gameplay bugs (SC, SpatialOS) | Type-enforced `GhostRef`; effects-only mutation; handoff exercised from Phase 1; torture bots |
| Big-battle fan-out | Budgets, LOD groups, command replication, volley aggregation, TiDi, early 1k-ship bots |
| netcode gateway scale (single-threaded, hundreds of slots) | Many instances per gateway; measure at Phase 2; patch or replace L1 behind `Endpoint` if needed |
| Cross-compiler determinism (nav, replay) | Fixed-point integrators; cross-compiler hash tests; periodic corrections as fallback |
| Gateway becomes a stateful bottleneck | Sessions sharded across netcode instances; horizontal scale; v2 layer only after measurement |
| Luau stalls ticks | Time slices, interrupts, heap caps, `ScriptState` rule, script-cost telemetry |
| Egress cost | Per-client budgets; bandwidth regression gates; hosting model in 05 |

### 11.6 Traceability

| Requirement | Section |
|---|---|
| R07-P0-1, R02-P0-3, R07-P1-20 | §1, §2, §9 |
| R07-P0-4, R02-P0-2 | §1, §6.5, §7 |
| R07-P0-5, R02-P0-4, R06-ENG-18, R04-P0-6, R05-P1-13, R01-P0-6 | §4 |
| R07-P0-6, R05-P0-3, R05-P0-11 | §5.1–5.5, §9 |
| R05-P0-4, R05-P0-9, R07-P1-16 | §5.6, §3.4 |
| R05-P1-12, R01-P1-10, R04-P1-15 | §5.7, §5.4 |
| R07-P1-13, R04-P1-11, R02-P0-1, R04-P0-4, R04-P0-7 | §6 |
| R07-P2-21, R07-P2-22, R04-P2-19 | §6.5, §8 |
| R01-P0-1/3/4/5, R04-P1-16 | §3 |
| R07-P1-14, R01-P1-14, R07-P2-25, R03-P0-10 | §8 |
| R03-P0-3/6/8, R05-P0-6, R07-P1-18 | §7 |
| R07-P0-10, R05-P1-19/20, R06-ENG-02/15/27, R01-P2-22 | §10 |
