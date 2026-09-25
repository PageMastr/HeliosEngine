# 05 — Backend Services, Data, Persistence, Ops, Patching/CDN

Status: draft v1. Conforms to ADR-002, -004, -006, -007, -008, -010, -012, -013 and **-014**.

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

---

## 1. Service catalogue
Each service is a package `internal/<svc>` that implements a generated connect-go interface. It owns one
Postgres schema and DB role, and exposes `/healthz`, metrics and traces. *Global* services span shards;
*shard* services are regional.

### 1.1 Identity/Auth — global — Phase 0 (OIDC federation, MFA: Phase 4)
**Owns:** accounts, credentials, refresh-token families, bans, MFA factors, OAuth clients and the global
handle+discriminator registry (R03-P0-8).

**Password hashing:** argon2id with m=64 MiB, t=3, p=1 (~80 ms per core), behind a 2×GOMAXPROCS semaphore
so login storms queue instead of running out of memory.

**API:**
- `Login` returns an EdDSA JWT (10 min) and a rotating refresh token (30 d, stored hashed). Reusing a
  refresh token revokes its whole family.
- `CreateLaunchCode` and `ExchangeLaunchCode` hand the client a one-time 60 s code, so refresh tokens never
  appear on a command line.
- Also `Register`, `Logout`, JWKS and `Ban`.
- Bot-scoped credentials exist for load tests and are accepted only by test shards (08 §1.13).
- Phase 4 adds OIDC, PKCE, Steam/Epic/Google federation and TOTP.

**Failure:** services verify JWTs against cached JWKS, so an outage blocks only new logins.

### 1.2 Login queue — shard — Phase 1 (cap), Phase 2 (lanes, 50/s admission), Phase 3 (full)
**State:** Valkey ZSET `q:<shard>`, scored `lane_offset + enqueue_ms`.

**API:**
- `Enqueue` returns an Ed25519-signed ticket `{account, shard, lane, enqueued_at, exp}`.
- `WatchStatus` streams position and ETA over SSE.

**Admission:** rate = min(orchestrator headroom, DB login budget), applied before the character DB is
touched (R07 §4).

**Lanes:** reconnect-grace (5 min) bypasses the queue. The rest run in order: GM/dev, priority, standard.

**Recovery:** after a Valkey loss, clients re-present their tickets and the queue is rebuilt in order.

### 1.3 Session & connect-token issuer — shard — Phase 0
**State:** Valkey `sess:<id>` holds account, character, gateway instance, content build and
`session_epoch` (24 h sliding TTL).

**Connect tokens:** `CreateSession(grant, character)` mints a netcode-format token in Go (04 §2.3).
- Encryption is `x/crypto` XChaCha20-Poly1305, with a CI interop test against vendored netcode.
- Expiry ≤ 45 s; timeout 10 s.
- The token lists 2–4 gateway *instance* addresses, chosen by the free slots each instance reports.
- The private part is sealed under the shard key. `client_id` = session ID, and 256 B of user data carry
  the character, content-version pin, placement ticket, entitlements and attestation hash.

**Key rotation:** netcode has one private key per server instance, so daily rotation mints only for
instances already on the new key and lets old-key instances drain (tokens live ≤ 45 s). A small two-key
patch is the fallback.

**Reconnect (04 §2.4):**
1. Every 60 s, gateways batch-call `SealReconnectTickets`. Tickets are valid for 5 min and hold session,
   character, zone and `session_epoch`.
2. The client redeems a ticket with `Reconnect(ticket)` over HTTPS.
3. Session CAS-increments `session_epoch`, which makes the old gateway drop the session, and returns a
   fresh token without queueing (R07-P0-3).

Gateways never touch a DB to accept a token (R07-P0-1).

### 1.4 Orchestrator / world directory — shard — Phase 0 → 5
**Owns:** PG tables `zone`, `process`, `region_lease` (generations) and `placement_log`; KV buckets
`LEASES` and `DIRECTORY`.

**API (per 04 §1):**
- `RegisterProcess` returns a process ID and an ID-node lease, kept alive by a 1 Hz heartbeat (KV TTL
  **3 s**).
- `AssignRegion`/`ReleaseRegion(instance, region, lease_gen)`, `Create/DestroyInstance`.
- `ReportLoad`, `ResolveRoute`, `TransferPlayer`, `Drain`.
- `PreProvision(zone, time, size)` sizes zones for scheduled sieges (R01-P1-14, R01-P2-21).
- `MintTrunkToken(src, dst)` issues netcode tokens under a trunk key for the gateway↔cell and cell↔cell
  trunks (04 §2.6).
- `Split`/`Merge` arrive in Phase 5.

**Leases:** generations are allocated in Postgres before any holder is told. A holder that cannot renew
self-fences.

**Placement:** `Placer` has two implementations. `LocalProcessPlacer` supervises processes with os/exec,
backoff and a warm pool (the SWG TaskManager analogue, R02-P2-19). `AgonesPlacer` uses the Allocation API.
Quiet zones share processes; hot zones get dedicated ones (R01 §3.2).

**Failure:** two replicas run, with the leader holding a KV CAS key. When a lease lapses, the leader bumps
the generation, kills the process and assigns a warm standby (one per 8 cells per class from Phase 2). The
standby CAS-advances the AG fences and loads their checkpoints (04 §6.4). Hitch **≤ 10 s**.

### 1.5 Character — shard — Phase 1
- **Owns:** character rows, the appearance vector, bind/home, last zone, a progression snapshot and the
  account-scope settings blob (≤ 256 KiB: keybinds, UI layout, chat tabs; 08 §1.4). The computed stat
  "brain" moves with handoff (R01 §3.2).
