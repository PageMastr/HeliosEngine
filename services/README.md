# Helios backend services (Go)

The Go control plane of Helios (ADR-008, ADR-014, `docs/plan/05-backend-services.md`). Phase 0 ships one
binary, `helios-backend`, that runs every service in-process on top of **embedded PostgreSQL 18**, an
**embedded NATS server with JetStream** and **miniredis** (the Valkey stand-in), so a Windows developer needs
nothing but Go. The same code runs against real PostgreSQL, NATS and Valkey via flags or Docker Compose.

| Service | Package | Phase 0 scope |
|---|---|---|
| Identity/Auth (05 §1.1) | `internal/identity`, `pkg/pii` | accounts with `Handle#1234` tags; e-mail addresses stored only encrypted under a per-account DEK wrapped by the subject KEK, and found by a keyed blind index (05 §6.6, below); argon2id (m=64 MiB, t=3, p=1 behind a 2×GOMAXPROCS semaphore that sheds load after 5 s), EdDSA JWTs (10 min) + JWKS, rotating refresh-token families with reuse detection (rotation and revocation serialized per family), one-time launch codes bound to the launcher's family, bans that kick the live session, per-IP/per-account GCRA rate limits, hash-chained append-only audit log |
| Session & connect tokens (05 §1.3, 04 §2.3–2.4) | `internal/session`, `pkg/connecttoken` | netcode 1.02 connect tokens (XChaCha20-Poly1305, byte-exact with vendored netcode 1.4.8), 1–4 gateway addresses picked by free slots on the newest shard key (old-key gateways drain), random 63-bit session IDs, `sess:<id>` + `session_epoch` in Valkey, reconnect tickets sealed for gateways over NATS and redeemed over HTTPS with an epoch CAS |
| Orchestrator / world directory (05 §1.4) | `internal/orchestrator` | one leader per shard anchored in PostgreSQL (`orch_leader`, 10 s lease, term-fenced writes, standby takeover), process registry (failure domain `fd{az, rack, host}` and server build stored at registration; 1 Hz heartbeat over NATS reporting the regions held with their generations; 12 s liveness TTL, per-name epochs), time-prefixed ID blocks (`id_alloc`, `AllocateIdBlocks`), v0 zone placement (one cell per zone, one region per zone) under `region_lease` generations allocated in PostgreSQL, `ResolveZone`, KV projection `DIRECTORY`, local process supervision with backoff |
| Platform | `internal/platform` | config (defaults < TOML < `HELIOS_*` env < flags), slog, OpenTelemetry hooks, `/healthz` `/readyz`, Prometheus `/metrics`, HTTP(S) servers with graceful shutdown |

## Run it

**Windows (PowerShell)** — any Go ≥ 1.21 works; `go.mod` pins `toolchain go1.27.1`, which the `go`
command downloads automatically on first use (`GOTOOLCHAIN=auto`, the default):

```powershell
cd services
go run ./cmd/helios-backend --seed dev
```

**Linux/macOS:** the same command. Do **not** run it as root: `initdb`/`postgres` refuse to, and
helios-backend stops with a clear error (use `--db postgres://...` if you must).

The first run downloads the PostgreSQL 18.3 binaries (~15 MB archive, ~60 MB extracted; from Maven Central via
`fergusstrange/embedded-postgres`) into `%LOCALAPPDATA%\helios\pg-bin` (Linux: `~/.cache/helios/pg-bin`,
override with `--pg-cache`), runs `initdb`, applies migrations and generates dev keys (about 6 s on a fast
connection). Later starts take well under a second (0.2–0.5 s measured on Linux, also right after a crash:
the restarted backend reclaims its own orchestrator lease at once; idle RSS of the backend plus PostgreSQL is
about 120 MB). Everything the backend owns lives in the data directory (`--data`, default
`.\saved\backend`):

```
saved/backend/  helios.toml (optional)  pg/  pg-runtime/  nats/  keys/  logs/  backend.lock
```

PostgreSQL holds one schema per service, `svc_identity` and `svc_orch` (05 §1.4, §3). A data directory
created before WP-0.15r has them as `identity` and `orchestrator`: the first start (or `migrate`) renames them
in place, encrypts the stored e-mail addresses with the keys in `keys/` and drops the plain-text columns, so
existing accounts keep working.

