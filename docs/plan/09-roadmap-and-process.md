# 09 — Roadmap, Build Loop, Testing and Risks

> **Status:** draft v5 (round-2 review fixes: merge policy by branch class, §5.2a; plan-change propagation,
> conformance rework and the conformance lint, §5.10; repository status refreshed, §8.1. Round 3, from the
> other sections' fixes: owners and phase exits for RT-20…22, ED-22, GP-4d, GP-15, BE-A20, NS-3.11/3.12 and
> RT-12's new clauses; the ISA-level deltas in §8.1; carries in the content merge tier, §5.2a. Round 4: the
> in-tree ISA and CPU-gate code handled as plan drift, with WP-0.2r, WP-0.5r, CONF-11/12 and D7, §5.10; §8.1
> refreshed; project schema publishes in the content tier, §5.2a; WP-2.16 split into sized sub-WPs with a DX
> schema lane and K38, §2.3a, §3.1; from the other sections' round-4 fixes: gateway relocation, `DrainGateway` and the
> fence budget in WP-4.3 and 4.4; determinism, co-location, `MigrationHold` and trunk deliverables in WP-1.5, 1.6,
> 2.4, 3.1 and 4.3, the Intel lab box and per-run replicant model (§4.3.1–4.3.2), and §6's Determinism row and
> K4; housing, cities and territory in WP-3.2, 3.6, 3.12 and 4.5, the exits and §2.7.4 (GP-16, GP-17); the
> world-script editor integration and ED-23 in WP-3.12). Round 5 (minor revisions, after the approval): §8.1
> and §8.2 rewritten from the lead's verified status. K2 fired, with ADR-004a opened (`docs/adr/`) and
> WP-1.1a added. K3 records spike (a). The new K39 (Luau fuel metering) fired, and WP-0.10r was added.
> WP-0.7 is marked done with its remaining emitters in WP-0.7b. §5.10.4 gains part (c), for code ahead of the
> round order, and (b) gains a refreshed gate row and a backstop row. D1 gains the `PLAN-REV` counter and D6
> gains a mechanical status check. Hand-offs from 02, 04, 05, 07 and 08 land in WP-0.5, 0.5r, 0.9, 1.5, 2.2,
> 2.4, 2.13, 3.1 and 3.8, the Phase 3 and 4 exits, and §6. **Conforms to:** ADR-001…016, ADR-004a (open) and the phase vocabulary in
> `README.md`.
> **Owns:** the phased roadmap, work packages (WPs), exit criteria, the developer-experience track (§2.7), the
> sponsor budget and funding gates (§4.3), the agent build loop (including the merge policy and the rule that
> re-baselines code when the plan changes), the test strategy, the risk register and the BENCH/scorecard lab
> (01 §3.2).
> **Criteria IDs.** `AAA-*` (01 §3), `RT-*` (02 §8.2), `RC-*` (03 §9.3), `ED-*` (07 §5.2) and `CL-*`
> (08 §4.4) are used as defined. Three sections number criteria without prefixes, so this section uses
> aliases: **`NS-p.k`** = the row with that ID in 04 §11.4, which lists every ID explicitly and only
> appends; **`BE-An`** = 05 §10 row A*n*; **`GP-n`** = 06 §12.2 item *n*.

## 0. Principles

1. **The scorecard is the plan.** A WP exists only to turn failing criteria green. The loop always works
   the highest-priority failing criterion (01 §3).
2. **Thin, end-to-end, every phase.** *Cinder Reach* is playable on Windows and Linux at every phase exit.
3. **Interfaces before fan-out.** Shared contracts (schema grammar, ECS API, RHI API, wire formats) merge
   first as small interface WPs; implementations then run in parallel.
4. **Honesty over optimism.** Criteria that need hardware, people or lawyers stay red until that evidence
   exists. Nothing is waived, and forecasts are re-baselined at every phase gate.

---

## 1. Roadmap overview

| Phase | Goal | Demo milestone | Human-studio calendar | Key gates |
|---|---|---|---|---|
| 0 Foundations | Every part exists as a skeleton on every toolchain | **M0 "Handshake"** | 9–12 months | PLT-1/2, REN-7 |
| 1 First Light | Vertical slice: launcher → cell → client → editor | **M1 "Descent"** (BENCH-2) | 12–18 months | REN-5, REN-8 (Ph1 panel), ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1, TOOL-7 (Ph1) |
| 2 Alpha Sandbox | Economy, quests, social; 500/zone; patching; SDK and project upgrades | **M2 "Osk Yard"** | 15–21 months | SRV Ph2, SEC-2/3/6, CNT-7, TOOL-2/6, TOOL-7/8 (Ph2) |
| 3 Beta Scale | Multi-cell, instances, 5k CCU, live co-editing; 3 starter templates | **M3 "Harrow Orbit"** (BENCH-4/5/6) | 18–24 months | SRV Ph3, REN-8 (Ph3 panel), STB-5, TOOL-3/5, TOOL-7/9 (Ph3), ITR-7 |
| 4 Launch Quality | **The AAA bar** | **M4 "Vane"** (BENCH-1…6, MIN and REF) | 18–24 months | every criterion with Ph ≤ 4 |
| 5 Ambition | Meshing, 100k shard, seamless galaxy, UGC, RT | **M5 "Lattice"** | 18–30 months | SRV Ph5, REN-6 (RT) |

**Phase exit rule (01 §3).** Phase *N* is done when every criterion with Ph ≤ *N* passes three consecutive
nightly runs on Windows and Linux (with the evidence classes of §5.6), the full-tree conformance lint has no
finding and no conformance-rework WP is open (§5.10), and the phase-exit review (§5.7) scores ≥ 9/10. Phases overlap: a Phase *N+1* WP may start once its dependencies are merged, but Phase *N+1*
cannot exit before Phase *N*.

**Rolling-wave detail.** Phases 0–2 are planned as WPs of 1–6 engineer-weeks (human-studio equivalent, the
unit of §4.1); Phases 3–5 are epics, decomposed when the phase starts. Any WP may split into sub-WPs
(`WP-1.7a`) that inherit its criteria. **A Phase 0–2 scope estimated above 6 engineer-weeks is split** into
sub-WPs, each with its own dependencies, acceptance and size, before the Director assigns it; WP-2.16 is split
this way in §2.3a. At round selection (§5.2 step 1) the Director re-estimates every ready Phase 0–2 WP and
splits any that has outgrown the cap.

Rows are grouped by **track**: **CR** Core/Runtime, **RD** Rendering, **NS** Networking/Servers, **BE**
Backend, **GP** Gameplay, **ED** Editor/Tools, **CL** Client/Launcher, **CT** Content/Sample game, **DX**
Developer experience (docs, SDK, project upgrades, starter templates; §2.7), **IN** Infra/CI (including
release engineering and legal).

**Funding gates.** Each phase exit is also a funding gate (**F0** at the Phase 0 exit, then F1–F3, and F4 only if
Phase 5 is chosen). The user
approves the next phase's envelope for the human-supplied inputs H1–H8 and the loop's running costs (§4.3)
before that phase's human-gated WPs start.

---

## 2. Phases and work packages

### 2.1 Phase 0 — Foundations

**Goal.** CI green on every toolchain; each of the six product parts exists as a skeleton; the riskiest
foundations (flecs, mimalloc heaps, determinism, lavapipe goldens, Go↔C++ tokens) are measured early.

**Demo M0 "Handshake" (Windows, no Docker).** `helios-backend.exe run` starts every service in process.
`helios-launcher.exe --channel dev` passes the CPU/GPU gate, logs in as `dev1`, and fetches and verifies a
signed, chunked manifest from the local CDN. It hands off to `helios-client.exe`, which renders the RC-1 test
scenes and connects through `helios-gateway.exe` to a headless `helios-cell.exe` ticking an empty *Tallis*
zone on a dilatable clock. `helios-editor.exe` edits the *Kestrel* hull record through transactions, with
undo and crash-journal replay.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-0.1 | IN | CI matrix | 00, 01 §3.10 | — | The ADR-001a 8-job PR matrix: clang-cl, MinGW and headless jobs, and **`windows-msvc-floor`** on the pinned `windows-2022` image (MSVC 14.44 via `msvc-dev-cmd` `toolset: 14.44`) next to the primary `windows-latest` (VS 2026) job; nightly ASan, `windows-vs2026` and `windows-vs2022` MSBuild builds; Go 1.27.1 (Win + Linux); CMake ≥ 3.28 presets; ccache; SARIF; the `main` ruleset and the Integrator's queue script `tools/ci/merge_queue` with the post-merge `merge-policy` check (§5.2a) | AAA-PLT-1 (all 8 PR jobs green; floor and primary run identical tests and determinism hashes); `merge-policy` fails a seeded squash of a `collab/*` test PR and a WP PR that lands as two commits |
| WP-0.2 | IN | Layering, licence, IP and conformance lints | 02 §1.1 | 0.1 | `helios_module(… LAYER n HEADLESS\|EDITOR_ONLY)`; full `HELIOS_MODULE_ORDER`; DAG/HEADLESS/EDITOR_ONLY configure checks; licence scanner; name grep (01 §4.1); **conformance lint** `tools/conformance` with rules CONF-01…12 and the anchor map `tools/conformance/map.jsonc` (§5.10). The in-tree layering and lint code (`cmake/HeliosLayering.cmake`, `tools/lint/`; §8.1) enters through this WP. **ISA levels are not in this WP:** the in-tree ISA code was written to the per-file allowlist that the ADR-011 amendment retired, so WP-0.2r reworks it (§5.10.4). This WP's PR excludes `cmake/HeliosIsa.cmake`, `cmake/isa_allowlist.cmake`, `tools/lint/isa_audit.cmake`, `tools/ci/msvc_gate_audit.ps1` and the gate files | RT-09 (layer part); bad module fails configure; each CONF rule fails its seeded violation fixture and passes the current tree after WP-0.15r, WP-0.2r and WP-0.5r. CONF-11's fixture is a verbatim copy of today's `HELIOS_ISA_AVX2_TARGETS`, `HELIOS_ISA_AVX2_SOURCE_PATTERNS` and `helios_avx2_sources()` |
| WP-0.2r | IN | Conformance rework: ISA levels and the audit (§5.10.4) | ADR-011 amendment; 02 §1.1 | 0.2 (CONF-11, 12) | Reworks the in-tree `cmake/HeliosIsa.cmake`, `cmake/isa_allowlist.cmake`, `tools/lint/isa_audit.cmake`, `tools/ci/msvc_gate_audit.ps1` and their fixtures from per-file AVX2 kernels to **whole-image levels**. (1) `HELIOS_ISA_AVX2` and `HELIOS_ISA_BASE` level sets (today's `helios_isa_avx2_flags()` and `helios_isa_base_flags()` already emit 02's flags). (2) The level on `helios_executable`, whose in-tree `ROLE` picks it (client, cell, gateway, voice, editor, bot, tool, sample and bench → `avx2`; launcher and bootstrap → `base`; an explicit `ISA avx2\|base` only for fixtures), applied to the image and to every engine module, gem, game module and third-party library it links. (3) `<module>@base` object libraries for `core`, `app`, `ui`, `text`, `loc`, `patch`, `crash` and their third-party libraries, configured only when the launcher or bootstrap is; a configure error when a `base` image links `physics`, `pcg`, `tp_jolt` or any other `avx2` library. (4) `tp_jolt`'s ISA compile options removed from its PUBLIC interface (its `JPH_USE_*` defines stay PUBLIC). (5) `helios_avx2_sources()`, `HELIOS_ISA_AVX2_TARGETS` and `HELIOS_ISA_AVX2_SOURCE_PATTERNS` deleted, so `isa_allowlist.cmake` keeps only the self-dispatch symbols and the gate's export and import lists; a new `cmake/pre_main_allowlist.cmake` names the attributed pre-`main` hooks (mimalloc's `.CRT$XLB`, `.CRT$XLY` and `.CRT$XIB` entries; Tracy's `.CRT$XCB` statics and `.CRT$XD*` `thread_local`s; Helios `.CRT$XCU` initializers). (6) The **five-check audit** of 02 §1.1 as `helios-tool isa-audit` (`--objects`, `--pre-gate`), with the CMake script kept as its Phase 0 driver until WP-0.18: check 1 per image level; check 2 on the gate objects (no `__security_cookie` or `__asan_*` reference); check 3 on every `avx2` image in both link flavours, with and without `HELIOS_PROFILE`, covering Windows (a) enumeration of every TLS callback, `.CRT$XI*`, `.CRT$XC*` and `.CRT$XD*` entry and `_pRawDllMain`, (b) the gate as `AddressOfCallBacks[0]`, (c) attribution of every other entry, and (d) load order, plus on Linux one `.preinit_array` entry, no `R_X86_64_IRELATIVE`, no `.init_array` priority below 101 and the `.dynsym` allowlist; check 4 on a `base` fixture image now and on the launcher and bootstrap from WP-0.17; check 5 (SDE and qemu, CL-17) on the gate test images now and on each `avx2` executable as it appears. `ci.yml` calls the Windows half on the MSVC and clang-cl jobs | RT-09 (AVX2 part). Each check fails its seeded fixture: a TU of an `avx2` image built at the default level; a `base` fixture image that links `tp_jolt` (configure error); a second `.CRT$XLA0` contribution linked before the gate; an unlisted AVX2 `.CRT$XIB` initializer; an avx2-built DLL outside the gate image's import closure; a canary `_pRawDllMain`; an unlisted AVX2 symbol in a `base` fixture image; a gate image under SDE `-snb` that must exit 78, never SIGILL. Check 3 passes on the real mimalloc and Tracy hooks. CONF-11 and CONF-12 are clean on `cmake/**`, `third_party/CMakeLists.txt`, `tools/**` and `engine/core/**`; `ctest -L lint` is green on all 8 PR configurations |
| WP-0.3 | IN | Nightly and scorecard | 09 | 0.1 | `scorecard.jsonc` (criterion → tests, class, platforms, threshold); nightly workflow; perf history and comparator; `tools/milestone/validate.ps1` | Nightly report covers every Ph0 criterion |
| WP-0.4 | IN | GPU runner (**human action**) | 03 §8.3; §4.3, K33 | 0.3 | User's Windows PC as self-hosted runner `win-gpu`, hardened per **§5.4a**: post-merge code only (`schedule`, `push` to `main`, user-run `workflow_dispatch`; never `pull_request*`); a dedicated non-admin `helios-ci` account or a Hyper-V GPU-P VM; no secrets; a wiped workspace per job. `tools/ci/check_runner_policy`. REF/MIN/SERVER purchase list priced against §4.3 H1/H2 | Vulkan goldens nightly on a real GPU; the policy check fails a workflow that targets `win-gpu` and has a PR trigger or reads `secrets.*`; a runner job asserts it is not in Administrators |
| WP-0.5 | CR | Core and math completion | 02 §2.1, §2.2 | 0.2 | CPUID (`core::cpu`), process spawn, async reads, exe manifests; `det::exp/ln/pow/asinh`, `Q16`, `Q32`, `Fixed64`, integer `rsqrt`; **02 §2.2's sharded, batched `MemoryTag` accounting** and the heap lifetime rule for every core `mi_heap_*` user (spike (a): exact accounting collapsed to 460–890 ns per pair at 4 threads, and core `alignedAlloc` ran ≈ 30× slower than mimalloc; `engine/ecs/SPIKES.md` §1). Most of this is in the working tree (§8.1) and enters through this WP. The CPU gate (`core::cpuGate()`, the gate TU and its pre-initializer hooks) is carved out into WP-0.5r, because the in-tree gate was built for the retired per-file model and sits in the wrong Windows slot (§5.10.4) | `det::` hashes equal on 5 compilers; CPUID, process and async-read tests on Windows and Linux; the manifest lint; the `core_tests` heap-recycling regression; tagged allocation ≤ 3× `mi_malloc` at 4 and 16 threads (02 §2.2) |
| WP-0.5r | CR | Conformance rework: CPU gate (§5.10.4) | ADR-011 amendment; 02 §1.1; 08 §2.2 | 0.2r | Reworks the in-tree gate (`engine/core/src/cpugate/*`, `platform/{win32,posix}/cpu_gate_hook.c` and the gate wiring in `engine/core/CMakeLists.txt` and `helios_executable`) to 02 §1.1. **Windows placement:** the entry `helios_cpu_gate_tls_entry` moves from `.CRT$XIB` to `.CRT$XLA0`, the image's first TLS callback, ahead of mimalloc's `.CRT$XLB` and the CRT's `.CRT$XLC`; the object pulls in `/INCLUDE:_tls_used` and `/INCLUDE:helios_cpu_gate_tls_entry` (a `_tls_used` reference and a `used` attribute under MinGW); it checks on `DLL_PROCESS_ATTACH` and returns at once for the other reasons. **Failure path under the loader lock:** stderr, then `MessageBoxW` only for `WINDOWS_GUI` executables (`GetModuleHandleW`) unless `HELIOS_CPU_GATE_SILENT=1`, then `TerminateProcess(GetCurrentProcess(), 78)` instead of today's `ExitProcess`, which would run mimalloc's `.CRT$XLY` detach hook; the backstop exits the same way. **Gate objects:** `/GS-` and `-fno-stack-protector`, no sanitizer or coverage instrumentation, and three external symbols (`helios_cpu_gate_run`, `helios_cpu_gate_verdict`, `helios_cpu_gate_tls_entry`); `core::platformInit()` stops with "CPU gate did not run" unless the verdict reads *pass*. The Linux `.preinit_array` entry stays. The in-tree `helios_executable` already links the gate by role. The rework rejects `NO_CPU_GATE` on `avx2` images (configure error), keeps the gate in the executable for monolithic images, moves it into `helios_runtime` for modular dev images once WP-0.6c lands, and makes `helios_cpu_gate()` internal to `cmake/HeliosIsa.cmake`. The gate keeps its C form, which 02 §1.1 now names. **Round-5 backstop rules (02 §1.1):** the illegal-instruction backstop keeps its trap passthrough (`ud2`/`ud1`/`ud0`, already in both hooks) and adds the VEX/EVEX and POPCNT classifier, so only a genuine ISA fault gets the CPU message; the crash handler takes #UD over when it installs (Linux: its handler replaces the backstop and re-raises with `SIG_DFL`, where today's `posix_crash.cpp` restores the backstop; Windows: a first-in-chain vectored handler sends non-trap #UD to the dump path); check 5's `ud2` and EVEX fixtures before and after crash-handler install | CL-17 (early): `core_cpugate_child_snb` prints the message and exits 78 on Windows and Linux, and a GUI-subsystem fixture under `HELIOS_CPU_GATE_SILENT=1` exits 78 without a dialog. A `ud2` trap and an EVEX fault after crash-handler install both produce a minidump, never the CPU message. Audit checks 2 and 3 are green on the gate test images in both link flavours. A Windows fixture image that links mimalloc and adds its own `.CRT$XLB` callback finds the gate's verdict already set when that callback runs. A build with the hook dropped fails its first smoke on the "CPU gate did not run" check. CONF-12 fails a seeded `.CRT$XIB` gate and a seeded `ExitProcess` call |
| WP-0.6 | CR | Spikes (3 weeks) | 02 §1.4, §2.2, §4.2; ADR-004, ADR-016 | 0.5 | (a) mimalloc v3 heap thread-affinity under job migration; (b) flecs 4.1.6 `DontFragment` + synthetic 50k-entity pre-benchmark; (c) **link model**: `HELIOS_MODULAR`, `windows-msvc-dev` + `linux-dev` presets, three link groups with `HELIOS_*_API`, `/MD` dev CRT, reload loader, symbol audit, `Probe` game DLL | Decisions in `docs/adr/`; pre-RT-01 within 2× budget, else custom-ECS ADR opened; **RT-18** (else ADR-016 reopened, snapshot-restart fallback). *Outcome of (a) and (b), 2026-09-25:* `engine/ecs/SPIKES.md`; the pre-bench failed the structural-ops clause at > 2×, so **ADR-004a is open** (K2) |
| WP-0.7 | CR | `helios-schemac` v0 + `reflect` | 02 §3 | 0.2 | 02 §3.1 grammar; `schema.lock`; emitters `cpp`, `go`, `luau`, `sql`, `repl` (full state), `lint`; `TypeInfo`, JSONC/tagged codecs. *Closed 2026-09-25 with the `cpp`, `go` (byte-identical) and JSON emitters; the others moved to WP-0.7b* | 01 §5.4 schemac clause (with WP-0.7b); ≤ 1 s per 2,000 types; lock reuse fails |
| WP-0.7b | CR | schemac Phase 0 emitters | 02 §3 | 0.7 | The `luau`, `sql`, `repl` (full state) and `lint` emitters, which are stubs today that report "not yet implemented". The other stubs go with the WPs that consume them: `records` with WP-0.8, `editor` with WP-0.18, `docs` with WP-1.24, and `proto` with the first WP that needs it | 01 §5.4 schemac clause: C++, Go, Luau and SQL from one package; the lock and golden tests per emitter |
| WP-0.8 | CR | ECS, records, assets v0 | 02 §3.3, §4, §6 | 0.7, 0.6 | flecs wrapper (IDs, registry, command buffers); client/server `.hrdb`; `.meta`, local DDC, `.hpak` v0; fuzz harnesses | AAA-SEC-4 on sample records |
| WP-0.9 | CR | Physics and PCG base | 02 §5.8, §7.1; 03 §5.5a | 0.5, 0.2r (image levels); 0.12 (twin) | One-grid `engine/physics` with stable body keys in `mUserData` and the vendored `third_party/jolt/patches/stable-order` (02 §7.1); fixed-point `hnoise` + 32-bit-only Slang twin + corpus; sub-WP **WP-0.9c `hnoise` throughput spike**: reference 40-node graph, GPU tiles per 0.8 ms on `win-gpu` + lavapipe (MIN GTX 1660 S / RX 5600 XT confirmation before WP-1.8 starts), CPU VM AVX2 SoA ms per tile per core, run on the default `vm_avx2.cpp` kernel (asserted from `pcg.kernel`), with the 4-lane twin hashing identically (02 §5.8) | RT-03 (Ph0 scope); RT-04 harness; WP-0.9c outcome (green / F2 / F3, 03 §5.5a) in `docs/adr/`, K5b armed |
| WP-0.10 | CR | Luau host v0 | 02 §7.4 | 0.2 | VM, sandbox, fuel-metered interrupt budget (04 §10.2) with **no involuntary yields**, sticky `fuel_kill`, `task.checkpoint`, `StaleHandle` checks, heap caps; tests for a kill and an exhausted budget inside a metamethod, a `table.sort` comparator and a C++→Luau callback, and for a handle invalidated across a `wait` | RT-13. *Done 2026-09-25 except the fuel-identity and overhead clauses, which moved to WP-0.10r* |
| WP-0.10r | CR | Conformance rework and RT-13 closure: Luau fuel metering (K39) | 02 §7.4; 04 §10.2 | 0.10 | The two vendored patches that 02 §7.4 (round 5) requires, recorded in `third_party/luau/patches/` and `MANIFEST.md`. (1) **`codegen-fornloop-fuel`:** Luau 0.739's code generator puts the numeric-`for` interrupt at the top of the loop body instead of in `FORNLOOP` (`CodeGen/src/IrTranslation.cpp`), so native code charges +1 fuel per loop left by `break` or `return`; the patch emits it in `FORNLOOP` as the interpreter does. (2) **`fuel-counter`:** an inline counter decremented at each `gc < 0` safepoint that calls the host only at zero, replacing the per-safepoint `interrupt` call (measured ≈ 12–15 % against ≤ 10 %). `VmConfig` **refuses** native codegen on cells and world-script hosts until (1) merges (today `create()` warns). Both patches join `det-math` in `sim_abi.script` (04 §6.7) and are rebased on every Luau bump (K10) | RT-13's "fuel counts identical between interpreter and codegen" (the pinned test flips to passing, and fails with patch (1) reverted) and its ≤ 10 % overhead on the perf workload with patch (2); a cell `VmConfig` with codegen fails `create()`; the 79 `script_tests` stay green on every toolchain |
| WP-0.11 | RD | RHI: Vulkan 1.3 + Null | 03 §1 | 0.2, 0.5 | volk/VMA, bindless, SDL3 swapchain (`engine/app`), caps mask, Null traces | RC-1 (triangle, compute, bindless); AAA-REN-7 |
| WP-0.12 | RD | Slang, graph v0, rendertest | 03 §1.7, §2, §8.4 | 0.11 | SHA-pinned Slang bootstrap; `helios-shaderc`; `spirv-val`; graph (barriers, culling, traces); `helios-rendertest` + ꟻLIP | RC-1 (≤ 10 min, render-twice identical) |
| WP-0.13 | NS | HTP transport | 04 §2, §10 | 0.2 | netcode + reliable build, `UdpSocket` Win32/POSIX, channels, NetSim, fuzz targets, trunk profile + throughput gate | NS-0.1, 0.2, 0.4, 0.7 |
| WP-0.14 | NS | Cell and gateway skeletons | 04 §3, §6.5 | 0.13, 0.8 | `ZoneHost`, `ZoneClock` (TiDi), gateway forwarder, AG/epoch/`IFence` interfaces | NS-0.3, 0.6 |
| WP-0.15 | BE | Go backend skeleton | 05 §1.1–1.4, §5, §8 | 0.1; 0.7 (generated types) | `helios-backend` (embedded NATS, PG, miniredis); identity; token minter + vectors; `LocalProcessPlacer`; ledger schema; testkit. Most of this exists in the working tree, built ahead of the round order (§8.1). It enters the merge queue only after WP-0.15r | BE-A1, A2; NS-0.5; AAA-PLT-2 (Ph0) |
| WP-0.15r | BE | Conformance rework of `services/` (§5.10) | ADR-004; 05 §1.4, §2.3, §3, §6.6 | 0.2 (CONF rules) | Brings the in-tree backend to the current plan (draft v5; rounds 3 and 4 added no delta for `services/`, §5.10.4(a)). Already reworked: PG-anchored leadership, lease generations in PG, no `LEASES` bucket, block IDs. Still to do (the open rows of §5.10.4): `svc_identity` and `svc_orch` schema names; `email_ct`, `email_bidx` and `subject_key` with per-account DEKs; the `region_lease` table; `fd` and `server_build` on registration; held regions and generations in heartbeats; README and migration comments updated | CONF-01…09 clean on `services/`; the `conformance/holder_rule` test (CONF-03) passes; `go test ./...` and `-tags integration` green on Windows and Linux; BE-A1, A2 rerun green |
| WP-0.16 | BE | Patch pipeline v0 | 05 §7; 08 §2.5 | 0.15, 0.5 | FastCDC in Go and C++ with shared vectors; `.hman`; Ed25519 trust chain; local CDN; `helios-patch publish` | CL-14 subset (tampered, expired, rolled back) |
| WP-0.17 | CL | Launcher and client skeletons | 08 §1.1, §1.16, §2.1–2.2 | 0.11, 0.14, 0.16, 0.2r (`@base` builds), 0.5r (gate) | x86-64-v1 (`base`, 02 §1.1) launcher with its pre-v2 emulator runs (SDE `-mrm`/`-pnr`, qemu `Opteron_G1`/`core2duo`/`phenom`; 08 §2.1.1) (SDL3 window drawn with `SDL_Renderer` + `SDL_RenderDebugText`, no RmlUi): gate, login, fetch/verify, launch-code stub; client boots RHI and connects. **SDL3 vendoring fix** (`third_party/CMakeLists.txt` today sets `SDL_RENDER OFF` and `SDL_WAYLAND OFF`): `SDL_RENDER ON` with `SDL_RENDER_D3D11`/`D3D12` on Windows (D3D9 off) and the OpenGL (`SDL_OPENGL ON`, `libGL` loaded at run time) and Vulkan renderers on Linux, plus SDL's software renderer as the last fallback; `SDL_GPU` stays off. Only the launcher and `engine/ui`'s SDL_Renderer backend may call `SDL_CreateRenderer` (symbol lint); the launcher sets the driver order `direct3d11,direct3d12,software` on Windows and `opengl,vulkan,software` on Linux. `SDL_WAYLAND ON` with `SDL_WAYLAND_SHARED` and `SDL_WAYLAND_LIBDECOR_SHARED` (loaded at run time) and X11 kept as the fallback (`SDL_VIDEO_DRIVER` overrides). CI Linux images add `libwayland-dev`, `wayland-protocols` and `libxkbcommon-dev` | 01 §5.4 launcher clause; headless login test; **import audit**: `dumpbin /dependents` (Windows) and `readelf -d` (Linux) show no `vulkan-1.dll`, `libvulkan.so.1`, `libGL.so.1`, `libwayland-*` or `libX11*` import in `HeliosLauncher`/`Helios` (all are loaded at run time); the launcher shows its "no Vulkan 1.3 driver" screen on a machine with no Vulkan loader (Windows runner with the loader hidden; Linux container without `libvulkan`) using the D3D11/WARP or software renderer; launcher and client smoke under `SDL_VIDEO_DRIVER=x11` (Xvfb) and `=wayland` (headless Weston) |
| WP-0.18 | ED | ToolsFramework + shell | 07 §1.2–1.4, §4.4 | 0.7, 0.8, 0.11 | Documents, commands, property-path transactions, journal, `helios-tool`; ImGui docking shell; property grid; **`helios-uitest` driver** (item table, `ui.*` input injection over the remote-control socket, deterministic test mode, ꟻLIP UI goldens under Xvfb + lavapipe); Dear ImGui Test Engine licence evaluation recorded | ED-1; ED-15 harness self-test (shell and property grid at 100/200 %, dark and high-contrast) |
| WP-0.19 | GP | Kernel v0 | 06 §1.1–1.2, §4 | 0.7, 0.5 | Tags; attributes/modifiers (Dogma order, stacking); HXL in C++ and Go + corpus; `ItemDef`, `ReasonCodeDef` | GP-1 (corpus clauses) |
| WP-0.20 | CT | *Cinder Reach* skeleton | 01 §4.2 | 0.7 | `content/`, `helios.project.jsonc`, Tallis stub, Kestrel record, provenance policy | Records compile; 100 % provenance |

