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
  SARIF as an artifact and to code scanning (category `helios-conformance`). In the SARIF a failing finding is
  an `error`; a suppressed or known-failing one is a `note` with its suppression, because code scanning does
  not apply SARIF suppressions on upload and would fail a PR's check on a reviewed line the PR touches.

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
| CONF-08 | 04 §2; reconciliation #12 | a default gateway address or port other than 7777: Go values named for the gateway (keyed fields, var and const specs, assignments, calls with a `"gateway"` argument, and `…GatewayPort` integers); TOML keys or tables named for the gateway (`gateways = […]`, `[gateway] port`); a published `…/udp` port in YAML; C++ `k…GatewayPort` constants (initialised with `=`, `{…}` or `(…)`), and in files named for the gateway the `listen`/`connect` defaults (`"host:port"`, `Address::ipv4(…, port)`). Tests and fuzzers are skipped: they choose their own ports | Go syntax; TOML, YAML and C scans | passes |
| CONF-09 | ADR-014 | a `go.mod` `go` directive other than 1.27.x, a `toolchain` other than go1.27.x, or no `go` directive; an `actions/setup-go` step (block or flow style) without `go-version-file: services/go.mod`, or with `go-version`; a `GOTOOLCHAIN` set in workflow YAML (an `env` key, `GOTOOLCHAIN=…` in a script or `$GITHUB_ENV`) to anything but `auto`, `local`, `path` or go1.27.x | go.mod and workflow YAML lines | passes |
| CONF-10 | 08 §1.16; reconciliation #19 | `SDL_CreateRenderer` (and SDL3's other renderer constructors: `SDL_CreateRenderer*`, `SDL_CreateWindowAndRenderer`, `SDL_CreateSoftwareRenderer`, `SDL_CreateGPURenderer`) in C-family code (C++20 module units and `.tpp` too), including by name in a string or split by a backslash-newline splice, outside `apps/launcher/**` and engine/ui's SDL_Renderer backend (`engine/ui/**` paths containing `sdl_renderer`; WP-0.17 names the real files). `#if 0` groups are not read | comment-aware token scan | passes |
| CONF-11 | ADR-011 amendment; 02 §1.1; reconciliation #25 | in CMake code (comments stripped): `HELIOS_ISA_AVX2_TARGETS` or `HELIOS_ISA_AVX2_SOURCE_PATTERNS`, and any other `set`/`list` of an `*avx*_{targets,sources,patterns,files,kernels,allowlist}` variable; `helios_avx2_sources()`, defined or called; `set_source_files_properties` or `set_property(SOURCE …)` carrying `/arch:AVX*` (or `-arch:AVX*`), `-mavx*`, `-mbmi*`, `-mf16c`, `-mlzcnt`, `-mfma` or a `-march=` other than `x86-64`/`x86-64-v1`, literally or through a variable (`${v}`, `$CACHE{v}`, or a `foreach` loop variable over one): one set from them or from `helios_isa_avx2_flags()` in the same function, or, in any file, a level set that `cmake/HeliosIsa.cmake` defines (a variable it sets from them, or fills by calling a function that returns them, outside a function, into the parent scope or the cache, such as 02 §1.1's `HELIOS_ISA_AVX2` or `helios_isa_avx2_flags(HELIOS_ISA_AVX2)`) and the output of a function there that returns them; the same flags in `target_compile_options`, `add_compile_options`, `add_definitions`, `set_target_properties`/`set_property(TARGET …)` `COMPILE_OPTIONS` or `INTERFACE_COMPILE_OPTIONS`, `set_property(DIRECTORY …)`/`set_directory_properties` `COMPILE_OPTIONS`, `CMAKE_<LANG>_FLAGS*` (not `CMAKE_REQUIRED_FLAGS`, which only feeds try-compile probes), or as option arguments of a call to a function or macro that the scanned CMake files define (a wrapper), anywhere but inside `helios_apply_isa_level` in `cmake/HeliosIsa.cmake`, the function that applies an image's level (exempt by that exact name, which WP-0.2r uses or renames here). The `levels` fixture seeds the regressions WP-0.2r's names make natural: `${HELIOS_ISA_AVX2}` on a file or a target, a function's output, and a per-target loop outside the level function; `levels_fn` fills the level set with today's `helios_isa_avx2_flags()` and caches a copy through a chain of functions. A command that never closes is a `conformance` finding (`unclosed`). The bad fixture is a verbatim copy of today's two lists, `helios_isa_avx2_flags()`, `helios_avx2_sources()` and `tp_jolt`'s options | CMake command scan | **known failing, owned by WP-0.2r** (11 pinned findings in 5 files; the ISA audit's own disassembly fixture in `tools/lint/lint_tests.cmake` carries a `conformance:allow`, as §5.10.3 prescribes) |
| CONF-12 | 02 §1.1 (gate placement and gate-TU rules); reconciliation #25 | in the gate objects (`engine/core/src/cpugate/**`, `engine/core/src/platform/*/cpu_gate_hook.c`): a `.CRT$X*` section other than `.CRT$XLA0`, or `#pragma init_seg`; `ExitProcess`; no `/INCLUDE:_tls_used` or `/INCLUDE:helios_cpu_gate_tls_entry` in the Windows hook, `cmake/**` or `engine/**/CMakeLists.txt`; an `#include` other than `cpu_gate.h`, `<stdint.h>`, `<intrin.h>`, `<cpuid.h>`, `<windows.h>`, `<signal.h>`, `<unistd.h>`; a file-scope definition without `static` other than `helios_cpu_gate_run`, `helios_cpu_gate_verdict` and `helios_cpu_gate_tls_entry`. Elsewhere in scope (vendored patches too): a `.CRT$XLA*` contribution, a `.preinit_array` entry, `constructor(n)`/`init_priority(n)` with n < 101, `ifunc` or `target_clones` | C token scan | **known failing, owned by WP-0.5r** (8 pinned findings in the Windows hook, the external `helios_cpu_gate_crt_entry` among them) |

