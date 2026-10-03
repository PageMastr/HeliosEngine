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
| CONF-01 | 05 §2.3, §1.4 | a JetStream KV bucket named `LEASES` or matching `(?i)lease\|leader\|fence` created, bound or read: Go `CreateKeyValue`, `CreateOrUpdateKeyValue`, `UpdateKeyValue`, `KeyValue` and `KeyValueConfig{Bucket: …}` (names resolved through constants, across packages), a `cfg.Bucket = …` assignment and a raw `$KV.<bucket>.` subject; nats.c `js_KeyValue`, `js_CreateKeyValue`/`js_UpdateKeyValue` and `kvConfig.Bucket`, with calls read across lines and names resolved through the scope's C and C++ string constants (`constexpr`/`const char*`, `char[]`, `std::string(_view)`, `#define`). A nats.c bucket the lint cannot resolve fails closed: a read projection bound through a variable carries a `conformance:allow`. A test that asserts the bucket is absent (`if _, err := js.KeyValue(…); err == nil { t.Fatal… }`, or the assignment then that `if`) is exempt | Go syntax; C token scan | passes |
| CONF-02 | 05 §1.4.1–1.4.2 | `TTL`, `LimitMarkerTTL` or `MaxAge` on a lease-named KV bucket or KV stream; a per-key TTL (`jetstream.KeyTTL`) on a lease key or on a key the lint cannot resolve; KV compare-and-set (`Create`, or `Update` with a revision, in jetstream's and nats.go's legacy API) on a key matching `lease\|leader\|fence\|elect\|term\|lock`; the nats.c forms (`cfg->TTL` on a lease bucket or one the lint cannot resolve, `kvStore_Create`/`kvStore_Update` on a leader-like key or one the lint cannot resolve) | Go syntax; C token scan | passes |
| CONF-03 | 05 §1.4.2 (holder rule) | the required test `conformance/holder_rule` (and any the map's `tests` add) missing from a language of the scope: Go needs `t.Run("holder_rule", …)` inside `func TestConformance`, C++ a `TEST_CASE("conformance/holder_rule…")` outside `#if 0`. It also fails a required test that is switched off: an unconditional `t.Skip` in `TestConformance` or the required case, a `//go:build` line on the file that defines `TestConformance`, or a C++ case marked `doctest::skip`, `may_fail`, `should_fail` or `expected_failures`. CI runs the tests (Go `services` job, C++ `server_tests`) | test presence | passes |
| CONF-04 | ADR-004, 05 §1.4.5 | in ID code (a file that names `idgen`, `AllocateIdBlocks`, a minter, `composeBlockId`, `BlockIdLayout` or Snowflake, or is named `*id*`/`*ids*`/`*minter*`), an identifier or config key for a node, worker, machine or datacenter ID (camelCase split, so `workerID` counts and a task graph's `NodeId` elsewhere does not); anywhere in scope, the Snowflake 41/10/12 layout (`<< 22` with `<< 12`), the retired 41/5/8/9 layout (`<< 22`, `<< 17`, `<< 9`), and `<< 22` (the time prefix) outside `pkg/idgen` and `engine/ecs`'s `entity_id.*`/`registry.*`. Shift amounts are evaluated through constants (Go across packages, C++ `constexpr` across the scope's headers); a literal left operand (`1 << 12`) is a size, not a field, unless it scales a field (`ms * (1 << 22)`). Node-ID keys in `services/**/*.toml` count too | Go syntax; C token scan | passes |
| CONF-05 | 05 §1.4.5 (who mints) | an import of `…/pkg/idgen`, or a call of `AllocateIdBlocks`, outside `services/internal/{identity, character, ledger, market, industry, mail, worldstate, world, activity, lifecycle, orchestrator, backend}`, `pkg/idgen` itself, `_test.go` files and test-helper packages (`testkit`, `testdata`, `testutil`, and `<name>test` for the store, db, nats and pg helpers and the minter packages; a name that only ends in "test", such as `latest`, is not one) | Go imports and calls | passes |
| CONF-06 | 05 §1.4, §3 | in the **net schema** (below), a schema not named `svc_<service>` (`CREATE SCHEMA`, `ALTER SCHEMA … RENAME TO`, or a `Name` in `migrations.Schemas`); a table created in, or moved (`SET SCHEMA`) to, a schema other than its service's, or created unqualified; an `ALTER TABLE` of a table that no earlier migration of the service creates | SQL evaluation | passes |
| CONF-07 | 05 §3, §6.6 (Phase 0 rule) | in the net schema, a column whose name's words are an e-mail, date of birth, IP address or real name (`email`, `email_norm`, `dob`, `date_of_birth`, `birthday`, `ip`, `ip_addr`, `client_ip`, `remote_addr`, `real_name`, `full_name`, `first_name`, `last_name`, `legal_name`, `given_name`, `family_name`, `surname`, …) or whose type is `INET`/`CIDR`, unless it is `*_ct` or `*_bidx`; any such column outside `svc_identity`; an `@pii` attribute in a `.hschema` package other than `identity` or `identity.*` | SQL evaluation; schema scan | passes |
| CONF-08 | 04 §2; reconciliation #12 | a default gateway address or port other than 7777: Go values named for the gateway (keyed fields, var and const specs, assignments, calls with a `"gateway"` argument, and `…GatewayPort` integers); TOML keys or tables named for the gateway (`gateways = […]`, `[gateway] port`); a published `…/udp` port in YAML; C++ `k…GatewayPort` constants, and in files named for the gateway the `listen`/`connect` defaults (`"host:port"`, `Address::ipv4(…, port)`). Tests and fuzzers are skipped: they choose their own ports | Go syntax; TOML, YAML and C scans | passes |
| CONF-09 | ADR-014 | a `go.mod` `go` directive other than 1.27.x, a `toolchain` other than go1.27.x, or no `go` directive; an `actions/setup-go` step (block or flow style) without `go-version-file: services/go.mod`, or with `go-version`; a `GOTOOLCHAIN` set in workflow YAML (an `env` key, `GOTOOLCHAIN=…` in a script or `$GITHUB_ENV`) to anything but `auto`, `local`, `path` or go1.27.x | go.mod and workflow YAML lines | passes |
| CONF-10 | 08 §1.16; reconciliation #19 | `SDL_CreateRenderer` (and SDL3's other renderer constructors: `SDL_CreateRenderer*`, `SDL_CreateWindowAndRenderer`, `SDL_CreateSoftwareRenderer`, `SDL_CreateGPURenderer`) in C-family code (C++20 module units and `.tpp` too), including by name in a string or split by a backslash-newline splice, outside `apps/launcher/**` and engine/ui's SDL_Renderer backend (`engine/ui/**` paths containing `sdl_renderer`; WP-0.17 names the real files). `#if 0` groups are not read | comment-aware token scan | passes |
| CONF-11 | ADR-011 amendment; 02 §1.1; reconciliation #25 | in CMake code (comments stripped): `HELIOS_ISA_AVX2_TARGETS` or `HELIOS_ISA_AVX2_SOURCE_PATTERNS`, and any other `set`/`list` of an `*avx*_{targets,sources,patterns,files,kernels,allowlist}` variable; `helios_avx2_sources()`, defined or called; `set_source_files_properties` or `set_property(SOURCE …)` carrying `/arch:AVX*`, `-mavx*`, `-mbmi*`, `-mf16c`, `-mlzcnt`, `-mfma` or a `-march=` other than `x86-64`, literally or through a variable set from them or from `helios_isa_avx2_flags()` in the same function; the same flags in `target_compile_options`, `add_compile_options`, `set_target_properties`/`set_property(TARGET …)` `COMPILE_OPTIONS` or `CMAKE_<LANG>_FLAGS*` outside `cmake/HeliosIsa.cmake` (the level sets). The bad fixture is a verbatim copy of today's two lists, `helios_isa_avx2_flags()`, `helios_avx2_sources()` and `tp_jolt`'s options | CMake command scan | **known failing, owned by WP-0.2r** (12 findings in 6 files) |
| CONF-12 | 02 §1.1 (gate placement and gate-TU rules); reconciliation #25 | in the gate objects (`engine/core/src/cpugate/**`, `engine/core/src/platform/*/cpu_gate_hook.c`): a `.CRT$X*` section other than `.CRT$XLA0`, or `#pragma init_seg`; `ExitProcess`; no `/INCLUDE:_tls_used` or `/INCLUDE:helios_cpu_gate_tls_entry` in the Windows hook, `cmake/**` or `engine/**/CMakeLists.txt`; an `#include` other than `cpu_gate.h`, `<stdint.h>`, `<intrin.h>`, `<cpuid.h>`, `<windows.h>`, `<signal.h>`, `<unistd.h>`; a file-scope definition without `static` other than `helios_cpu_gate_run`, `helios_cpu_gate_verdict` and `helios_cpu_gate_tls_entry`. Elsewhere in scope (vendored patches too): a `.CRT$XLA*` contribution, a `.preinit_array` entry, `constructor(n)`/`init_priority(n)` with n < 101, `ifunc` or `target_clones` | C token scan | **known failing, owned by WP-0.5r** (7 findings in the Windows hook) |

## The net schema (CONF-06, CONF-07)

The schema rules judge what a database holds after every migration, not each file alone (09 §5.10.4 (a)).
For each service directory `services/migrations/<dir>/`, the `-- +goose Up` sections of its files are applied
in version order: `CREATE SCHEMA`, `ALTER SCHEMA … RENAME TO`, `CREATE TABLE`, `ALTER TABLE` (`ADD`, `DROP` and
`RENAME` of columns, `RENAME TO`, `SET SCHEMA`) and `DROP TABLE`. Comments, string literals, dollar-quoted
bodies (functions, `DO` blocks) and `Down` sections are not statements of the net schema. A service's schema is
the `Name` its `migrations.Schemas` entry gives (`services/migrations/migrations.go`), or `svc_<dir>`. The legacy
rename that WP-0.15r declares there (`Legacy`, `LegacyVersion`) applies to the files up to `LegacyVersion`: their
`identity.account` is `svc_identity.account`, since `migrations.Up` renames the schema after them, and a later
file that still says `identity.` is misplaced. So `identity/00001`'s plain-text `email`, which `00004` drops, is
not a finding.

## Suppressions, known-failing records and the map

- **Suppression** (§5.10.3): a `conformance:allow CONF-nn <reason>` comment on the finding's line. The reviewer
  must approve it. Every suppression is printed on each run, so the round audit lists them. One without a rule
  ID or a reason is malformed, and one on a line where its rule reports nothing is unused: both fail.
- **Known failing** (`known_failing.jsonc`): findings of a rule under the record's paths are reported as
  "known failing, owned by `<WP>`" and do not fail the run. Each record names its rework WP, its §5.10.4 row
  and a reason, and the scorecard carries the same clause as a gap (`EXIT-0.conformance`). A record, or one of
  its paths, that matches no finding fails the run, so the WP that closes a gap must remove it (and the gap).
  `-strict` ignores the file: `go run ./cmd/helios-conformance -root ../.. -strict -rules CONF-11` is how
  WP-0.2r shows CONF-11 clean. Today's records: CONF-11 (WP-0.2r) and CONF-12 (WP-0.5r), the open rows of
  09 §5.10.4 (b).
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
  `fmt.Sprintf`): an unresolved Go bucket is not reported (a `KeyTTL` key is). A `$KV.` subject is read only as one
  literal.
- **CONF-01 and CONF-02, C and C++**, resolve buckets and keys through the scope's string constants by bare name (a
  name with several values matches if any value does); anything else fails closed. Not seen: a nats.c call made
  through a macro or a function pointer. A `js_CreateKeyValue` whose `kvConfig` is filled in another file is
  reported as unresolved.
- **CONF-03** checks that the required tests exist and are not switched off. It cannot tell a test that passes
  vacuously, and it leaves a conditional skip (`if testing.Short()`) to CI, which runs the Go jobs without
  `-short`.
- **CONF-04** evaluates shift amounts through constants (Go across packages, C++ `constexpr` across the scope) and
  `+`. Not seen: a shift amount in a local variable (`shift := 22`), `iota`, and a layout built with arithmetic other
  than `<<` and `* (1 << n)`. "ID code" is recognised by file name and keywords; a node-ID identifier elsewhere is
  not read.
- **CONF-05** reads direct imports and calls; a package that re-exports `idgen` under another name is not followed.
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
