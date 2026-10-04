# tools/conformance — the plan-conformance lint (WP-0.2)

Plan-Rev: 11

`helios-conformance` checks that code follows the plan's normative decisions: the CONF rules of
[09 §5.10.3](../../docs/plan/09-roadmap-and-process.md), against the anchor map of §5.10.2 D2. It is a Go
tool (§5.10.3) with no dependencies outside the standard library, so it builds offline wherever the
backend's Go 1.27 toolchain (ADR-014) is installed.

```
cd tools/conformance
go run ./cmd/helios-conformance -root ../..                  # the repository, known-failing records applied
go run ./cmd/helios-conformance -root ../.. -strict          # every finding fails (proves a rework closed)
go run ./cmd/helios-conformance -root ../.. -rules CONF-09   # one rule
go run ./cmd/helios-conformance -root ../.. -sarif out.sarif # also write SARIF 2.1.0
go run ./cmd/helios-conformance -list                        # the rules, anchors and scopes
go test ./...                                                # every seeded fixture, exactly
```

It reads the files git tracks or would track (`ls-files --cached --others --exclude-standard`), so
working-tree code that awaits its WP counts (D7) and ignored build output does not. Vendored code under
`third_party/` is skipped except `third_party/CMakeLists.txt` and `third_party/*/patches/**`, and so are the
lint's own fixtures (`testdata/`). Each finding prints as `path:line: CONF-nn: message`; exit code 1 means a
finding fails the run, 2 a usage or I/O error.

## Where it runs

- **CTest** (label `lint`, `CMakeLists.txt` here): `lint_conformance` over the repository,
  `lint_conformance_unit` (`go test`), and one `lint_conformance_fixture_<rule>_<case>` per seeded tree. Go is
  optional locally (the tests are then not registered, and configure says so) and required in CI.