- **API:** `List`, `Create`, `Delete` (soft, 30 days), `Get`, and the async `ApplyProgression(char, delta, idem)` (06).
- **Events:** `evt.<shard>.character.{created,entered_world,left_world}`.

### 1.6 Item & currency ledger — shard — Phase 1 core, Phase 2 full, Phase 5 partitioned
The value authority (R07-P0-7, R05-P0-8, R09-P0-7/8).

**Currency** is double-entry: postings sum to zero per currency.
- Each `ReasonCodeDef` (06), e.g. `Faucet.Bounty.NPC` or `Sink.Tax.Market.Broker`, has system accounts
  `mint:<code>` / `burn:<code>`. These may go negative and are not balance-materialized, so they never
  become hot rows.
- Player, corp-division and escrow wallets materialize `balance ≥ 0`.

**Items** have Snowflake IDs and are either uniques (rolled stats and sockets in `attrs JSONB`, per 06
`ItemInstance`) or stacks. Each has an owner, a location (container, hangar, structure or escrow), a flag
and a `version`.
- *At rest*, items change only through services.
- *In custody* of a cell's authority group (AG), a change also needs that AG's current fence (R09 A15).

**API:**
- `Execute(LedgerTx{idem_key, reason_code, actor, fence?, preconditions[], ops[]})` applies these ops
  atomically: `Mint`, `Burn`, `Transfer`, `MoveItem`, `Split`, `Merge`, `Modify`, `Destroy`,
  `TakeCustody`, `ReleaseCustody`, `EscrowOpen`/`Settle`/`Refund`.
- Wrappers: `Grant(reward_token)`, `AtomicSwap`, `ConsumeBatch`.
- Reads: `Journal`, `ListAssets`, `ItemHistory`. Admin: `Reverse`.

**Rules:**
- Unknown reason codes are rejected (R01-P0-8). Per-code rate caps implement 06's economy guard.
- Creation and owner-to-owner transfers are synchronous.
- `ledger_policy: batched_consume` records (ammo, fuel) are consumed in custody and reported by
  `ConsumeBatch` every 10 s, so a crash refunds at most one window.
- Rows lock in ascending-ID order at READ COMMITTED. 40P01 and 40001 errors are retried.

**Events:** `evt.<shard>.ledger.tx`, published via the outbox, carries every posting with its reason code.
Consumers: telemetry, trust, the public API and cell inventory caches.

**Scaling:** stateless handlers on the shard primary.
- Phase 2: 2k tx/s (AAA-SRV-8).
- Phase 4: 10k tx/s.
- Phase 5: 50k tx/s, with owner-hash partitions and cross-partition moves as escrow sagas.

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
- Actors are hash-sharded across replicas, with KV-leased ownership (R07 §4, R02-P1-11).

**Order flow:**
1. Ledger `EscrowOpen`, idempotent by order_id.
2. Insert the order and ack.
3. A match writes its trades in one market-DB transaction.
4. `EscrowSettle` runs from the outbox and sends broker fees and taxes to `burn:*`. It cannot fail for lack
   of funds, because both sides were escrowed up front.

**Recovery:**
- Actors reload their open orders.
- Unsettled trades are re-driven.
- A reconciler refunds orphan escrows after 10 min.

### 1.8 Industry, resources & durable timers — shard — Phase 2
**Timers** are a shared library plus a worker.
- Rows: `(id, due_us, kind, payload, idem_key, state)`.
- Workers claim 500 due rows at a time with `FOR UPDATE SKIP LOCKED`, keep the next 60 s in memory, and wake
  on `LISTEN/NOTIFY`.
- A fired timer publishes `timer.<shard>.fired.<kind>` with `Nats-Msg-Id = id`.
- Timers use wall clock only. TiDi timers stay in cells.
- Target: lateness p99 < 1 s with 1 M pending (R01-P1-13).
- `evt.<shard>.timers.upcoming` gives the orchestrator 30 min notice.

**Industry:**
- Recomputes duration and fee within clamps (06).
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

### 1.10 Chat — global channels, shard "local" — Phase 2
**Channels:** local, system, fleet, corp, alliance, whisper, custom, with ACLs, mutes and reports.
**History:** whisper 30 d, corp 90 d, local none. Local membership comes from cell events.

**Send path:**
1. Client → gateway lane → `rpc.<scope>.chat.Send`.
2. The service checks the ACL and the rate limit (5 msgs/10 s), then filters and persists.
3. It publishes on core NATS `chat.<scope>.<type>.<id>`.
4. Each gateway subscribes once per channel and fans out to its clients.

**Content:** plain text with allowlisted markup (R05 §7.4).

**Delivery:** at-most-once fan-out, with a history refetch on reconnect. No XMPP (R07 §4).

### 1.11 Social: friends, presence, corps, alliances — global — Phase 2
**Owns:** friends and blocks; `org` (corp/alliance) with members, 128-bit role masks, titles and standings.
Corp divisions are ledger wallets and locations.

**Presence:** Valkey `presence:<char>`, TTL 90 s, refreshed every 30 s.

**API:** friend, block, org, membership, role, alliance and standing operations, plus `CheckPermission`.

**Shared role model (R09 A11):** `pkg/perm` evaluates masks. Effective masks are projected into KV `PERMS`,
so the ledger, market and structures check permissions against a local cache.

### 1.12 Party, matchmaking & activity — Phase 3
- **Parties** persist across activities (R05 §3.3); **matchmaking** balances roles and keeps parties whole.
- **Population placement (R05-P0-6)** chooses an instance by capacity, friends, guild and language
  (R07 §2.2).