Only one helios-backend can own a data directory (an OS file lock; a crashed backend never leaves it
stuck). If a previous backend was killed hard and its PostgreSQL is still running, the next start stops it.
Everything listens on loopback unless you pass `--lan`, so Windows Firewall never prompts.

With `--seed dev` you get `dev1`…`dev10` (log in as `dev1@helios.test` or `dev1#0001`, password `dev`).

```powershell
$login = Invoke-RestMethod -Method Post -ContentType application/json `
  -Uri http://127.0.0.1:7700/helios.identity.v1.Identity/Login -Body '{"login":"dev1#0001","password":"dev"}'
Invoke-RestMethod -Method Post -ContentType application/json -Headers @{Authorization = "Bearer $($login.accessToken)"} `
  -Uri http://127.0.0.1:7700/helios.session.v1.Session/CreateSession -Body '{"zoneId":"1001"}'
```

Stop with Ctrl+C: readiness goes false, the API drains, supervised children are stopped (interrupt, then
kill after their timeout), then NATS, miniredis and PostgreSQL shut down.

### Ports

| Port | What | Flag |
|---|---|---|
| 7700 | public API (launcher, client, tools): identity, session, JWKS | `--http` |
| 7701 | ops: `/metrics`, `/healthz`, `/readyz`, `/debug/pprof/`, orchestrator directory, dev `/admin/*` | `--ops` |
| 7702 | embedded PostgreSQL (loopback) | `--pg-port` |
| 7703 | miniredis (loopback; random per-start password, used only in-process) | `--cache-listen` |
| 4222 | embedded NATS for nats.c cells and gateways (`""` = in-process only); user `fleet`, password in `<data>/keys/nats-fleet.json` | `--nats` |
| 7777/udp | gateway (C++, not part of this module) — the static fallback address in tokens | `session.gateways` |

### Commands and flags

```
helios-backend [run] [flags]                  run (default)
helios-backend migrate [flags]                apply migrations and exit
helios-backend keys rotate [netcode|tickets|jwt|nats] [flags]
helios-backend reset --yes [flags]            delete the data directory
helios-backend version
```

| Flag | Env | Default | |
|---|---|---|---|
| `--data` (`--data-dir`) | `HELIOS_DATA` | `./saved/backend` | data directory |
| `--config` | `HELIOS_CONFIG` | `<data>/helios.toml` if present | TOML file, see `deploy/helios.example.toml` |
| `--env` | `HELIOS_ENV` | `dev` | `prod` disables seeds and `/admin` |
| `--services` | `HELIOS_SERVICES` | `all` | `identity,session,orchestrator` subset |
| `--shard` / `--shard-index` | `HELIOS_SHARD`, `HELIOS_SHARD_INDEX` | `dev` / 0 | NATS subject scope; shard field of block IDs |
| `--db` | `HELIOS_DB` | `embedded` | or `postgres://user:pass@host:5432/db` |
| `--bus` | `HELIOS_BUS` | `embedded` | or `nats://user:password@host:4222` |
| `--cache` | `HELIOS_CACHE` | `miniredis` | or `valkey://host:6379` (`redis://`, `rediss://`) |
| `--http`, `--ops`, `--nats`, `--pg-port`, `--cache-listen` | `HELIOS_HTTP` … | see ports | listen addresses |
| `--tls-cert`, `--tls-key` | `HELIOS_TLS_CERT`, `HELIOS_TLS_KEY` | — | HTTPS (TLS 1.3 only) on the API |
| `--pg-cache` | `HELIOS_PG_CACHE` | user cache dir | PostgreSQL binaries |
| `--keys-dir` | `HELIOS_KEYS_DIR` | `<data>/keys` | key files; with `--env prod` they must already exist (never generated) |
| `--seed` | `HELIOS_SEED` | — | `dev` accounts |
| `--lan` | `HELIOS_LAN` | off | bind API and NATS on all interfaces |
| `--log-level`, `--log-format` | `HELIOS_LOG_LEVEL`, `HELIOS_LOG_FORMAT` | `info`, `text` | `json` for Loki |
| `--traces` | `HELIOS_TRACES` | `none` | `stdout` prints spans (OTLP arrives with the collector) |
| `--spawn` | `HELIOS_SPAWN` | `all` | which `[[orchestrator.spawn]]` entries to supervise |

