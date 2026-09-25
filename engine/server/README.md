# engine/server — cell and gateway runtime (WP-0.14)

`helios::server` (target `helios_server`, HEADLESS, L4, peer `authority`; deps `helios::core`,
`helios::net`, `helios::ecs`, `helios::authority`; private `helios::tp::natsc`, `helios::tp::yyjson`)
is everything `helios-cell` and `helios-gateway` run, as a library, so the whole path
**client → gateway → cell** runs in one test process (04 §1, §2.3–2.6, §3, §9; 05 §1.3–1.4, §2).
The executables in `apps/cellserver` and `apps/gateway` only parse options and call `run()`.

```
                    HTTPS                         NATS (nats.c, JSON until schemac codecs)
  client ──────────────────────► Go backend ◄──────────────────────────────────────┐
    │  netcode token (Session svc)   Session svc: tokens, session_epoch, tickets    │
    │                                 Orchestrator: registry, leases, ResolveZone   │
    │ HTP UDP 7777                                                                   │
    ▼                                                                                │
 GatewayServer ── ResolveZone, SealReconnectTickets, ctl.<shard>.gateway.all.* ─────┤
    │  session table · token user data · limits · Deliver fencing                   │
    │ HTP trunk (trunk key, trunk protocol id)                                        │
    ▼                                                                                │
 CellServer ── RegisterProcess, Heartbeat, AllocateIdBlocks, Deregister ─────────────┘
    ZoneHost (EDF) ─ ZoneInstance "tallis" [ZoneClock+TiDi · TickGraph · ecs::World · AgTable]
                   └ ZoneInstance "harrow" …
```