- **Activity state (the Destiny Activity Host, R05-P0-5):** objectives, encounter phase, checkpoints and
  weekly lockouts (R03-P1-8).
  - Lives in KV `ACTIVITY`, updated async by cells.
  - Snapshotted to PG.
  - A standby rehydrates from it in ≤ 5 s.

### 1.13 Persistence gateway & fence — shard — Phase 1
The SWG DatabaseServer / Star Citizen Scribe role (R04-P0-8, R07-P0-8). It also owns the **Fence**
(04 §6.1).

**Fence.**
- `authority_fence(ag_id, owner_cell, epoch)` lives in the shard primary.
- `Fence.Advance(ag, expected, next, owner)` is a conditional `UPDATE … WHERE epoch = $expected`
  (p50 ~1 ms, p99 < 5 ms). It publishes `evt.<shard>.fence.advanced`.

**Checkpoints.**
- Cells publish `persist.<shard>.<zone>.<cell>` batches every 1 s.
- A dirty AG is included every 30 s (10 s by Phase 5), and immediately on logout or zone exit.
- Record format: `{ag_id, epoch, seq, schema_ver, zstd blob, item_refs[]}`.
- The gateway flushes every 250 ms or 2,000 records: `COPY` into a temp table, then one merge
  `WHERE (excluded.epoch, excluded.seq) > (t.epoch, t.seq)`.
- Records whose epoch is below the fence are rejected and alerted.

**Load barrier.** `Checkpoint.Load(ag | zone, min_stream_seq)` answers only after the stream is applied up
to that sequence, normally in under 1 s. A standby therefore never loads a stale checkpoint.

**Ledger wins.** On load, the cell reconciles `item_refs` against ledger custody:
- items the ledger places elsewhere are dropped;
- custody items missing from the blob are respawned.

**Lifecycle.** Every persistent **record template** declares despawn, decay and retention rules. A cleanup
job expires AGs and destroys their value via the ledger with reason `decay` (R04 §3.2).

### 1.14 Content / publish — global — Phase 1 pins, Phase 3 live edit
**Owns:**
- builds: git commit, schema and record-DB hashes, manifests;
- channel pointers;
- pins, mapping (shard, zone, instance kind) to a build;
- edit changesets.

**Source of truth:** git (ADR-006). CI registers builds here.
- **Play instances pin a published build.**
- The **edit instance** journals changesets, which the editor exports to git (R03-P0-3/4).

**API:** `RegisterBuild`, `Promote`, `Pin`, `SubmitChangeset`, `Rollback`.

**Hot reload:** cells swap record-DB and Luau deltas at a tick boundary, in ≤ 2 s (R03-P0-5, ADR-012).

**Collab service (Phase 2 locks and presence, Phase 3 edit instances).** It runs in this domain and is
specified in 07 §1.8: the only writer of the JetStream stream `COLLAB_<session>`, KV `LOCKS_<session>` and
`NOTES_<session>`, validation with the generated Go validators, and **overlay content versions** (base build
+ journal range) that dev play instances pin for publish preview. Production channels accept only CI builds.

### 1.15 Live config, flags, kill switches, calendar — Phase 1
**Store:** `config_entry(scope, key, value JSONB, version, author, reason)`, with history.

**Delivery:** projected into KV `CONFIG` and applied within 1 s.

**Kill switches:** each names a feature: `econ.trade`, `market.region.<id>`, `mail.attach`. From Phase 4,
economy switches need two approvers.

**Calendar (R05-P1-16):** publishes season, reset and event triggers as `evt.<scope>.calendar.<event>`.

### 1.16 Telemetry, economy dashboards, trust — Phase 1 basic, Phase 2 ClickHouse
**Ingest:** `tel.<scope>.<source>.<type>` events are batched every 100 ms into ClickHouse (a PG table in
dev).

**Economy dashboards:** ledger events feed version-controlled Grafana dashboards (R01 §5, R09 A8):
- faucets and sinks by reason code, per hour;
- money supply and velocity;
- price indices;
- destruction.

**Trust (Phase 4):** consumes 04 §9 violation telemetry and ledger events. Rules cover wealth jumps > 6σ,
item-count drift and RMT graphs. A rule hit opens a case and can trip a kill switch.

**Transport:** JetStream until 200k events/s, then evaluate Redpanda (R07 flag 4).

### 1.17 GM/admin API & audit — Phase 1 commands, Phase 4 full
**RBAC roles:** `support`, `gm`, `senior_gm`, `economy`, `ops`, `dev`. MFA is required in prod.

**Actions:**
- kick, mute, ban, teleport and spawn, sent via `ctl.<shard>.cell.<id>.gm` and applied at a tick boundary;
- ledger history, restore and reverse;
- rollback (§4.5);
- moderation and config (R07-P0-12, R08-T27).

**Audit:** every call requires a reason and appends to a hash-chained `audit_log`
(`h_n = BLAKE2b(h_{n-1} ‖ row)`). The chain head is anchored daily in object storage.

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
- Headers: `Helios-Idem`, `Helios-Deadline-Ms`, `Helios-Fence` (`ag:epoch:cell`), `traceparent`.
- The generated Go binding calls the *same* handler as connect-go.
- No service discovery or HTTP/2 is needed in the sim process (R01-P1-11).

### 2.2 Subject naming
Grammar: `<class>.<scope>.<domain>[.<id>…].<verb>`.
- `class` ∈ {rpc, evt, ctl, persist, tel, chat, presence, content, timer, audit}.
- `scope` = shard (`eu1`) or `g` (global).
- Lowercase tokens only.
- Environments are separate NATS **accounts**, never subject prefixes.