Everything else (rate limits, TTLs, argon2 cost, zones, supervised processes) is in the TOML file.

### Production-like: external PostgreSQL, NATS and Valkey

```sh
docker compose -f deploy/docker-compose.yml up -d
go run ./cmd/helios-backend --data ./saved/compose \
  --db "postgres://helios:helios@127.0.0.1:5432/helios?sslmode=disable" \
  --bus nats://127.0.0.1:4222 --cache valkey://127.0.0.1:6379
```

`docker compose -f deploy/docker-compose.yml --profile backend up -d --build` also runs the backend image
(`deploy/Dockerfile`, distroless, non-root).

## API

JSON over HTTP with **Connect-protocol shapes**: `POST /<package>.<Service>/<Method>`,
`Content-Type: application/json`, lowerCamelCase fields, 64-bit integers as strings, errors as
`{"code":"unauthenticated","message":"..."}` with the Connect HTTP status (400/401/403/404/409/429/503…,
429 adds `Retry-After`). When schemac starts emitting `.proto` files these become generated connect-go
handlers without changing anything clients see. Authenticated calls take `Authorization: Bearer <accessToken>`.

| Method | Auth | Request → response |
|---|---|---|
| `helios.identity.v1.Identity/Register` | — | `{email, handle, password}` → `{accountId, handle, discriminator, tag}` |
| `…/Login` | — | `{login (email or Handle#1234), password}` → `TokenPair` |
| `…/Refresh` | — | `{refreshToken}` → `TokenPair` (rotated; replaying a used token revokes the family) |
| `…/Logout` | — | `{refreshToken}` → `{}` |
| `…/GetAccount` | bearer | `{}` → `{accountId, email, tag, createdAt, lastLoginAt}` |
| `…/CreateLaunchCode` | bearer | `{}` → `{code, expiresAt}` (60 s, one use; for the launcher → client hand-off) |
| `…/ExchangeLaunchCode` | — | `{code}` → `TokenPair` |
| `GET /.well-known/jwks.json` | — | Ed25519 JWKS (kid = RFC 7638 thumbprint) |
| `helios.session.v1.Session/CreateSession` | bearer | `{characterId?, zoneId?}` → `ConnectInfo` (see below for `characterId`) |
| `…/Reconnect` | — | `{ticket}` → `ConnectInfo` (epoch + 1; the ticket is single-use) |
| `…/EndSession` | bearer | `{}` → `{}` (kicks the session) |

`TokenPair` = `{accountId, tag, tokenType, accessToken, accessExpiresAt, refreshToken, refreshExpiresAt}`.
Account IDs (and every other minted ID) are block IDs, see below.

- **Launch codes** are bound to the refresh-token family behind the caller's access token and only minted
  while that family is live. The exchanged refresh token joins the same family, so logging out of the
  launcher (or reuse detection) also logs out the game client it launched, and an access token alone can
  never be turned into a fresh 30-day login.
- **`characterId`** goes into the token's user data, which gateways and cells trust, so it must belong to
  the caller. Until the character service exists (Phase 1), `env=dev` accepts any positive ID and every
  other environment refuses non-zero IDs (`failed_precondition`).
- **Session IDs** (netcode `client_id`) are random 63-bit values: the session service never mints IDs.
`ConnectInfo` = `{sessionId, sessionEpoch, connectToken (base64 of the 2048-byte netcode token), protocolId,
expiresAt, timeoutSeconds, gateways[], reconnectTicket, reconnectTicketExpiresAt}` — the client passes the
decoded token straight to `netcode_client_connect`.

Ops listener (7701): `/metrics`, `/healthz`, `/readyz` (JSON per dependency), `/debug/pprof/`,
`POST /helios.orchestrator.v1.Orchestrator/{ResolveZone,ListProcesses}`, and with `env=dev`
`POST /admin/identity/{Ban,VerifyAudit}` (the GM API with RBAC replaces these in Phase 1).

### NATS (cells and gateways, nats.c)

JSON payloads until schemac emits Helios-binary codecs. Request headers: `Helios-Deadline-Ms` (remaining
budget), `traceparent`; errors come back as headers `Helios-Error` (a Connect code) and `Helios-Error-Message`.

