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
| `orch_protocol.h` | The Go services' JSON contracts (the same JSON values as Go's encoder, byte for byte on the shared vectors), `validateRegistration`, subjects, RFC 3339 |
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

JSON over core NATS request/reply with the same values as `services/internal/orchestrator` and
`services/internal/session` write (64-bit integers as strings, `omitempty`, RFC 3339 times),
checked byte for byte against vectors produced by the Go types themselves
(`tests/data/go_contract_vectors.tsv`, regenerated by `tests/data/gen_go_vectors.go.txt`). Outside
the vectors the bytes can differ where only the escaping does: Go's encoder HTML-escapes `<`, `>`
and `&` (`\u003c` …) and yyjson does not, which any JSON decoder reads as the same string.
Requests carry `Helios-Deadline-Ms`; service errors arrive as `Helios-Error` /
`Helios-Error-Message` headers.

| Subject | Who | Use |
|---|---|---|
| `rpc.<shard>.orch.RegisterProcess` | cell, gateway | cells declare their zones and trunk address; gateways their UDP address, `keyId`, capacity; both their failure domain `fd{az, rack, host}` and `serverBuild` (05 §1.4) when the placer sets them |
| `rpc.<shard>.orch.Heartbeat` | both, every `heartbeatIntervalMs` | load (players, free slots, tick p99) and `held[{region, leaseGen}]`, the regions held with their generations (left out when none); the reply's assignments drive the lease holder |
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

**Placement fields.** The placer tells a process where it runs and which build it is:
`--fd-az`, `--fd-rack`, `--fd-host` (`HELIOS_FD_AZ`, `HELIOS_FD_RACK`, `HELIOS_FD_HOST`) and
`--server-build` (`HELIOS_SERVER_BUILD`, decimal). Phase 0's `helios-backend` supervisor sets none
of them, so they are left out and the orchestrator stores empty values; Agones node labels and
`helios.toml [fd]` supply them from Phase 2 (05 §1.4.3). `orch::validateRegistration` applies every
rule of Go's `validate()` (name, address, version, host and fd lengths, NUL bytes, zones, kind, a
gateway's `ip:port` and `keyId`, a non-negative build) plus valid UTF-8 in every string, and a
registration that fails it stops the process at start, because RegisterProcess would otherwise
fail on every retry. The UTF-8 rule matters on Windows, where `std::getenv` returns the ANSI code
page: yyjson writes no document for invalid UTF-8, which would have sent an empty request (such a
failure is now logged).

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

`server_tests` (doctest, 64 cases: 63 in the main entry, ≈ 1.5 s, where the cross-host case is a no-op; one `perf:`): wire contracts against Go's own JSON and user-data
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
outage. WP-0.14's PR: the `fd`/`serverBuild`/`held` vectors, their bounds and the placement
options; held lists that follow generation changes, stay put while unreachable and restart after
`lease_lost`; CONF-03 over four failure modes with a mid-outage re-placement; NS-0.3's wire vectors,
cross-host case and same-host refusal; NS-0.6's `perf:` gate; every `validate()` rule and UTF-8. `server.nats-live` (two cases: the orchestrator and session contracts, and subscription
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