| Purpose | Example | Kind |
|---|---|---|
| RPC | `rpc.eu1.ledger.Execute` | core req/reply, queue group |
| Event | `evt.eu1.ledger.tx`, `evt.eu1.fence.advanced` | JetStream |
| Control | `ctl.eu1.cell.4411.drain`, `ctl.eu1.gateway.all.kick` | core |
| Checkpoints | `persist.eu1.z1002.c4411` | JetStream |
| Chat / presence | `chat.g.corp.98001`, `presence.g.123456` | core |

### 2.3 JetStream streams and KV (R3 in prod, R1 in dev)

| Stream / bucket | Subjects | Retention | Limits | Dedup |
|---|---|---|---|---|
| `EVT` | `evt.>` | limits | 30 d, 500 GB | 10 min (outbox id) |
| `PERSIST` | `persist.>` | limits (replayable) | 24 h, 200 GB | 2 min (`ag:epoch:seq`) |
| `TIMERS` | `timer.>` | work-queue | — | 10 min |
| `TEL` | `tel.>` | limits | 72 h, 1 TB | — |
| `AUDIT` | `audit.>` | limits (PG keeps forever) | 30 d | 10 min |
| KV `LEASES` `DIRECTORY` `CONFIG` `PERMS` `ACTIVITY` | leases (TTL 3 s), routes, flags, masks, activities | — | history 1/5/10/1/5 | — |

Consumers are durable pull consumers with explicit ack (`AckWait` 30 s, `MaxDeliver` 10). After that,
messages go to `evt.<scope>.dlq.<consumer>` and an alert fires.

### 2.4 Cells and gateways ↔ services
The simulation thread does no network I/O.
- Calls go to I/O jobs.
- Replies drain at the start of the next tick, capped at 2 ms.
- Luau coroutines and C++ ops suspend meanwhile (R01-P0-4).

| Call | Caller | Mode | What waits |
|---|---|---|---|
| Connect-token check / reconnect-ticket sealing | gateway | local netcode decrypt / batched NATS call every 60 s | that client |
| `Fence.Advance` | cell (handoff, recovery) | async, ~1 ms | that AG's transfer |
| `Checkpoint.Load`, `character.Get`, custody query | cell | async; player "loading" | that player |
| Checkpoint batch, telemetry | cell, gateway | JetStream async publish; telemetry drop-and-count | nothing |
| Loot, trade, craft, currency | cell | async request with idem key; effect applied on reply | the coroutine |
| Market, mail, chat, social UI | gateway service lane | bypasses the cell; identity injected by gateway | nothing |
| Handoff, ghosts, replication | cells, gateways | HTP trunk, **never the bus** (ADR-008) | see 04 |

---

## 3. Data model

### 3.1 Stores

| Store | Holds |
|---|---|
| Global PostgreSQL 18 | identity, social, chat, content, config, audit, entitlements |
| Per-shard primary | character, ledger, fence, market, industry, mail, activity |
| Separate persistence cluster | `ag_checkpoint`, so checkpoints never compete with the ledger |
| ClickHouse | analytics |
| S3-compatible object storage (MinIO in compose) | backups, CDN origin, crash dumps, audit anchors |

### 3.2 Core schema sketch

- **IDs:** Snowflake `BIGINT`, laid out as 41-bit ms since 2026-01-01, 5-bit shard, 8-bit leased node,
  9-bit sequence.
- **Money:** `BIGINT` minor units.
- **Append-only tables:** INSERT/SELECT only for the app role.

```sql
CREATE TABLE ledger_tx (tx_id BIGINT PRIMARY KEY, reason_code BIGINT NOT NULL, actor_id BIGINT,
  ag_id BIGINT, epoch BIGINT, cell_id BIGINT, content_build BIGINT, reverses_tx BIGINT,
  created_at timestamptz NOT NULL) PARTITION BY RANGE (tx_id);       -- monthly (Snowflake = time)
CREATE TABLE idempotency (idem_key TEXT PRIMARY KEY, req_hash BYTEA NOT NULL, tx_id BIGINT,
  result BYTEA, created_at timestamptz NOT NULL);                    -- 30-day expiry
CREATE TABLE wallet (wallet_id BIGINT PRIMARY KEY, owner_kind SMALLINT, owner_id BIGINT, division SMALLINT,
  currency BIGINT, kind SMALLINT, balance BIGINT NOT NULL, CHECK (kind = 3 /*system*/ OR balance >= 0));
CREATE TABLE currency_entry (tx_id BIGINT, seq SMALLINT, wallet_id BIGINT, currency BIGINT,
  amount BIGINT NOT NULL, PRIMARY KEY (tx_id, seq)) PARTITION BY RANGE (tx_id);
CREATE TABLE item (item_id BIGINT PRIMARY KEY, type_id BIGINT NOT NULL, qty BIGINT CHECK (qty > 0),
  owner_kind SMALLINT, owner_id BIGINT, loc_kind SMALLINT, loc_id BIGINT, flag SMALLINT,
  custody_ag BIGINT, attrs JSONB, version BIGINT NOT NULL);
CREATE TABLE item_event (tx_id BIGINT, seq SMALLINT, item_id BIGINT, op SMALLINT, qty_delta BIGINT,
  from_loc BIGINT, to_loc BIGINT, version BIGINT, PRIMARY KEY (tx_id, seq)) PARTITION BY RANGE (tx_id);
CREATE TABLE svc_fence.authority_fence (ag_id BIGINT PRIMARY KEY, owner_cell BIGINT, epoch BIGINT NOT NULL);
CREATE TABLE market_order (order_id BIGINT PRIMARY KEY, client_order_id TEXT UNIQUE, region_id BIGINT,
  type_id BIGINT, side SMALLINT, price BIGINT, qty_left BIGINT, range SMALLINT, location_id BIGINT,
  owner_char BIGINT, escrow_tx BIGINT, state SMALLINT, expires_at timestamptz);
CREATE TABLE ag_checkpoint (ag_id BIGINT PRIMARY KEY, zone_id BIGINT, epoch BIGINT NOT NULL,
  seq BIGINT NOT NULL, schema_ver INT, blob BYTEA NOT NULL, item_refs BIGINT[])
  PARTITION BY HASH (ag_id);                                         -- 32 partitions
```

