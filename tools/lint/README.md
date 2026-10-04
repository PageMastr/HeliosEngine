# tools/lint — repository lints (WP-0.2)

Every lint in this directory is a CMake script (`cmake -P …`), so it runs wherever CMake runs.
`cmake/HeliosLayering.cmake` registers them as CTests (label `lint`) at the end of configure, and
`tools/ci/run_lints.cmake` runs the build-independent ones in one go (CI fast path, pre-commit hook),
together with the D6 status check of 09 §5.10.2 (`tools/status/check_status.cmake`): a module directory
not named in 09 §8.1, or a module README without its `Plan-Rev`, fails it. That check needs Python
3.10+; where it is not on `PATH`, pass `-DHELIOS_STATUS_PYTHON=<path>` to `run_lints.cmake`, which
forwards it.

```
ctest --test-dir build/<dir> -L lint --output-on-failure      # all lints + their fixtures
cmake -P tools/ci/run_lints.cmake                                # the lints that need no build
```

## SARIF in CI

The Linux headless job writes CTest JUnit XML and converts failed `lint_*` tests with
`tools/ci/ctest_to_sarif.py`. Findings with a reported `path:line` annotate that source line;
failures without a source location annotate the relevant lint script and retain the diagnostic.
When CTest runs, CI publishes the SARIF as an artifact. It also uploads to GitHub code scanning
on `main` and same-repository PRs; fork PRs have read-only tokens, so their SARIF stays in the
artifact. CTest's original pass/fail result remains the CI gate.

