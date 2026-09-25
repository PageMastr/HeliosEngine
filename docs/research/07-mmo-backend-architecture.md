# 07 — MMO Backend & Network Architecture (State of the Art + Recommendation)

> Research report for the sci-fi MMO engine program (engine + editor + client + launcher + MMO backend).
> Scope: world partitioning, replication/netcode, backend services, persistence, infrastructure, patching.
> Tentative stack under review: C++20 engine/cell servers (Vulkan 1.3, Jolt, LuaJIT), Go microservices, PostgreSQL, Redis, NATS.
> Note on method: direct page fetches were blocked for most domains in this research environment; many facts come from search-result
> summaries of the cited pages, from GitHub-hosted primary sources (fetched in full), and from well-established industry practice.
> Items marked *(background)* are from general domain knowledge and should be validated before being relied on for a hard decision.

---

## 1. Overview

**Bottom line.** No shipped game has yet delivered a fully general "seamless, dynamically meshed, 100k-player single world" at acceptable cost. The architectures that have *actually worked at scale for years* are (a) **discrete zones on separate processes behind session proxies** (EVE Online's SOL/PROXY cluster, Albion Online's zones, SWG's planets), plus (b) **elastic instancing** of those zones (WoW sharding/layering/CRZ, GW2 and ESO megaservers). The newer generation — Star Citizen's replication layer + server meshing, Ashes of Creation's dynamic gridding, New World's hub grid — adds **multi-server authority inside a zone**, and it is where the frontier is. The industrial cautionary tale is SpatialOS: a generic, middleware-first distributed-simulation layer that proved expensive to operate and hard to ship on; its Unreal GDK repository was archived on 29 Aug 2024.

**Our recommendation, in one paragraph.** Build a **staged cell architecture**: every connection terminates at a **C++ Gateway/Edge tier** (encrypted UDP, netcode.io-style connect tokens) that owns the client session and routes to **Cell servers** (C++ simulation processes). The world is a hierarchy **Shard → Zone (star system / planet / station / instance) → Cell (spatial region)**. At launch (P0) one Zone = one Cell with explicit transitions (hyperspace/loading), EVE-style. P1 adds multiple static cells per zone with **ghost entities and epoch-fenced authority handoff** (BigWorld/Ashes-style). P2 adds **dynamic split/merge** and an optional **replication-layer** role in the gateway (Star Citizen-style) so that simulation crashes don't disconnect players. Go microservices handle everything non-spatial (auth, login queue, characters, **item ledger**, market, chat, social, guilds, entitlements, telemetry, GM tools). PostgreSQL is the system of record; Redis/Valkey is cache/ephemeral; **NATS (+JetStream)** is the control/event bus; gRPC for service RPC. Kubernetes + **Agones** manage cell/instance fleets. Patching uses **content-defined chunking** (Riot/casync-style) served from a CDN.

**Top findings**
1. Persistent-world winners decouple *client connection* from *simulation process*: EVE's PROXY vs SOL nodes, Star Citizen's replication layer, BigWorld/KBEngine BaseApp vs CellApp. This is the single most important structural decision.
2. Spatial partitioning fails in the case that matters most for a sci-fi MMO — the giant battle where everyone interacts with everyone. The shipped mitigations are **time dilation** (EVE), **functional offloading** (EVE character nodes, Albion thread pools), **pre-allocated reinforced hardware**, and **admission caps/instancing**. Plan for all four.
3. SpatialOS's failure was economic and organisational as much as technical: third-party runtime cost, per-entity cross-worker chatter, engine-vendor dependency (the 2019 Unity ToS dispute), and flagship games that were cancelled or shut down.
4. Replication should follow the Tribes/Fiedler/Quake 3 lineage: per-client **interest set → priority accumulator → bandwidth budget → delta against acked baseline → quantization**. Unreal's Iris is the modern reference for how to structure this (filtering, prioritization, a separate copy of replicated state).
5. Item duplication is a *distributed-systems* bug, not a gameplay bug: fix it with single-owner leases + fencing epochs, an append-only **item ledger** with unique item IDs, atomic multi-party transactions, and idempotency keys.
6. Use **netcode.io-style connect tokens** (HTTPS-issued, AEAD-encrypted, short-lived) so game servers never talk to the auth DB, and hide cell servers behind the gateway tier for DDoS protection.
7. Content-defined chunking delivered a **~10× patch speed-up** for League of Legends; the pack-file format must be designed CDC-friendly from day one.
8. For always-on MMO servers, **egress bandwidth and 24/7 compute** dominate cost; a hybrid bare-metal baseline + cloud burst model deserves evaluation before committing to all-cloud.

---

## 2. World Partitioning Approaches

### 2.1 Taxonomy

| Model | How it works | Examples | Pros | Cons |
|---|---|---|---|---|
| **Separate worlds/realms** | N independent copies of the whole game; players pick one | OSRS worlds, WoW realms, SWG galaxies *(background)* | Simplest; failure isolation | Splits community; population imbalance |
| **Zones on processes** | World cut into discrete regions, each a process; transitions (loading/jump) move players between processes | EVE solar systems; Albion zones; SWG planets | Clean authority; easy capacity planning; proven for 20+ years | Visible seams; one hot zone can't use more than one process |
| **Elastic instancing of zones** | Spawn extra copies of a zone as population demands; merge when quiet | WoW sharding, layering, CRZ; GW2/ESO megaserver; Ashes "channels" | Absorbs launch spikes; fills empty zones | Breaks "one world" feel; friends split across copies |
| **Static multi-server grid** | Fixed spatial grid; each cell on a server; entities cross with authority handoff | New World hubs; Ashes (initial); Star Citizen static meshing | Seamless; larger zones | Boundary complexity; hot cells still single-process |
| **Dynamic spatial meshing** | Cells split/merge by load; ghosts across boundaries | Ashes dynamic gridding; Star Citizen dynamic meshing (planned); Hadean Aether; SpatialOS load balancer | Adapts to hotspots | Hardest to build/debug; crowded all-vs-all fights still don't partition well |
| **Functional decomposition** | Split by *system* rather than space (AI, physics, character calc, market) | EVE character nodes; SpatialOS "offloading"; Albion async pools | Scales CPU-heavy subsystems independent of geography | Only helps separable work |

### 2.2 Case studies

**EVE Online (single shard, 20+ years).** Each of 5,000+ solar systems is a process on one of hundreds of servers; quiet systems are packed together, hot ones get dedicated hardware. Players connect to **PROXY** nodes (session management, routing) which track the **SOL** node their character is on; a jump moves the player between SOL processes. Tranquility Tech IV assigns 170 nodes to general solar-system simulation; CCP also offloaded character computation to dedicated "character nodes". Under overload EVE applies **time dilation (TiDi)**, slowing the game clock (to as low as 10%) so the simulation stays correct *(background)*. Lesson: a single shard works if you accept seams, separate session from simulation, and have an explicit overload policy. (EVE Tech IV; High Scalability; Engadget; EVE "Character Nodes".)

**World of Warcraft.** *Sharding* spawns extra copies of crowded outdoor areas (invisible borders, separate NPCs/resources); *layering* (WoW Classic launch) applied this realm-wide, pinning players to a layer; *cross-realm zones* merge under-populated zones across realms. Lesson: elastic instancing is a **population-management tool** for both over- and under-crowding; design it in. (Warcraft Wiki; Wowhead; Blizzard CRZ patent.)

**Guild Wars 2 & ESO megaservers.** GW2 (April 2014) replaced overflows with equal-standing map copies created on demand, placing players by heuristics (party, guild, language, home world). ESO runs NA and EU megaservers with a custom UDP layer, parallel zone channels and persistent Cyrodiil "campaigns". Lesson: **placement heuristics** matter as much as capacity. (GW2 Wiki; UESP; ESO whitepaper thread.)

**Albion Online (single world, zoned).** Many game servers host zones; zone changes may reconnect the client to another server behind a short loading screen. Each cluster's game logic runs on **one thread** over an ordered event queue, with DB and pathfinding on async pools; Unity client, Photon networking, Cassandra. Lesson: single-threaded core per zone + async offload is simple and robust. (Photon blog; Salz Quo Vadis 2016; MMORPG.com.)

**RuneScape / OSRS.** Parallel worlds (~2,000 players each) with account-level login/friends/chat spanning all worlds *(background)*. Lesson: a shard-agnostic social layer makes separate worlds feel like one community.

**New World (AWS).** A grid overlays the map; seven stateless "hubs" each own two non-adjacent grid pieces, so walking moves players hub to hub. State is written to DynamoDB (≈800k writes per 30 s), so failed hubs are quickly replaced. Lesson: **high-volume write-behind + stateless sim hosts** make recovery practical. Launch worlds were still capped around 2,000–2,500 players *(background)* — design, not tech, set the cap. (Hegazy, "New World Game Architecture".)

**SpatialOS (Improbable) — and why it faded.** Entities are bags of components; "workers" gain **authority** over components and declare **interest** queries; a separate runtime brokers state between workers and load-balances regions. Problems: all cross-worker interaction went through that runtime; operating costs stacked license + cloud fees; the January 2019 Unity ToS dispute put live games in legal limbo (Improbable and Epic created a $25M fund for affected developers); flagship titles — Worlds Adrift (shut 2019), Mavericks: Proving Grounds (cancelled 2019), Scavengers (console cancelled, studio sold 2022) — did not survive. The Unreal GDK was archived on 29 Aug 2024. **Lessons:** own the core partitioning/networking code; cross-process chatter is the real cost; build the smallest architecture the design needs, then grow it. (Improbable IMS; TechCrunch; PC Gamer; TheGamer; mein-mmo; GitHub.)

**Hadean Aether Engine.** Distributed spatial simulation that partitions space across cores/machines and rebalances dynamically; EVE: Aether Wars (GDC 2019) totalled 3,852 human and 10,422 AI pilots (peak ~2,379 concurrent humans). Proven for tech demos, not yet a production MMO backend. Counterpoint (Metagravity): when interaction density rather than area drives load, partition by **interaction clusters**, not space. (Microsoft Game Dev; GamesRadar; Metagravity.)

**Star Citizen (CIG).** Moves entity state out of the dedicated game servers (DGS) into a **Replication Layer** that both clients and servers connect to; it synchronises servers and streams state to clients. **Persistent Entity Streaming** keeps entity state in a graph database across restarts *(background: Alpha 3.18, 2023)*. **Static server meshing** went live in Alpha 4.0 (Dec 2024, ~500 players/shard, several DGS per shard on fixed locations); **dynamic meshing** will subdivide dense areas on demand. Lesson: a replication layer buys crash resilience and seamless authority transfer at the cost of an extra hop and a new stateful tier. (Star Citizen Wiki; Server Meshing Q&A; Hangarbase.)

**Ashes of Creation (IntrepidNET, Unreal).** Server meshing spreads a realm's geography over multiple game-server "workers" with cross-server interaction; **dynamic gridding** splits/merges workers by density (5×10 km down to 500×500 m), targeting ~50k peak CCU per realm. Lesson: a dedicated team can build grid meshing in-house if boundaries and handoff are first-class. (Ashes Wiki; MMORPG.com.)

**Unreal Replication Graph → Iris.** The Replication Graph replaced per-actor relevancy polling with shared graph nodes (2D grid spatialisation, always-relevant lists) for Fortnite-scale actor counts *(background)*. **Iris** keeps its own copy of replicated state, tracks per-connection state, **filters** (owner/connection/group/spatial), **prioritises** with float priorities under bandwidth limits, and serialises — decoupled from gameplay code. It is the best public reference for layering our replication subsystem. (Epic Iris docs.)

**BigWorld / KBEngine lineage.** "Multi-process distributed dynamic load balancing": LoginApp → **BaseApp** (client proxy + non-spatial persistent state) and **CellApp** (spatial simulation with ghost entities across borders), plus managers and a DB manager *(component details: background)*. This base/cell split is the closest published analogue of our Gateway + Cell design. (GitHub kbengine.)

**Destiny (Bungie).** Hybrid "shared world": Bungie-hosted activity/mission logic, peer-hosted combat physics, matchmaking into shared "bubbles" *(background; GDC 2015 "Shared World Shooter")*. Lesson: support **per-instance-type tick rates and netcode profiles** rather than one global mode.

### 2.3 What this means for a sci-fi MMO

Sci-fi worlds come with **natural seams**: star systems, planetary surfaces, orbits, stations, ship interiors, instanced missions. Hyperspace/jump transitions are free loading screens. So the EVE/SWG zone model covers most of the world at P0, and meshing is needed only for **hot zones** (trade hubs, capital cities, stargates, planned battles) and for **seamless planet-to-orbit** play. Space combat at 100+ km weapon ranges defeats small uniform spatial cells: in space, partition by **interaction clusters** (k-d/k-means over entity positions) with large ghost margins, and fall back to **time dilation** plus **functional offload** when a fight can't be partitioned.

---

## 3. Replication & Netcode

### 3.1 Transport and security

- **Why UDP.** TCP's reliable ordered stream causes head-of-line blocking: a lost packet stalls newer, more relevant state behind its retransmission. The standard answer is UDP with per-packet sequence numbers and **redundant acks** (latest ack + 32-bit ack bitfield), RTT smoothing, and simple congestion avoidance (e.g., 30 ↔ 10 packets/s depending on conditions). (Fiedler, "Reliability, Ordering and Congestion Avoidance over UDP".)
- **Library options (C++).** **Valve GameNetworkingSockets** (BSD-3): reliable + unreliable messages over UDP, fragmentation, ack-vector reliability, weighted-fair-queued lanes, **AES-GCM-256** encryption with **Curve25519** key exchange, P2P via ICE; production-proven on Steam. **yojimbo/netcode/reliable** (BSD-3): connect tokens, encrypted packets, reliable + unreliable channels — but aimed at "100 players or less" and single-threaded. **ENet**: simple reliable UDP without encryption *(background)*. **QUIC** (msquic/quiche/ngtcp2) with datagrams: TLS 1.3 built in and the only browser path, via **WebTransport** (unreliable datagrams + independent reliable streams; simpler than WebRTC for client–server).
- **Connection auth: netcode.io connect tokens.** The client authenticates over HTTPS and receives a short-lived **connect token** whose 1024-byte private part (client ID, timeout, server addresses, session keys) is encrypted with **XChaCha20-Poly1305** under a key shared only by backend and servers. The server validates it without a DB call, runs challenge/response to defeat IP spoofing and amplification, then uses ChaCha20-Poly1305 per packet (64-bit sequence as nonce, 256-entry replay window) and rejects tokens issued before its own start. Exactly the right shape for MMO gateways; DTLS is a heavier alternative.

**Recommendation:** wrap the transport behind an engine interface. **Default: GameNetworkingSockets** (or a thin in-house protocol on the netcode/reliable design) for native clients, with **netcode.io-style connect tokens issued by our Session service**. Keep a **QUIC/WebTransport** adapter on the roadmap (P2) for web companion apps/spectators and for networks that block raw UDP.

### 3.2 Interest management (AOI)

- **Spatial layer:** uniform **spatial hash** for ground/interiors (cell size ≈ typical relevance radius / 2); **hierarchical grid or loose octree** for space, where relevance ranges vary by orders of magnitude (capital ship visible at 250 km; drone at 10 km; loot crate at 50 m). Each entity type declares a **relevance class** (radius + max update rate).
- **Non-spatial layer:** always-relevant for self, owned entities (ship, drones, pets), party/fleet members (reduced fields), current target, zone-global state (weather, sovereignty, event timers). Mirrors Iris's owner/connection/group filters and the Replication Graph's always-relevant nodes.
- **Hysteresis:** enter radius < leave radius to avoid churn at the edge.
- **Cost control:** compute interest per *grid cell* and share across connections in that cell (Replication-Graph idea), not per-connection brute force; update interest sets at 2–5 Hz, not every tick.

### 3.3 Prioritization and bandwidth budgets

Adopt the **priority accumulator** (Tribes → Fiedler): each tick, every entity in a connection's interest set adds its current priority (f(distance, screen size, relationship, recent change, gameplay importance)) to an accumulator; sort; serialise from the top until the **per-connection packet/bandwidth budget** is full; reset accumulators only for entities that were sent. Unsent entities rise to the top next tick. The budget can be tuned on the fly per connection. (Fiedler, "State Synchronization", which uses a 256 kbit/s example budget.)

**Budgets (starting targets, to validate in load tests):**
- Downstream: 128–256 kbit/s steady, burst to 512–1,000 kbit/s in large battles; payload ≤ 1,200 bytes per datagram (safe for IPv6/QUIC paths).
- Upstream: input at 30 Hz with the last 3–4 inputs redundantly included; ≤ 32–64 kbit/s.
- Server egress per cell must also be budgeted: 500 clients × 256 kbit/s ≈ 128 Mbit/s per cell.

### 3.4 Serialization: deltas, baselines, quantization

- **Delta against acked baselines (Quake 3 model):** encode each entity relative to the last state *the client has acknowledged*; the client continually reports its latest received snapshot and the server advances the baseline. Unchanged entities cost ~1 bit. For entity-granular prioritised updates, keep **per-entity baselines** per connection. (Fiedler "Snapshot Compression"; Quake 3 source review.)
- **Quantization:** Fiedler's example goes from 17.37 Mbit/s to ~256 kbit/s via bounded position quantization (512 values/m), **smallest-three quaternions** (29 bits instead of 128), at-rest flags, relative index encoding and per-field change flags. Physics state sync needs finer precision (e.g., 4,096 values/m, 15-bit quaternion components) and should **quantize on both sides** so extrapolation matches.
- **Large-world coordinates (sci-fi specific):** servers simulate in **double precision** (or 64-bit fixed point) with a sector/grid origin; the wire format sends **sector ID + quantized sector-relative position**; clients render with a floating origin. Planet-scale and system-scale (AU) ranges are impossible in 32-bit floats.
- **Schema:** generate serializers from an IDL (component schema) shared by C++ engine, gateway and Go tooling; version every schema so that N and N+1 servers can interoperate during rolling updates.

### 3.5 Prediction, reconciliation and server-authoritative movement

- **Client prediction + server reconciliation:** the client applies inputs immediately, keeps a ring buffer of (input, predicted state), and on each authoritative update rewinds to the server state and replays unacknowledged inputs. The server stays fully authoritative. (Fiedler, "What Every Programmer Needs To Know About Game Networking".)
- **Remote entities:** **snapshot interpolation** with an interpolation delay sized to survive loss/jitter (rule of thumb: ~3× send interval + jitter margin; ~100 ms at 20–30 Hz), Hermite for position, slerp for orientation; extrapolation only for short gaps. (Fiedler "Snapshot Interpolation"; Valve Source networking uses 100 ms by default *(background)*.)
- **Physics objects/ships:** server-authoritative Jolt simulation; clients predict only their own ship/vehicle; use jitter buffers (4–5 frames at 60 Hz) and adaptive visual smoothing for corrections (Fiedler "State Synchronization").
- **Anti-speedhack:** with input-driven authoritative movement, speedhacks reduce to **input-rate abuse**: enforce a per-connection **movement time budget** (the sum of claimed input durations may not exceed wall-clock time + small tolerance), clamp per-input delta time, and reject/flag bursts. If any movement is client-authoritative (e.g., for cost on low-stakes walking), validate max speed/acceleration, collision (navmesh/physics raycasts), and teleport distance server-side, and flag rather than silently snap to build evidence.

### 3.6 Lag compensation

For hitscan/fast projectiles in shooter-style activities: keep a ~1 s ring buffer of hitbox/transform history per tick, and resolve shots at *server time − client latency − client interpolation delay* (Valve Source model). **Cap rewind** (e.g., 200–250 ms) to limit high-ping abuse and "shot behind cover" complaints. For MMO-style targeted abilities, use generous range/LOS tolerances instead of rewinds. References: Overwatch GDC 2017 netcode talk; Bungie's "I Shot You First" (Halo: Reach).

### 3.7 Tick-rate profiles (per zone/instance type)

| Profile | Sim tick | Snapshot rate (near/far) | Use |
|---|---|---|---|
| Open world ground / stations | 20 Hz | 20 / 5 Hz | Social hubs, questing |
| Space (open) | 20 Hz (physics sub-step 60 Hz) | 20 / 2–5 Hz | Travel, mining, skirmish |
| Large battle (overload) | 20 Hz × TiDi factor | budget-driven | Fleet battles, sieges |
| Instanced FPS/raid | 30–60 Hz | 30–60 Hz | Destiny-like activities, PvP arenas |

---

## 4. Services Catalogue

Each service owns its data (no shared tables across services). Go unless noted.

| Service | Responsibilities | Store | Interfaces |
|---|---|---|---|
| **Identity/Auth** | Accounts, OAuth2/OIDC login (own IdP + Steam/Epic/console/Google federation), MFA, bans, short-lived JWT access tokens (5–15 min) + rotating refresh tokens | Postgres (global), Redis (revocation list) | HTTPS/OIDC |
| **Login Queue / Admission** | Per-shard capacity, login-rate throttling to protect DBs, queue position (Redis sorted set), priority lanes (reconnect grace, subscribers), signed queue tickets | Redis | HTTPS + SSE/long-poll |
| **Session & Connect-Token** | Issues netcode-style connect tokens with gateway addresses, session keys, character ID, client version gate | Redis (sessions) | HTTPS |
| **Gateway/Edge** *(C++)* | UDP/QUIC termination, decryption, DDoS filtering, per-message-type rate limits, client↔cell routing, handoff re-routing, (P2) per-client replication merge | In-memory; session in Redis | Custom UDP; internal reliable links to cells |
| **World Directory / Orchestrator** | Zone/cell topology, cell-to-process assignment, authority leases + epochs, split/merge decisions, instance creation, Agones allocation, TiDi policy | etcd or NATS KV (leases) + Postgres (topology) | gRPC + NATS |
| **Cell servers** *(C++)* | Authoritative simulation for a region: Jolt physics, AI, LuaJIT gameplay, interest, replication, ghosts, handoff | Memory; checkpoints via Persistence | Gateway links, cell mesh links, NATS control |
| **Persistence (World State Writer)** | Batched write-behind of entity checkpoints; fencing by authority epoch; restore on crash | Postgres (+ Redis/object storage for hot snapshots) | gRPC/NATS JetStream |
| **Character** | Character CRUD, appearance, location, skills/progression snapshot, character select | Postgres | gRPC |
| **Item Ledger / Inventory** | Unique item IDs, ownership, stacks, containers, append-only item events, atomic trades/escrow, idempotent grants, GM restore | Postgres (ledger + materialized inventory) | gRPC; JetStream events |
| **Wallet / Currency** | Double-entry ledger for in-game currency and premium currency; escrow for market/contracts | Postgres | gRPC |
| **Market / Economy** | EVE-style regional order books (price-time priority, range rules), matching engine as a single-writer actor per (region, item type), contracts, fees, price history | Postgres; Redis for order-book cache | gRPC; JetStream trade events |
| **Industry/Timers** | Long-running jobs (crafting, research, skill training), durable timers | Postgres | gRPC; JetStream |
| **Mail** | Player/system mail with attachments (attachments moved via Item Ledger escrow) | Postgres (Scylla at P2 if volume demands) | gRPC |
| **Chat** | Channels (local, system, fleet, corp, alliance, whisper, custom), history, moderation hooks, spam limits, profanity/abuse filter; fan-out over NATS subjects | Postgres/Scylla (history), Redis (membership) | Via gateway (in game) + WebSocket (companion) |
| **Social & Presence** | Friends, blocks, presence (Redis TTL heartbeats + NATS pub), cross-shard | Postgres, Redis | gRPC, NATS |
| **Guild/Corp/Alliance** | Membership, roles & permission bitsets, corp wallets and hangars (via ledgers), structures, diplomacy | Postgres | gRPC |
| **Group/Party & Matchmaking** | Parties/fleets, activity finder, instanced content, PvP matchmaking (custom or Open Match) | Redis, Postgres | gRPC |
| **Leaderboards** | Seasonal/global boards | Redis sorted sets + Postgres snapshots | gRPC |
| **Entitlements/Store/Payments** | Platform receipt validation, idempotent grants, refunds/chargebacks, SKU catalog | Postgres (separate, PCI scope minimised — use a payment provider) | HTTPS webhooks, gRPC |
| **Live Config & Content** | Versioned data tables, feature flags, live tuning, kill switches | Postgres + object storage | gRPC/NATS push |
| **Telemetry & Analytics** | Game events pipeline, economy metrics, funnels | NATS JetStream → Kafka/Redpanda (P1) → ClickHouse/data lake | OTLP / event SDK |
| **GM/Admin & Support** | RBAC'd tools: account actions, item restore from ledger, teleport/kick/mute, audit log, replay of item history | Reads all via APIs; own audit DB | Web UI + gRPC |
| **Anti-cheat & Trust** | Server-side validation signals, movement budgets, economy anomaly detection (dupe/RMT), report intake, client anti-tamper vendor integration | ClickHouse, Postgres | NATS events |
| **Notification** | Push/email/in-game notices | Postgres | NATS |
| **Patch/Manifest** | Build manifests, channel/branch pointers, CDN URLs, signatures | Object storage + CDN | HTTPS |

**Notes on selected services**

- **Login queues.** Queue *before* touching the character DB. Admission = min(shard headroom, DB login-rate budget); return a signed ticket with position; reconnects within a 2–5 min grace window bypass the queue.
- **Item integrity (the dupe problem).** Dupes come from four sources: (1) two servers believing they own an item (race during handoff/crash) → **single owner + lease epoch fencing**; (2) crash between "take" and "give" → **one atomic DB transaction** for multi-party moves, or escrow + saga with idempotency keys; (3) partial rollback (one side's checkpoint restored, the other's not) → value-bearing moves are **synchronous ledger writes**, never write-behind; (4) replayed/duplicated requests → **idempotency keys** (request UUID + unique constraint). Stackables need split/merge events with conserved quantities; add a periodic **conservation audit** (sum of currency/items created − destroyed = circulating) to catch economy exploits.
- **Event sourcing** is justified for the **item and currency ledgers** (auditing, GM restore, anti-RMT forensics), not for all world state.
- **Market.** EVE-style: region-scoped order books; buy orders carry a range (station/system/N jumps/region); matching is price-time priority with escrowed funds; settlement writes ledger entries atomically. A single-writer goroutine/actor per (region, type) serialises matching without distributed locks; persist the order before acknowledging.
- **Chat.** XMPP brings federation features we don't need; a custom protocol over the gateway plus NATS subject fan-out is simpler (Nakama likewise ships its own chat). Per-channel history with retention; moderation via the Trust service.
- **Rate limiting** at three layers: gateway (token buckets per message type), API edge (per account/IP), domain (chat per channel, market orders/min, mail/hour).

**Build vs buy.** Vendor backends (Nakama — auth, storage, social, chat, groups, leaderboards, tournaments, parties, purchase validation, matchmaker; Go/Lua/TS runtime; Postgres/CockroachDB; Apache-2 — and commercial Pragma, AccelByte, Beamable, PlayFab; hosting via GameLift) are designed for **session-based games**. They don't provide authoritative spatial simulation, item ledgers with MMO-grade integrity, or EVE-style markets. Recommendation: **build core MMO services in-house**, study Nakama's API design, and consider buying only commodity pieces (payments provider, client anti-tamper, CDN, DDoS scrubbing, possibly social/voice).

---

## 5. Data & Persistence

**Stores**
- **PostgreSQL = system of record.** Accounts/entitlements in a global cluster; per-region shard clusters for characters, ledgers, market, guilds. Partition large tables by character/shard. HA via managed Postgres or an operator (e.g., CloudNativePG/Patroni) with synchronous replica in-region + PITR backups to object storage.
- **CockroachDB** (Postgres wire-compatible; Nakama supports it) is attractive for a *global* multi-region accounts/entitlements tier, but check licensing/cost (Cockroach moved to an enterprise-license model in 2024 *(background)*). Default: Postgres; revisit at P2.
- **Redis/Valkey** for sessions, presence, queues, rate-limit counters, leaderboards, order-book cache, hot snapshots. *Never* the source of truth for value. (Given Redis licensing changes since 2024, Valkey is a safe default *(background)*.)
- **ScyllaDB/Cassandra** only if chat/mail/event volume outgrows Postgres (Albion runs Cassandra). P2 candidate.
- **ClickHouse** (or BigQuery/Snowflake) for analytics; **object storage** for backups, crash dumps, replays, CDN origin.

**Write-behind persistence for world state (New World/Star Citizen pattern)**
- Cells keep hot state in memory with per-entity dirty flags. The Persistence service receives **batched checkpoints** (e.g., every 30–60 s per dirty entity, and immediately on logout/zone exit/handoff), writes them to Postgres, and rejects any write whose **authority epoch** is older than the current one (fencing).
- **Value-bearing transitions bypass write-behind**: loot of rare items, trades, market, mail attachments, crafting consumption, currency changes → synchronous calls to the Ledger/Wallet services. Consequence: a crash can roll back positions/XP by seconds but can never create or destroy items.
- **Crash recovery:** lease expiry (3–5 s) → orchestrator reassigns the region to a warm standby → it loads last checkpoints + ledger state → gateway re-subscribes clients. Players see a short hitch, not a disconnect.
- **Messaging:** gRPC for request/response; **NATS core** for fan-out (chat, presence, invalidations, control plane); **NATS JetStream** for durable events (ledger events, audit, notifications) — it provides file/memory streams replicated via Raft, pull consumers, a KV store, and exactly-once semantics via a `Nats-Msg-Id` dedup window + double-ack. **Kafka/Redpanda** is worth adding for the analytics firehose (P1): long retention and a larger connector ecosystem, heavier operations. The stack's NATS choice is sound.

---

## 6. Infrastructure & Ops

- **Kubernetes + Agones for cells/instances.** Agones adds GameServer/Fleet CRDs with health checks, fleet autoscaling tied to cluster autoscaling, and metrics; Allocated servers are protected from scale-down, and **Counters/Lists** or label-locking support high-density servers hosting many sessions — a fit for cells hosting many zones/instances. Agones is eventually consistent, so keep a **warm pool** of Ready cells for split/merge and failover.
- **Go services** run as ordinary Deployments with gRPC internally and an Envoy ingress for launcher/web APIs.
- **Managed alternatives** (AWS GameLift fleets/FleetIQ/Anywhere/FlexMatch, PlayFab Multiplayer Servers) suit **instanced** content but add vendor coupling for persistent worlds *(background)*.
- **Cost model — flag for leadership.** 100k CCU × 256 kbit/s ≈ 25.6 Gbit/s ≈ 8 PB/month of egress at peak rates. At typical public-cloud egress prices that is a six-figure monthly bill before compute. Evaluate a **hybrid**: bare-metal/colo baseline for cells + gateways (committed bandwidth), cloud for Go services, burst capacity and instances. EVE runs its own hardware; New World runs on AWS.
- **DDoS:** gateway IPs behind scrubbing/anycast (or relay networks similar to Steam Datagram Relay); cells never exposed publicly; connect-token challenge prevents amplification.
- **Observability:** OpenTelemetry SDKs in Go services and C++ servers (traces across login → token → gateway → cell), Prometheus metrics (tick time p50/p99, entities/cell, bytes/client, handoff latency, queue depth, ledger TPS), Grafana dashboards, structured logs (Loki/ELK), crash dumps to object storage with symbol server. Define SLOs: login p95 < 3 s, cell tick p99 < budget, handoff p99 < 100 ms, zero ledger invariant violations.
- **Deployments & live ops:** Go services blue/green or rolling with backward-compatible protobufs. Cells: (P0) scheduled daily/weekly downtime window, as EVE does; (P1) **rolling zone restarts** by migrating entities to new-version cells — which requires wire/schema compatibility N↔N+1 in the cell mesh; LuaJIT script hot-reload and data-driven config pushes for hotfixes; client version gating in connect tokens.
- **Environments:** dev → CI load-test shard (headless bot clients) → PTS (public test) → live shards. Invest early in a **headless bot swarm** (thousands of simulated clients) — every MMO meshing effort lives or dies by load testing.

---

## 7. Patching / CDN

- **Riot (League of Legends):** replaced RADS (binary deltas) with a **content-defined chunking** patcher based on **FastCDC** (rolling hash > 1 GB/s/core): split files into variable-size chunks, compare chunk lists between versions, download only missing chunks. Result: **~10× faster patching**. *(Background: manifests ("RMAN") map files → chunk IDs; chunks are grouped into bundles on the CDN and fetched with HTTP range requests.)*
- **casync/desync (open source, Go):** chunks addressed by hash in any static store (HTTP, S3, OCI); index files list a file's chunks; clients seed from local files and fetch only what's missing ("nothing on the server computes a delta"). Their example: 109.8 MB downloaded instead of 292.2 MB for a 3.2 GB image. `desync` is a direct starting point for our Go patch tooling.
- **Steam** depots use fixed ~1 MB chunks with manifests; **Blizzard** uses CASC (content-addressable local storage; CascLib reads it) with CDN delivery and "play while downloading" prioritisation in the Battle.net Agent *(background)*. **zsync/bsdiff** are older approaches: zsync = rsync over HTTP; bsdiff = per-file binary delta (good for executables, needs a delta per version pair).

**Recommendation.** Content-addressed, CDC chunk store (FastCDC, ~64 KB average chunk) on object storage + multi-CDN; per-build signed manifests (Ed25519) and a channel pointer (live/PTS/beta); launcher does chunk dedup against local install, parallel range requests, resumable downloads, background pre-download, and **priority tiers** (login area + core first, remote regions streamed later). **Engine requirement:** pack files must be CDC-friendly — compress per asset/block, not across the whole archive; keep ordering stable between builds; otherwise small content changes cascade into huge downloads. If we ship on Steam, Steam patches that SKU; our launcher covers standalone.

---

## 8. Recommended Backend Architecture

### 8.1 Diagram

```
                           ┌──────────────── Global tier (multi-region) ─────────────────┐
  Launcher / Client        │  Identity/Auth (OIDC)   Entitlements/Store   Patch/Manifest  │
   │  HTTPS (REST/gRPC-web)│  Postgres(global)       Payment provider     Obj store + CDN │
   ├──────────────────────►│                                                              │
   │                       └──────────────────────────────────────────────────────────────┘
   │ HTTPS                   ┌──────────────── Regional shard (e.g. EU-1) ────────────────────────────────┐
   ├────────────────────────►│ Login Queue ─► Session/Connect-Token svc (issues netcode-style token)        │
   │                         │                                                                              │
   │ Encrypted UDP (GNS/     │  ┌──────────── Edge ─────────────┐        ┌──── Go services (k8s) ─────────┐ │
   │ netcode) / QUIC (P2)    │  │ Gateway 1..N (C++)            │ gRPC   │ Character   Item Ledger  Wallet│ │
   └────────────────────────►│  │ decrypt, rate-limit, route,   │◄──────►│ Market      Industry     Mail  │ │
                             │  │ session keep-alive, (P2) per- │        │ Chat        Social/Presence    │ │
                             │  │ client replication merge      │        │ Guild/Corp  Party/Matchmaking  │ │
                             │  └──────┬───────────────▲────────┘        │ Leaderboards LiveConfig  GM/Adm│ │
                             │         │ internal       │                 │ Anti-cheat  Telemetry ingest   │ │
                             │         │ reliable links │                 └───────┬─────────────▲──────────┘ │
                             │  ┌──────▼───────────────┴──────────────────┐      │  NATS core  │ JetStream │
                             │  │ Cell servers (C++, Agones fleets)       │◄─────┴─────────────┴──────────►│ │
                             │  │  Zone: Sol-Prime                        │  control plane / events        │ │
                             │  │   [Cell A]◄ghosts►[Cell B]◄ghosts►[C]   │                                │ │
                             │  │  Zone: Planet-X surface  [Cell D]       │   World Directory/Orchestrator │ │
                             │  │  Instances: raid-123, arena-456         │   (leases+epochs, split/merge, │ │
                             │  └──────────┬──────────────────────────────┘    TiDi, Agones allocation)    │ │
                             │             │ batched checkpoints (epoch-fenced)                             │ │
                             │  ┌──────────▼──────────┐  ┌──────────────┐  ┌───────────────────────────┐  │ │
                             │  │ Persistence svc     │─►│ PostgreSQL   │  │ Redis/Valkey (sessions,   │  │ │
                             │  └─────────────────────┘  │ (shard HA)   │  │ presence, queues, caches) │  │ │
                             │                           └──────────────┘  └───────────────────────────┘  │ │
                             └──────────────────────────────────────────────────────────────────────────────┘
   Observability: OpenTelemetry → Prometheus/Grafana/Tempo/Loki; analytics: JetStream → Kafka/Redpanda (P1) → ClickHouse
```

### 8.2 The Shard → Zone → Cell model

- **Shard** = a regional deployment (EU-1, NA-1) with its own gateways, cells and shard DB; account-level services are global so friends/chat/guild span shards. At P2, a shard may be the whole world (EVE-style single shard) with regional gateway PoPs.
- **Zone** = a gameplay space with its own static data and coordinate frame (star system, planetary surface, station interior, ship interior, instance). Zone transitions use hyperspace/elevator/docking sequences that hide a reconnect-free **route change** at the gateway.
- **Cell** = a spatial region of a zone owned by exactly one cell process at a time. One process can host several small cells/zones (EVE-style packing); hot cells get dedicated cores/hosts.
- **Entity identity:** 64-bit EntityId (Snowflake-style: shard bits + time + sequence), stable across handoffs and persistence. Each entity has `owner_cell` and `authority_epoch`.

**Ghosts.** Each cell keeps read-only *ghost* copies of neighbours' entities within a margin `M ≥ max(interaction range for that entity class, v_max × handoff_latency)`. Ghosts receive owner updates at reduced rate and are used for interest management, targeting and rendering on clients near the boundary. In space, where weapon ranges are large, use **cluster-aware partitioning** (k-d splits at entity medians) so combatants tend to share a cell, and accept larger margins.

**Cross-boundary interactions.** Actions are resolved by the actor's owner cell using ghosts of targets; *effects* on other entities are sent as ordered messages to the target's owner cell (e.g., `ApplyDamage(target, amount, source, tick)`), which is authoritative for its own state. Tightly coupled physics (collisions between ships straddling a boundary) triggers **co-location**: the smaller interaction island migrates to the other cell for the duration.

**Authority handoff protocol (P1).**
```
Cell A (owner, epoch e)                      Cell B (holds ghost)                 Gateway / Directory
1. entity crosses boundary + hysteresis H
2. A freezes entity at tick T, sends HANDOFF(eid, e+1, full state@T,
   unacked input seq, script state, pending effects)  ──────────►
3.                                           B promotes ghost → real (epoch e+1),
                                             replies ACCEPT(eid, e+1)  ◄────
4. A demotes to ghost, sends ROUTE(eid → B, e+1) ────────────────────────────────►  Gateway re-routes client
5. For grace window (≈250 ms) A forwards any late inputs/effects tagged epoch e to B
6. Persistence/ledger reject writes for eid with epoch < e+1 (fencing)
Abort: no ACCEPT within timeout → A keeps authority and retries later; B discards promotion.
```
Client prediction is unaffected because the input sequence numbers continue across the handoff. Hysteresis H (e.g., 10–20% of cell size or a fixed distance) prevents ping-pong. Target: handoff p99 < 100 ms, invisible to the player.

**Dynamic split/merge (P2).** The Orchestrator watches per-cell tick time, entity count and bytes out. When a cell exceeds ~70% of its tick budget for N seconds, it chooses a split plane (median along the axis of greatest spread, or k-means on interaction clusters), allocates a pre-warmed cell from the Agones pool with the zone's static data already loaded, and bulk-hands-off entities on one side using the same protocol. Merge when two neighbours are both below ~30% for several minutes. Minimum cell size is enforced so a 1,000-ship brawl in one spot doesn't cause endless splitting.

**Overload policy (when spatial splitting can't help).** In order: (1) **functional offload** — move AI planning, pathfinding, projectile ballistics, skill/character calcs to helper workers; (2) **replication degradation** — lower far-entity rates, aggregate distant ships into fleet-level proxies; (3) **time dilation** per cell/zone (EVE TiDi) down to a floor (e.g., 10%) with UI indication; (4) **admission control** — queue entry to the system/grid, or spin up an instance copy for non-contested content; (5) **reinforced nodes** — let players/GMs pre-announce big fights so dedicated hardware is assigned ahead of time.

**Replication-layer evolution (P2).** Initially each cell serialises updates per client and the gateway only forwards. When a client's interest spans several cells, the gateway merges the streams. At P2, move per-client interest+priority+delta encoding into the gateway ("replication layer"): cells publish entity state changes once to the gateways that need them, and the gateway serialises per client. Benefits: fewer cell→client duplicates, clients never disconnect on cell crash/handoff, cells become more stateless. Costs: a stateful edge tier with its own scaling and extra latency (sub-millisecond in-DC). Keep the codec shared (C++) so this is a deployment choice, not a rewrite.

### 8.3 Scale targets (initial, to validate with bot swarms)

| Level | P0 (launch) | P1 | P2 (ambition) |
|---|---|---|---|
| Players per cell (20 Hz) | 250–500 | 500 | 500–1,000 (with gateway replication) |
| Replicated entities per cell | 20k | 50k | 100k |
| Players per zone | 500 (1 cell) | 2,000–5,000 (static multi-cell) | 10k+ (dynamic cells + TiDi) |
| CCU per regional shard | 5,000 | 20,000–50,000 | 100,000+ single shard with meshing |
| Login admission rate | 50/s per shard | 200/s | 500/s |
| Handoff latency p99 | n/a (zone transitions ≤ 3 s) | < 100 ms | < 50 ms |
| Client downstream steady / battle | 256 / 512 kbit/s | same | 256 / 1,000 kbit/s |
| Ledger throughput | 2k tx/s per shard | 10k tx/s | 50k tx/s (partitioned) |
| Checkpoint data-loss window on cell crash | ≤ 60 s (non-value state only) | ≤ 30 s | ≤ 10 s |

### 8.4 Failure handling

| Failure | Detection | Response | Player impact |
|---|---|---|---|
| Cell process crash | Lease/heartbeat miss (3–5 s) | Orchestrator assigns warm standby; load checkpoints + ledger; bump epoch; gateway re-subscribes | 3–10 s hitch; positions/XP roll back ≤ checkpoint window; items never lost/duped |
| Cell overload | Tick-time SLO breach | Offload → degrade replication → split → TiDi → admission control | Slower sim, no desync |
| Gateway crash | Client timeout / LB health check | Client reconnects with **reconnect token** to another gateway; character held in world for grace period | 1–3 s reconnect |
| Handoff failure | ACCEPT timeout | Abort; A retains authority; retry with backoff | None |
| Split brain (two owners) | Epoch conflict | Fencing: lower epoch's writes rejected; lower-epoch cell drops the entity | None; alert |
| Postgres primary failure | Operator/managed failover | Sync replica promoted; services retry idempotently | Seconds of 5xx on non-world actions; world continues on in-memory state |
| NATS node failure | Cluster | Raft-replicated JetStream streams (R3) continue | None |
| Redis failure | Sentinel/cluster | Sessions rebuilt from tokens; queues re-established; leaderboards rebuilt from Postgres | Short queue disruption |
| Bad deploy | SLO/crash spike | Canary halt + rollback; kill-switch flags in Live Config | Localised |
| Economy exploit | Conservation audit / anomaly alerts | Kill-switch the feature; ledger-based rollback of affected items; bans | Targeted |
| DDoS on edge | Traffic anomaly | Scrubbing/anycast; rotate gateway IPs; cells stay private | Possible reconnects |

### 8.5 Stack review (flags)

- **Agree:** C++20 cells, Go services, PostgreSQL, NATS (+JetStream), Redis-compatible cache, Kubernetes + Agones.
- **Flag 1 — gateway language:** put the Gateway/Edge in **C++** (or Rust), not Go. It is a packet-rate hot path that should share serialization/replication code with the engine; GC pauses on the per-packet path are avoidable risk.
- **Flag 2 — transport:** don't write crypto from scratch. Use GameNetworkingSockets or the netcode/reliable design plus libsodium; add QUIC/WebTransport later.
- **Flag 3 — Redis licensing:** prefer **Valkey** or confirm license terms *(background)*.
- **Flag 4 — analytics bus:** NATS is right for control/events; add Kafka/Redpanda for the analytics firehose rather than stretching JetStream.
- **Flag 5 — don't start with dynamic meshing.** Ship zone-per-cell first (EVE/Albion model) with the handoff/ghost design *interfaces* in place; SpatialOS is the warning about building the most general system first.
- **Flag 6 — hosting economics:** model egress before choosing all-cloud.

---

## 9. Requirements (prioritised)

### P0 — required for first playable online / closed alpha (5k CCU shard)
1. **Gateway/Edge (C++)**: encrypted UDP (AEAD), netcode.io-style connect tokens (≤ 30–45 s expiry, challenge/response, replay window), per-connection rate limits; cells never publicly addressable.
2. **Session + connect-token service** and **OIDC-based Identity** with short-lived JWTs, refresh rotation, bans, platform federation (Steam at minimum).
3. **Login queue/admission control** per shard with reconnect grace bypass.
4. **Zone-per-cell server model** with World Directory/Orchestrator, authority **leases + epochs**, zone transitions routed at the gateway (no client reconnect), Agones-managed fleets with warm pool.
5. **Replication pipeline**: interest management (spatial hash + always-relevant sets), priority accumulator with per-connection bandwidth budgets, delta compression against acked per-entity baselines, quantization incl. sector-relative large-world coordinates, schema-generated serializers with versioning.
6. **Server-authoritative movement** with client prediction/reconciliation, snapshot interpolation for remote entities, movement time-budget anti-speedhack checks.
7. **Item Ledger + Wallet** with unique item IDs, append-only events, atomic multi-party transactions, idempotency keys, epoch fencing, conservation audits; all value-bearing changes synchronous.
8. **Write-behind persistence service** for non-value world state with checkpoint + crash restore to warm standby.
9. **Character, Chat (local/system/party/guild/whisper), Friends/Presence, basic Guild** services.
10. **Observability baseline**: OpenTelemetry tracing, Prometheus metrics for tick time/bytes/queue/ledger, crash dumps, headless **bot-swarm load testing** in CI.
11. **CDC patcher**: content-addressed chunk store, signed manifests, launcher with resume/dedup; CDC-friendly pack-file format in the engine.
12. **GM/Admin tools**: kick/mute/ban, teleport, item restore from ledger, full audit log.

### P1 — beta / launch (20–50k CCU per shard)
13. **Static multi-cell zones** with ghosts, cross-boundary effect messages and the handoff protocol (p99 < 100 ms).
14. **Overload policy**: time dilation per cell, replication degradation, functional offload workers (AI/pathfinding/character calc).
15. **Market/economy**: regional order books, single-writer matching per (region, type), escrow, contracts, price history; mail with escrowed attachments; industry timers.
16. **Party/fleet + activity matchmaking** for instanced content with per-instance tick/netcode profiles (30–60 Hz, lag compensation with capped rewind).
17. **Entitlements/store/payments** with idempotent grants and chargeback handling.
18. **Live ops**: feature flags/kill switches, LuaJIT hot-reload, rolling zone restarts with N/N+1 protocol compatibility, canary deploys.
19. **Analytics pipeline** (JetStream → Kafka/Redpanda → ClickHouse), economy dashboards, anomaly detection for dupes/RMT.
20. **DDoS protection** and multi-CDN patch delivery with play-while-downloading priority tiers.

### P2 — ambition (100k+ single shard, seamless planet↔space)
21. **Dynamic split/merge** of cells (cluster-aware partitioning, warm pools, minimum cell size).
22. **Gateway replication layer** (per-client merge of multi-cell interest; cells publish once), making cell crashes and handoffs invisible.
23. **Global single-shard topology** with regional gateway PoPs, a multi-region accounts tier (evaluate CockroachDB), and cross-shard social/economy.
24. **QUIC/WebTransport** transport for web/companion clients and UDP-hostile networks.
25. Reinforced-node scheduling for pre-announced battles; interaction-cluster-based AOI aggregation (fleet proxies) for 5k+ participant fights.
26. ScyllaDB for chat/mail/event history if Postgres volume limits are reached.

---

## 10. Sources

Netcode & transport
- Glenn Fiedler, "State Synchronization" — https://gafferongames.com/post/state_synchronization/ (full text via https://github.com/mas-bandwidth/gafferongames/blob/master/content/post/state_synchronization.md)
- Glenn Fiedler, "Snapshot Compression" — https://gafferongames.com/post/snapshot_compression/
- Glenn Fiedler, "Snapshot Interpolation" — https://gafferongames.com/post/snapshot_interpolation/
- Glenn Fiedler, "Reliability, Ordering and Congestion Avoidance over UDP" — https://gafferongames.com/post/reliability_ordering_and_congestion_avoidance_over_udp/
- Glenn Fiedler, "What Every Programmer Needs To Know About Game Networking" — https://github.com/mas-bandwidth/gafferongames/blob/master/content/post/what_every_programmer_needs_to_know_about_game_networking.md
- Glenn Fiedler, "Why can't I send UDP packets from a browser?" — https://gafferongames.com/post/why_cant_i_send_udp_packets_from_a_browser/
- netcode protocol standard — https://github.com/mas-bandwidth/netcode/blob/main/STANDARD.md
- yojimbo — https://github.com/mas-bandwidth/yojimbo
- Valve GameNetworkingSockets — https://github.com/ValveSoftware/GameNetworkingSockets
- W3C WebTransport explainer — https://github.com/w3c/webtransport/blob/main/explainer.md
- Game Networking Resources (Tribes model, Quake 3 review, Overwatch GDC 2017, Halo: Reach "I Shot You First", Albion talk) — https://github.com/gafferongames/GameNetworkingResources
  - Tribes Engine Networking Model — https://www.gamedevs.org/uploads/tribes-networking-model.pdf
  - Quake 3 network model — http://fabiensanglard.net/quake3/network.php
  - Overwatch Gameplay Architecture and Netcode — https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and
  - I Shot You First (Halo: Reach) — http://www.gdcvault.com/play/1014345/I-Shot-You-First-Networking
- Valve, Source Multiplayer Networking *(background; not fetched)* — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
- Epic, Introduction to Iris — https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-iris-in-unreal-engine
- Epic, Iris Prioritization — https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-prioritization-in-unreal-engine
- Epic, Iris Filtering — https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-filtering-in-unreal-engine
- Epic, Iris Replication System — https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine

World partitioning case studies
- EVE Online, Tranquility Tech IV — https://www.eveonline.com/news/view/tranquility-tech-iv
- High Scalability, EVE Online Architecture — https://highscalability.com/eve-online-architecture/
- EVE Online, Fixing Lag: Character Nodes — https://www.eveonline.com/news/view/fixing-lag-character-nodes
- Engadget, EVE Evolved: EVE Online's server model — https://www.engadget.com/2008-09-28-eve-evolved-eve-onlines-server-model.html
- Warcraft Wiki, Sharding — https://warcraft.wiki.gg/wiki/Sharding_(term); Layering — https://warcraft.wiki.gg/wiki/Layering
- Wowhead, Blizzard clears up layering misconceptions — https://www.wowhead.com/classic/news/blizzard-clears-up-layering-tech-misconceptions-in-classic-wow-294589
- Blizzard patent, Cross-realm zones — https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9220982
- Guild Wars 2 Wiki, Megaserver — https://wiki.guildwars2.com/wiki/Megaserver
- UESP, ESO Megaservers — https://en.uesp.net/wiki/Online:Megaservers; ESO megaserver whitepaper thread — https://forums.elderscrollsonline.com/en/discussion/105319/eso-megaserver-whitepaper
- Photon blog, Albion Online — https://blog.photonengine.com/albion-online/
- Albion Online software architecture (Quo Vadis 2016) — https://www.slideshare.net/slideshow/albion-online-software-architecture-of-an-mmo-talk-at-quo-vadis-2016-berlin/62724504
- David Salz, Secrets of Albion Online Part 1 — https://davidsalz.de/secrets-of-albion-online-part-1/
- MMORPG.com, Albion Online networking explainer — https://www.mmorpg.com/news/albion-online-explains-its-internet-networking-system-so-players-can-better-understand-its-single-shard-system-and-woes-2000137686
- Bill Hegazy, New World Game Architecture — https://billhegazy.medium.com/new-world-game-architecture-615484345467
- Improbable IMS, How SpatialOS works with game engines — https://ims.improbable.io/insights/how-spatialos-works-with-game-engines/
- SpatialOS GDK for Unreal (archived 29 Aug 2024) — https://github.com/spatialos/UnrealGDK
- TechCrunch, Improbable urges Unity to unsuspend license — https://techcrunch.com/2019/01/11/improbable-urges-unity-to-unsuspend-their-game-engine-license-or-clarify-terms/
- PC Gamer, Mavericks: Proving Grounds cancelled — https://www.pcgamer.com/the-1000-player-battle-royale-game-mavericks-proving-grounds-is-canceled/
- TheGamer, Scavengers console launch scrapped — https://www.thegamer.com/scavengers-midwinter-console-launch-canceled-behavior-interactive/
- mein-mmo, Another SpatialOS MMO shut down — https://mein-mmo.de/en/another-mmo-using-the-wonder-technology-spatialos-is-being-shut-down-what-is-going-on,379672/
- Microsoft Game Dev, Hadean helps CCP Games — https://developer.microsoft.com/en-us/games/articles/2020/08/hadean-helps-ccp-games-realize-its-vision-with-azure/
- GamesRadar, EVE: Aether Wars — https://www.gamesradar.com/eve-aether-wars-preview-e3-2019/
- Metagravity, Why spatial partitioning is not the answer — https://metagravity.medium.com/why-spatial-partitioning-is-not-the-answer-to-scaling-the-metaverse-5b1873466cb1
- Star Citizen Wiki, Server meshing — https://starcitizen.tools/Server_meshing; Replication layer — https://starcitizen.tools/Replication_layer
- Server Meshing & Persistent Streaming Q&A — https://star-citizen.wiki/Comm-Link:18397/en
- Hangarbase, Expanded Server Mesh (CitizenCon 2025) — https://hangarbase.org/news/star-citizen-the-expanded-server-mesh-the-future-of-the-verse-revealed-at-citizencon-2025
- Ashes of Creation Wiki, Dynamic gridding — https://ashesofcreation.wiki/Dynamic_gridding; IntrepidNET — https://www.ashesofcreation.wiki/Intrepid_Net
- MMORPG.com, Ashes of Creation server meshing stream — https://www.mmorpg.com/news/ashes-of-creation-stream-breaks-down-new-server-meshing-network-technology-2000132119
- KBEngine (BigWorld-style open-source MMO server) — https://github.com/kbengine/kbengine

Services, data, infrastructure
- NATS JetStream concepts — https://docs.nats.io/nats-concepts/jetstream (text via https://github.com/nats-io/nats.docs)
- Nakama — https://github.com/heroiclabs/nakama
- Agones — https://github.com/googleforgames/agones; High-density GameServers — https://agones.dev/site/docs/integration-patterns/high-density-gameservers/
- Open Match — https://github.com/googleforgames/open-match

Patching
- Riot Games, Supercharging Data Delivery: The New League Patcher — https://technology.riotgames.com/news/supercharging-data-delivery-new-league-patcher
- desync (casync in Go) — https://github.com/folbricht/desync
- CascLib (Blizzard CASC reader) — https://github.com/ladislav-zezula/CascLib
- A Thorough Investigation of Content-Defined Chunking Algorithms — https://arxiv.org/pdf/2409.06066