- **`cmake -P tools/ci/run_lints.cmake`**, with the other build-independent lints.
- **CI**: the `Conformance lint (tools/conformance)` job vets and tests the tool, runs it over the full tree
  (a superset of §5.10.3's "changed paths"; the known-failing records make that possible), and uploads the
  SARIF as an artifact and to code scanning (category `helios-conformance`).

## Rules

A rule's scope is its §5.10.3 Scope column plus the paths of every map entry that names it (D2). Every rule
has a seeded violation under `testdata/<rule>/bad/` whose `expect.txt` lists the exact findings, and most have
a `good/` tree for their exemptions; `go test` compares both exactly, and CTest runs each through the CLI.

| Rule | Anchor | What fails it | Kind | On the tree |
|---|---|---|---|---|
| CONF-01 | 05 §2.3, §1.4 | a JetStream KV bucket named `LEASES` or matching `(?i)lease\|leader\|fence` created, bound or read: Go `CreateKeyValue`, `CreateOrUpdateKeyValue`, `UpdateKeyValue`, `KeyValue` and `KeyValueConfig{Bucket: …}` (names resolved through constants, across packages), a `cfg.Bucket = …` assignment and a raw `$KV.<bucket>.` subject; nats.c `js_KeyValue`, `js_CreateKeyValue`/`js_UpdateKeyValue` and `kvConfig.Bucket`, with calls read across lines and names resolved through the scope's C and C++ string constants (`constexpr`/`const char* const`, `char[]`, `std::string(_view)`, `#define`). A nats.c bucket the lint cannot resolve fails closed, and so does a Go one in NATS code (a file that imports nats.go or its `jetstream` package): a read projection bound through a variable carries a `conformance:allow`. A test that asserts the bucket is absent (`if _, err := js.KeyValue(…); err == nil { t.Fatal… }`, or the assignment then that `if`) is exempt | Go syntax; C token scan | passes |
| CONF-02 | 05 §1.4.1–1.4.2 | `TTL`, `LimitMarkerTTL` or `MaxAge` on a lease-named KV bucket or KV stream, in the config literal (also an element of a slice or map literal) or assigned after it (`cfg.TTL = …`); a per-key TTL (`jetstream.KeyTTL`) on a lease key or on a key the lint cannot resolve; KV compare-and-set (`Create`, or `Update` with a revision, in jetstream's and nats.go's legacy API) on a key matching `lease\|leader\|fence\|elect\|term\|lock`, or, in NATS code, on a key the lint cannot resolve; a TTL on a KV bucket the lint cannot resolve, in NATS code; the nats.c forms (`cfg->TTL` or `LimitMarkerTTL` on a lease bucket or one the lint cannot resolve, also in a nats.c file that sets no `Bucket`; a stream's `MaxAge` in a file that names a lease bucket's stream, `KV_leases`; `kvStore_Create`/`kvStore_Update` and their `String` and `WithTTL` variants on a leader-like key or one the lint cannot resolve) | Go syntax; C token scan | passes |
| CONF-03 | 05 §1.4.2 (holder rule) | the required test `conformance/holder_rule` (and any the map's `tests` add) missing from a language of the scope: Go needs `t.Run("holder_rule", …)` inside `func TestConformance`, C++ a `TEST_CASE("conformance/holder_rule…")` outside `#if 0`. It also fails a required test that is switched off: an unconditional `t.Skip` in `TestConformance` or the required case, a `//go:build` line on the file that defines `TestConformance`, or a C++ case marked `doctest::skip`, `may_fail`, `should_fail` or `expected_failures` anywhere in its decorator chain (the case's argument is read to its closing parenthesis, so decorators with nested calls, strings and character literals do not end it; a conditional `doctest::skip(cond)` fails too). CI runs the tests (Go `services` job, C++ `server_tests`) | test presence | passes |
| CONF-04 | ADR-004, 05 §1.4.5 | in ID code (a file that names `idgen`, `AllocateIdBlocks`, a minter, `composeBlockId`, `BlockIdLayout` or Snowflake, or whose file name has `id`, `ids`, `idgen`, `entity_id`, `snowflake` or `minter` as a `_`-delimited word: `block_ids.cpp` is ID code, `grid.cpp` is not), an identifier or config key for a node, worker, machine or datacenter ID (camelCase split, so `workerID` counts and a task graph's `NodeId` elsewhere does not); anywhere in scope, the Snowflake 41/10/12 layout (`<< 22` with `<< 12`), the retired 41/5/8/9 layout (`<< 22`, `<< 17`, `<< 9`), and `<< 22` (the time prefix) outside `pkg/idgen` and `engine/ecs`'s `entity_id.*`/`registry.*`. Shift amounts, parenthesized ones too (`<< (kOffBits + kShBits)`), are evaluated through constants (Go across packages; C++ `constexpr` and `const` declarations, `#define`s and enumerators across the scope); a C++ left operand may be brace-initialized (`u64{prefix} << 22`, the codebase's widening idiom); a literal left operand (`1 << 12`, also converted or brace-initialized: `uint64(1) << 12`, `u64{1} << 12`) is a size, not a field, unless it scales a field (`ms * (1 << 22)`, also `ms * uint64(1 << 22)` and `ms * (u64{1} << 22)`; `4 * (1 << 22)` is a size). Node-ID keys in `services/**/*.toml` count too | Go syntax; C token scan | passes |
| CONF-05 | 05 §1.4.5 (who mints) | an import of `…/pkg/idgen`, or a call of `AllocateIdBlocks`, outside `services/internal/{identity, character, ledger, market, industry, mail, worldstate, world, activity, lifecycle, orchestrator, backend}`, `pkg/idgen` itself, `_test.go` files and test-helper packages (`testkit`, `testdata`, `testutil`, and `<name>test` for the store, db, nats and pg helpers and the minter packages; a name that only ends in "test", such as `latest`, is not one) | Go imports and calls | passes |
| CONF-06 | 05 §1.4, §3 | in the **net schema** (below), a schema not named `svc_<service>` (`CREATE SCHEMA`, `ALTER SCHEMA … RENAME TO`, or a `Name` in `migrations.Schemas`); a table created in, or moved (`SET SCHEMA`) to, a schema other than its service's, or created unqualified; a statement the evaluator cannot follow (fails closed, below) | SQL evaluation | passes |
| CONF-07 | 05 §3, §6.6 (Phase 0 rule) | in the net schema, a column whose name's words are an e-mail, date of birth, IP address or real name (`email`, `email_norm`, `dob`, `date_of_birth`, `birthday`, `ip`, `ip_addr`, `client_ip`, `remote_addr`, `real_name`, `full_name`, `first_name`, `last_name`, `legal_name`, `given_name`, `family_name`, `surname`, …) or whose type is `INET`/`CIDR` (or a domain over one), unless it is `*_ct` or `*_bidx`; any such column outside `svc_identity`; a statement the evaluator cannot follow (fails closed, below); an `@pii` attribute in a `.hschema` package other than `identity` or `identity.*` | SQL evaluation; schema scan | passes |
| CONF-08 | 04 §2; reconciliation #12 | a default gateway address or port other than 7777: Go values named for the gateway (keyed fields, var and const specs, assignments, calls with a `"gateway"` argument), with address strings, `net.JoinHostPort`, `fmt.Sprintf` and `fmt.Sprint` addresses and `"host:" + strconv.Itoa(port)` (also `host + ":" + port` with a host variable) evaluated, the `Port` field of such a value (`&net.UDPAddr{Port: …}`), a literal of a gateway-named type under any name (`GatewayConfig{Port: …, Addr: …}`, also as an elided slice or map element: `[]GatewayConfig{{Port: …}}`), and a value named for the gateway port (the words gateway and port: `…GatewayPort`, `"gateway-port"`) evaluated through constants; TOML keys (bare or quoted) or tables named for the gateway (address strings, and the integer of a key with the word port: `[gateway] port`, `listen_port`, `gateway_port`, not `transport`; an inline table's keys too: `listen = { host = …, port = … }`; a multi-line array is read whole, on its key's line); a published `…/udp` (or `/UDP`) port in YAML (the host side of `[ip:]published:container/udp`, and the container side too); C++ `k…GatewayPort` constants (initialised with `=`, `{…}` or `(…)`) and `#define …GATEWAY_PORT`, and in files named for the gateway the `listen`/`connect` defaults (`"host:port"`, `"host:" + std::to_string(port)`, `std::string{"host:"} + …`, `os << "host:" << port`, `Address::ipv4(…, port)`, `ipv4(octets, port)`, `ipv6(groups, port)`, `ipv6Bytes(bytes, port)`, `loopbackV4(port)`, an option's default argument, an address or an integer; also on the continuation lines of a statement that such a line starts: `net::Address listen =` then `Address::ipv4(…);`), resolved through the scope's constants. A gateway port the rule reads but cannot resolve fails closed. Tests and fuzzers are skipped: they choose their own ports | Go syntax; TOML, YAML and C scans | passes |
| CONF-09 | ADR-014 | a `go.mod` `go` directive other than 1.27.x, a `toolchain` other than go1.27.x, or no `go` directive; an `actions/setup-go` step (block or flow style) without `go-version-file: services/go.mod`, or with `go-version`; a `GOTOOLCHAIN` set in workflow YAML (an `env` key, `GOTOOLCHAIN=…` in a script or `$GITHUB_ENV`) to anything but `auto`, `local`, `path` or go1.27.x | go.mod and workflow YAML lines | passes |
| CONF-10 | 08 §1.16; reconciliation #19 | `SDL_CreateRenderer` (and SDL3's other renderer constructors: `SDL_CreateRenderer*`, `SDL_CreateWindowAndRenderer`, `SDL_CreateSoftwareRenderer`, `SDL_CreateGPURenderer`) in C-family code (C++20 module units and `.tpp` too), including by name in a string or split by a backslash-newline splice, outside `apps/launcher/**` and engine/ui's SDL_Renderer backend (`engine/ui/**` paths containing `sdl_renderer`; WP-0.17 names the real files). `#if 0` groups are not read | comment-aware token scan | passes |

## The net schema (CONF-06, CONF-07)

The schema rules judge what a database holds after every migration, not each file alone (09 §5.10.4 (a)). Service
directories `services/migrations/<dir>/` are applied in `migrations.Schemas` order, as `migrations.Up` applies them
(a directory it does not list comes after, by name), and the `-- +goose Up` sections of each one's files in version
order: `CREATE SCHEMA` (with an optional `AUTHORIZATION`), `ALTER SCHEMA … RENAME TO`, `CREATE TABLE` (a `LIKE`
element or an `INHERITS` clause copies the columns its source has at that point; `IF NOT EXISTS` of a table that
exists keeps it), `ALTER TABLE` (`ADD`, `DROP` and `RENAME` of columns, `ALTER COLUMN … TYPE`, `RENAME TO`, `SET
SCHEMA`, also after `ONLY` or the descendants marker `*`; `ADD COLUMN IF NOT EXISTS` of a column that exists keeps
it and its type), `DROP TABLE` and `CREATE DOMAIN` (a column of a domain over `INET` or `CIDR` has that type).
`EXPLAIN ANALYZE` runs its statement, which is read as if it stood alone; `EXPLAIN` without `ANALYZE` runs nothing.
ALTER TABLE actions that change no column are read and pass: constraints (`ADD
CONSTRAINT`/`PRIMARY`/`UNIQUE`/`CHECK`/`FOREIGN`/`EXCLUDE`, `DROP`, `VALIDATE`, `ALTER` and `RENAME CONSTRAINT`),
`ALTER [COLUMN] c SET/DROP/RESET/ADD …`, `OWNER TO`, `ENABLE`/`DISABLE`, `[NO] FORCE ROW LEVEL SECURITY`, `REPLICA
IDENTITY`, `CLUSTER ON`, `SET WITHOUT CLUSTER`/`LOGGED`/`UNLOGGED`/`ACCESS METHOD`/`TABLESPACE`, `SET (…)`, `RESET
(…)` and `ATTACH`/`DETACH PARTITION`. Comments, string literals, function bodies and `Down` sections are not
statements of the net schema. A statement the evaluator cannot follow fails closed, under CONF-06 and CONF-07 both
("the net schema cannot be evaluated"): `CREATE TABLE … AS` (also with a column list: `CREATE TABLE t (a, b) AS
SELECT …`), `PARTITION OF` or `OF type`, `CREATE SCHEMA` with schema elements (`CREATE SCHEMA s CREATE TABLE t
(…)`), `IMPORT FOREIGN SCHEMA`, a materialized view, `SELECT … INTO`, a `DO` block or a `CALL` in an `Up` section
(the body runs with the migration and is not read), any other `ALTER TABLE` action (`OF type`, `NOT OF`, …), `LIKE`
or `INHERITS` of a table no earlier statement creates, `ALTER TABLE` of such a table, `RENAME COLUMN` or `ALTER
COLUMN … TYPE` of a column the table does not have, `ALTER TABLE … INHERIT`, and `-- +goose ENVSUB ON` with the
`${…}` names it substitutes. Annotations are read as goose v3 reads them (`-- +goose down`, `--+goose Up`: any case
and spacing), and statements are split as PostgreSQL lexes them: nested `/* */` comments, strings that span lines,
`E'…'` backslash escapes, `"…"` identifiers, `$tag$` bodies whose tag has digits, and a `$` inside an identifier
(`a$b$`). An annotation with leading whitespace, which goose rejects, still counts. A service's schema is the `Name`
its `migrations.Schemas` entry gives (`services/migrations/migrations.go`), or `svc_<dir>`. The legacy rename that
WP-0.15r declares there (`Legacy`, `LegacyVersion`) applies to the files up to `LegacyVersion`: their
`identity.account` is `svc_identity.account`, since `migrations.Up` renames the schema after them, and a later file
that still says `identity.` is misplaced. So `identity/00001`'s plain-text `email`, which `00004` drops, is not a
finding.

## Suppressions, known-failing records and the map

- **Suppression** (§5.10.3): a `conformance:allow CONF-nn <reason>` comment on the finding's line. The reviewer
  must approve it. Every suppression is printed on each run, so the round audit lists them. One without a rule
  ID or a reason is malformed, and one on a line where its rule reports nothing is unused: both fail.
- **Known failing** (`known_failing.jsonc`): findings of a rule under the record's paths are reported as
  "known failing, owned by `<WP>`" and do not fail the run. Each record names its rework WP, its §5.10.4 row
  and a reason, and the scorecard carries the same clause as a gap (`EXIT-0.conformance`): the run fails on a
  record whose rule and owner no gap of that item names (when `scorecard.jsonc` exists, as it does in the
  repository). A record covers its own paths only, so the same rule's finding anywhere else still fails. A
  record that matches no finding fails the run, so the WP that closes it must remove it. `-strict` ignores the
  file.
- **Map** (`map.jsonc`): anchors to paths, rules and required tests. The run fails on a key that is not an
  anchor, an unknown rule, a bad glob, an entry with neither rules nor `why` (D5), and a rule in no entry.

## Deviation from 09 §5.10.3: syntactic scanners (accepted for Phase 0)

§5.10.3 names `go/analysis` passes for Go and ast-grep patterns for C++. This tool uses neither. The Director
accepted that for Phase 0 (WP-0.2 review, 2026-09-30) with each rule's limits written down below, and
§5.10.3 records it (Plan-Change: 09 §5.10.3, in PR #26).

- **C and C++**: ast-grep is a Rust binary that is not vendored, and the Go tree-sitter bindings need CGO, which
  ADR-014 rules out. The C-family rules use a comment- and literal-aware scanner (raw strings and digit
  separators included, macros not expanded).
- **Go**: `golang.org/x/tools/go/analysis` drivers type-check the packages, which needs the backend's whole
  module graph downloaded and code that compiles. The round audit runs the lint over working-tree code that may
  not compile yet (D7), and CTest runs it offline on every job, so the Go rules parse with the standard
  library's `go/parser` and resolve the constants they need themselves. `go/parser` ignores build tags, so a
  tagged file is still read.

The rules are the same either way; a port to `go/analysis` and ast-grep would change no fixture.

### Limits of each rule

What each rule does not see. Unless an item says otherwise it is a false negative, so a change in that form
needs the reviewer's eye. Over-reporting is called out where the scanner errs that way.

- **CONF-01 and CONF-02, Go**, match the jetstream and nats.go method names (`KeyValue`, `CreateKeyValue`, `Create`,
  `Update`, `KeyTTL`, …) without type information, which over-reports a same-named method of another type. Bucket
  and key names resolve through constants across packages, never through variables (`name := "leases"`,
  `fmt.Sprintf`). In NATS code (a file that imports `github.com/nats-io/nats.go` or its `jetstream` package), a
  bucket or compare-and-set key that does not resolve fails closed, in the call shapes of the API the file imports:
  jetstream's `KeyValue(ctx, b)`, `CreateKeyValue(ctx, cfg)`, `Create(ctx, k, v, …)` and `Update(ctx, k, v, rev)`,
  the legacy `KeyValue(b)`, `CreateKeyValue(cfg)`, `Create(k, v)` and `Update(k, v, rev)`, a `KeyValueConfig{Bucket:
  …}` and a `cfg.Bucket = …`. A config variable passed to a bind call is checked where this file sets its bucket (a
  `KeyValueConfig` literal with a `Bucket`, or a `.Bucket` assignment, matched by name); otherwise it fails closed.
  A `KeyTTL` key that does not resolve fails closed in any file. A TTL field assigned after the literal (`cfg.TTL =
  …`) is read on a config this file names by its type (a variable, a parameter or a struct field of `KeyValueConfig`
  or `StreamConfig`, one set from `new(T)`, and the elements of a slice, array or map of them: `cfgs[i]`, a range
  variable, `c := cfgs[i]`; matched by name), with the buckets the file sets on it (a collection's elements share
  theirs); in NATS code a KV config whose bucket does not resolve, or is not set in the file, fails closed. Not
  seen: a TTL assigned to a config reached another way (a call's result, a variable of another inferred type, a
  nested collection), and a KV call in a file that reaches nats.go only through a wrapper package of its own, with a
  bucket or key the lint cannot resolve (a resolved lease or leader name is still reported). Names are looked up
  without scopes, so a parameter or local that shadows a package-level constant resolves to that constant; this
  matters only where an unresolved name would fail closed. A `$KV.` subject is read only as one literal.
- **CONF-01 and CONF-02, C and C++**, resolve buckets and keys through the scope's string constants by bare name (a
  name with several values matches if any value does). A string constant is a `#define` or the declaration of a
  constant initialized with literals, at namespace or block scope or as a `static` class member: `constexpr`, or a
  top-level `const` object (`const std::string k = …`, `static const char k[] = …`, `const char* const k = …`).
  Anything else fails closed: a parameter and its default argument (a declarator inside parentheses), a non-static
  data member (its initializer is a default a constructor overrides), a variable (a `const char* k` can be
  re-pointed, and a `const` inside template arguments, as in `std::span<const char> k`, is not top-level), a member
  access (`o.bucket`, `p->bucket`) or a call. `#if 0` groups are not read. Remaining limit: a parameter or local
  with the same bare name as a string constant declared elsewhere in scope resolves to that constant (constants
  are `kPascalCase`, so such a collision is unlikely). Not seen: a nats.c call made through a macro or a function
  pointer. A `js_CreateKeyValue` whose `kvConfig` is filled in another file is reported as unresolved. A nats.c
  file is one that includes `nats.h` (`<nats.h>` or `<nats/nats.h>`) or names `kvConfig`; a TTL in one that sets no
  `Bucket` fails closed. A `TTL` or `LimitMarkerTTL` is attributed to every bucket its file sets, not to the
  config it is set on: a file whose one function sets `cfg->Bucket = "DIRECTORY"` and whose other sets
  `cfg->TTL` on a config its caller names passes (CONF-01 still reports a lease or unresolved `Bucket` wherever it
  is set, and its scope covers CONF-02's). `MaxAge` is a `jsStreamConfig` field: it counts in a file that sets a
  stream `Name` resolving to a lease-named stream (`KV_leases`); with a stream name the lint cannot resolve it
  is ordinary retention, as on the Go side.
- **CONF-03** checks that the required tests exist and are not switched off. It cannot tell a test that passes
  vacuously, and it leaves a conditional skip (`if testing.Short()`) to CI, which runs the Go jobs without
  `-short`. A decorator on the enclosing `TEST_SUITE` (`TEST_SUITE("x" * doctest::skip())`) is not read.
- **CONF-04** evaluates shift amounts through constants (Go across packages; C++ `constexpr` and `const`
  declarations, `#define`s and enumerators across the scope) and `+`. A C++ name defined more than once in scope
  counts with each of its values; values of 64 and up are not shift amounts and are dropped, so no bound on the
  combinations can lose one that is. C++ `#if 0` groups are not read. Not seen: a shift amount in a local variable
  (`shift := 22`), `iota`, and a layout built with arithmetic other than `<<` and `* (1 << n)`. "ID code" is
  recognised by file name and keywords; a node-ID identifier elsewhere is not read. Over-reports: a C++ literal
  converted by a cast or a functional cast (`static_cast<u64>(1) << 22`, `u64(1) << 22`) is read as a field, not a
  size (write `u64{1}` or `1ull`).
- **CONF-05** reads direct imports and calls; a package that re-exports `idgen` under another name is not followed.
- **CONF-06 and CONF-07** evaluate the `-- +goose Up` SQL of each service. Not seen: DDL that a Go migration step
  runs (`ExecContext` in `services/migrations/*.go`), statements built in Go strings, and DDL that a function the
  migration calls runs (`SELECT f()`, or a trigger; `DO` blocks and `CALL` fail closed). `INHERITS` copies the
  parent's columns once, so a column the parent drops later stays on the child (an over-report) and one it adds
  later is judged on the parent only. CONF-07 matches whole words of a column name: a plural (`emails`,
  `first_names`) and a quoted identifier in another case (`"Email"`) are not matched. Domains are matched by bare
  name and never dropped or altered (`ALTER DOMAIN`, `DROP DOMAIN` are not read); a composite type with an address
  field (`CREATE TYPE t AS (ip inet)`) used as a column type is not seen.
- **CONF-08** reads the forms of a gateway default listed in its row. Constants resolve by bare name (C and C++:
  `constexpr` and `const` declarations, `#define`s and enumerators across the scope, as CONF-04 reads them, with
  integer literals, casts and `+`; Go: package constants), and a value in those forms that does not resolve fails
  closed; so does an address prefix (`"127.0.0.1:"`) completed by `+` (C++ also `<<`) with a port the lint cannot
  evaluate. A literal of a gateway-named Go type is read whole, so another address field in it
  (`GatewayConfig{MetricsAddr: ":9100"}`) is read as the gateway's (an over-report). A multi-line TOML array is
  reported on its key's line, where a suppression goes. Not seen: a Go composite literal of a gateway-named type
  with a port field not named `Port` or for the gateway (`GatewayConfig{ListenPort: 7003}`) or with positional
  fields; an address assembled another way (`fmt::format("{}:{}", h, p)`, `absl::StrCat`, a Go `strings.Builder`);
  TOML multi-line strings (`"""…"""`); in C and C++, a port in a differently named constant or variable that no
  `listen`/`connect`/gateway line (or a statement such a line starts) passes on (an integer default of a `*port*`
  name that is not `…GatewayPort`); in YAML, compose's long syntax (`target:`/`published:`) and a Helm or Kubernetes
  `port` with `protocol: UDP` (only the short `…/udp` form is read).
- **CONF-09** reads YAML line by line, without a YAML parser. Not seen: a `uses:` written as a block scalar or
  pulled in through an anchor or alias (`<<: *setup`); a `GOTOOLCHAIN` set outside `.github/` (a script under
  `tools/ci/` that a workflow runs) or by a variable that a step assembles. A `GOTOOLCHAIN` whose value is an
  expression (`${{ … }}`) is reported, since the lint cannot tell its value. A `go-version-file` must name
  `services/go.mod` exactly, so a step that reaches the same file through `working-directory` is reported.
  Only `services/go.mod` is in scope (§5.10.3): other modules' `go.mod` files and `go.work` are not read.
- **CONF-10** scans C-family files (`.c`, `.cc`, `.cpp`, `.cxx`, headers, `.inl`, `.ipp`, `.tpp`, Objective-C
  and C++20 module units) with splices joined. Not seen: a name built by token pasting or another macro
  (`SDL_Create##Renderer`), a renderer created from Luau or Go, and an SDL renderer reached through a symbol
  lookup whose name is computed at run time. Only `#if 0` (or `false`) groups and the branches after `#if 1`
  are known dead; every other conditional group is read as compiled, which can over-report and never hides.
