# 05 — Backend Services, Data, Persistence, Ops, Patching/CDN

Status: draft v5. Round-1 review fixes: control-plane decoupling (§1.4), block IDs (§1.4.5), market fencing
(§1.7), world state (§1.21), storage-growth model (§3.6), privacy (§6.6), DR (§6.7) and on-call (§6.8).
Round-2 fixes:
- **Failure detection by failure domain (§1.4.3–1.4.4, §1.4.6).** A host or rack loss is recovered as crashes
  and no longer freezes the shard.
- **Content versions split for live ops (§1.14.1).** A build's client part and its server part (the server
  overlay) are versioned separately, under a declared compat epoch.
- **Fenced order writes (§1.7).** Order insert, modify and cancel are owner-fenced.

Round-3 fixes:
- **Player fleets (§1.12.1).** A fleet group kind of ≤ 256 members with a fleet → wing → squad hierarchy,
  boss and commander roles, invites, fleet adverts and roster events. 06 §8.3a adds the cell-side mechanics
  (fleet warp, broadcasts, command bursts, fleet killmails), and 06 GP-15 is the acceptance.
- **World scripts (§1.23).** Studios get a backend extension surface with no backend source edits:
  shard-scope sandboxed Luau with declared keyed tables, durable timers, fenced ledger intents, events and RPC
  endpoints callable from cells. A20 is the acceptance, and its proof is a cross-zone bounty board in
  `starter-sandbox`.
- **Gateway capacity for an availability-zone loss (§6.7).** Gateways are sized so that the two surviving
  zones hold every session (12 boxes at 50k CCU). A19 gains a gateway clause.
- **Stale cross-references fixed:** the fence column list (§1.13), `Fence.Park` in the Go interface (§8), the
  compat-epoch check in 08 §1.1 step 7, and the domain-sized warm pool in 09 §4.3.2's cost model.
- **From the other sections' round-3 fixes:** `Drain`, `PreProvision`, rolling restarts and the epoch flip
  use 04 §6.7's planned region migration (§1.4, §1.13, §1.14.1, A14); chat evidence tags, player cases with
  SLA targets and the admin case queue (§1.10, §1.16, §1.17, §5); the Store API with minors' spending caps
  (§1.20); dynamic project packages validated through `pkg/htypes` (§1.14); and the collab service's
  project-wide data session and document homes (§1.14, 07 §1.8.2).

Round-4 fixes:
- **Gateway deploys without disconnects (§6.3.1).** A drained gateway moves each session make-before-break:
  the client opens a connection to the new gateway while the old one keeps carrying its input and state. Every
  contributing cell re-keys its per-session state without losing baselines. `DrainGateway` opens the zone's
  warm spare first, then relocates ≤ 500 sessions/s per box. The release table (§1.14.1), the Session service
  (§1.3) and the orchestrator (§1.4) gain the matching steps, and A14 (Ph4) gains a gateway-roll clause.
- **Fence write load budgeted (§1.13, §3.6).** Fence operations and rows per second are sized for Phase 2
  and Phase 4 and added to the shard-primary WAL and IOPS figures. The contention rules are a cell-side fence
  gate, the fence check as the ledger transaction's last statement, one lock order, bounded lock waits and
  table tuning. A5 (Ph4) now runs the design fence mix beside 10k ledger tx/s, with a 60-member carrier.
- **From 06's round-4 fixes (housing, cities, territory):** ledger plot ops with an exclusion constraint
  (§1.6); the structure upkeep worker (§1.8); keyed world flags, territory rows, audited transitions and
  `PreProvision` requests (§1.21); system-job escrow claims and the Foundation `CityGovernance` world script
  (§1.23 items 6 and 13). 06 GP-16 and GP-17 are the acceptance.
- **From 04's and 07's round-4 fixes:** connect tokens choose gateway instances by zone-instance affinity, then
  free slots (§1.3); `AssignReplicant` covers ≤ 4 world-zone cells across zones and recovery restores from the
  replicant in every persistent world zone (§1.4, §1.4.6); the compat fingerprint names the physics build
  (§1.14.1); cell rolls hitch once per region moved (§1.14.1); replicant cost (§6.4); `--spawn` starts a
  world-script host by default (§5); the GM row-edit guard for `@currency` and escrow-condition fields and
  the WSH's `--dev` debugging hooks (§1.23 items 7 and 11).

Round-5 fixes (minor):
- **Cheat and bot detection (§1.16a).** Cells extract aim, input and routine features from data they
  already hold. A red-team corpus labels cheat and farm bots. Trust scores sessions and stages delayed ban
  waves through §1.17's cases. The client anti-cheat vendor is now chosen in Phase 3 (WP-3.11). A21 (Ph4) is
  the acceptance.
- **Shard-primary write mix (§3.6).** Every writer is budgeted, not only the ledger, the fence and world
  scripts, and there is a replay budget for the synchronous standby. Account progression becomes append-only
  deltas (§1.5). Leaderboards and activity snapshots move to the persistence cluster (§1.12, §3.1). A5 (Ph4)
  runs the whole mix at once.
- **Replicants stay in their cells' availability zone (§1.4.6).** A zone loss therefore restores that zone's
  world cells from checkpoints, with ≤ 30 s of non-value state lost. §6.7 and A19 now state this bound.
- **NATS least privilege (§6.5).** A generated permission matrix covers every role. Credentials are issued
  per process, and services check that the cell in `Helios-Fence` is the process that sent the request.
- **Node maintenance (§6.1a).** Cordoning a SERVER node triggers `Drain` for every process on it.
  GameServers cannot be evicted. Batches are bounded by the blast radius and add capacity before they drain.
  A14 (d) covers this in Phase 4.

Conforms to ADR-002, -004, -006, -007, -008, -010, -012, -013 and **-014**.

Citations: `R07-P0-7` = requirement 7 of `docs/research/07-*.md` (R09: its gameplay/backend list; R03: its
table IDs). Research P0/P1/P2 are priorities, not phases: the R07 §8.3 columns "P0 (launch)", "P1" and "P2" map
to **Phases 2–3, 3–4 and 5** (01 §3.4). Cell, gateway and fence interfaces follow 04-networking-and-servers.

## 0. Principles
1. **PostgreSQL is the only system of record.** Valkey and NATS state is always rebuildable (ADR-008).
2. **Value moves synchronously through the ledger; everything else is write-behind.** A crash may roll back
   position or XP by one checkpoint window, but it never creates or destroys an item (R07 §5, R02-P0-7).
3. **Every retryable mutation carries an idempotency key.** Every cell write carries its AG fence
   `(ag, epoch)` (04 §6.1).
4. **One code path, one SQL dialect.** `helios-backend.exe` runs the same Go packages over embedded
   PostgreSQL, embedded NATS and miniredis (ADR-014) that production runs over PostgreSQL, Valkey and NATS.
   The system is a **modular monolith** until Phase 3, and services split out only when metrics demand it.
5. **Schema-first.** Messages, events, reason codes and migration stubs are generated from `*.hschema`
   (ADR-004).
6. **Strict ownership.** There is no gameplay logic in SQL, and no cross-service table access except the
   fence row (§4.1).
7. **The simulation tick never waits on a service.**
8. **Safety comes from fences; liveness comes from heartbeats.** Ownership is decided by generations and
   epochs stored in PostgreSQL and checked on every write. Losing the control plane (NATS, the orchestrator
   leader) never stops a simulation; a process stops only when it *observes* it was superseded (§1.4).
9. **Personal data lives only in Identity.** Every other store uses pseudonymous IDs, and player text is
   encrypted under a per-account key, so erasure never rewrites the append-only ledger or audit chain (§6.6).

---

## 1. Service catalogue
Each service is a package `internal/<svc>` that implements a generated connect-go interface. It owns one
Postgres schema and DB role, and exposes `/healthz`, metrics and traces. *Global* services span shards;
*shard* services are regional. There are 23 services (§1.1–§1.23).

### 1.1 Identity/Auth — global — Phase 0 (OIDC federation, MFA: Phase 4)
**Owns:** accounts, credentials, refresh-token families, bans, MFA factors, OAuth clients, the global
handle+discriminator registry (R03-P0-8), **subject keys**, legal-document acceptances and data-subject
requests (§6.6). It is the only store of direct PII, and that PII is encrypted under each account's key.

**Password hashing:** argon2id with m=64 MiB, t=3, p=1 (~80 ms per core), behind a 2×GOMAXPROCS semaphore
so login storms queue instead of running out of memory.

**API:**
- `Login` returns an EdDSA JWT (10 min) and a rotating refresh token (30 d, stored hashed). Reusing a
  refresh token revokes its whole family.
- `CreateLaunchCode` and `ExchangeLaunchCode` hand the client a one-time 60 s code, so refresh tokens never
  appear on a command line.
- Also `Register`, `Logout`, JWKS and `Ban`.
- **Legal and age (Phase 2):** `Register` takes date of birth and country for the age gate. `Login`
  answers `LEGAL_ACCEPTANCE_REQUIRED{doc, version, url}` while a re-acceptance version of the ToS, EULA or
  privacy policy is in effect, and `CreateLaunchCode` is refused until `AcceptLegal(doc, version, sha256)`
  is recorded (08 §2.3).
- **Privacy (Phase 3):** `ExportMyData`, `EraseAccount`, `CancelErasure`, and the internal
  `GetSubjectKeys` (mTLS, role `privacy_keys`) (§6.6).
- Bot-scoped credentials exist for load tests and are accepted only by test shards (08 §1.13).
- Phase 4 adds OIDC, PKCE, Steam/Epic/Google federation and TOTP.

**Handles:** unique `handle#discriminator`. A handle freed by erasure or rename is quarantined for 30 days,
then released.

**Failure:** services verify JWTs against cached JWKS, so an outage blocks only new logins (§6.7).

### 1.2 Login queue — shard — Phase 1 (cap), Phase 2 (lanes, 50/s admission), Phase 3 (full)
**State:** Valkey ZSET `q:<shard>`, scored `lane_offset + enqueue_ms`.

**API:**
- `Enqueue` returns an Ed25519-signed ticket `{account, shard, lane, enqueued_at, exp}`.
- `WatchStatus` streams position and ETA over SSE.

**Admission:** rate = min(orchestrator headroom, DB login budget), applied before the character DB is
touched (R07 §4). Admission pauses only while the control plane is degraded (§1.4.4). A host or rack loss
is not a degraded episode (§1.4.3): admission continues, at a rate that follows the reduced headroom until the
warm pool has replaced the lost cells. CCU is also capped by the gateway zone rule (§6.7).

**Lanes:** reconnect-grace (5 min) bypasses the queue. The rest run in order: GM/dev, priority, standard. If
gateway slots ever run short, reconnects wait in their own lane ahead of every login, and admission pauses while
any reconnect waits (§6.7).

**Recovery:** after a Valkey loss, clients re-present their tickets and the queue is rebuilt in order.

### 1.3 Session & connect-token issuer — shard — Phase 0
**State:** Valkey `sess:<id>` holds account, character, gateway instance, content pin
`(compat_epoch, manifest_hash)` and `session_epoch` (24 h sliding TTL).

**Connect tokens:** `CreateSession(grant, character)` mints a netcode-format token in Go (04 §2.3).
- Encryption is `x/crypto` XChaCha20-Poly1305, with a CI interop test against vendored netcode.
- Expiry ≤ 45 s; timeout 10 s.
- The token lists 2–4 gateway *instance* addresses, chosen by **zone-instance affinity** (instances on boxes
  already carrying the target game zone instance's sessions, within 04 §2.6's box, availability-zone and
  ≤ 50 %-per-box rules), then by the free slots each instance reports.
- The private part is sealed under the shard key. `client_id` = session ID, and 256 B of user data carry
  the character, the content pin `(compat_epoch, manifest_hash)` (§1.14.1), the placement ticket,
  entitlements and the attestation hash.

**Key rotation:** netcode has one private key per server instance, so daily rotation mints only for
instances already on the new key and lets old-key instances drain (tokens live ≤ 45 s). A small two-key
patch is the fallback.

**Reconnect (04 §2.4):**
1. Every 60 s, gateways batch-call `SealReconnectTickets`. Tickets are valid for 5 min and hold session,
   character, zone and `session_epoch`.
2. The client redeems a ticket with `Reconnect(ticket)` over HTTPS.
3. Session CAS-increments `session_epoch`, which makes the old gateway drop the session, and returns a
   fresh token without queueing (R07-P0-3).
4. **Burst:** a lost gateway box sends up to 8k redemptions at once (04 §2.4, §2.6). The service redeems 10k
   tickets in ≤ 5 s at p99 < 250 ms per call per shard, and reconnects never consume login-admission slots
   (04 NS-4.7).
5. **Availability-zone loss (Phase 4, §6.7):** a lost zone sends ≈ 17k redemptions while a third of this
   service's replicas and possibly the Valkey primary are gone. The service runs ≥ 2 replicas per zone,
   topology-spread, and each is sized for ≥ 2.5k redemptions/s, so the four survivors redeem **≥ 20k tickets in
   ≤ 8 s at p99 < 250 ms** (A19). Tickets are self-contained: after a Valkey failover a redemption rebuilds a
   missing `sess:` record from the ticket and sets `session_epoch = max(stored, ticket) + 1`.