| Lint | Script | What fails it | Plan |
|---|---|---|---|
| Module layering | `cmake/HeliosLayering.cmake` (configure time) | upward or unlisted same-layer dependency, cycle, a module missing from `HELIOS_MODULE_ORDER` or the order not topological (a module edge is any path between two modules whose inner targets are not modules, such as an INTERFACE or STATIC helper), a `helios_module()` LAYER, HEADLESS, EDITOR_ONLY or PEERS entry that its table row in `engine/CMakeLists.txt` does not have, a second `helios_declare_module()` row for a module, a module with neither a row nor a LAYER, an executable under `apps/` that is not a `helios_executable()`, a `helios_executable()` without ROLE in a directory whose role cannot be inferred, a HEADLESS module reaching a non-HEADLESS module or a graphics library (volk, VMA, SDL3, ImGui, …), an `EDITOR_ONLY` module in a client/launcher/bot/cell/gateway/voice executable or under a runtime module, a server executable (cell, gateway, voice, bot) linking anything non-HEADLESS | 02 §1.1, RT-09 |
| ISA audit | `isa_audit.cmake` + `cmake/isa_allowlist.cmake` | AVX-class flags on a unit outside the allowlist (`tp_jolt`, `*_avx2.c(pp)` kernels; `/clang:`-forwarded flags included); FMA or AVX-512 on an allowlisted unit; any flag above x86-64-v1 on the CPU-gate units (`-msse3`…`-msse4.2`, `-mpopcnt`, `-mcx16`, `-mavx*`, `-mbmi*`, a `-march` other than `x86-64`, `/arch:` other than SSE2); `-march=native`, fast-math or FP contraction anywhere; a VEX/EVEX/opmask/BMI/LZCNT/POPCNT/MOVBE/CMPXCHG16B/AES/SHA/SSE3+ instruction (prefixes such as `lock` looked through), a weak/COMDAT/IFUNC symbol, an unlisted export or import (stack-protector and sanitizer symbols included) in a gate object; a gated ELF executable whose single `.preinit_array` entry is not the gate's `hcg_gate`, or that has `R_X86_64_IRELATIVE` relocations | 02 §1.1 "The audit", RT-09, CL-17 |
| Licences | `licenses.cmake` + `license_policy.cmake` | a dependency without a top-level licence file, a licence file (nested ones too) that is copyleft/unknown (GPL family, MPL, EPL, Artistic, CC-BY-SA/NC/ND, JSON licence, …) and not one option of an explicit multi-licence choice naming a permissive licence, a dependency without its own row in `third_party/MANIFEST.md` (Name cell, its first word or the upstream repository name), a MANIFEST licence identifier outside MIT/BSD/zlib/Apache-2.0/Boost/ISC/PostgreSQL/public domain (CC0) | CLAUDE.md, 01 §5.2, ADR-012 |
| Vendored patches | `vendor_patches.cmake` | a `third_party/<dep>/patches/` file not named `NNNN-<slug>.patch`, with no hunk, or not a row of the table under `third_party/MANIFEST.md`'s `### … (third_party/<dep>/patches/)` heading; a row there naming a missing file; a hunk whose post-image (context plus added lines) is not in the committed file, in hunk order; a truncated or malformed hunk; a patched file that is missing, or that the patch deletes but still exists. Line endings are normalized, so CRLF checkouts pass, and `\ No newline at end of file` markers are honoured (a post-image without a final newline must end the file); no git or network. It does not see an in-place edit outside every hunk: re-running `tools/vendor/fetch_third_party.sh <dep>` and diffing `third_party/` is the full proof | CLAUDE.md (vendored code), `third_party/MANIFEST.md` "Patches", K10 |
| IP names | `ip_names.cmake` + `ip_names_policy.cmake` | a *Cinder Reach* name (Kestrel, Harrow, Tallis, …) outside `content/` and `docs/concept/`, also inside identifiers and paths (`KestrelHull`, `cinder_reach`, `TALLIS_ORBIT`, `hull/kestrel`); an in-universe name from Star Wars, EVE, Destiny or Star Citizen anywhere it looks; a franchise title inside `content/` or `docs/concept/`. It looks at `engine/`, `apps/`, `tools/` (not `tools/lint/`, `tools/prebuilt/` or `tools/vendor/`), `schemas/`, `services/`, `shaders/`, `content/`, `gems/`, `cmake/`, `docs/concept/` and the top-level files; the rest of `docs/` is not scanned | 01 §4.1 rule 2, §5.2 |
| Concept references | `concept_refs.cmake` | under `docs/concept/`: an image (`.png`, `.webp`, `.jpg`, `.jpeg`) without its `<image>.concept.jsonc` sidecar; a sidecar without its image or next to a file that is not an image; a sidecar or `TEMPLATE.concept.jsonc` that is not JSONC, lacks a required field, has an unknown one, a value of the wrong type or outside its set (status, source kind, licence, phase, calendar date), a source kind without its evidence (CC0 address, commissioning rights, AI tool, model, prompt and inputs), or a sha256 that does not match the image or a committed AI input; an image over 1,048,576 bytes or 2,560 px on its long side, or whose header does not match its extension; any other file type (Markdown and `.gitattributes` aside); an image outside `editor/`, `client/`, `launcher/`, `world/`, not named `<tool-or-area>-<subject>[-<variant>]-vNN.<ext>`, or whose concept is not in the `## Index` section of `docs/concept/README.md` (as the concept name or a `<concept>-vNN` file name); a missing README, or one without that section. In a git checkout only the files git tracks or would track count. It cannot see who set a status or whether a version was overwritten | 01 §5.2, 09 §4.2, `docs/concept/README.md` |
| Test namespaces | `test_namespaces.cmake` | a doctest test macro outside an unnamed namespace in any file under a `tests/` or `test/` directory; in a test source (a `.cpp`/`.cc`/`.cxx` file there with a test macro), any declaration or definition at namespace scope outside an unnamed namespace or a waiver region; anything its lexer cannot follow. Details, the waiver syntax and the limits are in "Test namespaces" below | CLAUDE.md (tests), AAA-PLT-1 |
| Windows manifest | `windows_manifest.cmake` | `engine/platform/win/helios.manifest` without UTF-8 code page, PerMonitorV2, longPathAware, Windows 10/11 supportedOS or asInvoker; on Windows builds, `core_tests.exe` not embedding it | ADR-011, 02 §2.1 |
| Plan conformance (WP-0.2; CTests `lint_conformance*`, registered from `tools/conformance/CMakeLists.txt`; Go) | `tools/conformance` (`helios-conformance`) | a CONF rule of 09 §5.10.3 that the rule table in `tools/conformance/README.md` lists, outside a reasoned `conformance:allow CONF-nn <reason>` or a known-failing record owned by a rework WP; a malformed or unused suppression, a stale record or an invalid anchor map | 09 §5.10.2–5.10.3 |
| Shipped pipelines (WP-0.12; CTest `lint_shipped_pipelines`, registered from `tools/rendertest/tests/shipped_pipelines_tests.cmake`) | `tools/rendertest/tests/shipped_pipelines_lint.cmake` | an identifier `create<X>Pipeline` other than `createShippedPipeline`/`createLocalPipeline` in `engine/`, `apps/` or `tools/` (outside `engine/rhi/`, `tools/prebuilt/`, its own and the other lints' fixtures and `engine/render/src/shader_library.cpp`; backslash-newline splices joined) without a `// shipped-pipelines-lint: allow <reason>` waiver on its line or alone on the line above; a waiver without a reason of at least 12 characters. A best-effort textual check against accidental direct pipeline creation, not against deliberately adversarial source (code review and `rendertest.coverage` are the backstops): token pasting, a creation path (wrapper or member pointer) defined outside the scanned files, which includes `engine/rhi/`, and files with other extensions are not seen | 03 §9.3 RC-1 |

Each lint has seeded-violation fixtures under `tests/` that must be rejected with a specific message
(`lint_*_fixture_*`, `lint_isa_disasm_*`, `lint_isa_image_*`, `lint_layering_*`), so a lint that
silently stops matching fails CI. The fixtures run through `expect_fail.cmake`, which also requires a
non-zero exit: a lint that prints the finding but exits 0 fails its fixture. The layering check looks
through generator expressions (`$<BUILD_INTERFACE:…>`, `$<LINK_ONLY:…>`, conditions), so a dependency
cannot hide in one.

## Waivers

Waivers live next to the rules and carry a reason; every waiver is printed on each run.

- `license_policy.cmake`: HIDAPI's GPL-3.0 and original-licence files inside SDL3 (tri-licensed, used
  under BSD). ISC (netcode's bundled libsodium subset) and the PostgreSQL licence are on 01 §5.2's
  scanner allowlist and on CLAUDE.md's.
- The scanner covers the vendored tree (`third_party/`, `MANIFEST.md`). The Go modules that
  `services/go.sum` pins are not vendored, so it does not see them: an open gap against 01 §5.2, owned
  by the WP-0.2 follow-up (Go module licence scan) and recorded in the scorecard (`EXIT-0.conformance`).
  A review sample of 51 of the 145 modules found only permissive licences.
- `ip_names_policy.cmake`: reference names in engine test data and doc examples that predate the
  lint: `engine/ecs/tests/test_command_buffer.cpp` ("Kestrel"), `engine/reflect` (`hull/kestrel`,
  `ship.kestrel.name` in `record.h`, `types.h` and two tests) and
  `engine/authority/tests/test_lease_ag.cpp` (zones "tallis", "harrow"), and the cell/gateway
  runtime in development (`engine/server/`, `apps/cellserver/`, `apps/gateway/`: default zone
  `"tallis"`, test zones "tallis"/"harrow"; directory waivers). Their owners rename them, then the
  waivers go.

## Test namespaces

Why: doctest's `DOCTEST_ANON_FUNC_<n>` names repeat across a test executable. MSVC's `cl` names a
lambda in such a function after the function alone, and emits static member variable templates
instantiated with it (`Job::kInlineOps<Fn>`) as external COMDATs, so the linker keeps one and a test
runs another file's lambda (#15). A same-named file-scope helper collides the same way. Names in an
unnamed namespace carry a per-file hash. `engine/core/tests/test_tu_isolation_{a,b}.cpp` probe it on
the Windows jobs.

Threat model: `lint_test_namespaces` is a best-effort textual check against *accidental* omissions,
such as a new test file or a helper left outside the namespace. It is not a guarantee against
deliberately adversarial source: digraphs, form feeds, spliced directive names, macro-generated
braces and the like are documented limits (below). The backstops are code review and the MSVC
TU-isolation probe in CI.

What it scans: the files under a `tests/` or `test/` directory of `engine/`, `tools/` and `apps/`,
except `tools/prebuilt/` and its own fixtures. In a git checkout these are the files git tracks or
would track (`git ls-files --cached --others --exclude-standard`), so a new test file counts before
it is added and an ignored download does not; outside one, every file.

What fails it (each finding is a `path:line`):

- a doctest test macro (`TEST_CASE*`, `TEST_SUITE*`, `SCENARIO*`, `SUBCASE`,
  `GIVEN`/`WHEN`/`THEN`/`AND_WHEN`/`AND_THEN`, or its `DOCTEST_` form) outside an unnamed
  namespace, in any scanned file, headers included. No waiver covers a test macro;
- in a test source, any declaration or definition at namespace scope outside an unnamed namespace,
  including a macro call that ends one without a `;`. **The declaration rule applies only to test
  sources:** `.cpp`/`.cc`/`.cxx` files that contain a test macro. Support sources without one (mains,
  child processes, plugins, shared helpers) and headers are held to the test-macro rule only. This
  narrowing was confirmed by the Director in the PR #17 review. Exempt: preprocessor lines,
  comments, `using` directives and declarations, aliases, and waiver regions:
  ```
  // helios-lint: outside-anon-namespace begin (<reason>)
  ...
  // helios-lint: outside-anon-namespace end
  ```
  The reason must contain a letter or digit. Every waiver is printed on each run, and a region may
  not nest, change brace depth, or be closed by a `}`;
- anything the lexer cannot follow:
  - unbalanced braces;
  - an `#endif` without its `#if`, or an `#if` without its `#endif`;
  - an unterminated block comment, literal, raw string or waiver region;
  - a malformed `helios-lint` comment;
  - a line the lexer tokenizes that is over 500 characters and has more than 500 backslashes, `*`
    and `'`, counted in its comments too, so a 600-`*` banner comment fails. This is CMake's regex
    recursion bound: a 1 MB stack overflows at about 2,500. Lines the lexer only searches for a
    terminator (inside a block comment, raw string or spliced `//` comment, and directive or
    `#if 0` lines without a comment or quote) are not bounded and need not be;
  - no test macro at all in the repository run.

The lexer handles comments, string, character and raw string literals (with encoding prefixes),
digit separators and line splices. A `#` first on its line, after blanks and comments, starts a
preprocessing directive, and only its comments and literals are lexed. So a block comment that a
directive opens hides the next line, as it does for the compiler, and a `#` on a line that a
splice continues is not a directive. `#if 0` groups are skipped the same way: comment-aware, and
with the conditionals nested in them counted. The conditionals are counted, so a file whose
conditionals do not pair up as the lint sees them fails.

Limits:

- **Macros are not expanded.** A macro-generated test is caught only as a macro call at namespace
  scope in a test source. It is missed in a header, in a waiver region, and in a file whose only
  tests are macro-generated. A macro that expands to a brace or to `namespace {` is not seen:
  `#define CLOSE }` before a global `TEST_CASE`, and `#define OPEN namespace {` after it, pass.
- **Conditionals other than `#if 0` are not evaluated.** Both branches are lexed, so an opener in
  each is reported as unbalanced braces.
- **Directives it does not recognize can pair differently.** A directive name split by a line
  splice (`#en\` then `dif`), a `%:` digraph, or a form feed or vertical tab before the `#` is not
  a directive to the lint, so an `#if`/`#endif` pairing that relies on one can differ from the
  compiler's and pass. A directive inside a macro call's arguments is undefined behaviour in C++;
  the lint treats it as a directive, as GCC does.
- **Declarations in headers and support sources are not checked** (see above).
- **Run time is quadratic in three constructed corners.** 20,000 nested braces take about 18 s, and
  40,000 about 70 s. 4,000 raw strings on one line take about 19 s. 50,000 `/**/` comments at the
  start of one line take about 3 s (5,000 take 0.1 s). Real code is far from all three: the
  repository run takes about 4 s.

## ISA audit on MSVC and clang-cl

The flag check reads `compile_commands.json`, which the Ninja presets (`windows-msvc-*`,
`windows-clang-cl`) produce, and understands `/arch:AVX*`. The object checks need GNU binutils; on
MSVC the equivalent is `tools/ci/msvc_gate_audit.ps1` (run by the Windows CI jobs after the build):

- `dumpbin /disasm:nobytes cpu_gate.c.obj cpu_gate_hook.c.obj` — no mnemonic starting with `v`, no
  `ymm`/`zmm` operand, no BMI/LZCNT/TZCNT/POPCNT/MOVBE/SSE3+ instruction;
- `dumpbin /symbols` — `External` symbols are only `helios_cpu_gate_run` and
  `helios_cpu_gate_crt_entry` (defined) plus `HELIOS_ISA_GATE_ALLOWED_IMPORTS` (`UNDEF`); no
  `__security_cookie` / `__security_check_cookie` (the gate TUs build with `/GS-`).
- The image check of 02 §1.1 item 3 (the gate is the only non-CRT `.CRT$XCA`–`.CRT$XCT` contributor,
  nothing in `.CRT$XD*`) needs the linker map (`/MAP`); it lands with the client and cell
  executables (WP-0.17, WP-0.14).
- Base-image disassembly (item 4) and the Intel SDE emulation runs (item 5, CL-17) start with the
  launcher (WP-0.17) and the nightly tier (WP-0.3).

## Plan conformance

Plan-Rev: 3

`isa_audit.cmake` was written to plan revision 3, before the ADR-011 amendment (revision 4). Its open deltas
are in 09 §5.10.4 (b), and WP-0.2r reworks them, so this README keeps revision 3 (D7). The layering, licence,
IP-name and manifest lints have no open delta: WP-0.2 checked them against revision 11 (02 §1.1, 01 §4.1,
§5.2; revision 11 changed 04 §3.2 and status sections only) and added the `HELIOS_MODULE_ORDER` completeness,
row-flag, row-peer, single-row and `apps/` executable checks. `vendor_patches.cmake`
(WP-0.10r) was written to plan revision 6, `test_namespaces.cmake` (WP-0.1) to revision 10, and
`concept_refs.cmake` with the `docs/concept/` scope of the IP-name lint (an owner request, no WP) to
revision 12 (01 §5.2, 09 §4.2).