**Exit.** AAA-PLT-1, PLT-2 (Ph0), REN-7; RC-1; ED-1; NS-0.1–0.7; BE-A1, A2; RT-03 (Ph0 scope); GP-1 (corpus
clauses); RT-13, RT-18; the 01 §5.4 Phase 0 demos; all spike decisions recorded (WP-0.6a–c; WP-0.9c pending its MIN confirmation);
the WP-0.4 runner policy check green; the conformance lint clean on the full tree with WP-0.15r, WP-0.2r,
WP-0.5r and WP-0.10r merged (§5.10); ADR-004a's option A under way in WP-1.1a (the ADR itself closes at the
Phase 1 RT-01 gate);
and the **F0 funding decision** (§4.3.6) signed in `docs/evidence/funding-F0.md`.

### 2.2 Phase 1 — First Light

**Goal.** One thin slice through every layer, at 50 players.

**Demo M1 "Descent" (01 §5.4).** The user logs in via the launcher, undocks a *Kestrel* from *Harrow High*,
descends from 400 km through *Harrow*'s atmosphere with no loading screen (**BENCH-2**), lands, walks into
*Saltmarch* and shoots Hollow drones with prediction, alongside 49 bots. Relogging restores position and the
starter loadout. In the editor they place Saltmarch props, edit the Kestrel record and press F5 for PIE
(ED-2).

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-1.1 | CR | ECS production | 02 §4; ADR-004a | 0.8, 0.6; 1.1a | Access-DAG scheduler, ordered command buffers, `Mut<C>` dirty bits, prefabs; the formal RT-01 run on SERVER (ADR-004a M2–M4) | **RT-01** (closes ADR-004a: accepted (A) if the structural clause passes, custom ECS if it still fails with 1.1a merged); AAA-SRV-4 (Ph1) |
| WP-1.1a | CR | RT-01 structural ops: wrapper optimization (ADR-004a option A; runs first) | 02 §4; ADR-004a | 0.8 (ECS part, committed) | `engine/ecs/SPIKES.md` §3.4 items 1–5. (1) Identity bookkeeping batched per spawn group: EntityId and NetHandle blocks, bulk registry inserts, one `NetIdentity` pass. (2) A direct `componentInfo` array for component ids below 64k, cached in the flattened op list. (3) No children walk on destroy without a hierarchy record, and no `ecs_owns_id` on remove. (4) Specialized command buffers: `spawnN` for homogeneous spawns, and toggles sorted by (source table, id) and moved in batches through an upstream or table-level bulk path inside `engine/ecs`. (5) The `InFrame` storage decision. `ecs_bench` gains a release build with asserts off and a callgrind job. The structural log, identity, dirty bits and deterministic apply order stay unchanged, and the budget stays 9k ops per sync point | ADR-004a M1 (World ≤ 1.6× raw flecs in the same run) on the dev runner, and M5 recorded; `ecs_tests` and the cross-worker state hash unchanged on every toolchain. The Phase 1 midpoint check of ADR-004a §4 applies |
| WP-1.2 | CR | World model, streaming | 02 §5.1–5.7 | 1.1, 0.9 | `FrameGraph`, `Reparent` + `FrameChanged`, grids and transfers, disjoint-cluster bubbles, `ZoneInstance`, containers → `.hcc`, `StreamingManager` | RT-02; AAA-REN-5 (sim); RT-06 (I/O clause); RT-19 (Ph1) |
| WP-1.3 | CR | PCG for Harrow | 02 §5.8–5.8a | 0.9, 1.2 | Terrain-graph VM (level-adaptive octaves), stamps, scatter, Scree fields, twin conformance; collision tiles at the collision level with tile sets, slab gate, `TerrainFence`, holds and client prediction suspension, the `pcg.collision` cache and prefetch budget | RT-04; RT-20 (Ph1 slice); RC-4; RC-13 (CPU clause); AAA-CNT-1 (Ph1), PLT-4 |
| WP-1.4 | CR | Assets, hot reload | 02 §6, §1.2, §1.4 | 0.8, 0.6c | `helios-assetd`, glTF/texture import, records/Luau reload, game-DLL live reload (editor + PIE cell/clients) | RT-05 (Ph1), RT-14; AAA-ITR-1 (Ph1), ITR-5 |
| WP-1.5 | CR | Physics, anim, nav, audio | 02 §7.1–7.3 | 1.2 | Shared `CharacterVirtual` mover, hulls, ozz graphs + hitbox poses through `det::HitboxSampler` (no estimate intrinsics; 04 §10.2), Recast per grid with `qsort` call sites patched to `det::sort`, audio events; `NoCrossUpdateCache` on ShipHull bodies (02 §7.1) | RT-03 (full, with its permuted-`BodyID` variant and resting-creep bound) |
| WP-1.6 | CR | Script, UI, input | 02 §7.4–7.6 | 1.4, 0.10 | Luau tasks, DAP, reload; `scriptlib` glue with per-call fuel charges, charging builtin wrappers and `disabledBuiltins`, `--calibrate-fuel` cost table in the zone profile and replay header (02 §7.4); the Luau `det-math` patch and the `simdet` libm, estimate-intrinsic, sort and `long double` rules (04 §10.2); vendor RmlUi + text stack; view-models; `UiSurface`; input contexts | RT-13; GP-9 (runaway, reload) |
| WP-1.7 | RD | Render pipeline | 03 §1.3a, §2–4 | 0.12, 1.2 | Extract/prepare/submit, `presentation`, GPU scene, 2-phase HZB, MDI, LOD, clustered forward+ PBR, CSM, PSO lists, breadcrumbs; **texture and mesh residency manager** (GPU feedback, CPU prediction, transfer budgets, `StreamTexId` slots) | RC-11 (Ph1: forward+, HDR); RC-12 (Ph1) |
| WP-1.8 | RD | Space and planets | 03 §5 | 1.3, 1.7 | Starfield, nebula IBL, sun, flares, CDLOD + GPU tiles (adopted §5.5a variant, trajectory prefetch), Hillaire atmosphere, 2D clouds, horizon-map far terrain shadows (03 §5.4a) | RC-2, RC-3, RC-13, RC-11 (Ph1 sunset goldens); AAA-REN-1 (Ph1 slice), REN-5, REN-6 (Ph1) |
| WP-1.9 | RD | VFX, post, UI bridge | 03 §6–7 | 1.7, 1.6 | GPU particles, shields, plumes, beams; exposure, bloom, AgX, TAA; RmlUi renderer; diegetic RTT | AAA-REN-6 (Ph1 particles) |
| WP-1.10 | NS | Replication | 04 §4 | 1.1, 1.2, 0.14 | Interest hash, priority accumulator, change masks, serialize-once chunks, frame quantization, RPCs; SEC-1 lint | NS-1.1, 1.2; AAA-SEC-1, SRV-6 (Ph1) |
| WP-1.11 | NS | Netgame, transitions | 04 §5, §7 | 1.10, 1.5 | Time sync, prediction (characters, ships), interpolation, basic lag comp, movement checks, handoff-seam zone transfer, reconnect | NS-1.3, 1.4; CL-4; AAA-SRV-5 (Ph1) |
| WP-1.12 | NS | Bots, inspector | 04 §10 | 1.11 | `helios-bot`; 16 bots per PR, 50 nightly; packet inspector | AAA-SRV-1/2/3 (Ph1) |
| WP-1.13 | BE | Persistence, fence | 05 §1.5, §1.13 | 0.15, 1.10 | `Fence.Advance`, AG trees (`Fence.Join/Leave`, ledger `CreateAg`), dormancy (`Fence.Park`, dormant-fence ledger ops, `released`; 04 §6.1), checkpoints, load barrier, `item_refs` reconcile, character service | BE-A3a, A4 (fence); NS-1.5; AAA-SRV-9/12 (Ph1) |
| WP-1.14 | BE | Core ledger, ops basics | 05 §1.1–1.2, 1.6, 1.14–1.17 | 0.15 | Wallets, grants, custody, idempotency, audits (no trading); launch codes; queue cap; live config; GM audit | BE-A4, A14; AAA-SRV-7 (Ph1) |
| WP-1.15 | GP | Kernel runtime | 06 §1.3–1.6, §8.6–8.7 | 0.19, 1.11 | Effects, abilities, ASM compiler (text graphs), prediction keys, cues, damage, respawn, XP | GP-1, GP-2; ED-4 |
| WP-1.16 | GP | Flight, on-foot, AI v1 | 06 §7, §8.1–8.2, §9.6 | 1.15, 1.5 | FBW + bit-exact thruster allocation (06 §8.2; GP-4a hash in RT-03's job), basic quantum travel, hitscan, BT + perception + navmesh drones, spawners; interactables v1 (`Press` verbs, doors bound to portals, terminal sessions) | GP-4a, GP-12 (Ph1); M02, M03 (on foot; vehicles and mounts in WP-2.9), M05, G13 |
| WP-1.17 | ED | Viewport, world tools | 07 §1.3–1.5, §2.1, §2.6.3–2.6.5, §4.4 | 0.18, 1.2, 1.4, 1.7 | `imgui_impl_helios`, editor passes, T01, T02, T07 (Tallis), T24 MVPs; T01 Volumes mode (portal-cell generation, portals, grid volumes, `Whole` partitions); UI goldens, layout lints and the ED-2/ED-16 UI replays for every Ph1 tool | AAA-TOOL-1 (T01, T24); ED-15 (Ph1), ED-16 |
| WP-1.18 | ED | PIE, code tools | 07 §1.6, T10, T26 | 1.17, 1.12, 1.14 | PIE (cell + gateway + backend + clients + bots, NetSim toolbar); incremental nav tiles live in PIE with `DetourTileCache` obstacles (07 §4.1.1); VS Code/DAP; Tracy overlays | ED-3; AAA-ITR-3 |
| WP-1.19 | ED | Data and planet tools | 07 T04, T08, T09, T15, T16, T27–T29 | 1.17, 1.3 | T08 (incl. collision-matrix and physical-material customizers), T04, T15 (import; `helios-tool fit-hitboxes`), T16 (instances), T27 (GM), T28 (≥ 20 rules incl. Ph1 `phys.*`), T29 MVPs; T09 lite | AAA-TOOL-1; ED-2, ED-5; RT-09 |
| WP-1.20 | CL | Client application | 08 §1 | 1.6, 1.9, 1.11, 1.14 | State machine, threads, settings, gamepad, camera rig, MFDs; Foundation panel contract, `ThemeDef` themes and the Ph1 panels (frontend, basic character creator, HUD, settings, terminal host, system map) with their headless flows and RmlUi lints (08 §1.7); `ProductIdentity` and the `helios-dev` product (08 §2.10) | CL-4, CL-20, CL-23 (Ph1 panels); RT-12 (game thread, window-drag clause); AAA-PLT-3; R06 |
| WP-1.21 | CL | Launcher v1 | 08 §2 | 0.17, 1.6 | RmlUi on `SDL_Renderer`, login, launch code over inherited pipe, download, quick verify, dev installer, tarball | CL-1, CL-17; AAA-PLT-3 |
| WP-1.22 | CT | Phase 1 content | 01 §4.2 | 1.3, 1.8, 1.16, 1.19 | Tallis, Harrow, Harrow High, Saltmarch, Scree, Kestrel, drones, 3 weapons (~150 assets); scripted BENCH-2 | M1; BENCH-2 nightly; AAA-REN-8 (Ph1) capture set and panel |
| WP-1.23 | IN | Lab, release, legal kickoff | 09; 08 §2.8 | 0.3, 0.4 | BENCH-2 on REF (Win + Linux); symbol store; **HSM code-signing procurement started**; FreeType FTL, SIL OFL, libopus approvals; H1 lab installed within the F0 envelope (§4.3.1) | Sign-offs in `MANIFEST.md` |
| WP-1.24 | DX | Docs pipeline v1 | §2.7.1; 02 §3.5; 07 §1.2 | 0.7, 1.6, 1.17 | `schemac --emit docs`; Luau reference from `.d.luau` + binding doc comments; CVar and `helios-tool --help-json` dumps; `helios-docs` static site (Go, goldmark); coverage gate in the PR tier; manual-page template and one page per Ph1 tool; F1 help in the property grid | **AAA-TOOL-7 (Ph1)** |

**Exit.** Every criterion with Ph ≤ 1: AAA-REN-1 (Ph1 slice), REN-5, REN-6 (Ph1), REN-7, REN-8 (Ph1 panel); ITR-1 (Ph1), 3, 5;
SEC-1, 4; PLT-1, 2 (Ph0), 3, 4; TOOL-1, TOOL-7 (Ph1); SRV-1…7, 9, 12 at Ph1 values; CNT-1 (Ph1); RT-01…05 (Ph1 scope), 09,
12 (game-thread clause), 13, 14, 19, 20 (Ph1 slice); RC-1…4, RC-11 (Ph1), RC-12 (Ph1), RC-13; NS-1.1–1.5; BE-A3a, A4, A14; GP-1, 2, 4a, 9, 12 (Ph1
clauses); ED-2…5, 15 (Ph1), 16; CL-1, 4, 17, 20, 23 (Ph1); funding gate F1 (§4.3.6).

### 2.3 Phase 2 — Alpha Sandbox

**Goal.** The sandbox loop on the full ledger, 500 players per zone, production patching, and every P0 tool.

**Demo M2 "Osk Yard".** In *Osk* the user surveys a resource, places a harvester, crafts a rifle with rolled
perks and sells it on the Osk Yard market to a second account (escrow visible in `/admin`). They take
"Signal from Saltmarch" with dialogue, clear a Hollow lair and chat in a guild while 500 bots fill the zone.
The signed launcher then patches a 1 % content change by CDC and self-updates. **BENCH-1** (Harrow High
concourse, 200 bot avatars) joins the nightly lab as the first crowd measurement.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-2.1 | CR | Streaming, cook, paks | 02 §5.6–5.7, §6; 03 §1.3a | 1.2, 1.4 | HLOD, data layers, residency hook, all-asset reload, shared DDC, paks, zone cook; `.htex` mip ranges and mesh LOD ranges in paks; render residency on the `StreamingInstaller` demand path | RT-05 (Ph2), 08, 17; RC-12 (Ph2); AAA-ITR-2/4 |
| WP-2.2 | CR | Runtime breadth | 02 §4.3, §7 | 1.5, 1.6 | Update LOD, entity budgets, vehicles (Jolt `VehicleConstraint` and hover step listeners, 60 Hz collision steps in vehicle bubbles, `SingleBodyPredictor` with a constraint's `SaveState`; 02 §7.1, 06 §8.1a), IK and retargeting, audio banks/occlusion, nav tiles, IME, loc, rebinding, crashpad; small-bubble stepping (02 §7.1); `NoCrossUpdateCache` on Vehicle bodies (02 §7.1) | RT-10 (Ph2), RT-16, RT-20 (500 players with vehicles) |
| WP-2.3 | RD | Render Phase 2 | 03 §9.2 | 1.7–1.9 | Meshlet culling, impostor baker, liveries, clustered decals, material graph → Slang, probes, SSR, `IUpscaler` + FSR 2, shader reload, VRAM budgets, character composite tiers + cache + GPU BC encoders, appearance rest-mesh bakes + cache and the tiered skinned-output ring (03 §7.6a) | RC-5, RC-6; AAA-ITR-1 (Ph2) |
| WP-2.4 | NS | 500 per zone | 04 §2.5, §5.6–5.7, §7–8 | 1.11, 1.12 | AIMD, deltas, LOD groups, lag comp + projectiles, predicted abilities, TiDi, degrade stage, multi-instance cells, overflow layers, trunk sharding with the fixed trunk IO pool and per-trunk buffer sizing (04 §2.6), `SimInbox` + replay with replay keyframes and the cell script rebase (04 §10.2; NS-2.4's keyframe clause), `det::sort` and the asserted MXCSR policy (04 §10.2) | NS-2.1–2.6; AAA-SRV (Ph2), SEC-3 |
| WP-2.5 | BE | Economy services | 05 §1.6–1.9, §4 | 1.14 | Full ledger (items, trades, escrow, `AtomicSwap`), market actors, industry, timers, resources, mail | BE-A5 (2k), A6, A7, A10; AAA-SEC-2, SRV-8 |
| WP-2.6 | BE | Social, ops, crashes | 05 §1.10–1.11, §1.16, §6; 08 §3 | 1.14 | Chat, presence, corps RBAC; queue lanes; `crashgw`, Sentry, symbol server; dashboards; warm standby (`Fence.AdvanceOwnedBy`, persistence fence cache); two-signal failure detection with failure-domain registration and the silence classifier, and control-plane degraded mode (05 §1.4.3–1.4.4); world-state service (05 §1.21); partition retention; ToS acceptance and age gate; Linux images | BE-A3b, A8 (50/s), A17; NS-2.7; AAA-PLT-2 (Ph2); CL-15 |
| WP-2.7 | BE | Production patching | 05 §7; 08 §2 | 0.16, 1.21, 1.23 | Signed CDN, CDC diff, resume, repair, self-update, live/PTR, **signed installer**, uninstall; product-scoped paths, schemes, registry keys, credentials and endpoints, per-product keysets and the keyset and root-epoch ratchets (08 §2.10.2–2.10.3) | AAA-CNT-7, SEC-6, PLT-6 (Ph2); CL-9…12, 14, 18 |
| WP-2.8 | GP | Items, crafting | 06 §2–3 | 2.5, 1.15 | Ledger inventory, sockets and rolls, audited loot, fitting validator + `helios-fitsim`, survey, harvesters, schematics, industry | GP-5 (Ph2), 6, 7 (timers); G04, G05 |
| WP-2.9 | GP | Quests, AI, combat | 06 §5–9 | 1.16, 2.2, 2.4 | Quests, missions, dialogue runtime; utility AI, lairs; command flight, EWAR, all strategies, seats; ground vehicles and mounts (wheeled, hover and mount models, the `Drive` channel, owner prediction, vehicle deeds and creature mounts; 06 §8.1a, §9.5; GP-4d hash in RT-03's job); progression and achievements (account store, 05 §1.5); companions, pets and drones with the command channel and crew missions (06 §7.3); hold and multi-user verbs, loot containers, emotes, surface and interior maps (06 §9.6–9.8); death pipeline; PvP flags | GP-3 (Ph2), 4b, 4d, 9, 10 (Ph2), 11 (Ph2), 12; M03 (vehicles, mounts), M04, G03, G10, G11, G14 |
| WP-2.10 | ED | Graph, narrative tools | 07 T09, T11–T13, T25 | 1.19 | Vendor imgui-node-editor; T11 + logic graph; T09, T12, T13, T25 MVPs | AAA-TOOL-2 (part); ED-7 |
| WP-2.11 | ED | World and art tools | 07 T03, T05, T06, T15, T17–T23, T30, §2.6 | 1.19, 2.3 | T03, T05, T06, T17–T23 MVPs; T15 state machines and **Physics Asset tab** (hitboxes, HitZones, server-sampling preview and test shots, ragdolls, secondary chains); V-HACD collision in T24; T30 locks/presence (project-wide in the data session, 07 §1.8.2); T28 for every asset class | AAA-TOOL-2, TOOL-6; ED-17 |
| WP-2.12 | ED | Framework hardening | 07 §1, §4.1, §4.4 | 2.10 | Direct-write detector, Sentry journal link, role layouts, git-CLI source control with the §1.7.1 scale set-up, `gen-scale-repo` and weekly 500 GB runs; two-zone PIE; nightly editor perf; ED-7/ED-17 UI replays | ED-6, ED-8, ED-15 (Ph2), ED-18; AAA-ITR-6, STB-2 (journal) |
| WP-2.13 | CL | Client UI breadth, input latency | 08 §1.3a, §1.4–1.10 | 1.20, 2.5, 2.6, 2.8, 2.9 | The Ph2 Foundation panels of 08 §1.7.2, each with its flow: journal, chat and social, inventory and fitting, loot, market and contracts (`<datagrid>`), NPC vendor, trade window, mail, crafting and industry, survey and harvesters, structure placement, organization (roles, wallets), group frames (party ≤ 6), dialogue, galaxy, surface and interior maps, full character creator, all terminal services; queue UX, synced settings, rebinding, HOTAS, pseudo-loc; the `ui-reskin-fixture` gem; Phase A/B game-thread split, JIT `FramePacer`, 3-batch submit (with RD), speculative cue step, `LatencyMarkers`, `helios-latbench` on the lab | CL-2, 3, 5, 6 (Ph2), 8, 16 (Ph2), 19, 23 (Ph2 panels) |
| WP-2.14 | CT | Phase 2 content | 01 §4.2 | 2.8–2.11 | Osk, resources, Osk Yard, "Signal from Saltmarch", lairs, guild (~500 assets); scripted BENCH-1 | M2; ED-7 |
| WP-2.15 | IN | Scale and soak lab | 04 §10; 05 §8; §4.3.2 | 2.4 | `helios-swarm` on SERVER hardware, 1k-bot nightly, 24 h soak, 72 h backend chaos, release fuzzing, MIN lab; per-run cost report against §4.3.2; the 1k-ship half-load battle run (K28) and the first 2,000-ship tick-stamped gateway capture that BENCH-3 replays (01 §3.2) | AAA-STB-1/4 (Ph2); BE-A6; RC-5 input |
| WP-2.16 | DX | Project model, SDK, upgrades, products, project types (**split, §2.3a**) | §2.7.2–2.7.3; 07 §1.7, §1.10, T08; 02 §3.8; 08 §2.10 | Per sub-WP | Ten sub-WPs of 4–6 engineer-weeks each, with their own dependencies and acceptance: 2.16a1–a3 (project model, SDK, upgrades), 2.16b (products), 2.16c1–c3 (dynamic-schema C++ runtime), 2.16d (Go `htypes`), 2.16e (T08 schema editor) and 2.16f (editor extension API `0.x`). The umbrella has no deliverable of its own and closes when every sub-WP has merged | **AAA-TOOL-7 (Ph2), TOOL-8 (Ph2)**; ED-19 (Luau, Ph2); **ED-22**; **RT-21**; CL-24 (Ph2), distributed over the sub-WPs in §2.3a |

**Exit.** Every Ph ≤ 2 criterion, notably AAA-SRV (Ph2); ITR-1, 2, 4, 6; STB-1, 2, 4 (Ph2); SEC-2, 3, 6;
CNT-7; TOOL-2, 6, 7 (Ph2), 8 (Ph2); PLT-2, 6 (Ph2); RT-05, 08, 10 (Ph2), 16, 17, 19 (fleet scale), 20, 21; RC-5, 6, 12 (Ph2); NS-2.1–2.7; BE-A3b, A5–A8, A10, A12, A17;
GP-3 (Ph2), 4b, 4d, 5 (Ph2), 6, 7 (timers), 9, 10 (Ph2), 11 (Ph2), 12; ED-6…8, 15 (Ph2), 17, 18, 19 (Luau), 22; CL-2, 3, 5, 6 (Ph2), 8–12, 14–16, 18, 19, 23 (Ph2), 24 (Ph2); funding gate F2 (§4.3.6).

### 2.3a WP-2.16 split: project model, SDK, products and project types

As first planned, WP-2.16 held about 50–60 engineer-weeks of loosely coupled work in one squash-merged WP:
- the project model, SDK and upgrades;
- product packaging;
- the dynamic-schema runtime on client, cell and backend;
- T08's schema editor;
- the editor extension API.

It also omitted the replication (WP-1.10) and script and UI (WP-1.6) WPs that the dynamic path extends. RT-21,
ED-22 and CL-24 gate the Phase 2 exit, yet none of them showed on the critical-path analysis. §1's cap therefore
splits it. Sizes are human-studio engineer-weeks (ew), the unit of §4.1. The Director re-estimates each sub-WP
at selection, and one that exceeds 6 ew splits again. Tracks follow the owning team: 2.16c is Core/Runtime
work, 2.16d Backend and 2.16e–f Editor/Tools.

| Sub-WP | Tr | Scope | Deps | Deliverables | Acceptance | ew |
|---|---|---|---|---|---|---|
| WP-2.16a1 | DX | Project model and New Project | 1.18, 1.24, 2.12 | Project Browser and New Project wizard; `helios-tool new-project`; the `helios.project.jsonc` layout of §2.7.2 (engine pin, gems per target, channels, `schemas.native`, a `product` stub); project CMake presets, `.vscode/launch.json`, `.gitattributes` and the Windows + Linux CI workflow template; `starter-blank`; executable tutorials with nightly screenshot capture (§2.7.1) | **AAA-TOOL-7 (Ph2 clause):** every executable tutorial passes nightly on Windows and Linux. `starter-blank` reaches PIE with 2 clients and 4 bots from a source build on both OSes | 5–6 |
| WP-2.16a2 | DX | Binary SDK and releases | 2.16a1, 2.7, 1.23 | The binary SDK of §2.7.2 for Windows x64 and Linux x64 (release toolset MSVC 14.44 with no `/GL`; `DebugGame`; dev-flavour editor and tools; import libraries for the three dev shared libraries); the `sdk` channel on the 05 §7 pipeline, with side-by-side installs; the monthly release cut; the nightly `sdk-consumer` job and the rule-1 version-gate script test (ADR-001a); the SDK's hold on consumers' ISA level (02 §1.1): `avx2` `INTERFACE` options in `HeliosConfig.cmake`, the `sdk_config.h` `#error`s, the loader's `isa` check and the gate pulled into consumer executables | M2's "New Project → PIE with no engine checkout" on Windows and Linux. `sdk-consumer` is green on both images, including `isa-audit --objects` over `starter-blank`'s game module, check 3 on its shipping and dev images, and SDE `-nhm` on its shipping client (exit 78). The version gate rejects 19.43 and accepts 19.44, 19.50 and 19.51. An SDK update downloads ≤ 110 % of the changed bytes | 5–6 |
| WP-2.16a3 | DX | API policy and project upgrades | 2.16a2, 1.6 | The public-API definition and deprecations: `@deprecated` in schemas and Luau (luau-lsp warning plus a T28 rule) and `[[deprecated]]` C++ shims (§2.7.2); `engine/migrations/<N>-<N+1>/` with schema, Luau-codemod, content-transform and project-file steps; `helios-tool upgrade-project` with `--dry-run` and `UPGRADE-REPORT.md`; the `upgrade-test` release job (§2.7.3) | **AAA-TOOL-8 (Ph2):** *Cinder Reach* and `starter-blank` move N → N+1 with zero manual edits, then validation, cook and the 60 s smoke pass. `upgrade-test` blocks two seeded changes: a removal with no deprecation release and a rename with no codemod | 5–6 |
| WP-2.16b | DX | Product identity and packaging | 2.16a2, 2.7, 1.20 | The `product` block with `product lint`; the `helios-tool product init` key ceremony (`--dev --ceremony-sim`); `product stamp`; a prebuilt stampable client and launcher in the SDK; T29's *Package & publish* profile (Windows installer, Linux tarball layout; 08 §2.10) | **CL-24 (Ph2)**; `product lint` vectors; stamp → sign → verify round trips on SDK binaries | 5–6 |
| WP-2.16c1 | CR | Dynamic types: bundle and generic `TypeOps` | 0.7, 0.8 | `schemac --emit types` and the `.htypes` bundle (client and server parts, lock IDs, a layout hash per type); the native/dynamic switch in `helios.project.jsonc`; schemac's rejection of native-only constructs in dynamic packages; `reflect` bundle registration and run-time layout; `DynString`, `DynList`, `DynMap` and `DynOptional`; generic table-driven `TypeOps` (construction, copy, move, equality, hash, the JSONC, tagged and cooked codecs, `diff`/`apply`, property paths); `DynRecordView`; the dual-mode corpus in the PR tier (02 §8.3) | **RT-21 codec clauses.** The 200-type package round-trips and keyed-merges through generic `TypeOps`, and its JSONC, tagged and cooked bytes equal the native build on every determinism toolchain. On REF, relative to generated code: copy and compare ≤ 2×, tagged codec ≤ 2×, JSONC ≤ 1.5×. 2,000 types build in ≤ 1 s and the bundle loads in ≤ 50 ms | 5–6 |
| WP-2.16c2 | CR | Dynamic ECS, replication and HXL | 2.16c1, 1.1, 1.10, 0.19 | `ecs::registerDynamicComponent`; `reflect::DynMut` setting `_dirty` bits as `Mut<C>` does; dynamic components in prefabs, checkpoints, undo and the inspector; `repl::describeDynamic` with 04's table-driven quantizers and the `repl.dynamic_us` stage-6 counter; HXL `(TypeId, fieldId, offset)` reads; dynamic record types in the client, cell and Go record DBs | **RT-21 network clause.** Dynamic replicated components put the same bytes on the wire as the native build in the 16-bot smoke. A checkpoint and restore of 10k entities with 3 dynamic components gives an identical values checksum | 5–6 |
| WP-2.16c3 | CR | Dynamic Luau, view-models and data-only reload | 2.16c2, 1.4, 1.6 | Luau userdata per kind, with `useratom` field lookup and fuel charges; generated `.d.luau` for luau-lsp; `ui::DynViewModel` (a custom `Rml::VariableDefinition`, `DirtyVariable`, `@source` adapters); 02 §3.8's data-only reload pipeline (`TypesChanged`, the tick-boundary swap, tagged migration, Luau `__reload`, view-model rebinding); the `schema.dynamic-hot` lint | **RT-21 PIE clause.** 10k entities with 3 dynamic components survive 100 data-only edits with an identical checksum, each live in ≤ 5 s p95 with no compiler present. Luau dynamic field access is ≤ 40 ns p50 on REF. `schema.dynamic-hot` fails its two seeded breaches: > 1 ms of stage 6, and > 5,000 instances per tick | 5–6 |
| WP-2.16d | BE | Go `htypes` and collab validation | 2.16c1, 0.19, 2.11; 2.16a2 for the SDK clause | `services/pkg/htypes`: bundle reader, generic record and JSONC readers, range, unit and `@validate` validators through Go HXL, and a tagged decoder. It is wired into the SDK's `helios-backend`, the collab service's transaction validation and T28's Go rules, and GM and web tools render opaque blobs through it | **RT-21 Go clause.** The SDK's prebuilt `helios-backend` and collab service validate the 10k-record corpus with 0 differences from the native Go validators. The collab service rejects a record that breaks a `@validate` rule | 4–5 |
| WP-2.16e | ED | T08 governed schema editor | 2.16c3, 2.16d, 2.11, 1.19; 2.16b for ED-22's packaging clause | T08's schema editor for dynamic packages (07 T08): record types, structs, enums, `ScriptState` and other components, events and view-models, edited as ToolsFramework transactions on `.hschema` under the data session's T30 lock; the migration preview, with compat-epoch flagging; lint errors that block the save; the content tier's project-schema path class and `Helios-Compat` trailer (§5.2a rules 2, 3 and 5) | **ED-22**, nightly headless on Windows and Linux. The content-tier fixtures pass: a three-commit T08 publish (a new record type with its lock entries, a record that uses it, a `ScriptState` component) rebase-merges as three commits. The same publish bounces, naming the commit, when one commit touches a `schemas.native` package, a Foundation gem's schemas or `helios.project.jsonc` | 5–6 |
| WP-2.16f | ED | Editor extension API `0.x` | 2.16a2, 2.12 | Luau `EditorUI` panels, commands with menus and shortcuts, property customizers and T28 rules, declared in a gem's `editor` block (07 §1.10) and loaded from the binary SDK | **ED-19 (Luau half, Ph2)** | 4–5 |

**Total:** 48–58 ew. RT-21 is green only when 2.16c1–c3 and 2.16d have merged and its full clause set passes three
consecutive nightlies. ED-22 is green only when 2.16e and 2.16b have merged. §3.1's DX schema lane shows the
chain with its slack, and K38 watches it.

### 2.4 Phase 3 — Beta Scale (epics)

**Demo M3 "Harrow Orbit".** Harrow orbit runs on four cells. A *Mule* hauler crosses a boundary with three
crew and a boarding party walking inside (**BENCH-6**); handoff p99 < 100 ms. A 5k-CCU bot shard runs. Four
players enter the *Hollow Vault* strike (**BENCH-4**) in < 2 s and resolve a group conversation. Three
designers co-edit an authored Saltmarch town at BENCH-5 scale live (ED-10), and bots build the **BENCH-5** settlement through the housing rules (06 GP-16 (e)). *Vane* runs reinforcement timers. A
newcomer passes the designer day (ED-9).

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-3.1 | NS | Authority v1 | 04 §6 | 2.4 | Ghosts with location-based margins, effects with sequence-exact de-duplication and outboxes, the co-location protocol (sets, caps, leash, effect-only fallbacks, occupied-region presence; 04 §6.2a), handoff, bundles, zone leader + handle blocks, planned region migration (pre-copy stream, residual, `Fence.MigrateRegion`, `Drain`, `PreProvision`, the bounded `MigrationHold` and rejoin paths, `sim_abi` with the portable physics path; 04 §6.7), lag compensation across cell boundaries (ghost hit history, `M_hit` cook check; 04 §5.6), gateway view composition (viewer registration, budget arbitration, resync records; 04 §6.6), key rotation, cross-compiler and cross-vendor replay with the `ColocDecision` and `HandoffDecision` replay rows (04 §10.2; NS-3.8's multi-cell clause), handoff, migration and AG-tree torture bots, boundary-observer, seam-fighter and tug bots, the ghost audit | NS-3.1, 3.2, 3.4, 3.6–3.8, 3.10–3.12; AAA-SRV-2/5/10/12 (Ph3); S02 |
| WP-3.2 | NS | Instancing, activities | 04 §7; 05 §1.12 | 3.1 | Activity service, `ClaimGuard` lockouts (05 §1.6), parties, **fleets** (roster, hierarchy, roles, invites, adverts, KV `GROUP`; 05 §1.12.1), matchmaker (queues, role and party constraints, latency bands, Weng-Lin rating, widening, backfill, ready check), group finder, leaderboards, phases, housing instances (parking and reload of structures and interiors, 06 §9.2) | NS-3.5; AAA-SRV-11; GP-7, GP-14 (a, b), GP-15 (d), GP-16 (f); W07, G12, G19 |
| WP-3.3 | BE | Backend scale-out | 05 §6, §9 | 2.5, 2.6 | K8s + Agones, 5k CCU, chat 5k msg/s, PG failover, rollback tools, collab service, control-plane and host-loss chaos suite, node pools and domain-sized warm pool (05 §1.4.6), DSAR export/erasure sagas, 24/7 on-call and status page (05 §6.6–6.8); player reports, support tickets and the case queue with SLA timers, chat evidence tags and report evidence (05 §1.10, §1.16–1.17) | NS-3.3; BE-A11, A13, A15; AAA-SRV-3/9 (Ph3), STB-5 |
| WP-3.4 | CR | Runtime scale | 02 §8.1 | 2.1, 2.2 | Nested grids, boarding, IORing/io_uring, crowd LOD, voice (`engine/voice` client pipeline and `PortalGraph` proximity audience, 02 §7.3; `helios-voice` forwarder and moderation v1, 04 §2.7), Earth-size PCG, buoyancy, 100k records; MIN CPU budget closure (02 §2.4 MIN tables and `cpu.*` settings) | RT-06, 07, 11; RT-12 (MIN clause and 30/45 fps clause for BENCH-3 and 5, Ph3); NS-3.9; AAA-CNT-1 (Earth), 2, 4, 5 (Ph3) |
| WP-3.5 | RD | Render Phase 3 | 03 §9.2 | 2.3 | Mesh shaders, D3D12 seam test, impostors and brackets, HLOD, froxel fog, DDGI + AS management (relit assembly-time probes on MIN), volumetric clouds with fast mode (03 §5.8), generic crowd impostors (03 §7.6a), oceans, PVT, nebula v1, vegetation (LOD/impostor bands, wind, interaction), weather effects | RC-7, RC-11 (Ph3), RC-12 (Ph3, BENCH-6); AAA-REN-4, REN-6 (Ph3) |
| WP-3.6 | GP | Gameplay Phase 3 | 06 §12.1 | 2.8, 2.9, 3.1, 3.2 | Boarding; EVA (suit thrusters, magboots, grabs, tethers, gravity switch; 06 §8.2a); activities (encounters, checkpoints, wipes and revives, tiers and modifiers, level sync and bolster, solo mode, roster policy), PvP match rules and risk zones (06 §6.6–6.12); multi-crew, killmails; **housing** (`PlacementCheck`, ledger plots and `Lot`, decoration caps, the upkeep worker's maintenance → decay → condemnation → reclamation chain; 06 §9.2.1–9.2.3, 05 §1.6, §1.8), the cell side of **cities** (footprint enforcement, city terminals; 06 §9.2.4), **territory** (vulnerability windows, capture channel, reinforcement with `PreProvision` hints, influence hysteresis, audit; 06 §9.3, 05 §1.21), crimewatch, group conversations, fleet AI, **player fleets** (fleet warp, broadcasts, command bursts, fleet killmails, the bot fleet commander; 06 §8.3a), modular ships, 2 Hz fleet-battle command flight; follower handoff across cells, NPC crew, fighters; collections, codex, legacy; paired emotes, performances, airlocks and pressure; weather gameplay (06 §9.9); tracked and motorcycle drivetrains, mounted combat, vehicle turrets and hangar drive-out (06 §8.1a) | GP-3 (Ph3), 4c, 4d (Ph3 layouts), 5, 8, 10 (Ph3), 11 (Ph3), 13, 14 (c–f), 15 (a–c, Ph3), **16 (a–c, e)**, **17 (a–d)**; M01, M06, M07, M09, G07, G08, G12, G16, G19 |
| WP-3.7 | ED | 26/30 tools + live edit | 07 §5.1, §1.6.1, §1.6.3, §1.6.4, §1.8, §2.6.5, §2.7 | 2.10–2.12, 3.1, 3.3; 3.2 and 3.6 for ED-24 and ED-25 | T30 edit instances (multi-document transactions and groups, session rebase, linearized one-PR publish, journal durability and restore, 07 §1.8.1; the data session, document homes with checkout and check-in, and carry publishes, 07 §1.8.2), asynchronous HLOD, impostor and probe rebuilds (07 §4.1.1), web Writers' Room and Loc Review, T14 MVP, remote control, **multi-cell PIE** (Cells: N, forced handoff, per-cell kill and DAP, effect log, `.hrepro`), T01 Partition tool for v1 zones, **Activity PIE** and the **dev shard clock** (07 §1.6.3, §1.6.4; `--dev-clock`, 05 §5), the buildable-area heat map (07 §2.7.3), ED-10 UI replay, AAA polish | AAA-TOOL-3, 5, ITR-7, STB-2 (Ph3); ED-9…12, ED-14, ED-15 (Ph3), ED-20, ED-21, ED-24, ED-25 |
| WP-3.8 | CL | Client/launcher Ph3 | 08 §4.3 | 2.7, 2.13, 3.2, 3.3, 3.4, 3.6 | Install tiers, pre-download, in-game heal, overview grid, group dialogue UI, internal addons, AppImage (built by *Package & publish*), release gates; the Ph3 panels of 08 §1.7.2 with flows (killmail viewer, fleet window, activity director and group finder, group and raid frames (raid ≤ 16), the encounter and match HUD with scoreboard, AFK and vote-kick prompts, leaderboards, housing, decoration and city panels, player vendors, factories and invention, org hangars and alliances, image designer, collections reacquire, support and report, customization); `VK_NV_low_latency2`/`VK_AMD_anti_lag` providers, BENCH-4 latency runs (08 §1.3a) | CL-7, 13, 16 (Ph3), 23 (Ph3 panels), 24 (AppImage); AAA-PLT-6, STB-1 (Ph3) |
| WP-3.9 | CT | Phase 3 content | 01 §4.2 | 3.1–3.7 | Multi-cell orbit, Mule, settlements, Vane, Hollow Vault, phased story (~1,000 assets) | M3; BENCH-4/5/6 nightly; AAA-REN-8 (Ph3) capture set and panel |
| WP-3.10 | IN | Scale lab | 04 §10 | 3.1 | 10k-bot weekly chaos, cross-compiler replay CI, colo decision (05 §6.4) | AAA-STB-3 (Ph3) |
| WP-3.11 | IN | Security Ph3 | 04 §9; 05 §1.16a | 3.1 | 24 CPU-h fuzzing, text sanitization, rate limits, rewind caps, X25519 re-key design; focused external test of HTP, auth and the patcher before the public beta (§4.3 H7); **anti-cheat vendor decision** (EAC or BattlEye, native Linux support a criterion), cell trust features and red-team corpus v1 (05 §1.16a) | AAA-SEC-5, SEC-7 |
| WP-3.12 | DX | Starter templates I | §2.7.4 | 2.16a3, 2.16b, 2.16e, 2.16f, 3.2, 3.6; 3.7 for ED-23's code pane | `starter-sandbox`, `starter-fleet`, `starter-shooter` gems and template projects on the Foundation layer; one executable tutorial each; nightly `template-proof-<id>` jobs; C++ game-module API reference; editor extension API `1.0` with its reference, tutorial and the `sample-editor-ext` gem in `upgrade-test` (07 §1.10); **world-script API `1.0`** (05 §1.23: the `world-script` role of `helios-cell`, the `worldscript` service, the `world` Luau realm) with its reference, a "Build a bounty board" tutorial and the `starter-sandbox` bounty board in `upgrade-test`; the world-script editor integration (07 §1.6.2: one WSH in every PIE mode with per-partition DAP, T08's `worldscript` block editor and `ws.*` rules, the World scripts panel in T26 and T27, dev-data migrations); the Foundation **`CityGovernance`** world script (06 §9.2.4; 05 §1.23 item 13) with the `starter-sandbox` city; per-release "changes since N−1" page | **AAA-TOOL-7 (Ph3), TOOL-9 (Ph3)**; ED-19; **ED-23**; BE-A20; GP-16 (d) |
| WP-3.13 | CT | Locomotion capture | §4.3.3; 02 §7.2 | 2.2 | Contracted locomotion capture for the motion-matching database (human set plus the Keth digitigrade remap), with provenance and performer releases, or the procedural fallback decided by the Ph3 midpoint (K36) | Capture data in the asset DB with 100 % provenance; or the fallback ADR |

**Exit.** Every Ph ≤ 3 criterion, notably AAA-SRV (Ph3); REN-4, 6 (Ph3), 8 (Ph3 panel); ITR-7; STB-1, 2, 3 (Ph3), 5;
CNT-1 (Earth), 2, 4, 5; SEC-5, 7; TOOL-3, 5, 7 (Ph3), 9 (Ph3); PLT-6 (Ph3); RT-06, 07, 11, 12 (MIN and 30/45 fps clauses); RC-7, 11 (Ph3), 12 (Ph3); NS-3.1–3.12;
BE-A11, A13, A15, A20; GP-3 (Ph3), 4c, 4d (Ph3 layouts), 5, 7, 8, 10 (Ph3), 11 (Ph3), 13, 14 (Ph3), 15 (Ph3), 16, 17 (Ph3); ED-9…12, 14, 15 (Ph3), 19…21, 23…25; CL-7, 13, 16 (Ph3), 23 (Ph3), 24 (Ph3); funding gate F3 (§4.3.6).

### 2.5 Phase 4 — Launch Quality: the AAA bar (epics)

**Demo M4 "Vane".** The *Bastion* capital leads a 2,000-ship battle at TiDi 10 % (**BENCH-3** ≥ 45 fps on
REF). Six players clear the *Lattice Heart* raid, a season starts from the calendar, and ~300 voiced lines in
two languages play auto-staged. A cell killed in four-cell Harrow orbit resumes from its replicant with ≤ 1 s
of state lost and no disconnect (NS-4.6). A 72 h soak at 50k CCU is clean, the pentest has no open critical or
high findings, every BENCH scene meets budget on MIN and REF, and all 30 tools are AAA-complete.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-4.1 | RD | AAA render features | 03 §9.2 | 3.5 | Visibility buffer, VSM, clouds golden, TAA + upscalers, HDR10, DLSS/XeSS plugins, Nanite-lite v1, 120 fps mode (03 §8.1.6); D3D12 gate | RC-8, 9, 11 (Ph4); RT-12 (120 fps clause, Ph4 midpoint); AAA-REN-1/2/3/6 |
| WP-4.2 | CR | Perf and memory closure | 02 §2.2; 03 §8 | 4.1, 3.4 | BENCH-1…6 on MIN and REF, Windows and Linux | RC-10; RT-10, 12; AAA-CNT-3, PLT-5; CL-6, 21 |
| WP-4.3 | NS | Battle scale, replicant tier | 04 §6.4–6.5, §8 | 3.1; **Improbable review** (H8, K18) | Fleet aggregation, PVS, offload workers, 8k sessions per gateway with N+1 box headroom, zone-loss sizing (12 boxes over 3 zones, 05 §6.7) and the reconnect-storm path (04 §2.4, §2.6), far-broadcast fallback (04 §6.6), XDP; **replicant tier** (ADR-007): `helios-cell --replicant`, the cell → replicant stream, standby restore from RAM with `TickFlush` commits and the one-tick consistency pass (04 §6.4), `AssignReplicant` placement for every persistent world zone, 2-vCPU replicants shared across zones (05 §1.4); zone-instance affinity and the per-box trunk budget with the scattered-mesh test (04 §2.6, NS-4.4(c)); the gateway-loss rebind at every contributor (`HeldEntities`, 04 §6.6); make-before-break gateway relocation (`Relocate`, `ClientRebind{relocate}`, `RebindFence`; 04 §2.4, §6.6; 05 §6.3.1) | NS-4.1–4.4, NS-4.6, NS-4.7, NS-3.10 and NS-3.12 (Ph4 reruns); AAA-SRV-3/9/10 (Ph4); S02 (replicants), S05 |
| WP-4.4 | BE | Backend Phase 4 | 05 §9 | 3.3 | 10k tx/s, 30k AG/s checkpoints, 50M entities under lifecycle, 200/s admission, OIDC/MFA, N/N+1 restarts, DR drills, privacy SLA, ID-block scale, public API, hotfix ≤ 15 min, compat epochs and staged server-part hotfixes (05 §1.14.1), rack blast radius and rack-disjoint replicants (05 §1.4.3, §1.4.6); `DrainGateway` gateway rolls and `MintRelocation` (05 §6.3.1); the fence load budget, fence gate and lock rules (05 §1.13, §3.6); store API, checkout webhooks and 13–17 spending caps (05 §1.20) | BE-A5 (10k), A8 (200/s), A9, A14 (Ph4), A16, A18, A19; AAA-STB-6, ITR-8, CNT-5 (Ph4); G17 |
| WP-4.5 | GP | Gameplay Phase 4 | 06 §12.1 | 3.6 | AI LOD T0–T3 + HTN, resource network, device aim assist, seasons and reward tracks (06 §5.3), ranked seasons and leaderboard windows (06 §6.11, §6.13), 16-player operations, sovereignty (hubs, upgrades against region power and workforce, weekly fuel, occupancy index; 06 §9.3), insurance, character creation; player fleets at 2,000-ship scale in NS-4.2 (06 §8.3a) | GP-3, GP-11 (Ph4), GP-14 (Ph4), GP-15 (Ph4), GP-17 (e); G08 (sovereignty), G15, G17, G19 |
| WP-4.6 | GP | Animation, cinematics, VO | 02 §7.2; 07 T13–T15, T22; §4.3.3 | 3.7, 3.13 | Motion matching on the WP-3.13 database, FACS facial + lip-sync (captured heads, or the audio-driven viseme fallback of §4.3.3) on the 02 §7.2 facial runtime and 03 §7.6b's GPU face shapes, T13 auto-staging, T14/T22 AAA, 2-language VO, 500k strings | ED-13; RT-15; **RT-22**; AAA-CNT-6; R04 (either data source; the fallback relaxes no threshold) |
| WP-4.7 | ED | 30/30 tools | 07 §5.1 | 4.6 | T27 web admin, staged live-shard edits, accessibility pass; optional Perforce adapter (07 §1.7.2), planned earlier only if ED-18 fails twice; the TOOL-10 content-team run (01 §3.9.1) and fixes for its logged defects | AAA-TOOL-4, TOOL-10, STB-2 (Ph4); ED-12 |
| WP-4.8 | IN | Security hardening | 04 §9; 05 §1.16a, §6.5 | 3.11 | X25519 re-key decision and build, anti-cheat vendor integration, trust detectors and ban waves (05 §1.16a), DDoS drill, **external pentest** | AAA-SEC-8; NS-4.5; CL-22; BE-A21 |
| WP-4.9 | CL | Client/launcher Ph4 | 08 §4.3 | 3.8, 4.4, 4.5 | OIDC/PKCE, storefront plugin, HDR, public addons, gyro, TTS, RTL text, hang dumps; the Ph4 panels of 08 §1.7.2 with flows (crafting minigames, season track, ranked seasons and leaderboard windows, DNA-blend creator, store) | CL-6, 16 (Ph4), 22, 23 (Ph4 panels) |
| WP-4.10 | CT | Phase 4 content | 01 §4.2 | 4.1–4.6 | Bastion, battle scenario, Lattice Heart, seasons, VO (~1,500 assets); the final BENCH-3 capture of the *Bastion* battle, replacing WP-2.15's bot capture (01 §3.2) | M4; AAA-REN-8 (Ph4) capture set and panel |
| WP-4.11 | IN | Launch soak, legal | 01 §5.2; §4.3.2 | all Ph4 | 72 h 50k-CCU soak (cost per run reported against §4.3.2); crash-rate programme; **SWG terrain patent review signed**; licence sign-offs closed; SBOM | AAA-STB-1, 3, 4 (Ph4) |
| WP-4.12 | DX | Starter templates II, upgrade closure | §2.7.3–2.7.4 | 3.12, 4.5, 4.6 | `starter-seamless` and `starter-story`; N−2 → N upgrade tests for Cinder Reach and all templates; docs release process (versioned site, offline copy in the SDK) | **AAA-TOOL-8 (Ph4), TOOL-9 (Ph4)** |

**Exit — the AAA bar.** Every criterion with Ph ≤ 4, including AAA-REN-1, 2, 3, 6, 8 (Ph4 panel); CNT-3, 5, 6; STB-1…4
(Ph4), 6; SEC-8; SRV-9 (Ph4, ≤ 1 s in persistent world zones); TOOL-4, 8, 9, 10; ITR-8; PLT-5; RT-10, 12, 15, 22; RC-8…11; NS-4.1–4.7; BE-A5 (10k), A8, A9, A14 (Ph4), A16, A18, A19, A21; GP-3, 11 (Ph4), 14 (Ph4), 15 (Ph4), 17 (e);
ED-12, 13; CL-6, 9, 16, 21, 22, 23 (Ph4); plus the patent and licence sign-offs as M-class evidence.

### 2.6 Phase 5 — Ambition (epics)

**Demo M5 "Lattice".** *Ember* and *Quiet* join. The user flies seamlessly through the Lattice between
systems. A moving battle splits and merges cells, and a killed cell costs ≤ 1 s of hitch. BENCH-3 scales to
3,000 ships, a 100k-bot shard runs, players script structures, and RT effects run on SHOWCASE.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-5.1 | NS | Dynamic meshing (v2) | 04 §6.5 | 4.3 | Split/merge, cluster partitioning, handoff < 50 ms | NS-5.1, 5.2; AAA-SRV-5 (Ph5); S03 |
| WP-5.2 | NS | Gateway replication layer | 04 §6.5 | 5.1, **patent follow-up** (K18) | Client interest, priority and ghost records moved onto the Phase 4 replicant tier and the gateways | NS-5.3; AAA-SRV-12 (Ph5) |
| WP-5.3 | BE | 100k shard | 05 §9 | 4.4 | Partitioned ledger 50k tx/s, 500/s admission, 10 s checkpoints; cross-shard queue and group-finder federation (05 §1.12) | NS-5.4; BE-A8 (500/s); AAA-SRV (Ph5); GP-14 (h) |
| WP-5.4 | GP | Seamless galaxy | 02 §8.1; 06 §4, §7 | 5.1 | Lattice travel, Ember and Quiet, economy sim, ecosystems | W06, G18 |
| WP-5.5 | GP | Player scripting | 06 §11; 08 §1.12 | 4.8 | Metered UGC Luau VMs, addon portal, player blueprints | Security review; hostile-UGC suite |
| WP-5.6 | RD | Ray tracing | 03 §9.2 | 4.1 | RT shadows, ReSTIR, Nubis³, volumetric nebulae, frame generation | RC-11 (Ph5); AAA-REN-6 (Ph5) |
| WP-5.7 | CR | Mods, destruction | 02 §1.2 | 4.2 | Stable C ABI, learned motion matching, destruction hooks | R06-ENG-32 tests |

**Exit.** AAA-SRV (Ph5), REN-6 (RT); RC-11 (Ph5); NS-5.1–5.4; BE-A8 (500/s); GP-14 (h); the WP-5.5 security review.

### 2.7 Developer-experience track: Helios as a product

The north star (01 §1.1) is a studio that ships a game by writing records, Luau and domain graphs, **never
modifying engine or backend source**. Commercial engines make that possible with more than binaries: reference
documentation, tool manuals, tutorials, project templates, a versioned SDK and an upgrade path between releases.
*Cinder Reach* proves the capabilities, but only as one hybrid project that lives in the engine repository. The
DX track proves the product: each game class starts from a clean New Project, and projects survive engine
upgrades. Its WPs are WP-1.24, WP-2.16 (split into ten sub-WPs, §2.3a), WP-3.12 and WP-4.12. It is gated by
**AAA-TOOL-7, TOOL-8 and TOOL-9** (01 §3.9) and staffed from the Editor/Tools track (§4.1), except the
dynamic-schema runtime (WP-2.16c1–c3, Core/Runtime) and Go `htypes` (WP-2.16d, Backend).

#### 2.7.1 Documentation pipeline (WP-1.24; AAA-TOOL-7)

| Source | Generator | Output |
|---|---|---|
| `.hschema` types, fields, enums, components, messages and services, with `///` comments, units, ranges, defaults, audience and `@deprecated` | `helios-schemac --emit docs` (02 §3.5) | Record and component reference: one page per type, listing the tools that edit it and its Luau accessors |
| Luau bindings (the `.d.luau` from `--emit luau`, plus hand-written engine libraries), with doc comments | `helios-docs luau`, reading the `.d.luau` AST through Luau.Analysis | Luau API reference: signatures, types, realm (client, server, `world` for world scripts (05 §1.23), editor), fuel cost and determinism notes; the `worldscript` schema block's reference (tables, indexes, RPCs, events, timers, reasons, quotas) |
| C++ game-module API (`HELIOS_*_API` in `include/helios/game/`) and editor extension API (`HELIOS_EDITOR_API` in `include/helios/editor/`, 07 §1.10) | `helios-docs cpp` through libclang (Apache-2.0 with LLVM exception; tools only) | C++ reference with threading rules (DoD item 1) |
| CVars, `helios-tool` commands, T28 rules, reason and error codes | `--dump-cvars`, `helios-tool --help-json`, the rule registry | Reference tables |
| Tool manuals, `docs/manual/tools/Txx.md` | Hand-written from a template (purpose, workflows, shortcuts, data written, validation rules, limits); screenshots captured nightly by editor automation on a fixed sample project | One manual page per tool |
| Tutorials, `docs/tutorials/<name>.md` plus `<name>.tutorial.luau` | Each Markdown step is paired with a Luau automation script that performs it through ToolsFramework commands on a fresh project | **Executable tutorials**, run nightly on Windows and Linux as doc tests |

- **Site.** `helios-docs build` (Go, goldmark, MIT) produces static HTML and a search index. The site is versioned
  per engine release and an offline copy ships in the SDK. F1 in the editor opens the page for the focused
  panel, record type or field. Property-grid tooltips use the same strings (07 §1.2).
- **Coverage gate (PR tier).** A public Luau function, record type or field, component, CVar or `helios-tool`
  command without a doc string fails the build. New symbols have no allow-list from day one; legacy symbols
  reach zero by the Phase 1 exit. The C++ game-module and editor extension APIs and the world-script API join the gate in Phase 3.
- **No rot.** Screenshots and tutorials regenerate nightly. A failing tutorial is a nightly failure and gets a
  bisect agent (§5.8). DoD item 9 (§5.3) requires doc strings and manual updates in every WP.

#### 2.7.2 Projects, SDK and versioning (WP-2.16a1, 2.16a2, 2.16b)

- **Project layout.** `helios.project.jsonc` holds the `engineVersion` pin (semver), the gems enabled per target
  (02 §1.2) and the channels. Alongside it: `schemas/` (the project package), `content/`, `scripts/`, an
  optional `game/` C++ module, `.gitattributes` with LFS rules, project CMake presets, `.vscode/launch.json`
  for DAP, a Windows + Linux CI workflow template (build, T28, cook, project tests), a local `helios-backend`
  configuration and the `product` block with its branding kit (08 §2.10: product ID, install and executable names,
  URI scheme, endpoints, launcher theme, icons, signing provider; keys from `helios-tool product init`).
- **New Project.** The editor opens on a **Project Browser** (recent projects, installed engine versions,
  templates). `helios-tool new-project --template <id> --name <n> --path <p>` does the same headlessly. With the
  SDK's prebuilt Foundation DDC, a new project reaches PIE in ≤ 20 min on DEV (TOOL-9).
- **Binary SDK** per release, for Windows x64 and Linux x64: editor, tools, servers, `helios-backend`, headers
  with import libraries for the three dev shared libraries (ADR-016) and static libraries for the shipping
  link. The editor, `helios-tool`, `helios-assetd` and `helios-cook` are dev-flavour builds, so they load a
  project's game and editor modules (07 §1.10). The SDK also carries pinned Slang, Foundation gems, templates
  and offline docs. Windows libraries and binaries are built with the release toolset, MSVC 14.44 (VS 2022 17.14), with no `/GL`, so projects build with VS 2022 17.14 or any
  VS 2026 update. Projects debug game code in a `DebugGame` configuration (`/MD`, IDL 0), and a nightly
  `sdk-consumer` job proves both toolsets (ADR-001a). It is distributed as signed, CDC-chunked
  manifests on an `sdk` channel of the 05 §7 pipeline, so an engine update downloads only changed chunks. SDKs
  install side by side under `%LOCALAPPDATA%\Helios\SDK\<ver>` or `~/.local/share/helios/sdk/<ver>`. A
  studio that needs engine source clones the repository at the release tag; both routes use the same project
  format.
- **Releases.** From Phase 2 the loop cuts a monthly engine release from a `main` commit with a green nightly:
  `0.MINOR.PATCH` until the Phase 4 exit, then `1.0` at the AAA bar.
- **Public API and deprecation.** The public surface is the Luau API (`.d.luau`, including the `editor` realm),
  engine and Foundation record schemas, `helios.project.jsonc`, the `helios-tool` CLI, the C++ game-module
  headers, the **editor extension API** (`include/helios/editor/`, the `gem.jsonc` editor block and command-ID
  namespace; 07 §1.10, from its `1.0` in Phase 3), the **world-script API** (the `worldscript` schema block and
  the Luau `world` realm; 05 §1.23, from its `1.0` in Phase 3), and the content text formats. A breaking change ships as a
  deprecation first: `@deprecated("use X", since)` in schemas and Luau (luau-lsp warnings plus a T28 rule) and
  `[[deprecated]]` shims in C++. Removal comes no earlier than two minor
  releases later, and every change carries a migration step (DoD item 9).

#### 2.7.3 `helios-tool upgrade-project` (WP-2.16a3, WP-4.12; AAA-TOOL-8)

1. Read the project's `engineVersion` and plan the migration chain N → N+1 → … up to the installed SDK.
2. Apply each release's set from `engine/migrations/<N>-<N+1>/`:
   - **schemas:** `@was` renames and `@version` plus `upgrade<T>` hooks (02 §3.4), re-saving records and
     containers as canonical JSONC;
   - **Luau codemods:** a declarative rename and signature map applied to the Luau.Analysis AST, with the result
     type-checked;
   - **content transforms:** Luau automation scripts that run as ToolsFramework transactions, so they are
     journaled and undoable;
   - **project files and presets**; C++ modules compile against the `[[deprecated]]` shims. Because removal
     waits two minor releases, C++ that builds warning-free at N still builds at N+2 without edits; Luau never
     relies on that window, because the codemods rewrite deprecated calls.
3. Run T28, the cook, the project's tests and a scripted smoke (PIE with bots for 60 s). Write
   `UPGRADE-REPORT.md` on a new git branch. `--dry-run` reports without writing.

**`upgrade-test` (CI, every release candidate).** Frozen snapshots in `tests/upgrade/<N>/` (*Cinder Reach* at tag
N, every template generated at N and, from Phase 3, the `sample-editor-ext` gem of 07 §1.10 and the
`starter-sandbox` bounty board's world script with populated tables) are upgraded to the candidate. A world-script
table change must carry its `migrate` handler, which `upgrade-project` runs on the snapshot's rows (05 §1.23). Any manual step, validation error or smoke failure blocks the release. Phase 4 adds a two-hop N−2 → N run.

#### 2.7.4 Starter templates (WP-2.16a1, WP-3.12, WP-4.12; AAA-TOOL-9)

A template is a project skeleton in `templates/<id>/` plus a gem `gems/starter-<id>/`. It is built only on the
engine and the Foundation layer (01 §4.1), with Luau, records, graphs and UI and no C++. Its gem declares at
least one project type in a dynamic schema package (02 §3.8): a record type, a `ScriptState` component or a
view-model. The scripted proof adds one more through T08's schema editor and exercises it in the class loop,
and the job fails if any template package is native. Placeholder art is CC0 or
procedural, and the reference-name grep (01 §4.1 rule 2) keeps *Cinder Reach* names out. Each template has an
executable tutorial and a nightly **`template-proof-<id>`** job: New Project → an automation script adds
class-specific content → cook → headless cells and bots run the class loop → assertions → package the client →
patch it from a local CDN (T29's *Package & publish* profile under a template product ID with ceremony-simulated
keys, 08 §2.10.5) → run the flows of the Foundation panels the class uses (08 §1.7.3). The job fails on any engine
or backend source difference from the SDK hash.

| Template | Class | Contents | Scripted proof | Ph |
|---|---|---|---|---|
| `starter-blank` | — | Foundation plumbing: login, character select, HUD, one zone | New Project → PIE with 2 clients and 4 bots; `upgrade-test` | 2 |
| `starter-sandbox` | SWG | One planet zone; survey, harvest and craft with schematics; player vendor; housing plots and a player city (the Foundation `CityGovernance` world script, 06 §9.2.4); skill-based progression; a Luau *Resource survey* editor panel and command (07 §1.10); a **cross-zone bounty board** as a world script (05 §1.23) with a terminal tab in each zone | The script adds a resource class and a schematic, then opens the survey panel through `helios-uitest` and runs its command. 50 bots survey, harvest, craft and sell for 10 min. The ledger conservation audit finds 0 deltas, crafted stats stay within schematic ranges, and the item sells through the vendor. Bots post bounties in one zone and kill their targets in another: every bounty pays once or refunds once at expiry, the escrow-backing audit matches, and a WSH `kill -9` mid-run loses nothing (05 A20). **Housing and city:** each bot places a house on a housing plot through the placement ghost, and the client and cell `PlacementCheck` verdicts agree; 10 bots found a city, declare residence and reach Outpost at the first cycle; a mayoral election with a WSH `kill -9` during its close settles exactly once; a vendor sale inside the radius pays sales tax into the treasury under `Transfer.Tax.City.Sales`; one house left unpaid runs to reclamation on compressed timers; the escrow-backing and conservation audits find 0 deltas (06 GP-16 a, c, d) | 3 |
| `starter-fleet` | EVE | A station and two systems; fitting; regional market; corporation; a 2 Hz fleet-battle zone | The script adds a hull and a module. 200 bots fit, trade and fight a 100-ship battle as two player fleets that form from adverts, fleet-warp in and fight to broadcasts (06 §8.3a). The fitting validator passes, orders settle through escrow, killmails are recorded and the tick stays within the 2 Hz profile budget | 3 |
| `starter-shooter` | Destiny | Social hub; a matchmade 3-player strike with two tiers and a weekly modifier; a rated 6v6 Control match mode; loot with rolled perks; predicted abilities | The script adds a weapon with perks and an encounter. 30 bots queue solo and in parties of 2 and 3, are matched into 10 fireteams at queue p95 ≤ 60 s and clear the strike, and a cell kill mid-encounter resumes from the checkpoint in ≤ 5 s. 12 bots play a rated 6v6 match at 60 Hz whose final score equals the event-log fold, and ratings update once. Loot passes chi-square at 10⁶ rolls, and lockout rewards are granted once per bot. Mispredictions stay within GP-2 | 3 |
| `starter-seamless` | Star Citizen | A planet with orbit; a multi-crew ship; boarding; a two-cell zone | The script adds a ship variant. 12 bots crew 4 ships, descend with no loading screen and board across the cell boundary; handoff p99 < 100 ms (AAA-SRV-5) | 4 |
| `starter-story` | SWTOR | A class-story chapter with phases, a companion and a group conversation; auto-staged dialogue with TTS placeholder VO; a 4-player flashpoint with Story, Veteran and Solo tiers, level sync and a group-finder listing | The script adds a quest step and a conversation. A 4-bot group completes the chapter with correct phase transitions; ≥ 80 % of shots need no manual edit (the ED-13 rule). Four bots of mixed levels form through the group finder and clear the flashpoint on Veteran with synced stats within GP-14e's band; one bot clears it on Solo with its companion; each lockout reward is granted once | 4 |

---

## 3. Dependency graph, critical path and reconciliations

### 3.1 Graph

```
LANE          PHASE 0                                  PHASE 1
Spine         0.1 ─► 0.2 ─► 0.7 ─► 0.8 ──────────────► 1.1 ─► 1.2 ─► 1.10 ─► 1.11 ─► 1.15 ─► 1.16 ──┐
Core side           0.2 ─► 0.5 ─► 0.6 ─(ECS decision)─► 1.1      1.2 ─► 1.4, 1.5, 1.6               │
ISA rework          0.2 ─► 0.2r ─► 0.5r ─► 0.9, 0.17 (§5.10.4)                                      │
Rendering           0.2 ─► 0.11 ─► 0.12 ─► 0.9 ─► 1.3 ─┐                                            │
                                   0.12 ─► 1.7 ────────┴─► 1.8 ─► 1.9 ──────────────────────────────┤
Servers             0.2 ─► 0.13 ─► 0.14 ──────────────► 1.10                                        ├─► 1.22 ─► M1
Gameplay            0.7 ─► 0.19 ─────────────────────► 1.15                                         │
Editor              0.8 + 0.11 ─► 0.18 ─► 1.17 ─► 1.18, 1.19 ───────────────────────────────────────┤
Backend/client      0.1 ─► 0.15 (0.2 ─► 0.15r first) ─► 0.16 ─► 0.17 ─► 1.13, 1.14 ─► 1.20, 1.21 ───┘
Later chains  net 2.4 ─► 3.1 ─► 4.3 ─► 5.1 ─► 5.2 │ ledger 1.14 ─► 2.5 ─► 3.3 ─► 4.4 ─► 5.3
              render 1.8 ─► 2.3 ─► 3.5 ─► 4.1 ─► 4.2 ─► 5.6 │ tools 1.19 ─► 2.10/2.11 ─► 3.7 ─► 4.6 ─► 4.7
              DX 0.7 + 1.6 + 1.17 ─► 1.24 ─► 2.16a1 (+2.12) ─► 2.16a2 (+2.7) ─► 2.16a3, 2.16b, 2.16f ─► 3.12 ─► 4.12
              mocap 2.2 ─► 3.13 ─► 4.6
DX schema     0.7 + 0.8 ─► 2.16c1 ─► 2.16c2 (+1.1, 1.10, 0.19) ─► 2.16c3 (+1.4, 1.6) ─► 2.16e (+2.11) ─► RT-21, ED-22 ─► 3.12
                           2.16c1 ─► 2.16d (+2.11) ───────────────────────────────────► 2.16e ◄─ 2.16b (ED-22 packaging clause)
Human-gated   H1 GPU lab (RC-3/4) · H2 SERVER host + load-test cloud (RT-01, SRV) · H3 HSM cert (2.7)
              H4 licence and privacy counsel · H5 art/VO/mocap/facial (3.13, 4.6) · H6 playtesters
              H7 pentest (3.11, 4.8) · H8 patent counsel (4.3, 4.11, 5.2) — all costed in §4.3
Funding       F0 (Ph0 exit) · F1 (Ph1 exit) · F2 (Ph2 exit) · F3 (Ph3 exit), §4.3.6
```

**Critical path to M1** is the spine: schema → ECS → world → replication → prediction → gameplay → content.
**Near-critical** is the rendering lane, which must meet 1.3's GPU-terrain contract for BENCH-2; the WP-0.9c
throughput spike (03 §5.5a, K5b) measures it in Phase 0 and pre-decides the fallbacks, so it cannot surprise
Phase 1. The cheapest schedule protection is landing 0.7's grammar and 1.1's ECS API as interface WPs early.
**From Phase 2**, the critical path is networking scale (2.4 → 3.1 → 4.3), and the Phase 4 exit is also gated
by H1, H6 and H7, which the loop cannot shorten. WP-4.3 also waits on H8's Improbable review (K18), so that
review is scheduled and funded in Phase 3 (§4.3), before the chain reaches it. **Funding is on the critical
path too:** a phase's human-gated WPs (lab, soaks, content, pentest) start only after its funding gate
(§4.3.6), so an unapproved envelope stalls the phase exit even when every engineering WP is merged.

**DX schema lane: slack to the Phase 2 exit.** RT-21, ED-22 and CL-24 gate the Phase 2 exit (§2.3a), so the lane
is tracked like a critical chain. Months below count from the Phase 1 exit on the human-studio calendar, with
one engineer per sub-WP and the planning assumptions stated. The exit needs three green nightlies and the phase
review, so each "latest merge" is one month before the exit (month 14 on the 15-month calendar, month 20 on the
21-month one).

| Chain | Waits for | Earliest merge (month) | Slack at 15 / 21 months |
|---|---|---|---|
| 2.16c1 → c2 → c3 (RT-21 C++ clauses) | 0.7, 0.8, 0.19 (Phase 0); 1.1, 1.4, 1.6, 1.10 (Phase 1) | 4.5 (c1 may start during Phase 1; overlap only adds slack) | 9.5 / 15.5 |
| 2.16d (RT-21 Go clause) | c1 (month 1.5); 2.11's data-session locks and collab validation hook (assumed month 6); 2.16a2 for the SDK clause (month 8) | 7; the SDK clause reruns green at 8 | 6 / 12 |
| 2.16e (ED-22 without packaging) | c3, d, 2.11 | 8.5 | 5.5 / 11.5 |
| 2.16a1 → a2 → b (ED-22 packaging clause, CL-24) | 2.12 (assumed month 5), 2.7 (assumed month 5) | 9.5 | **4.5** / 10.5 |

The tightest slack is ED-22's packaging clause through 2.16b: 4.5 months on the short calendar. The Director
recomputes these figures every round from actual merge dates (§5.2 step 1). K38 fires when any row's slack
falls below 3 months or when an RT-21 cost target misses (§7).

### 3.2 Reconciliation of cross-section conflicts

| # | Conflict | Resolution (binding for the loop) |
|---|---|---|
| 1 | Ledger: README puts it in Ph2; 05 has a core ledger in Ph1 | **Ph1 core ledger** (wallets, grants, custody, idempotency, audits) for starter loadouts and bounties; no player-to-player value. **Ph2 full economy** (trades, escrow, market, mail). S06 and AAA-SEC-2 stay Ph2 |
| 2 | AAA-TOOL-3 has T15 complete in Ph3; facial animation (R04) is Ph4 | 07 §2 rule: T15 is AAA-complete in Ph3 for every item whose runtime has shipped. Motion matching (02 §7.2), FACS facial and lip-sync go with R04 in WP-4.6 and gate TOOL-4 (01 and 07 updated) |
| 3 | Module order and LAYER checks absent from `HeliosModule.cmake` | WP-0.2 implements 02 §1.1. `linux-headless` is the server preset (02 §1.1 now uses this name) |
| 4 | mimalloc heap thread affinity | WP-0.6a spike before per-tag heaps; fallback is header-based tag accounting (today's design). *Spike done (2026-09-25):* pooled heaps and batched accounting, now 02 §2.2 (K3) |
| 5 | flecs `DontFragment`; RT-01 timing | WP-0.6b pre-benchmark in Ph0, so the custom-ECS decision cannot surprise Ph1. RT-01 on SERVER hardware stays the formal gate. *Pre-bench done (2026-09-25):* structural ops failed at > 2×, so ADR-004a is open, with option A in WP-1.1a (K2) |
| 6 | libopus (02 §6.2) | **Approved** (BSD-3). Recorded by WP-1.23; vendored with voice (WP-3.4) or VO cooking (WP-4.6) |
| 7 | BE-A3 (Ph1, standby ≤ 10 s) vs warm standby Ph2 (04) and SRV-12 Ph1 = reconnect (01) | **BE-A3a (Ph1):** cell kill → restart from checkpoint, 0 item deltas, loss ≤ 60 s, players reconnect. **BE-A3b (Ph2):** warm standby ≤ 10 s. 01's ≤ 10 s hitch *without* reconnect stays Ph3 (05 §10 updated) |
| 8 | BE-A5 2k tx/s at Ph3 vs AAA-SRV-8 at Ph2 | 01 wins: 2k at Ph2, 10k at Ph4 (05 updated) |
| 9 | BE-A8 50/s at Ph3 vs AAA-SRV-7 at Ph2 | 01 wins: 50/s Ph2, 200/s Ph4, 500/s Ph5 (05 updated) |
| 10 | BE-A12 weaker than AAA-CNT-7 | CNT-7, CL-9 and CL-10 govern; A12 now restates CNT-7 |
| 11 | 08 has RmlUi in Ph0; 03 has it in Ph1 | Ph1 (WP-1.6, 1.9); the Ph0 launcher is a plain SDL3 window (08 §4.3 updated) |
| 12 | Gateway port 7777 vs 27015 | UDP 7777 |
| 13 | Go 1.24 (CI, CLAUDE.md, container) vs 1.27.1 (ADR-014) | Done: `services/go.mod` pins `go 1.27` and `toolchain go1.27.1`, CI reads `go-version-file: services/go.mod`, and CLAUDE.md says 1.27.1. The agent container still has Go 1.24.7, which downloads 1.27.1 through `GOTOOLCHAIN=auto`, so the old "stay 1.24-compatible" rule is retired (K16) |
| 14 | CMake 3.24 in presets vs ≥ 3.28 (ADR-011) | 3.28 (WP-0.1) |
| 15 | 06 §12.2 criteria have no phases | GP-1, 2 Ph1. GP-4a (FBW) Ph1, 4b (command flight) Ph2. GP-3: paths and 1k NPCs Ph2, moving-ship paths Ph3, LOD Ph4. GP-5: 24 h/500 bots Ph2, 72 h/5k Ph3. GP-6 Ph2. GP-7: timers Ph2, activity resume Ph3. GP-8 Ph3. GP-9: runaway/reload Ph1, 5k quests Ph2. Round 1 added GP-10: transfer, kill, budget and crew missions Ph2, cell handoff and fleet-scale drones Ph3; GP-11: achievements Ph2, collections, codex, legacy and mirroring Ph3, seasons Ph4; GP-12: `Press` verbs, doors and terminals Ph1, the rest Ph2; GP-13 Ph3. Round 2: GP-4a gains a bit-exact FBW clause (Ph1, WP-1.16); GP-4c EVA Ph3 (WP-3.6); GP-14 (a, b) Ph3 in WP-3.2, (c–f) Ph3 in WP-3.6, (g) Ph4 in WP-4.5, (h) Ph5 in WP-5.3 (06 §12.2). Round 3: GP-15 (player fleets) Ph3 at 500 ships, (a–c) in WP-3.6 and (d) in WP-3.2, and Ph4 inside NS-4.2 in WP-4.5; GP-4d (ground vehicles and mounts) Ph2 in WP-2.9, with the tracked and motorcycle layouts joining its hash in Ph3 (WP-3.6). Round 4: GP-16 (housing and cities) Ph3, with (a–c, e) in WP-3.6, (d) in WP-3.12 (which builds the world-script host) and (f) in WP-3.2; GP-17 (territory) Ph3 with (a–d) in WP-3.6, and (e) sovereignty Ph4 in WP-4.5 |
| 16 | "3 consecutive nightlies" cannot literally cover 72 h soaks, pentests or designer days | Evidence classes (§5.6), with no threshold relaxed |
| 17 | imgui-node-editor, nats.c in the editor, libgit2 | Vendor imgui-node-editor with WP-2.10; allow nats.c in the editor (WP-0.2 MANIFEST note); git CLI only |
| 18 | Game-DLL hot reload (02 §1.2, AAA-ITR-5) vs static engine modules, `/MT` per-image heaps and `/OPT:REF` links | **ADR-016**: modular `/MD` dev builds with three engine shared libraries; shipping stays monolithic `/MT`. WP-0.6c spike (RT-18) gates RT-14 |
| 19 | Launcher UI is RmlUi on `SDL_Renderer` and SDL picks Wayland or X11 (08 §1.16, §2.1), but the vendored SDL3 build sets `SDL_RENDER OFF` and `SDL_WAYLAND OFF` | WP-0.17 enables the renderers (D3D11/D3D12 on Windows; OpenGL, Vulkan and software on Linux) and Wayland with X11 fallback, all loaded at run time, with an import audit. The code change is listed in CONSISTENCY §3 |
| 20 | 01 §1.1's north star (studios never modify engine source) had no gate for docs, templates, an SDK or project upgrades | DX track (§2.7) with AAA-TOOL-7, 8 and 9 (01 §3.9 updated) |
| 21 | PLAN.md §7 said the Phase 4 bar matched Star Citizen's shipped architecture, but SC shipped its replication layer with PES (Alpha 3.18) and static meshing on it (4.0), while AAA-SRV-9 Ph4 still lost 30 s of state and hot standby covered only "hot zones" | **ADR-007 option (a):** the Phase 4 replicant tier (04 §6.4, v1.5) for every multi-cell zone, widened in round 4 to every persistent world zone (CONSISTENCY §35); AAA-SRV-9 Ph4 ≤ 1 s there (01 updated); NS-4.6 in WP-4.3; the Improbable review moves before WP-4.3 (K18, H8 in Ph3) |
| 22 | 07 §1.8 merges `collab/*` publish PRs "never a squash" to keep per-commit authorship and ED-10's check, while §5.2 squash-merged every PR, and *Cinder Reach* content shares the engine repository's queue | Merge method by branch class (§5.2a): `collab/*` rebase-merges and is validated commit by commit; WP branches squash. `main` requires linear history, so of 07's two allowed methods the engine repository uses rebase-merge |
| 23 | Draft v2 (the round-1 review fixes, CONSISTENCY §7 and §14.10) replaced draft v1's NATS-KV leases (3 s TTL, holder self-fencing), the `LEASES` bucket and Snowflake node IDs (ADR-004; 05 §1.4, §2.3) after `services/` had implemented them, and draft v3 added failure domains (05 §1.4.3), and no rule re-baselined existing code | §5.10: declared plan changes, an anchor → path map, conformance-rework WPs (WP-0.15r first) and the CONF lint in the PR tier and the round audit; phase exits need zero findings |
| 24 | §5.2a's content tier bounced every `collab/*` commit that touched `schemas/`, but 07 T08 sends governed schema edits to `main` only through the publish PR (07 §1.8), project `.hschema` packages are pinned documents of the data session (07 §1.8.2), and each new type appends to `schema.lock.jsonc` (02 §3.4). In the engine repository, ED-22 and TOOL-10's content team could not publish a type | Rule 2 now has a **project dynamic schemas** class: non-native project and non-Foundation gem packages plus their lock entries. It adds per-commit `--check-lock`, the native-construct and SEC-1 lint, the compat classification (`Helios-Compat`), head `schema.dynamic-hot` and a code-owner approval. Engine, Foundation and `schemas.native` packages and `helios.project.jsonc` stay WP-only. WP-2.16e seeds the fixtures |
| 25 | The ADR-011 amendment (round 3) retired the per-file AVX2 allowlist, and on the same day in-tree code for WP-0.2 and WP-0.5 was written to that allowlist, with the Windows gate in `.CRT$XIB`. No §5.10 rule fired, because D3 looked only at merged code and WP branches | §5.10.4's ISA rows; rework WPs WP-0.2r and WP-0.5r; CONF-11 (per-target and per-file ISA grants) and CONF-12 (gate placement); D7 (working-tree code and review rounds); §8.1 refreshed. The gate takes 02 §1.1's first TLS-callback slot, because `.CRT$XIB` ties with mimalloc's `.CRT$XIB` and runs after every TLS callback |
| 26 | WP-2.16 bundled about 50–60 engineer-weeks against §1's 1–6-week rule, and it omitted WP-1.10 and WP-1.6 from its dependencies. RT-21, ED-22 and CL-24 gated the Phase 2 exit without showing on the critical path | Ten sized sub-WPs (§2.3a) with their own dependencies; the DX schema lane and its slack (§3.1); K38; §1 splits any Phase 0–2 scope over 6 weeks |

---

## 4. Effort and staffing reality check

### 4.1 Human-studio baseline

Engineering is engine, tools, servers and backend; "content" is design, art, audio, writing and QA. These are
planning ranges derived from the WP breakdown.

| Phase | Engineers (peak) | Content, design, QA (peak) | Calendar | Person-years |
|---|---|---|---|---|
| 0 | 12–16 | 1–2 | 9–12 months | 10–16 |
| 1 | 25–35 | 8–12 | 12–18 months | 35–60 |
| 2 | 40–55 | 20–30 | 15–21 months | 80–130 |
| 3 | 65–85 | 35–50 | 18–24 months | 140–220 |
| 4 | 65–85 | 50–80 | 18–24 months | 180–300 |
| 5 | 40–60 | 20–40 | 18–30 months | 90–200 |
| **0–4 (AAA bar)** | | | **6–8 years**, overlapped | **~450–730** |

**Phase 3 peak by track:** Core/Runtime 8–10, Rendering 10–12, Networking/Servers 8–10, Backend 6–8,
Gameplay 10–12, Editor/Tools 12–16 (a quarter on widgets, 07 §5.3, and 2–3 on the DX track, §2.7),
Client/Launcher 4–6, Infra/CI/Release 4–6, test automation 3–5, security 1–2. At $180–250k per fully loaded
person-year, Phases 0–4 cost roughly **$80–180M**, plus lab hardware, load-test compute and a pentest. That
figure answers "what would a studio spend"; §4.3 answers "what does the sponsor of the agent loop spend".

**Comparison.** SWTOR took about six years, up to 800 developers and ~$200M on a licensed engine (R03 §3.1).
Star Citizen has spent over $800M since 2012 (R04 §9). EVE runs on a 23-year-old stack with ~2.4M lines of
Python (R01 §1). Unreal and Unity embody decades of work by hundreds of engine engineers, and Godot a decade
of a small paid team plus thousands of contributors, with no MMO backend *[general knowledge, order of
magnitude, not re-verified]*. Helios is estimated lower because it reuses Jolt, Luau, flecs, netcode, NATS
and PostgreSQL, claims no Nanite/Lumen parity (01 §5.3), and ships proof content, not a 500-hour game.

### 4.2 What the agent loop changes, and what it does not

| Activity | Agent-loop effect | Still requires |
|---|---|---|
| Well-specified code with objective tests | Large speed-up: parallel WPs around the clock; codegen-heavy work (emitters, bindings, platform layers, ImGui panels) is cheap | Adversarial review and the scorecard replace senior review; drift remains possible (K24) |
| Integration, CI | No change: MSVC builds, lavapipe goldens and soaks take wall time; merges serialize | CI capacity (K26) |
| Real-GPU correctness and performance | Agents write harnesses and triage | **REF/MIN/SHOWCASE machines, 3 GPU vendors, Windows and Linux** (H1) |
| Server scale and soaks | Agents write bots, swarm tooling, analysis | **SERVER hardware and cloud budget** (H2) |
| Art, animation, audio, VO (150 → 2,000 assets per phase) | Procedural content, greybox, kitbash, placeholders; generative art only with 01 §5.2 provenance | **Artists, animators, sound designers, voice actors** (H5) |
| Motion-matching and facial data (Ph3–4) | Retargeting, digitigrade remap, cleanup scripts, the procedural and audio-viseme fallbacks (§4.3.3) | **Locomotion capture, FACS head rigs and facial capture with performer releases** (H5) |
| Docs, tutorials, templates (§2.7) | Large: reference is generated, tutorials are executable and templates are proven by scripts | A newcomer who follows the tutorials once per phase (H6) |
| Feel, fun, crash-rate hours (STB-1), designer day (TOOL-5), look and feel against reference titles (REN-8), tools used at production scale (TOOL-10) | Bots measure; they cannot judge. Agents build the capture sets and the provenance report | **Human playtesters, a newcomer designer, an external look-and-feel panel and a contracted content team** (H6) |
| Legal and trust | Scanners, checklists, SBOMs | **Counsel** (patents, licences), **HSM certificate** tied to a legal entity, **pentest vendor** (H3, H4, H7, H8) |

**Honest forecast.** Pure-engineering WPs may compress 3–10× against the human baseline. Phase exits will
not compress that much, because from Phase 1 on each needs H1–H8 evidence. At the end of Phase 0 the loop
reports WPs merged per week, review rejection rate and escaped defects, and re-forecasts Phases 1–4 from
those numbers. Until then, any calendar for an agent-built Helios is unknown.

### 4.3 Sponsor budget: H1–H8 and the loop's running costs

The loop replaces most salaried engineering, so a single sponsor does not spend the $80–180M of §4.1. The
sponsor pays for three things instead: the human-supplied inputs H1–H8 that agents cannot produce, the agents'
own compute (C1), and CI and build infrastructure (C2). This is the real budget of "run until AAA".

Prices are 2026 US street and list prices, stated as ranges. The low end assumes the Lean choices named in each
row (renting, spot capacity, procedural and CC0 content, community playtests); the high end assumes the Full
choices. Every range is re-quoted at the funding gate before the phase that spends it (§4.3.6), and actual
spend is reported monthly against it.

| ID | Input | Unblocks | First needed | Supplier |
|---|---|---|---|---|
| **H1** | GPU lab: REF, MIN and later SHOWCASE machines; NVIDIA, AMD and Intel; Windows and Linux; a DEV-class workstation; a latency rig | Every H-class criterion (RC-3/4, REN-*, CL-6, PLT-5, ITR on DEV) | Ph1 midpoint (K7) | User buys; WP-0.4 writes the list |
| **H2** | SERVER hardware and load-test cloud | RT-01, AAA-SRV-*, STB-3/4/5, the soaks | Ph1 (RT-01) | User rents or buys |
| **H3** | A legal entity and an OV or EV code-signing certificate with HSM key storage | Signed installer (WP-2.7), SEC-6, PLT-6 | Procurement in Ph1, needed in Ph2 | User |
| **H4** | Licence and privacy counsel | K19 sign-offs, ToS and privacy policy, K32 review before the public beta, performer releases | Ph1 | User engages |
| **H5** | Art, animation, audio, VO, **locomotion mocap and facial data** (§4.3.3) | Content WPs 1.22, 2.14, 3.9, 3.13, 4.6, 4.10; R04; CNT-6 | Ph1 | Contractors, directed by the Content lead |
| **H6** | Playtesters, newcomer designers, the look-and-feel panel and the content team | STB-1, STB-2 user-hours, TOOL-5, tutorial walk-throughs; **AAA-REN-8** panels at the Ph1, Ph3 and Ph4 exits (01 §3.3.1; ≈ $4–8k, $10–20k, $12–24k); **AAA-TOOL-10**: the Ph3 rehearsal (≈ $3–6k) and the Ph4 content-team run (≈ $15–45k; 01 §3.9.1) | Ph1 (REN-8 panel), Ph2 (STB-1 hours) | Test vendor or community alpha; the panel and the content team through contracted vendors |
| **H7** | External security testing | AAA-SEC-8; the Ph3 public beta | Ph3 | Pentest vendor |
| **H8** | Patent counsel | K17 (Ph4 release), K18 (WP-4.3 replicant tier; WP-5.2 follow-up) | Ph3 (the Improbable review before WP-4.3 starts; SWG terrain by the Ph3 exit) | User engages |
| **C1** | Agent compute | Every WP | Ph0 | API account or plan |
| **C2** | CI and build infrastructure | PR, nightly and weekly tiers (§5.6) | Ph0 | Hosted runners plus rented boxes |

**Budget by phase** (US$, low–high; k = thousand, M = million):

| | Ph0 | Ph1 | Ph2 | Ph3 | Ph4 | **Ph0–4** |
|---|---|---|---|---|---|---|
| H1 GPU lab | 0 (user's PC) | 6–12k | 1.2–3.4k | 1–3k | 4.2–7.3k | **12–26k** |
| H2 servers and load tests | 0 | 3–8k | 11–28k | 35–100k | 50–195k | **0.10–0.33M** |
| H3 entity and signing | 0 | 0.5–2k | 0.5–1.5k | 0.5–1.5k | 0.5–1.5k | **2–7k** |
| H4 licence and privacy counsel | 0 | 1.5–9k | 8–25k | 10–30k | 5–15k | **25–79k** |
| H5 content, VO, mocap, facial | 0 | 31–210k | 90–670k | 193k–1.41M | 312k–2.13M | **0.63–4.42M** |
| H6 playtests, newcomers, REN-8 panels, TOOL-10 team | 0 | 5–11k | 7–14k | 37–76k | 89–194k | **138–295k** |
| H7 security testing | 0 | 0 | 0 | 15–35k | 50–120k | **65–155k** |
| H8 patent counsel (SWG terrain; Improbable before WP-4.3) | 0 | 0 | 0 | 25–70k | 0 | **25–70k** |
| C1 agent compute | 9–54k | 108–745k | 215k–1.41M | 368k–2.32M | 398k–2.67M | **1.1–7.2M** |
| C2 CI and build infra | 7–35k | 18–94k | 26–120k | 40–173k | 40–173k | **0.13–0.60M** |
| **Total** | **16–89k** | **0.17–1.09M** | **0.36–2.27M** | **0.72–4.22M** | **0.95–5.51M** | **≈ $2.2–13.2M** |

**Reading the table.** Reaching the AAA bar costs the sponsor roughly **$2.2M (Lean) to $13M (Full)**, an order
of magnitude less than §4.1's studio figure. Two rows dominate: agent compute (C1, 49–55 %) and content (H5,
28–34 %). Lab and server hardware plus load tests (H1, H2) are only 3–5 %, but they gate every H- and W-class
criterion, so they are funded first. Phase 5 is extra: about 2,000 more assets, 100k-bot shard runs (05 §6.4
sizes 240–480 cells at 100k CCU), a SHOWCASE-AMD box and an Improbable follow-up for the WP-5.2 gateway layer
(H8, $5–15k; the main review is in Ph3). It is costed at gate F4 only if the user opts in.

#### 4.3.1 H1: GPU lab bill of materials

Each lab machine dual-boots Windows 11 24H2 and Ubuntu 24.04. The nightly runs Windows first, reboots once
(`bcdedit /bootsequence`, `grub-reboot`) and then runs Linux, so one box covers both operating systems.

| Machine | Spec (01 §3.1) | Phase | Cost |
|---|---|---|---|
| REF-NV | Ryzen 7 7700, 32 GB, NVMe, RTX 4070 | 1 | $1.5–1.9k |
| REF-AMD | Same, RX 7800 XT | 1 | $1.5–1.9k |
| MIN-NV / MIN-AMD | Ryzen 5 3600, 16 GB, SATA SSD, GTX 1660 SUPER / RX 5600 XT (refurbished parts) | 1 | $1.2–1.8k for both |
| Intel | REF-class box with an Intel Core CPU (Core Ultra 5 245K class) and an Arc B-series GPU: correctness goldens and the cross-vendor determinism nightly (RT-03/04, 04 NS-3.8), no BENCH tier | 1 | $1.2–1.5k |
| DEV | Ryzen 9 7950X, 64 GB, RTX 4070 Ti SUPER (iteration budgets); $0 if the user's PC already qualifies | 1 | $0–3.8k |
| Lab plumbing | Switch, UPS, smart PDU for remote power-cycling, KVM | 1 | $0.6–1.2k |
| Latency rig | Photodiode and microcontroller (08 §1.3a) | 2 | $0.15–0.4k |
| SHOWCASE-NV + HDR10 monitor | Ryzen 7 9800X3D, RTX 5080 (DLSS plugin, 120 fps mode, HDR10) | 4 | $3.2–4.3k |
| Refresh and power | Driver-regression spares and replacements, and about $80 a month of power | 2–4 | $1–3k per phase |

#### 4.3.2 H2: servers and the load-test cost model

- **Ph1:** one rented dedicated EPYC 9004-class box for RT-01, cell budgets and the 50-bot nightly, at
  $250–450 a month.
- **Ph2 (WP-2.15):** three rented boxes (cells and gateways, backend, bots), $750–1,350 a month.
- **Ph3:** a permanent staging cluster (3 K8s nodes, PG HA, NATS ×3, Valkey), $1.5–3k a month, plus the weekly
  6 h 10k-bot chaos run: 30–50 cells, 5–10 bot instances and the backend, **$90–250 a run**.
- **Ph4:** the staging cluster continues, plus the 50k-CCU soak and the 3–5× stress runs below.

The per-run model uses 05 §6.4's sizing. At 50k CCU that is 100–200 world cells at 250–500 players each, plus
a warm pool sized to the largest rack (05 §1.4.6: ⌈(C_max + 1) · 9/8⌉ over 9 racks, so 15–27 slots, or
13–15 %), plus replicants for every world zone (2 vCPU per ≤ 4 cells, ≈ 6 %, 6–13 cell-equivalents). That totals ≈ 120–240
cell-equivalents. The Ph3 chaos run's 30–50 cells likewise include the Ph3 host-sized pool of ≈ 30 %. A cell-hour costs $0.20 (spot or committed) to $0.45 (on demand). A bot instance (8 vCPU,
1,000–2,000 bots, 04 §10.3) costs $0.15–0.41 an hour.

| 72 h soak at 50k CCU (STB-4, NS-4.1) | Quantity | Cost per run |
|---|---|---|
| Cells | 120–240 × 72 h | $1.7–7.8k |
| Gateways (8k sessions each, spread over 3 zones for the zone-loss rule, 05 §6.7) | 12 × 72 h | $0.15–0.36k |
| Backend (PG primary and replica, NATS ×3, Valkey, services) | $10–20 an hour × 72 h | $0.7–1.4k |
| Bots (50k) | 25–50 instances × 72 h | $0.3–1.5k |
| Observability and storage | | $0.2–0.5k |
| Egress | $0 with bots in the same region; one 1 h internet-path segment at 12.8 Gbit/s (≈ 5.8 TB at $0.02–0.05/GB) | $0.1–0.3k |
| **Total** | | **≈ $3–12k per run** |

- **Egress rule.** Soak bots run in the shard's region. Egress for the full 72 h would add about 415 TB, or
  **$8–21k per run**, and prove nothing more than the 1 h segment does.
- **Runs needed.** W-class evidence (§5.6) needs 2 consecutive passes, and failures before them are likely. The
  budget is 4–6 soak runs ($12–72k).
- **Stress runs.** A 4 h run at 3–5× target (150–250k bots against the 50k shard) tests admission and overload.
  The extra bots mostly wait in the login queue, so a run costs **$0.2–1k**; 12–24 runs are budgeted in Ph4.
- **K30 trigger.** A run that costs more than 125 % of this model is investigated before the next run.

#### 4.3.3 H5: content, VO, mocap and facial data

**Assets.** 01 §4.2 adds about 150, 500, 1,000 and 1,500 unique assets in Phases 1–4. The blended price per
asset is **$170 (Lean) to $1,300 (Full)**:
- **Lean:** 70 % procedural, kitbash or CC0 at about $0; 27 % outsourced props and modules at about $300; 3 % hero
  assets (ships, characters, creatures) at about $3k.
- **Full:** 50 % procedural or CC0 at about $50; 45 % outsourced at about $1,200; 5 % hero assets at about $15k.
- The *Bastion* capital alone is $30–60k in the Full case.

| H5 line | Ph1 | Ph2 | Ph3 | Ph4 |
|---|---|---|---|---|
| Unique assets | 26–195k | 85–650k | 170k–1.3M | 255k–1.95M |
| Music and SFX (commissioned music; SFX libraries whose licences allow game redistribution) | 5–15k | 5–20k | 5–25k | 10–40k |
| Locomotion capture for motion matching (below) | — | — | 18–85k | — |
| VO: ~300 lines in 2 languages, about 12 characters, casting, studio and direction; plus translation of the second language | — | — | — | 20–65k |
| FACS head rigs and facial capture (below) | — | — | — | 27–75k |
| **Total** | **31–210k** | **90–670k** | **193k–1.41M** | **312k–2.13M** |

**Motion and facial data.** Motion matching (02 §7.2, WP-4.6), FACS facial animation and the digitigrade Keth
need data that neither agents nor CC0 packs supply.
- **Locomotion database (WP-3.13).** About 60–90 minutes of usable human capture: unarmed, rifle and pistol
  stances; walk, jog, run and sprint; starts, stops, pivots, strafes and turns; crouch; slopes and stairs.
  - *Full:* 2–3 days at an optical studio ($6–15k a day, including a performer and technician) plus cleanup,
    solving and retargeting to the Helios skeleton, $30–85k.
  - *Lean:* a purchased inertial suit ($3–15k), contracted performer days ($0.5–1.5k each) and animator cleanup,
    $18–45k.
- **Keth.** There is no digitigrade capture source. The human set is remapped by the digitigrade leg IK in 02
  §7.2's retargeting, plus about one animator-day of keyed stride cycles ($3–8k, included above). An optional
  capture day with a performer on digitigrade leg extensions is a Full-case item.
- **Facial (WP-4.6).** FACS rigs for 4–6 hero heads ($3–8k each) and performance capture recorded with the VO
  sessions, using a depth-camera phone or a head-mounted camera with 52-blendshape output solved to Helios FACS
  curves: $27–75k in total. All other lines use audio-driven visemes.

**Provenance and licence rules (01 §5.2).**
- Captures are work-for-hire, with the rights assigned to the sponsor. Each performer signs a release covering
  likeness, performance, use for solving and retargeting, and storage of **biometric** facial data. Counsel
  reviews the release templates in Ph3 (H4).
- Raw facial video is deleted after solving unless the release allows retention. Only solved curves enter the
  asset DB.
- Every clip's `.meta` records source, performer, session, licence and consent (07 T24 provenance).
- Public datasets are allowed only under CC0 or with counsel sign-off. Non-commercial or no-derivatives
  licences, and any data taken from a commercial game, are rejected by the licence scanner and T28.

**Fallback (so R04 and WP-4.6 are never blocked).** If no source is contracted by the Ph3 midpoint (K36 trigger),
the fallback becomes the plan of record by ADR:
- **Locomotion:** procedural locomotion (inertialized blend spaces, foot and digitigrade leg IK, procedural
  stride warping) over about 10 minutes of animator-keyed clips. The motion-matching runtime and T15 tooling are
  still built and proven on a database generated from those clips.
- **Facial:** audio-driven visemes from the VO (phoneme alignment in `helios-assetd`) drive the FACS rig. T13
  auto-staging adds emotion curves.
- **No threshold changes.** R04, RT-15, AAA-CNT-6 and ED-13 keep their thresholds. Only visual fidelity drops,
  and the phase-exit auditor records that.

#### 4.3.4 C1 and C2: agent compute and CI

- **C1 model.** Take an implementer session of 1–2 hours. It reads about 5–20M tokens (mostly cache reads) and
  writes 0.2–1M. At Opus-class list prices ($5 per million input tokens, $25 per million output tokens, cache
  reads about $0.50), that is $10–40 a session. One engineer-week of WP scope takes 3–10 sessions, and review,
  rework, CI triage, bisects and audits add 50–100 %. The planning value is **$150–600 per engineer-week of WP
  scope**.
- **Scope.** Phase 0 is about 60–90 engineer-weeks. From Phase 1, scope is 60 % of §4.1's engineering
  person-years × 46 weeks: agents carry no hiring, onboarding or meetings, but rework counts.
- **Fixed-price plans.** A flat-rate plan changes the dollars, not the token count, and its rate limits cap
  parallelism (§5.5).
- **C1 re-forecast.** At the Phase 0 exit the Director replaces the planning value with measured **$ per merged
  WP** and **$ per engineer-week** (§4.2). F0 uses the measured values.
- **C2 model.** A PR-tier run is about 300–450 runner-minutes, 40 % on Windows at twice the Linux rate. At list
  prices that is ≈ $2.5–5 a run, and there are 15–40 runs a day from Phase 1.
- **C2 extras.** A rented Linux box (two from Phase 2) takes micro-benchmarks, fuzzing and lavapipe nightlies at
  $250–450 each a month. Artefact storage (symbols, goldens, DDC, perf history) is $50–300 a month.
- **Public repository.** Hosted minutes on standard runners are free for a public repository, and C2 falls to
  about $20–60k in total.
- **The Windows GPU runner** (WP-0.4) costs nothing, but it runs only post-merge work (§5.4a).

#### 4.3.5 What the budget does not buy

- Nothing here relaxes a threshold. An unfunded input leaves its criteria *unmeasured*, and unmeasured counts as
  failing (§5.6).
- Salaries for a live-operations team, marketing, and hosting for real players after launch are out of scope.
  05 §6.4 sizes live hosting at $42–105k a month at 50k CCU; the plan builds the engine and proof game, not a
  commercial service (01 §5.1).

#### 4.3.6 Funding gates F0–F4

| Gate | When | The Director presents | The user decides |
|---|---|---|---|
| **F0** | Phase 0 exit (before Phase 1 WPs other than interface WPs) | Measured C1 and C2 (§4.3.4); re-quoted Ph1 envelope and an indicative Ph2 envelope; the WP-0.4 purchase list | One of: **Full**; **Lean**; **Engine-only** (C1, C2, H1, H2 and the H4 licence sign-offs; content stays agent-made greybox; criteria needing H3, H5–H8 stay red, so the loop can reach the Phase 1 exit at most); or **stop** |
| **F1** | Phase 1 exit | Actuals against F0; Ph2 envelope; the H3 entity and certificate status | Same choices for Phase 2 |
| **F2** | Phase 2 exit | Ph3 envelope, including the H7 focused test, H8, locomotion capture (or the fallback) and H6 community versus paid playtests | Same choices for Phase 3 |
| **F3** | Phase 3 exit | Ph4 envelope: 50k soaks, the pentest, VO, facial, the 3,000 STB-1 play-hours, the REN-8 panel and the TOOL-10 content team | Same choices for Phase 4 |
| **F4** | Phase 4 exit (the AAA bar) | A Phase 5 envelope, only if the user opts in to Phase 5 (§5.7) | Fund Phase 5, or stop at the bar |

- **Recording.** Each decision is signed in `docs/evidence/funding-F<n>.md`. The Director never starts a
  human-gated WP (for example WP-1.23, WP-2.15, WP-3.13 or WP-4.11) without an approved envelope.
- **Overruns.** Actual spend above 110 % of any row's envelope for two consecutive months triggers K34 and a
  re-plan before more is spent.
- **Risk triggers tied to these numbers:** K7 (H1, H2), K23 (H5), K30 (H2, C2), K34 (the whole envelope) and K36
  (mocap).

---

## 5. The build loop

### 5.1 Roles

- **Director** (orchestrating agent): plans rounds from the scorecard, assigns WPs, holds module locks, runs
  the merge queue and escalates human-gated items. When a plan change merges, it opens conformance-rework
  WPs and re-baselines in-flight WPs in the same round (§5.10). A review round's fixes are resolved against the
  working tree too (D7). It refreshes §8.1 at the start of every round.
- **Implementer** (one agent per WP): tests first where possible; works in branch `wp/<id>` and its own build
  directory `build/<wp-id>`.
- **Adversarial reviewer** (fresh agent, no implementer context): tries to break the change, writes extra
  failing tests, and checks CLAUDE.md rules, determinism, threading, parser bounds, Windows portability,
  licences and budget claims. It can block.
- **Integrator:** rebases, runs the tier for the branch class, merges with that class's method (§5.2a),
  updates `scorecard.jsonc`.
- **Auditor** (independent, per round and per phase): scores the scorecard (§5.7), deep-audits two random
  merged WPs per round, and runs the conformance lint over the full tree (§5.10.3).
- **User** (human, Windows): validates milestones (§5.9), provides H1–H8, decides the funding gates (§4.3.6) and
  accepts phase exits.

### 5.2 Rounds

1. **Select.** Rank failing criteria by current phase, then critical-path slack, then risk. Pick ready WPs
   (dependencies merged) whose criteria fail, within §5.5 limits.
2. **Interface pass.** A WP that changes a shared contract first lands a header- or schema-only PR.
3. **Implement and self-check** locally: `linux-gcc`, `linux-clang`, `cross-mingw`, `linux-debug-asan` for
   touched modules, `go test ./...` if `services/` changed, and `xvfb-run -a ctest --preset linux-gcc -L gpu`
   for render changes.
4. **Adversarial review**, at least one round. Each blocking finding is fixed or rebutted with evidence.
5. **CI PR tier** (§5.6): the ADR-001a 8-configuration matrix (`windows-msvc`, `windows-msvc-dev`,
   `windows-msvc-floor`, `windows-clang-cl`, `linux-gcc`, `linux-clang`, `linux-headless`, `cross-mingw`),
   the conformance lint on changed paths (§5.10), Go on Windows and Linux, lavapipe goldens, 16-bot smoke.
6. **Integration verify.** The merge queue brings the branch up to date with `main` and reruns the tier for
   its branch class (§5.2a). Failure bounces the WP. A WP whose `Plan-Rev` predates a plan change mapped to
   its paths is also bounced for re-baselining (§5.10).
7. **Merge by branch class (§5.2a).** WP branches squash-merge with the WP ID and criteria in the message.
   `collab/*` content publishes rebase-merge commit by commit and are **never squashed**.
8. **Score.** The nightly run and the round audit feed the next selection.

A round ends when every selected WP is merged or bounced (target: 1–3 days).

### 5.2a Merge policy by branch class

*Cinder Reach* lives in the engine repository (§2.7), so engine WPs and the collab service's content publishes
(07 §1.8) share one `main` and one queue. The two need different merge methods. One commit per WP keeps
reverts and bisects atomic. A publish must keep each author's commits, `Helios-Tx` trailers and the order
in which the edit instance really changed.

**Repository settings (WP-0.1).** The `main` ruleset requires a pull request and **linear history**. It
allows squash and rebase merges and disables merge commits. Only the Integrator's bot identity may merge.
The queue is the Integrator's serialized script (`tools/ci/merge_queue`), not GitHub's native merge queue,
because a native queue applies one merge method to every PR in it. Of the two methods 07 §1.8 allows, the
engine repository therefore uses rebase-merge. A studio's own project repository may use either.

| Branch class | Opened by | Merge method | Queue validation | Message and trailers |
|---|---|---|---|---|
| `wp/<id>` (and sub-WPs `wp/<id>a`), `iface/<id>` interface PRs, `fix/<id>` P0 fixes, `conf/<id>` conformance rework (§5.10) | Implementer | **Squash** | Rebased on `main`, then the PR tier (§5.6) on the result | Title `WP-<id>: <scope>`. Trailers `WP:`, `Criteria:`, `Plan-Rev:` (§5.10), `Plan-Change:` when `docs/plan/**` or `docs/adr/**` changed, then the attribution lines |
| `collab/<session>` (07 §1.8 publish of *Cinder Reach* or template content, including governed project-schema edits from T08) | Collab service (committer); designers or designer agents (authors) | **Rebase-merge, never squash.** Exempt from step 7's squash | The content tier below, commit by commit | Per commit, as 07 §1.8 exports it: the git author is the designer and `Helios-Tx: <first>..<last>` and `Helios-Session:` are trailers, plus `Helios-Compat:` on schema commits. The PR body names the content WP (for example WP-2.14) |

**Content tier for `collab/*` (≤ 30 min p50 for ≤ 200 commits).**
1. **Up to date by re-export, never by a GitHub rebase.** The branch's base must be the current `main` head.
   If `main` has moved, the queue calls the collab service's `Resubmit`. That runs 07 §1.8's session rebase
   (per-property 3-way merge, and `helios-tool migrate` when a schema changed) and re-exports the branch on
   the new head. A conflict goes back to its authors, and the PR leaves the queue until it is re-exported.
   The commits that merge are therefore exactly the ones the service exported and CI validated. GitHub's
   rebase-merge of an up-to-date branch changes only committer and SHA; authors, trees and trailers stay
   the same. **No starvation:** a re-exported publish goes to the head of the queue. After its second
   re-export in a row, the Integrator holds WP merges until the publish's content tier finishes.
   **Carries first:** a publish that depends on another session's unpublished journal (07 §1.8.2) enters the
   queue only after the carry PRs the service opened for it (`collab/<session>-carry-<n>`, same branch class)
   have merged. Carries go to the head of the queue, and the dependent PR is then re-exported as above.
2. **Scope.** Every commit touches only the two publishable path classes below. Anything else bounces the PR
   and names the commit. Governed schema edits are publishable because 07 T08 sends them to `main` only
   through the publish PR (07 §1.8), project `.hschema` packages are pinned documents of the data session
   (07 §1.8.2), and a new type appends to the project's `schema.lock.jsonc` (02 §3.4).

   | Class | Paths | Extra validation | Approval |
   |---|---|---|---|
   | **Content** | `content/**`, `templates/*/content/**` and their LFS objects | Rules 3 and 4 | The adversarial reviewer's content checklist (rule 6) |
   | **Project dynamic schemas** (02 §3.8) | `schemas/<pkg>/**` and `templates/*/schemas/<pkg>/**` for every package that the project's `helios.project.jsonc` does not list in `schemas.native`, including a new package, because dynamic is the default; `gems/<gem>/schemas/**` of non-Foundation gems; the project's `schema.lock.jsonc`, limited to appended or tombstoned entries of those packages | Rule 3's schema checks on every such commit; rule 4's `schema.dynamic-hot` run on the head | A code owner of each touched package approves the PR head. `.github/CODEOWNERS` lists them: for *Cinder Reach*, the owning content WP's implementer and the Gameplay-track reviewer; in a studio's repository, the project's schema owners (07 T08, "the project's reviewers approve it"). The queue script checks the approval before rule 6 |

   **Still WP-only (bounced here):**
   - engine schemas (`engine/<module>/schema/`) and every other path under `engine/`;
   - Foundation gems (`gems/foundation-*/**`), whose packages are native;
   - any package listed in `schemas.native`;
   - `helios.project.jsonc` itself. Its `schemas.native` list moves a package to native, which needs a C++ and
     Go build;
   - lock entries of engine, Foundation or native packages, and any lock edit that renumbers or deletes an
     entry;
   - `services/`, `tools/`, `third_party/`, `.github/` and CMake files.
3. **Per-commit validation.** For each commit C in `main..head`, in order:
   1. **Schema checks** (only when C touches a project-schema path):
      - `helios-schemac --check-lock` at C. No ID is reused, and the lock diff from C^ holds only appended or
        tombstoned entries of dynamic packages that C touches.
      - schemac's lint. It rejects native-only constructs in a dynamic package (`scriptlib`, `service`,
        `message`, `relation`, `@predicted`, `@store(ledger)`, `@ledger_policy`, `@sql`, custom `@table`), and
        it applies SEC-1 (`@ratelimit` and `@intent` on client→server `rpc`s) and the unit and range rules.
      - **Compat classification.** The compat fingerprint (05 §1.14.1) is computed at C^ and at C from the DDC.
        An equal pair is `none`; a changed pair is `client`. The result must equal the commit's
        `Helios-Compat:` trailer, which the collab service writes from T08's migration preview. On a
        `release/*` target, a `client` commit bounces unless the PR opens a new compat epoch
        (05 §1.14.1).
   2. Incremental T28 from C's parent to C (`helios-tool validate --since C^ --at C`, sharing the DDC). This
      also revalidates every record against the types at C, so a schema change must carry its record
      migrations in the same commit, as T08's migration preview writes them.
   3. The schema-lock check, the reference-name grep and 100 % provenance (T24).

   Commits are sharded over parallel runners when a publish exceeds 50 commits. Schema checks add ≤ 20 s per
   schema commit (schemac builds 2,000 types in ≤ 1 s, and the fingerprint reuses the DDC), so the tier's
   budget holds. One failing commit bounces the whole PR and names the commit and its `Helios-Tx` range. This
   is the run 07 §1.8 promises: every intermediate commit is a state the edit instance really had, so each one
   must pass.
4. **Head validation.** Cook; the 16-bot smoke on the cooked build; lavapipe goldens of the scenes the diff
   touches; ED-10's journal-vs-export diff = 0 on the head (07 §1.8). If the publish touched a project schema,
   `schema.dynamic-hot` also runs from the smoke's per-type generic-path costs (02 §3.8). After a rebase-merge
   the head tree on `main` equals the branch head tree, so the ED-10 evidence stays valid on `main`.
5. **Trailer integrity.** `Helios-Tx` ranges strictly increase and are contiguous. Together they cover
   exactly the published journal range, or the dependency-closed subset the service recorded for a partial
   publish. Every author maps to a studio account, and the committer is the collab service identity. Every
   commit that touches a project-schema path carries `Helios-Compat: none|client`.
6. **Merge** with `gh pr merge --rebase`, after the code-owner check of rule 2 for schema publishes. The
   adversarial reviewer's content checklist (provenance, names, budgets of touched scenes) applies as for any
   PR.

**Fixtures (WP-2.16e).** The content tier's script carries two seeded publishes:
- A three-commit T08 publish must rebase-merge as three commits: a new record type in a *Cinder Reach*
  dynamic package with its lock entries, then a record that uses it, then a `ScriptState` component.
- The same publish must bounce and name the offending commit in each of four variants: one commit edits a
  package listed in `schemas.native`; one edits a Foundation gem's schema; one edits `helios.project.jsonc`;
  one carries a `Helios-Compat` trailer that disagrees with the fingerprint.

**Post-merge `merge-policy` check** (on `push` to `main`, WP-0.1). It compares what landed with the PR:
- A `collab/*` PR must land as the same number of commits, with the same authors, trees and trailers.
- A WP-class PR must land as exactly one commit.

A violation fails the check. The Integrator then reverts the landed commit(s) and re-merges with the correct
method within the same hour, so per-commit authorship and the ED-10 evidence are restored. A second
violation in a round raises K25.

**Bisect and revert.** The nightly bisect agent (§5.8) steps through content one author run at a time. A
culprit inside a publish reverts that publish's whole commit range (`git revert <first>^..<last>`), because
its commits are dependency-ordered. The session then fixes the problem and publishes again.

### 5.3 Definition of done (per WP)

1. Scope delivered; public APIs documented with threading rules.
2. New behaviour has tests that fail without the change (the reviewer spot-checks by reverting).
3. Warning-clean on MSVC (the primary 14.5x and the 14.44 floor, ADR-001a), clang-cl, GCC 13, Clang 17 and MinGW;
   no OS-specific code outside `platform/`.
4. `ctest` green, including `gpu` on lavapipe and sanitizer runs of touched modules.
5. Layer, licence, schema-lock, AAA-SEC-4 and reference-name lints pass.
6. Performance-sensitive code states a budget with a benchmark and stays within thresholds.
7. Golden updates carry a justification the reviewer approves.
8. Every acceptance criterion is automated and registered in `scorecard.jsonc` with its evidence class.
9. Docs are updated: the module README; a doc string for every new public Luau API, record type or field,
   component, CVar and `helios-tool` command (the TOOL-7 coverage gate); the manual page of every new or changed
   tool (`docs/manual/tools/Txx.md`); and the affected plan sections. A changed decision gets an ADR entry. A
   breaking change to the public API (§2.7.2) ships as a deprecation with a migration step for
   `upgrade-project` (TOOL-8).
10. No open blocking review findings; integration verify green.
11. **Conformance (§5.10).** The branch's `Plan-Rev` is current for every anchor mapped to the paths it touches,
    and the conformance lint is clean on those paths. Code that implements a normative anchor registers its
    paths in `tools/conformance/map.jsonc`. A WP that changes the plan declares `Plan-Change:` and adds the
    CONF rule or conformance test that checks the change. Code written ahead of the round order records the
    `Plan-Rev` it was written to in its module README (D7).

### 5.4 Cross-platform policy

Agents run in Linux containers, so MSVC and clang-cl are verified **only in CI**, on `windows-latest` (VS 2026,
the primary toolset) and the pinned `windows-2022` (MSVC 14.44, the floor and release toolset; ADR-001a), and
local MinGW stands in for Win32 API portability. A WP touching `platform/win32`, the installer, WinHTTP,
Credential Manager or DPI must add a Windows-runner test that exercises that code. The nightly run on the
user's PC (WP-0.4) covers the Windows Vulkan path, on post-merge code only (§5.4a).

### 5.4a Self-hosted runner policy (the user's PC)

GitHub advises against self-hosted runners for untrusted pull-request code. Here all code is agent-authored,
and the runner is the user's own machine, so `win-gpu` executes only code that has passed adversarial review
and the merge queue. It is still treated as untrusted (K33).

| Control | Rule | Enforced by |
|---|---|---|
| Triggers | Only `schedule`, `push` to `main` and `workflow_dispatch` started by the user. No `pull_request`, `pull_request_target` or `workflow_run` triggers. Jobs also guard with `if: github.ref == 'refs/heads/main'` | `tools/ci/check_runner_policy` in the PR tier fails any workflow that targets the `win-gpu` label and breaks a rule |
| Secrets | No repository, environment or organization secrets reach `win-gpu` jobs. `permissions: contents: read`; results upload as artefacts. Signing keys, cloud credentials and deploy tokens exist only in hosted-runner environments with required reviewers | Same check (no `secrets.*` in those jobs); GitHub environment protection |
| Account isolation (default) | The runner service runs as a dedicated local **standard** account `helios-ci`, with no Administrators membership, no access to the user's profile, browser data, SSH or Git credentials, or network shares. The work directory is on its own volume (`D:\helios-ci`), BitLocker is on, and a job-started hook (`ACTIONS_RUNNER_HOOK_JOB_STARTED`) wipes the work volume so every job starts clean. No registration token is stored on the machine | A first job step asserts the account is not in Administrators and that the user's profile folder is unreadable; runner registration docs in WP-0.4 |
| VM isolation (preferred when it works) | A Hyper-V VM with GPU partitioning (GPU-P) hosts the runner, if the WP-0.4 smoke shows Vulkan 1.3 conformance (the RC-1 suite) inside the guest. Otherwise the account isolation above is used, and the decision is recorded | WP-0.4 ADR |
| Network | No inbound ports. Per-account outbound rules (`New-NetFirewallRule -LocalUser`) block the rest of the home LAN except the lab machines | Firewall script in WP-0.4 |
| Blast radius | The user's interactive validation (§5.9) runs from the user's own account on a separate clone, never from the runner's work directory | §5.9 script |

### 5.5 Parallelism limits

- **Per container** (4 vCPU today): at most 2 concurrent full builds; shared ccache.
- **In flight:** at most 6 implementer WPs, about one per track with ready work, spread across sessions.
- **Module locks:** one WP at a time may change a module's public headers.
- **Merges** go through one queue, with a merge method per branch class (§5.2a). Wire formats, schema grammar
  and the RHI API change only via interface WPs.
- Scale up only while review rejection stays below 30 % and the PR tier below 45 min p50.

### 5.6 CI tiers and evidence classes

| Tier | When | Contents | Budget |
|---|---|---|---|
| **PR** | Every push to a WP-class branch | The ADR-001a 8-configuration matrix (primary and floor MSVC, MSVC modular, clang-cl, GCC, Clang, headless, MinGW), unit tests, lints (including the conformance lint on changed paths, §5.10), Null traces, lavapipe goldens, Go tests (Win + Linux), 16-bot smoke, fixed-runner micro-benchmarks | ≤ 45 min |
| **Content** | Every `collab/*` publish | §5.2a: per-commit T28, schema lock, name grep and provenance, plus schemac `--check-lock`, the native-construct and SEC-1 lint and the compat classification on project-schema commits; head cook, 16-bot smoke, touched-scene goldens, ED-10 diff, `schema.dynamic-hot` after schema changes; trailer integrity; code-owner approval for schema publishes | ≤ 30 min p50 (≤ 200 commits) |
| **Nightly** | Daily | All N- and H-class criteria, BENCH on the lab, 1 h soak with 50 → 1k bots, 1 h fuzz per target, ASan/UBSan/TSan, 5-compiler determinism hashes, editor perf | ≤ 8 h |
| **Weekly** | Weekly | 10k bots with chaos (Ph3+), 24 h and 72 h soaks, full cook, cross-compiler replay | ≤ 3 days |
| **Release** | Phase exits | ≥ 24 CPU-h fuzzing, pentest (Ph4), legal checklist, signing verification | — |

**Evidence classes** apply 01's rule to criteria that cannot run nightly, relaxing no threshold:
- **N (nightly):** 3 consecutive passing nightly runs on Windows and Linux (01's rule, literally).
- **H (hardware):** as N, on lab hardware. With no lab the criterion is *unmeasured*, which counts as failing.
- **W (long-running):** 2 consecutive scheduled passes, the latest within 14 days of the exit streak. Server
  soaks run on Linux, plus one Windows run where PLT-2 parity applies.
- **M (manual or external):** a signed record in `docs/evidence/` within the exit window (designer day,
  look-and-feel panel REN-8, content-team zone TOOL-10,
  pentest, legal). Rate criteria need ≥ 3 ÷ target-rate observed hours: STB-1 at Ph4 (1 per 1,000 h) needs
  ≥ 3,000 play-hours.

### 5.7 Termination condition and independent scoring

- **Every round** an auditor scores 0–10: 60 % is the fraction of Ph ≤ *N* criteria passing under §5.6, and
  40 % is a rubric (01 §2 archetype proofs playable, code health, docs and tutorials (TOOL-7 status and the
  nightly tutorial pass rate), the human-UI share from TOOL-10's provenance report, the latest internal REN-8
  rubric trend, platform parity). **Code health scores 0** while the full-tree conformance lint has a finding
  with no open rework WP, or §8.1 is older than the round (§5.10.3). The auditor lists the top five failing
  criteria, the open conformance findings and the spend against the approved envelope (§4.3).
- **Every phase exit** needs two independent auditors at **≥ 9/10**, every criterion green, no open critical or
  high risk without an accepted mitigation, no conformance finding or open rework WP (§5.10), and the user's
  Windows validation.
- **Loop termination ("AAA reached")** is the Phase 4 exit: every Ph ≤ 4 criterion green on Windows and Linux,
  two auditors at ≥ 9/10 on closeness to the user's goal (a studio could build SWG-, EVE-, Destiny-, Star
  Citizen- and SWTOR-class games, shown by the five archetype proofs in *Cinder Reach*), and user acceptance.
  Phase 5 runs only if the user opts in.
- **Pauses.** A criterion blocked on H1–H8, or on a funding gate the user has not approved, is parked and
  reported; the loop works elsewhere and never marks it green.

### 5.8 Preventing regressions

- **Ratchet.** A criterion that turns green joins its tier's blocking set; a PR that turns it red cannot merge.
- **Nightly failures** start a bisect agent, which reverts the culprit or opens a P0 fix WP that pre-empts the
  next round. A culprit inside a content publish reverts that publish's whole range (§5.2a).
- **Plan drift** is a regression too: a conformance-lint finding on `main` is handled like a red criterion
  (§5.10).
- **Budgets** are tracked per commit: ±5 % for render passes, ±10 % for backend, editor and iteration.
- **Goldens and determinism hashes** change only with justification; a hash change needs an ADR-level reason.
- **Flaky tests** are quarantined for at most 7 days and then count as failing; no skip without a linked issue.
- **Anti-gaming.** Auditors check that tests exercise real paths (bots use the real client core) and
  mutation-test samples of merged code.

### 5.9 How the user validates milestones on Windows

Install **Visual Studio 2026** (recommended; the primary toolset) or **Visual Studio 2022 17.14 or later** (the
floor, ADR-001a) with "Desktop development with C++" and "C++ CMake tools", which provide CMake and Ninja. Also
install Git for Windows with LFS, Go 1.27.1 and a Vulkan 1.3 driver. Then open the **x64 Native Tools Command
Prompt** of that Visual Studio:

```bat
git pull
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
cd services && go build -o ..\build\go\ ./cmd/... && cd ..
powershell -ExecutionPolicy Bypass -File tools\milestone\validate.ps1 -Milestone M1
```

`validate.ps1` (WP-0.3) accepts either toolset:
- **Toolset check.** It reads `CMAKE_CXX_COMPILER_VERSION` from `build\windows-msvc-release\CMakeCache.txt`
  and accepts 19.44 (VS 2022 17.14) or any 19.5x (VS 2026). An older compiler stops the script with the
  install fix.
- **No prompt needed.** If `build\windows-msvc-release\` is missing or stale, it finds the newest Visual Studio
  in `[17.14, 19.0)` with `vswhere`, enters that Visual Studio's developer shell, and runs the configure, build
  and `ctest` steps above itself. The user can therefore start it from plain PowerShell.
- **Run.** It starts `build\go\helios-backend.exe run` and the servers in `build\windows-msvc-release\bin\`,
  runs scripted checks and opens the launcher for the interactive part.
- **Report.** It writes `milestone-M1.txt` for the user to paste back, with the toolset and version recorded.

The auditors' reference build is the floor toolset, the one that ships. A result on the primary counts as
evidence because both toolsets run identical tests in CI. For the IDE, run `cmake --preset windows-vs2026`
(or `windows-vs2022`) and open the solution under `build\windows-vs2026\` (or `build\windows-vs2022\`).

| Milestone | The user does | The user should see |
|---|---|---|
| M0 | Log in as `dev1`/`dev`; edit and undo the Kestrel record | Manifest verified; test scenes; "tick N, dilation 1.00"; clean undo |
| M1 | Launcher → Play; editor → F5 | BENCH-2 descent, no loading screen, ≥ 60 fps at 1080p Medium on REF-class hardware; PIE with 2 clients |
| M2 | Trade between two accounts; patch; install the SDK and create a project from `starter-blank` | Escrow settles; download ≈ changed bytes; signed installer; New Project → PIE with no engine checkout |
| M3–M4 | Scripted BENCH tours, free play; follow one starter-template tutorial | In-game budget overlay matches the nightly report; the tutorial works as written |

Linux uses the same flow with the `linux-gcc` preset (configure, build, `ctest`).

### 5.10 Plan changes: conformance rework and the conformance lint

The plan changes while code already exists. Draft v2 (the round-1 review fixes, CONSISTENCY §7 and §14.10)
replaced three draft-v1 designs with PG-anchored leadership and lease generations and with time-prefixed ID
blocks (ADR-004; 05 §1.4, §2.3):
- region leases in NATS KV with a 3 s TTL, whose holders fenced themselves;
- a `LEASES` bucket;
- Snowflake node-ID leases.

The `services/` backend had been written to draft v1, and its README described that design. Nothing in the
loop told anyone to change it. It happened again in round 3. The ADR-011 amendment (02 §1.1) replaced the
per-file AVX2 allowlist with whole-image ISA levels. On the same day, the in-tree ISA and CPU-gate code for
WP-0.2 and WP-0.5 was written to the retired allowlist (§5.10.4). That code was neither merged nor on a WP
branch, so D3 as then written did not cover it. The rules below, with D7, make every plan change reach the
code it affects, wherever that code lives.

#### 5.10.1 What is normative

- **Normative:** the ADRs (00, `docs/adr/`). In 01–08: criteria and thresholds; APIs and message shapes;
  wire and file formats; SQL DDL with schema and table names; ID layouts; protocols (leases, fences,
  handoff, publish); constants given in tables; and every "never", "only" and "must" rule.
- **Not normative:** rationale, research notes, estimates and comparisons.
- **Anchors.** Every normative item has an anchor: an ADR ID, a criterion ID, or `<section> §<n>` (for example
  `05 §1.4.2`).

#### 5.10.2 Rules for the Director

- **D1 Declare.** A PR that changes `docs/plan/**` or `docs/adr/**` carries `Plan-Change: <anchor>, …`, or
  `Plan-Change: none`. The adversarial reviewer checks the list against the diff. On merge the Integrator
  increments `docs/plan/PLAN-REV`. It also appends one line per anchor to `CONSISTENCY.md`, naming the rework
  WPs that the change opened. The counter started at **6** on 2026-09-25. Revisions 1–5 are drafts v1–v5,
  and each draft after v1 is one review round's fixes. Revision 6 is the round-5 minor revisions.
- **D2 Map.** `tools/conformance/map.jsonc` maps anchors to code, for example
  `"05 §1.4.2": {"paths": ["services/internal/orchestrator/**", "engine/net/cell/**"], "rules": ["CONF-02",
  "CONF-03"], "tests": ["conformance/holder_rule"]}`. DoD item 11 keeps it current: a WP that implements a
  normative anchor adds its paths. An anchor with no entry resolves to the modules of the WPs whose `Own`
  column cites that section (§2).
- **D3 Re-baseline in the same round.** For each changed anchor the Director resolves the map to three sets:
  - **Merged or in-tree code** under the paths gets a **conformance-rework WP** per module. Its ID is the
    original WP's ID plus `r` (for example WP-0.15r), and its branch is `conf/<id>`. The scope is the delta
    only. Deliverables: the code, expand/contract migrations (05 §3.3), and updated READMEs and comments.
    Acceptance: the anchor's CONF rules and conformance tests pass, and the original WP's criteria rerun
    green.
  - **In-flight WPs** whose branches touch those paths get the delta from the Director, recorded in the WP.
    The queue bounces any PR whose `Plan-Rev` trailer is lower than the revision of a change to an anchor
    mapped to its paths. The PR re-enters once the implementer has re-baselined and raised the trailer.
  - **Unstarted WPs** have their §2 rows edited in the plan PR itself.
- **D4 Priority.** Rework WPs rank directly after nightly P0 fixes. They take the module lock ahead of feature
  WPs on that module and must merge within two rounds. No feature WP on the module merges before its rework
  WP, so no new code builds on a superseded decision.
- **D5 Make it checkable.** A change that bans or requires a code construct adds a CONF rule (static) or
  names a conformance test (behavioural) in the same PR. When the reviewer accepts that a change cannot be
  checked mechanically (a process rule, for example), the map entry records why.
- **D6 Keep the status true.** The Director refreshes §8.1 and PLAN.md §11 at the start of every round. Once
  WP-0.3 lands, `tools/status/snapshot` generates them from the tree: modules present, test counts per
  toolchain, CI jobs, WPs in flight and open rework WPs. A status older than the round scores 0 on code
  health (§5.7). **Until then the check is mechanical anyway.** At every round audit the Director runs
  `cmake -P tools/status/check_status.cmake`, which fails in two cases:
  - a module directory under `engine/`, `apps/`, `tools/`, `services/cmd/`, `services/internal/` or
    `services/pkg/` is not named in §8.1;
  - a module README in those directories has no `Plan-Rev:` line (D7).

  A red check is a stale status. WP-0.2 registers the script with the build-independent lints, and WP-0.3's
  snapshot absorbs it.
- **D7 Working-tree code and review rounds.**
  - *Coverage.* Code written ahead of the round order (as `services/` was, and the ISA and gate code on
    2026-09-25) counts as in-tree code for D3 from the moment it exists, whether or not it is committed.
  - *Recording.* The module README of such code records `Plan-Rev: <n>`, the plan revision it was written
    to, so D3 can find it before it has a commit trailer. A module with an open conformance delta keeps the
    revision before the change that opened it, and its rework WP raises the number. Every in-tree module
    README carries the line from 2026-09-25 (§5.10.4 (c)).
  - *Review rounds.* The fixes of one plan review round are one plan change. Before they are accepted, the
    Director resolves every changed anchor against three places: `main`, the open WP branches and the working
    tree. It then adds the §5.10.4 rows, opens the rework WPs in §2, updates the WP rows that the delta
    touches, and refreshes §8.1 (D6).
  - *Implementers.* An implementer whose anchor changes while they are coding stops and takes the delta. They
    do not finish to the old text.
  - *Audit.* Until WP-0.2's lint exists, the round audit checks the working tree by hand against the map, as
    §5.10.4 does.

#### 5.10.3 The conformance lint (`tools/conformance`, WP-0.2)

- **Tool.** A Go tool: `go/analysis` passes for Go, ast-grep patterns for C++, and pattern rules for SQL, TOML,
  JSONC and workflow YAML. It writes SARIF.
- **When it runs.** In the PR tier, over the changed paths. At every round audit, over the full tree and over
  any working-tree code that awaits its WP (D7): the auditor lists the findings, and the Director opens a
  rework WP in the same round for any finding that has none.
- **Suppressions.** A `conformance:allow CONF-nn <reason>` comment on the line. The reviewer must approve it,
  and the round audit lists every suppression.
- **Fixtures.** Each rule ships with a seeded violation that must fail it.

| Rule | Anchor | Flags | Scope | Kind |
|---|---|---|---|---|
| CONF-01 | 05 §2.3, §1.4 | A JetStream KV bucket named `LEASES`, or one whose name matches `(?i)lease\|leader\|fence`, being created, bound or read. Tests that assert the bucket is absent are exempt | `services/**`, `engine/net/**`, `apps/**` | Static |
| CONF-02 | 05 §1.4.1–1.4.2 | Lease or leadership state kept in NATS: per-key TTL, `MaxAge` or `TTL` on such a bucket, or KV compare-and-set used for leadership | Same | Static |
| CONF-03 | 05 §1.4.2 (holder rule) | A holder that drops regions on a local timer. Required test `conformance/holder_rule`: with the control plane unreachable for 60 s, the Go `Agent` (now) and the C++ cell host (from WP-0.14) keep their regions and drop one only after seeing a higher `lease_gen` | `services/internal/orchestrator/**`, the cell host | Test |
| CONF-04 | ADR-004, 05 §1.4.5 | Node-ID minters: identifiers or config keys matching `(?i)\b(node\|worker\|machine\|datacenter)_?id\b` in ID code (files that compose, parse or allocate 64-bit IDs; graph, UI and K8s node names are out of scope); the Snowflake 41/10/12 layout (`<< 22` with `<< 12`) or the retired 41/5/8/9 layout; an ID composed outside `pkg/idgen` or the C++ `EntityRegistry` minter | `services/**`, `engine/ecs/**`, `engine/net/**` | Static |
| CONF-05 | 05 §1.4.5 (who mints) | `pkg/idgen` imported, or `AllocateIdBlocks` called, from outside the minter allow-list: cells, identity, character, ledger, market, industry, mail, world state, activity, lifecycle cleanup, plus the orchestrator that allocates blocks, the wiring in `internal/backend`, and tests. Session IDs stay random 63-bit values | `services/**` | Static (import graph) |
| CONF-06 | 05 §1.4, §3 | A PostgreSQL schema not named `svc_<service>`, or a table created outside its own service's schema | `services/migrations/**` | Static |
| CONF-07 | 05 §3, §6.6 (Phase 0 rule) | A direct-PII column (e-mail, date of birth, IP address, real name) that is not `*_ct` ciphertext or a `*_bidx` blind index, or that lives outside `svc_identity`; an `@pii` field outside Identity | `services/migrations/**`, `schemas/**` | Static |
| CONF-08 | 04 §2; reconciliation #12 | A default gateway port other than UDP 7777 | `services/**`, `engine/net/**`, `deploy/**` | Static |
| CONF-09 | ADR-014 | A `go.mod` `go` or `toolchain` directive other than 1.27.x; a CI `setup-go` step that does not read `services/go.mod` | `services/go.mod`, `.github/**` | Static |
| CONF-10 | 08 §1.16; reconciliation #19 | `SDL_CreateRenderer` outside the launcher and `engine/ui`'s SDL_Renderer backend. This absorbs WP-0.17's symbol lint | `**` | Static |
| CONF-11 | ADR-011 amendment; 02 §1.1 (whole-image ISA levels); reconciliation #25 | ISA flags granted below the image level. Flagged: a per-target or per-file AVX2 list (`HELIOS_ISA_AVX2_TARGETS`, `HELIOS_ISA_AVX2_SOURCE_PATTERNS`, or any list or regex that selects targets or sources for AVX-class flags); `helios_avx2_sources()`, or any `set_source_files_properties(… COMPILE_OPTIONS\|COMPILE_FLAGS …)` carrying `/arch:AVX*`, `-mavx*`, `-mbmi*`, `-mf16c`, `-mlzcnt`, `-mfma` or a `-march=` above `x86-64`; such flags in any library's `target_compile_options`, of any visibility, or in `CMAKE_<LANG>_FLAGS*`, outside `cmake/HeliosIsa.cmake`'s level sets. `isa_allowlist.cmake` may keep the self-dispatch symbol list and the gate's export and import lists (02 §1.1). The audit's own disassembly fixtures carry a `conformance:allow CONF-11` suppression | `CMakeLists.txt`, `**/CMakeLists.txt`, `cmake/**`, `tools/**/*.cmake`, `third_party/CMakeLists.txt` | Static |
| CONF-12 | 02 §1.1 (gate placement and gate-TU rules); reconciliation #25 | In the gate objects (`engine/core/src/cpugate/**`, `platform/*/cpu_gate_hook.c`): a Windows entry anywhere but `.CRT$XLA0` (for example `.CRT$XI*`, `.CRT$XC*` or `init_seg`); a missing `/INCLUDE:_tls_used` or `/INCLUDE:helios_cpu_gate_tls_entry`; a call to `ExitProcess` (the gate ends with `TerminateProcess`); an include other than `cpu_gate.h`, `<stdint.h>`, the CPUID intrinsic headers (`<intrin.h>`, `<cpuid.h>`) and the hooks' `<windows.h>`, `<signal.h>` and `<unistd.h>`; an external symbol other than `helios_cpu_gate_run`, `helios_cpu_gate_verdict` and `helios_cpu_gate_tls_entry`. Anywhere else in Helios code or a vendored-library patch: a `.CRT$XLA*` contribution; a `.preinit_array` entry; `__attribute__((constructor(n)))` or `init_priority` with n < 101; `ifunc` or `target_clones`. Audit check 3 is the link-level test of the same rule | `engine/**`, `gems/**`, `apps/**`, `game/**`, `third_party/CMakeLists.txt` and vendored patches | Static |

The ISA audit (02 §1.1, five checks) enforces the level assignment on built images. CONF-11 and CONF-12 catch
the retired per-file design and a misplaced gate in source, on the changed paths, before anything is built.
WP-0.2's layer and licence lints stay separate. They already enforce ADR-011's module rules and ADR-013.

#### 5.10.4 Applications on the repository of 2026-09-25: WP-0.15r, WP-0.2r and WP-0.5r

**(a) The backend, WP-0.15r.** The draft-v2 changes to ADR-004 and 05 §1.4 and §2.3, and draft v3's failure
domains (05 §1.4.3), arrived after the WP-0.15 code existed. Draft v4 (the round-3 fixes) touched 05 §1.4
again, but only with scope that `services/` has not built (`Drain` and `PreProvision` as planned migrations,
`region_migration`, world-script partitions as regions), so it adds no row below. Draft v5 (the round-4 fixes)
touched 05 §1.3 and §1.4 with Phase 3–4 scope only: `MintRelocation`, `DrainGateway`, `AssignReplicant` across
zones, and zone-instance affinity in the token's gateway choice. The in-tree `session.pickGateways` already
implements the free-slot order that affinity (WP-4.3) puts second, so round 4 adds no row either. Applying §5.10.2 by hand,
before the lint exists, gives the following. The **Reworked** rows are what the working
tree shows now, and the CONF rules must confirm them once WP-0.2 lands. The **Open** rows are WP-0.15r's
scope.

| Delta | Anchor | State in the working tree | Rework |
|---|---|---|---|
| Region leases in NATS KV with a 3 s TTL and holder self-fencing | 05 §1.4.2 | **Reworked.** Generations are allocated in PG (`zone.lease_gen`, `placement_log`). The 12 s liveness TTL is only the orchestrator's fallback. `orchestrator.Agent` implements the holder rule, and `service_test.go` asserts that no `LEASES` bucket exists | CONF-01…03 in CI |
| Orchestrator leader election in NATS KV | 05 §1.4.1 | **Reworked.** `orch_leader` with term-fenced writes: 10 s lease, 2 s renewal, acts only within 8 s of its last renewal | — |
| Snowflake node-ID leases | ADR-004; 05 §1.4.5 | **Reworked.** `id_alloc` blocks, `pkg/idgen.Minter` and the shared vectors `testdata/vectors/block_ids.json`. One stale `-- Snowflake` comment remains on `identity.account.account_id` | Fix the comment to cite 05 §1.4.5 |
| Schema names `identity` and `orchestrator` | 05 §1.4 (**Owns**), §3 | **Open** | Rename to `svc_identity` and `svc_orch` with an expand/contract migration before WP-1.13 adds tables (CONF-06) |
| Plain-text `email` and `email_norm` columns | 05 §3, §6.6 (Phase 0: per-account DEKs) | **Open** | `email_ct` and `email_bidx`, plus `subject_key` with a wrapped per-account DEK; login looks up the blind index (CONF-07) |
| `zone.lease_gen` instead of `region_lease` | 05 §1.4.2 | **Open.** Equivalent for v0 zones, which have one region | Add `region_lease` rows per `ZonePartitionDef` region and move the generation there before WP-1.13, whose fences carry `(region, lease_gen)` |
| `RegisterProcess` has no failure domain; heartbeats omit held regions | 05 §1.4 API | **Open** | Add `fd{az, rack, host}` and `server_build` to registration and `held[{region, lease_gen}]` to heartbeats. Phase 0 stores them; Phase 2's failure detection uses them |
| Identity mints account IDs, but 05 §1.4.5's minter list omitted Identity and Character | 05 §1.4.5 | **Plan fixed** (05 updated; CONSISTENCY §23) | — |

**(b) ISA levels and the CPU gate, WP-0.2r and WP-0.5r.** Round 3's ADR-011 amendment (02 §1.1) replaced the
per-file AVX2 allowlist with whole-image levels. On 2026-09-25 the working tree gained the following files,
all uncommitted and all written to the design the amendment replaced:
- `cmake/HeliosIsa.cmake` and `cmake/isa_allowlist.cmake`;
- `tools/lint/isa_audit.cmake` and `tools/ci/msvc_gate_audit.ps1`;
- `engine/core/src/cpugate/*` and `engine/core/src/platform/{win32,posix}/cpu_gate_hook.c`;
- the gate wiring in `engine/core/CMakeLists.txt` and `helios_executable`.

A headless GCC 13.3 build of that tree is warning-free. All 39 `lint` tests pass, including `lint_isa_audit`,
which shows only that the tree conforms to the retired model. Applying D7 by hand gives the following rows.
WP-0.2 and WP-0.5 have not merged, and their rows now exclude this code, so no feature WP can build on it
before the rework (D4).

| Delta | Anchor | State in the working tree | Rework |
|---|---|---|---|
| Per-target and per-file AVX2 allowlist | ADR-011 amendment; 02 §1.1 | **Open.** `isa_allowlist.cmake` sets `HELIOS_ISA_AVX2_TARGETS tp_jolt` and `HELIOS_ISA_AVX2_SOURCE_PATTERNS "_avx2\.(c\|cc\|cpp)$"`. `helios_avx2_sources()` compiles only named `*_avx2.cpp` kernels at AVX2, and every other TU builds at the compiler default | WP-0.2r: delete both lists and the function, and add image levels (CONF-11) |
| Level flag sets | 02 §1.1 table | **Conforming content, wrong shape.** `helios_isa_avx2_flags()` and `helios_isa_base_flags()` emit 02's flags, including `-mbmi2`, `-mno-fma` and `-ffp-contract=off`, but they are applied per file | WP-0.2r: reuse them as `HELIOS_ISA_AVX2` and `HELIOS_ISA_BASE` |
| `tp_jolt`'s PUBLIC ISA options | 02 §1.1 build rules | **Open.** `third_party/CMakeLists.txt` still adds `/arch:AVX2` or `-mavx2 …` as PUBLIC options, without `-mbmi2` and `-mno-fma` | WP-0.2r: remove them. The `JPH_USE_*` defines stay PUBLIC, and the image level supplies the flags (CONF-11) |
| Image levels, `<module>@base` builds and the `base`-links-`avx2` configure error | 02 §1.1 build rules | **Missing.** `helios_executable` has `ROLE` and `CPU_GATE\|NO_CPU_GATE` but no level. No `@base` library exists | WP-0.2r: the role picks the level; `@base` builds for the launcher's modules; a configure fixture |
| The five-check audit | 02 §1.1 checks 1–5 | **Partial, to the old model.** Check 1 validates flags against the allowlist, not against the image level. Check 2, the gate object (objdump in CMake, dumpbin in `msvc_gate_audit.ps1`), matches 02. Check 3 covers only ELF `.preinit_array` and `R_X86_64_IRELATIVE`. Checks 4 and 5 do not exist. Fixtures such as `lint_isa_fixture_unlisted_avx2` encode the allowlist | WP-0.2r: check 1 per image level; check 3 adds Windows TLS order, `.init_array` priorities and `.dynsym`; checks 4 and 5; fixtures rewritten |
| Windows gate placement | 02 §1.1 (`.CRT$XLA0`) | **Non-conforming.** `cpu_gate_hook.c` places the entry in `.CRT$XIB`. That is earlier than `.CRT$XCC`, where `init_seg(compiler)` would put it. But it ties with mimalloc's own `.CRT$XIB` initializer, so link order decides which runs first. It also runs after every TLS callback, including mimalloc's `.CRT$XLB` `mi_tls_attach` and the CRT's `.CRT$XLC` dynamic-TLS callback. Both are AVX2-built code that would run before the gate | WP-0.5r: `.CRT$XLA0` with `/INCLUDE:_tls_used`, run on `DLL_PROCESS_ATTACH` (CONF-12; audit check 3) |
| Windows failure path and gate-object rules | 02 §1.1 | **Partly conforming (re-checked in round 5).** Conforming now: `helios_cpu_gate_sources()` (`cmake/HeliosIsa.cmake`) builds the gate objects with `/GS-`, or `-fno-stack-protector`, and `-fno-sanitize=all`, and `isa_allowlist.cmake` admits no `__security_cookie` or `__stack_chk_*` import. Still open: the hook ends with `ExitProcess`, which would run mimalloc's `.CRT$XLY` detach hook once the gate sits in a TLS callback. It shows a message box whenever stderr is unusable, not by the PE subsystem, and has no `HELIOS_CPU_GATE_SILENT`. The exports are `helios_cpu_gate_run` and `helios_cpu_gate_crt_entry`, and there is no `helios_cpu_gate_verdict` or `platformInit` check | WP-0.5r: `TerminateProcess(…, 78)`; the subsystem test and the silent switch; the three exports; the verdict check (CONF-12; audit check 2) |
| Illegal-instruction backstop | 02 §1.1 (round 5) | **Partial.** Both hooks pass the deliberate traps `ud2`/`ud1`/`ud0` through (`hcg_is_deliberate_trap`: Windows `EXCEPTION_CONTINUE_SEARCH`; Linux returns after `SA_RESETHAND` restored `SIG_DFL`). Missing: the VEX/EVEX and POPCNT classifier, so today every other #UD still gets the CPU message and exit 78; the crash-handler handover (`posix_crash.cpp` restores the backstop instead of re-raising with `SIG_DFL`, and Windows has no first-in-chain vectored handler); check 5's `ud2` and EVEX fixtures before and after crash-handler install | WP-0.5r (row extended in §2.1) |
| Linux gate placement | 02 §1.1 | **Conforming.** A `.preinit_array` entry in `posix/cpu_gate_hook.c` | — (audit check 3 confirms it) |
| Where the gate is linked | 02 §1.1 | **Partial.** `helios_executable` links the hook object into each executable by role, and `NO_CPU_GATE` can opt an AVX2 role out. Only the two test children use it today. Modular dev builds need it in `helios_runtime` | WP-0.5r: no opt-out for `avx2` images; in `helios_runtime` for modular builds after WP-0.6c |
| Gate TU form | 02 §1.1 gate-TU rules | **Conforming in form.** The probe is C (`cpugate/cpu_gate.c` plus the two hooks), at x86-64-v1 flags. It has static functions and one `extern "C"` probe entry, calls only allowlisted functions, requires 08 §2.2's features plus BMI2, exits with 78 and resolves `MessageBoxW` at run time. `core_tests` covers 11 gate cases, including a recorded Sandy Bridge and pre-initializer order. Round 3's 02 §1.1 named `engine/core/src/cpu_gate.cpp` | Keep the C form, which meets the no-STL, no-exceptions and no-RTTI rules by construction. 02 §1.1 now names the C files (its round-4 fixes, CONSISTENCY §36) |

**(c) Code ahead of the round order, and the round-5 changes (D7).** Round 5's reviewers found six modules
written ahead of §8.2's order. `engine/authority` and `engine/hxl` existed before 09's round-4 edit.
`engine/gameplay`, `engine/server`, `apps/cellserver` and `apps/gateway` appeared after it, together with
`services/pkg/hxl` and the vendored `nats.c`. No module README recorded `Plan-Rev`, as D7 requires.

On 2026-09-25 the Director applied D7 by hand at plan revision 6, the round-5 minor revisions. Every
in-tree module README now carries its `Plan-Rev`. Each anchor that round 5 changed (CONSISTENCY §42) was
resolved against the working tree, and the CONF rules were checked by reading the code, because the lint does
not exist yet.

| Module | WP | Order | `Plan-Rev` | Round-5 anchors and CONF rules checked | Result |
|---|---|---|---|---|---|
| `engine/authority` | WP-0.14 (its fence and AG-table semantics feed WP-1.13) | Ahead | 6 | 05 §1.4.2 holder rule (CONF-03); 04 §6.1 fence rows; 04 §10.2 keyframe contents (Phase 2) | **Conforming.** `LeaseHolder` never fences on unreachability. It gives a region up only on `lease_lost`, a missing or re-generated assignment, a higher generation seen elsewhere, or a rejected region checkpoint, and generation floors stop it re-acquiring a generation it gave up. Tests: `authority.lease: the holder rule never fences on unreachability`, and `server.orch: holder rule …` (a 20 s partition, then orchestrator unavailability and bus loss). CONF-03's `conformance/holder_rule` asks for 60 s, so WP-0.14 lengthens the partition before merge |
| `engine/server`, `apps/cellserver`, `apps/gateway`, `third_party/nats.c` | WP-0.14 | Ahead | 6 | 04 §10.2's new replay rows and keyframes (v0 has no recorder; WP-2.4, WP-3.1); 05 §6.5's permission matrix, accounts and per-process credentials (Phases 2–3; v0 connects as the shared `fleet` user); CONF-01, 02, 04 and 08 | **Conforming.** There is no KV bucket, lease TTL or node-ID code. The gateway listens on UDP 7777 by default, and `OrchestratorClient` feeds `LeaseHolder` and counts failures as unreachability |
| `engine/hxl`, `services/pkg/hxl`, `engine/gameplay` | WP-0.19 | Ahead | 7 | None: round 5 changed neither 06 §1.1–1.2 nor 06 §4; revision 7 is WP-0.19's own `Plan-Change: 06 §1.2`, which the code implements | **No round-5 delta.** WP-0.19's review owns GP-1. The two fields that 06's illustrative snippets lacked, `AttributeDef.id` and `ModifierDef.priority`, are adopted into 06 §1.2 by WP-0.19's PR (`Plan-Change: 06 §1.2`). HXL and the slot constants need an identifier, which the localized `name` cannot be, and the aggregation's "highestPriority" needs a priority. Open tooling items against 06 §1.2 rules 2, 5 and 6 (hand-copied `det` constants pinned by a test; `hxlfloat` as a unit test, not a `go vet -vettool` pass, and no pre-commit hook; `CheckGOAMD64` not yet called by `helios-backend`) are listed with proposed owners in `services/pkg/hxl`'s README. The engine schemas sit in `schemas/gameplay/` (CLAUDE.md), not 02 §3.1's `engine/<module>/schema/`: raised with the Director |
| `engine/script` | WP-0.10 | In order | 5 | 02 §7.4 (R5-02.4) | **Open.** `create()` warns where 02 now requires `VmConfig` to refuse native codegen on cells and world-script hosts, and the `fuel-counter` and `codegen-fornloop-fuel` patches do not exist. Rework: WP-0.10r |
| `engine/ecs` | WP-0.8, WP-0.6a–b | In order | 6 | 02 §2.2 (R5-02.2): the heap lifetime rule and batched accounting | **Conforming.** `ecs::TaggedHeap` is where the rule came from: pooled heaps that are never freed, `destroyAll` only for thread-confined heaps, and `Accounting::Batched` with `flushAccounting()`. RT-01's structural clause is a failing criterion, not a conformance delta: ADR-004a, WP-1.1a |
| `engine/core` | WP-0.5, 0.5r | In order; gate code ahead ((b)) | 3 | 02 §1.1 backstop (R5-02.1); 02 §2.2 (R5-02.2) | **Open.** The backstop is the new row of (b). For accounting, the working tree's WP-0.5 change shards the `MemoryTag` counters and adds batched `trackAllocations`, but 02 §2.2's ≤ 3× `mi_malloc` target is not yet measured. Rework: WP-0.5r (backstop) and WP-0.5 (accounting) |
| `tools/lint` and the ISA files in `cmake/` | WP-0.2, 0.2r | Ahead ((b)) | 3 | None new in round 5 | **Open**, as in (b). Rework: WP-0.2r |
| `services/` (`cmd`, `internal`, `pkg`, `migrations`) | WP-0.15, 0.15r | Ahead (draft v1) | 1 | 05 §6.5 (Phase 2–3 scope); 05 §3.6's progression deltas and §1.4.6's replicant placement (not built) | **No new delta.** The open rows of (a) remain, re-checked on 2026-09-25: the `identity` and `orchestrator` schemas, plain `email_norm`, and no `region_lease`. Rework: WP-0.15r |
| `engine/math`, `engine/reflect`, `engine/net`, `engine/rhi`, `engine/render`, `tools/schemac`, `tools/shaderc`, `tools/rendertest` | WP-0.5, 0.7, 0.11–0.13 | In order | 6 | None of round 5's anchors map here. schemac's `nats-perms.json` emitter (05 §6.5) is new Phase 2 scope, not a delta | **No delta** |
| `engine/platform`, `apps/samples`, `tools/ci`, `tools/prebuilt`, `tools/vendor` | 0.1, 0.5, 0.11–0.12 | — | — | — | These are not modules with an API, so they have no README. `tools/status/check_status.cmake` requires them in §8.1 but asks for no `Plan-Rev` |

---

## 6. Testing strategy

| Layer | What | When | Gates |
|---|---|---|---|
| Unit | doctest per module; `go test` with injected clocks; `rapid` properties (conservation, uncrossed books) | PR, all toolchains | Every WP |
| Integration | `pkg/testkit` (embedded PG, NATS, miniredis); headless PIE; `helios-client --headless --rhi=null` flows | PR, nightly | ED-3, CL-20 |
| Golden images | Lavapipe at 320×180–640×360 every commit, rendered twice to catch races; Null traces on all toolchains; **real GPUs** (NVIDIA, AMD, Intel; Windows + Linux) nightly, goldens per driver | PR, nightly | RC-1, RC-11 |
| Network simulation | NetSim `lan`, `good`, `mobile`, `awful`; burst loss; reconnect storms | PR (good), nightly | NS-*, CL-5 |
| Bot swarms | 16 per PR → 50 nightly (Ph1) → 1k nightly (Ph2) → 10k weekly with chaos (Ph3) → 3–5× target CCU (Ph4) → 100k (Ph5) | Nightly, weekly | AAA-SRV-*, STB-5 |
| Soak | 8 h client; 24 h server; 72 h backend chaos; 72 h at 50k CCU (Ph4); RSS growth ≤ 2 % | Weekly | STB-4, RT-10 |
| Determinism | Physics, PCG + `hnoise` twin, HXL (C++ = Go, with FMA-sensitive vectors; Go on `windows/amd64`, `linux/amd64` at `GOAMD64=v1` and `v3`, and a native `linux/arm64` runner; the `hxlfloat` lint, 06 §1.2), `det::`, `Fixed64`, tick replay (including the Luau `det-math` corpus and `det::HitboxSampler`, 04 §10.2), ASM rollback fuzz, the `simdet` planted-violation self-test; MSVC, clang-cl, GCC, Clang, MinGW; 1/4/16 workers; AMD and Intel CPUs (the lab's SERVER and Intel boxes, nightly) | Hashes per PR; full and cross-vendor nightly | AAA-PLT-4, RT-03/04, 04 NS-3.8 |
| Performance | Micro-benchmarks per PR on a fixed runner; BENCH scenes nightly on the lab; per-pass GPU ms; ±5/±10 % gates | PR, nightly | REN-*, RT-12, 07 §4.1 |
| Fuzzing | libFuzzer on netcode/reliable reads, channels, generated decoders, JSONC/tagged/cooked/`.hpak` readers, manifests, hot-reload protocol; `go test -fuzz` on tokens, manifests, CDC | 1 h per target nightly; ≥ 24 CPU-h per release | AAA-SEC-7, CL-14 |
| Statistics | Chi-square and perk-pair independence at 10⁶ rolls per loot, plug and resource table | Nightly | GP-6 |
| Chaos | `kill -9` of cells, gateways, services mid-trade and mid-handoff; NATS loss; PG switchover; installer `FaultyFs`/`FaultyHttp` | Nightly, weekly | STB-5, CL-12, BE-A6 |
| Sanitizers, content | ASan/UBSan nightly; TSan on jobs, ECS, streaming; T28 rules with SARIF annotations | Nightly, PR | AAA-TOOL-6 |
| Editor UI (07 §4.4) | `helios-uitest`: every tool opened by injected input and layout-linted at 5 scales (PR); ꟻLIP goldens of every tool at 100/200 % in dark and high-contrast themes, dual-path scenario hashes, UI-level replays of ED-2, 7, 10, 16, 17 and 23…25 (nightly; Xvfb + lavapipe on Linux, `win-gpu` post-merge on Windows); multi-cell PIE seam scenario (nightly, headless) | PR, nightly | ED-14, ED-15, ED-23…25 |
| Source-control scale (07 §1.7.1) | 50 GB cut of the reference repository nightly; the 500 GB repository weekly on lab hardware | Nightly, weekly (W) | ED-18 |
| Collab scale and durability (07 §1.8.1, §1.8.2) | 10 editors for 30 min with one replica failover and one stream restore, plus a 30-min cross-session cut of 2 zone sessions and the data session (nightly); 25 editors at 50 tx/s for 4 h with replica and NATS-node kills and a restore, beside a second zone session and the data session editing overlapping records with checkouts and 0 cross-session publish conflicts (weekly, lab); the studio's quarterly restore drill from object storage and from the WIP ref | Nightly, weekly (W), quarterly | ED-20 |
| Editor extensions and derived data (07 §1.10, §4.1.1) | `sample-editor-ext` built from the SDK and exercised in editor, `helios-tool` and assetd, with UI lints; incremental nav, HLOD, impostor and probe rebuilds timed and compared byte for byte with clean rebuilds | Nightly | ED-3, ED-19, ED-21 |
| Client UI (08 §1.7.3) | One headless Luau flow per Foundation panel (happy path, reject paths, keyboard-only and gamepad-only passes) against a dev backend; RmlUi layout lints at 75–200 % with pseudo, CJK and RTL locales; the `ui-reskin-fixture` rerun with a binary-hash check | PR (changed panels, 100 %), nightly (all, both OSes) | CL-19, CL-23 |
| Product packaging (08 §2.10) | `product lint` vectors; stamp → sign → verify round trips on SDK binaries; ceremony simulation with share reconstruction and rotations; two branded fixture products installed, patched, crashed and uninstalled side by side with filesystem, HKCU and Secret Service diffs | Nightly (Windows and Linux runners) | CL-14, CL-24 |
| Security reviews | Threat model per phase; internal red team Ph3; focused external test before the Ph3 public beta; external pentest Ph4 | Phase exits | AAA-SEC-8 |
| Docs, templates, upgrades | Doc-coverage gate; executable tutorials; `template-proof-<id>` jobs; `upgrade-test` on every release candidate; launcher import audit and x11/wayland smoke (WP-0.17) | PR (coverage, import audit), nightly (tutorials, templates), release (upgrades) | AAA-TOOL-7/8/9, PLT-3 |
| CI policy | `check_runner_policy` on every workflow change (§5.4a); post-merge `merge-policy` on every push to `main` (§5.2a) | PR, post-merge | K33, K25 |
| ISA levels and the CPU gate (02 §1.1; WP-0.2r, WP-0.5r) | The five-check audit: per-image flags, the gate objects, the pre-gate path of every `avx2` image in both link flavours (TLS-callback order, attributed `.CRT$X*` entries, load order, `.preinit_array`, IFUNCs, `.dynsym`), post-link symbol mapping of `base` images, SDE and qemu emulator runs, each with seeded canaries; `isa-audit --objects` in `sdk-consumer` | PR (checks 1–4), nightly (check 5, `sdk-consumer`) | RT-09, CL-17 |
| Plan conformance (§5.10) | CONF-01…12 on changed paths, each with a seeded-violation fixture; conformance tests such as `conformance/holder_rule`; the full-tree lint, plus working-tree code awaiting its WP, at every round audit (D7); the `Plan-Rev` check in the queue | PR, round audit | K37; phase exits |
| Content publishes (§5.2a) | Per-commit T28, schema lock, name grep and provenance over `main..head`; on project-schema commits, schemac `--check-lock`, the native-construct and SEC-1 lint and the compat classification against `Helios-Compat`; head cook, bot smoke, touched-scene goldens, the ED-10 diff and `schema.dynamic-hot`; the WP-2.16e fixtures (a T08 schema publish merges; native, Foundation and project-file edits bounce) | Content tier | ED-10, ED-22, AAA-TOOL-6 |

**Lavapipe limits.** It rasterizes on the CPU: it proves correctness and synchronization at small sizes and
covers cap-masked fallbacks, but says nothing about performance or vendor drivers, so every H-class
criterion needs the real-GPU lab.

---

## 7. Risk register

L/I = likelihood/impact (H, M, L). Owners are roles; "User" is the human sponsor. When a trigger fires, the
row says **Fired** with the date and the disposition, and its Trigger column names the next trigger. Fired
so far: K2 and K39 (2026-09-25, §8.1).

| ID | Risk | Cat | L/I | Mitigation | Owner | Trigger |
|---|---|---|---|---|---|---|
| K1 | Effort exceeds capacity (§4) | Scope | H/H | Phase gates; seams first; cut Ph5, then polish, never MVP | Director | Velocity < 60 % of forecast |
| K2 | The flecs-based ECS misses RT-01 (the structural-ops clause), or fragments | Tech | **Fired 2026-09-25**; H/H until WP-1.1a re-measures | **What fired:** the WP-0.6b pre-bench (`engine/ecs/SPIKES.md` §3–4) passed every RT-01 clause except structural ops, 3.4–6.7 ms for 9k ops against ≤ 1.5 ms (> 2×). Raw flecs does the same ops in ≈ 0.9 ms, 40 % under budget. The cost is the Helios World wrapper: identity maps, the structural log, command fusion and `componentInfo` lookups. A custom ECS behind the API would therefore not help on its own, since it needs the same bookkeeping. **Disposition (ADR-004a, open):** option A, wrapper optimization in WP-1.1a, keeping flecs, with the budget still per sync point. A custom ECS only if the optimized wrapper still fails RT-01 on SERVER at the Phase 1 gate. flecs stays confined to `engine/ecs`, with no flecs type in a public header, so that fallback stays open | Runtime lead | Fired: pre-bench > 2× budget. Next: ADR-004a's indicator M1 above 2.5× raw flecs at the Ph1 midpoint (scope B); the RT-01 structural clause red on SERVER at the Ph1 gate with WP-1.1a merged (B decided) |
| K3 | mimalloc heaps break under job migration, or tag accounting does not scale | Tech | M/M | **Spike (a) done** (`engine/ecs/SPIKES.md` §1). mimalloc v3 serves one heap per tag group across workers. **Hazard found:** a heap that other threads used, destroyed and re-created at the same address receives their allocations (200/200 misrouted in the raw API, and one crash). `ecs::TaggedHeap`'s pooled, never-freed heaps avoid it (0/200), and 02 §2.2 makes that the rule for every `mi_heap_*` user. **Contention found:** exact tag accounting collapsed to 460–890 ns per pair at 4 threads, and core `alignedAlloc` ran ≈ 30× slower than mimalloc. The fix is sharded, batched accounting (02 §2.2) in WP-0.5. The header-based fallback stays | Runtime lead | Cross-thread free faults; a wrong-heap allocation in the recycling regression; tagged allocation > 3× `mi_malloc` at 4 or 16 threads (02 §2.2) |
| K4 | Cross-compiler determinism drift | Tech | M/H | `det::`, no FP contraction, 5-compiler hashes, fixed-point integrators; AMD/Intel cross-vendor replay nightly (§6, 04 NS-3.8) | Runtime lead | Any hash mismatch |
| K5 | GPU/CPU terrain mismatch on a vendor | Tech | M/H | Fixed-point `hnoise` twin; CPU-tile fallback; vendor lab | Render lead | RC-4 > 1 cm |
| K5b | `hnoise` throughput below budget: MIN GPU integer rate, or CPU VM > 0.5 ms per tile per core (03 §5.5a) | Perf | M/H | 32-bit-only twin; level-adaptive octaves; WP-0.9c spike; pre-decided F2 (≥ 16 tiles per frame, deeper prefetch) and F3 (fixed-point base octaves, float far tiles) | Render lead | Spike below green; MIN unconfirmed at the Ph1 midpoint; RC-13 red |
| K6 | Lavapipe limits (slow, unrepresentative) | QA | H/M | Small goldens, real-GPU nightly, pinned Mesa | Render lead | Suite > 10 min; HW-only regression escapes |
| K7 | No real GPU or server hardware | Org | H/H | User's PC as runner from Ph0; lab bought by mid-Ph1 from the costed BOM (§4.3.1–4.3.2); unmeasured = failing | User | F0 does not approve the Ph1 H1 ($6–12k) and H2 ($3–8k) envelopes; lab not installed by the Ph1 midpoint; RC-3/4 unmeasured at the Ph1 midpoint |
| K8 | Windows Vulkan driver defects | Tech | M/M | Caps workarounds, min-driver check, D3D12 seam/gate | Render lead | Any 03 §1.6 trigger |
| K9 | Shader/PSO stutter | Perf | M/H | Closed shading models, recorded PSO lists, RC-7 | Render lead | Any PSO frame > 50 ms |
| K10 | Upstream churn or stalls (Slang, flecs, RmlUi, SDL3, Luau) | Tech | M/M | Hash pins, quarterly bumps, confinement, glslang plan B | Runtime lead | Upstream blocker > 1 month |
| K11 | netcode lacks forward secrecy | Security | M/H | Secret-store key, daily rotation; X25519 re-key designed Ph3, built for the Ph4 review | Net lead | Key exposure; pentest finding |
| K12 | Single-threaded netcode limits gateways | Perf | M/M | Instance per worker/port; measure Ph2; replace L1 behind `Endpoint` | Net lead | < 256 slots per instance |
| K13 | Handoff dupes or lost AGs | Tech | M/H | `GhostRef`, effects-only mutation, fence, torture bots | Net lead | Non-zero conservation audit |
| K14 | Ledger throughput, hot rows | Perf | M/H | Unmaterialized system accounts, partitions, load tests | Backend lead | BE-A5 over budget |
| K15 | embedded-postgres fragile on Windows | Tech | M/M | Cached binaries, `pg-install --from`, cold-start CI | Backend lead | Weekly cold-start failures |
| K16 | Go 1.27.1 missing in agent containers (1.24.7 today) | Infra | L/L | `go.mod` `toolchain go1.27.1` downloads it through `GOTOOLCHAIN=auto` (verified 2026-09-25); cached toolchain for offline containers; CI reads `go.mod` (CONF-09) | Infra | `go build` fails in a container; the toolchain download is blocked |
| K17 | SWG terrain patents US 8,115,765 / 8,207,966 / 8,368,686 | Legal | L/H | Generic node DAG; counsel review before any Ph4 public release | User (counsel) | Ph3 exit; any public build |
| K18 | Improbable US 11,792,306 ("view replication over unreliable networks") vs the Phase 4 replicant tier and the Phase 5 gateway replication layer | Legal | M/H | Review before WP-4.3 starts, follow-up before WP-5.2; replicants keep state for recovery only and serve no client views; design-around: a per-zone hot-standby cell fed by the same stream (ADR-007); v1 (Phase 3) does not need it | User (counsel) | WP-4.3 planning (Ph3); WP-5.2 planning |
| K19 | Pending sign-offs: FreeType FTL, Inno Setup, EOS/EAC, Sentry FSL, SIL OFL | Legal | M/M | Scanner; isolated plugins; GlitchTip fallback; sign off before vendoring | User (counsel) | WP-1.6, 2.6, 4.8, 4.9 start |
| K20 | HSM code-signing certificate slow (CA/B HSM rules; eligibility unverified) | Legal | H/M | Procurement starts Ph1 (WP-1.23); unsigned dev builds; cloud-HSM OV fallback | User | Not issued 3 months before Ph2 exit |
| K21 | GPL/AGPL contamination (leaked SWG, Core3, SWG:ANH, Blender add-on, libgit2) | Legal | L/H | Scanner, provenance, review checklist | Director | Any hit |
| K22 | Third-party IP in content | Legal | L/H | Name grep, provenance, art review | Content lead | Grep hit |
| K23 | Art, VO, content bottleneck | Scope | H/H | Procedural/kitbash content, CC0 with provenance, contract artists; Lean content tier (§4.3.3); mocap and facial fallbacks (K36) | User | < 70 % of assets 2 months before exit; H5 spend > 110 % of the approved phase envelope (§4.3, e.g. Ph2 $90–670k) at < 90 % of assets; H5 envelope not approved at a funding gate |
| K24 | Agent quality drift, test gaming, architectural erosion | Process | H/H | Adversarial review, fail-without-change tests, mutation-sampled audits, layering lints | Director | Rejection > 40 %; > 2 escaped defects per round |
| K25 | Parallel-agent integration conflicts | Process | M/M | Module locks, interface WPs, merge queue | Director | > 2 rebase failures per round |
| K26 | CI capacity and wall time | Infra | M/M | ccache/sccache, `/Z7`, split jobs, self-hosted runners | Infra | PR tier > 45 min p50 |
| K27 | BENCH budgets missed at Ph4 | Perf | M/H | Budgets from Ph1, per-pass tracking, visibility buffer | Render lead | BENCH > 10 % over for 2 weeks |
| K28 | 2,000-ship fan-out beyond TiDi | Perf | M/H | Command replication, aggregation, 1k-ship bots from Ph2 | Net lead | Nightly NS-4.2 half-load run (1,000 ships, from Ph2): tick p99 > 2.25 s at `d` = 0.1 (half the 4.5 s ceiling), or tick cost grows > 2.5× from 500 to 1,000 ships (01 §3.4). BENCH-3 is the client gate and never triggers K28 |
| K29 | Editor widget cost; iteration regressions | Scope | H/M | Widget library first, one owner, 25 % reserve; nightly editor perf | Tools lead | MVP slips a round; ITR red 3 nights |
| K30 | Hosting, egress and load-test cost | Org | M/M | §4.3.2 per-run model; in-region bots (no soak egress); spot capacity; colo decision Ph3; rerun 05 §6.4 per gate | User | A soak or stress run > 125 % of its §4.3.2 estimate (72 h soak > $15k); H2 + C2 monthly spend > 110 % of the approved envelope for 2 months |
| K31 | User validation bandwidth (one person) | Org | H/M | One script per milestone; milestones only per phase and mid-phase | Director | Milestone unvalidated > 2 weeks |
| K32 | Privacy law (GDPR, CCPA, COPPA) and data residency vs the append-only ledger and audit chain | Legal | M/H | PII only in Identity; crypto-shredding; pseudonymous ledger and audit; EU residency; counsel review of SCC/DPF transfers, age gating and ToS (05 §6.6) | User (counsel) | Before the Phase 3 public beta; BE-A18 red |
| K33 | The self-hosted `win-gpu` runner on the user's PC executes agent-authored code, exposing the machine and the user's credentials | Security | M/H | §5.4a: post-merge triggers only, no secrets, dedicated standard account or GPU-P VM, a workspace wiped per job, no inbound ports and LAN-blocking outbound rules; policy check in the PR tier | Infra (User registers) | Policy check fails or is bypassed; any runner job runs as an administrator or reads the user's profile; a dependency advisory for code that ran on the runner |
| K34 | The sponsor cannot fund the H1–H8 and loop envelope (§4.3) | Org | M/H | Funding gates F0–F4 with Lean and Engine-only options; fund H1/H2 first (3–5 % of the total); re-forecast C1 from measured $/WP at F0 | User | A gate decides Engine-only or stop; spend on any row > 110 % of its envelope for 2 consecutive months; C1 measured at F0 > 1.5× the planning value |
| K35 | Engine API churn breaks studio projects, so the north star (01 §1.1) fails in practice | Product | M/H | Public-API definition, semver and a two-release deprecation window (§2.7.2); `upgrade-project` migrations required by DoD item 9; `upgrade-test` on every release candidate | Tools lead | An upgrade test needs a manual edit; a public symbol removed without a deprecation release; TOOL-8 red on a release candidate |
| K36 | Mocap or facial data unavailable, unlicensable, or captured without adequate performer consent (biometric data) | Legal/Scope | M/M | Work-for-hire capture with releases reviewed by counsel; CC0-or-signed-off datasets only; raw facial video deleted after solving; procedural locomotion and audio-driven viseme fallbacks (§4.3.3) | Content lead, User (counsel) | No capture source contracted by the Ph3 midpoint (fallback ADR activates); any clip without provenance or release; a dataset licence outside the allow-list |
| K37 | Plan–code drift: merged, in-flight or working-tree code keeps a superseded decision (as `services/` kept draft v1's NATS leases and node IDs, and the in-tree ISA code kept the per-file AVX2 allowlist), and later WPs build on it | Process | H/M | §5.10: declared plan changes, the anchor map, rework WPs in the same round, `Plan-Rev` in the queue and in module READMEs (D7), review rounds resolved against the working tree (D7), CONF lint in the PR tier and the round audit, phase exits blocked by findings; §8.1 refreshed every round and checked against the tree by `tools/status/check_status.cmake` (D6) | Director | A full-tree CONF finding with no rework WP after one round; a rework WP open > 2 rounds; a merged PR whose `Plan-Rev` is stale for its paths; working-tree code whose README `Plan-Rev` predates a change mapped to its paths with no §5.10.4 row; §8.1 older than the round |
| K38 | The dynamic project-type path (02 §3.8) misses its per-type cost targets or slips past the Phase 2 exit. RT-21, ED-22 and the no-C++ templates (TOOL-9) then fail, or studios fall back to native packages and a C++ toolchain | Tech/Scope | M/H | As 02 §8.4: byte-identical formats in both modes, so a hot package moves to `schemas.native` with one line and no data migration; `schema.dynamic-hot` budgets in CI; the dual-mode corpus in the PR tier. Plus the split of §2.3a: the runtime chain (2.16c1–c3) starts on Phase 0–1 dependencies and measures its targets from c1, with records and `ScriptState` first and view-models last; the Director moves Core/Runtime staff onto the lane when slack falls. No threshold is relaxed, and a template never goes native (§2.7.4) | Runtime lead (2.16c), Tools lead (2.16e) | Any RT-21 per-type target missed on REF for 2 consecutive nightlies after its sub-WP merges: copy or compare > 2×, tagged codec > 2×, JSONC > 1.5× generated code; Luau dynamic field access > 40 ns p50; schemac > 1 s per 2,000 types or bundle load > 50 ms; a data-only edit > 5 s p95; a *Cinder Reach* zone's dynamic replicated components > 1 ms of stage 6. Or any DX schema lane row's slack (§3.1) below 3 months |
| K39 | Luau fuel metering misses RT-13: native codegen counts different fuel from the interpreter, and the metering hook costs more than its budget | Tech | **Fired 2026-09-25**; H/M | **What fired:** both clauses, measured in WP-0.10 on Luau 0.739. Codegen charges +1 fuel per numeric `for` left by `break` or `return`, because the loop interrupt sits at the top of the body instead of in `FORNLOOP`. That breaks the fuel identity 04 §10.2's replay needs. The `interrupt` hook costs ≈ 12–15 % against ≤ 10 %. **Mitigations (WP-0.10r):** the vendored `third_party/luau/patches/codegen-fornloop-fuel` (the interrupt in `FORNLOOP`, `CodeGen/src/IrTranslation.cpp`), with cells and world-script hosts interpreter-only until it lands (`VmConfig` refuses codegen there; today `create()` warns), and the vendored `fuel-counter` inline-counter VM patch. Both are in `sim_abi.script` and are rebased at every Luau bump (K10) | Runtime lead | Fired: RT-13's fuel-identity and overhead clauses red. Next: either patch unmerged at the Phase 0 exit; overhead > 10 % with `fuel-counter`; a Luau bump the patches do not rebase onto within the quarterly window |

---

## 8. Current status and next steps

### 8.1 Phase 0 status (repository on 2026-09-25)

The Director refreshes this table at the start of every round (§5.10.2 D6). `tools/status/check_status.cmake`
fails the round audit when a module in the tree is missing from it, and from WP-0.3 on,
`tools/status/snapshot` generates it. This refresh, in round 5, uses the facts the lead verified on
2026-09-25. Rows marked *re-checked* were confirmed by reading the tree the same day.

Terms:
- **Committed:** in the repository, with the GCC and Clang tests passing, the MinGW cross-builds linking and
  MSVC building in CI. The merge queue and the `main` ruleset do not exist yet (WP-0.1), so "committed"
  stands in for "merged" until they do.
- **In progress:** in the working tree and not merged. D7 covers it (§5.10.4 (c)).

| Item | Status | Evidence / gap |
|---|---|---|
| Plan: ADR 00 (with ADR-004a in `docs/adr/`), sections 01–09, `docs/PLAN.md` | **Approved** in review round 5 (backend 9.2, engine 9.0, tools 9.0: approve with minor revisions). The round-5 minor revisions are applied (plan revision 6) | The five rounds' scores are in PLAN.md §13, and the round-5 revisions in `CONSISTENCY.md` §42. ADR-004a is open |
| WP-0.1 CI matrix | **Partial** (committed) | **PR:** Windows MSVC primary (`windows-latest`, VS 2026, MSVC 14.51), `windows-msvc-floor` (`windows-2022`, MSVC 14.44), Windows clang-cl, Linux GCC, Linux Clang (lavapipe GPU tests), Linux headless, the MinGW cross-build, Go on Windows and Linux, and Go integration (embedded PostgreSQL, non-root). **Nightly:** ASan and the VS 2026 and VS 2022 MSBuild builds. **SARIF:** the Linux headless lint run publishes an artifact and, for same-repo PRs and `main`, code scanning results. **Missing:** scorecard and nightly perf (WP-0.3), plus the libFuzzer nightly (NS-0.4). The modular `windows-msvc-dev` job waits for WP-0.6c. *Re-checked:* `tools/ci` has no merge-queue script or `merge-policy` check (§5.2a), and `CMakePresets.json` now declares `cmakeMinimumRequired` 3.28, matching the top-level minimum |
| WP-0.3 nightly and scorecard (`scorecard.jsonc`, `tools/scorecard`, `tools/status`) | **In progress** (PR 1 of 3) | `scorecard.jsonc` registers the 18 Phase 0 criteria (5 measured, 10 partial, 3 unmeasured; every gap names its owner) and 14 exit items; each threshold quotes the plan. CTest `lint_scorecard` checks it against the plan's criterion tables and 09 §2.1's exit, `ci.yml` and each native build's CTest and doctest inventory. `tools/status/snapshot.py` absorbs `check_status.cmake`, which stays as a wrapper, and generates the tree inventory row. **Next:** the nightly report from JUnit, doctest XML and `go test -json`, the perf history and its ±5/±10 % comparator, and NS-0.4's 1 h libFuzzer run per target (PR 2); `tools/milestone/validate.ps1` (PR 3); then PLAN.md §11 and the §5.8 ratchet state, which D6 also names, generated by `snapshot.py` |
| `engine/core`, `engine/math` | **Done for the Phase 0 scope** (committed), except WP-0.5's additions (**in progress**) | `core_tests` 141, `math_tests` 104 |
| WP-0.2 and WP-0.5, with 0.2r and 0.5r: layering and licence lints, ISA levels, the CPU gate, core and math completion (`tools/lint`, `cmake/`, `engine/core`, `engine/platform`) | **In progress** (not merged) | *Re-checked:* the ISA and gate code still has the per-file allowlist and the `.CRT$XIB` entry. The gate objects now build with `/GS-` or `-fno-stack-protector`, and both hooks pass deliberate traps through. Row by row in §5.10.4 (b). The conformance lint (`tools/conformance`) does not exist yet |
| WP-0.7 `helios-schemac` and reflection (`tools/schemac`, `engine/reflect`) | **Done** (committed; 121 tests) | The grammar, including `scriptlib` and `fn`; the lock file; and the C++, Go (byte-identical) and JSON emitters. The `luau`, `repl`, `sql`, `proto`, `editor`, `records` and `docs` emitters are stubs that report "not yet implemented", and so, *re-checked*, is `lint`. The Phase 0 ones are WP-0.7b, because 01 §5.4 needs Luau and SQL output. The rest go with the WPs that consume them |
| WP-0.6 spikes | (a) and (b) **done** (`engine/ecs/SPIKES.md`); (c), the link model, **not started** | (a) mimalloc v3 heaps: pooled heaps avoid the recycling hazard. Exact tag accounting and core `alignedAlloc` collapse at 4 threads, so 02 §2.2 now requires sharded, batched accounting (K3; WP-0.5). (b) The DontFragment defaults are implemented, and the `InFrame` choice moves to ADR-004a. (c) gates RT-18, the modular `windows-msvc-dev` job and WP-1.4 |
| WP-0.8 ECS, records, assets v0 (`engine/ecs`) | **Partial.** The ECS on flecs is **done** (committed; 90 tests). Records (`.hrdb`), `.meta`, the DDC and `.hpak` v0 are **not started** | **The RT-01 pre-bench fails one clause, and K2 has fired.** Every criterion passes except 9k structural ops, at 3.4–6.7 ms against ≤ 1.5 ms. Raw flecs does the same ops in ≈ 0.9 ms, so the overhead is the Helios World wrapper: identity maps, the structural log, command fusion and `componentInfo` lookups. **ADR-004a is open.** The wrapper is optimized first (WP-1.1a) and flecs is kept; a custom ECS comes only if the optimized wrapper still fails RT-01 at the Phase 1 gate |
| WP-0.9 physics and PCG base (`engine/physics`, `engine/pcg`, `shaders/pcg`, `tests/corpus/hnoise`) | **Done for the Phase 0 scope** (committed), **except the `stable-order` Jolt patch** | One grid, stable body keys in `mUserData`, `CharacterVirtual`, and the fixed-point `hnoise` with scalar, SSE4.2 and AVX2 kernels plus the 32-bit Slang twin, all bit-identical over `tests/corpus/hnoise` (the twin on lavapipe). RT-03 (Ph0): the 600-step golden holds on GCC and Clang and is checked on every PR job. **Open:** `third_party/jolt/patches/stable-order` is not vendored, so the permuted-BodyID variant still diverges for multi-contact islands (a `KNOWN DIVERGENCE` test pins it). `helios_physics` is an interim entry in `HELIOS_ISA_AVX2_TARGETS` until WP-0.2r. **WP-0.9c:** red (F3) on the CPU clause (AVX2 ≈ 3.2 ms per tile against 0.5 ms), GPU clause measured on lavapipe only; MIN confirmation due before WP-1.8 (`docs/adr/ADR-0.9c-hnoise-throughput.md`) |
| WP-0.10 Luau host v0 (`engine/script`) | **Done** (committed; 79 tests), **except two RT-13 clauses**. K39 has fired | **Fuel identity:** Luau 0.739's native codegen charges +1 fuel per numeric `for` exited by `break` or `return`, so it needs a vendored CodeGen patch (`codegen-fornloop-fuel`). `create()` warns when codegen is enabled on a cell, where 02 §7.4 now requires it to refuse. **Interrupt overhead:** ≈ 12–15 % against ≤ 10 %, which needs the inline-counter VM patch (`fuel-counter`). Both are in WP-0.10r. The DAP adapter is WP-1.6 |
| WP-0.11 RHI (`engine/rhi`, `apps/samples`, `tools/prebuilt`) | **Done** (committed; 51 tests) | Vulkan and Null backends, lavapipe goldens, the SDL3 swapchain and the `rhi_triangle` sample. Slang v2026.18.2 is fetched with a pinned SHA-256 on Linux and Windows |
| WP-0.12 render graph, `helios-shaderc`, `helios-rendertest` (`engine/render`, `tools/shaderc`, `tools/rendertest`) | **In progress** (not merged) | RC-1 closes with it |
| WP-0.13 HTP transport (`engine/net`) | **Done** (committed; 84 tests), except NS-0.4's nightly run | NS-0.1 and NS-0.2 pass: raw ≈ 480k datagrams/s per core, and the full encrypted stack 117k packets/s per core. NS-0.7 passes: the 600 s gate, at 190 Mbit/s of payload with 0 drops. **NS-0.4 is partial:** sanitizer fuzzing ran, but the 1 h libFuzzer run per target needs clang's compiler-rt and is owed in the nightly (WP-0.3) |
| WP-0.14 cell and gateway (`engine/server`, `engine/authority`, `apps/cellserver`, `apps/gateway`, `third_party/nats.c`) | **In progress** (not merged) | NS-0.3 and NS-0.6 close with it. The code was written ahead of the round order. The D7 check in §5.10.4 (c) found it conforming: `LeaseHolder` follows the holder rule |
| WP-0.15 Go backend skeleton (`services/`: `cmd/helios-backend`; `internal/app`, `backend`, `identity`, `integration`, `orchestrator`, `platform`, `session`, `stack`; `pkg/authn`, `clock`, `connecttoken`, `idgen`, `keyring`, `ratelimit`, `rpc`, `testkit`) | **Done** (committed; 124 tests; Go 1.27.1) | `helios-backend` runs on embedded PostgreSQL 18, NATS and miniredis. It has identity, netcode connect tokens byte-exact with the C implementation, and the orchestrator with PG leadership and ID blocks. *Re-checked:* WP-0.15r's rows in §5.10.4 (a) are still open: the `identity` and `orchestrator` schema names, plain `email_norm`, and no `region_lease`. WP-0.15 was committed ahead of its rework, against its own row. D4 therefore holds every further backend WP (0.16, 1.13, 1.14) until WP-0.15r merges |
| WP-0.19 gameplay kernel (`engine/gameplay`, `engine/hxl`, `services/pkg/hxl`) | **In progress** (PR open, not merged) | Tags, attributes and modifiers, `ItemDef`, `ReasonCodeDef`, and HXL in C++ and Go with the shared corpus (`tests/corpus/hxl`: 10 files, 1,619 cases, 1,050 FMA-sensitive rows). `hxl_tests` 27 plus `hxl_tests_stack` (the stack budget under `ulimit -s 256`); `gameplay_tests` 44 (3 `perf:`); `lint_gamedef_go_current` keeps the committed Go records current; `go test ./pkg/hxl/...` green, and it also runs the corpus at `GOAMD64=v3` (`TestCorpusAtGOAMD64v3`). The corpus is bit-identical on GCC, Clang and Go at `GOAMD64=v1` and `v3`; MSVC, clang-cl and `windows/amd64` run in CI. **GP-1 gaps:** no `linux/arm64` corpus run (needs an arm64 runner, WP-0.1), and the 8-worker budget (10k × 40 attributes at 5 % dirty ≤ 1 ms) is not measured, because no runner has 8 cores (≈ 1.35–2.2 ms on 4 threads here; its CTest reports Skipped). D7 found no round-5 delta (§5.10.4 (c)) |
| Vendored dependencies (`third_party/`, `MANIFEST.md`) | 25 committed; `nats.c` v3.14.0 arrives with WP-0.14 | RmlUi, the text stack, sentry and libopus are due by phase. *Re-checked:* `third_party/CMakeLists.txt` still builds SDL3 with `SDL_RENDER OFF` and `SDL_WAYLAND OFF`, which the launcher cannot use (reconciliation #19; WP-0.17) |
| Not started | — | WP-0.4 (the user's `win-gpu` runner, a human action); WP-0.6c; WP-0.16 (patching); WP-0.17 (launcher, client); WP-0.18 (ToolsFramework, editor); WP-0.20 (*Cinder Reach* `content/`). Also WP-0.7b and the rest of WP-0.8 |
| Fired risks and failing gates | **K2** (RT-01 structural ops; ADR-004a, WP-1.1a), **K39** (RT-13 fuel identity and overhead; WP-0.10r) and **K5b** (armed by WP-0.9c's red outcome; `pcg_tests_perf` fails by design) | §7. K3's hazard was found and mitigated in spike (a) |
| Plan conformance (§5.10) | Rules defined; the lint not started; checked by hand under D7 | §5.10.4 has three parts: (a) the backend, (b) ISA and the gate, and (c) the modules ahead of the round order and round 5's changes. Open rework WPs: 0.15r, 0.2r, 0.5r and 0.10r. Every in-tree module README records its `Plan-Rev` (D7), and `tools/status/check_status.cmake` checks this table against the tree (D6) |
| Tree inventory (D6 check) | Every module directory, with its WP | `engine/authority` (0.14), `engine/core` (0.5), `engine/ecs` (0.8), `engine/gameplay` (0.19), `engine/hxl` (0.19), `engine/math` (0.5), `engine/net` (0.13), `engine/pcg` (0.9), `engine/physics` (0.9), `engine/platform` (0.5), `engine/reflect` (0.7), `engine/render` (0.12), `engine/rhi` (0.11), `engine/script` (0.10), `engine/server` (0.14); `apps/cellserver` (0.14), `apps/gateway` (0.14), `apps/samples` (0.11); `services/cmd/helios-backend`, `services/internal/app`, `services/internal/backend`, `services/internal/identity`, `services/internal/integration`, `services/internal/orchestrator`, `services/internal/platform`, `services/internal/session`, `services/internal/stack`, `services/pkg/authn`, `services/pkg/clock`, `services/pkg/connecttoken` (all 0.15), `services/pkg/hxl` (0.19), `services/pkg/idgen`, `services/pkg/keyring`, `services/pkg/ratelimit`, `services/pkg/rpc`, `services/pkg/testkit` (all 0.15); `tools/ci` (0.1), `tools/lint` (0.2), `tools/prebuilt` (0.11–0.12), `tools/rendertest` (0.12), `tools/schemac` (0.7), `tools/scorecard` (0.3), `tools/shaderc` (0.12), `tools/status` (D6; 0.3), `tools/vendor` (0.1) |
| Docs pipeline, SDK, templates | Not started | The DX track, from WP-1.24 (§2.7) |
| Funding | No envelope approved | F0 at the Phase 0 exit (§4.3.6). The WP-0.4 purchase list prices H1 and H2 before then |

### 8.2 Next 12 work packages, in execution order

**Finish first (in progress).** WP-0.14, WP-0.19 and WP-0.12, then WP-0.2 and WP-0.5 without the ISA and gate
code, each through §5.2's review as it completes. Their D7 rows are in §5.10.4 (b) and (c).

| # | Round | WP | Why now |
|---|---|---|---|
| 1 | 1 | **WP-0.15r** Conformance rework of `services/` | Rework ranks right after P0 fixes (§5.10.2 D4). WP-0.15 was committed ahead of it, so no further backend WP (0.16, 1.13, 1.14) merges until it lands |
| 2 | 1 | **WP-0.10r** Luau fuel patches (`codegen-fornloop-fuel`, `fuel-counter`) | K39 has fired, and RT-13 gates the Phase 0 exit. Round 5 made the codegen refusal normative, so this is D3 rework as well |
| 3 | 1 | **WP-1.1a** ADR-004a option A: the wrapper's structural ops | K2 has fired. The ECS is on the critical path (schema → ECS → world model). Starting now, on the committed ECS, gives the Phase 1 gate a measured decision rather than a surprise (reconciliation #5) |
| 4 | 1 | **WP-0.1** (rest): the merge-queue script and `merge-policy`, the `main` ruleset | AAA-PLT-1. The queue turns "committed" into "merged" (§5.2a) |
| 5 | 2 | **WP-0.2r** ISA-level rework | A rework WP (D4). WP-0.9's `avx2` kernel and WP-0.17's `base` launcher need image levels |
| 6 | 2 | **WP-0.3** Nightly, scorecard, `validate.ps1`, `tools/status/snapshot` | Phase exits cannot be measured without it. It also owes NS-0.4's 1 h libFuzzer run per target (clang's compiler-rt on the nightly image), and it absorbs `check_status.cmake` (D6) |
| 7 | 2 | **WP-0.7b** schemac Phase 0 emitters | 01 §5.4's Phase 0 clause needs Luau and SQL output, and `repl` feeds WP-1.10 |
| 8 | 2 | **WP-0.8** (rest): `.hrdb`, `.meta`, local DDC, `.hpak` v0 | On the critical path to WP-1.1 and WP-1.4 |
| 9 | 2 | **WP-0.6c** Link-model spike | RT-18 gates the Phase 0 exit and RT-14. It also unblocks the modular `windows-msvc-dev` job and decides where the gate lives in modular builds |
| 10 | 3 | **WP-0.5r** CPU-gate rework | Needs 0.2r's levels and audit. It moves the Windows gate to the first TLS-callback slot, and adds round 5's backstop classifier and crash-handler handover, before any `avx2` executable ships |
| 11 | 3 | **WP-0.9** Physics and PCG base | The head of the physics and PCG lanes. RT-03's Phase 0 scope, with stable body keys and the `stable-order` patch from the first body. WP-0.9c's `hnoise` spike arms K5b. Needs 0.2r |
| 12 | 3 | **WP-0.16** Patch pipeline v0 | After WP-0.15r (D4). It unblocks WP-0.17's launcher, the longest remaining chain to M0 |

WP-0.17, WP-0.18 and WP-0.20 follow and complete M0.

**In parallel, from the user:**
- register the Windows PC as the `win-gpu` runner under a dedicated standard account, following §5.4a (WP-0.4);
- install any Go ≥ 1.21, which downloads the pinned 1.27.1 on first use in `services/` (`go.mod` `toolchain` line);
- read §4.3 and say which tier (Lean or Full) to price in detail for the F0 decision at the Phase 0 exit.

---

## 9. Traceability

| Requirement | Where |
|---|---|
| 01 §3 scorecard as terminator, §3.2 BENCH lab, §5.2 patents, §5.4 gates | §1, §2, §5.6–5.7, K17–K18 |
| Cross-section asks: 02 §8.6, 03 §9.6, 04 re-key, 05 §13, 07 §5.5, 08 §4.8 | §3.2; WP-0.2, 0.4, 0.6, 1.1, 1.23, 2.15; K11, K19–K20, K29 |
| R01-P2-22, R05-P1-20, R06-ENG-17/27 (bot swarms, all-target CI) | §5.6, §6 |
| 01 §1.1 north star: studios ship without modifying engine or backend source | §2.7 (docs, SDK, upgrades, starter templates); WP-1.24, 2.16, 3.12, 4.12; AAA-TOOL-7/8/9; K35 |
| The user runs the loop "until AAA": what that costs a single sponsor | §4.3 (H1–H8, C1, C2 by phase), funding gates F0–F4; K7, K23, K30, K34 |
| The user's preference for Windows with Linux parity (launcher UI, Wayland/X11) | WP-0.17 SDL3 renderer and Wayland fix, import audit; reconciliation #19 |
| Safety of the user's own PC as a CI runner | §5.4a; WP-0.4; K33 |
| Motion matching and FACS data (02 §7.2, R04) | H5, §4.3.3; WP-3.13, WP-4.6; K36 |
| One queue for engine WPs and collab content publishes (07 §1.8: per-commit authorship, ED-10) | §5.2a merge policy by branch class, content tier, `merge-policy` check; WP-0.1; reconciliation #22 |
| Plan changes reach code that already exists (ADR-004, 05 §1.4/§2.3 vs the in-tree backend; the ADR-011 amendment vs the in-tree ISA and gate code) | §5.10 (D1–D7, CONF-01…12, WP-0.15r, WP-0.2r, WP-0.5r); DoD item 11; exit rules; K37; reconciliations #23 and #25 |
| Round-5 status facts: RT-01's structural-ops pre-bench failure and the Luau RT-13 findings (fired triggers, D6) | §8.1; K2 and ADR-004a (`docs/adr/`), WP-1.1a; K39, WP-0.10r; K3 and 02 §2.2 (WP-0.5); §5.10.4 (c) and `Plan-Rev` in module READMEs (D7); `tools/status/check_status.cmake` (D6) |
| Governed project-schema edits reach `main` (07 T08, §1.8.2; ED-22, TOOL-10) | §5.2a rule 2's project-schema class, rules 3–6; WP-2.16e fixtures; reconciliation #24 |
| Phase 0–2 WPs sized at 1–6 engineer-weeks; RT-21, ED-22 and CL-24 on the critical-path analysis | §1 split rule; §2.3a (WP-2.16a1…f); §3.1 DX schema lane and slack; K38; reconciliation #26 |