## The net schema (CONF-06, CONF-07)

The schema rules judge what a database holds after every migration, not each file alone (09 §5.10.4 (a)).
For each service directory `services/migrations/<dir>/`, the `-- +goose Up` sections of its files are applied
in version order: `CREATE SCHEMA`, `ALTER SCHEMA … RENAME TO`, `CREATE TABLE`, `ALTER TABLE` (`ADD`, `DROP` and
`RENAME` of columns, `RENAME TO`, `SET SCHEMA`) and `DROP TABLE`. Comments, string literals, dollar-quoted
bodies (functions, `DO` blocks) and `Down` sections are not statements of the net schema. Annotations are read as
goose v3 reads them (`-- +goose down`, `--+goose Up`: any case and spacing), and statements are split as
PostgreSQL lexes them: nested `/* */` comments, strings that span lines, `E'…'` backslash escapes, `"…"`
identifiers, `$tag$` bodies whose tag has digits, and a `$` inside an identifier (`a$b$`). An annotation with
leading whitespace, which goose rejects, still counts. A service's schema is
the `Name` its `migrations.Schemas` entry gives (`services/migrations/migrations.go`), or `svc_<dir>`. The legacy
rename that WP-0.15r declares there (`Legacy`, `LegacyVersion`) applies to the files up to `LegacyVersion`: their
`identity.account` is `svc_identity.account`, since `migrations.Up` renames the schema after them, and a later
file that still says `identity.` is misplaced. So `identity/00001`'s plain-text `email`, which `00004` drops, is
not a finding.

## Suppressions, known-failing records and the map

- **Suppression** (§5.10.3): a `conformance:allow CONF-nn <reason>` comment on the finding's line. The reviewer
  must approve it. Every suppression is printed on each run, so the round audit lists them. One without a rule
  ID or a reason is malformed, and one on a line where its rule reports nothing is unused: both fail.
- **Known failing** (`known_failing.jsonc`): each record names its rule, its rework WP (`owner`), its §5.10.4 row
  (`anchor`), a reason, and the findings it pins, one `{"path", "fingerprint"}` entry per finding. A fingerprint
  hashes the rule, the file, the message and the finding's source line with its surrounding whitespace trimmed,
  so it survives edits elsewhere in the file but not an edit of the line itself; `-fingerprints` prints them,
  and SARIF carries them as `partialFingerprints`. Pinned findings print as "known failing, owned by `<WP>`"
  and do not fail the run. Every other finding fails it, even of the same rule in the same file: a new grant
  next to an old one, a third copy of a line pinned twice, or a pinned line that gains a flag. An entry that
  matches no finding fails the run too, so the WP that closes a gap removes its entries and the record (and the
  gap). The scorecard carries each record's clause as a gap of `EXIT-0.conformance`, and the run fails on a
  record whose rule and owner no gap of that item names (when `scorecard.jsonc` exists, as it does in the
  repository). `-strict` ignores the file: `go run ./cmd/helios-conformance -root ../.. -strict -rules CONF-11`
  is how WP-0.2r shows CONF-11 clean. Today's records: CONF-11 (WP-0.2r) and CONF-12 (WP-0.5r), the open rows
  of 09 §5.10.4 (b). Adding a record is a Director decision (D3).
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
  `fmt.Sprintf`): an unresolved Go bucket is not reported (a `KeyTTL` key is). Names are looked up without scopes,
  so a parameter or local that shadows a package-level constant resolves to that constant; this matters only for
  `KeyTTL` and compare-and-set keys, which otherwise fail closed. A `$KV.` subject is read only as one literal.