**Credentials.** The embedded server accepts no anonymous clients. Cells and gateways connect as user
`fleet`; the password is the `"secret"` string of the current key in `<data>/keys/nats-fleet.json` and
supervised children receive `HELIOS_NATS_URL`, `HELIOS_NATS_USER` and `HELIOS_NATS_PASSWORD`. The fleet user
may call services and read KV, but cannot serve `rpc.>` subjects (impersonate a service), publish `ctl.>` or
`evt.>`, write KV or change streams. `keys rotate nats` changes the password (restart the backend and its
children). With an external bus, credentials go in `--bus nats://user:password@host:4222`.

| Subject | Who | Payload |
|---|---|---|
| `rpc.<shard>.orch.RegisterProcess` | cell, gateway | `{name, kind: cell\|gateway, address, zones[], keyId, capacity, pid, version, fd: {az, rack, host}, serverBuild}` → `{processId, epoch, leaseTtlMs, heartbeatIntervalMs, assignments[], idShard, idBlocks[]}` (cells get 2 block prefixes; gateways never mint). `fd` and `serverBuild` are optional and stored in `svc_orch.process` (05 §1.4.3); a bare `host` fills `fd.host` |
| `rpc.<shard>.orch.Heartbeat` | every `heartbeatIntervalMs` | `{processId, epoch, load: {players, freeSlots, tickP99Ms}, held: [{region, leaseGen}]}` → `{leaseExpires, assignments[], mode}`; `failed_precondition "lease_lost"` means: stop acting as owner, register again. `held` (optional, ≤ 256) is what the process holds; the leader records it with the registration |
| `rpc.<shard>.orch.AllocateIdBlocks` | cell minter | `{processId, epoch, n: 1..16}` → `{idShard, prefixes[]}` |
| `rpc.<shard>.orch.Deregister` | on shutdown | `{processId, epoch}` |
| `rpc.<shard>.orch.ResolveZone` | anyone | `{zoneId \| zoneName}` → `{zoneId, zoneName, leaseGen, processId, process, epoch, address}` |
| `rpc.<shard>.session.SealReconnectTickets` | gateway, every 60 s | `{sessions: [{sessionId, zoneId, epoch, ticket}]}` → `{tickets: [{sessionId, ticket, expiresAt}], missing: []}`. `epoch` is the session_epoch the gateway serves (a newer record means: drop your copy); `ticket` is the latest ticket the gateway holds, used only to rebuild a record lost with Valkey, so a Valkey restart does not drop every player |
| `ctl.<shard>.gateway.all.kick` | gateways subscribe | `{sessionId, reason}` (superseded, logout) |
| `ctl.<shard>.gateway.all.session_epoch` | gateways subscribe | `{sessionId, epoch}` — drop your copy if you hold an older epoch |
| `evt.<shard>.orch.{process.up,process.down,zone.changed}` | informational | registry changes |
| KV `DIRECTORY` (`zone.<id>`) | watchers | read projection of `region_lease` (PostgreSQL is the authority; there is no `LEASES` bucket, 05 §2.3) |

**Regions and generations (05 §1.4.2).** `svc_orch.region_lease(region_id, instance_id, holder_proc, lease_gen,
assigned_at)` has one row per region. A v0 zone is its own instance with one region (`Whole`) whose ID is the
zone ID, so `zoneId` names the region in assignments and `held` lists until multi-region zones add a region
field. Every placement bumps the region's `lease_gen` in a term-fenced transaction before the holder is told.

`internal/orchestrator.Agent` is the reference implementation of the register/heartbeat/fence loop. The
**holder rule** (05 §1.4.2): a process never fences itself because it cannot reach the orchestrator, however
long that lasts; it keeps its regions, keeps heartbeating and reporting them, and drops a region only on an
answer from the leader: `lease_lost` (every region), a higher `leaseGen` for the region in a heartbeat reply, or
the region missing from the reply's assignments (`Agent.OnRegionLost` names which). A process silent for the
12 s liveness TTL loses its zones; a gateway that misses three heartbeats gets no new connect tokens.

