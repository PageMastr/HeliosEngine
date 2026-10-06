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
lint's own fixtures (`testdata/`). Each finding prints as `path[:line]: CONF-nn: message` (a finding about a
whole file has no line, and a suppressed or known-failing one says so before its message); exit code 1 means a
finding fails the run, 2 a usage or I/O error.

## Where it runs

- **CTest** (label `lint`, `CMakeLists.txt` here): `lint_conformance_build` (builds the tool once for the
  others), `lint_conformance` over the repository, `lint_conformance_unit` (`go test`), and one
  `lint_conformance_fixture_<dir>_<case>` per seeded tree (each rule's, and the framework's under
  `testdata/framework/`). Go is
  optional locally (the tests are then not registered, and configure says so) and required in CI.
- **`cmake -P tools/ci/run_lints.cmake`**, with the other build-independent lints.
- **CI**: the `Conformance lint (tools/conformance)` job vets and tests the tool, runs it over the full tree
  (a superset of §5.10.3's "changed paths"; the known-failing records make that possible), and uploads the
  SARIF as an artifact and to code scanning (category `helios-conformance`). In the SARIF a failing finding is
  an `error`; a suppressed or known-failing one is a `note` with its suppression, because code scanning does
  not apply SARIF suppressions on upload and would fail a PR's check on a reviewed line the PR touches.

## Rules

A rule's scope is its §5.10.3 Scope column plus the paths of every map entry that names it (D2). Every rule
has a seeded violation under `testdata/<rule>/bad/` whose `expect.txt` lists the exact findings, and a
`good/` tree for its exemptions; `go test` compares both exactly, and CTest runs each through the CLI.

| Rule | Anchor | What fails it | Kind | On the tree |
|---|---|---|---|---|
| CONF-01 | 05 §2.3, §1.4 | a JetStream KV bucket named `LEASES` or matching `(?i)lease\|leader\|fence` created, bound or read: Go `CreateKeyValue`, `CreateOrUpdateKeyValue`, `UpdateKeyValue`, `KeyValue` and `KeyValueConfig{Bucket: …}` (names resolved through constants, across packages), a `cfg.Bucket = …` assignment and a raw `$KV.<bucket>.` subject; nats.c `js_KeyValue`, `js_CreateKeyValue`/`js_UpdateKeyValue` and `kvConfig.Bucket`, with calls read across lines and names resolved through the scope's C and C++ string constants (`constexpr`/`const char* const`, `char[]`, `std::string(_view)`, `#define`). A nats.c bucket the lint cannot resolve fails closed, and so does a Go one in NATS code (a file that imports nats.go or its `jetstream` package): a read projection bound through a variable carries a `conformance:allow`. A test that asserts the bucket is absent (`if _, err := js.KeyValue(…); err == nil { t.Fatal… }`, or the assignment then that `if`) is exempt | Go syntax; C token scan | passes |
| CONF-02 | 05 §1.4.1–1.4.2 | `TTL`, `LimitMarkerTTL` or `MaxAge` on a lease-named KV bucket or KV stream, in the config literal (also an element of a slice or map literal) or assigned after it (`cfg.TTL = …`); a per-key TTL (`jetstream.KeyTTL`) on a lease key or on a key the lint cannot resolve; KV compare-and-set (`Create`, or `Update` with a revision, in jetstream's and nats.go's legacy API) on a key matching `lease\|leader\|fence\|elect\|term\|lock`, or, in NATS code, on a key the lint cannot resolve; a TTL on a KV bucket the lint cannot resolve, in NATS code; the nats.c forms (`cfg->TTL` or `LimitMarkerTTL` on a lease bucket or one the lint cannot resolve, also in a nats.c file that sets no `Bucket`; a stream's `MaxAge` in a file that names a lease bucket's stream, `KV_leases`, or, in a file that uses KV, a stream whose name it cannot resolve or does not set; `kvStore_Create`/`kvStore_Update` and their `String` and `WithTTL` variants on a leader-like key or one the lint cannot resolve) | Go syntax; C token scan | passes |
| CONF-03 | 05 §1.4.2 (holder rule) | the required test `conformance/holder_rule` (and any the map's `tests` add) missing from a language of the scope: Go needs `t.Run("holder_rule", …)` inside `func TestConformance`, C++ a `TEST_CASE("conformance/holder_rule…")` outside `#if 0`. It also fails a required test that is switched off: an unconditional `t.Skip` in `TestConformance` or the required case, a `//go:build` line on the file that defines `TestConformance`, or a C++ case marked `doctest::skip`, `may_fail`, `should_fail` or `expected_failures` anywhere in its decorator chain (the case's argument is read to its closing parenthesis, so decorators with nested calls, strings and character literals do not end it; a conditional `doctest::skip(cond)` fails too). CI runs the tests (Go `services` job, C++ `server_tests`) | test presence | passes |
| CONF-04 | ADR-004, 05 §1.4.5 | in ID code (a file that names `idgen`, `AllocateIdBlocks`, a minter, `composeBlockId`, `BlockIdLayout` or Snowflake, or whose file name has `id`, `ids`, `idgen`, `entity_id`, `snowflake` or `minter` as a `_`-delimited word: `block_ids.cpp` is ID code, `grid.cpp` is not), an identifier or config key for a node, worker, machine or datacenter ID (camelCase split, so `workerID` counts and a task graph's `NodeId` elsewhere does not); anywhere in scope, the Snowflake 41/10/12 layout (`<< 22` with `<< 12`), the retired 41/5/8/9 layout (`<< 22`, `<< 17`, `<< 9`), and `<< 22` (the time prefix) outside `pkg/idgen` and `engine/ecs`'s `entity_id.*`/`registry.*`. Shift amounts, parenthesized ones too (`<< (kOffBits + kShBits)`), are evaluated through constants (Go across packages; C++ `constexpr` and `const` declarations, `#define`s and enumerators across the scope); a C++ left operand may be brace-initialized (`u64{prefix} << 22`, the codebase's widening idiom); a literal left operand (`1 << 12`, also converted or brace-initialized: `uint64(1) << 12`, `u64{1} << 12`) is a size, not a field, unless it scales a field (`ms * (1 << 22)`, also `ms * uint64(1 << 22)` and `ms * (u64{1} << 22)`; `4 * (1 << 22)` is a size). Node-ID keys in `services/**/*.toml` count too | Go syntax; C token scan | passes |
| CONF-05 | 05 §1.4.5 (who mints) | an import of `…/pkg/idgen`, or a call of `AllocateIdBlocks`, outside `services/internal/{identity, character, ledger, market, industry, mail, worldstate, world, activity, lifecycle, orchestrator, backend}`, `pkg/idgen` itself, `_test.go` files and test-helper packages (`testkit`, `testdata`, `testutil`, and `<name>test` for the store, db, nats and pg helpers and the minter packages; a name that only ends in "test", such as `latest`, is not one) | Go imports and calls | passes |
| CONF-06 | 05 §1.4, §3 | in the **net schema** (below), a schema not named `svc_<service>` (`CREATE SCHEMA`, `ALTER SCHEMA … RENAME TO`, or a `Name` in `migrations.Schemas`); a table created in, or moved (`SET SCHEMA`) to, a schema other than its service's, or created unqualified; a statement the evaluator cannot follow (fails closed, below) | SQL evaluation | passes |
| CONF-07 | 05 §3, §6.6 (Phase 0 rule) | in the net schema, a column whose name's words are an e-mail, date of birth, IP address or real name (`email`, `email_norm`, `dob`, `date_of_birth`, `birthday`, `ip`, `ip_addr`, `client_ip`, `remote_addr`, `real_name`, `full_name`, `first_name`, `last_name`, `legal_name`, `given_name`, `family_name`, `surname`, …) or whose type is `INET`/`CIDR` (also `pg_catalog.inet` and quoted, or a domain over one, through domains of domains), unless it is `*_ct` or `*_bidx`; any such column outside `svc_identity`; a statement the evaluator cannot follow (fails closed, below); an `@pii` attribute in a `.hschema` package other than `identity` or `identity.*` | SQL evaluation; schema scan | passes |
| CONF-08 | 04 §2; reconciliation #12 | a default gateway address or port other than 7777: Go values named for the gateway (keyed fields, var and const specs, assignments, calls with a `"gateway"` argument), with address strings, `net.JoinHostPort`, `fmt.Sprintf` and `fmt.Sprint` addresses and `"host:" + strconv.Itoa(port)` (also `host + ":" + port` with a host variable, parenthesised or not) evaluated, the `Port` field of such a value (`&net.UDPAddr{Port: …}`), a literal of a gateway-named type under any name (`GatewayConfig{Port: …, Addr: …}`, an unexported `port` field too, also as an elided slice or map element, nested ones included: `[]GatewayConfig{{Port: …}}`, `map[string][]GatewayConfig{"a": {{Port: …}}}`), and a value named for the gateway port (the words gateway and port: `…GatewayPort`, `"gateway-port"`) evaluated through constants; TOML keys (bare or quoted) or tables named for the gateway (address strings, and the integer of a key with the word port: `[gateway] port`, `listen_port`, `gateway_port`, not `transport`; an inline table's keys too: `listen = { host = …, port = … }`, also in an array of them, nested ones too: `listeners = [{ host = …, port = … }]`, `[[{ port = … }]]`; a multi-line array is read whole, on its key's line); in or outside such a table, a command line in a TOML array (a supervised process's `[[orchestrator.spawn]] args = ["--listen", "127.0.0.1:7777"]`): the value of `--listen`, `--connect` or an option named for the gateway, in the next element or after `=`, an address or, for an option named for a port, an integer (a value that is not `ip:port` is not reported: the gateway refuses to start with it, `parseAddress` in `engine/server/src/app_env.cpp`); a published `…/udp` (or `/UDP`) port in YAML (the host side of `[ip:]published:container/udp`, and the container side too); C++ `k…GatewayPort` constants (initialised with `=`, `{…}` or `(…)`) and `#define …GATEWAY_PORT`, and in files named for the gateway the lines that set its listen or connect address, found by the words of their names and strings (`listen`, `listenAddress`, `listen_address`, `clientListen`, `kListenAddr`, `"--connect"`; not `bind`, which the trunk sockets use too) (`"host:port"`, `"host:" + std::to_string(port)`, `std::string{"host:"} + …`, `os << "host:" << port`, `Address::ipv4(…, port)`, `ipv4(octets, port)`, `ipv6(groups, port)`, `ipv6Bytes(bytes, port)`, `loopbackV4(port)`, an option's default argument (its last argument, an address or an integer; an option call whose parentheses do not close fails closed: one that the file's end, an enclosing `}` or `]` (a block's or an initializer's brace, a subscript's bracket) or a `;` outside its brackets reaches first), and a string constant named there: `Address::parse(kDefaultListen)`; also on the continuation lines of a statement that such a line starts: `net::Address listen =` or `net::Address listen{` then `Address::ipv4(…);`, a statement ending at `;` or a block's brace, not an initializer's: one after a name, `=`, `,`, `(`, `{`, `return`, a template's `>` or an array bound such as `listen[1]{`), resolved through the scope's constants. A gateway port the rule reads but cannot resolve fails closed. Tests and fuzzers are skipped: they choose their own ports | Go syntax; TOML, YAML and C scans | passes |
| CONF-09 | ADR-014 | a `go.mod` `go` directive other than 1.27.x, a `toolchain` other than go1.27.x, or no `go` directive; an `actions/setup-go` step (block or flow style) without `go-version-file: services/go.mod`, or with `go-version`; a `GOTOOLCHAIN` set in workflow YAML (an `env` key, `GOTOOLCHAIN=…` in a script or `$GITHUB_ENV`) to anything but `auto`, `local`, `path` or go1.27.x | go.mod and workflow YAML lines | passes |
| CONF-10 | 08 §1.16; reconciliation #19 | `SDL_CreateRenderer` (and SDL3's other renderer constructors: `SDL_CreateRenderer*`, `SDL_CreateWindowAndRenderer`, `SDL_CreateSoftwareRenderer`, `SDL_CreateGPURenderer`) in C-family code (C++20 module units and `.tpp` too), including by name in a string or split by a backslash-newline splice, outside `apps/launcher/**` and engine/ui's SDL_Renderer backend (`engine/ui/**` paths containing `sdl_renderer`; WP-0.17 names the real files). `#if 0` groups are not read | comment-aware token scan | passes |
| CONF-11 | ADR-011 amendment; 02 §1.1; reconciliation #25 | in CMake code (comments stripped): `HELIOS_ISA_AVX2_TARGETS` or `HELIOS_ISA_AVX2_SOURCE_PATTERNS`, and any other `set`/`list` of an `*avx*_{targets,sources,patterns,files,kernels,allowlist}` variable; `helios_avx2_sources()`, defined or called; `set_source_files_properties` or `set_property(SOURCE …)` carrying `/arch:AVX*` (or `-arch:AVX*`), `-mavx*`, `-mbmi*`, `-mf16c`, `-mlzcnt`, `-mfma`, `-mfma4`, `-mxop`, Clang's `-mvaes`, `-mvpclmulqdq`, `-msm3`, `-msm4` and `-msha512` (which imply AVX or AVX2 there), a front-end target feature (`-Xclang -target-feature -Xclang +avx2`, `-mattr=+avx2`, also inside `SHELL:`) or a `-march=` other than `x86-64`/`x86-64-v1` (and, failing closed, a `-march=`, `/arch:` or `-m` whose value is a variable, also `$CACHE{v}` or `$ENV{v}`, or a generator expression: `-march=${level}`, `/arch:$CACHE{v}`, `-march=$<…>`, `-m${ext}`), literally or through a variable (`${v}`, `$CACHE{v}`, or a `foreach` loop variable over one or over literal flag options, also the `ZIP_LISTS` variable that takes such a list): one set from them, from `helios_isa_avx2_flags()` or from a function's output argument, `PARENT_SCOPE`, `return(PROPAGATE)` or macro, or read from a caller's scope, or, in any file, a level set that `cmake/HeliosIsa.cmake` defines (a variable it sets from them, or fills by calling a function that returns them, outside a function, into the parent scope or the cache, such as 02 §1.1's `HELIOS_ISA_AVX2` or `helios_isa_avx2_flags(HELIOS_ISA_AVX2)`) and the output of a function there that returns them; the same flags in `target_compile_options`, `add_compile_options`, `add_definitions`, `set_target_properties`/`set_property(TARGET …)` `COMPILE_OPTIONS` or `INTERFACE_COMPILE_OPTIONS`, `set_property(DIRECTORY …)`/`set_directory_properties` `COMPILE_OPTIONS`, any `CMAKE_*` variable (`CMAKE_<LANG>_FLAGS*`, `CMAKE_<LANG>_COMPILE_OBJECT`, …; not `CMAKE_REQUIRED_*`, which only feed try-compile probes) or `ENV{…}` (`ENV{CXXFLAGS}` seeds `CMAKE_CXX_FLAGS`), or as option arguments of a call to a function or macro that the scanned CMake files define (a wrapper; also one that fills an output argument with flags); any other command that carries them (`cmake_language(CALL|EVAL …)`, a function defined elsewhere; fails closed, see the limits), a `$ENV{…}` read in a grant or in link options (`target_link_options`, `add_link_options`, `LINK_OPTIONS`, which LTO compiles with), passed to a wrapper or set into a compiler flags variable (`CMAKE_<LANG>_FLAGS*`, `…_COMPILE_OBJECT`, `…_COMPILE_OPTIONS`, `ENV{…FLAGS…}`; not the variable's own value, `set(ENV{CXXFLAGS} "$ENV{CXXFLAGS} …")`; fails closed), and a `set()` of them into a name built at run time other than an output parameter (`${ARG_OUT}`, `${prefix}_FLAGS`, `${ARGV0}`; fails closed), anywhere but inside `helios_apply_isa_level` in `cmake/HeliosIsa.cmake`, the function that applies an image's level (exempt by that exact name, which WP-0.2r defines in `cmake/HeliosIsa.cmake`). The `levels` fixture seeds the regressions WP-0.2r's names make natural: `${HELIOS_ISA_AVX2}` on a file or a target, a function's output, and a per-target loop outside the level function; `levels_fn` fills the level set with the pre-WP-0.2r `helios_isa_avx2_flags()` and caches a copy through a chain of functions. A command that never closes is a `conformance` finding (`unclosed`). The bad fixture is a verbatim copy of the pre-WP-0.2r lists, `helios_isa_avx2_flags()`, `helios_avx2_sources()` and `tp_jolt`'s options | CMake command scan | passes (WP-0.2r replaced the per-file allowlist with image levels and removed its 11 pinned findings; the seeded fixture of the configure check against ISA options on a library's interface, in `tools/lint/tests/layering/CMakeLists.txt`, carries a `conformance:allow`, as §5.10.3 prescribes for the audit's own fixtures) |
| CONF-12 | 02 §1.1 (gate placement and gate-TU rules); reconciliation #25 | in the gate objects (`engine/core/src/cpugate/**`, `engine/core/src/platform/*/cpu_gate_hook.c`): a `.CRT$X*` section other than `.CRT$XLA0`, or `#pragma init_seg`; `ExitProcess`; no `/INCLUDE:_tls_used` or `/INCLUDE:helios_cpu_gate_tls_entry` in the Windows hook, `cmake/**` or `engine/**/CMakeLists.txt`; an `#include` other than `cpu_gate.h`, `<stdint.h>`, `<intrin.h>`, `<cpuid.h>`, `<windows.h>`, `<signal.h>`, `<unistd.h>`; a file-scope definition without `static` other than `helios_cpu_gate_run`, `helios_cpu_gate_verdict` and `helios_cpu_gate_tls_entry`. Elsewhere in scope (vendored patches too): a `.CRT$XLA*` contribution, a `.preinit_array` entry, `constructor(n)`/`init_priority(n)` with n < 101, `ifunc` or `target_clones` | C token scan | passes (WP-0.5r part 1 moved the Windows gate to `.CRT$XLA0` with `/INCLUDE:_tls_used`, `/INCLUDE:helios_cpu_gate_tls_entry` and `TerminateProcess`, made `helios_cpu_gate_run`, `helios_cpu_gate_verdict` and `helios_cpu_gate_tls_entry` the only external definitions, and removed the record that pinned its 8 findings) |

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
  is how WP-0.2r showed CONF-11 clean. There are no records today: WP-0.2r part 1 removed CONF-11's and
  WP-0.5r part 1 CONF-12's (09 §5.10.4 (b)). Adding a record is a Director decision (D3).
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
  or `StreamConfig`, one set from `new(T)`, and the elements of a slice, array or map of them, declared (a variadic
  parameter `cfgs ...T` too), written as a literal or made with `make`: `cfgs[i]`, a range variable, `c := cfgs[i]`;
  matched by name), with the buckets the file sets on it (a collection's elements share theirs); in NATS code a KV
  config whose bucket does not resolve, or is not set in the file, fails closed. Not seen: a TTL assigned to a config
  reached another way (a call's result, a variable of another inferred type, a nested collection, a collection of a
  named type such as `type Configs []jetstream.KeyValueConfig`, whose literals CONF-01 does not read either), and a KV
  call in a file that reaches nats.go only through a wrapper package of its own, with a bucket or key the lint cannot
  resolve (a resolved lease or leader name is still reported). Names are looked up without scopes, so a parameter or
  local that shadows a package-level constant resolves to that constant; this matters only where an unresolved name
  would fail closed. Config names are matched without scopes too, so configs that share a name share their
  buckets. A range variable or element copy (`c := cfgs[i]`) is a config of its own that takes the buckets of
  every collection its name ranges over or copies from, never the collection itself. One over a KV collection on
  which the file sets no bucket, and a KV parameter or struct field (or a collection of them) whose name's buckets
  come only from literals, add a bucket the lint cannot resolve (a range variable over such a collection, or a copy
  of one of its elements, takes it too), so they fail closed in NATS code. Not seen: a KV parameter, range variable
  or element copy named like a config that a `.Bucket = …` assignment sets elsewhere in the file takes that bucket,
  since the assignment is not tracked to one of them. A `$KV.` subject is read only as one literal.
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
  is set, and its scope covers CONF-02's, `engine/authority/**` included; `TestRepositoryMap` checks that).
  `MaxAge` is a `jsStreamConfig` field: it counts in a file that sets a stream `Name` resolving to a lease-named
  stream (`KV_leases`). With a stream name the lint cannot resolve, or none set in the file, it is ordinary
  retention, as on the Go side, except in a file that uses JetStream KV (any `js_…KeyValue` call, `js_DeleteKeyValue`
  included, or a `kvStore_*` call) or spells a `"KV_…"` name, where it fails closed. Like a `TTL`, a `MaxAge` is
  not tied to its config: any `.Name = …` or `->Name = …` in the file counts as a stream name, so an unrelated
  struct's `p->Name = "peer";` makes the name resolve and a `MaxAge` in a KV file pass (there is no nats.c JetStream or
  KV code in the tree yet: `engine/server/src/nats_bus.cpp` uses core NATS only).
- **CONF-03** checks that the required tests exist and are not switched off. It cannot tell a test that passes
  vacuously, and it leaves a conditional skip (`if testing.Short()`) to CI, which runs the Go jobs without
  `-short`. An off-decorator is read by its constructor call, so `skip()` under `using namespace doctest` and
  `dt::skip{}` through a namespace alias count. Not read: a decorator on the enclosing `TEST_SUITE`
  (`TEST_SUITE("x" * doctest::skip())`), one held in a variable (`constexpr auto kOff = doctest::skip();`, then
  `TEST_CASE("conformance/holder_rule" * kOff)`), and one reached through a macro (`#define HOLDER_OFF
  doctest::skip()`, then `TEST_CASE("conformance/holder_rule" * HOLDER_OFF)`).
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
  reported on its key's line, where a suppression goes. A C++ port is reported once per line; a listen or connect
  constant in a gateway file is reported where it is declared and again on each read line that names it. A C++ string
  constant named on a read line is read as an address, so one that is not the gateway's (`bus.connect(kNatsAddr)` with
  `"127.0.0.1:4222"`) is reported, and so is every value of a bare name declared more than once in scope
  (over-reports). C++ braces are told apart by what precedes them, without a parser: a brace after a name opens an
  initializer unless the statement declares a namespace, class, struct, union, enum or extern block or the name is
  `else`, `do`, `try` (also before an attribute: `else [[likely]] {`) or a qualifier (`const`, `noexcept`,
  `override`, `final`, `mutable`), so a function body after a
  macro (`void f() HELIOS_NOEXCEPT {`) is read as part of its signature's statement (an over-report). Not seen: a Go
  composite literal of a gateway-named type with a port field not named `Port` or for the gateway
  (`GatewayConfig{ListenPort: 7003}`) or with positional fields; an address assembled another way
  (`fmt::format("{}:{}", h, p)`, `absl::StrCat`, a Go `strings.Builder`); TOML multi-line strings (`"""…"""` over
  several lines; a triple-quoted string on one line is skipped as text); in C and C++, a port or address in a
  variable, or in a differently named constant that no `listen`/`connect`/gateway line (or a statement such a line
  starts) names (an integer default of a `*port*` name that is not `…GatewayPort`); a client-side bind default under a
  name without listen or connect (`net::Address bindAddr = …`); a name with listen or connect only inside a longer
  word (`clientListenerAddr`, `listening`) or after an acronym (`UDPListen`, `HTTPListen`: a name splits only where a
  lower-case letter or digit meets a capital, so `udplisten` is one word); a value that a block or a preprocessor line separates
  from its read line: a multi-line immediately-invoked lambda initializer (`net::Address listen = [] {`, `return
  ipv4(…, 7000);`, `}();`), an accessor's body (`net::Address listen() const {`, `return ipv4(…, 7000);`), the
  lines of a multi-line option call after a lambda argument whose body holds a `;` (`args.get("listen",`,
  `[&] { audit(); return true; }(),`, `"127.0.0.1:7000", …);`: the default, its last argument, is still checked) and
  an initializer split by `#if`. A nested call that `#if` branches leave unbalanced inside an option call is closed
  by the outer call's `)`, so the option's default is read from there (`parseAddress(args.get("listen", "",`, `#if`,
  `pick("127.0.0.1:7000"`, `#else`, `pick("0.0.0.0:7777"`, `#endif`, `)), "--listen")` checks `"--listen"`): the
  branch values are the `#if` limit, and nothing after the call is hidden. In YAML, not seen: compose's long syntax
  (`target:`/`published:`) and a Helm or Kubernetes `port` with `protocol: UDP` (only the short `…/udp` form is read).
- **CONF-09** reads YAML line by line, without a YAML parser. Not seen: a `uses:` written as a block scalar or
  pulled in through an anchor or alias (`<<: *setup`); a `GOTOOLCHAIN` set outside `.github/` (a script under
  `tools/ci/` that a workflow runs) or by a variable that a step assembles. A `GOTOOLCHAIN` whose value is an
  expression (`${{ … }}`) is reported, since the lint cannot tell its value. A `go-version-file` must name
  `services/go.mod` exactly, so a step that reaches the same file through `working-directory` is reported.
  Only `services/go.mod` (§5.10.3's scope) and `tools/conformance/go.mod` (added by `map.jsonc`'s ADR-014 entry)
  are read: other modules' `go.mod` files and `go.work` are not.
- **CONF-10** scans C-family files (`.c`, `.cc`, `.cpp`, `.cxx`, headers, `.inl`, `.ipp`, `.tpp`, Objective-C
  and C++20 module units) with splices joined. Not seen: a name built by token pasting or another macro
  (`SDL_Create##Renderer`), a renderer created from Luau or Go, and an SDL renderer reached through a symbol
  lookup whose name is computed at run time. Only `#if 0` (or `false`) groups and the branches after `#if 1`
  are known dead; every other conditional group is read as compiled, which can over-report and never hides.
- **CONF-11** reads CMake commands as CMake's lexer does: `#` and `#[[…]]` comments are dropped, and a quoted
  argument, a bracket argument or an escaped `\(` does not close or open a command; a command still open at the end
  of a file fails the run as a `conformance` finding. A variable is followed by scope: one set at file scope stays
  tracked across function and macro definitions, and a function or macro body sees every variable its file sets
  (also one set after the definition, since a body runs when it is called) and every flag variable in scope where it
  is called (CMake's dynamic scope: a callee reads its caller's locals). A body hands flags back to its caller
  through an output argument (`set(${out} … PARENT_SCOPE)`, or `set(${out} …)` in a macro, also forwarded to another
  such function), `PARENT_SCOPE` or `CACHE`, `return(PROPAGATE v)`, or any `set()` in a macro (a macro writes its
  caller's scope); a call receives them, and the scope around the definition counts too, so a function never called
  still does. Any other `set()` in a body stays local. CMake functions are global, so the scan reads every file in
  scope until it learns nothing new about them before it reports. `string()`, `list()` and `separate_arguments()`
  write the argument CMake writes (`string(REPLACE <match> <replace> <out> …)`, `string(REGEX REPLACE …)`,
  `string(JOIN <glue> <out> …)`, every `<out>` of `list(POP_FRONT|POP_BACK <list> <out>…)`, …), a `list()`
  sub-command reads its list by name (`list(GET l 0 out)` reads `${l}`), and a `REPLACE` or `REGEX` pattern does not
  carry a flag into the output. A `foreach` loop variable holds flags inside its loop, when it runs over a flag
  variable or over literal items that are options (`foreach(f -mavx2 -mfma)`; a quoted sentence that names a flag is
  not one, and a quoted list, `"sse4.2;-mavx2"`, is read item by item; a quoted item that holds a `SHELL:` group is
  options whatever its words, since CMake passes each of them: `"SHELL:-Xclang -target-feature -Xclang +avx2"`,
  `"SHELL:-mavx2 -include simd.h"`, also inside a generator expression), and is restored when the loop ends
  (CMP0124); with `IN ZIP_LISTS` the variable that takes a flag list holds them (the i-th of several loop variables,
  or `<v>_<i>` for a single one). A `set()` of flags into a name built at run time (`${ARG_OUT}` from
  `cmake_parse_arguments`, `${prefix}_FLAGS`, `${ARGV0}`), other than an output parameter of the enclosing
  definition, fails closed where it is written, since no reference can be matched to it; so does a producer called
  with such a name. Across files a variable is followed for `cmake/HeliosIsa.cmake`'s level sets and flag functions
  (names matched in any letter case; a macro called at file scope and a file-scope `foreach` over a level set, or
  over literal flags, define level sets too), for the variables the root `CMakeLists.txt` and the `cmake/*.cmake`
  modules end their file scope with (seen in every directory, as they run first), for those an ancestor directory's
  `CMakeLists.txt` ends its file scope with (seen in its subdirectories' `CMakeLists.txt`; one set after the
  `add_subdirectory()` counts too, an over-report; ancestors are found by path, not by `add_subdirectory()`, so a
  directory added from elsewhere, as `engine/render` adds `tools/shaderc`, is given its path's ancestors rather than
  the adding directory's), and through function calls as above; any other variable set from AVX flags in one file
  and read at file scope in another (an `include()`d file outside `cmake/`) is not. A wrapper is recognised only
  when the scanned CMake files define it, and only its option arguments count (an unquoted argument, or a quoted one
  made of options, `+feature` words and variables, or one that holds a `SHELL:` group, whose words are all options
  whatever they look like: `"SHELL:-x c++ -mavx2"`), so a message that names a flag is not a grant. Any other
  command that carries an AVX-class flag fails closed (`cmake_language(CALL …)`, `cmake_language(EVAL CODE …)`, a function defined outside
  the scanned files), except `message()`, conditions (`if`, `elseif`, `while`), compiler-flag probes
  (`check_*_compiler_flag`) and `cmake_parse_arguments()`; so does a nested reference (`${${name}}`) in a grant,
  whose value the scan cannot tell. `block()` scopes are not modelled: a variable set inside one counts after it too
  (an over-report). A `$ENV{…}` read in a grant or in link options, passed to a wrapper or set into a compiler
  flags variable fails closed (a `string(REPLACE)` match string is not a value it writes); one copied into another
  variable first (`set(f $ENV{X})`, then `${f}` in a grant) is not followed. Not seen either (none is in the tree):
  a `$ENV{…}` in `set_target_properties(… LINK_FLAGS …)` or `LINK_FLAGS_<CONFIG>` (a literal flag there is
  reported) or in `target_link_libraries()` (where an item starting with `-` is a link flag), and a front-end CPU
  (`-Xclang -target-cpu -Xclang haswell`), which sets the ISA as `-march=` does. Inside `helios_apply_isa_level`
  nothing is reported, and a computed name it writes (`set(${tgt}_FLAGS … PARENT_SCOPE)`) does not carry flags to
  its callers: the exemption covers what that function hands back, except an output argument it fills
  (`set(${out} -mavx2 PARENT_SCOPE)`), which is tracked like any other function's, so a caller's grant of it is
  reported. A call to a function that fills an output argument is read like any other call, so the options it is
  passed count when the function is a wrapper (`helios_isa_avx2_flags(out -mfma)` is reported as well, an
  over-report). A list of targets or sources is flagged by name only when the name says `avx`; one with another name
  is caught where it grants the flags. Generator expressions are read as text, so a flag inside one counts. Not in
  §5.10.3's scope at all: `CMakePresets.json` `cacheVariables` (a `CMAKE_CXX_FLAGS` there), toolchain files outside
  `cmake/` and command-line `-D` options. Whether the presets belong in CONF-11's scope is a question for the
  Director.
- **CONF-12** recognises an external symbol in a gate TU by a file-scope definition without `static` (an `extern`
  one with a body or an initializer too, every declarator of a statement, a variable of `struct`, `enum` or
  `union` type, named or anonymous, and an array whose bound or `_Alignas` has parentheses), with macros not
  expanded; audit check 2 (the objects' symbol lists) is the
  object-level backstop. A pre-gate hook elsewhere is found by its section name or attribute, not by what a macro
  expands to.