- **CONF-01 and CONF-02, C and C++**, resolve buckets and keys through the scope's string constants by bare name (a
  name with several values matches if any value does). A string constant is a `#define` or the declaration of a
  constant initialized with literals: `constexpr`, or a `const` object (`const std::string k = …`, `static const
  char k[] = …`, `const char* const k = …`). Anything else fails closed: a parameter, a variable (a `const char* k`
  can be re-pointed), a member access (`o.bucket`, `p->bucket`) or a call. Remaining limit: a parameter or local
  with the same bare name as a string constant declared elsewhere in scope resolves to that constant (constants
  are `kPascalCase`, so such a collision is unlikely). Not seen: a nats.c call made through a macro or a function
  pointer. A `js_CreateKeyValue` whose `kvConfig` is filled in another file is reported as unresolved.
- **CONF-03** checks that the required tests exist and are not switched off. It cannot tell a test that passes
  vacuously, and it leaves a conditional skip (`if testing.Short()`) to CI, which runs the Go jobs without
  `-short`.
- **CONF-04** evaluates shift amounts through constants (Go across packages, C++ `constexpr` across the scope) and
  `+`. A C++ name defined more than once in scope counts with each of its values. Not seen: a shift amount in a
  local variable (`shift := 22`), `iota`, and a layout built with arithmetic other than `<<` and `* (1 << n)`. "ID
  code" is recognised by file name and keywords; a node-ID identifier elsewhere is not read.
- **CONF-05** reads direct imports and calls; a package that re-exports `idgen` under another name is not followed.
- **CONF-06 and CONF-07** evaluate the `-- +goose Up` SQL of each service. Not seen: DDL that a Go migration step
  runs (`ExecContext` in `services/migrations/*.go`), DDL inside a `DO $$ … $$` block, and statements built in Go
  strings. CONF-07 matches whole words of a column name: a plural (`emails`, `first_names`) and a quoted
  identifier in another case (`"Email"`) are not matched, and `ALTER COLUMN … TYPE INET` is not followed.
- **CONF-08** matches the forms of a gateway default the tree uses today. Not seen: a Go composite literal of a
  gateway-named type (`GatewayConfig{Port: 7003}`); in gateway-named C and C++ files, an integer default of a
  `*port*` name not named `…GatewayPort`, a `listen` default held in a differently named constant, and
  `Address::ipv4(…, kName)` through a constant; in YAML, a Helm `gateway: {port: …}` or a Kubernetes Service
  `port` with `protocol: UDP` (only compose's `…/udp` form is read). The PR #28 review lists these as follow-ups.
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
- **CONF-11** reads CMake commands as CMake's lexer does: `#` and `#[[…]]` comments are dropped, and a quoted
  argument, a bracket argument or an escaped `\(` does not close or open a command; a command still open at the
  end of a file fails the run as a `conformance` finding. A variable is followed within its function, and across
  files only for `cmake/HeliosIsa.cmake`'s level sets and flag functions: a variable set from AVX flags in one
  file and used in another is not. A wrapper is recognised only when the scanned CMake files define it, and only
  its option arguments count (an unquoted argument, or a quoted one made of options and variables), so a message
  that names a flag is not a grant. A list of targets or sources is flagged by name only when the name says
  `avx`; one with another name is caught where it grants the flags. Generator expressions are read as text, so a
  flag inside one counts. Not in §5.10.3's scope at all: `CMakePresets.json` `cacheVariables` (a
  `CMAKE_CXX_FLAGS` there), toolchain files outside `cmake/` and command-line `-D` options. Whether the presets
  belong in CONF-11's scope is a question for the Director.
- **CONF-12** recognises an external symbol in a gate TU by a file-scope definition without `static` (an `extern`
  one with a body or an initializer too, and every declarator of a statement), with macros not expanded; audit
  check 2 (the objects' symbol lists) is the object-level backstop. A pre-gate hook elsewhere is found by its
  section name or attribute, not by what a macro expands to.