**Indexes:**
- `currency_entry (wallet_id, tx_id DESC)`
- `item (owner_kind, owner_id, loc_id)`, `item (loc_kind, loc_id)`
- `item_event (item_id, tx_id)`
- partial: custody items, unsent `outbox`, open orders

The other tables (accounts, characters, orgs, mail, outbox/inbox, content, `audit_log`) follow the same
conventions.

### 3.3 Partitioning, migrations, backup

**Partitioning**
- The ledger tables and `market_trade` use monthly range partitions, created 3 months ahead.
- After 13 months, partitions are detached to Parquet, which ClickHouse can still query.
- `ag_checkpoint_hist` uses daily partitions kept for 14 days.
- Every index must back a named sqlc query.
- Scylla and FoundationDB are benchmarked behind the `CheckpointStore` seam before Phase 4 (R07-P2-26).

**Migrations**
- Services use `pgx/v5` natively, with `otelpgx` tracing.
- goose v3 SQL migrations, one directory per schema, are embedded via `embed.FS`. Stubs are generated
  from `.hschema`.
- Changes follow expand/contract: release N only adds, and contraction happens in N+2.
- Migrations run pre-deploy under an advisory lock.

**Backup (CloudNativePG)**
- A synchronous in-region replica gives RPO 0.
- WAL archiving (`archive_timeout=60s`) gives PITR within 60 s.
- Base backups are nightly and kept 35 days.
- A PITR drill and conservation audit run monthly, with RTO ≤ 1 h.

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
- **Schemas and roles:** one per service, as in prod, so JSONB, partitions, `SKIP LOCKED` and
  `LISTEN/NOTIFY` behave identically.
- **Stale processes:** a stale `postmaster.pid` is detected and stopped at startup.
- **No SQLite fallback for services.** SQLite is reserved for tools and caches. `--db postgres://…` targets
  any local server.

---

## 4. Consistency & integrity

### 4.1 Dupe prevention (R07 §4's four sources)
1. **Two owners** → single custody plus the fence (04 §6.1).
   - Inside its transaction, `Execute` reads the AG's fence row `FOR SHARE`. This is the one sanctioned
     cross-schema grant.
   - It requires the request's `(epoch, cell)` and `item.custody_ag` to match.
   - A concurrent `Fence.Advance` waits for the ledger transaction. Once an advance commits, no write at
     the old epoch can commit.
   - Recovery CAS-advances a dead cell's AGs (04 §6.4). Zombies self-fence at lease loss (3 s).
2. **Crash between take and give** → one PG transaction (`AtomicSwap`, crafting finalize), or escrow plus a
   saga.
3. **Partial rollback** → value never goes through write-behind (§1.13).
4. **Replays** → idempotency keys stored with `req_hash`.
   - Same hash returns the stored result. A different hash returns `IDEMPOTENCY_MISMATCH` and raises an
     alert.
   - Keys are deterministic where possible: `loot:<killmail>:<n>`, `job:<id>:deliver`,
     `quest:<instance>:<stage>`, `reward:<token>`.

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
- Every step has an idempotent action and a compensation.
- Retries run on durable timers with capped backoff, then the saga is parked for GM review.
- Temporal was rejected as an extra stateful server with no Windows single-binary mode.

| Saga | Steps | Compensation |
|---|---|---|
| Market order | `EscrowOpen` → insert order | `EscrowRefund` |
| Mail + attachments | escrow → mail → notify | return to sender |
| Crafting slot / industry job | escrow or consume → job → timer → mint | refund per rules |
| Phase 5 cross-partition / shard transfer | escrow at A → credit at B | re-credit A |

A player trade between two cells is **not** a saga. It is one `AtomicSwap` after both players confirm.

### 4.4 Conservation audits
**Pre-commit check (Go):** postings sum to 0 per currency.

**Hourly, on a replica:**
- Σ of all balances, including system accounts, is 0 per currency.
- Each wallet touched equals the Σ of its entries.
- For each item type, minted − destroyed = live.
- Version chains have no gaps.

**Weekly:** a full audit of 1 B entries in under 30 min.

**On violation:** page on-call and optionally trip a kill switch (R07-P0-7).

### 4.5 Audit trails and rollback tooling
Every transaction records its actor, cell, epoch, content build and reason.

**Reversal:** `helios-admin ledger reverse --tx` writes a compensating transaction. Nothing is ever deleted.

**Character rollback:** `rollback-character --to T`
1. plans the character's transactions after T;
2. follows **taint** downstream to other players (dupe/RMT forensics);
3. reports conflicts;
4. needs `economy` approval;
5. executes as one `gm_rollback` batch.

Non-value state is restored from `ag_checkpoint_hist`.

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
              keys\   (generated dev JWT, shard netcode, trunk, ticket and manifest Ed25519 keys)