**Leadership (05 §1.4.1).** Each shard has one orchestrator leader, recorded in `svc_orch.orch_leader`
(10 s lease, renewed every 2 s; the leader acts only while its last renewal is < 8 s old). Every mutating
statement share-locks that row and checks the term, so a deposed leader's writes touch nothing. A second
backend on the same database stays on standby and takes over when the lease expires; a backend restarted on
the same host and data directory reclaims its own lease at once. A new leader starts from an empty registry:
processes are answered `lease_lost` and register again under new epochs and lease generations.

**IDs (05 §1.4.5).** 63-bit block IDs: `0 | 41-bit block prefix (ms since 2026-01-01) | 5-bit shard |
17-bit offset`. `AllocateIdBlocks` advances `svc_orch.id_alloc` (`last_ms = GREATEST(last_ms + n, now_ms)`),
so prefixes never repeat and no process needs a node ID. `pkg/idgen.Minter` holds 2 blocks (4 for battle
cells), refills in the background at half use, retires blocks after an hour and keeps minting from retired
blocks while the control plane is unreachable. Cells mint the same layout in C++ (`testdata/vectors/block_ids.json`).

### Connect tokens, keys and gateways

`pkg/connecttoken` writes netcode 1.02 tokens exactly as `netcode_generate_connect_token` does (layout in the
package doc); the 256-byte user data carries the Helios layout v1 (`userdata.go`: account, character,
session epoch, content build, zone, placement ticket, entitlements, attestation hash). Key files in
`<data>/keys` are JSON keyrings (`{"version":1,"keys":[{"id":1,"secret":"<base64 32 bytes>",…}]}`):

- `netcode-shard.json` — gateways load the same file (`HELIOS_NETCODE_KEYS` is passed to supervised
  children), use one generation and report its `id` as `keyId` when registering. `keys rotate netcode` adds a
  generation; new tokens go only to gateways on the newest key that has capacity, older ones drain (05 §1.3).
- Gateways must set netcode's `max_connect_token_lifetime` to `session.token_expiry` (default 30 s,
  `HELIOS_TOKEN_LIFETIME` for children) or they refuse fresh tokens right after they start.
