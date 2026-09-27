# helios-cell

The headless cell server (04 §1, §3): hosts zone instances on dilatable clocks (TiDi), accepts HTP
trunks from gateways and, given a NATS URL, registers with the orchestrator, heartbeats at 1 Hz,
obeys the lease holder rule and hosts exactly the zones it is assigned. The runtime is
`helios::server::CellServer` (`engine/server`); this directory is only the executable.

```
helios-cell [--zone tallis[:1002]]... [--tick-hz 20] [--zone-hz name:hz]... [--standalone]
            [--trunk 127.0.0.1:7810] [--trunk-public ip:port] [--trunk-keys keyring.json]
            [--nats nats://127.0.0.1:4222] [--backend-data services/saved/backend] [--shard dev]
            [--name cell-1] [--workers n] [--dev-insecure-keys] [--log-level info] [--help]
            [--fd-az az] [--fd-rack rack] [--fd-host host] [--server-build n]
```

| Setting | Where from |
|---|---|
| NATS | `--nats` or `HELIOS_NATS_URL`; user/password `HELIOS_NATS_USER` / `HELIOS_NATS_PASSWORD`, else user `fleet` with the password from `<backend-data>/keys/nats-fleet.json`. Without a URL (or with `--standalone`) the cell hosts its `--zone` list at lease generation 0 |
| Trunk key | `--trunk-keys` / `HELIOS_TRUNK_KEYS`; in dev it falls back to the shard keyring (`HELIOS_NETCODE_KEYS`, `<backend-data>/keys/netcode-shard.json`); `--dev-insecure-keys` for loopback-only runs without the backend |
| Failure domain, server build | `--fd-az`, `--fd-rack`, `--fd-host`, `--server-build` or `HELIOS_FD_AZ`, `HELIOS_FD_RACK`, `HELIOS_FD_HOST`, `HELIOS_SERVER_BUILD`, from the placer; sent with RegisterProcess (05 §1.4) and left out when unset. A value the orchestrator would refuse stops the cell at start |
| Zones | orchestrated: `--zone` names are declared to the orchestrator (none = any zone); standalone: `name:id` pairs (default `tallis:1002`) |

Ctrl+C releases the zones, tells connected gateways (`ZoneFenced`) and deregisters. Crash reports go
to `--crash-dir` (default `saved/crashes`). How to run it with the backend and a gateway on Windows:
`engine/server/README.md`, "Run it locally".

## Plan conformance

Plan-Rev: 9

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 9 by WP-0.14's PR on
2026-09-27: revisions 7–9 changed 06 §1.2, 02 §7.4 with 04 §10.2 and 09 §5.2a, none of which this
executable implements. No conformance delta is open; see §5.10.4 (c) there.