* **NS-0.6** empty-zone tick < 0.5 ms: `perf: NS-0.6 …` gates the p99 of 2,000 ticks, both the
  tick graph's time and `ZoneHost::runDue()`'s wall time, on the serial perf run (≈ 1.4 µs p99 and
  ≈ 25 µs max on this container's GCC 13 RelWithDebInfo build); `server.zonehost: NS-0.6 …` checks
  the median and p90 on every run, Windows included (the Windows jobs run no perf label).
* **CONF-03** holder rule for the C++ cell host: `conformance/holder_rule: …` runs 15 s each of no
  responders, timeouts, `unavailable` and a dead bus, back to back on the simulated clock (≈ 0.3 s
  of real time). One zone is re-placed at a higher generation half-way through. The cell keeps both
  zone instances, its registration and its sessions throughout, and its heartbeats report what it
  holds. Once the control plane answers, it drops exactly the re-placed zone and hosts it again at
  the new generation. The case fails if the client fences on no responders or gives regions up
  after 30 failed heartbeats.
* **NS-0.3** Windows client ↔ Linux gateway, in two halves (`tests/test_interop.cpp`):
  * *The bytes:* every client and trunk message and the trunk user data match
    `tests/data/wire_vectors.tsv` and decode back, on every toolchain that runs `server_tests`
    (GCC and Clang here; MSVC and clang-cl in CI), and the real-UDP loopback case runs on the Win32
    sockets in CI.
  * *The hosts* (09 §5.6 class H; the scorecard counts it only from the `windows-lab` run, WP-0.4's
    lab: the `win-gpu` runner's MSVC build against a Linux gateway and cell host). The CTest
    `server_tests_ns03_remote` connects a probe to the gateway in `HELIOS_NS03_GATEWAY` (ip:port),
    with tokens from the keyring in `HELIOS_NS03_KEYS` (the gateway's `--keys` file), and checks the
    Welcome, 10 echoes and 20 tick states. Its results:

    | Target | Result |
    |---|---|
    | none configured | Skipped |
    | another machine, served | **Passed**, the only evidence result |
    | loopback (127/8, `::1`), unspecified (`0.0.0.0`, `::`), or any address this host holds (found by binding it, so LAN, IPv4 link-local, VPN and bridge addresses too) | Failed: `NS-0.3 remote: refused: …` |
    | the same, with `HELIOS_NS03_ALLOW_LOOPBACK=1` | Skipped when the smoke run passes, Failed when it does not |
    | a filter that matches no case (a renamed case) | Failed |

    A private address that this host does not hold is accepted, such as a WSL 2 VM behind NAT on
    the Hyper-V switch. WSL's mirrored mode shares the host's addresses, so it is refused; use NAT
    mode or another machine. `server_tests_ns03_refusal` seeds `127.0.0.1:7777` and passes only
    when it is refused.

    **Owner's run, 2026-09-27 (supporting evidence, not the H verdict).** A Windows client built
    with MinGW-w64 against `helios-gateway` and `helios-cell` on Linux in WSL, over WSL's virtual
    network, passed 2 of 2 times, each with the Welcome, 10/10 echoes and 20 tick states and no
    mismatches (PR #16). It predates the same-host refusal, which accepts that setup (see above).
    No MSVC-built client has run against a Linux gateway yet.

    **Repeating it with an MSVC build, without the toolchain.** The `Windows (MSVC, primary)` CI
    job uploads `server_tests.exe` as the artifact `server-tests-msvc` (7 days; static CRT, no other
    files needed):
    1. On the Linux host, make a keyring `ring.json` (`helios-backend`'s `keys/netcode-shard.json`
       works too):
       ```sh
       python3 -c 'import base64,os,json; print(json.dumps({"version":1,"purpose":"netcode-shard","keys":[{"id":1,"secret":base64.b64encode(os.urandom(32)).decode(),"created":"2026-09-27T00:00:00Z"}]}))' > ring.json
       ```
       then start `helios-cell --standalone --trunk-keys ring.json --zone tallis:1002` and
       `helios-gateway --standalone --listen <linux-ip>:7777 --keys ring.json --trunk-keys ring.json
       --cell 127.0.0.1:7810`.
    2. On Windows, download the artifact from the PR's or `main`'s latest CI run (the run's
       Artifacts section, or `gh run download <run-id> -n server-tests-msvc`), and copy `ring.json`
       next to it.
    3. In PowerShell, run it directly, with the environment its CTest entry would set:
       ```powershell
       $env:HELIOS_NS03_ENTRY = '1'; $env:HELIOS_NS03_GATEWAY = '<linux-ip>:7777'; $env:HELIOS_NS03_KEYS = 'ring.json'
       .\server_tests.exe "--test-case=server.interop: NS-0.3 a probe here is served by the gateway on another host (HELIOS_NS03_GATEWAY)"
       ```
       A pass prints `test cases: 1 | 1 passed` and the `NS-0.3: echoes 10/10 …` line, with no
       `same-host smoke run` line.
    4. `helios-gateway probe --connect <linux-ip>:7777 --keys ring.json` is the same check with
       the shipped executable.

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
* Higher lease generations reach the cell only through register and heartbeat replies
  (`LeaseHolder::onAssignments`). v0 has no DIRECTORY watch, gateway `RouteUpdate`, trunk `Fenced`
  NACK, `ctl` message or region checkpoint, so nothing here calls `onHigherGeneration` or
  `onRegionCheckpointRejected` yet (`authority_tests` covers both). Until the next heartbeat reply,
  a superseded cell is kept from acting by the receivers: gateways drop its deliveries below the
  generation they route to (05 §1.4.2).
* Environment variables are read with `std::getenv`, which on Windows returns the ANSI code page,
  so a non-ASCII key-file path in `HELIOS_NETCODE_KEYS` is not found there; core has no public
  UTF-8 environment accessor yet (use the `--keys` / `--backend-data` options instead).

## Plan conformance

Plan-Rev: 9

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 9 by WP-0.14's PR on
2026-09-27. Revisions 7–9 changed 06 §1.2, 02 §7.4 with 04 §10.2 (a cell `VmConfig` refuses native
codegen; this module hosts no Luau VM yet) and 09 §5.2a, none of which this code implements. The one
open §5.10.4 (a) row for this module, `fd`/`serverBuild` on registration and held regions in
heartbeats, is closed by that PR. No conformance delta is open.