- `reconnect-ticket.json`, `jwt-ed25519.json` — backend-only.
- `subject-kek.json`, `email-bidx-pepper.json` — Identity's PII keys (05 §6.6). Each account's 256-bit data
  key (DEK) is wrapped by the current KEK generation (XChaCha20-Poly1305, bound to the account's row) in
  `svc_identity.subject_key`; the address is sealed under the DEK in `account.email_ct` (AAD = table, column,
  account ID), and `account.email_bidx` = HMAC-SHA256(pepper, trimmed lower-case address) is the unique
  login key. Without these files no address can be read or looked up, and deleting an account's DEK shreds its
  PII. There is no `keys rotate` for them yet: a KEK generation may be dropped only after every DEK it wraps is
  re-wrapped, and the pepper rotates only with a re-index. Staging and prod mount them from the secret store
  until KMS holds them (05 §6.5).
- Dev generates missing key files; `--env prod` refuses to (a silently generated shard key would make every
  gateway reject tokens) and expects them in `--keys-dir`, mounted from the secret store.

## Tests

```sh
go build ./... && go vet ./... && go test ./...           # fast, no network, no C compiler
HELIOS_NETCODE_INTEROP=1 go test ./pkg/connecttoken/       # + builds a C harness against third_party/netcode
go test -tags integration ./internal/integration/ -v       # boots the whole stack on embedded PostgreSQL
go test -run 'TestConformance/holder_rule' ./internal/orchestrator/   # CONF-03's test, 60 s (3 s with -short)
```

- **Golden vectors** in `testdata/vectors/` are shared with the C++ side: `netcode_token_fixed.json` (tokens
  written by netcode's own C writer from fixed inputs; Go must reproduce them byte for byte),
  `netcode_token_api.json` (a token from `netcode_generate_connect_token` that Go must decrypt) and
  `block_ids.json` (ID layout and the allocation rule). Regenerate the netcode ones with the harness in
  `pkg/connecttoken/testdata/interop/netcode_interop.c` (`gen-fixed`, `gen-api`); the interop test fails if
  the C library stops reproducing them.
- **C interop** (`HELIOS_NETCODE_INTEROP=1`): netcode parses and decrypts Go tokens, completes a real
  client/server handshake with them, and rejects a token for an address it does not list.
- **Integration** (`-tags integration`): needs network on first use (PostgreSQL binaries; cached afterwards,
  `HELIOS_PG_CACHE` to choose where) and a non-root user (as root every test skips; in a root-only
  container build the binary with `go test -c -tags integration -o it.test ./internal/integration/` and run it
  from `internal/integration/` as an unprivileged user). It runs register → login → CreateSession → token
  validation (Go, plus netcode C with `HELIOS_NETCODE_INTEROP=1`), authenticated gateway/cell registration
  over TCP NATS, cell ID blocks, sealed tickets and Reconnect, PostgreSQL leadership (a second orchestrator
  stays on standby, stale-term writes are refused), launch-code families, ban kicks, refresh reuse detection,
  the PostgreSQL audit chain and the store conformance suites against real PostgreSQL (including races of
  revocation against rotation). It also checks the schema the plan requires (only `svc_*` schemas; the only
  e-mail columns are `email_ct` and `email_bidx`; leases in `region_lease`), the upgrade of a database built by
  the pre-WP-0.15r migrations (rename, encryption of stored addresses, generations carried into `region_lease`)
  and concurrent first migrations. `HELIOS_TEST_LOG=1` shows backend logs.
- **Conformance** (`TestConformance/<name>`, the test IDs `conformance/<name>` of 09 §5.10): `holder_rule`
  (CONF-03) keeps the control plane unreachable for 60 s (no responders, timeouts, `unavailable` answers) and
  requires the Agent to keep every region, then to drop exactly the region whose generation rose.
- Fuzzing: `go test -fuzz FuzzParse ./pkg/connecttoken/`.

## Layout

```
cmd/helios-backend/        main: run, migrate, keys rotate, reset, version
internal/app/              Service lifecycle + ordered runner
internal/backend/          wiring used by main and the integration test
internal/platform/         config, logging, telemetry, health, metrics, HTTP servers
internal/stack/            embedded/external PostgreSQL, NATS, Valkey; data-dir lock
internal/identity/         identity service, stores (PostgreSQL, memory), storetest suite
internal/session/          session service, Valkey store, reconnect tickets
internal/orchestrator/     registry, stores, NATS service, Agent, supervisor
internal/integration/      integration tests (build tag "integration")
pkg/connecttoken/          netcode 1.02 codec + Helios user data (+ cinterop test helper)
pkg/authn/                 EdDSA JWT issue/verify, JWKS, bearer middleware
pkg/ratelimit/             GCRA buckets in Valkey (one Lua script)
pkg/rpc/                   Connect-style JSON over HTTP, JSON request/reply over NATS, error codes
pkg/pii/                   DEKs, KEK wrapping, field sealing (XChaCha20-Poly1305) and blind indexes (05 §6.6)
pkg/idgen/ pkg/keyring/ pkg/clock/ pkg/testkit/
migrations/<service>/      goose migrations for svc_<service>, embedded (plus Go steps in migrations.go)
deploy/                    docker-compose.yml, Dockerfile, helios.example.toml
testdata/vectors/          golden vectors shared with C++
```

## Not yet (by design, see 05 §9)

Login queue, character/ledger/market and the other Phase 1+ services; connect-go/protobuf stubs (waiting for
schemac); OIDC/MFA; multi-region zones, instances, zone leaders and handle blocks; failure detection that uses
`fd` and held regions, and control-plane degraded mode (Phase 2); KEK re-wrapping, erasure and DSAR (Phase 3);
per-role NATS users with narrow subject permissions; PostgreSQL roles per service; the transactional outbox and
JetStream `EVT` stream; OTLP exporters.

## Plan conformance

Plan-Rev: 6

Written to draft v1 (plan revision 1), reworked to revision 2's lease and ID design, and brought to revision 6 by
WP-0.15r (09 §5.10.4 (a)): the `svc_identity` and `svc_orch` schema names, e-mail as `email_ct` and
`email_bidx` under per-account DEKs, `region_lease`, `fd` and `serverBuild` in registration, and held regions in
heartbeats. Revisions 4–6 added no delta (§5.10.4 (a), (c)). Known gap outside §5.10.4 (a): IP addresses,
which 05 §6.6 classes as direct PII, are still stored in plain text in `svc_identity.refresh_token.client_ip`
and `svc_identity.audit_log.client_ip`; CONF-07 will flag them.