```

| Flag | Default | Notes |
|---|---|---|
| `--data-dir` | `.\helios-data` | all state; `reset` wipes it |
| `--services` | `all` | same binary runs one service in prod |
| `--db` / `--bus` / `--cache` | `embedded` / `embedded` / `miniredis` | or `postgres://`, `nats://`, `valkey://` |
| `--http` | `127.0.0.1:7700` | Connect APIs, launcher, `/admin`, `/cdn`, public API |
| `--ops` | `127.0.0.1:7701` | `/metrics`, `/healthz`, pprof |
| `--nats` | `127.0.0.1:4222` | cells and gateways connect here |
| `--spawn` | `gateway,cell` | supervisor runs `helios-gateway.exe` (UDP 7777) and `helios-cell.exe` |
| `--seed` | `dev` | `dev1…dev10` (password `dev`), 1 M credits, items, a corp, NPC orders; `loadtest` = 50k accounts |
| `--lan` | off | loopback avoids the Windows Firewall prompt |

**Subcommands:** `migrate`, `seed`, `reset`, `pg-install`, `doctor`.

**Compose.** `deploy/compose/docker-compose.yml` runs on Docker Desktop/WSL2 and Linux. It includes:
- postgres:18 and valkey:8;
- NATS 2.15, 3 nodes under the `ha` profile;
- MinIO;
- otel-collector, Prometheus, Grafana, Tempo and Loki;
- ClickHouse (`analytics` profile) and self-hosted Sentry (`crash` profile);
- one container per service, all from a single image.

**Admin console (`/admin`, Go templates + htmx, no Node):** health, the shard map (leases, fences, CCU),
accounts, the ledger explorer, market books, moderation, kill switches, content channels, audit, and economy.

---

## 6. Ops

### 6.1 Deployment
**Phases 0–2:** the combined binary runs on VMs. `helios-agent` supervises the cells and gateways, and there
is a weekly maintenance window.

**Phase 3+ (Kubernetes):**
- Go services run as Deployments with HPA and PDBs.
- Data: CloudNativePG (PG 18), NATS via Helm (3 nodes, R3), Valkey with Sentinel.
- Cells run as high-density **Agones Fleets** per zone class (Counters/Lists). A ≥ 10 % Ready buffer is the
  warm pool (R07 §6).
- Gateways run ≥ 2 per shard, on bare metal behind anycast scrubbing (04 §1).
- A colo baseline is decided at Phase 3 (§6.4).

### 6.2 Observability and SLOs
**Pipeline.**
- Go services use OpenTelemetry. Cells export Prometheus metrics and OTLP spans (04 §10).
- Traces go to Tempo, metrics to Prometheus, and `slog` JSON logs to Loki.
- `traceparent` rides in Connect and NATS headers, so one trace spans login → token → gateway → cell →
  ledger (R07-P0-10).

**Crashes (Phase 2).** Crashes go from sentry-native/crashpad through `crashgw` (08 §3) to self-hosted
Sentry. CI uploads symbols (R10 §9).

**Key metrics:** RED per RPC, consumer lag, outbox age, ledger tx/s by reason, fence conflicts, checkpoint
lag and cell tick p99 (R04-P1-16). Alerts use multi-window burn rates.

| SLO | Target |
|---|---|
| Login (auth + token, no queue) | p95 < 3 s; 99.9 % success / 30 d (R07 §6) |
| Connect-token issue / `Fence.Advance` | p99 < 100 ms / < 5 ms |
| Ledger `Execute` | p99 < 50 ms (Phase 2–3), < 25 ms (Phase 4); 99.95 % available |
| `PlaceOrder` / trade → settled | p99 < 150 ms / < 2 s |
| Checkpoint → PG durable | p99 < 5 s; page if > 60 s |
| Chat delivery / timer lateness | p99 < 250 ms / < 1 s |
| Ledger invariant violations | **0**; any violation pages |

### 6.3 Releases, live ops, hotfixes
**Go services** go through canary steps of 1 → 10 → 50 → 100 %.
- HTTP traffic shifts by route weight; NATS traffic shifts by replica share.
- Every step is SLO-gated.
- Rollback redeploys the previous image. This is safe because migrations are expand-only.

**Cells** take weekly downtime until Phase 3. After that they roll through handoff, which requires N↔N+1
mesh compatibility (04).

**Hotfixes** are minor content builds:
1. Validate the build.
2. Canary it on one PTR zone.
3. `Promote` it.
4. Cells apply it at a tick boundary.

To roll back, move the pointer back. Kill switches give instant mitigation (R07-P1-18, R03-P1-11).

### 6.4 Capacity & egress cost (R07 flag 6)
Assumptions, to be replaced by vendor quotes at Phase 3:
- 256 kbit/s average downstream per player;
- average CCU = 50 % of peak;
- egress at $0.02–0.05/GB;
- one 8-vCPU cell per 250–500 players, plus a 20 % warm pool.

| Peak CCU | Egress peak | Per month | Cloud $/month | Cells |
|---|---|---|---|---|
| 5k (Phase 3) | 1.3 Gbit/s | ~210 TB | $4–11k | 15–25 |
| 50k (Phase 4) | 12.8 Gbit/s | ~2.1 PB | $42–105k | 120–240 |
| 100k (Phase 5) | 25.6 Gbit/s | ~4.2 PB | $84–210k | 240–480 |

Patch egress is similar (200k MAU × 2 GB/week ≈ 1.7 PB/month). Colo bandwidth is typically several times
cheaper.