**Relocation (Phase 4, §6.3.1):**
- **`MintRelocation`.** A draining gateway sends batches of ≤ 64 `{session, session_epoch e, from, target,
  fallback, contributors}`. For each session the service CASes `sess:<id>` from `(e, from)` to `(e+1, target,
  relocating_from = from, deadline = now + 15 s)`. It then mints a connect token for the target instance, plus
  a fallback instance on a third box, with a 10 s expiry.
  - In the token's user data, a `relocate{from, prev_epoch, budget_kbit, srtt_ms, contributors[≤ 16], home
    first}` block (≤ 84 B) takes the placement ticket's place.
  - A lost CAS means a reconnect, a relog or another relocation won, and the session is skipped.
  - The CAS makes relocation, reconnect and relog mutually exclusive per session.
- **`RelocateDone(session, e+1)`** clears the marker and sends the old gateway `SessionMoved{session, e+1}`.
- **Abort.** A marker past its deadline is **aborted back**: `sess:<id>` becomes `(e+2, from)`, and the old
  gateway is told to rebind at e+2.
- **Capacity.** ≤ 1,000 mints/s per shard, at ≤ 2 ms of service time per batch. That is a small share of the
  ≥ 15k tokens/s the replicas are sized for (2 per zone, ≥ 2.5k/s each), so a box kill during a gateway roll
  still meets NS-4.7.

Gateways never touch a DB to accept a token (R07-P0-1).

### 1.4 Orchestrator / world directory — shard — Phase 0 → 5
**Owns:** PG schema `svc_orch` with the tables `zone`, `process`, `region_lease` (generations), `zone_leader`
(generations), `zone_handle_block`, `orch_leader`, `id_alloc` and `placement_log`. KV `DIRECTORY` is a read
projection of `region_lease` for gateways and cells. It is never the authority and is repaired from PG on any
mismatch.

**API (per 04 §1):**
- `RegisterProcess(kind, fd{az, rack, host}, server_build)` returns a process ID and, for minting processes,
  the first ID blocks (§1.4.5). The failure domain `fd` drives detection and placement (§1.4.3, §1.4.6).
- `Heartbeat` (1 Hz, core NATS request/reply, not JetStream) carries load and the regions held with their
  generations. The leader's reply carries each region's current generation and the shard's control-plane
  mode.
- `ReportSuspect(gateway, silent[{cell, silent_ms}], live_trunks[])`: gateways report trunk silence
  (04 §6.4) once per second while any trunk is silent. Each report lists every silent peer and every trunk
  peer that delivered traffic in the last 1 s. The orchestrator can therefore judge each gateway's own
  health per failure domain (signal T, §1.4.3).
- `ProbeCell(gateway, cell)`, sent as `ctl.<shard>.gateway.<id>.probe`: the gateway probes the cell over its
  trunk and answers `ok | silent` (signal R, §1.4.3). The same trunk path carries `Fenced{region, lease_gen}`
  to a cell that NATS cannot reach.
- `AssignRegion`/`ReleaseRegion(instance, region, lease_gen)`, `Create/DestroyInstance`.
- `AssignZoneLeader(instance, cell, leader_gen)` and `LeaseHandleBlocks(instance, cell, n)`: zone-leader
  assignment with a standby, and `NetHandle` blocks recorded in PG before use (04 §3.5).
- `ReportLoad`, `ResolveRoute`, `TransferPlayer`.
- `Drain(process, reason, deadline)` empties a process by planned region migration (04 §6.7): no new
  placements, idle instances parked, the rest migrated in descending CCU order, ≤ 4 at a time. Migration state
  is the PG row `region_migration(region, from, to, lease_gen, state)`, and its commit is one term-fenced
  transaction that bumps `region_lease`, moves `zone_leader` if the source leads, and re-homes
  `zone_handle_block` rows.
- `PreProvision(zone, time, size)` sizes zones for scheduled sieges (R01-P1-14, R01-P2-21): ≥ 10 min ahead it
  migrates the zone to a reserved host of that size class (04 §6.7).
- `DrainGateway(box, reason ∈ rollout | maintenance | rebalance | evacuate, deadline)` (Phase 4) empties a
  gateway box by make-before-break relocation (§6.3.1). It opens the zone's warm spare first and stops
  listing the box in tokens. It then moves the box's sessions at ≤ 500/s per box and ≤ 1,000/s per shard. The
  box and zone headroom rules (§6.7) hold throughout.
- `MintTrunkToken(src, dst)` issues netcode tokens under a trunk key for the gateway↔cell and cell↔cell
  trunks (04 §2.6).
- `AllocateIdBlocks(n)` (§1.4.5).
- `AssignReplicant(cells[], replicant)` (Phase 4): one 2-vCPU replicant per ≤ 4 cells of persistent world
  zones, from one zone instance or several (the default for every world-zone profile, mandatory for multi-cell
  zones; instances and housing opt in), placed on a different host and, while the blast radius is a
  rack, a different rack from every one of its cells, but in their availability zone (§1.4.6). It is generation-fenced like a region (04 §6.4, ADR-007).
- `Split`/`Merge` arrive in Phase 5.

**Why the design changed:** draft v1 had region leases in NATS KV with a 3 s TTL, holders that self-fenced
when they could not renew, and orchestrator leader election through the same KV. A routine JetStream
stream-leader election or a NATS node loss would then self-fence every cell in the shard at once. Self-fencing
adds no safety, because the ledger, persistence, gateways and neighbour cells already reject stale epochs and
generations. The design below separates safety from liveness.

#### 1.4.1 Leadership (anchored in PG)
- `orch_leader(shard PK, term BIGINT, holder TEXT, expires_at timestamptz)`. A replica becomes leader with
  `UPDATE … SET term = term + 1, holder = $me, expires_at = now() + interval '10 s' WHERE shard = $s AND
  expires_at < now()`. The leader renews every 2 s and acts only while its monotonic clock is less than 8 s
  past its last successful renewal.
- **Term-fenced writes.** Every mutating orchestrator statement includes
  `AND (SELECT term FROM orch_leader WHERE shard = $s) = $my_term`, so a deposed leader's writes touch 0 rows.
- Replicas: 2 through Phase 3 and 3 from Phase 4. Every replica subscribes to heartbeats and gateway
  reports, so a follower takes over with warm liveness state in ≤ 10 s.
- A PG failover (< 30 s, A13) leaves the shard leaderless for that long. This is the degraded mode of
  §1.4.4, not an outage.

#### 1.4.2 Region leases and generations
- `region_lease(region_id PK, instance_id, holder_proc, lease_gen BIGINT, assigned_at)`. The generation is
  bumped in PG, term-fenced, before any holder is told. `DIRECTORY` is written after the commit. Zone-leader
  generations (`zone_leader`, 04 §3.5) follow the same rules.
- **Where regions come from.** `CreateInstance` creates one `region_lease` row per region of the zone's
  `ZonePartitionDef` in the pinned content build (07 §2.6.5; 02 §5.5): one row for a v0 zone (`Whole`), up to
  64 for a v1 static multi-cell zone. A partition change takes effect only for instances created on a build
  that contains it.
- **The holder has no TTL to beat.** The **liveness TTL of 12 s** is only the orchestrator's fallback for a
  process on which neither T nor R can be evaluated (§1.4.3).
- **Holder rule.** A cell keeps simulating while it cannot reach the control plane. It stops simulating a
  region only when it *observes* one of these:
  - a higher `lease_gen` for that region, from a heartbeat reply, the `DIRECTORY` watch, a gateway
    `RouteUpdate`, a trunk `Fenced` NACK or a `ctl` message;
  - a failed fence CAS, or a `STALE_EPOCH` rejection from the ledger or persistence for one of the region's
    AGs. That AG is dropped at once and the region is re-verified with the orchestrator;
  - a rejected region checkpoint. Region checkpoints (transients, spawners) carry `lease_gen`, and
    persistence refuses one below the region's current generation (§1.13).

  A cell that has lost every region exits (fail-stop).
- **Receivers fence too.** Every trunk message from a cell carries its `(region, lease_gen)`. Gateways and
  neighbour cells drop messages below the generation they know, and gateways ignore trunk state for an AG
  whose epoch is below their route table's (04 §6.3–6.4). A superseded cell therefore cannot reach players or
  apply effects, even to transients.

#### 1.4.3 Failure detection: independent signals and failure domains

**Failure domains.** Every process registers `fd = {az, rack, host}` (§1.4 API).
- *Agones placement:* from node labels (`topology.kubernetes.io/zone`, `helios.dev/rack`, the node name).
- *`helios-agent` on VMs:* from `helios.toml [fd]`, checked against the machine ID at registration.

The **blast radius B** (`orch.blast_radius`) is the largest domain whose loss is handled as ordinary crashes.
It never freezes the shard. Placement bounds each domain's size (§1.4.6).

| Phase | Placement | B | Max cells per host | Control plane (NATS, PG, orchestrator, Valkey) |
|---|---|---|---|---|
| 2 | VMs under `helios-agent` | host (VM) | 4 | its own VMs |
| 3 | Kubernetes + Agones | host | 8 | `control` node pool, host-disjoint from SERVER hosts |
| 4–5 | Kubernetes + Agones, ≥ 3 SERVER racks per AZ | rack, including its top-of-rack switch | 12 | `control` pool on racks that hold no SERVER host |

**Signals.**

| Signal | Source | Fires when |
|---|---|---|
| **H** heartbeat | process → orchestrator, core NATS; every orchestrator replica subscribes | no heartbeat for 3 s |
| **T** trunk silence | gateways, via `ReportSuspect` (04 §6.4) | Every gateway that holds a live trunk to the process, and has itself reported in the last 2 s, reports ≥ 2 s of silence from it. Each of those gateways is **trunk-healthy** outside that domain: ≥ 90 % of its trunks to processes outside the silent process's failure domain delivered traffic in the last 1 s. At least one of them lies outside the process's host (from Phase 4, outside its rack) |
| **P** process state | placer: `os/exec` wait, Agones `GameServer` Unhealthy/Shutdown, pod deletion | the process exited or is not Ready |
| **R** direct probe | orchestrator | **Both** probes fail, in 2 consecutive rounds 500 ms apart: (1) the NATS ping `ctl.<shard>.cell.<id>.ping`; (2) a trunk probe by two trunk-healthy gateways outside the process's failure domain. A trunk probe uses an existing trunk, or a short-lived probe connection minted by `MintTrunkToken` when the gateway has none. It fails when no challenge or `ProbeAck` arrives within 500 ms |
| **N** NATS-isolated (not a failure) | orchestrator | H has fired, but a trunk probe answers or a gateway still sees trunk traffic |

**Rules.**
- **Confirmation needs H plus one of T, P or R.** Neither T nor R waits for P. Two cases therefore confirm in
  ≈ 3 s: a VM that dies without `helios-agent` ever reporting it, and a Kubernetes node that is not yet
  NotReady (≈ 40 s, then a 300 s eviction).
- **A process with no trunks** (a cell without players, a replicant, a standby) is confirmed by H plus R, in
  ≈ 4 s. The 12 s liveness TTL is used only when no trunk-healthy gateway can run R. The process is then
  confirmed by H held for 12 s, plus P.
- **A NATS-isolated process (N) is alive and is never reassigned.** It keeps serving players over its
  trunks and applies degraded mode locally (§1.4.4).
  - `CellNatsIsolated` pages SEV3 after 10 s.
  - After 60 s, if the rest of the shard is in normal mode, the orchestrator replaces the process on purpose.
    It bumps the generation, sends `Fenced{region, g+1}` over a gateway trunk (the probe path), and recovers
    the region to a standby (§1.4.6).
  - The cost is non-value state only. In Phase 4 world zones the replicant is fed over HTP, not NATS,
    so ≤ 1 s is lost. Elsewhere the loss is ≤ the isolation time plus the checkpoint window.
- **Typical crash timeline:**
  - 0 s: the cell crashes;
  - 0.5 s: gateways mark it suspect and freeze its sessions;
  - 3 s: H plus T or P confirm the failure;
  - ~3.5 s: the generation is bumped in PG and a standby is assigned;
  - ≤ 10 s after confirmation: the standby serves players (04 §6.4, A3b).

  This is the same 3 s detection that 04 §6.4's budget assumed.

**Correlated silence.** Suppose H fires, within 5 s, for a set S of at least max(3, 10 %) of the shard's
cells and replicants. The orchestrator then classifies S before it acts:

| # | Check | It passes when | If it fails |
|---|---|---|---|
| C1 | Orchestrator health | the leader's NATS connection is up; it has seen no JetStream API error for 2 s; no follower replica in another AZ still receives heartbeats from a member of S | control-plane fault |
| C2 | Gateway health | ≥ 50 % of gateways reported in the last 5 s, and ≥ 90 % of those are trunk-healthy outside the domains of S | control-plane fault |
| C3 | Containment | S lies within one blast-radius domain, together with every process confirmed dead in the preceding 30 s | wide loss |
| C4 | Corroboration, per member | the member has T (if it has trunks) or R (if it has none) | a member whose trunk or probe answers is N; an uncorroborated member is re-evaluated every 1 s |

- **C1, C2 and C3 pass: domain loss.** Every corroborated member is confirmed at once, and all are recovered
  in parallel (§1.4.6).
  - There is **no degraded mode**. Login admission, instance creation and every other reassignment continue.
  - `FailureDomainLost` pages SEV2.
  - A host power-off is confirmed ≈ 3 s after the fault, and a rack partition the same way.
- **Only C3 fails: wide loss.** Examples: an AZ, two racks at once, or a mass crash from a poison build.
  - The orchestrator enters degraded mode for a **5 s hold**, so that a transient partition can heal.
  - Confirmation keeps running during the hold, and the exit rule (§1.4.4) excludes confirmed-dead
    processes. The hold therefore ends by itself once the survivors are healthy.
- **C1 or C2 fails: control-plane fault.** The orchestrator enters degraded mode, and nothing is reassigned
  before exit.

**Every branch is safe.** A process confirmed dead by mistake becomes a zombie, and fences stop zombies
(§1.4.2, §4.1). The classifier only decides whether acting now helps players.

**Classifier tests.** `Classify` in `internal/orch/domain` is a pure function of heartbeat times, gateway
reports, probe results, failure domains and orchestrator health. Its table test covers these single faults:
- one process, one host, one rack, one AZ, two racks;
- a NATS node, the leader's NATS link, and NATS blocked on one host only (port 4222);
- all gateways lost, a PG failover, and a poison build.

It also covers every pair of those faults. Each case asserts its class and its time to confirmation. The live
drills are A15 and 04's NS-4.1.

#### 1.4.4 Control-plane degraded mode
**Entry.** Any one of these:
- there is no PG-confirmed leader;
- the leader has lost NATS, or has seen JetStream API errors, for > 2 s;
- fewer than 50 % of gateways have reported in the last 5 s;
- correlated silence is classified as a control-plane fault or a wide loss (§1.4.3). A **domain loss never
  enters degraded mode.**

**While degraded:**
- **Orchestrator:** no reassignments, placements, instance creation, drains, zone-leader changes or
  split/merge. Generations are frozen and `PreProvision` is deferred.
  - **Detection keeps running.** H+T, H+R and P still mark processes `confirmed_dead`. The mark is a
    term-fenced PG write to `svc_orch.process`, so a new leader inherits it.
- **Cells** learn the mode from the heartbeat reply, or assume it after 3 s without a heartbeat ack.
  - They keep simulating.
  - They defer handoffs. The current owner keeps the AG, the boundary hysteresis widens to 3·H and
    co-location leases extend (04 §6.2a–6.3).
  - Zone transitions and instance entry wait.
  - Value calls show "pending" and retry with the same key.
  - Checkpoint records coalesce per AG in a bounded buffer. Only the newest record per AG is kept, so
    memory is bounded by live AGs × blob budget (§3.6), not by outage length. The buffer flushes on
    reconnect.
- **Gateways** keep forwarding. The **login queue** pauses admission, and queued players keep their
  positions. **Market, mail and chat** calls on the service lane fail fast with retry-after.

**Exit.** All of these hold:
- there is a PG-confirmed leader;
- NATS has been healthy for 5 s (a wide-loss hold ends at the same moment);
- ≥ 50 % of gateways are reporting;
- ≥ 90 % of the processes **not confirmed dead** are heartbeating. The dead of a host, rack or AZ therefore
  never hold the mode open.

Then:
1. Every process re-reports its regions and generations, and the leader reconciles them against PG. A process
   holding a lower generation is sent `ctl…fenced`.
2. Confirmed-dead processes are recovered first, in descending CCU order (§1.4.6).
3. The normal detector resumes.

Degraded mode lasting > 60 s pages (§6.8).

**Trade-off.** Suppose a cell really crashes during a NATS or PG outage. It stays frozen, with its sessions
held linkdead by the gateways, until NATS is back and H+T confirms it (≈ 5 s after exit at most). That trade
is deliberate: R07 §8.4 expects no player impact from a NATS node loss, and such losses are far more common
than a coinciding cell crash. Host and rack losses are not control-plane faults, and they are handled at once.

#### 1.4.5 ID allocation (time-prefixed blocks)
IDs are 63-bit: `0 | 41-bit ms since 2026-01-01 | 5-bit shard | 17-bit offset` (§3.2). They are minted from
**blocks** of 2^17 = 131,072 IDs.

**Allocation.** `AllocateIdBlocks(n ≤ 16)` advances the per-shard row `id_alloc.last_ms` by n and returns
the n prefixes:
```sql
UPDATE id_alloc SET last_ms = GREATEST(last_ms + $n, $pg_now_ms) WHERE shard = $s RETURNING last_ms
```
- Prefixes strictly increase inside one PG row, whichever replica serves the call. There are therefore no
  node IDs and no need for a leader.

**Who mints.** Only processes that mint IDs hold blocks: cells, identity (account IDs), character (character
IDs), ledger, market, industry, mail, world state, activity and lifecycle cleanup. Gateways, queue, session
and chat never mint; session IDs are random 63-bit values. 09 §5.10's CONF-05 enforces this list.

**Minting.**
- A minter holds two blocks and refills in the background once the current block is half used.
  Battle-profile cells hold four.
- Minting is a local increment, with no lock and no stall. A 2,000-ship volley burst draws on at most one or
  two blocks.
- A block retires 1 h after allocation and its remainder is discarded. This keeps IDs within 1 h of
  wall-clock order for the `tx_id` range partitions. While the control plane is degraded, minters keep
  using retired blocks rather than stall.
- A crash wastes only the unused remainder. Blocks are never reused, so no zombie can collide with a
  successor.

**Capacity.** Prefixes stay on the wall clock up to 1,000 blocks/s per shard, which is 131 M IDs/s. The
41-bit time lasts until 2095. The old 41/5/8/9 layout allowed 256 nodes per shard, fewer than §6.4's 480
cells plus services, and a 9-bit sequence could stall a volley.

**Implementation.** Go uses `pkg/idgen`; C++ uses the cell's `EntityRegistry` minter (02 §4.1). Golden
vectors cover both. `NetHandle` blocks (04 §3.5) are a separate, zone-local space.

#### 1.4.6 Placement and recovery
**Placement:** `Placer` has two implementations.
- `LocalProcessPlacer` supervises processes with os/exec, backoff and a warm pool (the SWG TaskManager
  analogue, R02-P2-19). `helios-agent` runs it on each VM.
- `AgonesPlacer` uses the Allocation API.

Quiet zones share processes; hot zones get dedicated ones (R01 §3.2).

**Spread across failure domains (from Phase 3).** Fleets use Agones `scheduling: Distributed`, not the
default `Packed`, plus topology spread constraints on host, AZ and, from Phase 4, rack:
- **Cells:** at most `max_cells_per_host` per host (§1.4.3). The cells of one multi-cell zone instance spread
  with `maxSkew 1` across hosts (from Phase 4, across racks), so one domain loss costs a zone as few cells as
  possible.
- **Replicants** (Phase 4) never share a host with any of their cells. While B = rack they also never share a
  rack with them, so a rack loss leaves the lost cells' replicant state alive.
  - **They stay in their cells' availability zone.** `AssignReplicant` groups a replicant's ≤ 4 cells by
    zone and picks a replicant in that zone. It uses another zone only while that zone has no replicant
    capacity, and it moves the assignment back once capacity returns. The ≤ 20 Mbit/s stream per cell
    (04 §6.4) therefore normally never crosses zones (§6.4).
  - **Consequence.** An availability-zone loss is a wide loss (§1.4.3) that takes a zone's cells and their
    replicants together. Those cells restore from checkpoints and lose ≤ 30 s of non-value state (§6.7,
    A19). AGs whose replicant lived in another zone still restore from it.
  - **Rejected alternative:** zone-disjoint replicants. They would keep a zone loss at ≤ 1 s, but they add
    ≈ 4 Gbit/s of cross-zone traffic at Phase 4 peak, ≈ $7–13k/month at cloud inter-zone rates. That buys
    ≤ 29 s less rollback in an event whose recovery already takes up to 5 min (§6.7).
- **Standbys** spread with `maxSkew 1` across hosts and AZs, and from Phase 4 across racks. A recovery
  assigns only standbys outside the failed domain, and never one on a host that already runs another cell of
  the same zone instance.
- **The control plane** runs on the `control` pool (§1.4.3 table). A SERVER host or rack loss therefore never
  removes a NATS, PG or orchestrator quorum member.

**Warm pool.** It is counted in **slots**. One slot is the vCPU and RAM of one 8-vCPU world cell. Smaller
activity cells count ¼ slot, and a small-class standby takes up to 4 of their regions.
- **Phase 2:** one standby per 8 cells per class. A VM loss beyond that restarts its cells cold (≤ 60 s), and
  it no longer freezes the shard.
- **Phase 3 onwards, per class:** for every blast-radius domain d, the standby slots outside d must be ≥ the
  cell slots inside d, plus 1. The +1 covers an independent crash during the recovery.
  - With the pool spread evenly over n domains, this is `W ≥ ⌈(C_max + 1) · n / (n − 1)⌉`, where `C_max`
    is the largest domain's cell slots.
  - *Generic standbys* (engine up, no class content) count for any class. They load the zone template in
    parallel with fence advance and state restore (≤ 4 s), so a short class costs time, not correctness.
- **Worked sizes:**

  | Phase | Fleet | Largest domain | Pool | Share |
  |---|---|---|---|---|
  | 3 | 25 cells on 5 SERVER hosts | 1 host: ≤ 5 slots (up to 8 cells) | ⌈6 · 5/4⌉ = **8 slots** (64 vCPU, ≈ $2k/month in cloud) | ≈ 30 % |
  | 4 | 240 cells on 9 SERVER racks | 1 rack: ≤ 27 slots (≈ 34 processes with replicants) | ⌈28 · 9/8⌉ = **32 slots** | ≈ 13 % |

- **Refill.** The Fleet autoscaler restores the pool after a draw-down. `WarmPoolBelowDomain` fires (SEV3,
  SEV2 after 15 min) while the pool would not survive the worst single domain loss. Lost replicants are
  replaced from the Fleet buffer. This is not time-critical, because their cells resync them within 5 s
  (04 §6.4).

**Recovery:** once a failure is confirmed (§1.4.3):
1. The leader bumps the region generation in PG. A domain loss bumps every lost region in one term-fenced
   transaction (≤ 0.5 s for 40 regions).
2. It sends a best-effort `ctl…kill` to the old process: over NATS, or through a gateway trunk for a
   NATS-isolated process.
3. It assigns a warm standby outside the failed domain.
4. The standby bulk-advances the region's fence rows with `Fence.AdvanceOwnedBy(region, lease_gen, owner)`.
   This takes ≤ 1 s p99 at 5k AGs, and ≤ 2 s p99 for 40 regions totalling 150k AGs advancing at once.
5. The standby restores state.
   - From Phase 4, in persistent world zones (single-cell or multi-cell), it restores from the region's
     replicant and loses ≤ 1 s. This holds for any loss inside the blast radius. In an availability-zone
     loss the zone's replicants are lost too, so checkpoints apply (≤ 30 s).
   - Otherwise, and for any AG the replicant lacks, it loads checkpoints (04 §6.4), at most 64 in flight per
     standby. The persistence cluster's load path is sized for 30k loads/s at Phase 4, so even a rack whose
     zones have no replicant restores in ≤ 6 s.
   - Hitch **≤ 10 s** after confirmation, including for a whole blast-radius domain.

A zone leader on a confirmed-dead cell is replaced by its standby under a new `leader_gen` (04 §3.5).

**Ordering and brakes.**
- **Order.** Beyond the warm pool (a wide loss), regions recover in descending CCU order as the Fleet scales
  up. Populated regions come before empty ones.
- **Poison-build brake.** Suppose 3 standbys that took regions pinned to the same server build crash within
  60 s of assignment. The orchestrator then stops recovering regions on that build and pages SEV1
  `StandbyCrashLoop`. The runbook pins the previous build (§6.8).

### 1.5 Character — shard — Phase 1
- **Owns:** character rows, the appearance vector, bind/home, last zone, a progression snapshot and the
  account-scope settings blob (≤ 256 KiB: keybinds, UI layout, chat tabs; 08 §1.4). The computed stat
  "brain" moves with handoff (R01 §3.2).
- **API:** `List`, `Create`, `Delete` (soft, 30 days), `Get`, the async `ApplyProgression(char, delta,
  idem)` (06) and `ResolveNames(ids[])`. Crafter stamps and killmails store IDs, and names resolve at display
  time (§6.6).
- **Follower progression (06 §7.3):** the levels, influence and learned commands of a bound companion or pet
  are progression rows keyed by its control-device item ID, written through the same `ApplyProgression`.
- **Account (legacy) progression (06 §5.2–5.3), Phase 2:**
  - *Stores:* per-`(account, shard)` base rows `account_progress(account_id, graph_id, completed BYTEA,
    counters BYTEA, claims BYTEA, hw BYTEA, version)` for achievement, collection, codex, legacy and
    season-track graphs, ≤ 32 KiB per account. Flushes go to **append-only deltas**
    `account_progress_delta(account_id, graph_id, incarnation, flush_seq, deltas BYTEA)` of ≈ 200 B each.
    Rewriting the whole BYTEA row on every flush would cost ≈ 3 KB of WAL on average and up to 32 KiB; a
    delta costs ≈ 0.4 KB (§3.6).
  - *API:* `ApplyAccountProgression(account, deltas[], flush_seq)` applies counter increments and
    completions. It is idempotent by `(account, cell incarnation, flush_seq)`, at 2k flushes/s with
    p99 ≤ 20 ms. It takes `pg_advisory_xact_lock(account)`, which writes no WAL. The flush is a duplicate if
    the base row's high-water mark `hw` for that incarnation (kept 7 days) is ≥ `flush_seq` or the delta row
    already exists. Otherwise it inserts one delta row.
  - *Folding.* An account's deltas are folded into its base rows once it has 32 of them, or once the oldest
    is 15 min old. The fold takes the same lock, applies the deltas, advances `hw` and `version` and deletes
    the folded rows, all in one transaction. `GetAccountProgression(account)` returns the base rows with any
    unfolded deltas applied, so readers never see the split.
  - *Rewards* are ledger grants keyed `(account, node, tier)`. The legacy bank is a ledger hangar with
    owner kind `Legacy`.
  - *Mirroring:* graphs marked `mirrorAsEntitlement` also issue an idempotent entitlement grant (§1.20),
    so every shard honours the unlock.
  - *Privacy:* the rows are part of the §6.6 export and erase set.
- **Events:** `evt.<shard>.character.{created,entered_world,left_world,erased}`.

### 1.6 Item & currency ledger — shard — Phase 1 core, Phase 2 full, Phase 5 partitioned
The value authority (R07-P0-7, R05-P0-8, R09-P0-7/8).

**Currency** is double-entry: postings sum to zero per currency.
- Each `ReasonCodeDef` (06), e.g. `Faucet.Bounty.NPC` or `Sink.Tax.Market.Broker`, has system accounts
  `mint:<code>` / `burn:<code>`. These may go negative and are not balance-materialized, so they never
  become hot rows.
- Player, corp-division and escrow wallets materialize `balance ≥ 0`.

**Items** have block IDs (§1.4.5) and are either uniques (rolled stats and sockets in `attrs JSONB`, per 06
`ItemInstance`) or stacks. Each has an owner, a location (container, hangar, structure or escrow), a flag
and a `version`.
- *At rest*, items change only through services.
- *In custody* of a cell's authority group (AG), a change also needs that AG's current fence (R09 A15).

**API:**
- `Execute(LedgerTx{idem_key, reason_code, actor, fence?, preconditions[], ops[]})` applies these ops
  atomically: `Mint`, `Burn`, `Transfer`, `MoveItem`, `Split`, `Merge`, `Modify`, `Destroy`,
  `TakeCustody`, `ReleaseCustody`, `CreateAg` (a new persistent AG's fence row, in the same transaction as the
  items it takes into custody, 04 §6.1), `EscrowOpen`/`Settle`/`Refund`, and `ClaimGuard(scope, key,
  period, payload?)`: an insert into `ledger_guard` that fails the transaction with `PRECONDITION_FAILED` if
  the row exists. `payload` is ≤ 64 B, such as a saved instance ID. It is the guard entity (§3.3) for once-per-period rewards whose keys outlive the idempotency
  window, such as activity lockouts (06 §6.8). `ledger_guard` is partitioned by period, and a partition is
  dropped two periods after it closes.
- Plot ops for structures (06 §9.2.1, Phase 3): `OccupyPlot(zone, face, circle, structure, zone_max)` and
  `VacatePlot(structure)` write `ledger_plot(zone, face, structure, plot circle)`. The table carries the
  integrity constraint `EXCLUDE USING gist (zone WITH =, face WITH =, plot WITH &&)` (`btree_gist`), so two
  overlapping structures can never both commit, and the loser fails with `PRECONDITION_FAILED` (`PLOT_TAKEN`).
  `OccupyPlot` also increments `ledger_plot_zone.count` under its row lock and fails with `ZONE_FULL` at
  `zone_max`. `ListPlots(zone)` serves the cells' zone manifests, and plot ops appear in `evt.<shard>.ledger.tx`.
- Wrappers: `Grant(reward_token)`, `AtomicSwap`, `ConsumeBatch`.
- Reads: `Journal`, `ListAssets`, `ItemHistory`. Beyond the 90-day online window, reads come from the
  Parquet archive (§3.3). Admin: `Reverse`.

**Rules:**
- Unknown reason codes are rejected (R01-P0-8). Per-code rate caps implement 06's economy guard.
- Creation and owner-to-owner transfers are synchronous.
- `ledger_policy: batched_consume` records (ammo, fuel) are consumed in custody and reported by
  `ConsumeBatch` every 10 s, so a crash refunds at most one window.
- `Execute` first takes `pg_advisory_xact_lock(idem_hash)`, then probes the live idempotency partitions
  (§3.3). This serializes concurrent retries of the same key.
- Rows lock in ascending-ID order at READ COMMITTED. 40P01 and 40001 errors are retried.

**Events:** `evt.<shard>.ledger.tx`, published via the outbox, carries every posting with its reason code.
Consumers: telemetry, trust, the public API, the Economy Sim and cell inventory caches.

**Scaling:** stateless handlers on the shard primary.
- Phase 2: 2k tx/s (AAA-SRV-8).
- Phase 4: 10k tx/s.
- Phase 5: 50k tx/s, with owner-hash partitions and cross-partition moves as escrow sagas.

Storage for each rate is in §3.6.

**Failure:** cells keep simulating. Value actions show "pending" and retry with the same key. Kill switches
disable single features.

### 1.7 Market — shard/regions — Phase 2
**Owns:** orders, trades, daily history, and contracts (courier, exchange, auction) with collateral in
escrow (R09 A6).

**API:** `PlaceOrder(client_order_id, region, type, side, price, qty, min_qty, range, location)`,
`ModifyOrder`, `CancelOrder`, `GetBook`, `History`, and the contract calls.

**Matching:**
- One **single-writer goroutine actor per (region, type)**, with price-time priority.
- Buy ranges are checked against the record-DB jump graph.

**Ownership and fencing** (R07 §4, R02-P1-11):
- Books hash into 256 partitions. `market_partition(partition_id PK, owner_replica, owner_gen)` in PG is the
  authority. KV holds only a routing hint.
- Partitions are assigned by rendezvous hashing over the live market replicas in the orchestrator's process
  registry.
- **Takeover.** A replica takes a partition with a CAS:
  ```sql
  UPDATE market_partition SET owner_gen = owner_gen + 1, owner_replica = $me
   WHERE partition_id = $p AND owner_gen = $seen
  ```
  The new owner then loads the partition's open orders, and it accepts commands only once the load is done
  (≤ 1 s per partition). Commands that arrive meanwhile queue in the actor.
- **Every order write is fenced, not only matches.** Each of these runs in one market-DB transaction:
  - order insert (`PlaceOrder`);
  - `ModifyOrder`;
  - `CancelOrder`, and expiry;
  - a match with its trades.

  The transaction reads `market_partition … FOR SHARE` and aborts unless `owner_gen` equals the actor's
  generation (the pattern of §4.1).
  - A takeover `UPDATE` waits for every in-flight order write. Once it commits, no write at the old
    generation can commit.
  - The new owner's load therefore sees every committed order, and nothing can later add, change or remove an
    open order behind its in-memory book.
  - An actor whose write aborts stops and drops its in-memory book.
- **Only the owning actor writes orders.**
  - Any market replica accepts the RPC. It forwards the command to the owner on
    `rpc.<shard>.market.<p>.Exec`, which only the current owner subscribes to (a routing hint, not the
    authority).
  - If the owner's fenced write aborts, the command returns `NOT_OWNER`. The receiving replica re-reads
    `market_partition` from PG and retries once at the new owner, else answers retry-after.
  - Retries are safe: `client_order_id` is unique, and every escrow call is idempotent.
- **Fills are conditional.** Each fill runs
  `UPDATE market_order SET qty_left = qty_left - $n WHERE order_id = $id AND state = 1 AND qty_left >= $n`.
  It must touch exactly one row per side, or the transaction rolls back.
- **Cancels and modifies are conditional too.**
  - A cancel runs `UPDATE market_order SET state = 2 WHERE order_id = $id AND state = 1 RETURNING qty_left`.
    Its outbox row refunds exactly the returned remainder, so a cancel can never race a fill.
  - A buy-order price rise first tops up escrow, keyed `order:<id>:mod:<n>`. A price cut refunds the
    difference from the outbox after the fenced update.
- A nightly audit also checks that Σ fills per order ≤ its original quantity.

**Order flow:**
1. Ledger `EscrowOpen`, idempotent by order_id.
2. The owning actor inserts the order in a fenced transaction and acks. Modify and cancel follow the same
   fenced path.
3. A match writes its trades in one fenced market-DB transaction.
4. `EscrowSettle` runs from the outbox and sends broker fees and taxes to `burn:*`.

`EscrowSettle` cannot fail for lack of funds. Fills are bounded by `qty_left`, which is bounded by what
`EscrowOpen` reserved, and the owner fence rules out a second matcher or a stale cancel. The escrow wallet's
`balance ≥ 0` CHECK is the last line of defence, and a violation pages.

**Recovery:**
- Actors reload their open orders.
- Unsettled trades are re-driven.
- A reconciler refunds orphan escrows after 10 min. It first has the owning actor write a fenced `void`
  tombstone for the order ID, and only then calls `EscrowRefund`. A late `PlaceOrder` retry therefore hits
  the tombstone and can never insert an order whose escrow was refunded.

### 1.8 Industry, resources & durable timers — shard — Phase 2
**Timers** are a shared library plus a worker.
- Rows: `(id, due_us, kind, payload, idem_key, state)`.
- Workers claim 500 due rows at a time with `FOR UPDATE SKIP LOCKED`, keep the next 60 s in memory, and wake
  on `LISTEN/NOTIFY`.
- A fired timer publishes `timer.<shard>.fired.<kind>` with `Nats-Msg-Id = id`.
- Timers use wall clock only. TiDi timers stay in cells.
- Target: lateness p99 < 1 s with 1 M pending (R01-P1-13).
- `evt.<shard>.timers.upcoming` gives the orchestrator 30 min notice.
- **Crafting-session refunds (06 §3):** *Slot* opens escrow `craft:<session>` and arms the timer
  `craft:<session>:refund`, due 15 min after the session's last heartbeat. *Finalize* cancels the timer in
  the same ledger transaction. The refund's guard entity is the escrow itself: once it is settled, a late
  timer fails its precondition.
- **Structure upkeep (06 §9.2.3), Phase 3.** The upkeep worker owns `structure_upkeep(structure, zone, owner,
  def, rate, seq, paid_through, stage, stage_at, debt, version)`. It creates a row from a ledger `OccupyPlot`
  event for a def with upkeep and deletes it on `VacatePlot` (inbox-deduplicated). The timer
  `upkeep:<structure>:<seq>` fires at the earlier of `paid_through` and the def's settle period (24 h). The
  settle is one ledger transaction, key `upkeep:<structure>:<seq>`, with `ClaimGuard('upkeep',
  <structure>:<seq>, week)`: it burns the accrued upkeep from the system wallet `upkeep:<structure>` and adds
  the city-tax legs as escrow claims into `CityGovernance` (§1.23 item 6), at the rates of the city footprint in
  KV `WORLD` (§1.21). The row's `seq`, stage and next timer then commit in one PG transaction, so a crash
  between the two replays the settle under its key. Stage changes publish `evt.<shard>.structure.stage`.
  Reclamation is one ledger transaction, key `reclaim:<structure>`: a loaded structure's owning cell issues it
  under its fence, and the worker issues it for a parked one with the dormant precondition (§4.1).

**Industry:**
- Recomputes duration and fee in Go HXL (`pkg/hxl`) within clamps (06).
- Consumes inputs in one ledger transaction (`job:<id>:start`).
- Mints outputs with a crafter stamp (`job:<id>:deliver`).
- Jobs and time-trained skills progress offline.

**Resources (R02-P1-9):** spawns are scheduled with seeded stats and a 6–21 day lifetime.

### 1.9 Mail — shard — Phase 2
**API:** `Send(recipients, subject, body, attachments?, cod?)`, `List`, `Read`, `Claim`, `Delete`.

**Attachments:**
- Move into escrow `mail:<id>` on send.
- `Claim` releases them. A cash-on-delivery claim is an `AtomicSwap`.
- A timer returns expired attachments.

**Limits:** 100 mails/h, 50 recipients. System notices use the same path.

**Storage:** bodies are encrypted under the author's subject key (§6.6). A mail is kept 90 days after it is
read and 365 days at most.

### 1.10 Chat — global channels, shard "local" — Phase 2
**Channels:** local, system, fleet, corp, alliance, whisper, custom, with ACLs, mutes and reports.
**History:** whisper 30 d, corp 90 d, local none. Stored bodies are encrypted under the author's subject key
in daily partitions, which are dropped at retention (§6.6). Local membership comes from cell events.

**Send path:**
1. Client → gateway lane → `rpc.<scope>.chat.Send`.
2. The service checks the ACL and the rate limit (5 msgs/10 s), then filters and persists.
3. It publishes on core NATS `chat.<scope>.<type>.<id>`.
4. Each gateway subscribes once per channel and fans out to its clients.

**Content:** plain text with allowlisted markup (R05 §7.4). Accounts aged 13–17 get strict filtering, and
whispers from non-friends are off (§6.6).

**Evidence tags (Phase 3).** Every delivered message carries its `msg_id` and a 16-byte `tag`: keyed BLAKE2b-128
over `(msg_id, channel, sender, sent_at, body)`. The key rotates daily and each key is kept for 30 days. A
report can therefore attach lines the reporter actually received, including local chat, which is never
stored, and the service verifies them without keeping more history (§1.17 cases; 08 §1.7.2 Support and
report). The tag adds 16 bytes per delivered message.

**Delivery:** at-most-once fan-out, with a history refetch on reconnect. No XMPP (R07 §4).

### 1.11 Social: friends, presence, corps, alliances — global — Phase 2
**Owns:** friends and blocks; `org` (corp/alliance) with members, 128-bit role masks, titles and standings.
Corp divisions are ledger wallets and locations.

**Presence:** Valkey `presence:<char>`, TTL 90 s, refreshed every 30 s.

**API:** friend, block, org, membership, role, alliance and standing operations, plus `CheckPermission`.

**Shared role model (R09 A11):** `pkg/perm` evaluates masks. Effective masks are projected into KV `PERMS`
and mirrored into every shard region (§6.7), so the ledger, market and structures check permissions against
a local cache.

### 1.12 Party, fleet, matchmaking, group finder, leaderboards & activity — shard — Phase 3 (cross-shard: Phase 5)
06 §6.6–6.13 owns the records (`ActivityDef`, `QueueDef`, `LeaderboardDef`, `GroupListing`) and the cell
side. This service owns groups (parties and fleets), queues, ratings, listings, boards and activity state.

- **Parties** persist across activities (R05 §3.3). They hold up to 16 members (operations), a leader,
  loot rules and a ready check. Parties are PG rows with Valkey presence, and each has a §1.10 party
  channel. A party is one of two **group kinds**; the other, the EVE-style **fleet** of ≤ 256, is §1.12.1.
- **Population placement (R05-P0-6)** chooses an instance by capacity, friends, guild and language
  (R07 §2.2); risk zones also use power band (06 §6.12).
- **Matchmaker.** One leader per queue partition. Ownership follows the market's pattern (§1.7): the PG row
  `mm_partition(queue, partition PK, owner_replica, owner_gen)` is the authority, partitions are assigned by
  rendezvous hashing over the live replicas, and a takeover is a CAS on `owner_gen`. There is no NATS KV
  lease (§2.3; 09 CONF-01/02). A deposed leader stops when a write at its old `owner_gen` fails, so it cannot
  emit matches after a takeover.
  - *Tickets.* Cells send `Enqueue{ticket}` over the service lane after validating entry, lockouts and the
    deserter tag (06 §6.13); `Enqueue` p99 ≤ 50 ms. Tickets live in the leader's memory and are mirrored
    to a Valkey hash `mm:<queue>`. A new leader rebuilds from the mirror in ≤ 5 s. A lost ticket only
    requeues its party, and no value is at stake.
  - *Buckets.* Each queue is split by tier and by latency band (RTT to the shard's region: ≤ 60, 60–120
    and > 120 ms). Bands merge as tickets age, per `QueueDef.latency`.
  - *Pass.* Each bucket runs a pass every 1 s. Tickets sort by `createdAt`, backfill tickets first. For
    each seed ticket the pass greedily adds compatible tickets: rating within the seed's window
    (`initialSigma + perSec · age`, capped) and a feasible role assignment. Feasibility is a bipartite
    matching of members to role slots (Hopcroft–Karp, ≤ 16 members × ≤ 4 roles). PvP matches split teams
    to put the predicted win probability nearest 0.5, keeping parties together: exhaustively for 6v6
    (462 splits), and by greedy assignment plus 2-opt swaps for larger teams. A match is emitted when
    the probability is within the current window.
  - *Widening.* Rating windows grow linearly with wait to a cap, and latency bands merge every 30 s up to
    150 ms. Role minimums never relax. When a role has been short for 60 s, the queue advertises
    `QueueDef.roles.fillBonus` to players who did not tick that role.
  - *Output.* A formed match gets a 30 s ready check through the gateway, then `CreateInstance` (04 §7),
    ready in ≤ 2 s (AAA-SRV-11). Declines requeue the others at the front with their wait kept.
  - *Backfill* requests from running instances (06 §6.10) sort ahead of new tickets.
  - *Ratings* live in PG `rating(char, model, season, mu, sigma, games)`. `ReportMatchResult` first
    inserts into `match_result` (primary key `matchId`), and the Weng-Lin update applies in the same
    transaction, so each match updates ratings exactly once.
  - *Capacity per shard.* 20k queued players across all queues. A 5k-ticket bucket pass takes ≤ 100 ms
    of CPU on one core. Memory is ≤ 1 KiB per ticket. The service forms ≥ 1,000 matches per minute and
    needs ≤ 2 cores at the Phase 4 bar of 50k CCU. GP-14a is the acceptance.
- **Group finder.** `lfg_listing` rows in PG, with a Valkey index per (activity, tier). A listing lives
  30 min unless the leader's client refreshes it. ≤ 50k listings per shard, search p95 ≤ 200 ms. Apply and
  accept go through the party API, and notes pass the §1.10 text filter.
- **Leaderboards.** The truth is PG `leaderboard_entry(board, window, partition, entrant, value,
  result_hash, instance)`, upserted with keep-best, keep-latest or sum.
  - *Location.* It lives on the **persistence cluster** (§3.1), not the shard primary. It is non-value,
    write-behind state, and 10k writes/s would take ≈ 4.5 MB/s of the primary's WAL budget (§3.6).
  - *Writes.* Only `ReportActivityResult` and `ReportMatchResult` write entries. They commit the result (and
    the ratings, for a match) on the shard primary, together with an outbox event. The board writer consumes
    that event and upserts, deduplicated by an inbox row in the same persistence-cluster transaction, ≤ 1 s
    behind the result.
  - *Failover.* After a persistence-cluster failover the consumer rewinds 5 min in `EVT` (7 d retention),
    and the inbox discards what survived, so no entry is lost.
  - *Reads.* Valkey sorted sets are the read cache, rebuilt from PG (§3.4).
  - *Budgets per shard:* ≥ 10k writes/s, top-100 reads p95 ≤ 50 ms, "around me" p95 ≤ 100 ms. Windows
    close on calendar events (§1.15) and are archived (GP-14g).
- **Activity state (the Destiny Activity Host, R05-P0-5):** objectives, encounter phase and checkpoints
  (06 §6.7 lists the contents).
  - It lives in KV `ACTIVITY`. Cells update it asynchronously through `Activity.Update(instance, (region,
    lease_gen), state)`. The service rejects a superseded cell and writes the key, so cells need no KV write
    rights (§6.5).
  - It is snapshotted to the persistence cluster every 10 s and on each phase change: ≤ 4 KiB per
    snapshot, 500/s at design load (§3.6).
  - A standby rehydrates from it in ≤ 5 s.
  - Lockouts are ledger guards (§1.6 `ClaimGuard`, 06 §6.8, R03-P1-8). `LockoutView(char)` reads them
    and is cached per character for 60 s.
- **Cross-shard federation (Phase 5, R03-P1-8).** Shards in one region may federate a queue or the group
  finder. Tickets carry the home shard, and the instance runs on the leader's shard. Visiting characters'
  AGs stay fenced by their home shard, and they persist through their home shard's persistence gateway and
  ledger over the in-region service mesh (≤ 2 ms RTT). Value never crosses shards: loot is personal and
  bound, and trade and need or greed are disabled between shards (GP-14h). Federation is not needed for
  the Phase 4 bar, because one shard holds 50k CCU, which meets GP-14a on its own.

#### 1.12.1 Fleets (EVE; M01, R01) — Phase 3
A fleet is the group kind for organized space warfare, mining and logistics. It is what 04 §2.7's
`fleet:<id>` voice channel, 06 §1.2's `Fleet` modifier domain and 06 §7.3's drone assist refer to. This
service owns the **roster**. 06 §8.3a owns the cell-side mechanics: fleet warp, broadcasts, command bursts and
killmail attribution. 08 §1.7.2's Fleet panel is the UI, and 06 GP-15 is the acceptance.

**Hierarchy.**

| Level | Count | Holds | Commander slot |
|---|---|---|---|
| Fleet | 1 | the FC slot and ≤ 5 wings | fleet commander (FC) |
| Wing | ≤ 5 | the WC slot and ≤ 5 squads | wing commander (WC) |
| Squad | ≤ 5 per wing | ≤ 10 members, one of them the squad commander | squad commander (SC) |

A fleet therefore holds at most 1 + 5 + 5 × 5 × 10 = **256 members**. 04 §2.7's "≤ 5 × 50" is a wing.
`FleetDef` (06 §8.3a) may lower these caps but never raise them. Commander slots may be empty. The boss
creates, names and deletes wings and squads.

**Roles and rights.**

| Role | Held by | May |
|---|---|---|
| Boss | exactly one member; transferable; the creator by default | invite, kick and move anyone; create, rename and delete wings and squads; set the MOTD, free-move and the advert; grant commanders the delegate rights *invite* and *move*; transfer boss; disband |
| FC, WC, SC | the member in that slot | broadcast to and fleet-warp their level (06 §8.3a); voice priority on their level and muting of its members (04 §2.7); with a delegate right, invite into or move within their level |
| Member | everyone else | broadcast upward (target, need repair, in position; 06 §8.3a); move themselves into a squad with space while free-move is on; leave |

Rights are a mask computed from (slot, delegate flags) and carried in every roster snapshot, so cells,
forwarders and gateways check them locally.

**Data** (this service's schema, PG):
- `grp(group_id, kind ∈ {party, fleet}, boss_char, free_move, motd_ct, advert_id, roster_ver, created_at)`;
- `grp_member(group_id, char_id UNIQUE, wing, squad, slot ∈ {member, SC, WC, FC}, delegate, joined_at)`;
- `grp_wing(group_id, wing, name_ct)`, `grp_squad(group_id, wing, squad, name_ct)`;
- `grp_invite(group_id, char_id, position, inviter, expires_at)`.

`char_id UNIQUE` keeps a character in at most one group. `ConvertToFleet(party)` keeps the `group_id`, makes
the leader FC and puts the others in wing 1, squad 1, and it keeps the chat and voice channels (only their kind
changes). Wing and squad names and the MOTD are player text, encrypted under the author's subject key (§6.6).

**Operations.** `CreateFleet`, `ConvertToFleet`, `Invite(fleet, char, position?)`, `Accept`, `Join(advert)`,
`Leave`, `Kick`, `Move(member, position, expect_ver?)`, `SetSlot`, `SetDelegate`, `TransferBoss`,
`SetFreeMove`, `SetMotd` and `Disband`.
- Each is one PG transaction. It bumps `roster_ver` and writes the outbox row
  `evt.<shard>.group.<id>.roster{ver, ops[]}` (§4.2).
- A `Move` with a stale `expect_ver` returns `ROSTER_STALE`, and the panel refreshes, so two officers
  dragging the same pilot cannot both win.
- Invites expire after 60 s. An inviter may have ≤ 20 pending, and an invite to a player who blocks the
  inviter (§1.11) is refused.
- p99 ≤ 100 ms; ≤ 200 roster mutations/s per shard.

**Succession.** When the boss has been linkdead for 60 s or logs out, boss passes to the FC, then to the WC of
the lowest-numbered wing, then to the longest-serving member. A commander who logs out keeps the slot for the
5 min linkdead window (04 §2.4), then becomes a member. A fleet with no online member for 15 min disbands on a
§1.8 timer.

**Fleet adverts.** An advert is a `GroupListing` with `kind: Fleet` (06 §6.13). It lives in the same
`lfg_listing` table and Valkey index, keyed by a goal tag (`Fleet.Goal.Roam`, `.Mining`, `.Defense`,
`.Incursion`, `.Logistics` and so on) instead of an activity.
- It adds a **join ACL**: public, corp, alliance, standing ≥ x toward the boss's org, or a character list.
  `Join` evaluates it against `PERMS` and standings. A match joins directly into the advert's default squad
  with no invite; a miss becomes an application to the boss.
- ≤ 1 advert per fleet, with the 30 min lifetime that the boss's client refreshes, and ≤ 5k fleet adverts
  per shard.

**Roster fan-out.** The service projects every roster into KV `GROUP` (`<group_id>` → a snapshot of ≤ 16 KiB
at 256 members, holding character, position, slot and rights; history 1) in step with the outbox event.
Consumers:
- `helios-voice` forwarders and gateways, for the `fleet:<id>` channel's talk levels and mute rights (04 §2.7);
- chat, for §1.10's fleet channel, whose membership is the roster;
- cells: a cell watches the `GROUP` key of every fleet that has a member it owns (04 §6.6 relations), sets that
  member's `FleetMembership{fleetId, wing, squad, slot, rights, rosterVer}` component (06 §8.3a) at the next
  tick boundary, and ignores a snapshot older than the one it holds;
- clients, through the gateway service lane (`FleetVM`, 08 §1.7.2).

A roster change reaches every consumer at **p99 ≤ 1 s**. The panel's location column (zone, docked or in
space, hull) comes from presence (§1.11), which fleet members refresh on change, at most once every 5 s.

**Scale per shard.** ≤ 2,000 fleets and ≤ 100k fleet members. A 256-member fleet forms from an advert in
≤ 60 s (≈ 4 joins/s). KV `GROUP` stays ≤ 32 MB. Fleets never cross shards.

**What the service does not do.** It never moves ships, applies boosts or routes broadcasts; cells do that from
the roster (06 §8.3a). During an outage of this service, fleet warp, broadcasts and bursts keep working with the
last roster, and only roster edits fail with retry-after.

### 1.13 Persistence gateway & fence — shard — Phase 1
The SWG DatabaseServer / Star Citizen Scribe role (R04-P0-8, R07-P0-8). It also owns the **Fence**
(04 §6.1).

**Fence** (the AG model is specified in 04 §6.1).
- `authority_fence(ag_id, root_ag, owner_cell, owner_region, owner_lease_gen, epoch, kind, state,
  parked_seq)` lives in the shard primary, with `state ∈ {active, dormant}` (04 §6.1; DDL in §3.2). Only persistent AGs have rows; transients are fenced by the region lease. Rows are created by the
  service transaction that creates the AG (character create, asset mint, ledger `CreateAg`).
- **Tree invariant:** every row of an AG tree (a ship and the characters or craft aboard) carries the root's
  epoch and owner, so custody never moves on boarding and the ledger check reads only the custody AG's row.
- `Fence.Advance(root, expected, next, owner)` is a conditional `UPDATE … WHERE root_ag = $root AND epoch =
  $expected` over the whole tree (p50 ~1 ms, p99 < 5 ms). `Fence.Join/Leave` re-root a member subtree and bump
  both trees' epochs in one transaction. `Fence.AdvanceMany` commits bundle handoffs all-or-nothing.
  `Fence.AdvanceOwnedBy(region, lease_gen, owner)` bulk-advances every **active** row with `owner_region =
  $region AND owner_lease_gen < $lease_gen` for crash recovery (≤ 1 s p99 at 5k AGs, on a partial index over
  active rows). `Fence.MigrateRegion(region, lease_gen, owner, manifest)` is its cooperative variant for
  planned migration (04 §6.7): it also requires `owner_cell` = the source, reports rows missing from the
  source's manifest, and emits `cause = migrate` with no `released`.
- **Dormancy (04 §6.1).** `Fence.Park(root, e, cell, ckpt_seq)` runs after an AG's final checkpoint (logout,
  linkdead expiry, instance or structure unload). It sets the tree's owner fields to NULL, `epoch = e+1`,
  `state = dormant` and `parked_seq`, which also puts its custody items at rest. A load-time `Advance`
  reactivates a dormant row. Taking over an active row is a flagged exception. After a takeover or a recovery,
  this service sends the superseded owner `ctl.<shard>.cell.<id>.released{ag, epoch}`.
- Every operation publishes `evt.<shard>.fence.advanced` via the outbox (bulk events carry ≤ 1,000 AGs).

**Write load and lock contention** (rates in §3.6; A5 Ph4 tests them). Fence writes share the shard primary
with the ledger, and every ledger transaction share-locks a fence row. At the Phase 4 design rates that is
≈ 2.9k fence operations/s (≈ 8k rows/s) beside 10k ledger tx/s. A tree-wide `FOR UPDATE` on a busy tree,
such as a 60-member carrier, would otherwise contend with its members' ledger traffic. Five rules keep both
inside budget:
- **Fence gate (cell side, 04 §6.1).** While a fence operation on a tree is in flight, its owner cell sends
  no new ledger request for the tree's AGs. It holds them until the reply, normally ≤ 5 ms, and stamps
  them with the epoch the reply returns.
  - A tree-wide `FOR UPDATE` therefore waits only for ledger transactions already in flight. PostgreSQL lets
    a compatible share locker pass a waiting exclusive locker, so without the gate a stream of share lockers
    could starve the advance of a busy tree.
  - The gate also removes the `FENCE_STALE` retries that `Join` and `Leave` would otherwise cause.
  - A handoff needs no hold, because the tree is frozen from tick T (04 §6.3).
- **Fence check last.** `Execute` writes its items and currency first. Its last statement before commit reads
  the custody rows `FOR SHARE`, so each share lock lasts only through the commit (≈ 1–2 ms with the
  synchronous standby). A transaction that reaches a row an advance holds waits, sees the new epoch and fails
  with `FENCE_STALE`. The new owner then re-issues it under the same idempotency key. From Phase 3 the check
  also requires the `owner_cell` of the AG named in `Helios-Fence` to be the authenticated caller (§6.5).
- **One lock order.** Every statement locks fence rows in ascending `ag_id`:
  - `Advance`, `Join` and `Leave` run `SELECT … ORDER BY ag_id FOR UPDATE` over their tree or trees before
    they update;
  - a two-party ledger transaction locks its custody rows in the same order.

  Fence and ledger transactions therefore cannot deadlock each other.
- **Bounded waits.**
  - Fence transactions set `lock_timeout = 50 ms` and return `FENCE_BUSY`. The caller retries after 5 ms, up
    to 3 times, which fits inside a handoff's 250 ms accept window.
  - Ledger transactions use 100 ms and retry under their idempotency key.
  - `fence_lock_wait_ms` is exported per operation, and `FenceLockWaitHigh` fires when its p99 exceeds 3 ms
    for 5 min.
- **Table tuning.** `authority_fence` holds ≈ 10 M rows at Phase 4, ≈ 3 GB with its three indexes, and stays
  resident in memory.
  - `fillfactor = 70`, so epoch-only changes such as an abort re-promotion stay HOT.
  - Autovacuum runs at a 1 % scale factor with no cost delay on this table.
  - `pg_prewarm` runs at start and on the standby's promotion, so the first advances after a failover never
    read disk.
  - `multixact_offset_buffers` and `multixact_member_buffers` are raised (PG 18). Concurrent share locks on
    one custody row create MultiXacts, and `FenceMultixactAgeHigh` warns at 40 % of
    `autovacuum_multixact_freeze_max_age`.

**Checkpoints.**
- Cells publish `persist.<shard>.<zone>.<cell>` batches every 1 s. NATS deterministic subject partitioning
  maps zones onto the streams `PERSIST_0…7` (§2.3), which spreads stream leaders across nodes.
- A dirty AG is included every 30 s (10 s by Phase 5), and immediately on logout or zone exit.
- Record format: `{ag_id, epoch, seq, schema_ver, zstd blob, item_refs[]}`.
- **Blob budget per AG (zstd):** characters and small ships have a mean ≤ 2 KiB and p99 ≤ 16 KiB. Capital
  ships and structures with interiors get ≤ 64 KiB. A record > 256 KiB is sent alone and raises
  `CheckpointOversize`. The cook lints each record template's worst-case estimate against its class budget.
- The gateway flushes every 250 ms or 2,000 records: `COPY` into a temp table, then one merge
  `WHERE (excluded.epoch, excluded.seq) > (t.epoch, t.seq)`.
- Records whose epoch is below the fence are rejected and alerted. The fence lives in the shard primary, so the
  gateway validates against a **fence cache** fed by `evt.<shard>.fence.advanced`, with one batched synchronous
  read of the primary per flush on a miss. A cell that gains an AG writes an immediate `(e+1, 0)` record in its
  next batch, which closes the window before the event arrives (04 §6.1).
- **Region checkpoints** (transients and spawners, 04 §6.1) are keyed by region and carry `lease_gen`. A
  record below the region's current generation is rejected, which is one way a superseded cell learns that it
  lost the region (§1.4.2).
- `ag_checkpoint_hist` keeps a **sampled** history (§3.3).

**Load barrier.** `Checkpoint.Load(ag | zone, min_stream_seq)` answers only after the zone's partition
stream is applied up to that sequence, normally in under 1 s. A standby therefore never loads a stale
checkpoint.

**Ledger wins.** On load, the cell reconciles `item_refs` against ledger custody:
- items the ledger places elsewhere are dropped;
- custody items missing from the blob are respawned.

**Lifecycle.** Every persistent **record template** declares despawn, decay and retention rules. A cleanup
job expires AGs and destroys their value via the ledger with reason `decay` (R04 §3.2).

### 1.14 Content / publish — global — Phase 1 pins, Phase 2 compat epochs, Phase 3 live edit
**Owns:**
- builds: the git commit, schema and record-DB hashes, and each build's **client part**, **server part** and
  **compat epoch** (§1.14.1);
- channel pointers;
- pins, which map (shard, zone, instance kind) to a compat epoch and a server part;
- edit changesets.

**Source of truth:** git (ADR-006). CI registers builds here.
- **Play instances pin a published build**: its epoch for life, and its server part until a hotfix swaps it.
- The **edit instance** journals changesets, which the editor exports to git (R03-P0-3/4).

**API:** `RegisterBuild`, `Promote`, `Pin`, `SwapServerPart(scope, build, stage)`, `SubmitChangeset`,
`Rollback`.

**Hot reload:** cells swap record-DB and Luau deltas at a tick boundary, in ≤ 2 s (R03-P0-5, ADR-012). In dev
this covers any content. On live shards it covers only the server part (§1.14.1).

**Collab service (Phase 2 locks and presence, Phase 3 edit instances).** It runs in this domain and is
specified in 07 §1.8:
- it is the only writer of the JetStream stream `COLLAB_<session>` and the KV buckets `LOCKS_<session>` and
  `NOTES_<session>`, and of the read-only home mirror `HOMES_data`. There is one session per zone edit instance
  and one project-wide **data session** that homes documents with no zone (records, quests, dialogue, string
  tables) and holds their soft locks and presence. Each document has one home session at a time, and home
  changes are fenced handoff and adopt events on the streams (07 §1.8.2);
- it validates with the generated Go validators for native packages, and with the `pkg/htypes` interpreter for
  dynamic project packages (02 §3.8), so the prebuilt service accepts project types;
- it serves **overlay content versions** (base build + journal range), which dev play instances pin for
  publish preview. These are unrelated to the server part.
- it keeps unpublished work durable (07 §1.8.1): `COLLAB_*` streams are R3 on the `ha` Compose profile and on
  Kubernetes; journal segments (every ≤ 30 s) and snapshots go to object storage, and a WIP git ref is pushed
  every 10 min. Session actors are placed by a `collab_owner` PG lease with §1.4.1's term fencing, so a replica
  takeover completes in ≤ 8 s. `helios-tool collab restore` rebuilds a stream, and the runbook
  `deploy/runbooks/collab-restore.md` is drilled quarterly.

Production channels accept only CI builds.

#### 1.14.1 Content versions and live compatibility
Live ops needs two things that a single content hash cannot give together:
- server-only hotfixes with no client patch (AAA-ITR-8, 07 ED-13);
- client builds staged by `rollout_pct` over days (§7).

It also must not split a single-shard world into per-version copies of a zone. So a build has three parts:

| Part | Holds | Delivered by | Identity |
|---|---|---|---|
| **Client part** | cooked assets; the client-visible projection of the record DB; client Luau and UI; predicted-ability programs; schema and tag registries | CDN manifest (§7) | `manifest_hash` |
| **Server part** | server-only records (loot, spawns, AI, vendor and economy tuning, `ZonePartitionDef`); server Luau; tuning values | content service → cells, never the CDN | `server_hash` |
| **Compat epoch** | `compat_epoch` (u32) and its `compat_fingerprint` | pointer, token, instance pin | the epoch number |

**Compat fingerprint.** A BLAKE2b-256 hash over the client↔server contract, which is everything both sides
must agree on:
- replication descriptors, field IDs and the protocol version (04 §2.3);
- the IDs and fields of every client-visible record;
- the tag registry (06 §1.1);
- predicted-ability programs, and the movement, collision and physics parameters that prediction uses,
  including the physics library build (Jolt version and determinism defines, 04 §6.7 `sim_abi.physics`);
- every zone's client-visible geometry and collision.

Server-only records, server Luau and server tuning are outside it.

**Rules.** CI, `RegisterBuild` and `Promote` enforce them.
- **Epochs.** Builds with equal fingerprints share an epoch. A changed fingerprint needs `compat_epoch + 1`,
  and `RegisterBuild` refuses a build that changed it without the bump.
- **What a server part may reference.** Only records present in its epoch's client projection. A lint checks
  every replicated component and spawn, so a server hotfix can never spawn something a client cannot draw.
- **Pinning.** Tokens pin `(compat_epoch, manifest_hash)` (§1.3). netcode's `protocol_id` is derived from
  the protocol version and the compat epoch.
  - Gateways reject only an epoch mismatch.
  - Placement matches the epoch, never the client build.
  - A ZoneInstance pins its epoch for life. It records its current server part, which can change under it at
    a tick boundary.
  - A swap never changes a running instance's regions. A `ZonePartitionDef` change applies only to instances
    created after it (§1.4.2).
- **Audit.** The ledger's `content_build` column records the server part in effect (§4.5).
- **Hotfix time budget (≤ 15 min, AAA-ITR-8).** Incremental cook and validation of the server part take
  ≤ 5 min. The canary zone soaks for 5 min, the 10 % stage for 2 min, and 100 % is reached ≤ 2 min later.
  Each stage is gated on error rate, tick budget and the economy guard (§6.3).

**Four kinds of release.**

| Kind | What changed | Path | Players see |
|---|---|---|---|
| **Server hotfix** (AAA-ITR-8, ED-13) | server part only | `SwapServerPart` in stages: canary zone, then 10 % of instances, then 100 %. Each cell swaps at a tick boundary (≤ 2 s) through hot reload. Rollback swaps back | nothing: no patch, no disconnect, no re-placement |
| **Compatible client build** | client part, same fingerprint | pointer `rollout_pct` over days; `min_client` retires old builds | a patch at next launch; old and new clients share every instance |
| **Server binaries** (cells, gateways, services) | images, same epoch | Cells: rolling N/N+1 restart by planned region migration, hitch ≤ 1 s per region moved (one per single-cell zone, n per n-cell zone; AAA-STB-6, 04 §6.7). Gateways: `DrainGateway` per box, each session relocated make-before-break (§6.3.1). Go services: canaries (§6.3) | nothing from Phase 4. In Phase 3 a gateway roll is a paced reconnect in an announced low-CCU window: a ≤ 3 s "Reconnecting" overlay, bounded at 10 s (§6.3.1) |
| **Epoch change** | the fingerprint | coordinated flip through `next.available_at` (below) | a pre-download, then a short relaunch at the flip |

- **`rollout_pct` may stage only builds of the live epoch.** The patch manifest service refuses
  `rollout_pct < 100` for a build of any other epoch. A 5 % staged client therefore never needs its own copy
  of a zone.
- **Hotfix changesets are classified by CI** (T27 staged live edits, 07 §1.8):
  - server part only → server hotfix;
  - client-visible but same fingerprint → compatible client build;
  - fingerprint change → refused as a hotfix.

**Coordinated epoch flip** (E → E+1). Flips are batched into major releases, about every 6–8 weeks.
1. **Prepare.**
   - The build passes PTR on E+1.
   - The pointer publishes `next{build_id, manifest_hash, compat_epoch: E+1, available_at}` ≥ 72 h ahead,
     and launchers pre-download it (08 §2.6).
   - The orchestrator pre-starts E+1 processes: one standby per zone, plus the warm pool.
2. **Count down.** From T−15 min, launcher and in-game banners count down. From T−5 min, new instanced
   content (activities, phases) is no longer created on E.
3. **Flip clients at T.**
   - The pointer flips for 100 %, and session issuance requires E+1.
   - Gateways disconnect E sessions with reason `CONTENT_EPOCH_FLIP` and a reconnect ticket (5 min, no
     queue).
   - The client applies the pre-downloaded build (renames only, 08 §2.5 step 7) and rejoins.
4. **Roll the world in place, meanwhile.** Each region migrates to an E+1 process by planned region migration
   (04 §6.7; N↔N+1 residuals, append-only schemas). A zone is served by E or by E+1, never by both.
5. **Targets.**
   - Every zone is on E+1 within ≤ 10 min.
   - A player is back in the world ≤ 3 min after the flip, without queueing. The client relaunch dominates.
   - 0 value errors.
6. **Rollback.** Back to E only if E+1's migrations were expand-only; otherwise forward-fix.

The flip is the only planned client interruption. Server hotfixes, compatible client builds and server
binaries never interrupt a session. For gateway binaries and gateway host maintenance this holds from
Phase 4, through relocation (§6.3.1). Acceptance: A14's Phase 4 clauses, which measure AAA-ITR-8 and
AAA-STB-6.

### 1.15 Live config, flags, kill switches, calendar — Phase 1
**Store:** `config_entry(scope, key, value JSONB, version, author, reason)`, with history.

**Delivery:** projected into KV `CONFIG` and applied within 1 s.

**Kill switches:** each names a feature: `econ.trade`, `market.region.<id>`, `mail.attach`. From Phase 4,
economy switches need two approvers.

**Calendar (R05-P1-16):** publishes season, reset and event triggers as `evt.<scope>.calendar.<event>`.

### 1.16 Telemetry, economy dashboards, trust — Phase 1 basic, Phase 2 ClickHouse
**Ingest:** `tel.<scope>.<source>.<type>` events are batched every 100 ms into ClickHouse (a PG table in
dev).
- Ingest replaces account and character IDs with the account's random `analytics_id` (§6.6). The only
  exception is the pseudonymous `trust` stream, whose detectors act on accounts (§1.16a).
- It truncates IP addresses to /24 (IPv4) or /48 (IPv6).
- Raw events are kept 13 months. Aggregates carry no IDs.

**Economy dashboards:** ledger events feed version-controlled Grafana dashboards (R01 §5, R09 A8):
- faucets and sinks by reason code, per hour;
- money supply and velocity;
- price indices;
- destruction.

**Trust (Phase 3: report evidence, cases, cheat features; Phase 4: rules and detectors):** stores report
evidence, including 04 §2.7's voice-ring snapshots, and opens §1.17's cases. From Phase 4 it also consumes 04 §9
violation telemetry and ledger events.
- Economy rules cover wealth jumps > 6σ, item-count drift and RMT graphs.
- §1.16a's detectors cover aimbots, triggerbots, input automation and farm bots.
- A rule or detector hit opens a case. An economy rule can also trip a kill switch.

**Transport:** JetStream until 200k events/s, then evaluate Redpanda (R07 flag 4).

### 1.16a Trust detection: cheats and bots — Phase 3 features and corpus, Phase 4 detectors and ban waves
**Why.** Authority (04 §9) stops a client from asserting value, movement or hits, and AAA-SEC-3 catches speed
and teleport hacks. Neither catches input that is legal but not human:
- aimbots and triggerbots in Destiny-class PvP;
- input automation;
- the mining, ratting, mission and gathering bots that inflate EVE- and SWG-class economies.

R05 records that Bungie eventually needed kernel anti-cheat, and a native Linux client (08) limits which
vendors can serve it. Detection is therefore **server-side first**, built from data the cell already holds. A
client vendor adds signals, but no detector depends on one (04 §9).

**1. Features, computed in cells.** Each session has a `TrustFeatures` accumulator, updated from the input
stream and from 04 §5.6's lag-compensation resolve. Clients send no new data.

| Family | Features (per session, per 60 s window) | Source |
|---|---|---|
| Aim | **snap angle**: view change in the 100 ms before a shot. **Time to target**: from the target's first unoccluded frame in the shooter's rewound view to the first on-target shot, per weapon class. **On-target dwell before fire**: triggerbot latency and its spread. **Hit and precision-zone rate** by distance band against target visibility, including the share of shots and pre-aim at targets occluded in the rewound view (ESP). **Silent-aim mismatch**: hits the server validates although the view never crossed the target | look yaw/pitch and `view_tick` per input (04 §5.2); rewind history and occlusion test (04 §5.6) |
| Input | inter-input interval entropy and coefficient of variation (1 ms bins); exact repeats of input sequences ≥ 2 s long; the look-angle jerk spectrum; **`device_class` consistency**: stick-quantized look deltas versus mouse deltas, since aim assist depends on the declared class (04 §5.2) | the input stream |
| Routine | route and loop periodicity (autocorrelation of 10 s position samples over 30 min, per zone); action cadence regularity; **stimulus response**: time from an unscripted event (NPC aggro, a node that depletes early, a local-chat mention) to a changed action; session length and daily active hours | positions, actions and events the cell already has |
| Economy | yield per active hour against the zone's population percentile; faucet mix | ledger events (§1.6), joined in Trust |

- **Cost:** ≤ 1 % of the cell's tick budget at 500 players, measured in NS-4.1.
- **Output:** one ≈ 200 B summary per session per 60 s on `tel.<shard>.<proc>.trust`, ≈ 0.2 MB/s at 50k
  CCU. The subject's process token is authenticated (§6.5), so no process can forge another's features.
- **Evidence:** a window that crosses a cheap in-cell pre-filter also sends its shot-level trace and asks the
  cell to flush its tick-recording ring for that session, as a report does (04 §10.2).
- **Normalization:** features are normalized per `device_class`, weapon class and activity, so pad aim assist
  and PvE auto-targeting are not flagged.

**2. Scoring (Trust, Go).** Session features are stored in ClickHouse under the pseudonymous `account_id` for
180 days (§6.6). The `trust` stream is the one telemetry stream that keeps `account_id` instead of
`analytics_id` (§1.16), because Trust acts on accounts. It is still pseudonymous. There are two detector tiers:
- **Streaming rules**, which flag within ≤ 15 min. Initial thresholds, re-tuned on the corpus:
  - silent-aim mismatch > 2 % over ≥ 200 shots;
  - on-target-to-fire latency with p50 < 60 ms and IQR < 20 ms over ≥ 100 shots;
  - input-interval entropy below the honest floor for ≥ 30 min.
- **Daily models**, which flag within ≤ 24 h: gradient-boosted trees per family (aim, input, routine). They are
  trained offline on the labelled corpus, evaluated in Go (`pkg/trust`), versioned, and changed only through
  a reviewed change.

A score above its detector's threshold opens a `trust_rule` case (§1.17). The case carries the feature
vector, the top contributing features and linked evidence (shot traces, tick recordings, ledger references).
Each threshold is set for ≤ 0.1 % false positives on the honest populations below.

**3. Labelled corpus.**
- **Cheats (red team).** `helios-bot` variants (04 §10.3), each at humanization levels 0–3 (timing jitter,
  route noise, breaks):
  - the seam fighter with aim cheats: a snap aimbot with 0–150 ms Bézier smoothing, silent aim, a
    triggerbot with a randomized 50–150 ms delay, and ESP-driven pre-aim at occluded targets;
  - input macros (ability rotations with jitter);
  - farm bots: mining loops, ratting and mission runners, gatherers.
- **Honest.**
  - NS-4.1's bot swarm in `--human-model` mode, which replays recorded human input timing and aim onto the
    bots' goals. It is used for the aim and input detectors only, because scripted routes are periodic by
    design.
  - ≥ 5,000 human sessions from internal playtests and the Phase 3 beta, reviewed as clean. They are used
    for every detector.
- Feature logs and tick recordings are versioned. The red team adds variants every quarter. A nightly job
  scores every detector on every corpus version and fails on a recall or false-positive regression.

**4. Delayed ban waves (§1.17).** Detections are not acted on at once, so cheat authors cannot bisect the
detectors.
- A `gm` reviews each `trust_rule` case (P3 SLA). A confirmed case stages its sanction in
  `ban_wave(wave_id, scheduled_at, state, case_ids[])`.
- A wave runs at a randomized 7–21-day interval. Before it runs, a dry run lists the accounts, the sanctions
  and the value to remediate, and two people approve it: `senior_gm`, plus `economy` for remediation.
- Execution:
  - `LinkSanction` runs per case, so each ban carries its case ID;
  - farmed value is remediated through ledger `Reverse` or `Burn` with reason `Sink.Trust.Remediation`, and
    the conservation audits still hold (§4.4);
  - the account's entries in open leaderboard windows are voided.
- **Immediate action** is reserved for economy-threatening cases (RMT rings, exploit dupes): a kill switch
  (§1.15), an account lock, then the normal case.
- Appeals are `appeal` cases. A detector pauses (`TrustDetectorPaused`) until it is re-evaluated on the
  corpus when appeals overturn > 2 % of a wave's sanctions for it, or overturn any of its high-confidence hits.

**5. Client vendor (`IAntiCheatProvider`, 08 §1.14).** The vendor is chosen in **Phase 3 by WP-3.11**, not
Phase 4. Its Linux support constrains the native Linux client, and a kernel-mode choice needs counsel and
platform review before the public beta.
- **Criteria:** native Linux and Proton support; kernel or user mode; licence (09 K19's EOS/EAC sign-off); a
  server-side signal API.
- **Candidates:** EAC (via EOS) and BattlEye.
- Vendor detections and attestation failures enter Trust as one more feature family. A player without a vendor
  module, on Linux for example, is scored on server features alone.

**Phasing.** Phase 3: cell features, the `trust` telemetry stream, corpus v1 and the vendor decision
(WP-3.11). Phase 4: detectors, ban-wave tooling, vendor integration and A21 (WP-4.8).

### 1.17 GM/admin API & audit — Phase 1 commands, Phase 4 full
**RBAC roles:** `support`, `gm`, `senior_gm`, `economy`, `ops`, `dev`, `privacy` (DSAR handling). MFA is
required in prod.

**Actions:**
- kick, mute, ban, teleport and spawn, sent via `ctl.<shard>.cell.<id>.gm` and applied at a tick boundary;
- ledger history, restore and reverse;
- rollback (§4.5);
- moderation and config (R07-P0-12, R08-T27).

**Audit:** every call requires a reason and appends to a hash-chained `audit_log`
(`h_n = BLAKE2b(h_{n-1} ‖ row)`). The chain head is anchored daily in object storage.
- Rows hold only pseudonymous IDs.
- Free text (GM reasons, notes, report excerpts) lives in `audit_note`, encrypted under the subject's key.
  The chained row carries `note_digest = BLAKE2b(ciphertext)`, so crypto-shredding leaves the chain
  verifiable (§6.6).

**Cases: player reports and support tickets (Phase 3).** One queue serves player reports, support tickets,
trust-rule hits (§1.16) and appeals. The player side is 08 §1.7.2's Support and report panel; the staff side
is the admin console (§5) and T27 (07).
- *Model.* `case(case_id, kind {report, ticket, trust_rule, appeal}, category, priority P1–P4, state {new,
  triaged, in_progress, waiting_player, resolved, closed}, shard, subject_account?, subject_character?,
  reporter_count, assignee_role, assignee?, opened_at, first_response_due, resolve_due, first_responded_at,
  resolved_at, sla_paused_ms, breached_first_response, breached_resolve, linked_sanctions[], merged_into?)`.
  Messages and evidence are `case_item(case_id, seq, author {player, staff alias, system}, kind {text,
  chat_excerpt, voice_clip, ledger_ref, receipt_ref, client_log, screenshot}, ciphertext, digest, verified)`,
  encrypted under a per-case key and kept 2 years after closure (§6.6).
- *Player API.* `FileReport(subject_character, category, note ≤ 500 chars, chat[≤ 20 {msg_id, channel,
  sender, sent_at, body, tag}], voice_evidence_id?, context{zone, pos, tick})`, `OpenTicket(category,
  text ≤ 2,000 chars, attachments)`, `ListMyCases`, `GetMyCase`, `ReplyToCase` and `CloseMyCase(rating 1–5)`.
  `OpenAccessTicket` is for players who cannot sign in: it is unauthenticated, rate-limited per IP and email,
  and replies go by email. The email stays in Identity, and the case keeps a pseudonymous contact handle.
- *Limits.* Per account: 10 reports an hour, 3 per subject a day, and 5 open tickets. A report on the same
  subject and category within 24 h merges into the open case and raises `reporter_count`. Five distinct
  reporters raise the priority one step.
- *Evidence is verified, not trusted.* Chat lines need a valid tag (§1.10). Voice evidence must be a ring
  snapshot the forwarder took (04 §2.7). Ledger and receipt references must belong to the ticket's account.
  Anything unverifiable (a screenshot) is labelled player-supplied.
- *SLA targets* (first response / resolution, per product in live config, §1.15). The clocks pause in
  `waiting_player`.

  | Priority | Categories | First response | Resolve |
  |---|---|---|---|
  | P1 | Threat of real-world harm, child safety, account takeover in progress | ≤ 1 h, 24/7 | ≤ 24 h |
  | P2 | Harassment or hate with evidence, compromised account, billing | ≤ 8 h | ≤ 72 h |
  | P3 | Cheating, botting, RMT, item or currency loss, stuck character | ≤ 24 h | ≤ 5 d |
  | P4 | Offensive name, other, feedback | ≤ 72 h | ≤ 14 d |

  A breach sets its flag, escalates the case to `senior_gm` and counts in `case_sla_breach_total`, which has
  a burn-rate ticket alert (§6.8).
- *Staff API.* `QueueCases(filter: priority, category, state, shard, assignee, due_before)`, `Claim`,
  `Assign`, `Transition`, `Reply` (from localized macros), `Merge`, `ViewEvidence` and `LinkSanction`.
  `ViewEvidence` decrypts and is access-logged in `audit_log`; `support` may open tickets and `gm` may open
  reports. `LinkSanction` runs a §1.17 action whose audit reason carries the `case_id`. Reporters see only the
  state and "action taken" or "no action", never the sanction.

### 1.18 Public read API (ESI-style) — Phase 4
**Transport:** REST/JSON on chi, documented with OpenAPI 3 (R01-P1-17, R05-P2-23).

**Auth:** OAuth2 auth-code + PKCE, with read scopes per domain (`wallet.read`, `assets.read`, …).

**Caching:** `ETag`/`Expires` follow projection refresh (market 5 min, assets 1 h). Responses come from
caches and read models, never the primary.

**Rate limits:** floating window per (route group, app+character or IP). Token cost: 2xx=2, 3xx=1, 4xx=5,
5xx=0 (R01 §4).

### 1.19 Patch manifest service — Phase 0 tool, Phase 2 prod
Serves signed pointers and manifests, controls staged-rollout percentages (R10 §9), and hosts
`helios-patch publish` (§7).

### 1.20 Entitlements/store — Phase 1 stub, Phase 4 real
Grants are idempotent by receipt, and premium currency is its own ledger currency. Refunds become
compensating transactions. A webhook payment provider keeps us out of PCI scope (R07-P1-17).
- The provider holds invoices and payment PII.
- We keep a pseudonymous receipt `{receipt_id, account_id, amount, currency, country, time}` for 10 years
  (tax).
- Accounts aged 13–17 have store spending caps (§6.6). The defaults are €50 (13–15) and €100 (16–17), or the
  local equivalent, per calendar month, with per-country overrides in live config (§1.15).
- **Store API (Phase 4; 08 §1.7.2 Store):**
  - `ListOffers(locale, country)` serves `StoreOfferDef` records published as content (§1.14): SKU, grants
    (entitlements, premium currency, or a season premium lane per 06 §5.3), provider price IDs per currency,
    window, a `randomized` flag, and age and country rules.
  - `CreateCheckout(offer, price_version, idem)` checks the cap and returns `{checkout_id, provider_url,
    expires_at}` (30 min). Above the cap it returns `SPEND_CAP_EXCEEDED{remaining, resets_at}`. The provider's
    signed webhooks (`paid`, `refunded`, `chargeback`) grant or compensate idempotently by `receipt_id`.
    `CheckoutStatus` streams `pending → paid → granted`, or `failed`, `cancelled` or `expired`, to the client.
  - `SpendWithPremium(offer, price_version, idem)` is one ledger transaction in premium currency.
  - `ListEntitlements`, `ListReceipts`, and `GetSpendStatus → {band, period_end, cap, spent, remaining}`.
  - Randomized offers always publish their odds. They are refused for 13–17 accounts and in countries listed
    in live config.
  - The dev backend (§5) runs a fake provider that signs webhooks with a dev key, for 08 §1.7.3's flows.

### 1.21 World state — shard — Phase 2 (flags, meta-events), Phase 3 (influence, territory), Phase 4 (sovereignty)
Implements 06 §6's `WorldFlagDef`s and cross-zone meta-events, and owns 06 §9's influence and sovereignty
accumulators (R09-A9; SWG GCW, R02; the Elite BGS tick, R04).

**Owns** (schema `svc_world`):
- `world_flag(scope_kind, scope_id, flag_id, value BIGINT, blob JSONB, version, updated_by, expires_at)`,
  scoped to shard, zone or region per `WorldFlagDef`;
- `meta_event(event_id, def_id, phase, zones BIGINT[], progress JSONB, version, deadline)`;
- `influence(faction_id, region_id, value_fp BIGINT, t_last_ms)` and
  `sov_state(system_id, owner_org, levels JSONB, since, version)`;
- `territory_structure(structure_id, def_id, zone, owner_org, state, layer, since, reinforce_until, window
  JSONB, pending_window JSONB, window_at, capture JSONB, version)` and the append-only
  `territory_audit(structure_id, seq, from_state, to_state, cause, evidence JSONB, seed, at)` (06 §9.3);
- the `RegionEconomyState` record written by the Economy Sim (§1.22).

**API:**
- `SetFlag(scope, flag, value, expect_version?, idem, fence?)` and `AddFlag`, a clamped increment.
- `GetFlags(scope)`.
- `MetaEvent.Start`, `MetaEvent.Report(event, zone, objective, delta, idem)` and `MetaEvent.Advance`.
- `AddInfluence(batch)` and `SetSovereignty`.
- `Territory.Transition(structure, from, version, to, cause, evidence, idem)` from the hosting cell, with its
  `(region, lease_gen)`; `Territory.SetWindow`; `InstallUpgrade` and `RemoveUpgrade` (Phase 4). Timer-driven
  transitions (anchoring, reinforcement exit, vulnerability end, window changes, weekly sovereignty fuel) are
  §1.8 timers that this service consumes itself.

**Write rules:**
- Cell callers carry their region's `(region, lease_gen)`, so a superseded cell cannot flip world flags.
- `WorldFlagDef.writers` restricts who may write (cell, service, GM, calendar, or a named world script).
- A **keyed** def adds a key suffix, so one def holds many flags. 06 §9.2.4's city footprints are
  `zone.<zone>.City.<cityId>`, written only by the `CityGovernance` world script with `expect_version`, and
  06 §9.3's territory states are `zone.<zone>.Territory.<structureId>`.
- GM and live-ops writes go through §1.17 audit.

**Delivery:**
1. Writes go through to PG. No value is involved.
2. The outbox publishes `evt.<shard>.world.flag_changed`.
3. The service projects KV `WORLD` (`<scope>.<id>.<flag>`).
4. Cells watch the keys for the zones and regions they host and apply changes at a tick boundary.
5. Replicated flags reach clients on each zone's `WorldFlags` singleton component, with the audience set
   per def.

After a KV loss, `WORLD` is rebuilt from PG in ≤ 30 s, and cells keep their last values meanwhile. Consumers
of the change events include 06's Event Director, spawners and vendors, the Economy Sim and telemetry.

**Meta-events (06 §6):** the service is the single coordinator.
- Zones report objective progress idempotently.
- A phase advances in one PG transaction with a version CAS, so it happens exactly once. Then
  `evt.<shard>.world.meta.<event>` and the KV update fan out.
- Deadlines are durable timers (§1.8).
- A standby cell rehydrates its zone's view from `WORLD` plus `ACTIVITY` (§1.12) in ≤ 5 s.

**Accumulators (06 §9):**
- Cells batch `AddInfluence` per (faction, region) every 10 s.
- Decay is applied lazily in fixed point, so replays agree: `v ← v·2^(−Δt/half_life) + Σδ`.
- Crossing an `InfluenceDef` threshold, with hysteresis, sets the region's control flags.
- Sovereignty levels consume the region's power and workforce attributes and change only through audited
  transactions.
- **Territory (06 §9.3).** Each transition is one PG transaction: the CAS on `version`, the `territory_audit`
  row, the next timers and the outbox event, which also projects `WORLD`. Entering Reinforced also writes an
  outbox request `PreProvision(zone, reinforce_until, profile)` to the orchestrator (§1.4, 04 §6.7), idempotent
  on `(structure, layer, cycle)`. The hourly audit folds each structure's audit rows and compares the result
  with its row. 06 GP-17 is the acceptance.

**Scale:** ≤ 500 flag writes/s and 50k watched keys per shard. Reads never touch PG.

### 1.22 Economy Sim — shard — Phase 4 hooks, Phase 5 full (R04-P2-20)
Implements 06 §4's economy simulation hooks.
- It consumes `evt.<shard>.ledger.tx` and activity events.
- It runs a per-region tick every 15 min over the `EconomyModelDef` agents.
- It writes only the `RegionEconomyState` record (price indices, scarcity, demand, traffic, mission
  weights) through §1.21. Vendor formulas, spawners and mission generators read it.
- It never writes to the ledger. Its outputs are clamped by the def and watched on the economy dashboards.

### 1.23 World scripts: the studio's backend extension surface — shard — Phase 3 (API 1.0), Phase 4 (scale-out, public reads)
Luau runs in cells, and §1.21's `WorldFlagDef` scalars are the only shard-wide state it can reach. A studio's
own **cross-zone, persistent system** needs more: a player bounty board, a galactic senate with elections
(SWG-style), an org-war declaration market, a shard lottery. Each needs keyed durable state, timers, value
handling and a request/response surface, and 01 §1.1 forbids editing backend source to get them. World scripts
provide all of that from records and Luau.

**Decision.** Option (a), a shard-scope Luau host with declared tables, is chosen over (b), a public Go
service-module API.
- Go's `plugin` package does not support Windows, so (b) would make every studio rebuild `helios-backend`
  with a Go toolchain.
- Studio Go would also bypass the fuel metering, heap caps and determinism that the Luau sandbox enforces
  (02 §7.4).
- Designers already write server Luau (06 §11).

(b) is revisited with the Phase 5 UGC API. The Go service interfaces (§8) stay internal.

**1. Declaration.** A `worldscript` block in the project's `.hschema` package. schemac validates it,
generates typed Luau accessors in the `world` realm (`.d.luau`), and adds it to the server part (§1.14.1):

```
worldscript BountyBoard @version(1) @partitions(4) {
  escrow Credits;                                   // project escrow wallet ws:<project>:BountyBoard:Credits
  table Bounty @key(id: u64) @partitionKey(target) @index(target, status) @index(status, expiresAt)
               @escrowBacked(amount, when: status == Open) @ttl(closedAt + 30d) @maxRows(2_000_000) {
    target: CharacterId @subject; poster: CharacterId @subject; amount: i64 @currency(Credits);
    escrowTx: EscrowClaim; status: enum { Open, Paid, Expired, Cancelled };
    note: EncryptedText(140); expiresAt: Time; closedAt: Time?;
  }
  rpc Post(PostBounty) -> BountyId  @callers(cell) @rate(5 per 10 min per character) @partitionBy(target);
  rpc List(ListBounties) -> list<BountyView> @callers(cell, gm, public) @readOnly @cacheSecs(5);
  on event Killmail @partitionBy(victim);
  timer Expire(BountyId);
  reason BountyPayout: EscrowRelease; reason BountyRefund: EscrowRelease; reason BountyFee: Sink;
}
```

**2. Host.** `helios-cell --role world-script` (the **WSH**) is the cell binary with no zone simulation.
- It loads the script's server-part Luau (`scripts/world/<name>/*.luau`) into **one VM per partition**, under
  the cell sandbox (02 §7.4): fuel metering, heap caps, no I/O, deterministic `hmath`, and a per-invocation
  RNG seeded by the invocation ID.
- **No hidden state.** Module globals are frozen after load, so every durable fact lives in a declared table.
  A hot swap or a failover therefore loses nothing but the row cache.
- Partitions share a pool of ≤ 4 worker threads per WSH, and each partition runs on at most one of them at a
  time.
- **Each partition is an orchestrator region** of kind `ws`, with a region lease and `lease_gen` (§1.4.2), the
  H, T, P and R detection signals, and a warm standby. A crashed WSH's partitions are served again ≤ 10 s after
  confirmation, the cell budget (§1.4.6). A superseded WSH's commits fail the `lease_gen` check.
- **Placement:** in Phase 3 one WSH (plus its standby) hosts every partition of every world script, ≤ 4
  partitions per script. From Phase 4, partitions spread over ≥ 2 WSHs in different zones, up to 16 per script.

**3. Invocations.** Every RPC call, event delivery, timer firing and migration batch is an **invocation**. Its
ID is deterministic: the caller's `Helios-Idem` key for an RPC, the JetStream message ID for an event, the
timer ID for a timer, and `(migration, partition, batch)` for a migration.
- It runs to completion on its partition's single thread, as an actor, so no partition ever has two writers.
- The handler gets a `ctx`:
  - `ctx.tables.<T>`: `get`, `query(index, from, to, limit ≤ 500, cursor)`, `count(index prefix)`, `put`,
    `delete`;
  - `ctx.ledger`: the intents in step 6;
  - `ctx.timers:at(key, when, payload)` and `cancel`;
  - `ctx.emit(event)`;
  - `ctx.world`: `GetFlags`, `SetFlag` and `AddFlag` (§1.21), carrying the partition's `(region, lease_gen)`;
  - `ctx.mail:system(to, template, attachments from escrow)` (§1.9);
  - `ctx.now`, the wall clock captured when the invocation starts, and `ctx.rng`.
- Effects are buffered until commit, so nothing is visible outside before it.
- A read that misses the partition cache suspends the coroutine; the partition's other invocations wait
  behind it, as the actor model requires.
- There are no joins, and schemac rejects a query that no index serves.

**4. Commit.** The WSH batches finished invocations every 10 ms or 128 invocations into
`worldscript.Commit(partition, lease_gen, [ {invocation_id, row_writes[], outbox[]} ])`. The Go `worldscript`
service applies the batch in one PG transaction:
1. `ws_partition.lease_gen = $gen` is read `FOR SHARE`, the market's `owner_gen` pattern (§1.7).
2. Each `invocation_id` is inserted into `ws_inbox`. A retried RPC or a redelivered event therefore commits
   once, and the duplicate gets the stored reply.
3. Row writes are applied with `version` checks, together with index rows.
4. Outbox rows are written for ledger intents, timers, mail and events (§4.2).

RPC replies are sent only after the commit is durable. A version conflict can happen only after a takeover. It
aborts the batch, and the invocations replay on reloaded state.

**5. Storage** (schema `svc_worldscript`, so CONF-06 holds and no project needs its own DDL):
- `ws_table(table_id, project, script, name, version, schema_hash)`;
- `ws_row(table_id, pk BYTEA, partition SMALLINT, version BIGINT, value BYTEA, expires_at)`, keyed
  `(table_id, pk)` and hash-partitioned 16 ways. `value` is Helios-binary with field IDs and `schema_ver`
  (ADR-004), upgraded on read;
- `ws_index(table_id, idx SMALLINT, ikey BYTEA, pk BYTEA)`, keyed by all four and written in the same
  transaction as its row;
- `ws_partition(script, partition, lease_gen)`, and `ws_inbox(invocation_id, day, reply BYTEA)` with daily
  partitions that are kept 4 days (§3.3);
- `ws_dead(invocation_id, script, input, error, attempts)` for review.

The WSH caches its partitions' rows (LRU, ≤ 256 MiB per host). The cache stays coherent because the WSH is
the only writer. A §1.13-style lifecycle job deletes rows past their `@ttl`.

**Quotas per project and shard** (defaults; live config may raise them): 64 tables, 4 indexes per table,
16 KiB per row, 10 M rows and 10 GiB per table, and 2k invocations/s per partition. All world scripts on a
shard share a write budget of ≤ 5k row writes/s, which §3.6 counts; `Commit` defers batches beyond it.

**6. Value safety.** A world script **never debits a character**.
- **Value enters only through a cell.** The player's protected action runs `EscrowOpen` in the cell, under
  the cell's fence (§4.1), from the character's wallet or custody into the script's escrow, and returns a
  single-use claim. The RPC carries the claim, and the handler redeems it with `ctx.ledger:claim(escrowTx)`
  before it writes the row. A claim redeems once, idempotently, and only for the script that the escrow names.
  It binds the escrow to the invocation ID. A reconciler refunds, to the payer, any escrow still unclaimed 15 min
  after it opened, and any claimed escrow whose invocation has no `ws_inbox` row after 15 min, so a crash
  between the claim and the commit never strands value (the market's orphan-escrow rule, §1.7).
- **System jobs.** A §1.8 job that already debits a system-owned pool it is registered for (a structure's
  upkeep pool, 06 §9.2.3) may add an `EscrowOpen` leg into a world script's escrow to that same ledger
  transaction. It hands the claim to the script as the event `ws.<script>.claim`, which the handler redeems
  like an RPC's claim, and the same reconciler refunds an unredeemed leg to the pool. No job ever opens a claim
  from a character's wallet.
- **Value leaves only as these intents:**
  - `release(escrow → character or org wallet, amount, reason)`;
  - item releases, as mail attachments from escrow (§1.9), because items need custody;
  - `sink(escrow, amount, reason)`;
  - `grant(reason, …)`, only for a `ReasonCodeDef` with a `faucet {dailyCap}`. The ledger enforces the cap
    per (project, reason, day); a refused grant raises `WorldScriptFaucetCap`.
- Intents leave through the outbox with idempotency keys `ws:<script>:<invocation>:<n>` (§4.1 rule 4). Their
  results come back to the script as `ledgerResult` events.
- **Audits.** Escrow wallets are system wallets, so the conservation audits (§4.4) cover them. An hourly
  escrow-backing audit checks, for each `@escrowBacked` table, that the escrow balance equals the sum of the
  declared field over the rows that match the `when` condition. A mismatch pages SEV1 and trips the script's
  kill switch.

**7. Callers and events.**
- **Cells** call `World.call("BountyBoard.Post", req)` from server Luau (06 §11). The call is async and
  never blocks a tick. It carries the caller's `(region, lease_gen)` and the acting character, and the reply
  drains through the `SimInbox` (§2.4). The subject is `rpc.<shard>.ws.<script>.<partition>.<Method>`,
  served only by the partition's current holder. Cells emit script events with `World.emit`.
- **Clients** never call a world script directly. A terminal or panel calls it through its cell (08 §1.7.1),
  which validates the player.
- **GM tools:** `helios-admin ws call | rows | edit`, with RBAC and §1.17 audit, and the editor's World scripts
  panel over the same API (07 §1.6.2). An edit is a GM invocation, deduplicated and committed like any other.
  The service refuses an edit that changes a `@currency` field or a field that an `@escrowBacked` condition
  reads, so an edit cannot unbalance an escrow; value corrections go through a handler's ledger intents.
- **Public reads (Phase 4):** a `@callers(public) @readOnly` endpoint appears on the §1.18 public read API. It
  is served from a projection that is refreshed every `cacheSecs`.
- **Subscribable events:** `killmail`, `world.flag_changed`, `world.meta.*`, `calendar.*`, `org.*` (war
  declarations, membership), `activity.result`, `market.trade` (aggregated per minute), `structure.stage`
  (§1.8), and `ws.<script>.*` from cells, system jobs or other world scripts. Each is one durable JetStream consumer per (script, event), with inbox
  dedup (§4.2).
- **Timers** are §1.8 durable timers of kind `ws.<script>.<timer>`, exactly once through the inbox.

**8. Budgets and isolation.**
- **Per invocation:** 5 ms of fuel (02 §7.4's calibrated fuel), a 16 MiB heap, ≤ 256 row operations, ≤ 8
  ledger intents, ≤ 16 timers and ≤ 64 KiB of events.
- **Failures.** A runaway handler is killed at its fuel limit and nothing it did is committed. After 3
  failures on the same input, the input moves to `ws_dead` and pages SEV3. The partition continues.
- **Per host:** ≤ 4 cores. Each partition has a 2k invocations/s token bucket, and an RPC over it gets
  `RATE_LIMITED`.
- **Kill switch.** `ws.<script>` (§1.15) makes the script's RPCs fail fast with `FEATURE_DISABLED` and pauses
  its consumers. Events wait in JetStream for up to 7 days.

**9. Versioning and live ops.**
- **Hot swap.** World scripts are server-part content (§1.14.1). A change that leaves the table shapes alone
  hot-swaps at an invocation boundary: the partition finishes its batch, swaps VMs and resumes, a pause of
  ≤ 1 s. It goes through the canary, 10 % and 100 % stages, counted in partitions.
- **Evolution.** Table and RPC records evolve under ADR-004's rules: additive fields, `@was`, and `@version`
  with `upgrade<T>` hooks. Old rows are upgraded on read. Because cells and WSHs swap at different moments,
  CI refuses a non-additive RPC record change within a compat epoch.
- **Migration.** A non-additive table change needs a declared `migrate` handler. The WSH runs it per partition
  as resumable batch invocations of 1,000 rows each, interleaved with RPCs, so the script keeps serving. `helios-tool
  upgrade-project` runs the same handlers on dev data (09 §2.7.3).
- **API surface.** The public API is the Luau `world` realm plus the `worldscript` schema block. It is versioned
  with the SDK (`1.0` at Phase 3), follows 09 §2.7.2's deprecation rule, and has a generated reference and
  an executable tutorial (09 §2.7.1).

**10. Privacy.** World tables may not hold `@pii` (CONF-07). Player text is `EncryptedText` under the author's
subject key, so erasure crypto-shreds it (§6.6). `worldscript` is a `PrivacyParticipant`: export lists the rows
whose `@subject` fields match the subject, and erasure shreds their text and keeps the pseudonymous IDs.

**11. Dev.** `helios-dev up` starts one WSH beside the dev cell, backed by embedded PG (ADR-014), on Windows
and Linux, and every editor PIE mode starts one too (07 §1.6.2). Handlers debug over DAP like cell scripts,
with one port per partition VM, and `helios-admin ws rows` or the editor's World scripts panel browses the
tables. A WSH started with `--dev` reports a partition stopped at a breakpoint as `DebugPaused`, so the dev
orchestrator suspends its detection signals, and it sends in-progress acks for the JetStream deliveries that the
partition holds. It also streams a per-invocation trace (fuel, row operations, outcome) and keeps re-run records
of its last 10k invocations. These hooks are compiled out of shipping builds.

**12. Proof.** The `starter-sandbox` template ships a cross-zone bounty board as a world script (09 §2.7.4).
Posting happens at terminals in any zone, and kills in any zone pay out. A20 is the acceptance.

**13. Foundation world scripts.** The Foundation layer (01 §4.1) ships `CityGovernance`, SWG-style player
cities (06 §9.2.4): cities, citizens and residences, elections and ballots, permits, an escrow-backed treasury,
city-tax and civic-upkeep reason codes, and the footprint projection into `WORLD`. It is ordinary world-script
content that a studio can override, and it runs on the Phase 3 WSH. 06 GP-16 (d) is its acceptance.

---

## 2. Protocols

### 2.1 RPC

**Go↔Go and HTTPS clients: connect-go v1.21 + protobuf (ADR-014).**
- One `net/http` port speaks gRPC, gRPC-Web and Connect.
- Go gets deadlines and streaming.
- The launcher (WinHTTP on Windows, libcurl on Linux), admin console and browser tools use plain POST+JSON.

**Single schema source.** Services are declared once in `.hschema`. schemac emits:
- `.proto` files, used for connect-go stubs, `buf lint` and `buf breaking` (N↔N+1, R07 §6);
- Helios-binary codecs for C++ and Go.

Generated Go is committed and CI-verified, so Go builds never need the C++ toolchain.

**Cells and gateways: nats.c v3.14 with Helios-binary payloads (ADR-013).**
- Calls are request/reply on `rpc.<scope>.<svc>.<Method>`, with the service name as queue group.
- Headers: `Helios-Idem`, `Helios-Deadline-Ms`, `Helios-Fence` (`ag:epoch:cell`), `traceparent`. From Phase 3 the
  `cell` in `Helios-Fence` must equal the caller's authenticated process ID from the server-stamped
  `Nats-Request-Info` header (§6.5).
- The generated Go binding calls the *same* handler as connect-go.
- No service discovery or HTTP/2 is needed in the sim process (R01-P1-11).

### 2.2 Subject naming
Grammar: `<class>.<scope>.<domain>[.<id>…].<verb>`.
- `class` ∈ {rpc, evt, ctl, persist, tel, chat, presence, content, timer, audit}.
- `scope` = shard (`eu1`) or `g` (global).
- Lowercase tokens only.
- Environments are separate NATS **accounts** (a `svc` and a `sim` account each, §6.5), never subject prefixes.

| Purpose | Example | Kind |
|---|---|---|
| RPC | `rpc.eu1.ledger.Execute`, `rpc.eu1.orch.Heartbeat`, `rpc.eu1.market.17.Exec`, `rpc.eu1.ws.BountyBoard.2.Post` | core req/reply, queue group (heartbeats: all orchestrator replicas subscribe; a market or world-script partition subject: only its owner) |
| Event | `evt.eu1.ledger.tx`, `evt.eu1.fence.advanced`, `evt.eu1.world.flag_changed` | JetStream |
| Control | `ctl.eu1.cell.4411.drain`, `ctl.eu1.gateway.all.kick`, `ctl.eu1.gateway.7.probe` | core |
| Checkpoints | `persist.eu1.z1002.c4411` | JetStream (partitioned) |
| Chat / presence | `chat.g.corp.98001`, `presence.g.123456`, `chat.eu1.fleet.5512` (fleet chat and broadcasts, 06 §8.3a) | core |

### 2.3 JetStream streams and KV (R3 in prod, R1 in dev)
Sizes follow §3.6's Phase 4 model. Streams alert at 70 % of `max_bytes`.

| Stream / bucket | Subjects | Retention | Limits | Dedup |
|---|---|---|---|---|
| `EVT` | `evt.>` | limits | 7 d, 1.5 TB | 10 min (outbox id) |
| `PERSIST_0…7` | `persist.>`, partitioned by zone | limits (replay buffer) | 1 h, 300 GB total | 2 min (`ag:epoch:seq`) |
| `TIMERS` | `timer.>` | work-queue | — | 10 min |
| `TEL` | `tel.>` | limits, **R1** (drop-and-count data) | 24 h, 2 TB | — |
| `AUDIT` | `audit.>` | limits (PG keeps forever) | 30 d | 10 min |
| KV `DIRECTORY` `CONFIG` `PERMS` `ACTIVITY` `WORLD` `GROUP` | route projection (from PG), flags, masks, activities, world flags, party and fleet rosters (§1.12.1) | — | history 5/10/1/5/5/1 | — |

There is no `LEASES` bucket: generations and leadership live in PG (§1.4).

Consumers are durable pull consumers with explicit ack (`AckWait` 30 s, `MaxDeliver` 10). After that,
messages go to `evt.<scope>.dlq.<consumer>` and an alert fires.

### 2.4 Cells and gateways ↔ services
The simulation thread does no network I/O.
- Calls go to I/O jobs.
- Replies drain at the start of the next tick through the zone's `SimInbox`, capped by count (≤ 256 replies
  or 256 KB), not by time; the apply tick of each reply is recorded for replay (04 §10.2).
- Luau coroutines and C++ ops suspend meanwhile (R01-P0-4).

| Call | Caller | Mode | What waits |
|---|---|---|---|
| Connect-token check / reconnect-ticket sealing | gateway | local netcode decrypt / batched NATS call every 60 s | that client |
| `Heartbeat` / `ReportSuspect` | cell / gateway | core NATS, 1 Hz / on event | nothing |
| `Fence.Advance`, `Join/Leave`, `AdvanceMany`, `AdvanceOwnedBy`, `MigrateRegion` | cell (handoff, boarding, recovery, planned migration) | async, ~1–2 ms (bulk ≤ 1 s) | that AG's transfer or tree switch, and new ledger requests for that tree (the fence gate, §1.13); never the tick |
| `MintRelocation` / `RelocateDone` (Phase 4) | draining gateway / new gateway | batched NATS request, ≤ 64 per call (§1.3, §6.3.1) | nothing: the session keeps playing on the old gateway |
| `Checkpoint.Load`, `character.Get`, custody query | cell | async; player "loading" | that player |
| Checkpoint batch, telemetry | cell, gateway | JetStream async publish; per-AG coalescing while degraded; telemetry drop-and-count | nothing |
| Loot, trade, craft, currency | cell | async request with idem key; effect applied on reply | the coroutine |
| World flags | cell | KV watch; `SetFlag` async with `(region, lease_gen)` | nothing / the coroutine |
| Activity state | cell | `Activity.Update` async with `(region, lease_gen)`; the service writes KV `ACTIVITY` (§1.12) | nothing |
| Fleet roster | cell | KV `GROUP` watch, applied at a tick boundary (§1.12.1) | nothing |
| World-script RPC (§1.23) | cell | async request to the partition owner with `(region, lease_gen)`; reply after commit | the coroutine |
| Market, mail, chat, social UI | gateway service lane | bypasses the cell; identity injected by gateway | nothing |
| Handoff, ghosts, replication | cells, gateways | HTP trunk, **never the bus** (ADR-008) | see 04 |

---

## 3. Data model

### 3.1 Stores

| Store | Holds |
|---|---|
| Global PostgreSQL 18 (EU home region, §6.7) | identity (the only direct PII, encrypted), social, chat global channels, content, config, audit, entitlements |
| Per-shard primary | character (with account-progression base rows and deltas, §1.5), ledger, fence, orchestrator, market, industry and durable timers, mail, world state, parties and fleets, matchmaking, ratings and listings, world-script tables (§1.23). Each writer has a WAL budget (§3.6) |
| Separate persistence cluster | `ag_checkpoint`, region checkpoints and `ag_checkpoint_hist`, so checkpoints never compete with the ledger; from Phase 3 also the non-value write-behind tables `leaderboard_entry` and activity snapshots (§1.12) |
| ClickHouse | analytics (pseudonymous `analytics_id` only) and the ledger archive query path |
| S3-compatible object storage (MinIO in compose) | backups, WAL archive, ledger Parquet archive, CDN origin, crash dumps, audit anchors, `shred_log`, DSAR exports |

### 3.2 Core schema sketch

- **IDs:** `BIGINT` time-prefixed block IDs, laid out as `0 | 41-bit ms since 2026-01-01 | 5-bit shard |
  17-bit offset` (§1.4.5). The top bit stays 0, which is 02 §4.1's runtime-spawn range.
- **Money:** `BIGINT` minor units.
- **Append-only tables:** INSERT/SELECT only for the app role.
- **PII:** columns annotated `@pii` in `.hschema` may be `direct` only in `svc_identity`, where they are
  stored as `*_ct` ciphertext or `*_bidx` blind indexes. A CI lint enforces this (§6.6).

```sql
CREATE TABLE ledger_tx (tx_id BIGINT PRIMARY KEY, idem_hash BYTEA, reason_code BIGINT NOT NULL,
  actor_id BIGINT, ag_id BIGINT, epoch BIGINT, cell_id BIGINT, content_build BIGINT, reverses_tx BIGINT,
  created_at timestamptz NOT NULL) PARTITION BY RANGE (tx_id);       -- monthly (ID prefix = time)
CREATE TABLE idempotency (idem_hash BYTEA NOT NULL, day DATE NOT NULL, req_hash BYTEA NOT NULL,
  tx_id BIGINT, result BYTEA, PRIMARY KEY (idem_hash, day))
  PARTITION BY RANGE (day);                                          -- daily; today + 3 days live (§3.3)
CREATE TABLE wallet (wallet_id BIGINT PRIMARY KEY, owner_kind SMALLINT, owner_id BIGINT, division SMALLINT,
  currency BIGINT, kind SMALLINT, balance BIGINT NOT NULL, CHECK (kind = 3 /*system*/ OR balance >= 0));
