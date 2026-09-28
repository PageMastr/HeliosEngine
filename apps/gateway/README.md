# helios-gateway

The public game edge (04 §1, §2.3–2.6, §9): the only process clients talk to. It validates
Session-service connect tokens (netcode, UDP 7777), routes each session to the cell that owns its
zone (`ResolveZone` over NATS, or static routes), forwards messages both ways over HTP trunks,
evicts a session when the Session service announces a newer `session_epoch`, delivers reconnect
tickets every 60 s and enforces the 04 §9 limits. The runtime is `helios::server::GatewayServer`
(`engine/server`); this directory is only the executable.

```
helios-gateway [--listen 127.0.0.1:7777] [--public ip:port]... [--max-clients 256]
               [--keys netcode-shard.json] [--protocol-id 0x48454c494f530001] [--token-lifetime 45]
               [--nats nats://127.0.0.1:4222] [--backend-data services/saved/backend] [--shard dev]
               [--standalone --cell 127.0.0.1:7810 --route zoneId@ip:port...] [--default-zone tallis]
               [--trunk-keys keyring.json] [--ticket-interval 60] [--dev-insecure-keys] [--help]
               [--fd-az az] [--fd-rack rack] [--fd-host host] [--server-build n]
helios-gateway probe [--connect 127.0.0.1:7777] [--token <base64 connect token>] [--zone id]
               [--echoes 10] [--seconds 5] [--keys ... | --dev-insecure-keys]
```

| Setting | Where from |
|---|---|
| Shard key | `--keys` / `HELIOS_NETCODE_KEYS` / `<backend-data>/keys/netcode-shard.json` (the newest generation; its id is reported as `keyId`, so the Session service lists this gateway in tokens) |
| Protocol id, token lifetime | `HELIOS_PROTOCOL_ID`, `HELIOS_TOKEN_LIFETIME` (the backend passes both to supervised children; set `--token-lifetime` to the backend's `session.token_expiry`, 30 s by default). The protocol id is read exactly as the backend reads `session.protocol_id` (Go `strconv.ParseUint(s, 0, 64)`: `0x…` hex, `0o…`/leading-`0` octal, `0b…`, else decimal), so bare hex without `0x` is refused rather than misread |
| Trunk sockets | bound to loopback when the cell is on loopback (no Windows Firewall prompt on a dev box), else to the wildcard of the cell's address family; a trunk no session uses is closed after 10 min (a failed one after 30 s) instead of being retried forever |
| NATS | as for `helios-cell` |
| Failure domain, server build | as for `helios-cell` (`--fd-*`, `--server-build`, `HELIOS_FD_*`, `HELIOS_SERVER_BUILD`) |

`probe` is a minimal client: with `--token` it uses a token from `Session/CreateSession`
(`connectToken`), otherwise it mints one with the shard key. It prints the Welcome, echo round
trips, tick states, tickets and kicks, and exits non-zero if it was not routed or lost echoes. Run
on Windows against a Linux gateway it is the NS-0.3 interop check by hand; `server_tests_ns03_remote`
(`engine/server/README.md`, Acceptance) is the same check as a CTest, which refuses a target on the
probe's own host. Walkthrough:
`engine/server/README.md`, "Run it locally".

## Plan conformance

Plan-Rev: 11

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 9 by WP-0.14's PR on
2026-09-27: revisions 7–9 changed 06 §1.2, 02 §7.4 with 04 §10.2 and 09 §5.2a, none of which this
executable implements. No conformance delta is open; see §5.10.4 (c) there.

Revision 10 (PR #14: ADR-004a §6–7 for `engine/ecs`, plus status rows) changes no anchor that
maps here (checked 2026-09-27 against CONSISTENCY's revision-10 rows).

Revision 11 (PR #21, the Director's 04 §3.2 Plan-Change: the TiDi load is measured against the
dilated tick interval) maps here through D2's fallback: 04 §3.2 has no map entry, and WP-0.14's Own
column cites 04 §3. This executable runs `engine/server`'s gateway, which only copies the dilation a
cell reports into the client welcome (the probe mode prints it); the load formula is
`engine/authority`'s `TiDiController`, which already matched the new text. Re-checked on 2026-09-27
with no delta. D7 keeps the earlier revision while a delta is open; none is, so the re-check records
11, as `engine/authority`'s README does.