### 6.5 Security
- **Secrets:**
  - Dev uses local keys, staging SOPS/age, prod Vault/KMS via External Secrets.
  - The manifest **root key stays offline**; subkeys rotate quarterly.
  - The shard netcode key rotates daily by draining gateway instances (§1.3).
  - 04's X25519 re-key (forward secrecy) is a Phase 4 security-review item.
- **Transport:** TLS 1.3 at the edge, mTLS between services.
- **NATS:** per-role users with **subject permissions**. A cell publishes only to `persist.<shard>.>`,
  `tel.>` and `rpc.<shard>.{ledger,persist,fence,character,activity,chat}.>`, and subscribes only to
  `ctl.<shard>.cell.<own-id>.>`.
- **Database:** each role is confined to its own schema. `UPDATE` and `DELETE` are revoked on the ledger
  and audit tables.
- **Rate limits (R07 §4):** at the gateway (per message type), at the API edge (GCRA per account/IP), and
  per domain (chat, orders, mail).
- **DDoS:** cells are never public, and the token challenge blocks amplification (04). APIs sit behind a
  CDN/WAF with bot protection on `Login`, and the login queue is the pressure valve.
- **Supply chain:** `govulncheck`, license checks, SBOMs, signed binaries. Messages are capped at 1 MiB.
  The admin console is reachable via SSO/VPN only.

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
/keys/keyset.json                                      root-signed subkey list
```

**Manifest (`.hman`).** A schema-generated binary, zstd-compressed. It holds:
- build, monotonic `sequence` and platform;
- per file: path, size, hash, chunk list, tags (language, optional content, vaulting — R05-P2-25) and an
  install **tier** (0 = launcher/client/login area, 1 = common, 2 = streamable regions);
- a pack index and patches.

**Signing and rollout.**
- The manifest is Ed25519-signed (`crypto/ed25519`). The launcher checks the signature with
  `monocypher-ed25519` (ADR-013) before running anything.
- The pointer is `{build_id, sequence, manifest_hash, min_launcher, min_client, cdn_hosts[],
  next?{build_id, manifest_hash, available_at}, rollout_pct, expires, sig}`. `next` drives pre-download
  (08 §2.6).
- A client takes a staged build only if `hash(install_id) mod 100 < rollout_pct`.
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
- Tokens pin the content build, and gateways reject a mismatch.
- Promotion to live needs two approvers and is audited.
- In dev, `helios-patch publish --channel dev` writes to `helios-data\cdn`.

---

## 8. Go layout, interfaces, testing

`services/`: module `helios.dev/services`, `go 1.27.0` / `toolchain go1.27.1`, `CGO_ENABLED=0`.

```
cmd/        helios-backend helios-admin helios-patch helios-loadgen helios-dev helios-agent
internal/   app/, <service>/{service.go, domain/, store/ (sqlc), events.go}   # one per §1 service
pkg/        bus cache idgen authn perm outbox inbox saga ratelimit obs connecttoken cdc manifest testkit
gen/ proto/ migrations/<svc>/ deploy/{compose,k8s,grafana} seed/ loadtest/ testdata/vectors/
```

```go
type Service interface { Name() string; Register(*app.Registry) error
    Start(context.Context) error; Stop(context.Context) error; Health(context.Context) error }
type Store interface { InTx(ctx context.Context, o pgx.TxOptions, fn func(pgx.Tx) error) error } // retries 40001/40P01
type Bus interface { Publish(ctx context.Context, subj string, m hs.Message, o ...PubOpt) (PubAck, error)
    Request(ctx context.Context, subj string, in, out hs.Message) error
    Consume(ctx context.Context, stream, durable string, h Handler) error; KV(bucket string) (KV, error) }
type Placer interface { Allocate(context.Context, ProcRequest) (ProcHandle, error); Release(context.Context, ProcHandle) error }
type CheckpointStore interface { ApplyBatch(context.Context, []Checkpoint) error
    Load(context.Context, AgID, StreamSeq) (Checkpoint, error) }