CREATE TABLE currency_entry (tx_id BIGINT, seq SMALLINT, wallet_id BIGINT, currency BIGINT,
  amount BIGINT NOT NULL, PRIMARY KEY (tx_id, seq)) PARTITION BY RANGE (tx_id);
CREATE TABLE item (item_id BIGINT PRIMARY KEY, type_id BIGINT NOT NULL, qty BIGINT CHECK (qty > 0),
  owner_kind SMALLINT, owner_id BIGINT, loc_kind SMALLINT, loc_id BIGINT, flag SMALLINT,
  custody_ag BIGINT, attrs JSONB, version BIGINT NOT NULL);
CREATE TABLE item_event (tx_id BIGINT, seq SMALLINT, item_id BIGINT, op SMALLINT, qty_delta BIGINT,
  from_loc BIGINT, to_loc BIGINT, version BIGINT, PRIMARY KEY (tx_id, seq)) PARTITION BY RANGE (tx_id);
CREATE TABLE svc_fence.authority_fence (ag_id BIGINT PRIMARY KEY, root_ag BIGINT NOT NULL, kind SMALLINT,
  owner_cell BIGINT, owner_region BIGINT, owner_lease_gen BIGINT, epoch BIGINT NOT NULL,
  state SMALLINT NOT NULL /*1 active, 2 dormant*/, parked_seq BIGINT,
  CHECK ((state = 1) = (owner_cell IS NOT NULL)));
  -- indexes: (root_ag), (owner_region) WHERE state = 1; tree rows share epoch, owner and state (04 §6.1)
