# tools/lint — repository lints (WP-0.2)

Every lint is a CMake script (`cmake -P …`): it runs wherever CMake runs, including Windows
developer machines without Python. `cmake/HeliosLayering.cmake` registers them as CTests (label
`lint`) at the end of configure, and `tools/ci/run_lints.cmake` runs the build-independent ones in
one go (CI fast path, pre-commit hook).

```
ctest --test-dir build/<dir> -L lint --output-on-failure      # all lints + their fixtures
cmake -P tools/ci/run_lints.cmake                                # licences, patches, IP names, manifest (no build)
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
| Module layering | `cmake/HeliosLayering.cmake` (configure time) | upward or unlisted same-layer dependency, cycle, `HELIOS_MODULE_ORDER` not topological, a HEADLESS module reaching a non-HEADLESS module or a graphics library (volk, VMA, SDL3, ImGui, …), an `EDITOR_ONLY` module in a client/launcher/bot/cell/gateway/voice executable or under a runtime module, a server executable (cell, gateway, voice, bot) linking anything non-HEADLESS | 02 §1.1, RT-09 |
| ISA audit | `isa_audit.cmake` + `cmake/isa_allowlist.cmake` | AVX-class flags on a unit outside the allowlist (`tp_jolt`, `*_avx2.c(pp)` kernels; `/clang:`-forwarded flags included); FMA or AVX-512 on an allowlisted unit; any flag above x86-64-v1 on the CPU-gate units (`-msse3`…`-msse4.2`, `-mpopcnt`, `-mcx16`, `-mavx*`, `-mbmi*`, a `-march` other than `x86-64`, `/arch:` other than SSE2); `-march=native`, fast-math or FP contraction anywhere; a VEX/EVEX/opmask/BMI/LZCNT/POPCNT/MOVBE/CMPXCHG16B/AES/SHA/SSE3+ instruction (prefixes such as `lock` looked through), a weak/COMDAT/IFUNC symbol, an unlisted export or import (stack-protector and sanitizer symbols included) in a gate object; a gated ELF executable whose single `.preinit_array` entry is not the gate's `hcg_gate`, or that has `R_X86_64_IRELATIVE` relocations | 02 §1.1 "The audit", RT-09, CL-17 |
| Licences | `licenses.cmake` + `license_policy.cmake` | a dependency without a top-level licence file, a licence file (nested ones too) that is copyleft/unknown (GPL family, MPL, EPL, Artistic, CC-BY-SA/NC/ND, JSON licence, …) and not one option of an explicit multi-licence choice naming a permissive licence, a dependency without its own row in `third_party/MANIFEST.md` (Name cell, its first word or the upstream repository name), a MANIFEST licence identifier outside MIT/BSD/zlib/Apache-2.0/Boost/ISC/PostgreSQL/public domain (CC0) | CLAUDE.md, 01 §5.2, ADR-012 |
| Vendored patches | `vendor_patches.cmake` | a `third_party/<dep>/patches/` file not named `NNNN-<slug>.patch`, with no hunk, or not a row of the table under `third_party/MANIFEST.md`'s `### … (third_party/<dep>/patches/)` heading; a row there naming a missing file; a hunk whose post-image (context plus added lines) is not in the committed file, in hunk order; a truncated or malformed hunk; a patched file that is missing, or that the patch deletes but still exists. Line endings are normalized, so CRLF checkouts pass; no git or network. It does not see an in-place edit outside every hunk: re-running `tools/vendor/fetch_third_party.sh <dep>` and diffing `third_party/` is the full proof | CLAUDE.md (vendored code), `third_party/MANIFEST.md` "Patches", K10 |
| IP names | `ip_names.cmake` + `ip_names_policy.cmake` | a *Cinder Reach* name (Kestrel, Harrow, Tallis, …) outside `content/`, also inside identifiers and paths (`KestrelHull`, `cinder_reach`, `TALLIS_ORBIT`, `hull/kestrel`); an in-universe name from Star Wars, EVE, Destiny or Star Citizen anywhere; a franchise title inside `content/` | 01 §4.1 rule 2, §5.2 |
| Windows manifest | `windows_manifest.cmake` | `engine/platform/win/helios.manifest` without UTF-8 code page, PerMonitorV2, longPathAware, Windows 10/11 supportedOS or asInvoker; on Windows builds, `core_tests.exe` not embedding it | ADR-011, 02 §2.1 |

Each lint has seeded-violation fixtures under `tests/` that must be rejected with a specific message
(`lint_*_fixture_*`, `lint_isa_disasm_*`, `lint_isa_image_*`, `lint_layering_*`), so a lint that
silently stops matching fails CI. The fixtures run through `expect_fail.cmake`, which also requires a
non-zero exit: a lint that prints the finding but exits 0 fails its fixture. The layering check looks
through generator expressions (`$<BUILD_INTERFACE:…>`, `$<LINK_ONLY:…>`, conditions), so a dependency
cannot hide in one.

## Waivers

Waivers live next to the rules and carry a reason; every waiver is printed on each run.

- `license_policy.cmake`: HIDAPI's GPL-3.0 and original-licence files inside SDL3 (tri-licensed, used
  under BSD). ISC (netcode's bundled libsodium subset) and the PostgreSQL licence are allowed by
  01 §5.2's scanner allowlist; CLAUDE.md's shorter list should name them too.
- `ip_names_policy.cmake`: reference names in engine test data and doc examples that predate the
  lint: `engine/ecs/tests/test_command_buffer.cpp` ("Kestrel"), `engine/reflect` (`hull/kestrel`,
  `ship.kestrel.name` in `record.h`, `types.h` and two tests) and
  `engine/authority/tests/test_lease_ag.cpp` (zones "tallis", "harrow"), and the cell/gateway
  runtime in development (`engine/server/`, `apps/cellserver/`, `apps/gateway/`: default zone
  `"tallis"`, test zones "tallis"/"harrow"; directory waivers). Their owners rename them, then the
  waivers go.

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
are in 09 §5.10.4 (b), and WP-0.2r reworks them. The layering, licence, IP-name and manifest lints have no open
delta. `vendor_patches.cmake` (WP-0.10r) was written to plan revision 6.