type Fence interface { Advance(ctx context.Context, ag AgID, expect, next Epoch, owner CellID) (bool, error) }
```

**Testing.**
- **Unit/property:** domain packages are pure and take an injected clock. `rapid` property tests check
  conservation under random op sequences and that the order book never crosses. `go test -fuzz` covers the
  token, manifest and CDC codecs.
- **Integration:** `pkg/testkit` runs embedded-postgres, NATS and miniredis in-process. It needs no Docker
  and runs on Windows. Each test clones a migrated template database (~150 ms). Linux CI adds
  `testcontainers-go` for real Valkey, a 3-node NATS cluster and PG failover.
- **Contract:** golden vectors (tokens, manifests, Helios-binary messages, Snowflake IDs) are checked by
  both `go test` and doctest.
- **Chaos:** `kill -9` mid-saga, NATS node loss, PG switchover, duplicated or reordered messages,
  zombie-cell writes, ±2 s clock skew.
- **Load:** `helios-loadgen` covers login storms, the ledger mix, a hub market, checkpoint floods and chat.
  The C++ bot swarm runs at **3–5× target CCU** (R05-P1-20). Runs are nightly with a ±10 % regression gate.

---

## 9. MVP → AAA feature ladder

| Area | Phase 0 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 |
|---|---|---|---|---|---|---|
| Runtime | all-in-one exe, Win+Linux CI | compose; combined binary in prod | Sentry crash ingest | K8s + Agones | multi-region DR | global tier (CockroachDB eval) |
| Identity/session | password, JWT, tokens + vectors | launch codes, reconnect tickets | queue lanes, 50/s admission | full queue | OIDC, MFA, 200/s | 500/s |
| Orchestrator | local supervisor, 1 cell | leases, fence, crash restore | warm standby, multi-zone | multi-cell, instances, pre-provision | rolling restarts | split/merge |
| Ledger | schema, reason codes | wallets, grants, custody, idem | items, trades, escrow, audits, 2k tx/s | rollback tools | 10k tx/s, trust rules | partitioned 50k tx/s |
| Persistence | — | checkpoints ≤ 30 s, barrier | lifecycle cleanup | history + restore | hot standby (04) | ≤ 10 s window |
| Economy | — | — | market, industry, timers, resources, mail | contracts, territory timers | public market API | economy sim (R04-P2-20) |
| Social | — | — | chat, friends, presence, corps | alliances, parties, matchmaking, activity | moderation tools | cross-shard |
| Content | local build pointer | pins, hot reload | live/ptr/dev channels, collab locks/presence | live-edit changesets, overlay versions | two-person promote | UGC publish (R03-P2-1) |
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
| A5 | PG ledger 2k tx/s for 1 h at p99 < 50 ms; Phase 4: 10k tx/s at p99 < 25 ms | 2 / 4 |
| A6 | 72 h chaos soak: **0** invariant violations, **0** stuck sagas | 2 |
| A7 | Hub market 2k orders/s into one actor; `PlaceOrder` p99 < 150 ms; settlement p99 < 2 s | 2 |
| A8 | Admission 50/s (Phase 2), 200/s (Phase 4), 500/s (Phase 5) per shard; unqueued login p95 < 3 s | 2–5 |
| A9 | Checkpoints 30k AG/s per persistence cluster; durability lag p99 < 5 s | 4 |
| A10 | Timers: 1 M pending, lateness p99 < 1 s, 0 double effects | 2 |
| A11 | Chat 5k msg/s at p99 < 250 ms | 3 |
| A12 | Patch download ≤ 1.5× the changed bytes; verifying a 50 GB install ≤ 5 min (AAA-CNT-7; 08 CL-9, CL-10) | 2 |
| A13 | PG failover: recovery < 30 s, 0 lost committed ledger txs; monthly PITR drill ≤ 1 h | 3 |
| A14 | Content hot reload ≤ 2 s from publish to applied (dev) | 1 |

---

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Checkpoint write volume | Separate cluster, COPY batching, hash partitions, `CheckpointStore` seam + Scylla/FDB benchmark |
| embedded-postgres offline/first run (R10 §14) | Cached binaries, `pg-install --from`, `--db` to local PG, Windows cold-start CI |
| Ledger becomes a single point of failure | Sync replica, idempotent retries, "pending" UX, per-feature kill switches |
| Hot rows (escrow, corp wallets) | Unmaterialized system accounts; escrow wallets sharded per region |
| Connect and NATS bindings drift apart | One `.hschema` source; conformance suite runs every RPC over both |
| JetStream limits | Per-stream quotas, R3 sized by load tests, Redpanda for telemetry |
| Egress cost | Rerun the §6.4 model at each phase gate; colo decision by Phase 3 |
| Signing-key compromise | Offline root, rotating subkeys, pinned keyset, anti-rollback |

---

## 12. Traceability

| Requirement | Section |
|---|---|
| R07-P0-1/2/3, R03-P0-8 | §1.1–1.3 |
| R07-P0-4, R02-P2-19, R01-P1-14, R01-P2-21 | §1.4, §6.1 |
| R07-P0-7, R05-P0-8, R09-P0-7/8, R02-P0-7 | §1.6, §1.13, §4 |
| R07-P0-8, R04-P0-8 | §1.13 |
| R07-P0-9, R02-P2-16 | §1.10–1.11 |
| R07-P0-10, R01-P2-22, R05-P1-20 | §6.2, §8 |
| R07-P0-11, R01-P0-9, R03-P1-6, R05-P2-25 | §7 |
| R07-P0-12, R08-T27 | §1.17 |
| R07-P1-15, R02-P1-9/11, R01-P1-13 | §1.7–1.9 |
| R05-P0-5/6, R03-P1-8 | §1.12 |
| R07-P1-17/18, R03-P1-11, R05-P1-16 | §1.15, §1.20, §6.3 |
| R07-P1-19, R01-P0-8 | §1.16 |
| R07-P1-20, R07-P2-21/23/26 | §6.5, §7, §3.3, §9 |
| R01-P1-11/12/17, R05-P2-23 | §2.1, §3, §1.18 |
| R03-P0-3/4/5, R04-P0-9, R05-P0-7 | §1.14, §2.1 |

---

## 13. Cross-section dependencies

| Section | Depends on / must deliver |
|---|---|
| **04 Networking** | Token format + vectors, service lane, `Fence.Advance`, checkpoint cadence, `item_refs` reconcile, N↔N+1 mesh |
| **02 Engine** | Snowflake 41/5/8/9. `.hschema` must support service blocks, `ledger_policy`, lifecycle rules and `ReasonCodeDef`. schemac must emit `.proto`, Helios-binary codecs and the Go NATS binding. |
| **06 Gameplay** | Keep the anchors §1.5, §1.6 and §1.8. Use "record template", not "archetype". |
| **08 Launcher** | Launch codes, SSE queue, BLAKE2b chunks, tiers, `rollout_pct`, patch-from, anti-rollback, install journal |
| **07 / 09** | Collab service and changeset export to git (07 §1.8). A1–A14 (A3 split into A3a/A3b) as phase exits, with the perf environment and bot swarm by Phase 2 (09). |