CREATE TABLE svc_orch.orch_leader (shard SMALLINT PRIMARY KEY, term BIGINT NOT NULL, holder TEXT,
  expires_at timestamptz NOT NULL);
CREATE TABLE svc_orch.region_lease (region_id BIGINT PRIMARY KEY, instance_id BIGINT, holder_proc BIGINT,
  lease_gen BIGINT NOT NULL, assigned_at timestamptz);
CREATE TABLE svc_orch.process (proc_id BIGINT PRIMARY KEY, kind SMALLINT, az TEXT, rack TEXT, host TEXT,
  server_build BIGINT, state SMALLINT, confirmed_dead_at timestamptz, confirmed_by SMALLINT); -- §1.4.3
CREATE TABLE svc_orch.id_alloc (shard SMALLINT PRIMARY KEY, last_ms BIGINT NOT NULL);
CREATE TABLE market_partition (partition_id SMALLINT PRIMARY KEY, owner_replica TEXT,
  owner_gen BIGINT NOT NULL);
CREATE TABLE market_order (order_id BIGINT PRIMARY KEY, client_order_id TEXT UNIQUE, region_id BIGINT,
  type_id BIGINT, side SMALLINT, price BIGINT, qty_total BIGINT, qty_left BIGINT CHECK (qty_left >= 0),
  range SMALLINT, location_id BIGINT, owner_char BIGINT, escrow_tx BIGINT, state SMALLINT,
  expires_at timestamptz);