| Header (`helios/server/…`) | What it provides |
|---|---|
| `zone_host.h` | `ZoneHost`: many zone instances per process, each at its own tick rate; `runDue()` runs due ticks **earliest deadline first**; per-zone CPU feeds per-zone TiDi |
| `zone_instance.h` | `ZoneInstance`: a flecs world, `ZoneClock`, `TickGraph`, `AgTable`, attached sessions; thread-safe inbox (`post`), outbox (`takeOutbox`); the Phase 0 dev game (Echo, `TickState` every tick, `TimeDilation` notices) |
| `tick_graph.h` | `TickGraph`: the 04 §3.3 stages (Input → PrePhysics → Physics → PostPhysics → AuthorityFlush → ReplicationGather → ConnectionWrite → Send) with named hooks, `after` ordering, tick-thread and parallel (job-system) hooks, per-stage budgets (35 ms at 20 Hz, scaled by 20/Hz) and overrun counters |
| `cell_server.h` | `CellServer`: trunk server, zone lifecycle from lease events (or static zones), session routing with trunk fencing, zone output to trunks |
| `gateway_server.h` | `GatewayServer`: netcode server for clients, trunk clients to cells, ResolveZone routing with a short cache, forwarding both ways, `session_epoch` eviction, kicks, reconnect tickets, 04 §9 limits |
| `protocol.h` | Trunk messages, client CONTROL messages, dev game messages, connect-token user data (Go layout v1) |
| `orch_protocol.h` | The Go services' JSON contracts (byte-exact with Go's encoder), subjects, RFC 3339 |
| `orchestrator_client.h` | `OrchestratorClient` (the C++ twin of Go's `orchestrator.Agent`) and `OrchestratorIdBlockSource` (the production `ecs::IdBlockSource` over `AllocateIdBlocks`) |
| `bus.h`, `nats_bus.h`, `fake_bus.h` | `IBus` (request/reply, subscribe, publish); `NatsBus` over nats.c 3.14; `FakeBus` in process with NATS subject semantics |
| `keys.h` | base64, Go keyring files, protocol ids, `insecureDevKey` |
| `probe_client.h` | `ProbeClient` (a minimal game client) and `mintDevToken` |
| `app_env.h` | Options (`--key=value` / `--key value`), backend environment variables, key lookup, Ctrl+C |

## Zones, ticks and TiDi (04 §3)

A `ZoneInstance` tick consumes one due `ZoneClock` step and runs its `TickGraph`:

| Stage | Hooks registered by the zone |
|---|---|
| Input | `zone.input` (ID blocks to the minter, fence replies via `AgTable::poll`, `World::beginTick`, inbox drain ≤ 256 items), `ecs.Input` |
| PrePhysics / Physics / AuthorityFlush | `ecs.<stage>` (sim, the physics stub, effects and handoff later) |
| PostPhysics | `ecs.PostPhysics`, `zone.gameplay` (Echo replies) |
| ReplicationGather | `ecs.ReplicationGather`, `zone.gather` (`World::gatherChanges`; replication arrives with `engine/replication`) |
| ConnectionWrite | `ecs.ConnectionWrite`, `zone.write` (a `TickState` STATE chunk per session) |
| Send | `ecs.Send`, `zone.send` (outbox for the host) |

The tick's wall time goes to the TiDi controller (see `engine/authority/README.md`); a new dilation
is scheduled two ticks ahead and announced to every session on CONTROL (`TimeDilation{tick, d}`),
and every `TickState` carries the current `d`. **NS-0.6** (empty-zone tick < 0.5 ms) is a test: the
median and p90 of 400 ticks of an empty zone (flecs world, eight ECS stages, graph bookkeeping) are
≈ 1–2 µs on this container's GCC 13 RelWithDebInfo build.

## Wire protocols

**Trunk** (gateway ↔ cell): netcode connections with the trunk profile (`ConnectionConfig::trunk()`),
opened with trunk tokens (protocol id `0x48454C494F540001`, the trunk key; the orchestrator's
`MintTrunkToken` replaces local minting later). Every message is `varint stream` + `u8 type` + body:

| Type | Stream | Channel | Body |
|---|---|---|---|
| Hello / Welcome | 0 | CONTROL | version, process id (+ epoch), name |
| ZoneFenced | 0 | CONTROL | zone id, the lease generation given up |
| Attach | session | EVENT_R | session_epoch, zone (0 = the cell's default), account, character, flags |
| AttachAck / AttachNack | session | EVENT_R | epoch, zone, lease_gen, tick, tick Hz, d, name / reason (not_hosted, stale_epoch, full) |
| Detach | session | EVENT_R | epoch, reason |
| Forward | session | EVENT_R or EVENT_U | client channel + payload |
| Deliver | session | EVENT_R or EVENT_U | lease_gen, client channel + payload |

Per-session order holds end to end (Attach, Detach and reliable traffic share EVENT_R). The cell
routes a session only from the trunk that attached it most recently at the highest epoch; the
gateway drops a Deliver below the lease generation it attached under and deliveries for sessions it
did not attach through that trunk (receivers fence too, 05 §1.4.2).

**Client CONTROL** (gateway ↔ client): `Welcome` (routed: zone, tick, rate, d), `ReconnectTicket`
(sealed by the Session service, every 60 s), `TimeDilation`, `Kick{reason}`, `RouteState` (being
re-routed after a cell loss), `Ping`/`Pong`. **Dev game**: `EchoRequest`/`EchoReply` on EVENT_R,
`TickState` on STATE. **Token user data**: Go `connecttoken.UserData` v1, checked byte for byte.

## Control plane (05 §1.3–1.4, §2)

JSON over core NATS request/reply exactly as `services/internal/orchestrator` and
`services/internal/session` write it (64-bit integers as strings, `omitempty`, RFC 3339 times),
checked against vectors produced by the Go types themselves (`tests/data/go_contract_vectors.tsv`,
regenerated by `tests/data/gen_go_vectors.go.txt`). Requests carry `Helios-Deadline-Ms`; service
errors arrive as `Helios-Error` / `Helios-Error-Message` headers.

| Subject | Who | Use |
|---|---|---|
| `rpc.<shard>.orch.RegisterProcess` | cell, gateway | cells declare their zones and trunk address; gateways their UDP address, `keyId`, capacity |
| `rpc.<shard>.orch.Heartbeat` | both, every `heartbeatIntervalMs` | load (players, free slots, tick p99); the reply's assignments drive the lease holder |
| `rpc.<shard>.orch.AllocateIdBlocks` | cell | ID-block prefixes for every zone's `EntityIdMinter` |
| `rpc.<shard>.orch.ResolveZone` | gateway | zone → owning cell's trunk address and lease generation |
| `rpc.<shard>.orch.Deregister` | both, on shutdown | hands zones back without waiting for the 12 s TTL |
| `rpc.<shard>.session.SealReconnectTickets` | gateway, every 60 s | ≤ 1,024 sessions per call; `missing` sessions are kicked |
| `ctl.<shard>.gateway.all.session_epoch` | gateway subscribes | a newer epoch evicts the session (`Server::findSession` + `disconnect`) |
| `ctl.<shard>.gateway.all.kick` | gateway subscribes | logout, superseded, ban |

**Holder rule.** Timeouts, no responders, a leadership change (`unavailable`) or a dead bus never
fence: the cell keeps its zones ticking and keeps heartbeating. Only `lease_lost`
(`failed_precondition`) or an assignment list without the zone, or with another generation, stops a
zone; the cell then registers again (new epoch) and hosts what it is given, under the new
generations. Verified live: with the backend stopped for 6 s the zone kept ticking; the restarted
orchestrator (empty registry) answered `lease_lost`, the cell fenced, re-registered as epoch 2 and
hosted Tallis again at lease_gen 2.

## Run it locally (Windows; Linux is the same with `/` paths)

Build (Developer PowerShell for VS): `cmake --preset windows-msvc-release` then
`cmake --build --preset windows-msvc-release --target helios-cell helios-gateway`.

**1. The backend with the Tallis zone.** In `services\`, create `saved\backend\helios.toml`:

```toml
[[orchestrator.zones]]
id = 1001
name = "dev-sandbox"

[[orchestrator.zones]]
id = 1002
name = "tallis"

# Optional: let the backend start and supervise both servers (it passes HELIOS_NATS_URL,
# HELIOS_NATS_USER/PASSWORD, HELIOS_NETCODE_KEYS, HELIOS_PROTOCOL_ID, HELIOS_TOKEN_LIFETIME).
[[orchestrator.spawn]]
name = "cell-a"
exe = "../build/windows-msvc-release/bin/helios-cell.exe"
args = ["--name", "cell-a", "--zone", "tallis"]

[[orchestrator.spawn]]
name = "gateway"
exe = "../build/windows-msvc-release/bin/helios-gateway.exe"
args = ["--name", "gw-1", "--listen", "127.0.0.1:7777", "--token-lifetime", "30"]
```

```powershell
cd services
go run ./cmd/helios-backend --seed dev
```

**2. Without the spawn entries**, start the servers yourself from the repository root; they find
the NATS password and the shard key in the backend's data directory:

```powershell
.\build\windows-msvc-release\bin\helios-cell.exe --nats nats://127.0.0.1:4222 --backend-data services\saved\backend --zone tallis
.\build\windows-msvc-release\bin\helios-gateway.exe --nats nats://127.0.0.1:4222 --backend-data services\saved\backend --token-lifetime 30
```

**3. Log in, get a token, connect** (the probe stands in for `helios-client` until WP-0.17):

```powershell
$login = Invoke-RestMethod -Method Post -ContentType application/json `
  -Uri http://127.0.0.1:7700/helios.identity.v1.Identity/Login -Body '{"login":"dev1#0001","password":"dev"}'
$s = Invoke-RestMethod -Method Post -ContentType application/json -Headers @{Authorization = "Bearer $($login.accessToken)"} `
  -Uri http://127.0.0.1:7700/helios.session.v1.Session/CreateSession -Body '{"zoneId":"1002"}'
.\build\windows-msvc-release\bin\helios-gateway.exe probe --token $s.connectToken --seconds 5
```

Expected: `welcome: zone 1002 'tallis' at 20 Hz …`, `echoes: 10/10 …` and tick states at 20 per
second (reconnect tickets follow every 60 s; `--ticket-interval 5` on the gateway shows them sooner). The orchestrator's view: `Invoke-RestMethod -Method Post -ContentType
application/json -Uri http://127.0.0.1:7701/helios.orchestrator.v1.Orchestrator/ListProcesses -Body '{}'`.
`Session/Reconnect` with `$s.reconnectTicket` returns a token at the next epoch; connecting with it
evicts the first probe (`kicked: superseded`) at once.

**Without the backend** (loopback only, fixed public dev keys):

```powershell
.\build\windows-msvc-release\bin\helios-cell.exe --dev-insecure-keys --zone tallis:1002
.\build\windows-msvc-release\bin\helios-gateway.exe --standalone --dev-insecure-keys --cell 127.0.0.1:7810
.\build\windows-msvc-release\bin\helios-gateway.exe probe --dev-insecure-keys --zone 1002
```

## Tests

`server_tests` (doctest, 55 cases, ≈ 2 s): wire contracts against Go's own JSON and user-data
bytes, keyrings, base64, RFC 3339, trunk and client codecs incl. 20k random inputs; tick-graph
ordering (stage order, `after`, parallel hooks after tick hooks and before the next stage, cycles
and invalid edges rejected), budgets and overruns; zone attach / echo / tick state / detach,
epoch rebinds and refusals, the count-capped inbox, TiDi notices to sessions; NS-0.6; EDF across
20/60/1/10 Hz zones with capped catch-up; per-zone TiDi isolation; `FakeBus` semantics;
orchestrator client registration, heartbeats, the holder rule (partition, `unavailable`, bus down),
`lease_lost` → fence + re-register, generation changes, register back-off, ID blocks into a minter,
ResolveZone, deregistration; end to end over `VirtualNetwork`: standalone and orchestrated routing,
default and unknown zones, `session_epoch` eviction with a NAT-rebinding reconnect, ticket sealing
and `missing`, control kicks, a zone generation change re-routing without a disconnect, a cell
crash and restart, 04 §9 limits (rate, oversized, 3 malformed → kick), refused tokens, cell-side
fencing of a superseded trunk, refusal of trunks whose token names no gateway, gateway-side fencing
of stale lease generations, and real UDP loopback (trunk and probe sockets on loopback). Review
additions: slow seal replies vs. a reconnect through the same gateway, trunk retirement, Detach
before a re-route, `ZoneFenced` by acked generation (including a session acked by the old instance
of a zone the directory already moved), the bounded zone inbox, cell config validation, the tick
p99 report, Go-exact protocol ids, non-finite JSON numbers, and the CONF-03 holder rule over a 60 s
outage. `server.nats-live` (two cases: the orchestrator and session contracts, and subscription
lifetimes) runs against a real `helios-backend` when `HELIOS_NATS_URL` (and `HELIOS_NATS_USER` /
`HELIOS_NATS_PASSWORD`) are set; it passed against the backend built from `services/`
(orchestrator and session over TCP NATS as the `fleet` user).

**Review hardening (adversarial review of WP-0.14).**
* `NatsBus::unsubscribe()` (and the destructor) return only once nats.c can no longer run the
  handler (nats.c's on-complete callback; a handler may unsubscribe itself), and the destructor
  waits for nats.c's closed callback, which nats.c runs later on its own thread. Before, both left
  nats.c calling into freed memory.
* `SealReconnectTickets` results are matched to the session_epoch each session was *sent* with: a
  slow reply never kicks (`missing`) or hands an old-epoch ticket to a newer connection of the same
  session id that reconnected through this gateway meanwhile (`staleSealResults`).
* A zone's inbox holds at most `maxInboxQueued` (4,096) client messages; beyond that they are
  dropped and counted, so sessions that together post faster than the capped drain cannot grow cell
  memory without bound. Attach/Detach are never dropped.
* Gateways retire trunks no session uses (idle 10 min, failed 30 s), release a session at its old
  cell (`Detach`) before re-routing it, re-route on `ZoneFenced` only sessions attached under the
  fenced generation, and bind trunk sockets as documented (`trunkBind` was ignored before).
* Cells reject bad tick rates and TiDi floors at start, not when the orchestrator assigns a zone
  (which would have left the cell holding a lease it does not serve).
* JSON numbers: NaN/infinity are written as 0 (Go cannot parse them) and reals are accepted for
  integer fields only when integral and in range (the old conversion was undefined).
* Protocol ids parse exactly like Go's `strconv.ParseUint(s, 0, 64)`.

## Acceptance (09 §2.1 WP-0.14)

* **NS-0.6** empty-zone tick < 0.5 ms: automated (`server.zonehost: NS-0.6 …`).
* **CONF-03** holder rule for the C++ cell host: `conformance/holder_rule: …` keeps both zones
  ticking and serving through a 60 s control-plane outage and drops one only after seeing its
  higher `lease_gen`.
* **NS-0.3** Windows client ↔ Linux gateway: the protocol is little-endian and byte-exact, the
  MinGW build compiles and links, and `helios-gateway probe --connect <linux-host>:7777 --keys
  <netcode-shard.json>` on Windows against a Linux gateway is the check; it needs the Windows CI
  runner (09 §5.4) and is not automated here.

## Known limitations

* One loop thread per process: trunk IO and zone ticks share it (04 §2.6's trunk IO pool and
  per-instance IO threads come in Phase 2). Zone stages already use the job system.
* JSON on NATS until schemac emits Helios-binary codecs; hand-written binary trunk messages until
  `schemas/net/*.hschema` exists.
* Trunk tokens are minted by the gateway with a shared trunk key (`--trunk-keys`, defaulting to the
  shard keyring in dev); `MintTrunkToken` (05 §1.4) is not implemented in the backend yet.
* No linkdead handling yet: a detached session leaves the zone at once (Phase 1: avatar stays,
  `ClientRebind`, `HeldEntities`). No suspect/probe reporting (`ReportSuspect`, Phase 2).
* Gateway limits are per channel, not per RPC (per-RPC buckets come from schema annotations).
* `NatsBus` requests use a small blocking thread pool (nats.c's request API); fine at 1 Hz control
  traffic, to revisit if service RPC volume grows. A slow RPC (a 5 s seal batch while the session
  service hangs) can delay a heartbeat queued behind it by up to its timeout.
* Environment variables are read with `std::getenv`, which on Windows returns the ANSI code page,
  so a non-ASCII key-file path in `HELIOS_NETCODE_KEYS` is not found there; core has no public
  UTF-8 environment accessor yet (use the `--keys` / `--backend-data` options instead).

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