CREATE TABLE ag_checkpoint (ag_id BIGINT PRIMARY KEY, zone_id BIGINT, epoch BIGINT NOT NULL,
  seq BIGINT NOT NULL, schema_ver INT, blob BYTEA NOT NULL, item_refs BIGINT[])
  PARTITION BY HASH (ag_id);                                         -- 32 partitions (§3.3 storage)
-- Identity (global): the only direct PII, encrypted under the account's DEK (§6.6)
CREATE TABLE svc_identity.account (account_id BIGINT PRIMARY KEY, state SMALLINT, email_bidx BYTEA UNIQUE,
  email_ct BYTEA, dob_ct BYTEA, age_band SMALLINT, band_until DATE, country CHAR(2),
  created_at timestamptz);
CREATE TABLE svc_identity.subject_key (account_id BIGINT PRIMARY KEY, wrapped_dek BYTEA,
  kek_version INT, analytics_id BIGINT UNIQUE, shredded_at timestamptz);
CREATE TABLE svc_identity.legal_acceptance (account_id BIGINT, doc_kind SMALLINT, version INT,
  doc_sha256 BYTEA, accepted_at timestamptz NOT NULL, PRIMARY KEY (account_id, doc_kind, version));
```

**Indexes:**
- `currency_entry (wallet_id, tx_id DESC)`
- `item (owner_kind, owner_id, loc_id)`, `item (loc_kind, loc_id)`
- `item_event (item_id, tx_id)`
- partial: custody items, unsent `outbox`, open orders

The other tables (characters, orgs, mail, chat, outbox/inbox, content, `audit_log`/`audit_note`,
`legal_doc`, world state, `zone_leader`, `zone_handle_block`, the `grp*` group tables of §1.12.1 and the
`ws_*` world-script tables of §1.23) follow the same conventions.

### 3.3 Partitioning, retention, migrations, backup

**Partitioning and retention**
- **Ledger tables and `market_trade`:** monthly range partitions, created 3 months ahead.
  - Partitions stay **online for 90 days**. They are then detached to zstd Parquet in object storage.
  - `Journal`/`ItemHistory` queries older than 90 days go to ClickHouse over that archive. §3.6 shows why
    13 months online would need ~50 TB on a Phase 4 primary.
- **`idempotency`:** daily partitions by first-seen day. Today's partition and the 3 before it are live (72–96 h of
  retention), and older ones are dropped, never deleted row by row.
  - `Execute` takes `pg_advisory_xact_lock(idem_hash)` and then probes every live partition. The per-day
    primary key therefore cannot let a retry that crosses midnight apply twice.
  - Keys whose retries can outlive the window (saga steps, timers, reward tokens, job delivery) must name a
    **guard entity** whose state changes in the same transaction: `job.state`, `reward_token.redeemed_tx`,
    the escrow's state, a quest stage version. A late replay then fails with `PRECONDITION_FAILED`.
    schemac lints every deterministic key scheme for its guard.
- **`outbox`:** daily partitions. A partition is dropped 3 days after its last row was sent, and never while
  it holds unsent rows. Unsent age > 5 min alerts.
- **`inbox`:** daily partitions with 7-day retention. `helios-admin dlq replay` refuses messages older than
  that.
- **`ag_checkpoint`:** 32 hash partitions with `fillfactor = 60` and `toast_tuple_target = 8160`. `blob` is
  stored `EXTERNAL`, because it is already zstd. Records ≤ 8 KiB therefore update in place without TOAST
  churn.
- **`ag_checkpoint_hist`:** daily partitions kept 14 days. It keeps **sampled** versions, not every write:
  - the first checkpoint of each AG in each clock hour;
  - every logout or zone-exit checkpoint;
  - a `gm_snapshot` taken before any GM action or rollback that targets the AG.

  That is ~6 M rows/day at 50k CCU, rather than the 2.6 B that keeping every write would cost at 30k AG/s.
- **Chat, mail and login history:** daily partitions, dropped at their retention (§6.6).
- Every index must back a named sqlc query.
- Scylla and FoundationDB are benchmarked behind the `CheckpointStore` seam before Phase 4 (R07-P2-26),
  using §3.6's rates.

**Migrations**
- Services use `pgx/v5` natively, with `otelpgx` tracing.
- goose v3 SQL migrations, one directory per schema, are embedded via `embed.FS`. Stubs are generated
  from `.hschema`.
- Changes follow expand/contract: release N only adds, and contraction happens in N+2.
- Migrations run pre-deploy under an advisory lock.

**Backup (CloudNativePG)**
- A synchronous in-region quorum standby (`ANY 1` of 2, across zones) gives RPO 0 for a zone loss.
- WAL archiving (`archive_timeout=60s`) gives PITR within 60 s.
- Base backups are nightly and kept 35 days. Base backups and WAL are copied cross-region (§6.7).
- The persistence cluster keeps 3 days of WAL, because it holds non-value data.
- A PITR drill and conservation audit run monthly, with RTO ≤ 1 h. Every restore replays `shred_log`
  before it takes traffic (§6.6).

### 3.4 Valkey is never the truth
Valkey (`go-redis/v9`) holds sessions, the queue, presence, GCRA rate-limit buckets (one Lua script),
matchmaking pools, leaderboards and API caches. Sentinel failover can lose acknowledged writes, so everything
is rebuildable: sessions from refresh tokens and reconnect tickets, the queue from signed tickets, presence
from heartbeats, leaderboards from PG.

No ledger, order or progression state lives only in Valkey (R07 §5, R02-P0-7).

### 3.5 Dev database: embedded-postgres (ADR-014)
`helios-backend` runs a real **PostgreSQL 18.3** child process via `fergusstrange/embedded-postgres`.
- **Binaries:** cached once in `%LOCALAPPDATA%\helios\pg-bin`. Offline machines install from a zip with
  `pg-install --from`.
- **Data:** `helios-data\pg`, listening on loopback only.
- **Schemas and roles:** one per service, as in prod, so JSONB, partitions, `SKIP LOCKED`, advisory locks and
  `LISTEN/NOTIFY` behave identically.
- **Stale processes:** a stale `postmaster.pid` is detected and stopped at startup.
- **No SQLite fallback for services.** SQLite is reserved for tools and caches. `--db postgres://…` targets
  any local server.

### 3.6 Storage growth and capacity model
Sizing is per shard. The "design" columns are the acceptance rates (A5, A9), which tests sustain for 1–72 h.
They size IOPS and WAL, not retention. The "expected" columns size retention.

**Assumptions** (rerun at every phase gate with measured rates, 09 K30):
- 0.05 ledger tx/s per CCU: loot, trades, crafting, market, and 10 s consume batches for combatants.
- 0.12 checkpointed AG/s per CCU: 3 dirty AGs per player every 30 s, plus 20 % NPC-owned persistent AGs.
- On-disk sizes, including tuple headers and indexes: `ledger_tx` 150 B, `currency_entry` 120 B (1.5 per
  tx), `item_event` 135 B (2 per tx), which is ≈ 600 B per tx. `idempotency` 140 B. A checkpoint write is
  2.25 KB (2 KiB mean blob). WAL is ≈ 1.5 KB per ledger tx and ≈ 1.1× checkpoint payload. The 1.5 KB includes
  the heap-lock record that the ledger's `FOR SHARE` on its custody fence row writes.
- **Fence operations (§1.13) per player:**
  - one load `Advance` and one `Park` per session, with a 2 h mean session; 10 % of parks follow a `Leave`;
  - one zone transition (an `Advance`, 04 §7) every 3 min;
  - from Phase 3, one cell-boundary handoff every 2 min for the ≈ 40 % of players in multi-cell zones, plus
    20 % more for NPC-owned persistent AGs;
  - one `Join` and one `Leave` every 10 min (boarding, docking, hangars).
- **Rows per fence statement:** 1.5 for loads and parks, 2 for transitions and handoffs (ship and pilot) and
  3 for `Join`/`Leave`, which lock both trees.
  - Bulk: a rolling restart re-advances every active row (≈ 3.6 per player) once in ≈ 10 min, and a crash
    recovery advances ≤ 5k rows in ≤ 1 s (NS-2.7).
  - The design columns use the admission rate (A8) for loads and parks, and ≈ 3–9× expected for the rest, which
    covers a 2,000-ship fleet jumping or crossing a boundary together. They add 2k rows/s of bulk migration.
- **Fence WAL:** ≈ 0.4 KB per operation (its outbox event and commit record) plus ≈ 0.8 KB per row. A row
  update is not HOT, because the owner columns are indexed, so it touches the heap and all three indexes. The
  0.8 KB also amortizes full-page images at 15 min checkpoints with `wal_compression = zstd`.
- **Shard-primary write IOPS:** page writeback of ≈ WAL bytes ÷ 8 KB, plus one WAL flush per group commit
  (≈ 1 per 5–10 commits at design load).

| Quantity | Ph2 expected | Ph2 design | Ph4 expected | Ph4 design |
|---|---|---|---|---|
| Shard CCU (AAA-SRV-3) | 2k | 2k | 50k | 50k |
| Ledger tx/s | 100 | 2,000 (A5) | 2,500 | 10,000 (A5) |
| Rows inserted/s (≈ 6.5 per tx incl. idempotency and outbox) | 650 | 13k | 16k | 65k |
| Ledger table growth | 0.06 MB/s = 5.2 GB/day | 1.2 MB/s = 104 GB/day | 1.5 MB/s = 130 GB/day | 6 MB/s = 0.52 TB/day |
| Fence operations/s (loads + parks + transitions + handoffs + `Join`/`Leave`) | ≈ 20 (0 + 0 + 11 + 0 + 7) | 250 (50 + 50 + 100 + 0 + 50) | ≈ 660 (7 + 8 + 280 + 200 + 167) | ≈ 2,900 (200 + 200 + 1,000 + 1,000 + 500) |
| Fence rows updated/s, bulk included | ≈ 45 | 500, with 5k-row recovery bursts | ≈ 1.5k | ≈ 8.1k (6.1k + 2k bulk) |
| Shard-primary WAL: ledger | 0.15 MB/s | 3 MB/s | 3.8 MB/s | 15 MB/s |
| Shard-primary WAL: fence | 0.04 MB/s | 0.5 MB/s | 1.5 MB/s | 7.6 MB/s |
| Shard-primary WAL: total, every writer (Phase 4 split in the write-mix table below; Phase 2 design = A5's mix, since A7, A10 and 06's progression test run separately at ≤ 2 MB/s each) | 0.26 MB/s | 3.5 MB/s = 0.3 TB/day | 8.1 MB/s = 0.70 TB/day | 34.5 MB/s = 3.0 TB/day |
| Shard-primary write IOPS (8 KB pages + WAL flushes) | ≈ 190 | ≈ 0.9k | ≈ 2.1k | ≈ 7.5k |
| `idempotency` live size (4 partitions) | 4.8 GB | 97 GB | 121 GB | 484 GB |
| Ledger online (90 days) | 0.47 TB | — | 11.7 TB | — |
| Parquet archive growth (≈ 5× zstd) | 1 GB/day | — | 26 GB/day (9.5 TB/year) | — |
| Checkpointed AG/s | 250 | 3,000 | 6,000 | 30,000 (A9) |
| Checkpoint payload | 0.56 MB/s | 6.8 MB/s | 13.5 MB/s | 68 MB/s |
| Persistence-cluster WAL (from Phase 3 including leaderboards and activity snapshots: +1.7 / +6.2 MB/s at Phase 4) | 0.6 MB/s = 53 GB/day | 7.4 MB/s = 0.64 TB/day | 17 MB/s = 1.5 TB/day | 80 MB/s = 6.9 TB/day |
| `ag_checkpoint` live size | 2 M AGs ≈ 4.5 GB | — | 10 M AGs ≈ 23 GB (38 GB at fillfactor 60) | — |
| `ag_checkpoint_hist` (14 d, sampled) | 8 GB | — | 195 GB | ≤ 760 GB (1 M AGs dirty/h) |
| `PERSIST` stream per replica (1 h) | 2 GB | 24 GB | 49 GB | 243 GB |
| `EVT` stream (7 d, ≈ 600 B per tx) | 36 GB | — | 0.9 TB | — |

**Shard-primary write mix (Phase 4, per writer).** Every service that writes the shard primary has a row. The
design rates are the writers' acceptance rates, and A5 (Ph4) runs all of them at once. WAL per unit includes
tuple, index, outbox and commit records, with full-page images amortized as above. IOPS is page writeback
(WAL ÷ 8 KB). WAL flushes, ≈ 21k commits/s at design grouped ≈ 7 per flush, add ≈ 3k IOPS to the total row.

| Writer | Rate: expected / design | Rows/s: exp / design | WAL MB/s: exp / design | Writeback IOPS (design) |
|---|---|---|---|---|
| Ledger (§1.6) | 2,500 / 10,000 tx/s (A5) | 16k / 65k | 3.8 / 15 | 1.9k |
| Fence (§1.13) | 660 / 2,900 operations/s (A5) | 1.5k / 8.1k | 1.5 / 7.6 | 950 |
| World scripts (§1.23) | 1,000 / 5,000 row writes/s (the shard cap) | 1k / 5k | 1.0 / 5.0 | 625 |
| Market orders and trades (§1.7); escrow legs are ledger rows | 250 / 2,000 order operations/s (A7's hub rate as the shard ceiling), ≈ 1 KB each | 625 / 5k | 0.25 / 2.0 | 250 |
| Durable timers (§1.8): arm, claim, fire | 300 / 1,500 timers/s over 1 M pending (A10), ≈ 0.7 KB each | 900 / 4.5k | 0.21 / 1.05 | 130 |
| Account progression (§1.5), as deltas | 1,000 / 2,000 flushes/s (06 §5.2's 30 s flush; §1.5's rate) at ≈ 0.4 KB, plus 55 / 65 folds/s at ≈ 5 KB | 2.8k / 4.1k | 0.7 / 1.1 | 140 |
| Character: rows, `ApplyProgression`, follower progression; settings blobs only when changed | 200 / 1,000 writes/s at ≈ 1 KB, plus ≤ 20 blobs/s at ≈ 16 KiB | 200 / 1k | 0.3 / 1.3 | 160 |
| Mail (§1.9) | 50 / 200 mails/s, ≈ 2.5 KB each | 150 / 600 | 0.13 / 0.5 | 60 |
| World state (§1.21): flags, territory, influence batches | 100 / 500 flag writes/s, ≈ 0.6 KB each | 200 / 1k | 0.06 / 0.3 | 40 |
| Groups (§1.12, §1.12.1): rosters, listings, matchmaker rows, ratings | ≤ 200 roster mutations/s, ≈ 30 listing refreshes/s, 17 matches/s × 13 rating rows | 300 / 1.2k | 0.08 / 0.3 | 40 |
| Industry jobs, resources, structure upkeep (§1.8) | 50 / 300 job rows/s, ≈ 0.8 KB | 50 / 300 | 0.04 / 0.25 | 30 |
| Orchestrator, ID blocks, migrations (§1.4) | — | ≤ 100 | 0.05 / 0.1 | 10 |
| **Total on the shard primary** | | **≈ 24k / 96k** | **8.1 / 34.5** | **4.3k (+ ≈ 3k flushes)** |
| *Moved to the persistence cluster:* leaderboards (§1.12) | 2,000 / 10,000 upserts/s, ≈ 0.45 KB (non-HOT: the rank index changes) | 2k / 10k | 0.9 / 4.5 | — |
| *Moved to the persistence cluster:* activity snapshots (§1.12) | 250 / 500 snapshots/s, ≤ 4 KiB, ≈ 3.3 KB WAL | 250 / 500 | 0.8 / 1.65 | — |

**Standby replay budget.** The synchronous standby replays WAL in one process, which is the binding limit
(below). Its capacity for this mix is measured by `helios-loadgen replay`: it replays 30 min of recorded
design-mix WAL flat out on the reference standby, with `recovery_prefetch = on` and the working set resident.
- **Rule:** the design mix must stay ≤ 2/3 of the measured rate.
- **Planning figure:** 60 MB/s (≈ 150k records/s at ≈ 400 B), which gives a budget of 40 MB/s.
- **Why the two moves:** the mix above is 34.5 MB/s, 58 % of the planning figure. Without them it would
  exceed the budget: leaderboards and activity on the primary make 40.7 MB/s, and whole-row progression
  flushes (≈ 3 KB each, 6 MB/s at design) make 45.6 MB/s.
- **If the measured capacity is below 52 MB/s** (34.5 ÷ 2/3), the next moves, in order, are:
  1. world-script tables to their own cluster (−5 MB/s; they commit through one service and reach the
     ledger only through the outbox, §1.23);
  2. the Phase 5 ledger split.

**Consequences:**
- **Phase 4 shard primary:** ≈ 15 TB NVMe (11.7 TB of online ledger plus idempotency, other schemas and 25 %
  headroom). Phase 5's owner-hash ledger partitions split this.
  - At design load it writes ≈ 35 MB/s of WAL and ≈ 7.5k IOPS, far inside one NVMe's write rate. The binding
    limits are commit latency to the synchronous standby and that standby's single-process replay. A5 (Ph4)
    therefore runs the whole write mix above and asserts replay lag and the replay budget.
  - Fence traffic is ≈ 22 % of that WAL (7.6 of 34.5 MB/s). If A5 finds the fence budget exceeded, the first
    fix is less WAL per row, by making more updates HOT, and the Phase 5 ledger split comes after that.
- **Phase 4 persistence cluster:** it must sustain ≥ 90 MB/s of WAL: 74 MB/s of checkpoints, 6.2 MB/s of
  leaderboards and activity snapshots, and headroom. That is ≈ 40k row writes/s. A9 measures exactly this.
  The checkpoint numbers drive the Scylla/FDB comparison.
- **WAL archive:** 35 days of shard WAL at Phase 4 expected load (8.1 MB/s) is ≈ 24.5 TB raw (≈ 8.7 TB
  compressed).
- **NATS:** at design load, `PERSIST` means ≈ 200 MB/s of disk writes across the R3 cluster. This is why it
  has 8 partitions, whose leaders spread over the nodes.
- **Bounded growth:** idempotency, inbox, outbox, hist, chat and login history all expire by partition drop.
  Only the ledger grows without bound, and it moves to object storage after 90 days. A6 asserts this over
  72 h.
- **World scripts and groups (§1.23, §1.12.1):** world scripts share a shard write budget of ≤ 5k row
  writes/s (≈ 5 MB/s of WAL, their row in the write mix above) and, by default quota, ≤ 64 ×
  10 GiB of rows per project, with `@ttl` expiry. Fleet rosters add ≤ 100k rows and ≤ 200 writes/s. Both are
  rerun here at each gate with the templates' measured rates.
- **Every writer is rerun at each gate** (09 K30). A new shard-primary writer, or a rate change > 20 %, adds or
  updates its row in the write mix and `helios-loadgen mix` before its WP closes.

---

## 4. Consistency & integrity

### 4.1 Dupe prevention (R07 §4's four sources)
1. **Two owners** → single custody plus the fence (04 §6.1).
   - Inside its transaction, `Execute` reads the AG's fence row `FOR SHARE`. This is the one sanctioned
     cross-schema grant.
   - It requires the request's `(epoch, cell)` and `item.custody_ag` to match. Rows of an AG tree share the
     root's epoch and owner, so a handed-off ship's old owner also fails for its passengers' items (04 §6.1).
   - A **dormant** row (a parked AG, 04 §6.1) means its items are at rest. A service op on them (an
     out-of-game mail claim, a GM restore, erasure step 4) carries the precondition `{ag, epoch, dormant}`
     instead of a cell. A load that reactivates the row either waits for the transaction or makes it fail
     with `FENCE_STALE`, after which the service re-issues through the owning cell.
   - Transient AGs (NPCs, loot drops) never hold custody; loot is minted on pickup into the looter's AG.
   - A concurrent `Fence.Advance` waits for the ledger transaction. Once an advance commits, no write at
     the old epoch can commit. The wait is short and bounded. The owner cell's fence gate sends no new
     request for a tree while a fence operation on it is in flight. The custody check is the transaction's
     last statement. All fence rows are locked in `ag_id` order (§1.13).
   - Recovery bulk-advances a dead region's AGs with `Fence.AdvanceOwnedBy` (04 §6.4). A zombie that kept
     simulating is harmless: its ledger and checkpoint writes fail the fence, gateways and neighbour cells
     drop its trunk messages by generation, and it stops at the first rejection (§1.4.2). No cell self-fences
     on lost renewals.
   - The market uses the same pattern with `market_partition.owner_gen` (§1.7).
2. **Crash between take and give** → one PG transaction (`AtomicSwap`, crafting finalize), or escrow plus a
   saga.
3. **Partial rollback** → value never goes through write-behind (§1.13).
4. **Replays** → idempotency keys stored with `req_hash`.
   - Same hash returns the stored result. A different hash returns `IDEMPOTENCY_MISMATCH` and raises an
     alert.
   - Keys are deterministic where possible: `loot:<killmail>:<n>`, `job:<id>:deliver`,
     `quest:<instance>:<stage>`, `reward:<token>`.
   - Keys retire after 72–96 h. Anything that can retry later names a guard entity (§3.3).

Every item mutation also checks `item.version`.

### 4.2 Exactly-once effects on JetStream
**Producer side (transactional outbox):**
1. The event row is written in the state change's own transaction.
2. A relay woken by `LISTEN/NOTIFY` publishes it with `Nats-Msg-Id = outbox.id`, and the stream dedups.
3. The row is marked sent after PubAck.

**Consumer side:**
1. `inbox(consumer, msg_id)` is inserted in the same transaction as the effect.
2. The consumer then calls `AckSync`.
3. A redelivery hits the primary key and is only acked.

Checkpoints need no inbox, because the `(epoch, seq)` merge is already idempotent.

### 4.3 Sagas
- `pkg/saga` persists `(id, type, step, state, payload, attempts, next_retry_at)` in the owner's schema.
- Every step has an idempotent action, a compensation and a guard entity (§3.3).
- Retries run on durable timers with capped backoff (24 h at most), then the saga is parked for GM review.
- Temporal was rejected as an extra stateful server with no Windows single-binary mode.

| Saga | Steps | Compensation |
|---|---|---|
| Market order | `EscrowOpen` → owner-fenced insert (§1.7) | `EscrowRefund` |
| Mail + attachments | escrow → mail → notify | return to sender |
| Crafting slot / industry job | escrow or consume → job → timer → mint | refund per rules (§1.8) |
| Data-subject export / erasure (§6.6) | participants in order → package / shred | resume; never un-erase |
| Phase 5 cross-partition / shard transfer | escrow at A → credit at B | re-credit A |

A player trade between two cells is **not** a saga. It is one `AtomicSwap` after both players confirm.

### 4.4 Conservation audits
**Pre-commit check (Go):** postings sum to 0 per currency.

**Hourly, on a replica:**
- Σ of all balances, including system accounts, is 0 per currency.
- Each wallet touched equals the Σ of its entries.
- For each item type, minted − destroyed = live.
- Version chains have no gaps.

**Nightly:** Σ fills ≤ original quantity for every market order.

**Weekly:** a full audit of 1 B entries in under 30 min, including archived partitions through ClickHouse.

**On violation:** page on-call (SEV1, §6.8) and optionally trip a kill switch (R07-P0-7).

### 4.5 Audit trails and rollback tooling
Every transaction records its actor, cell, epoch, content build (the server part in effect, §1.14.1) and
reason.

**Reversal:** `helios-admin ledger reverse --tx` writes a compensating transaction. Nothing is ever deleted.
Erasure never needs to delete either (§6.6).

**Character rollback:** `rollback-character --to T`
1. plans the character's transactions after T;
2. follows **taint** downstream to other players (dupe/RMT forensics);
3. reports conflicts;
4. needs `economy` approval;
5. executes as one `gm_rollback` batch.

Non-value state is restored from `ag_checkpoint_hist`. It is sampled hourly, and a `gm_snapshot` is taken
before every GM action (§3.3).

---

## 5. Local dev experience on Windows
`helios-backend.exe run` starts, with no Docker:
- every service;
- embedded NATS 2.15 with JetStream (in-process for Go, loopback for nats.c);
- embedded PostgreSQL;
- miniredis;
- the admin console;
- a static CDN.

`helios-dev up` (04's `tools/devcluster`) wraps it.

```
helios-data\  helios.toml  pg\  nats\  cdn\{chunks,packs,manifests,channels}\  content\  logs\
              keys\   (generated dev JWT, shard netcode, trunk, ticket, manifest Ed25519 and dev KEK keys)
```

| Flag | Default | Notes |
|---|---|---|
| `--data-dir` | `.\helios-data` | all state; `reset` wipes it |
| `--services` | `all` | same binary runs one service in prod |
| `--db` / `--bus` / `--cache` | `embedded` / `embedded` / `miniredis` | or `postgres://`, `nats://`, `valkey://` |
| `--http` | `127.0.0.1:7700` | Connect APIs, launcher, `/admin`, `/cdn`, public API |
| `--ops` | `127.0.0.1:7701` | `/metrics`, `/healthz`, pprof |
| `--nats` | `127.0.0.1:4222` | cells and gateways connect here |
| `--spawn` | `gateway,cell,ws` | supervisor runs `helios-gateway.exe` (UDP 7777), `helios-cell.exe` and, when the project declares a `worldscript` block, one `helios-cell.exe --role world-script` (§1.23). The editor's PIE passes `--spawn none` and launches its own cells and WSH (07 §1.6.2) |
| `--seed` | `dev` | `dev1…dev10` (password `dev`), 1 M credits, items, a corp, NPC orders; `loadtest` = 50k accounts |
| `--lan` | off | loopback avoids the Windows Firewall prompt |
| `--retention-scale` | `1` | shrinks every retention horizon, for soak tests (A6) |
| `--dev-clock` | off | dev flavour only, refused when `env` is `staging` or `live`: a monotonic offset and rate for game-calendar time, published as the dev-only KV `CONFIG` key `dev.clock` and applied by the timer worker (due timers fire in due order), the calendar, `--dev` cells and WSHs and offline-progress services; leases, tokens, TTLs and idempotency windows stay on real time (07 §1.6.4) |

**Subcommands:** `migrate`, `seed`, `reset`, `pg-install`, `doctor`, `privacy {export,erase} --account`,
`chaos {nats-leader,orch-leader,pg-switchover,kill-domain,nats-block}`.
- `kill-domain --fd host=h2` hard-kills every process registered in that simulated failure domain. Spawned
  dev processes take `--fd`, so the §1.4.3 classifier runs on one Windows machine.
- `nats-block --proc <id>` cuts one process's NATS link and leaves its trunks up (the N state).

**Compose.** `deploy/compose/docker-compose.yml` runs on Docker Desktop/WSL2 and Linux. It includes:
- postgres:18 and valkey:8;
- NATS 2.15, 3 nodes under the `ha` profile;
- MinIO;
- otel-collector, Prometheus, Alertmanager, Grafana, Tempo and Loki;
- ClickHouse (`analytics` profile) and self-hosted Sentry (`crash` profile);
- one container per service, all from a single image.

**Admin console (`/admin`, Go templates + htmx, no Node):** health and control-plane mode, the shard map
(generations, fences, CCU), accounts, the ledger explorer, market books, moderation, kill switches, content
channels, world flags, audit, economy, the DSAR queue, the **case queue** (§1.17: filters by priority,
category, state, shard, assignee and SLA due time; each row shows the first-response and resolve due times,
time left and a breach badge; an evidence viewer and linked sanctions) and the incident/status editor.

---

## 6. Ops

### 6.1 Deployment
**Phases 0–2:** the combined binary runs on VMs. `helios-agent` supervises the cells and gateways, and there
is a weekly maintenance window.

**Phase 3+ (Kubernetes):**
- Go services run as Deployments with HPA and PDBs.
- Data: CloudNativePG (PG 18), NATS via Helm (3 nodes, R3, one per zone), Valkey with Sentinel.
- Cells run as high-density **Agones Fleets** per zone class (Counters/Lists), with `scheduling:
  Distributed` and topology spread over host, AZ and (Phase 4) rack. The Fleet's Ready buffer is the warm
  pool, sized to the largest blast-radius domain and spread across domains (§1.4.6), not a flat 10 %
  (R07 §6).
- **Node pools.** `server` holds cells, replicants and standbys. `control` holds NATS, PG, the orchestrator,
  Valkey and the Go services. The pools are host-disjoint in Phase 3 and rack-disjoint from Phase 4
  (§1.4.3). Nodes carry `topology.kubernetes.io/zone` and `helios.dev/rack` labels, and a CI check refuses a
  SERVER node without them.
- Gateways run on bare metal behind anycast scrubbing (04 §1), spread evenly over the shard's 3 availability
  zones and sized so that two zones hold every session (§6.7): 6 boxes at Phase 3, 12 at Phase 4.
- A colo baseline is decided at Phase 3 (§6.4). Regional placement is in §6.7.
- SERVER nodes are patched only through the node-maintenance controller (§6.1a).

### 6.1a Node maintenance: cordon, Drain, release — Phase 3 (host batches), Phase 4 (rack batches, A14 (d))
**Why.** SERVER hosts need kernel, firmware and Kubernetes upgrades about monthly: ≈ 5 hosts at Phase 3 and
≈ 30 at Phase 4. `Drain(process, maintenance)` (§1.4) already moves regions without disconnects, but nothing
connected a node cordon to it. An allocated Agones GameServer blocks `kubectl drain`, and a forced eviction is
a crash: A3b's ≤ 10 s hitch and, outside the replicant tier, up to 30 s of non-value state lost. Without a
controller, patching either stalls or takes the crash path.

**Controller.** `helios-nodemaint` runs inside the orchestrator deployment and is leader-elected with it
(§1.4.1). It is the only path that empties a SERVER node.
1. **Request.** The patch pipeline (kured or the provider's node upgrade), or an operator with
   `helios-admin node maintain <node | rack>`, labels nodes `helios.dev/maintenance=requested`. A plain
   `kubectl cordon` of a SERVER node counts as a request.
2. **Admission.** A batch is admitted only when all of these hold:
   - all of its nodes lie in one blast-radius domain: one host in Phase 3, the hosts of one rack in Phase 4
     (§1.4.3);
   - no other batch is in flight;
   - the control plane is in normal mode (§1.4.4);
   - neither `WarmPoolBelowDomain` nor `FailureDomainLost` is open;
   - no `PreProvision` window or epoch flip falls within the next hour (§1.14.1);
   - its surge capacity is Ready (step 3).
3. **Surge first.** The Fleet buffer grows by the batch's slots (its cells, standbys, replicants and WSHs,
   §1.4.6) on nodes outside the batch's domain, and the drain starts only once they are Ready. The warm pool
   therefore never falls below the domain bound, and a real host or rack loss during maintenance is still
   absorbed.
   - In cloud, surge capacity comes from the cluster autoscaler.
   - In colo, each shard keeps one spare SERVER host (≈ 3 % of Phase 4 SERVER capacity). Each patched host
     rejoins as the next batch's surge, so the spare is used once per roll.
4. **Cordon and Drain.** The controller cordons the batch and calls `Drain(process, maintenance, 30 min)` for
   every Helios process on it:
   - **cells** move by planned region migration (04 §6.7), ≤ 4 regions at a time per process and ≤ 16 per
     shard, so each region hitches ≤ 1 s, once;
   - **replicants** move make-before-break: `AssignReplicant` opens the replacement, the cells stream it full
     state, and the old replicant is released only once the new one has committed a tick for every region,
     so no cell is ever left without one;
   - **world-script partitions** hand over their lease at an invocation boundary, a ≤ 1 s pause (§1.23
     item 9);
   - **Ready standbys** are deleted once the surge has replaced them.

   An emptied process exits through the Agones SDK `Shutdown`.
5. **Eviction is blocked.** GameServers run with Agones `eviction.safe: Never`. Neither `kubectl drain`, the
   cluster autoscaler nor a patch agent can evict an allocated process. The controller labels the node
   `helios.dev/maintenance=ready` only once no Helios pod remains, and the patch agent then reboots or replaces
   it.
6. **Rejoin.** Once the node is Ready again it runs a self-test: a canary GameServer boots, ticks and opens a
   trunk to one gateway per zone. The node is then uncordoned, the surge buffer returns to normal and the next
   batch may start.
7. **Brakes.**
   - A drain past its deadline raises `NodeDrainOverdue` (SEV3) and pauses the roll. It is never forced.
   - A roll also pauses when the hitch p99 of its last 50 migrations exceeds 1 s, or when one migration aborts
     twice.
   - `helios-admin node evacuate` (for failing hardware) takes the crash path on purpose, and is audited
     (§1.17).

**Times (Phase 4).** A host of 12 cells drains in ≈ 2–3 min (three rounds of ≤ 30 s preparation and a ≤ 1 s
hitch). With a ≈ 10 min reboot and ≈ 2 min rejoin, a batch takes ≈ 15 min. One-host batches (colo, one
spare) patch ≈ 30 hosts in ≈ 7.5 h. Rack batches with cloud surge take ≈ 9 × 20 min ≈ 3 h. Every region moves
once per roll.

**Metrics:** `node_maint_state{node}`, `node_maint_drain_seconds` and the per-region migration hitch.
**Dev and CI:** the dev orchestrator's `LocalProcessPlacer` accepts `helios-admin node maintain local`, which
drains every local process through the same path. Acceptance is A14's clause (d).

### 6.2 Observability and SLOs
**Pipeline.**
- Go services use OpenTelemetry. Cells export Prometheus metrics and OTLP spans (04 §10).
- Traces go to Tempo, metrics to Prometheus, and `slog` JSON logs to Loki. Fields tagged `@pii` are
  redacted at source (§6.6).
- `traceparent` rides in Connect and NATS headers, so one trace spans login → token → gateway → cell →
  ledger (R07-P0-10).

**Crashes (Phase 2).** Crashes go from sentry-native/crashpad through `crashgw` (08 §3) to self-hosted
Sentry. CI uploads symbols (R10 §9).

**Key metrics:** RED per RPC, consumer lag, outbox age, ledger tx/s by reason, fence conflicts, fence
operations and rows per second with `fence_lock_wait_ms` (§1.13), gateway relocations by outcome (§6.3.1), checkpoint
lag, cell tick p99 (R04-P1-16), control-plane mode, heartbeat misses, silence classifications by class
(domain loss, wide loss, control-plane fault, NATS-isolated), warm-pool slots surviving the worst domain,
gateway free slots surviving the worst availability zone (§6.7), world-script invocations, fuel kills and
commit lag per partition (§1.23), ID-block reserve, partition-drop success and table sizes against §3.6,
shard-primary WAL per writer and standby replay lag against the §3.6 write mix, node-maintenance state
(§6.1a), NATS permission violations and caller mismatches (§6.5), and trust detector flags and appeal
overturns (§1.16a). Alerts use multi-window burn rates (§6.8).

| SLO | Target |
|---|---|
| Login (auth + token, no queue) | p95 < 3 s; 99.9 % success / 30 d (R07 §6) |
| Connect-token issue / `Fence.Advance` (at the §3.6 design fence mix) | p99 < 100 ms / < 5 ms |
| Gateway relocation (Phase 4, §6.3.1) | p99 ≤ 2 s; input gap p99 ≤ 250 ms; fallbacks ≤ 0.1 % of relocations / 30 d |
| Ledger `Execute` | p99 < 50 ms (Phase 2–3), < 25 ms (Phase 4); 99.95 % available |
| `PlaceOrder` / trade → settled | p99 < 150 ms / < 2 s |
| Checkpoint → PG durable | p99 < 5 s; page if > 60 s |
| Chat delivery / timer lateness | p99 < 250 ms / < 1 s |
| Cell failure confirmed → standby assigned (normal mode, including a host or rack loss inside the blast radius) | p99 ≤ 4 s after the crash |
| Control-plane degraded time | ≤ 5 min / 30 d; any episode > 60 s pages |
| DSAR export / erasure | ≤ 72 h each (§6.6) |
| Global-tier regional failover | RTO ≤ 1 h, RPO ≤ 60 s (§6.7) |
| Ledger invariant violations | **0**; any violation pages |

### 6.3 Releases, live ops, hotfixes
**Go services** go through canary steps of 1 → 10 → 50 → 100 %.
- HTTP traffic shifts by route weight; NATS traffic shifts by replica share.
- Every step is SLO-gated.
- Rollback redeploys the previous image. This is safe because migrations are expand-only.

**Cells** take weekly downtime until Phase 3. After that they roll by planned region migration (04 §6.7), which
requires N↔N+1 mesh compatibility.

**Gateways** share that downtime until Phase 3. In Phase 3 they roll by paced reconnect, and from Phase 4 box
by box through `DrainGateway` and make-before-break relocation (§6.3.1).

**Content releases** follow §1.14.1: server hotfixes, compatible client builds, server binaries, and
epoch flips.

**A server hotfix** is a build that changes only the server part (AAA-ITR-8, ≤ 15 min):
1. CI validates the build and proves the fingerprint is unchanged.
2. Canary it on one PTR zone.
3. `Promote` it.
4. `SwapServerPart` stages it over live instances: one canary zone, then 10 %, then 100 %. Each stage is gated
   on error, tick-budget and economy-guard SLOs. Cells apply it at a tick boundary.

To roll back, swap the previous server part back. Client-visible changes wait for a compatible client build
or an epoch flip. Kill switches give instant mitigation (R07-P1-18, R03-P1-11).

#### 6.3.1 Gateway deploys and maintenance: make-before-break relocation (Phase 4)
**Why.** netcode has no session migration (04 §2.4). Stopping a gateway process is therefore NS-4.7's kill:
each of up to 8k sessions times out and goes linkdead, vulnerable, and gets back only after ≈ 6–10 s. Boxes
still need restarts, about monthly per box, for:
- gateway binaries, including netcode or security patches and the Phase 4 X25519 re-key (04 §2.3);
- kernels and NIC firmware;
- hardware.

So a gateway moves its sessions before it stops, **make-before-break**. The client opens the new connection
while the old one keeps carrying its input and state, and the old path closes only after every contributing
cell has switched.

**Relocating one session** s from gateway instance G to instance G' on another box, at `session_epoch` e:

| Step | Who | Action | Budget |
|---|---|---|---|
| 1. Mint | G → Session service | `MintRelocation` (§1.3), batched ≤ 64 per 10 ms. It CASes `sess:<s>` to `(e+1, G', relocating from G)` and returns a 10 s token for G' and a fallback. The token's user data carries `relocate{from G, prev_epoch e, budget_kbit, srtt_ms, contributors, home first}`. A lost CAS (a reconnect or relog won) skips s | ≤ 20 ms p99 |
| 2. Offer | G → client | `Relocate{token}` on CONTROL (≈ 2 KB, reliable fragmentation, 04 §2.2). It travels under the old connection's AEAD, so it is as protected as the HTTPS path | 1 RTT |
| 3. Connect | client → G' | The client opens a second netcode connection to G' while the old one stays live. From the handshake until step 6 it sends **every input frame on both connections**, and the home cell de-duplicates by input sequence, as after a handoff (04 §6.3). G' takes the route and the contributor list from the token instead of calling placement, and seeds the session's AIMD budget and RTT from it (04 §2.5) | 1.5 RTT |
| 4. Rebind | G' → contributors | `ClientRebind{s, e+1, mode = relocate}` to the home cell and every listed contributor, then a `RemoteViewer` from the home cell's next `ViewerState`. Each contributor switches as listed below the table, **keeping its acked baselines**, and answers `RebindAck`. A cell outside the list that G' now registers starts as any new registration (04 §6.6) | ≤ 1 trunk RTT + 1 tick |
| 5. Close the old path | G | G keeps forwarding s's input and chunks until it holds `RebindFence` from every contributor it had (at most 1 s). It then sends `PathClosed{e}` on the old connection's EVENT_R, after every EVENT_R message it carried | ≤ 1 tick after step 4 |
| 6. Done | G' → Session service, client | With every `RebindAck` in, G' reports `RelocateDone{s, e+1}`, and the Session service sends G `SessionMoved`. G' sends `RelocateDone` on CONTROL. The client stops sending on the old connection and closes it after `PathClosed`, or after 2 s. G forwards replies to s's outstanding Go RPCs (chat, market) that arrive by then, then frees the slot | whole relocation ≈ 0.3–0.6 s, ≤ 2 s p99 |

What a contributor does at step 4:
- re-keys its per-(s, cell) state from G's trunk to G''s: ghost records, **acked baselines (kept)**,
  accumulators and grant;
- ORs every chunk mask in flight via G, sent but neither acked nor reported lost, into `lost_mask`, and
  ignores G's later acks and losses for s, which that mask already covers;
- sends `RebindFence{s, e+1}` to G as its last message for s on that trunk;
- from its next tick, writes s's chunks, `Demand` and EVENT_R only to G';
- answers `RebindAck` to G'.

**Why nothing is lost, duplicated or popped.**
- **Input** is sent on both paths between steps 3 and 6 and de-duplicated by sequence, so the home cell sees
  no gap beyond ordinary loss.
- **State.** The client's entity table is continuous, so baselines stay valid and nothing is created. The
  rebind needs no `HeldEntities` and no full masks.
  - Chunks from both paths obey 04 §6.6's per-handle tick rule, so a late chunk via G cannot overwrite a
    newer one via G'.
  - A chunk that died on the old path is covered by the ORed in-flight mask.
- **Reliable messages.**
  - EVENT_R is ordered only within one connection. The client therefore holds EVENT_R that arrives on the new
    connection until the old one delivers `PathClosed`, which comes after every contributor's `RebindFence`.
    The hold is ≈ one RTT.
  - Go RPC replies are matched by request ID.
  - Pushes such as chat and mail notices may arrive on both paths during the overlap. They are de-duplicated
    by message ID.
- **Voice.** G' registers s's voice channels with `helios-voice` at step 4. Frames may arrive on both paths
  for ≤ 1 s, and the jitter buffer drops duplicates by stream sequence (04 §2.7).
- **Handoffs and zone transitions commute with a relocation.** A `HandoffOffer` session record and a
  `RouteUpdate` carry s's `session_epoch`. A cell that adopts a record at e and then receives
  `ClientRebind{e+1}` applies the rebind at once.

**Failures.**
- **The client cannot reach G' within 10 s**, for example because of UDP filtering or a route loss. It
  abandons the attempt and keeps playing on G.
  - The marker expires 15 s after the mint, and the Session service **aborts back**: `sess:<s>` becomes
    `(e+2, G)`.
  - G sends `ClientRebind{s, e+2, mode = relocate}` over its own trunks. This returns any contributor that had
    switched, under the same rules, and makes the unused e+1 token worthless.
  - G tries two more targets. After that it falls back to a paced reconnect, the Phase 3 path below, which
    `gw_relocate_fallback_total` counts.
- **G' dies mid-relocation.** The same abort-back returns every contributor that had switched to G.
- **G dies mid-relocation.**
  - If the new connection is up, G' completes the relocation. Contributors that had not switched see the dead
    trunk (04 §6.6, rule 1). The client sends `HeldEntities` on the new connection when the old one dies
    before `RelocateDone`, and those contributors rebuild from it.
  - Otherwise the client redeems its reconnect ticket, as after any gateway loss.

**`DrainGateway(box, reason, deadline)`** (orchestrator, §1.4):
1. **Refusals.** The orchestrator refuses while it is degraded (§1.4.4), while `GatewayAzHeadroomLow` is
   open, or while another box of the same zone is draining or restarting. Reason `evacuate` overrides all
   three.
2. **Surge first.** It opens the zone's warm spare (§6.7: booted, announced, offering 0 slots) on the target
   build before it withdraws any slot. The box rule and the zone rule (§6.7) therefore hold throughout, and a
   zone loss during a roll is still absorbed. Without the surge, removing one box at 50k CCU would break the
   zone rule: the 7 boxes left outside a zone hold 56k slots against a need of 50k + 1.2 × ⌈50k / 7⌉ ≈ 58.6k.
3. **Stop tokens.** The box is marked `draining`, and the Session service no longer lists its instances in
   new, reconnect or relocation tokens.
4. **Relocate.**
   - Sessions move at **≤ 500 relocations/s per box and ≤ 1,000/s per shard**: at most 2 boxes drain at once,
     in different zones.
   - Sessions in zones at overload stage ≥ 4 go last.
   - Targets are only boxes already on the target build, the opened spare first. They are chosen by free
     slots, with the draining box's zone preferred, so a session moves at most once per roll.
5. **Restart.** The empty box restarts on the new build, or goes to maintenance.
   - It must pass a self-test: a loopback bot handshake and a trunk to one cell per zone.
   - It then rejoins as the zone's warm spare, or with slots if the roll continues.
   - Only then does the next box in that zone start draining, so a zone never has more than one box out.
6. **Overrun.** Sessions still on the box at the deadline take the paced-reconnect fallback, and
   `GatewayDrainOverdue` (SEV3) is raised. A drain also pauses, and raises `GatewayRelocateFallbackHigh`
   (SEV3), when more than 1 % of its relocations in the last minute fell back.

**Rates and times.**
- **Why 500/s.**
  - Mints take ≤ 1,000/s of the Session service, whose six replicas are sized for ≥ 2.5k tokens/s each
    (§1.3). That leaves ≥ 14k/s for a box that dies during the roll, whose storm needs 10k redemptions in
    ≤ 5 s (04 NS-4.7).
  - A relocation costs each contributor ≈ 20 µs, for the re-key and the mask OR. At 1,000/s and ≤ 10
    contributors each, spread over 240 cells, that is ≈ 0.1 ms per cell per second.
  - The handshakes cost < 1 % of a gateway core.
- **Times.**
  - A full box of 8k sessions drains in ≈ 16 s. A typical Phase 4 box at 50k CCU, ≈ 4.2k sessions, drains in
    ≈ 9 s.
  - A binary roll takes ≈ 1.5 min per box with two zones in parallel, so **≈ 10 min for all 12 boxes**.
  - A host patch with a ≈ 10 min reboot takes ≈ 1 h per shard.
- **Rebalance.** The same drain with reason `rebalance` and no restart spreads sessions back over three zones
  after a lost zone returns (§6.7), at ≤ 200 relocations/s per box.

**Phase 3 has no relocation.** A gateway roll is a **paced reconnect** in an announced low-CCU window:
- the gateway sends each session `Kick{reason = GATEWAY_DRAIN, reconnect_now}` on CONTROL and closes it, at
  ≤ 200 sessions/s per box;
- the client redeems its reconnect ticket at once, with no loss detection and no jitter;
- players see a ≤ 3 s "Reconnecting" overlay, bounded by NS-4.7's 10 s path, while the avatar stands in the
  world without input.

**Dev and CI.** `helios-dev up --gateways 2` runs two gateways on `127.0.0.1:7777` and `:7778`, and
`helios-admin gateway drain <id>` exercises the whole path. A relocation integration test runs in Windows and
Linux CI (§8).

**Metrics:** `gw_relocations_total{outcome}`, `gw_relocate_seconds`, the home cell's
`relocate_input_gap_ms`, the client's `relocate_state_gap_ms` and `gw_relocate_fallback_total`. Acceptance is
A14's clause (c).

### 6.4 Capacity & egress cost (R07 flag 6)
Assumptions, to be replaced by vendor quotes at Phase 3:
- 256 kbit/s average downstream per player;
- average CCU = 50 % of peak;
- egress at $0.02–0.05/GB;
- one 8-vCPU cell per 250–500 players, plus a warm pool sized to the largest blast-radius domain (§1.4.6):
  ≈ 30 % of cell capacity at Phase 3, ≈ 13 % at Phase 4;
- from Phase 4, one 2-vCPU replicant per ≤ 4 world-zone cells (04 §6.4), about 6 % more compute. Replicants
  sit in their cells' availability zone (§1.4.6), so their ≤ 20 Mbit/s per-cell streams (≈ 4 Gbit/s at
  Phase 4 peak) add no cross-zone traffic;
- in colo, one spare SERVER host per shard as node-maintenance surge (§6.1a);
- gateways at 8k sessions per 8-core box, spread over 3 availability zones and sized so that two zones hold
  every session and still meet 04 §2.6's box rule (§6.7): 12 boxes at 50k CCU, ≈ $1.2–2.8k/month more than
  the 8 a box-only rule needs, or 2–3 % of the Phase 4 bill.

| Peak CCU | Egress peak | Per month | Cloud $/month | Cells |
|---|---|---|---|---|
| 5k (Phase 3) | 1.3 Gbit/s | ~210 TB | $4–11k | 15–25 |
| 50k (Phase 4) | 12.8 Gbit/s | ~2.1 PB | $42–105k | 120–240 |
| 100k (Phase 5) | 25.6 Gbit/s | ~4.2 PB | $84–210k | 240–480 |

Patch egress is similar (200k MAU × 2 GB/week ≈ 1.7 PB/month). Colo bandwidth is typically several times
cheaper. Storage sizing is in §3.6.

### 6.5 Security
- **Secrets:**
  - Dev uses local keys, staging SOPS/age, prod Vault/KMS via External Secrets.
  - The manifest **root key stays offline**; subkeys rotate quarterly.
  - The shard netcode key rotates daily by draining gateway instances (§1.3).
  - Subject-key KEKs live in KMS and rotate yearly by re-wrapping DEKs. The blind-index pepper never leaves
    KMS (§6.6).
  - 04's X25519 re-key (forward secrecy) is a Phase 4 security-review item.
- **Transport:** TLS 1.3 at the edge, mTLS between services.
- **NATS: a generated permission matrix, per-process credentials and authenticated callers.** Phase 2 has
  per-role users with the generated matrix and its CI check. From Phase 3, before the public beta, there is a
  credential per process and the caller check.
  - **Generated matrix.** `schemac` emits `nats-perms.json` from the `.hschema` service blocks, their
    declared callers (`@callers`, service-lane marks) and the declared event, control and KV subjects. It is
    one entry per role, listing publish, subscribe, `allow_responses` and KV buckets. Deployment renders it
    into NATS account and user templates, and nothing is written by hand. The table below is a summary, and
    the generated file is authoritative.

    | Role | Publishes and requests | Subscribes and serves | KV |
    |---|---|---|---|
    | Cell (`helios-cell`) | `rpc.<shard>.{orch,ledger,persist,fence,character,activity,group,chat,world,trust}.>`; `rpc.<shard>.ws.>` (world-script calls, §1.23); `persist.<shard>.*.<self>`; `tel.<shard>.<self>.>`; the event subjects its schema declares (`World.emit`) | `ctl.<shard>.cell.<self>.>`, `ctl.<shard>.cell.all.>` (04 §6.6's `gateway_dead`), its own reply inbox | read: `DIRECTORY`, `CONFIG`, `PERMS`, `ACTIVITY`, `WORLD`, `GROUP` (fleet rosters, §1.12.1). Writes go through services (`Activity.Update`, `SetFlag`) |
    | Replicant (`--replicant`) | `rpc.<shard>.orch.{RegisterProcess,Heartbeat}`; `tel.<shard>.<self>.>` | `ctl.<shard>.cell.<self>.>` | read: `DIRECTORY` |
    | World-script host (`--role world-script`) | `rpc.<shard>.{orch,worldscript,world}.>`; `tel.<shard>.<self>.>` | `rpc.<shard>.ws.>` (only the partitions it holds answer; commits are lease-fenced, §1.23); its `ws.<script>` event and timer consumers; `ctl.<shard>.cell.<self>.>` | read: `DIRECTORY`, `CONFIG`, `WORLD` |
    | Gateway | `rpc.<shard>.orch.{Heartbeat,ReportSuspect}`; `rpc.<shard>.session.>`; the service-lane RPCs that schemac marks callable by gateways (market, mail, chat, social, groups, cases; §2.4), on `rpc.<shard>` or `rpc.g`; `tel.<shard>.<self>.>` | `ctl.<shard>.gateway.<self>.>`, `ctl.<shard>.gateway.all.>`, `chat.>`, `presence.>` | read: `DIRECTORY`, `CONFIG`, `PERMS`, `GROUP` |
    | Voice forwarder (`helios-voice`) | `rpc.<shard>.orch.{RegisterProcess,Heartbeat}`; `rpc.<shard>.trust.>` (voice-ring snapshots, §1.16); `tel.<shard>.<self>.>` | `ctl.<shard>.voice.<self>.>` | read: `GROUP` |
    | Go service `<svc>` | requests to the services its block declares as dependencies; outbox publishes to its own `evt.<scope>.<svc>.>`; its declared `ctl` subjects (orchestrator, persistence's `released`, GM) | serves `rpc.<scope>.<svc>.>` (queue group `<svc>`); its declared durable consumers | writes only the bucket it projects (orchestrator → `DIRECTORY`, config → `CONFIG`, social → `PERMS`, activity → `ACTIVITY`, world → `WORLD`, group → `GROUP`) |

  - **CI check.**
    - Every integration test and bot run connects with the generated credentials, and any server
      `Permissions Violation` fails the run.
    - A static check resolves every subject literal and subject builder in Go (`pkg/bus`) and C++
      (`engine/server`'s NATS bus) to a declared subject, so a subject the code uses cannot be missing from
      the matrix.
  - **Two accounts per environment.** `svc` holds the Go services, the world-script hosts and the JetStream
    streams and buckets. `sim` holds cells, replicants, gateways and voice forwarders.
    - `svc` exports its RPC and stream-publish subjects to `sim` as **service exports with `share: true`**.
      The NATS server therefore stamps every cross-account request with a `Nats-Request-Info` header naming
      the calling user. A client cannot set or forge that header, and JetStream stores it with a `persist`
      message.
    - `ctl`, `chat` and `presence` are exported the other way as needed.
    - The read-only buckets are mirrored into `sim`, as §6.7 mirrors global buckets into shards, so
      simulation processes hold no JetStream API rights in `svc`.
  - **Per-process credentials (Phase 3).**
    - A process proves its pod with a projected Kubernetes service-account token (on VMs, the
      `helios-agent` machine credential) to the orchestrator's credential endpoint (mTLS). It receives its
      process ID and an nkey user JWT whose name is `p<proc_id>` and whose tags are `role:<role>` and
      `proc:<proc_id>`. It is signed by a role-scoped signing key whose template fills `<self>` from the tag.
    - JWTs expire after 30 days and are renewed after 7. The client reconnects with the new one, a
      sub-second NATS blip that nothing on the tick path notices.
    - A process confirmed dead, deregistered or deliberately replaced (§1.4.3) is revoked in the account JWT
      at once, so a zombie also loses the bus.
  - **Caller checks.**
    - The ledger, persistence (fence operations and `persist` batches) and the WSH reject a request whose
      `Helios-Fence` cell, or whose `persist` subject token, differs from the `proc` tag in
      `Nats-Request-Info`.
    - The fence check then also requires the `authority_fence.owner_cell` of the AG named in the header to
      be that process (§1.13). A compromised cell therefore cannot write fenced requests as another cell.
    - The WSH also requires the caller to hold the region named by the `(region, lease_gen)` it sends.
    - Services accept a player identity injected on the service lane only from a `role:gateway` caller.
    - A mismatch returns `CALLER_MISMATCH` and raises `NatsCallerMismatch` (SEV1, a possible compromise,
      §6.8).
- **Database:** each role is confined to its own schema. `UPDATE` and `DELETE` are revoked on the ledger
  and audit tables. Partition maintenance runs under a separate `retention` role that may only detach and
  drop partitions on the expiry schedule.
- **Rate limits (R07 §4):** at the gateway (per message type), at the API edge (GCRA per account/IP), and
  per domain (chat, orders, mail).
- **DDoS:** cells are never public, and the token challenge blocks amplification (04). APIs sit behind a
  CDN/WAF with bot protection on `Login`, and the login queue is the pressure valve.
- **Supply chain:** `govulncheck`, license checks, SBOMs, signed binaries. Messages are capped at 1 MiB.
  The admin console is reachable via SSO/VPN only.

### 6.6 Data protection and privacy (GDPR/CCPA)
**Phasing.** Erasure shapes keys and audit rows, so the seams exist from Phase 0 (seams first):
- **Phase 0:** PII only in Identity, per-account DEKs and the `@pii` lint.
- **Phase 2:** ToS/privacy acceptance, the age gate and retention jobs.
- **Phase 3:** DSAR export and erasure sagas, before public beta.
- **Phase 4:** the SLA gate (A18) and counsel's residency sign-off (09 K32).

**Data classes**

| Class | Examples | Where it may live |
|---|---|---|
| Direct PII | email, date of birth, IP addresses, payment-provider customer token, real names in support tickets | `svc_identity` only, encrypted under the subject key (blind indexes excepted) |
| Pseudonymous IDs | `account_id`, `character_id`, `analytics_id`, wallet and item owners | everywhere. The ledger, audit and telemetry use only these |
| Player-authored text | chat, mail, bios, report text, GM notes | encrypted at rest under the author's or subject's key |
| Chosen names | `handle#discriminator`, character and corp names | plaintext, which search and uniqueness need. Tombstoned or released on erasure |

**Crypto-shredding.**
- Each account has a 256-bit data key (DEK) in `subject_key`, wrapped by a KMS/Vault KEK. Dev uses a local
  key file.
- Identity encrypts direct PII with it: XChaCha20-Poly1305, AAD = table + column + row ID.
- Chat, mail and moderation encrypt stored player text under the author's DEK.
- Services fetch DEKs through `Identity.GetSubjectKeys` (mTLS, role `privacy_keys`). They cache them in
  memory for ≤ 1 h, never persist or log them, and evict them on `evt.g.privacy.shredded`.
- Deleting a DEK makes every copy unreadable, including copies in backups, WAL archives and Parquet, without
  rewriting append-only data.
- `shred_log(account_id, shredded_at)` is also written to WORM object storage. Every restore replays it
  before the database takes traffic.
- Email lookup uses a blind index `HMAC-SHA256(pepper, lower(email))`, with the pepper in KMS.

**Ledger, audit and telemetry stay valid.**
- Ledger rows (`actor_id`, wallet and item owners) and `audit_log` rows hold only pseudonymous IDs.
  Erasure never touches them, so conservation audits and the hash chain stay valid by construction.
- Audit free text is in `audit_note`, and the chain covers its ciphertext digest (§1.17).
- Telemetry carries only `analytics_id`. Deleting that mapping makes the ClickHouse rows anonymous.
- Crafted items and killmails store only character IDs (06 §3), and names resolve at display, so an erased
  crafter shows as "Unknown".

**Retention schedule.** Enforced by partition drops and TTL jobs. CI fails if an `@pii` column has no row
here.

| Store | Data | Retention | On erasure |
|---|---|---|---|
| Identity | account PII | account lifetime | DEK shred |
| Identity | login and IP history | 90 d | shred; partition drop |
| Identity | ToS/EULA/privacy acceptances | lifetime + 6 y (legal claims) | kept, pseudonymous |
| Character | characters, settings blob | lifetime; soft delete 30 d | hard delete; name → `Deleted-<hash>` |
| Ledger, item history, `audit_log` | value movements, GM actions | 90 d online, then Parquet indefinitely (fraud, accounting) | kept, pseudonymous |
| Entitlements | receipts | 10 y (tax) | kept, pseudonymous |
| Chat | whisper / corp / local | 30 d / 90 d / none | DEK shred; partition drop |
| Mail | bodies | 90 d after read, 365 d max | delete; DEK shred |
| Moderation | cases, report evidence | 2 y after closure; held while open | DEK shred at expiry |
| Trust (§1.16a) | session feature vectors, detector scores | 180 d; features attached to a case follow the case | kept, pseudonymous; partition drop |
| Idempotency / outbox / inbox | pseudonymous | 3–4 d / 3 d / 7 d | partition drop |
| JetStream | `EVT` / `TEL` / `AUDIT` | 7 d / 24 h / 30 d | pseudonymous; ages out |
| ClickHouse | raw events / aggregates | 13 months / indefinite (no IDs) | `analytics_id` mapping deleted |
| Loki / Tempo / Prometheus | logs / traces / metrics | 30 d / 7 d / 90 d | redacted at source; ages out |
| Crash dumps | scrubbed minidumps (08 §3) | 90 d | ages out |
| Backups + WAL | everything | 35 d | shred-log replay on restore |
| DSAR exports | zip | 7 d | object lifecycle |

**Log hygiene.** `@pii` fields are redacted by the `slog` handler and the OTel attribute filter. A CI test
logs a canary account at debug level and asserts that nothing reaches Loki.

**Data-subject requests.** They run as a saga in the Identity domain (§4.3). Every service holding personal
data implements `PrivacyParticipant` (§8), and a CI lint fails if a table with `@pii` columns belongs to a
service without one.
- **`ExportMyData`:**
  - It requires re-authentication, plus MFA when enrolled.
  - Each participant exports its section. The result is one zip of JSON (machine-readable, GDPR Art. 20)
    plus an HTML summary, encrypted in object storage and delivered by a signed link valid for 7 days.
  - It covers account data, login history, characters, assets and the full wallet journal (older than 90
    days from Parquet), orders and trades, authored mail and chat within retention, purchases, acceptances,
    moderation actions against the account (staff pseudonymized) and a telemetry summary.
  - **SLA ≤ 72 h**, target 24 h. GDPR allows a month.
  - Self-service allows one request per 30 days; support can raise more.
- **`EraseAccount`:** it becomes final immediately, or after an optional 7-day cancel window. Then, as
  idempotent steps:
  1. lock the account, revoke refresh-token families and end sessions (`session_epoch` bump);
  2. cancel orders and contracts (escrow refunds) and return mail attachments to senders;
  3. leave corps (a CEO seat passes to the senior director, or the corp closes);
  4. burn remaining currency and destroy owned items through the ledger with reason
     `Sink.Account.Erased`, so conservation holds. This step starts only once every AG of the account is
     dormant (step 1's session end makes cells park them, 04 §6.1), and its ops carry the dormant-fence
     precondition (§4.1);
  5. hard-delete characters (names tombstoned) and delete settings, friends, blocks and presence;
  6. delete the `analytics_id` mapping;
  7. send the final confirmation email;
  8. shred the DEK and write `shred_log`;
  9. quarantine the handle for 30 days, then release it.

  **SLA ≤ 72 h after the request is final.** Legal holds (an open moderation case, a chargeback, a
  preservation order) delay only the affected records. Other players keep what they own: items the subject
  gave away, corp hangars, trades and received mail.

**Age gating and legal acceptance.**
- Registration collects date of birth and country. Only an age band and the date the band ends are stored
  in plaintext; the DOB is encrypted.
- Accounts below the country's digital-consent age are refused: 13–16 under GDPR Art. 8, 13 under COPPA.
  There is no parental-consent flow before Phase 5.
- Accounts aged 13–17 get strict chat filtering, no whispers from non-friends, and store spending caps.
- ToS, EULA and privacy policy are versioned `legal_doc(kind, version, locale, sha256, effective_at,
  requires_reaccept)` rows. Identity records each acceptance and enforces re-acceptance (§1.1; launcher:
  08 §2.3).
- We do not sell or share personal data for advertising, so the CCPA opt-out is a recorded no-op.

**Residency.** Decision:
- Direct PII lives only in the global tier's EU home region, and its DR standby is also in the EU (§6.7).
- Shards anywhere hold only pseudonymous data, plus encrypted text whose keys stay in the EU.
- Transfers of pseudonymous game data to non-EU shards rely on SCCs or the EU-US Data Privacy Framework.
  Counsel signs off before Phase 4 (09 K32).
- A per-residency identity partition (for example APAC) is a Phase 5 option behind the same API.

### 6.7 Disaster recovery and regional placement
**Placement.**
- **Each shard** runs in one region across 3 availability zones, or 3 failure domains in colo:
  - shard PG with a synchronous quorum standby;
  - the persistence cluster with an async replica;
  - NATS R3, one node per zone;
  - Valkey with a Sentinel quorum spanning the three zones;
  - cells in every zone, and gateways spread evenly over the zones and sized for a zone loss (below).
- **The global tier** (identity, social, global chat channels, content, config, audit, entitlements, patch
  manifests) runs in the EU home region across 3 zones. A warm DR region, also in the EU, holds a
  CloudNativePG replica cluster, mirrored object storage and scaled-to-zero Deployments.

**Latency budget for global calls from shards.**
- Only these may cross regions synchronously: session start (`Login`, refresh, `CreateLaunchCode`,
  entitlement check), global-channel chat, social mutations and uncached `GetSubjectKeys`. Each is one
  round trip at ≤ 150 ms RTT (EU↔US-West).
- Nothing on a tick, handoff, ledger, checkpoint or fence path calls the global tier. A service-registry
  lint enforces this.
- Shards read global data from local mirrors:
  - JWKS, cached 24 h;
  - `PERMS`, `CONFIG` and content pins, through NATS leaf nodes and KV mirrors;
  - audit, through a stream source.

**When the global tier is down:**
- Players in the world keep playing. JWTs are verified locally, and ledger, market and mail are shard
  services.
- New logins and refreshes fail, and the queue shows maintenance.
- Global chat channels and social edits pause. Local chat works.
- Permission masks and config freeze at their last values.
- Store purchases queue at the provider.

**RPO/RTO per store**

| Store / service | Zone loss | Region loss: RPO / RTO | Mechanism |
|---|---|---|---|
| Shard PG (ledger, character, market, fence, orchestrator) | RPO 0; RTO < 30 s (A13) | ≤ 60 s / ≤ 4 h (≤ 1 h from Phase 4) | quorum sync standby; WAL and base backups copied cross-region; async standby in the DR region from Phase 4 |
| Persistence cluster | ≤ checkpoint window; < 60 s | ≤ 5 min / with the shard | async replica; loss rolls back only position and XP |
| Global PG | RPO 0; < 30 s | ≤ 60 s / ≤ 1 h | CNPG replica cluster in the DR region |
| NATS JetStream | none (R3) | rebuilt / ≤ 15 min | streams recreated; outbox re-publishes unsent rows; KV projections rebuilt from PG |
| Valkey | rebuilt (§3.4) | rebuilt | never the truth |
| ClickHouse | replica | ≤ 24 h / ≤ 24 h | daily backup; analytics only |
| Object storage (backups, anchors, `shred_log`, manifests, CDN origin) | multi-zone | ≤ 15 min / minutes | cross-region replication; CDN origin failover |
| Cells, gateways | cells: warm standby outside the lost AZ (wide-loss hold, §1.4.3), restored from **checkpoints**, losing ≤ 30 s of non-value state (one checkpoint window) and 0 value. Replicants sit in their cells' zone (§1.4.6), so the lost zone's replicants go with it; an AG whose replicant was in another zone restores from it (≤ 1 s). Gateways: reconnect tickets onto the surviving zones' pre-provisioned slots (below) | with the shard | state is in checkpoints, and from Phase 4 also in replicant RAM (04 §6.4), which covers losses inside the blast radius |

**Gateway capacity for an availability-zone loss (Phase 3 sizing, Phase 4 drill).** 04 §2.6's box rule
(free slots ≥ 1.2 × the sessions on the most loaded box) covers one box of ≤ 8k sessions. A zone holds a third of
the shard's sessions, ≈ 17k at 50k CCU. Bare-metal anycast gateways also cannot be bought, racked and
announced within the box rule's 60 s. The policy is therefore to **size for it**: capacity for a zone loss is
provisioned in advance, and degradation is only a backstop.
- **Spread.** Gateway boxes are spread evenly over the shard's 3 zones, with box counts within ±1 of each other.
  Every connect token lists instances in ≥ 2 zones (§1.3).
- **Zone rule.** For every zone z, the slots on gateways outside z must hold every session on the shard and
  still meet the box rule afterwards:
  `slots_outside(z) ≥ CCU + 1.2 × ⌈CCU / boxes_outside(z)⌉`. The orchestrator checks it every 10 s against
  reported free slots. `GatewayAzHeadroomLow` pages SEV2 after 10 min of violation. The box rule and its
  60 s warm spare (04 §2.6) still apply.
- **Sizes.**

  | Phase | CCU | Sessions per box | Boxes (per zone) | Slots | After a zone loss: boxes, slots, free | Box rule after the loss |
  |---|---|---|---|---|---|---|
  | 3 | 5k | 2,048 (NS-2.6) | 6 (2) | 12,288 | 4, 8,192, 3,192 | 1.2 × 1,250 = 1,500 ✓ |
  | 4 | 50k | 8k (NS-4.4) | 12 (4) | 96k | 8, 64k, 14k | 1.2 × 6.25k = 7.5k ✓ |

  After a zone loss, the Phase 4 shard is exactly 04 §2.6's 8-box configuration, so a further box loss is still
  absorbed. The 4 extra boxes cost ≈ $1.2–2.8k/month (§6.4); egress is unchanged.
- **Admission.** The login queue (§1.2) admits new sessions only while the zone rule would still hold after the
  admission: `CCU < min over z (slots_outside(z) − 1.2 × ⌈CCU / boxes_outside(z)⌉)`. At Phase 4 that is
  ≈ 55.6k (64k / 1.15), above the 50k shard cap, so normally the shard cap binds. When boxes are lost or
  drained, the gateway cap binds instead: queued players see its ETA, and reconnects are never counted
  against it.
- **Restoring headroom.** After a zone loss, `GatewayAzHeadroomLost` (SEV2) opens a hardware ticket. The shard
  runs on the box rule until the zone returns or boxes are added, which takes hours to days. The box rule's
  60 s response is met by a pre-racked **warm spare** per zone (booted, announced, offering 0 slots), never by
  procurement. A gateway roll borrows its zone's spare only while one box drains and restarts, and the drained
  box then becomes the spare (§6.3.1). The active box count per zone therefore never drops during a roll.
- **Session, Valkey and voice.** The Session service keeps ≥ 2 replicas per zone and redeems ≥ 20k tickets in
  ≤ 8 s with a third of them gone (§1.3). Valkey's Sentinel quorum spans the three zones
  (`down-after-milliseconds` 3,000; failover ≤ 8 s), and tickets are self-contained, so a failover delays
  redemptions without failing them. `helios-voice` forwarders spread over the zones; their channels rehash
  in ≤ 1 s (04 §2.7).
- **Backstop, not plan.** Suppose the zone rule was already failing when the zone was lost (for example, a
  second fault or an ignored SEV2). The Session service still never answers a reconnect with "full". It holds
  the redemption in a **reconnect lane** ahead of every new login, returns an ETA the client shows on its
  "Reconnecting" overlay (08 §1.2), and pauses new-login admission while any reconnect waits. A19 requires this
  lane to stay empty.
- **What players see.**
  - *Gateway lost, cell alive* (≈ 2/3 of the lost zone's gateway sessions): back in the world in ≤ 20 s. The
    budget is 2 s to detect the loss, ≤ 2 s of jitter, ≤ 8 s for a Valkey failover or the redemption
    backlog, ≤ 4 s of backoff granularity, 0.5 s of handshake and `ClientRebind`, and ≤ 1 s of near-tier
    resync: ≈ 17.5 s.
  - *Cell lost:* the "Reconnecting" overlay stays up until the player's region is served again. That is ≤ 5 min
    after the wide-loss hold ends, in descending CCU order (§1.4.6). The player resumes from the last
    checkpoint: position and XP roll back ≤ 30 s, and value never does.

**What a region loss costs.** Losing a shard's region loses at most the last 60 s of committed ledger
transactions. PITR restores a transaction-consistent point, so trades vanish atomically on both sides,
conservation holds, and players are told the rollback time. RPO 0 across regions would add a cross-region
round trip to every ledger commit and break the p99 < 25 ms SLO, so it is rejected.

**Planned region evacuation** (provider maintenance, a cost move, an impending outage):
1. T−30 min: announce on the launcher banner and the status page, then stop admission.
2. Switch over to the DR standby after replay lag reaches 0, so RPO is 0.
3. Drain cells with a final checkpoint.
4. Start cells, gateways and services in the target region from the same images.
5. Move the gateway anycast/DNS and the pointer's `cdn_hosts`.
6. Resume admission.

Target: ≤ 30 min of downtime and 0 value loss. It is rehearsed every quarter from Phase 4 (A19).

**Unplanned region loss** is SEV1.
- **Global tier:** promote the DR replica cluster and scale up its Deployments (RTO ≤ 1 h).
- **Shard:** restore in the DR region from the cross-region WAL archive (RPO ≤ 60 s). Replay `shred_log`,
  then run the conservation audit before opening.

### 6.8 On-call, incidents and runbooks
**Rotation.**
- **Phases 1–2:** business hours, best effort (internal and alpha).
- **Phase 3 (beta):** 24/7 primary and secondary, weekly handover, SEV1 acknowledged in ≤ 15 min.
- **Phase 4 (launch):** follow-the-sun where staffing allows. SEV1 acknowledged in ≤ 5 min and SEV2 in
  ≤ 15 min. An incident-commander rota is kept separate from responders, and service owners (CODEOWNERS) are
  the escalation path.

**Paging.** Prometheus Alertmanager feeds a paging service, whose vendor is chosen at Phase 3. Every alert
rule has a `severity`, a `runbook_url` and an owning team, enforced by a CI lint.

**Severity.**
- **SEV1:** a shard or login is down, value integrity is at risk (any invariant violation), data is
  breached, or a region is lost.
- **SEV2:** degraded service, such as control-plane degraded > 60 s, queue ETA > 30 min, a feature
  kill-switched, or checkpoint lag > 60 s.
- **SEV3:** a single cell or instance, or internal tooling.
- **SEV4:** no player impact.

**Incident process.**
- SEV1/2 incidents have incident-commander, comms and ops roles.
- The status page is updated within 15 min, then every 30 min.
- A blameless postmortem is written within 5 working days for SEV1/2, in `docs/postmortems/`, with owners
  and due dates for every action item. Action items open > 30 days are reviewed at each phase gate.

**Status page.** A static page on the CDN, independent of game infrastructure, edited from the admin
console. The same incident feed drives launcher banners through live config (08 §2.4).

**Error budgets** are per §6.2 SLO, over 30 days.
- Alerts are multi-window burn rates: page at 14.4× over 1 h and 5 min; open a ticket at 6× over 6 h and
  30 min.
- A service that exhausts its budget freezes feature releases (reliability fixes only) until it recovers.
- Burning half the budget in 7 days moves reliability work to the top of the next build round.
- Ledger invariant violations have no budget.

**Game days.** Each runbook is executed at least quarterly in staging through the chaos suite (§8). Each
failure mode's drill is a nightly or weekly chaos test (09 §6).

**Runbooks** live in `deploy/runbooks/<alert>.md` and are linked from each alert's `runbook_url`.

| Failure (R07 §8.4 + Helios) | Alert | Automatic response | First human steps |
|---|---|---|---|
| Cell crash | `CellConfirmedDead`; crash loop ≥ 3/h | warm standby (§1.4.6), restored from the replicant in world zones | confirm the standby served; triage the dump in Sentry; on a crash loop, pin the zone to the previous content build |
| Host or rack loss | `FailureDomainLost` | domain-loss confirmation by H+T or H+R; parallel recovery to standbys outside the domain; no degraded mode (§1.4.3, §1.4.6) | check the host or ToR; confirm every region is served and the pool refilled; after the heal, confirm zombie writes were rejected |
| NATS-isolated cell | `CellNatsIsolated` | none for 60 s, then a planned replacement over a gateway trunk | check the host's NATS path (firewall, DNS, client bug) |
| Warm pool below the worst domain | `WarmPoolBelowDomain` | Fleet scale-up; node maintenance pauses admission (§6.1a) | add SERVER capacity or fix the spread constraints |
| Node maintenance stuck | `NodeDrainOverdue` (SEV3) | the roll pauses; nothing is forced (§6.1a) | find the process that will not drain (migration aborts, hold); resume, or `helios-admin node evacuate` for failing hardware |
| Forged or mismatched caller on NATS | `NatsCallerMismatch` (SEV1), permission violations | the request is rejected (§6.5) | revoke the process's credential, isolate its host, run the conservation audit, open a security incident |
| Standby replay falling behind | `StandbyReplayLagHigh` (p99 > 1 s for 5 min) | — | compare WAL per writer with the §3.6 write mix; throttle the offending writer; apply §3.6's next move |
| Trust detector misfiring | `TrustDetectorPaused` (appeal overturns > 2 % of a wave) | the detector stops flagging (§1.16a) | re-evaluate it on the corpus; re-tune the threshold; review its open cases |
| Standby crash loop | `StandbyCrashLoop` (SEV1) | poison-build brake stops recovery on that build (§1.4.6) | pin the previous server build; triage the dumps |
| Replicant crash (Phase 4) | `ReplicantDown`; `replicant_lag_ms` > 1,000 | `AssignReplicant`; cells resend full state (04 §6.4) | confirm the resync; check trunk loss and replicant host load |
| Cell overload | `TickBudgetBurn` | overload ladder (04 §8) | check TiDi and admission; `PreProvision`; move instances |
| Gateway crash | `GatewayDown` | reconnect tickets (04 §6.4) | replace the node; check anycast health |
| Gateway drain stuck, relocations falling back | `GatewayDrainOverdue`, `GatewayRelocateFallbackHigh` (SEV3) | the drain pauses; remaining sessions take the paced-reconnect fallback at the deadline (§6.3.1) | check UDP reachability of the target boxes and `MintRelocation` latency; resume, or re-issue as `evacuate` |
| Gateway zone headroom | `GatewayAzHeadroomLow` (SEV2 after 10 min); `GatewayAzHeadroomLost` after a zone loss | the login queue caps admission at the gateway cap; reconnects keep priority (§6.7) | undrain or add boxes; after a zone loss, open the hardware ticket and confirm the box rule holds |
| World-script failure | `WorldScriptDead` (an input in `ws_dead`), `WorldScriptCommitLag` > 5 s, `WorldScriptFaucetCap`, escrow-backing mismatch (SEV1) | partition recovery like a cell (§1.23); a mismatch trips the `ws.<script>` kill switch | inspect the input and the handler; hot-fix the script; replay from `ws_dead`; run the conservation audit |
| Handoff failure | `HandoffAbortRate > 1 %` | abort + backoff | check fence latency and N/N+1 mesh skew |
| Fence contention | `FenceLockWaitHigh` (p99 > 3 ms for 5 min), `FENCE_BUSY` > 0.01 %, `FenceMultixactAgeHigh` | callers retry within their budgets (§1.13) | find the hot tree from `fence_lock_wait_ms` by root; check that cell's fence gate and ledger mix; vacuum `authority_fence`; rerun §3.6 |
| Split brain | `StaleEpochWrite`, `FenceConflict` | fencing (§4.1) | find and kill the zombie; run the conservation audit |
| PG primary failure | `PGPrimaryDown` | CNPG failover | confirm promotion; drain the outbox backlog; run the conservation audit |
| NATS node failure | `NatsNodeDown`, `StreamNoLeader` | Raft; degraded mode if needed | replace the node; confirm exit from degraded mode |
| Control plane degraded | `ControlPlaneDegraded` > 60 s | freeze (§1.4.4) | restore NATS or PG quorum; after exit, check cells that crashed while frozen |
| Valkey failure | `ValkeyDown` | rebuild (§3.4) | watch the queue rebuild and session re-presentation |
| Bad deploy | `CanarySLOBreach` | canary halt + rollback | pin the previous image or content build |
| Economy exploit | `ConservationViolation`, trust rule hit | kill switch | freeze the feature; `rollback-character` with taint; bans |
| DDoS | `EdgeTrafficAnomaly` | scrubbing | rotate gateway IPs; tighten login admission |
| Stuck saga / DLQ | `SagaParked`, `DLQDepth > 0` | park | inspect and fix; replay within the inbox horizon |
| Checkpoint lag | `CheckpointLag` > 60 s | — | check persistence-cluster I/O and `PERSIST` partition balance |
| Storage growth | `PartitionDropFailed`, disk > 70 % | — | fix the retention job; extend the volume; rerun §3.6 |
| ID blocks low | `IdBlockReserveLow` | background refill | check `AllocateIdBlocks` latency and PG |
| Region loss | `RegionUnreachable` | — | §6.7 DR procedure |
| Privacy SLA | `DSARDueIn24h` | — | unblock the stuck participant; escalate to the privacy owner |

---

## 7. Patching / CDN

**Chunk store.**
- **Chunking:** FastCDC with min 16 KiB, average 64 KiB, max 256 KiB (R07 §7). Pak files are compressed per
  asset/block in a stable order (ADR-006).
- **Chunk ID:** the **BLAKE2b-256** of the uncompressed bytes (Monocypher 4.0.3 in C++,
  `x/crypto/blake2b` in Go). XXH3 is not used here because it is not adversary-resistant.
- **Storage:** chunks are stored zstd-19 (`klauspost/compress` in Go).
- **Packs:** chunks under 32 KiB are grouped into ~8 MiB packs and fetched with coalesced range requests.

**CDN layout.** Everything except the pointers is immutable, so nothing is ever invalidated.

```
/chunks/<aa>/<bb>/<blake2b-hex>.zst        Cache-Control: public, max-age=31536000, immutable
/packs/<hash>.pack  /packs/<hash>.idx
/manifests/<product>/<build-id>/<platform>.hman        signed, immutable
/patches/<from-hash>_<to-hash>.zpatch                  optional (zstd --patch-from)
/channels/<product>/<channel>/<platform>.json          signed pointer, max-age=60
/keys/<product>/keyset.json                            root-signed subkey list, max-age=60 (08 §2.10.3)
```

**Manifest (`.hman`).** A schema-generated binary, zstd-compressed. It holds:
- build, monotonic `sequence` and platform;
- per file: path, size, hash, chunk list, tags (language, optional content, vaulting — R05-P2-25) and an
  install **tier** (0 = launcher/client/login area, 1 = common, 2 = streamable regions);
- a pack index and patches.

**Signing and rollout.**
- The manifest is Ed25519-signed (`crypto/ed25519`). The launcher checks the signature with
  `monocypher-ed25519` (ADR-013) before running anything.
- The pointer is `{build_id, sequence, manifest_hash, compat_epoch, min_launcher, min_client, cdn_hosts[],
  next?{build_id, manifest_hash, compat_epoch, available_at}, rollout_pct, expires, sig}`. `next` drives
  pre-download (08 §2.6), and a `next` with a new `compat_epoch` is a coordinated flip (§1.14.1).
- A client takes a staged build only if `hash(install_id) mod 100 < rollout_pct`. The manifest service
  stages only builds of the live compat epoch; another epoch's build is published at `rollout_pct = 100`
  through `next`.
- A lower `sequence` is refused unless the pointer is signed `rollback: true`.

**Delta strategy.**
- Chunk dedup against the local install and a shared cache (casync-style).
- CI-built zstd `--patch-from` patches from the last 3 builds for CDC-poor executables, used only when
  smaller than the missing chunks.
- Never a per-user server delta.

**Launcher protocol** (08 owns the UI):
1. Self-update.
2. Verify the pointer and the manifest.
3. Diff against the local install DB.
4. Source each chunk from local file → cache → patch → CDN.
5. Download over 8 resumable connections (WinHTTP on Windows, libcurl on Linux), verifying each chunk.
6. Stage, then rename atomically under an install journal.
7. The game is playable after tier 0 (R03-P1-6).
8. `verify`/`repair` re-hash everything.

**Channels:** `live`, `ptr`, `beta`, `dev`, `qa`.
- Tokens pin `(compat_epoch, manifest_hash)`. Gateways reject a compat-epoch mismatch only, so a staged
  client build and the live one share every instance, and a server hotfix invalidates no pin (§1.14.1).
- Promotion to live needs two approvers and is audited.
- In dev, `helios-patch publish --channel dev` writes to `helios-data\cdn`.

---

## 8. Go layout, interfaces, testing

`services/`: module `helios.dev/services`, `go 1.27.0` / `toolchain go1.27.1`, `CGO_ENABLED=0`.

```
cmd/        helios-backend helios-admin helios-patch helios-loadgen helios-dev helios-agent
internal/   app/, <service>/{service.go, domain/, store/ (sqlc), events.go, privacy.go}   # one per §1 service
            orch/nodemaint (§6.1a)
pkg/        bus cache idgen authn perm outbox inbox saga ratelimit obs connecttoken cdc manifest hxl
            privacy retention testkit trust (§1.16a)
gen/ proto/ migrations/<svc>/ deploy/{compose,k8s,grafana,alerts,runbooks} seed/ loadtest/ testdata/vectors/
```

`pkg/hxl` is the Go HXL interpreter (06 §1.2). It must agree with C++ on 100 % of the shared corpus.
Go may fuse `x*y + z` into an FMA (it does on arm64 and at `GOAMD64=v3`), so `pkg/hxl` follows 06 §1.2's
cross-language float rules:
- every float multiplication is wrapped as `float64(a*b)`;
- the `det` port has no constant arithmetic;
- the `hxlfloat` vet analyzer runs in CI;
- services build with `GOAMD64=v1`, and `helios-backend` checks the value in its build info at start;
- the corpus, with its FMA-sensitive vectors, runs on `windows/amd64`, on `linux/amd64` at both `v1` and
  `v3`, and on `linux/arm64`.

```go
type Service interface { Name() string; Register(*app.Registry) error
    Start(context.Context) error; Stop(context.Context) error; Health(context.Context) error }
type Store interface { InTx(ctx context.Context, o pgx.TxOptions, fn func(pgx.Tx) error) error } // retries 40001/40P01
type Bus interface { Publish(ctx context.Context, subj string, m hs.Message, o ...PubOpt) (PubAck, error)
    Request(ctx context.Context, subj string, in, out hs.Message) error
    Consume(ctx context.Context, stream, durable string, h Handler) error; KV(bucket string) (KV, error) }
type Placer interface { Allocate(context.Context, ProcRequest) (ProcHandle, error); Release(context.Context, ProcHandle) error
    Status(context.Context, ProcHandle) (ProcState, error) }                     // signal P (§1.4.3)
type IdMinter interface { Next() ID; Reserve() int }                           // blocks from AllocateIdBlocks
type CheckpointStore interface { ApplyBatch(context.Context, []Checkpoint) error
    Load(context.Context, AgID, StreamSeq) (Checkpoint, error) }
type Fence interface { Advance(ctx context.Context, root AgID, expect, next Epoch, owner Owner) (bool, error)
    Park(ctx context.Context, root AgID, e Epoch, owner Owner, finalCkptSeq StreamSeq) (bool, error) // → dormant (04 §6.1)
    Join(ctx context.Context, member AgID, em Epoch, root AgID, er Epoch, owner Owner) (Epoch, error)
    Leave(ctx context.Context, member AgID, e Epoch, owner Owner) (Epoch, error)
    AdvanceMany(ctx context.Context, ags []AgEpoch, owner Owner) (bool, error)
    AdvanceOwnedBy(ctx context.Context, region RegionID, gen LeaseGen, owner Owner) ([]AgEpoch, error)
    MigrateRegion(ctx context.Context, region RegionID, gen LeaseGen, to Owner, m AgManifest) ([]AgEpoch, error) } // 04 §6.7
type PrivacyParticipant interface { Export(context.Context, Subject) (Section, error)
    Erase(context.Context, Subject) (Report, error) }                           // §6.6
type WorldScriptStore interface {                                               // §1.23; internal, not a studio API
    Commit(ctx context.Context, p PartitionID, gen LeaseGen, batch []Invocation) ([]Reply, error)
    Get(ctx context.Context, t TableID, pk []byte) (Row, error)
    Query(ctx context.Context, t TableID, idx IndexID, from, to []byte, limit int, cur Cursor) ([]Row, Cursor, error) }
```

**Testing.**
- **Unit/property:** domain packages are pure and take an injected clock. `rapid` property tests check:
  - conservation under random op sequences;
  - that the order book never crosses;
  - that fills never exceed an order's quantity under interleaved owners;
  - that no order insert, modify, cancel or match commits at a stale `owner_gen`;
  - that ID blocks never overlap;
  - that a world-script `Commit` at a stale `lease_gen` never applies, a replayed invocation ID never
    applies twice, and every `@escrowBacked` table's escrow balance equals its open rows' sum after random
    RPC, event, timer and takeover sequences;
  - that concurrent roster edits never exceed a fleet's caps and a stale `expect_ver` never commits;
  - that the silence classifier (§1.4.3) never returns "domain loss" for a silent set spanning more than
    one blast-radius domain, and never confirms a process whose trunk or probe still answers.

  `go test -fuzz` covers the token, manifest and CDC codecs.
- **Integration:** `pkg/testkit` runs embedded-postgres, NATS and miniredis in-process. It needs no Docker
  and runs on Windows. Each test clones a migrated template database (~150 ms). A relocation test runs two
  dev gateways and 16 bots through a `DrainGateway`, on Windows and Linux. Linux CI adds
  `testcontainers-go` for real Valkey, a 3-node NATS cluster and PG failover. Integration runs connect with
  the generated per-role (from Phase 3, per-process) NATS credentials, and any permission violation fails the
  run. A forged-caller test sends a fenced request under another process's cell ID and expects
  `CALLER_MISMATCH` (§6.5).
- **Contract:** golden vectors (tokens, manifests, Helios-binary messages, block IDs) are checked by both
  `go test` and doctest.
- **Chaos:**
  - `kill -9` mid-saga, NATS node loss, PG switchover, duplicated or reordered messages, zombie-cell
    writes, ±2 s clock skew;
  - the control-plane suite (kill the JetStream meta and stream leaders, kill the orchestrator leader,
    partition the leader from NATS);
  - market split-brain (SIGSTOP the owner past a takeover, then SIGCONT, with orders placed, modified and
    cancelled throughout);
  - failure domains: power off a SERVER host, partition a rack at its ToR, block only NATS on one host
    (A15, 04 NS-4.1);
  - live content: server-part hotfix swaps and N/N+1 rolling restarts with a staged client build (A14);
  - gateway rolls: `DrainGateway` under load with a box kill mid-roll, and relocations aborted by blocking UDP
    to the target box (A14 c);
  - host-patch rolls through `helios-nodemaint`, with an unadmitted `kubectl drain` and a host power-off
    mid-roll (A14 d);
  - world scripts: `kill -9` and SIGSTOP/SIGCONT of the WSH past a takeover, duplicated events and RPC
    retries, hot swaps and `migrate` runs under load (A20);
  - an availability-zone loss including its gateways, Session replicas and the Valkey primary (A19);
  - ID-block bursts;
  - the privacy canary (export, erase, then scan every store for the canary's markers);
  - DR drills.
- **Load:** `helios-loadgen` covers login storms, the ledger mix beside the design fence mix
  (`helios-loadgen fence`, A5), the whole §3.6 shard-primary write mix (`helios-loadgen mix`, A5 Ph4), the
  standby's replay capacity (`helios-loadgen replay`, §3.6), a hub market, checkpoint floods and chat.
  The C++ bot swarm runs at **3–5× target CCU** (R05-P1-20). Runs are nightly with a ±10 % regression gate.

---

## 9. MVP → AAA feature ladder

| Area | Phase 0 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 |
|---|---|---|---|---|---|---|
| Runtime | all-in-one exe, Win+Linux CI | compose; combined binary in prod | Sentry crash ingest; generated NATS permission matrix and CI check (§6.5) | K8s + Agones; gateways over 3 zones (6 boxes); per-process NATS credentials and caller checks (§6.5); node-maintenance controller, host batches (§6.1a) | multi-region DR, zone-loss gateway sizing (12 boxes) and drill (§6.7), gateway rolls by make-before-break relocation (§6.3.1), rack-batch host-patch rolls (A14 d) | global tier (CockroachDB eval) |
| Identity/session | password, JWT, tokens + vectors | launch codes, reconnect tickets | queue lanes, 50/s admission, ToS acceptance, age gate | full queue | OIDC, MFA, 200/s | 500/s |
| Orchestrator | local supervisor, 1 cell, PG leadership, block IDs | PG generations, fence, crash restore | warm standby, two-signal detection, failure-domain registration and classifier (host blast radius), degraded mode, multi-zone | multi-cell, zone leader, instances, pre-provision, control-plane and host-loss chaos (A15), domain-sized warm pool, spread constraints | rolling restarts, 3 replicas, rack blast radius (NS-4.1), ID scale (A16), rack-disjoint, zone-local replicant placement (§1.4.6) | split/merge |
| Ledger | schema, reason codes | wallets, grants, custody, idem | items, trades, escrow, audits, 2k tx/s, partition retention | rollback tools | 10k tx/s inside the full §3.6 write mix (A5), trust rules, Parquet archive reads | partitioned 50k tx/s |
| Persistence | — | checkpoints ≤ 30 s, barrier | lifecycle cleanup, blob budgets, `PERSIST` partitions | sampled history + restore; leaderboards and activity snapshots (§1.12) | 30k AG/s beside them (A9); replicant tier (04 §6.4) cuts world-zone loss to ≤ 1 s inside the blast radius | ≤ 10 s window elsewhere |
| Economy | — | — | market (fenced partitions), industry, timers, resources, mail | contracts, territory timers | public market API, economy-sim hooks | economy sim (R04-P2-20) |
| World state | — | — | flags, meta-events | influence, territory | sovereignty | cross-shard events |
| Social | — | — | chat, friends, presence, corps | alliances, parties, **fleets** (§1.12.1), matchmaking, activity; trust features, red-team corpus v1, anti-cheat vendor decision (§1.16a) | moderation tools; trust detectors, ban waves, vendor integration (§1.16a, A21) | cross-shard |
| World scripts (§1.23) | — | — | — | WSH, declared tables, timers, escrow-only ledger intents, events, cell RPC; API `1.0`; bounty-board proof (A20) | partitions over ≥ 2 WSHs, public read endpoints | Go service modules revisited with the UGC API |
| Content | local build pointer | pins, hot reload | live/ptr/dev channels, client/server parts and compat epochs (§1.14.1), collab locks/presence | live-edit changesets, overlay versions | two-person promote, staged server-part hotfixes, coordinated epoch flip (A14 Ph4) | UGC publish (R03-P2-1) |
| Privacy | PII only in Identity, DEKs, `@pii` lint | — | retention jobs, log redaction | DSAR export and erasure sagas | 72 h SLA (A18), residency sign-off | per-residency identity |
| Operations | — | runbooks for Ph1 alerts | business-hours on-call, error budgets | 24/7 on-call, status page, postmortems | follow-the-sun, DR drills (A19) | — |
| Patching | chunker, local CDN | dev channel, launcher install | signed prod CDN, repair | tiers, streaming, PTR | multi-CDN, patch-from, staged rollout | — |

---

## 10. Acceptance criteria

| # | Criterion | Phase |
|---|---|---|
| A1 | `helios-backend.exe run` on Windows 11: warm start ≤ 5 s, first run ≤ 30 s (PG binaries cached), idle RSS incl. Postgres ≤ 500 MB | 0 |
| A2 | Token golden vectors pass in Go and C++; issue p99 < 100 ms at 100/s | 0 |
| A3a | Killing a cell: the orchestrator restarts it from checkpoints and players reconnect; non-value loss ≤ 60 s, **0** item deltas in the audit | 1 |
| A3b | Killing a cell: a warm standby serves in ≤ 10 s, non-value loss ≤ 30 s, **0** item deltas (no-reconnect ≤ 10 s hitch is AAA-SRV-12, Ph3) | 2 |
| A4 | Dev ledger (embedded PG, laptop) 500 tx/s at p99 < 20 ms; `Fence.Advance` p99 < 5 ms | 1 |
| A5 | PG ledger 2k tx/s for 1 h at p99 < 50 ms, beside the Phase 2 design fence mix (250 operations/s, §3.6) with `Fence.Advance` p99 < 5 ms. **Phase 4: 10k tx/s for 1 h at p99 < 25 ms, beside the Phase 4 design fence mix** (§1.13, §3.6). That mix is ≈ 2.9k fence operations/s and ≈ 8.1k rows/s: 200 loads, 200 parks, 1,000 zone transitions, 1,000 handoffs and 500 `Join`/`Leave` per second, `MigrateRegion` calls on 750-row regions totalling 2k rows/s, and one 5k-row `AdvanceOwnedBy` per minute. It includes a **60-member carrier tree** that hands off across a boundary every 2 s and launches or recovers a fighter (`Join`/`Leave`) every second, while its members issue 200 ledger tx/s. `helios-loadgen fence` drives the mix with synthetic cells that speak the fence and ledger RPCs, with 04 §6.3's handoff timing and the fence gate; the carrier flies in a real 2-cell zone with bot crews. Pass: `Fence.Advance`, `Join`/`Leave` and `Park` p99 < 5 ms, including the carrier's 61-row statements; ledger p99 < 25 ms, overall and for the carrier's members; the carrier's **handoff p99 < 100 ms** (offer to first authoritative tick); `AdvanceOwnedBy` ≤ 1 s p99; **0** deadlocks; `FENCE_BUSY` ≤ 0.01 % of fence operations and `FENCE_STALE` ≤ 0.1 % of ledger transactions; synchronous-standby replay lag p99 ≤ 1 s; `mxid_age` of `authority_fence` below 40 % of `autovacuum_multixact_freeze_max_age`; a zero conservation audit. **The Phase 4 run carries the whole §3.6 shard-primary write mix at once:** `helios-loadgen mix` adds every other writer at its design rate: world scripts 5k row writes/s, market 2k order operations/s, timers 1.5k/s over 1 M pending, account progression 2k flushes/s (including 32 KiB worst-case accounts, with folds running), character 1k/s, mail 200/s, world flags 500/s, groups 1.2k rows/s and industry 300 rows/s. A9's persistence-cluster writers run at the same time. Pass adds: each writer's WAL within ±20 % of its §3.6 row; total WAL (34.5 MB/s) and write IOPS (≈ 7.5k) within ±20 %; the design mix ≤ 2/3 of the replay capacity that `helios-loadgen replay` measured on the same standby; each writer's own SLO held (`ApplyAccountProgression` p99 ≤ 20 ms, `PlaceOrder` p99 < 150 ms, timer lateness p99 < 1 s, `worldscript.Commit` lag p99 < 1 s) | 2 / 4 |
| A6 | 72 h chaos soak: **0** invariant violations, **0** stuck sagas. **Bounded storage:** with retention horizons compressed via `--retention-scale` (idempotency 6 h, outbox 3 h, inbox 12 h, hist 24 h), every partition-drop job runs ≥ 3 times, each of those tables stays within ±10 % over the last 24 h, and total PG growth matches §3.6 within ±20 % | 2 |
| A7 | Hub market 2k orders/s into one actor; `PlaceOrder` p99 < 150 ms; settlement p99 < 2 s. **Split-brain chaos:** in 1,000 runs, an old owner paused past a partition takeover and resumed mid-match gives **0** over-fills, **0** settlement failures and a zero conservation audit. Bots keep placing, modifying and cancelling orders on the partition before, during and after the takeover: every open order in PG is in the new owner's book ≤ 1 s after the takeover (book-vs-PG diff = 0), **0** order inserts, modifies or cancels commit at a stale `owner_gen`, and **0** orphan escrows remain after the reconciler horizon | 2 |
| A8 | Admission 50/s (Phase 2), 200/s (Phase 4), 500/s (Phase 5) per shard; unqueued login p95 < 3 s | 2–5 |
| A9 | Checkpoints 30k AG/s per persistence cluster at the §1.13 blob budget, beside leaderboard upserts at 10k/s and activity snapshots at 500/s (§1.12); durability lag p99 < 5 s; leaderboard entries visible ≤ 1 s after their result p99; WAL (≈ 80 MB/s) and disk rates within §3.6 ±20 % | 4 |
| A10 | Timers: 1 M pending, lateness p99 < 1 s, 0 double effects | 2 |
| A11 | Chat 5k msg/s at p99 < 250 ms | 3 |
| A12 | Patch download ≤ 1.5× the changed bytes; verifying a 50 GB install ≤ 5 min (AAA-CNT-7; 08 CL-9, CL-10) | 2 |
| A13 | PG failover: recovery < 30 s, 0 lost committed ledger txs; monthly PITR drill ≤ 1 h | 3 |
| A14 | Content hot reload ≤ 2 s from publish to applied (dev). **Phase 4, live compatibility (§1.14.1; measures AAA-ITR-8 and AAA-STB-6):** under 50k bots, with a same-epoch client build staged at `rollout_pct` 5 %, run (a) a server-part hotfix from canary to 100 % of instances in ≤ 15 min, each swap ≤ 2 s at a tick boundary, then its rollback, and (b) a rolling N/N+1 restart of every cell by planned region migration (04 §6.7), with each zone's **hitch p99 ≤ 1 s** and **0 state loss** (every migration's state hash equal on both sides, 0 lost transients). In both (a) and (b): **0** clients disconnected or re-placed, **0** zones duplicated by version (one instance set per zone and layer throughout) and **0** value errors. **(c) Gateway roll (§6.3.1):** under the same 50k bots, `DrainGateway` rolls all 12 gateway boxes to a new gateway build, two zones in parallel. A host-patch roll then reboots one box per zone, and during the binary roll one more box is `kill -9`ed (NS-4.7's procedure). Pass, for every relocated session: **0** linkdead episodes (no netcode timeout, no ticket redemption, `gw_relocate_fallback_total` = 0); input gap at the home cell p99 ≤ 250 ms and max ≤ 1 s; STATE gap at the client p99 ≤ 250 ms; **0** creates or destroys caused by a relocation (handle audit and boundary observers, 04 §10.3); NS-4.7's field-state audit clean on 1,000 sampled field-auditor bots 2 s after `RelocateDone`; relocation p99 ≤ 2 s. For the roll: each box empty ≤ 20 s after its drain starts, and the binary roll done in ≤ 15 min; the box and zone rules (§6.7) hold at every 10 s check, with **0** "full" denials; the killed box's sessions still pass NS-4.7; **0** value errors. **(d) Host-patch roll (§6.1a):** under the same 50k bots, `helios-nodemaint` takes every SERVER node (≈ 30 hosts) through admission, surge, cordon, `Drain`, reboot and rejoin, in rack batches where surge capacity allows. During the roll, one `kubectl drain` of an unadmitted node is attempted, and one host is powered off mid-drain. Pass: each region moves once, with a **hitch p99 ≤ 1 s per region moved** and **0** state loss (state hash equal on both sides, 0 lost transients); **0** clients disconnected; every world-zone cell keeps a live replicant throughout; `WarmPoolBelowDomain` never opens because of maintenance; no batch spans two racks and none overlaps another; the unadmitted `kubectl drain` evicts nothing; the powered-off host is recovered within A15's bounds and pauses the roll until `FailureDomainLost` closes; **0** value errors; the roll finishes in ≤ 8 h. CI and `Promote` refuse a build whose fingerprint changed without an epoch bump, and the manifest service refuses `rollout_pct` < 100 for another epoch. A PTR epoch-flip rehearsal puts every zone on E+1 in ≤ 10 min, with players back in ≤ 3 min without queueing | 1 / 4 |
| A15 | **Control-plane chaos** under 5k-CCU bot load (AAA-STB-5). Each is run 10×: kill the NATS node leading the JetStream meta group and the `PERSIST`/`DIRECTORY`/`EVT` streams; kill the orchestrator leader; do a PG switchover. Result: **0** self-stopped cells, **0** reassignments, **0** value errors (only "pending" retries), no client hitch > 2 s. A real `kill -9` of a cell is still recovered ≤ 10 s in normal mode, and ≤ 10 s after exit when it happens during degraded mode. **Failure domains** (§1.4.3), each run 10×: power off a SERVER host carrying ≥ 8 cells (a hard stop, no clean shutdown), and block only NATS (port 4222) on one host. Pass: the host's cells are confirmed ≤ 3.5 s after the fault (≤ 5 s for cells without players), and every affected region is served by a standby ≤ 10 s after confirmation. **No** degraded episode lasts > 5 s (none is expected), login admission **never** pauses, ≥ 99 % of affected sessions are kept, unaffected cells hitch ≤ 2 s, and there are **0** value errors. The NATS-blocked host's cells are not reassigned in the first 60 s and keep serving over their trunks. Phase 4 repeats this under 50k bots, adding a SERVER-rack partition at its ToR, as part of 04 NS-4.1 | 3 (Ph4: NS-4.1) |
| A16 | **IDs:** 600 concurrent minting processes per shard for 1 h, plus a 2,000-ship volley burst (250k IDs in 1 s from one cell) and 1 M IDs/s shard-wide: **0** duplicates over all minted IDs, **0** minter waits for a block, `AllocateIdBlocks` p99 < 20 ms; killing the orchestrator leader mid-burst changes nothing | 4 |
| A17 | **World state:** a flag set by one cell is applied by every cell watching its scope, p99 ≤ 1 s; a 3-zone meta-event advances each phase exactly once under duplicated reports and `kill -9` of a zone cell; after a `WORLD` KV loss, it is rebuilt from PG in ≤ 30 s with 0 lost flags; a superseded cell's `SetFlag` is rejected | 2 |
| A18 | **Privacy:** 100 DSAR exports and 100 erasures on a loaded shard each complete in ≤ 72 h. Afterwards: the conservation audits pass; the audit hash chain verifies end to end; a canary account's unique markers appear in plaintext **0** times across PG, ClickHouse, Loki, JetStream, object storage and a restored backup (after shred-log replay); the handle is released after 30 days; login refuses an account until the current ToS version is accepted | 4 (tooling 3) |
| A19 | **DR drill:** failing the global home region → global services serve from the DR region with RTO ≤ 1 h and RPO ≤ 60 s, while live shards keep playing with **0** committed ledger loss; a shard availability-zone loss → PG RPO 0 and recovery < 30 s, then the lost zone's cells confirmed through a wide-loss hold (§1.4.3) and every populated region served again ≤ 5 min after exit, in descending CCU order. **State loss (§1.4.6, §6.7):** for the lost zone's world-zone AGs, measured against the truth tap (04 §10.3), non-value rollback is ≤ 30 s p99 and ≤ 35 s max (the checkpoint window plus flush lag) with **0** value loss; AGs whose replicant was in a surviving zone roll back ≤ 1 s. **Gateways (§6.7):** at 50k CCU the zone loss takes that zone's gateways (≈ 17k sessions), Session replicas and the Valkey primary with it; ≥ 99 % of sessions whose gateway was lost and whose cell survived are back in the world ≤ 20 s, and 100 % ≤ 60 s; ≥ 99 % of all the lost zone's sessions, gateway or cell, are back ≤ 5 min; the Session service redeems ≥ 20k tickets in ≤ 8 s at p99 < 250 ms per call; **0** "full" denials and **0** reconnects held in the reconnect lane; login admission resumes ≤ 10 s after the wide-loss hold ends; a gateway-box kill during the drill (NS-4.7's procedure) still passes NS-4.7. Restoring a shard in another region from backups and WAL → RTO ≤ 1 h, RPO ≤ 60 s, conservation audit passing before opening; planned evacuation ≤ 30 min downtime, 0 value loss; every alert has a runbook exercised in the last quarter | 4 |
| A20 | **World scripts (§1.23)** on the `starter-sandbox` bounty board (09 §2.7.4), under 5k-CCU bots across ≥ 3 zones on ≥ 4 cells: bots post 20k bounties at terminals in every zone, and kills in other zones claim them. Pass: every bounty is paid exactly once or refunded once at expiry; the conservation audit finds 0 deltas and every hourly escrow-backing audit matches; with 50 `kill -9`s of the WSH, 20 restarts of the `worldscript` service, duplicated killmail events and RPC retries, and a JetStream stream-leader kill, there are **0** duplicate effects and every partition serves again ≤ 10 s after confirmation; a WSH paused past a takeover and resumed commits **0** writes; `Post` p99 ≤ 50 ms and `List` p99 ≤ 20 ms at 2k invocations/s per partition; a runaway handler is killed at its fuel limit with nothing committed and no other partition slowed > 5 %; a hot swap under load pauses each partition ≤ 1 s and loses 0 invocations; a v1 → v2 table change with a `migrate` handler passes `upgrade-test` and migrates 1 M rows online in ≤ 10 min with RPCs served throughout; the template differs from the SDK in **no** engine or backend source file | 3 |
| A21 | **Trust detection (§1.16a)** under NS-4.1's 50k-bot load. The red-team corpus runs live: snap aimbot, silent aim, triggerbot, ESP pre-aim, input macros and farm bots (mining, ratting and mission runners, gatherers), each at humanization levels 0–3, with ≥ 200 sessions per variant. Pass: **≥ 90 %** of the aimbot, triggerbot and silent-aim sessions and ≥ 90 % of the farm-bot sessions are flagged within 24 h (each family scored separately), and 100 % of level-0 variants within 15 min; false positives **≤ 0.1 %** on each honest population (the swarm in `--human-model` mode for the aim and input detectors, and ≥ 5,000 reviewed human sessions for every detector); feature extraction ≤ 1 % of each cell's tick budget; every flag opens a `trust_rule` case with its evidence linked; a staged ban wave's dry run matches its execution (accounts, sanctions, remediated value), remediation leaves a zero conservation audit, and no sanction executes before its wave | 4 |

---

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Checkpoint write volume | Separate cluster, COPY batching, hash partitions, blob budgets, `PERSIST` partitions, `CheckpointStore` seam + Scylla/FDB benchmark at §3.6 rates |
| A control-plane outage freezes or fences the shard | Fences give safety and heartbeats only liveness; PG-anchored leadership and generations; two-signal detection; degraded mode; A15 |
| A host or rack loss is mistaken for a control-plane fault and freezes the shard | Failure-domain-aware classifier (H+T or H+R, no wait on P); exit ratio over processes not confirmed dead; control plane on its own node pool; domain-sized, spread warm pool; replicants rack-disjoint; A15 host clause, NS-4.1 rack clause |
| A mass false confirmation (for example a gateway bug) triggers a reassignment storm | T requires trunk-healthy gateways outside the domain; a set wider than the blast radius holds 5 s; fences make a wrong confirmation cost a hitch, never value; poison-build brake |
| ID exhaustion or collisions | PG-allocated time-prefixed blocks with no node IDs; only minters hold blocks; A16 |
| Storage growth outruns the primary | §3.6 model rerun at each gate; partition-drop retention; 90-day online ledger; A6 growth assertion |
| Market split-brain over-fills, or orders stranded outside the owner's book | `owner_gen` fence on every order insert, modify, cancel and match; only the owning actor writes; conditional `qty_left` and cancel; nightly fill audit; A7 chaos with order traffic during takeovers |
| Live content splits the world by version, or a hotfix forces a client patch | Client part, server part and compat epoch (§1.14.1); tokens pin the epoch; staging only within an epoch; epoch changes as coordinated flips; A14 (Ph4) |
| Privacy law vs append-only data | PII only in Identity; crypto-shredding; pseudonymous ledger and audit; `@pii` lint; A18; counsel sign-off (09 K32) |
| Region loss | Cross-region WAL and backups, DR replica cluster, evacuation runbook, A19 drills |
| A gateway deploy or host patch drops up to 8k sessions into linkdead | Make-before-break relocation (§6.3.1): the client connects to the new gateway before the old path closes, and contributors keep their baselines. The zone's warm spare opens first, so the headroom rules hold. Relocations are paced at ≤ 500/s per box, with abort-back on failure; A14 (c) |
| Fence writes and ledger share locks contend on the shard primary (busy trees, handoff storms) | Fence operations and rows budgeted in §3.6 and counted in WAL and IOPS; fence gate, fence check last, one lock order, lock timeouts, a resident and tuned `authority_fence` (§1.13); A5 (Ph4) with a 60-member carrier |
| An availability-zone loss strands ≈ 17k sessions with no gateway slots | Gateways sized so two zones hold every session and still meet the box rule (12 boxes at 50k CCU); admission capped by the zone rule; a reconnect lane ahead of logins as a backstop; Session and Valkey spread over zones; A19 gateway clause (§6.7) |
| A studio world script corrupts value, stalls or leaks load into the shard | Escrow-only value in, capped faucets, outbox with idempotency keys, hourly escrow-backing audit; per-invocation fuel, heap and op caps; per-partition rate limits; `ws_dead`; per-script kill switch; lease-fenced commits; A20 (§1.23) |
| Fleet mechanics exist only for bots, so EVE-class play is untested | Player fleets as a group kind (§1.12.1) with 06 §8.3a's warp, broadcasts and bursts; the Fleet panel (08 §1.7.2); GP-15; NS-4.2's bots fight as player fleets |
| Aimbots, triggerbots, input automation or farm bots play inside the rules that authority enforces | Cell-side aim, input and routine features from existing data; a labelled red-team corpus with a nightly regression gate; streaming rules and daily models; delayed ban waves with two-person approval and ledger remediation; the client vendor chosen in Phase 3 for Linux support but never depended on (§1.16a); A21 |
| The shard-primary write mix outgrows the synchronous standby's single-process replay | Every writer budgeted (§3.6 write mix); a replay budget of ≤ 2/3 of measured capacity; account-progression deltas; leaderboards and activity snapshots on the persistence cluster; ordered next moves (world-script cluster, Phase 5 ledger split); A5 (Ph4) runs the whole mix |
| A compromised process impersonates another on NATS (forged `Helios-Fence` cell, foreign subjects) | Generated per-role permission matrix with a CI check; per-process credentials; server-stamped `Nats-Request-Info` checked against the fence header and `owner_cell`; revocation on confirmed death (§6.5) |
| Monthly host patching stalls on allocated GameServers or takes the crash path | `helios-nodemaint`: cordon → `Drain`, `eviction.safe: Never`, surge-first batches bounded by the blast radius, pause on `WarmPoolBelowDomain` (§6.1a); A14 (d) |
| embedded-postgres offline/first run (R10 §14) | Cached binaries, `pg-install --from`, `--db` to local PG, Windows cold-start CI |
| Ledger becomes a single point of failure | Sync replica, idempotent retries, "pending" UX, per-feature kill switches |
| Hot rows (escrow, corp wallets) | Unmaterialized system accounts; escrow wallets sharded per region |
| Connect and NATS bindings drift apart | One `.hschema` source; conformance suite runs every RPC over both |
| JetStream limits | Per-stream quotas sized by §3.6, R3 sized by load tests, Redpanda for telemetry |
| Egress cost | Rerun the §6.4 model at each phase gate; colo decision by Phase 3 |
| Signing-key compromise | Offline root, rotating subkeys, pinned keyset, anti-rollback |

---

## 12. Traceability

| Requirement | Section |
|---|---|
| R07-P0-1/2/3, R03-P0-8 | §1.1–1.3 |
| R07-P0-4, R02-P2-19, R01-P1-14, R01-P2-21 | §1.4, §6.1 |
| R07 §8.4 (failure handling) | §1.4.3–1.4.4, §1.4.6 (failure domains, warm pool), §6.7, §6.8 runbooks |
| R07-P0-7, R05-P0-8, R09-P0-7/8, R02-P0-7 | §1.6, §1.13, §4 |
| R07-P0-8, R04-P0-8 | §1.13, §3.6 |
| R07-P0-9, R02-P2-16 | §1.10–1.11 |
| R07-P0-10, R01-P2-22, R05-P1-20 | §6.2, §8 |
| R07-P0-11, R01-P0-9, R03-P1-6, R05-P2-25 | §7 |
| R07-P0-12, R08-T27 | §1.17 |
| R07-P1-15, R02-P1-9/11, R01-P1-13 | §1.7–1.9 |
| R05-P0-5/6, R03-P1-8 | §1.12 |
| R01 fleet warp-ins and fleet boosts; M01 (EVE fleet battles) | §1.12.1 (fleets), 06 §8.3a |
| 01 §1.1 (no backend source edits), AAA-TOOL-9 | §1.23 (world scripts), A20 |
| R09-A9, R04 (BGS), R04-P2-20, 06 §6/§9 | §1.21–1.22 |
| R07-P1-17/18, R03-P1-11, R05-P1-16 | §1.14.1, §1.15, §1.20, §6.3, §6.3.1 (gateway relocation) |
| R07-P1-19, R01-P0-8; R05 (server-side cheat detection, Bungie's kernel anti-cheat) | §1.16, §1.16a (trust detection), A21 |
| R07-P1-20, R07-P2-21/23/26 | §6.1a (node maintenance), §6.5 (NATS permission matrix, per-process credentials), §6.7 (including zone-loss gateway sizing), §7, §3.3, §3.6 (write mix), §9 |
| R01-P1-11/12/17, R05-P2-23 | §2.1, §3, §1.18 |
| R03-P0-3/4/5, R04-P0-9, R05-P0-7 | §1.14, §1.14.1 (compat epochs), §2.1 |
| Data protection (GDPR, CCPA, COPPA) | §1.1, §6.6 |

---

## 13. Cross-section dependencies

| Section | Depends on / must deliver |
|---|---|
| **04 Networking** | Token format + vectors, service lane, fence ops over AG trees (`Advance`, `Join/Leave`, `AdvanceMany`, `AdvanceOwnedBy`, `MigrateRegion`), planned region migration with `Drain` and `PreProvision` (04 §6.7), fence cache, checkpoint cadence, `item_refs` reconcile, zone-leader and handle-block tables, N↔N+1 mesh, `AssignReplicant` placement (Phase 4). Detection: H plus one of T, P or R, classified by failure domain (§1.4.3). Gateways report `trunk_health` with `ReportSuspect`, run `ProbeCell` over a trunk or a probe connection, and relay `Fenced` over a trunk. The NS-4.1 rack and host clause is shared with A15. Placement matches the compat epoch, not the client build, and open-world zones roll in place (§1.14.1, 04 §7). No cell self-fences on lost renewals; it stops on a higher generation or a rejected write. Gateways and neighbour cells drop trunk messages below the current `(region, lease_gen)`. Handoffs and zone-leader changes are deferred in degraded mode (§1.4.4). Gateways are sized for a zone loss, not only a box loss (§6.7; 04 §2.6 defers to it). The voice forwarder reads fleet rosters from KV `GROUP` (§1.12.1). NS-4.2's bots fight as player fleets (06 §8.3a). Gateway relocation (Phase 4): `Relocate`, `ClientRebind{mode = relocate}` with the contributor re-key that keeps baselines, `RebindFence` and `PathClosed` (04 §2.4, §6.6; §6.3.1). The cell's fence gate holds new ledger requests for a tree while a fence operation on it is in flight (04 §6.1; §1.13). Round 5: cells compute `TrustFeatures` from the input stream and the lag-compensation resolve (04 §5.2, §5.6) within ≤ 1 % of the tick, and flush the tick-recording ring on a trust pre-filter (§1.16a); `helios-bot` gains the red-team cheat and farm-bot variants and `--human-model` (04 §10.3). Cells subscribe to `ctl.<shard>.cell.all.>` and connect with per-process NATS credentials under the generated matrix (§6.5). Replicants sit in their cells' availability zone, so an AZ loss restores those cells from checkpoints (§1.4.6, §6.7). |
| **02 Engine** | Time-prefixed block IDs 41/5/17 and the cell `EntityRegistry` minter (§1.4.5). `.hschema` must support service blocks, `ledger_policy`, lifecycle rules, `ReasonCodeDef`, `@pii` column classes and deterministic-key guards. schemac must emit `.proto`, Helios-binary codecs and the Go NATS binding. The cook splits each build into a client part and a server part and computes the compat fingerprint (§1.14.1). A ZoneInstance pins its compat epoch; its server part hot-swaps at a tick boundary. The cell binary runs as a world-script host (`--role world-script`, §1.23) with the 02 §7.4 sandbox and no zone simulation; schemac accepts `worldscript` blocks and emits the `world` realm's `.d.luau`. schemac also emits the NATS permission matrix (`nats-perms.json`) from service, caller and subject declarations (§6.5). |
| **06 Gameplay** | Keep the anchors §1.5, §1.6 and §1.8. Use "record template", not "archetype". World state (§1.21) owns `WorldFlagDef` storage, meta-events, influence, territory structures and sovereignty; the Industry service owns structure upkeep (§1.8), and the ledger owns plots (§1.6). The Economy Sim (§1.22) writes `RegionEconomyState`. Crafting refunds and crew missions use §1.8 timers; Go HXL is `pkg/hxl` and follows 06 §1.2's float rules (§8). `CraftStamp` stores only `crafterId` (§6.6). Account progression and follower progression live in §1.5; `mirrorAsEntitlement` grants go through §1.20; `WeatherOverride` flags live in §1.21. Activities, lockouts, PvP results, queues, the group finder and leaderboards (06 §6.6–6.13) are served by §1.12, and lockouts use the ledger's `ClaimGuard` (§1.6). Fleets: §1.12.1 owns the roster and 06 §8.3a the mechanics (`FleetDef`, `FleetMembership`, fleet warp, broadcasts, command bursts, killmail `fleetId`), with GP-15; `GroupListing` gains `kind: Fleet`. World scripts (§1.23) are called from server Luau with `World.call` and `World.emit` (06 §11). |
| **08 Launcher** | Launch codes, SSE queue, BLAKE2b chunks, tiers, `rollout_pct`, patch-from, anti-rollback, install journal, legal-acceptance screen and age gate (§1.1, §6.6), status banners from the incident feed (§6.8). Pointer `compat_epoch` fields; on `CONTENT_EPOCH_FLIP` the client applies the pre-downloaded build and rejoins with its reconnect ticket (§1.14.1, 08 §2.6); `CreateSession` refuses only another compat epoch or a build below `min_client` (08 §1.1 step 7). The Fleet panel (`FleetVM`) with its CL-23 flow in Ph3; the "Reconnecting" overlay shows the reconnect lane's ETA (§6.7). The invisible Relocating state (Phase 4) keeps two connections open, sends input on both and holds EVENT_R until `PathClosed` (08 §1.2; §6.3.1). The `IAntiCheatProvider` vendor is chosen in Phase 3 by WP-3.11 and integrated in Phase 4; its signals are one Trust feature family (§1.16a, 08 §1.14) |
| **07 / 09** | Collab service and changeset export to git (07 §1.8). T27 hotfix changesets are classified by CI as server hotfix, compatible client build or refused (§1.14.1). A1–A21 (A3 split into A3a/A3b) as phase exits: A17 in Ph2, A15 and A20 in Ph3, A16/A18/A19/A21 and A14 (Ph4) in Ph4. A21 and the trust detectors belong to WP-4.8; WP-3.11 delivers the cell features, corpus v1 and the anti-cheat vendor decision (§1.16a). WP-3.3 builds the node-maintenance controller and per-process NATS credentials (§6.1a, §6.5), and WP-4.4 the rack-batch rolls (A14 d) and the full write mix (A5 Ph4, §3.6). The perf environment and bot swarm arrive by Phase 2 (09). Privacy and residency counsel sign-off is 09 risk K32. A20 in Ph3 (WP-3.12), with the world-script API `1.0` in the public API list, its generated reference and tutorial, and the bounty board in `starter-sandbox` and `upgrade-test` (09 §2.7). 09 §4.3.2's soak model uses the domain-sized warm pool and 12 gateway boxes. WP-4.3 builds gateway relocation, and WP-4.4 builds `DrainGateway` and the fence budget; both are accepted by A5 and A14 (Ph4). |
