# Consistency pass: log of cross-section fixes

*Integration pass of 2026-09-25. Inputs: [_integration-notes.md](_integration-notes.md), a full read of
[00-decisions.md](00-decisions.md) through [09-roadmap-and-process.md](09-roadmap-and-process.md), and
automated checks. Each fix is a minimal edit in the section that owns the statement. When two sections
disagreed, the owner won: **01** for scorecard numbers and phases, **00** for technology choices, and **09
§3.2** where it had already ruled.*

## 1. Automated checks

| Check | Result |
|---|---|
| Every `AAA-*` ID cited in any section exists in 01 §3, including slash lists such as `SRV-2/5/10/12` | Pass: 0 undefined |
| Every `RT-*`, `RC-*`, `ED-*` and `CL-*` ID cited exists in 02 §8.2, 03 §9.3, 07 §5.2 and 08 §4.4 (17, 11, 18 and 22 IDs at the first pass; now RT-01…22 after §29 (RT-20, RT-21) and §31 (RT-22), RC-1…13 after fix 9.1, ED-1…22 after the round-2 section (ED-19…21) and §29 (ED-22), ED-1…23 after §39 (ED-23), and CL-1…24 after the round-2 section (CL-23, CL-24); each round-3 and round-4 ID has an owning WP and a phase exit in 09) | Pass (re-run in §41) |
| 09's aliases `NS-p.k` resolve to a row of 04 §11.4 (7/5/7/12/7/4 IDs for Ph0–5 after §5, §14, the round-2 04 fixes and the round-3 04 fixes below; 04 §11.4 lists every ID explicitly and only appends, and 09's alias definition now says so); `BE-A1…A21` and `GP-1…17` exist (GP-4 as 4a–4c after §22, and 4a–4d after §30; BE-A20 and GP-15 after §26; GP-16 and GP-17 after §40; BE-A21 after the round-5 05 revisions) | Pass. `BE-A3a/A3b` were missing before fix 5.4; `BE-A15…A19` were added by 05's round-1 fixes (§7 below), and `BE-A20` and `GP-15` by 05's round-3 fixes (§26); all are assigned to WPs and exits |
| "archetype" means only ECS storage, or the five reference game classes (01 §2) | 11 stale uses fixed (fixes 0.2, 1.2, 2.3, 2.4, 6.1) |
| No SQLite for services; the dev database is embedded-postgres | Pass after fixes 2.5, 8.3. The remaining SQLite mentions are the tool registry (07 §3.4, allowed by ADR-014) and "no SQLite" notes |
| Gateway game port | UDP 7777 in 04 §1 and 05 §5. No 27015 anywhere |
| Planet library name | `engine/pcg` everywhere (fixes 2.1, 7.1) |
| Library versions match ADR-013/014 and `third_party/MANIFEST.md` | Pass after fixes 0.1, 0.3, 0.4 |
| Go version | 1.27.1 in every plan section (fix 0.1). CLAUDE.md and CI still say 1.24 (§3) |
| Every relative link and `#anchor` in PLAN.md and `docs/plan/*.md` resolves (GitHub slug rules) | Pass after integration pass 5 (§41): 134 links, 0 broken, 67 anchored links in PLAN.md |
| PLAN.md §7 matches 01 §2.6 row for row (ID, critical-for, owner, phase) | Pass: 52 capabilities (G19 added by fix 22.4); S06's "2 (core 1)" is the README's Ph1 core ledger |
| No lease or leadership state in NATS KV anywhere in the plan (05 §0 rule 8, §2.3; 09 CONF-01/02) | Pass after fix 25.1 |
| Determinism toolchain lists agree (ADR-001a rule 7; ADR-013's Jolt row; 02 RT-03; 06 GP-4a, GP-4d and GP-16 (a); 09 §5.6, §6, K4) | Pass after fixes 25.2, 33.8 and 41.3 |
| Every `0N §x.y` reference in PLAN.md and `docs/plan/*.md` names an existing heading of that section | Pass (§41): ≈ 1,780 references with lists expanded, 0 unresolved |
| Every `helios-cell` mode (`--replicant`, `--role world-script`) appears in 02 §1.3, 04 §1 and §11.1, and PLAN.md §4.2 | Pass after fixes 33.2 and 33.3 |
| Every `.hschema` declaration kind and `@realm` value a section uses is in 02 §3.1–3.2 | Pass after fix 33.1 (`worldscript`, `world`) |
| Every section's status header names its latest review round, and its conformance list covers every ADR it cites | Pass after fixes 33.9, 33.10 and 41.11 (every section with round-4 changes says draft v5) |
| The replicant tier's scope reads "every persistent world zone" wherever a section states it (00 ADR-007, 01 SRV-9 and glossary, 04 §6.4, 05, 06 §7.3, 09 exits and #21, README, PLAN.md) | Pass after fixes 35.1–35.7, 41.2, 41.4, 41.5 and 41.6 |
| A reference to a split WP names the sub-WP that owns the deliverable (WP-2.16 → 2.16a1…f) | Pass after fixes 37.1–37.3 and 41.1 |
| A section's MVP → AAA ladder puts each deliverable in the phase of the WP that 09 assigns it (checked for every round-4 deliverable) | Pass after fixes 41.7 and 41.9 |
| 09 §8.1 names every module directory under `engine/`, `apps/`, `tools/` and `services/{cmd,internal,pkg}/`, and every module README records a `Plan-Rev` ≤ `docs/plan/PLAN-REV` (`cmake -P tools/status/check_status.cmake`; 09 D6, D7) | Pass after R5-09.10 and R5-09.11: 42 directories, PLAN-REV 6 |

## 2. Fixes

| # | File | What changed | Why |
|---|---|---|---|
| 0.1 | 00-decisions.md (ADR-002) | "Go 1.27" → "Go 1.27.1" | Matches ADR-014 exactly |
| 0.2 | 00-decisions.md (ADR-009) | Tool list "data & archetypes" → "data & record templates" | ADR-004 terminology |
| 0.3 | 00-decisions.md (ADR-013) | RmlUi "6.x" → "6.3"; sentry-native "0.17" → "0.17.1" | 02 §7.5, 08 §1.7, 08 §3 and the MANIFEST pin these exact versions |
| 0.4 | 00-decisions.md (ADR-013) | nats.c row now also covers the editor (T30 collab); Audio row adds libopus (BSD-3) | 07 §1.8 links nats.c in the editor; 09 §3.2 #6 approved libopus (02 §6.2) |
| 1.1 | 01-vision-and-scope.md | Header cites ADR-001…014, not …012 | ADR-013/014 exist and are binding |
| 1.2 | 01-vision-and-scope.md (§2 d) | "cleanup policy for every persistent archetype" → "…record template" | ADR-004; matches 05 §1.13 lifecycle rules |
| 1.3 | 01-vision-and-scope.md (§2.6) | M06 Ph "3" → "1 (full: 3)" | ASM compiler and GP-2 ship in Ph1 (06 §12.1, 09 WP-1.15, ED-4 cites M06 at Ph1); the full Destiny-grade capability stays Ph3 (09 WP-3.6) |
| 1.4 | 01-vision-and-scope.md (AAA-TOOL-3) | Note that T15's motion-matching and facial items follow their Ph4 runtime | 09 §3.2 #2; 02 §7.2 puts motion matching and facial in Ph4 |
| 1.5 | 01-vision-and-scope.md (AAA-TOOL-4) | "T14 auto-staging" → "T13 auto-staging, T14" | Auto-staging is T13's Ph4 AAA item (07 T13); 07 §5.1 says the four Ph4 tools are T13, T14, T22 and T27 |
| 1.6 | 01-vision-and-scope.md (§6) | Glossary entry "Reference archetype" | Separates the third meaning of "archetype" from ECS archetypes and record templates |
| 2.1 | 02-engine-runtime.md (§1.1, §5.8, §8.6) | Dropped the "(07's planetgen)" aliases; the name is `engine/pcg` | Integration note from 02; 07 now uses `engine/pcg` |
| 2.2 | 02-engine-runtime.md (§1.1) | `linux-server` → the `linux-headless` preset | `CMakePresets.json` defines `linux-headless`; 09 §3.2 #3 |
| 2.3 | 02-engine-runtime.md (§5.6) | "SoA component blocks per archetype" → "per ECS archetype" | ADR-004 terminology (storage sense, made explicit) |
| 2.4 | 02-engine-runtime.md (RT-01) | "~150 archetypes" → "~150 ECS archetypes" | Same |
| 2.5 | 02-engine-runtime.md (§8.6) | Stale "05's SQLite mentions" row → "embedded-postgres (ADR-014, 05 §3.5); no service uses SQLite" | 05 was already correct (lead-verified) |
| 2.6 | 02-engine-runtime.md (§1.1) | Added the L4 HEADLESS module `assembly` (deps: gameplay, records) | 07 T21 specifies a shared HEADLESS `engine/assembly`, which was missing from the layering DAG that CI enforces |
| 2.7 | 02-engine-runtime.md (§1.3) | Launcher row adds `crash` and the shipped names `Helios.exe` + `HeliosLauncher.exe` | 08 §2.1 links `crash` into the launcher and defines the bootstrap names |
| 3.1 | 03-rendering.md (§2.5) | "02's 12 ms pacing allowance" → "the 16.6 ms frame (02 §2.4)" | 02 defines no 12 ms allowance (main ≤ 8 ms, render ≤ 4 ms) |
| 3.2 | 03-rendering.md (§9.6) | The "Conflict" on per-object camera-relative matrices is marked resolved | 02 §5.2 adopted two-level transforms |
| 4.1 | 04-networking-and-servers.md (§5.2) | Input carries `device_class` (mouse, pad, HOTAS) | Server-side per-device aim assist (04 §5.6, 06 §8.6, 08 §1.5); 08 §4.8 asked for it |
| 4.2 | 04-networking-and-servers.md (§11.3) | Netcode Ph1 adds basic hitscan lag comp and ability prediction keys; Ph2 becomes full lag comp, projectiles and predicted abilities at 500/zone | 01 M05 is Ph1; 09 WP-1.11 ("basic lag comp") and WP-1.15 (prediction keys, GP-2) are Ph1 |
| 4.3 | 04-networking-and-servers.md (§11.3) | Instancing: activities move from Ph2 to Ph3; Ph2 is instance lifecycle and overflow layers | The activity service is Phase 3 in 05 §1.12 and 09 WP-3.2; 01 G12 is Ph3 |
| 4.4 | 04-networking-and-servers.md (§2.5, §11.4 Ph4) | Battle downstream is 512 kbit/s through Ph4 (was "≤ 1 Mbit/s"), 1,000 kbit/s from Ph5 | 01 AAA-SRV-6 (256/512 through Ph4, 256/1,000 in Ph5; R07 §8.3) |
| 5.1 | 05-backend-services.md (header) | R07 P0/P1/P2 map to Phases "2–3, 3–4, 5" (was "3, 4, 5") | 01 §3.4 defines the mapping |
| 5.2 | 05-backend-services.md (§1.6, §9) | Ledger 2k tx/s moves from Ph3 to Ph2 (scaling list and ladder) | AAA-SRV-8; 09 §3.2 #8 |
| 5.3 | 05-backend-services.md (§1.2, §9, A8) | 50/s admission moves from Ph3 to Ph2 (queue lanes); A8 phase "2–5" | AAA-SRV-7; 09 §3.2 #9 |
| 5.4 | 05-backend-services.md (A3) | Split into **A3a** (Ph1: restart from checkpoint, players reconnect, loss ≤ 60 s, 0 item deltas) and **A3b** (Ph2: warm standby ≤ 10 s) | 09 cites A3a/A3b; the old A3 promised a Ph2 warm standby in Ph1 (04 §6.4, 05 §1.4) and exceeded AAA-SRV-9/12 at Ph1 (09 §3.2 #7) |
| 5.5 | 05-backend-services.md (A5) | Phase "3 / 4" → "2 / 4" | AAA-SRV-8 |
| 5.6 | 05-backend-services.md (A12) | Restated as AAA-CNT-7: download ≤ 1.5× changed bytes; 50 GB verify ≤ 5 min | A12 (≤ 3 %, 60 GB in < 10 min) was weaker than CNT-7, CL-9 and CL-10 (09 §3.2 #10) |
| 5.7 | 05-backend-services.md (§1.14, §9, §13) | Added the **collab service** (sequencer stream, lock/notes KV, overlay content versions) to the content domain | 07 §1.8 places it at "05 §1.14", which did not describe it; integration note from 07 |
| 5.8 | 05-backend-services.md (§7) | Pointer adds `min_client`, `cdn_hosts[]` and `next{build_id, manifest_hash, available_at}` | 08 §1.1, §2.6 and §4.8 depend on these fields |
| 5.9 | 05-backend-services.md (§1.5, §1.1, §6.2) | Character service owns the ≤ 256 KiB settings blob; bot-scoped credentials; crashes route through `crashgw` | 08 §1.4, §1.13 and §3; 08 §4.8 asks |
| 5.10 | 05-backend-services.md (§2.1, §7) | Launcher HTTP is "WinHTTP on Windows, libcurl on Linux" | 08 §2.5; Linux parity (ADR-001) |
| 6.1 | 06-gameplay-framework.md (§1.2, §7, §8.1, §8.7) | Seven designer-content uses of "archetype" → "record template" (`AttributeSetDef`, `AttrBlock`, blackboard, spawner entries, movement model, `DamageModelDef`, `DeathPipelineDef`) | ADR-004; integration notes from 01, 05 and 06 |
| 6.2 | 06-gameplay-framework.md (§6) | `PhaseFilter` TagQueries compile to bits of 04 §7's 64-bit per-zone `phase_mask` | 04 tests a `phase_mask` in the interest filter; 06 described a TagQuery with no link between the two |
| 6.3 | 06-gameplay-framework.md (§12.2) | Header points to 09 §3.2 #15 for phases | 06's criteria carry no phases; 09 assigns them |
| 7.1 | 07-editor-and-tools.md (T04, §5.1, §5.3, §5.5) | `engine/planetgen` / `planetgen` → `engine/pcg`; T04 now says the GPU twin makes visual LODs from fixed-point `hnoise` | 02 §1.1, §5.8 and 03 §5.5 |
| 7.2 | 07-editor-and-tools.md (T15, §5.1) | Motion matching moves from T15's Ph3 AAA list to Ph4 with facial and lip-sync | 02 §7.2 ladder (motion matching Ph4); 09 WP-4.6 |
| 7.3 | 07-editor-and-tools.md (§1.8, §5.5) | nats.c in the editor is now "allowed by ADR-013" | Fix 0.4 |
| 8.1 | 08-client-and-launcher.md (§4.3) | Game UI Ph0 "RmlUi on RHI" → none (RmlUi lands in Ph1; the Ph0 launcher is plain SDL3) | 03 §9.2 and 09 §3.2 #11, WP-0.17 |
| 8.2 | 08-client-and-launcher.md (§4.1) | States that 02 §7.5–7.6 specifies the `engine/ui`/`text`/`input`/`loc` runtimes; 08 owns HUD, screens, markers and addons | Integration note from 08: the README listed game UI under both 02 and 08 |
| 8.3 | 08-client-and-launcher.md (§4.8) | Stale asks (04 SQLite, A12, README game UI, ADR-010) marked resolved with pointers | Fixed in 04, 05, README and ADR-010 |
| 8.4 | 08-client-and-launcher.md (CL-7) | 40 diegetic screens "≤ 0.8 / 1.5 ms" → "≤ 1.0 ms CPU / 0.5 ms GPU" | 02 §7.5, RT-12 and 03 §7.5 set this budget; 03 owns GPU budgets |
| 9.1 | 09-roadmap-and-process.md (§3.2) | Rows #2, #3 and #7–#11 note where the owning section was updated; #2 adds motion matching | Keeps the reconciliation table truthful after fixes 1.4, 2.2, 5.x, 7.2, 8.1 |
| 9.2 | 09-roadmap-and-process.md (§8.1) | Test counts updated: core 133/133; math 101/102 (one in-flight noise-statistics check fails); Plan row points to PLAN.md and this log | Headless GCC 13.3 build of the working tree on 2026-09-25 |
| R.1 | README.md | 02 row: "game-UI runtime (`engine/ui`, `engine/text`)"; 08 row: "HUD and screens on `engine/ui`"; lists CONSISTENCY.md | Single owner for the UI runtime |
| R.2 | README.md (phase vocabulary) | Ph1 adds the core ledger (starter loadouts, currency, no trading); Ph2 says "full inventory ledger and trading" | 09 §3.2 #1 |
| R.3 | README.md (phase vocabulary) | Ph4 render: volumetric clouds and TAA/upscaling land earlier and become golden-gated in Ph4 | 03 §9.2 ships clouds in Ph3 and TAA in Ph1; AAA-REN-6 gates them in Ph4 |
| N.1 | _integration-notes.md | Status banner: all items resolved, see this log | Historical record kept unchanged |

## 3. Out of scope for this pass (outside `docs/plan/`, owned by Phase 0 WPs)

| Item | Where | Owner |
|---|---|---|
| Go 1.24 → 1.27.1 | `CLAUDE.md` (Build), `.github/workflows/ci.yml` (`go-version`) | WP-0.1 (09 §3.2 #13) |
| `cmakeMinimumRequired` 3.24 → 3.28 | `CMakePresets.json` | WP-0.1 (09 §3.2 #14) |
| Dear ImGui row says "Editor / launcher / debug UI" | `third_party/MANIFEST.md` | WP-0.2: launcher UI is RmlUi on SDL_Renderer (ADR-010) |
| Module order predates 02 §1.1 (no `hxl`, `records`, `pcg`, `assembly`, `replay`, `voice`…) | `engine/CMakeLists.txt` `HELIOS_MODULE_ORDER` | WP-0.2 (09 §3.2 #3) |
| One failing math test (noise statistics, `test_noise.cpp:309`) | `engine/math/tests` | The in-flight math WP |
| SDL3 is vendored with `SDL_RENDER OFF` and `SDL_WAYLAND OFF`, but the launcher draws with `SDL_Renderer` (08 §2.1) and SDL must pick Wayland or X11 (08 §1.16). Enable `SDL_RENDER` (D3D11/D3D12 on Windows, D3D9 off; OpenGL, Vulkan and software on Linux), `SDL_OPENGL` on Linux, and `SDL_WAYLAND` with `SDL_WAYLAND_SHARED`/`SDL_WAYLAND_LIBDECOR_SHARED`; keep `SDL_GPU` off; add `libwayland-dev`, `wayland-protocols`, `libxkbcommon-dev` to CI Linux images; add the launcher import audit | `third_party/CMakeLists.txt` (SDL3 block), `.github/workflows/ci.yml` | WP-0.17 (09 §3.2 #19) |
| No workflow may run on the `win-gpu` self-hosted runner from a pull-request trigger or with secrets; add `tools/ci/check_runner_policy` | `.github/workflows/*`, `tools/ci/` | WP-0.4 (09 §5.4a, K33) |
| Add the ADR-001a floor job `windows-msvc-floor` (`runs-on: windows-2022`, `ilammy/msvc-dev-cmd` with `toolset: 14.44`, `windows-msvc-release` build + all tests) beside the existing `windows-latest` (VS 2026) job; add nightly `windows-vs2026`/`windows-vs2022` MSBuild builds | `.github/workflows/ci.yml` | WP-0.1 (ADR-001a) |
| Set `HELIOS_MODULAR=ON` in the `windows-vs2026` and `windows-vs2022` IDE presets and add `windows-msvc-dev`/`linux-dev` | `CMakePresets.json` | WP-0.6c (ADR-001a rule 5, ADR-016) |
| `CLAUDE.md` says "MSVC 2022 / clang-cl" and suggests `windows-vs2022`. It should say "MSVC: VS 2026 primary, VS 2022 17.14 floor (ADR-001a) / clang-cl" and suggest `windows-vs2026`. Not edited in this pass, because `CLAUDE.md` changes need the user's sign-off | `CLAUDE.md` (Platforms, Build) | The user / WP-0.1 |

## 4. Accepted variances (checked, deliberately unchanged)

- **`hmath`** (06, 07) is an alias for `helios::det` (02 §8.6 says so).
- **06's citation keys** (`R09-A14`, `R09-G5`, `R02-Q10`) differ from 01 §7's style. 06 defines them in its
  header and they are unambiguous.
- **"Reference game archetypes"** (01 §2, 09 §5.7) is the game-class sense, now in the glossary (fix 1.6).
- **`helios-launcher`** is the build target; players see `Helios.exe` and `HeliosLauncher.exe` (fix 2.7). 09's
  M0 dev command line uses the target name.
- **S03 (dynamic meshing) is ● for Star Citizen but Phase 5.** *Corrected in §14.* The first pass said the
  Phase 4 bar matched SC's shipped architecture with static meshing alone and that SC still had its gateway
  replication layer in progress. Both were wrong: SC shipped its replication layer (Replicant, Atlas, Scribe,
  Gateway) with PES in Alpha 3.18 and static meshing on it in Alpha 4.0, so a game-server crash no longer
  disconnects players; only dynamic meshing is in progress (R04 §4.1–4.2). ADR-007 now adds the Phase 4
  replicant tier, so at the bar the SC class has static multi-cell zones plus crash recovery from RAM (no
  disconnect, ≤ 1 s of state, AAA-SRV-9 Ph4, NS-4.6). S03 stays ● and Phase 5, because it is SC's own
  unfinished target; SC has not shipped it either.
  [PLAN.md §7](../PLAN.md#7-capability-coverage-for-the-five-reference-game-classes) states this, including
  the one structural difference (SC's replicants also stream client replication; Helios moves that in v2).
- **Stricter-than-scorecard targets** stay as they are, for example 04 Ph1 tick p99 ≤ 25 ms (01 allows 40 ms)
  and 06's 72 h soak (09 gives it phases). A section may be stricter than 01, never looser.

## 5. Round-1 review fixes owned by 04 (2026-09-25)

04 was changed substantively: §6.1 AG model, §6.3–6.4, §3.5 zone leader, §10.2 replay contract, §2.6 trunks
and gateways, §2.7 voice, §11.2–11.5. The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 5.1 | 00-decisions.md | New **ADR-015 Voice chat**: Opus over HTP through a C++ `helios-voice` forwarder; LiveKit/WebRTC SFU rejected for the native client | Voice was promised (02 §7.3, WP-3.4, libopus approved) with no server path. The reviewer asked for an ADR |
| 5.2 | PLAN.md (§5, §6); 01 header | ADR-015 row; "fifteen ADRs"; "ADR-001…015" | Keeps the ADR index and summaries complete |
| 5.3 | 05-backend-services.md (§1.4, §1.6, §1.13, §2.4, §3.2, §4.1, §8, §13) | Fence rows gain `root_ag`, `kind`, `owner_region`, `owner_lease_gen`; tree invariant; `Fence.Join/Leave/AdvanceMany/AdvanceOwnedBy`; ledger op `CreateAg`; persistence fence cache and `(e+1, 0)` records; orchestrator `zone_leader`/`zone_handle_block` tables and calls; replies drain by count, not a 2 ms cap | Mirrors 04 §6.1 (AG model), §6.4 (bulk recovery), §3.5 (zone leader) and §10.2 (replay) |
| 5.4 | 02-engine-runtime.md (§2.4, §5.7, §7.3, §7.4) | Cell Luau budgets are fuel counted at safepoints, with a logged 5 ms wall-clock kill; the cell sandbox also removes `collectgarbage` and rejects `__mode`; cell container activation is metered in entity work units and recorded; voice pointer to 04 §2.7 | Wall-clock scheduling contradicted bit-exact replay (04 §10.2, NS-2.4) |
| 5.5 | 06-gameplay-framework.md (§11) | Interrupt wording matches the fuel budget; new rule 9: no address-ordered table iteration, `EntityMap` for entity keys | Same |
| 5.6 | 08-client-and-launcher.md (§1.4) | Voice-chat settings: device, push-to-talk, per-channel volume, proximity opt-in, mute strangers, mute/block/report | Client UX for the 04 §2.7 moderation policy |
| 5.7 | 09-roadmap-and-process.md (WP-0.10, 0.13, 1.13, 2.4, 2.6, 3.1, 3.4; Ph0–3 exits) | New clauses NS-0.7 (trunk), NS-1.5 (AG trees), NS-2.6 (gateway), NS-2.7 (recovery fence), NS-3.6 (AG-tree torture), NS-3.7 (zone-leader kill), NS-3.8 (cross-compiler replay), NS-3.9 (voice) assigned to WPs and exits; NS-2.4 strengthened in place | Every new criterion has an owner WP and a phase exit |


## 6. Round-1 review fixes owned by 01 (2026-09-25)

01 §3.3 (REN-6/7 split into coverage and completeness) and §3.4 (named tick profiles, one 2 Hz fleet-battle
profile, SRV-3 and SRV-10 measurement rules) changed substantively. The edits below are the smallest consistent
changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 6.1 | 04-networking-and-servers.md (§3.4) | Fleet-battle row: "10 Hz × TiDi" → "2 Hz × `d`; every ship `CommandKinematic`"; new note: fleet battle is the one battle profile, replication capped at the tick rate, response gated in ticks of game time | 01 §3.4, M01 and 06 §8.1 used 1–2 Hz. At `d` = 0.1 even a 10 Hz tick lasts 1 s of wall time, so "< 1 s wall" could not pass |
| 6.2 | 04-networking-and-servers.md (§4.7) | "Space battle" relabelled as the 20 Hz open-space case; new 2 Hz fleet-battle row (2,000 ships ≈ 147 kbit/s at `d` = 1) | Shows NS-4.3 / AAA-SRV-6 still hold at the new rate |
| 6.3 | 04-networking-and-servers.md (§11.4) | NS-4.1 = 50k CCU held ≥ 1 h (and the STB-4 soak); NS-4.2 = 30 min, `d` ≥ 0.1, module response p95 ≤ 1 tick of game time + RTT at `d` = 0.1, no activation waiting more than 2 ticks. Both edited in place; the IDs are unchanged | A range ("20–50k") is not a pass threshold, and STB-4 already needed 50k |
| 6.4 | 06-gameplay-framework.md (§8.1) | `CommandKinematic` runs in "2 Hz fleet-battle zones" (was "1 Hz zones") | One battle profile |
| 6.5 | 03-rendering.md (§9.3 RC-11) | Ph1 set spelled out and now includes CDLOD cube-sphere planets and the orbit-to-ground Hillaire atmosphere; Ph3 is Earth-size planets with oceans plus froxel fog | Planets ship in Ph1 (BENCH-2, 03 §9.2 ladder), but REN-6 gated them only in Ph3 |
| 6.6 | README.md (Phase 4); ../PLAN.md (§9 SRV row) | "20–50k CCU" → "50k"; the README says REN-6 is a completeness gate over per-commit goldens; the PLAN.md SRV summary names the 2 Hz profile and the tick-bounded response | Matches 01 §3.3–3.4 |

## 7. Round-1 review fixes owned by 05 (2026-09-25)

05 changed substantively: §1.4 control plane (PG-anchored leadership and generations, two-signal failure
detection, degraded mode, no self-fencing, block IDs), §1.7 market fencing, §1.21 world state, §1.22 Economy Sim,
§3.3/§3.6 retention and storage-growth model, §6.6 privacy, §6.7 DR, §6.8 on-call, and A6/A7/A9 plus new
A15–A19 in §10. The 04-owned fence edits of fix 5.3 were merged unchanged. The edits below are the smallest
consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 7.1 | 04-networking-and-servers.md (§1 table, §3.5 failover, §6.1 transient row, §6.4 detection) | Region leases are PG generations with heartbeat liveness (no "NATS KV, TTL 3 s"). Zone-leader failover and crash recovery start at two-signal confirmation (≈ 3 s) and never while the control plane is degraded. A superseded cell is stopped by rejected writes and generation checks at gateways and neighbour cells, not by a 3 s self-fence. The ≤ 10 s hitch budget counts from confirmation | A NATS stream-leader election or node loss would have self-fenced every cell at once; R07 §8.4 expects no player impact from a NATS node loss |
| 7.2 | 02-engine-runtime.md (§4.1 identity table) | Runtime-spawn `EntityId`s are time-prefixed block IDs 41/5/17 (was Snowflake 41/5/8/9), minted by the `EntityRegistry` from PG-allocated blocks | 256 node IDs per shard were fewer than 05 §6.4's cell count, and a 9-bit sequence could stall volley bursts |
| 7.3 | 06-gameplay-framework.md (§3 crafting, §6 events, §9 territory, §12.4 dependencies) | `CraftStamp` drops `crafterName` (resolved from `crafterId` at display); world flags, meta-events, influence and sovereignty point to 05 §1.21; the four 05 asks now cite 05 §1.21, §1.22, §1.8 and `pkg/hxl` | Erasure without rewriting items; the world-state service was missing |
| 7.4 | 08-client-and-launcher.md (§2.3) | Step 5: age gate at registration, versioned ToS/EULA/privacy acceptance before any launch code, Settings → Privacy links to export and erasure | 05 §1.1 and §6.6; the reviewer asked for acceptance in both 05 and 08 |
| 7.5 | 09-roadmap-and-process.md (WP-2.6, WP-3.3, WP-4.4, Ph2–4 exits, risk K32) | BE-A17 (world state) in WP-2.6 and the Ph2 exit; BE-A15 (control-plane chaos) in WP-3.3 and the Ph3 exit; BE-A16 (IDs), A18 (privacy), A19 (DR) in WP-4.4 and the Ph4 exit; new risk K32 (privacy law and residency, counsel) | Every new criterion has an owner WP and a phase exit |
| 7.6 | ../PLAN.md (§2 backend row, §6 05 summary, §9 criteria list) | 22 services; control plane, world state, retention model, privacy, DR and on-call named; `BE-A1…A19` | Keeps the summary truthful |
| 7.7 | This file (§1 checks) | `BE-A1…A19` | New criteria |

**Correction to the first pass.** The _integration-notes item "05: world-state flags service, Economy Sim
service" was logged as resolved (fix N.1) but 05 had no such services. It is now resolved in 05 §1.21–1.22
(fix 7.3 updates 06's pointers). The Go HXL package and crafting refund timers are now explicit too
(05 §8, §1.8).

## 8. Round-1 review fixes owned by 02 (2026-09-25)

02 changed substantively in five places:
- **§1.4 (new):** the dev versus shipping link model and the game-DLL reload protocol, with ADR-016, RT-18 and
  the WP-0.6c spike.
- **§2.4:** the three-thread client model.
- **§5.4:** bubbles are disjoint clusters with the I1 and I2 invariants, and RT-19.
- **§7.3:** the `engine/voice` client pipeline, plus a `voice` module and the `helios-voice` target.
- **§7.4:** no involuntary Luau yields, sticky fuel kills and `StaleHandle`, with RT-13 extended.

02 also adopted 07's and 08's values for four duplicated facts. The edits below are the smallest consistent
changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 8.1 | 00-decisions.md | New **ADR-016**: shipping is monolithic static `/MT`. Dev (`HELIOS_MODULAR`) is three engine shared libraries with `HELIOS_*_API` exports, `/MD` everywhere and incremental linking, plus reloadable game DLLs. ADR-011's CRT bullet now says `/MT` for shipped binaries and `/MD` for dev modular builds | Game-DLL hot reload (AAA-ITR-5) conflicted with static modules, per-image `/MT` heaps, split singletons and `/OPT:REF` links. ADR-015 was already taken by voice |
| 8.2 | ../PLAN.md (§5, §6, index); 01 header | ADR-016 row; "sixteen ADRs"; "ADR-001…016" (the index row still said …014) | Keeps the ADR index complete |
| 8.3 | 09-roadmap-and-process.md (WP-0.6, WP-0.10, WP-1.2, WP-1.4, WP-1.20, WP-3.4, Ph0–2 exits, §3.2 #18) | WP-0.6 becomes 3 weeks with spike (c), the link model (RT-18). WP-1.4 depends on 0.6c. WP-0.10 lists the yield and kill tests. WP-1.2 carries RT-19. RT-12 is the game-thread clause. The Ph0 exit adds RT-13 and RT-18 ("all three spike decisions"); Ph1 and Ph2 add RT-19. WP-3.4 voice names `engine/voice`. Reconciliation row 18 records the link-model conflict | Every new criterion has an owner WP and a phase exit |
| 8.4 | 04-networking-and-servers.md (§3.1, §5.3, §10.2 Luau rows, NS-2.4, risks) | The interrupt never yields. The lane stops resuming once `fuel_per_tick` is spent, and a resume is killed at `fuel_kill` (≈ 5 ms fuel, sticky). A 20 ms wall backstop is the only logged wall-time kill. NS-2.4 injects fuel and backstop kills instead of relying on "fuel yields". §5.3: the hull is predicted in the owning bubble's coordinates (the replicated `BubbleOrigin`, 02 §5.4) | `lua_yield` errors across metamethod/C-call boundaries (`VM/src/ldo.cpp`), and invisible yields broke 06 §11's handle rule |
| 8.5 | 06-gameplay-framework.md (§11 VMs, rule 6) | Same interrupt wording; yields only at explicit calls; stale handles raise `StaleHandle` | Same |
| 8.6 | 08-client-and-launcher.md (§1.4) | CVar flags use the landed `core/cvar.h` names: `Saved`, `Cheat`, `Restart`, `Replicated`, `Dev` | 08 used `archive` and `server_locked` for flags the code calls `Saved` and `Replicated` |
| 8.7 | 07-editor-and-tools.md (§1.6) | Gameplay C++ modules live-reload in the editor and PIE (ADR-016, 02 §1.4); "Rebuild & restart PIE" is the fallback when engine headers change | 07 described a restart only, while 02 and AAA-ITR-5 promise a reload that keeps the world |

**Duplicated facts resolved in 02, with no edit needed in the owner:**

| Fact | Was (02) | Now (single owner) |
|---|---|---|
| Local DDC cap and path | 100 GB | 200 GB at `%LOCALAPPDATA%\Helios\DDC`, LRU (**07 §3.2**) |
| assetd transport | Localhost TCP/HTTP | Named pipe `\\.\pipe\helios-assetd-<project>` or a Unix socket for requests, products and notifications; remote cells use NATS plus the shared DDC (**07 §3.1**) |
| Asset registry storage | Append-only tagged log | `.helios/registry.db` (SQLite, tools only); 02 keeps the cooked `registry.hreg` (**07 §3.4**) |
| User settings and rebinds | `%APPDATA%\Helios\<project>\`, `input.jsonc` | Machine file `%LOCALAPPDATA%\Helios\<channel>\settings\machine.jsonc`, plus the account-scope blob for keybinds (**08 §1.4–1.5**) |
| Client threading | SDL, ticks and extract on one "main" thread | OS thread (SDL), game thread (ticks, update, extract), render thread (**08 §1.3**); 02 §2.4 rule 1 adds the input ring and `SDL_RunOnMainThread` |

**Out of scope here (code, owned by WP-0.6c):** add `HELIOS_MODULAR`, the three link groups and the
`windows-msvc-dev` and `linux-dev` presets to `CMakeLists.txt`, `cmake/HeliosModule.cmake` and
`CMakePresets.json`. The top-level `/OPT:REF /OPT:ICF` link options must also become conditional on
`NOT HELIOS_MODULAR`.

## 9. Round-1 review fixes owned by 03 (2026-09-25)

03 changed substantively: §1.3a texture and geometry streaming residency (GPU feedback, CPU trajectory
prediction, upload caps, descriptor-stable `StreamTexId` slots, eviction under `memory_budget`, mesh LOD
streaming); §4.6a acceleration-structure management for DDGI; §5.5a terrain-generation throughput (32-bit-only
`hnoise` twin, level-adaptive octaves, demand and cost model, spike with pre-decided fallbacks); §5.6a
vegetation; §5.8a weather effects; §1.2 feature table (`shaderResourceMinLod` required, `shaderInt64` explicitly
not required); §8.1 gains a REF BENCH-5 column, DDGI and weather rows and planet-surface sub-budgets; new
criteria RC-12 (streaming) and RC-13 (tile throughput); RC-11's Ph3 set adds DDGI, vegetation and weather. The
edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 9.1 | 09-roadmap-and-process.md (WP-0.9, 1.3, 1.7, 1.8, 2.1, 3.5; Ph0–3 exits; §3.1; §7) | WP-0.9 gains sub-WP **WP-0.9c**, the `hnoise` throughput spike (MIN confirmation before WP-1.8). RC-12 goes to WP-1.7 (Ph1), WP-2.1 (Ph2) and WP-3.5 (Ph3, BENCH-6). RC-13 goes to WP-1.3 (CPU clause) and WP-1.8. WP-3.5 lists AS management, vegetation and weather. The Ph0 exit records WP-0.9c; the Ph1–3 exits add RC-12/13. New risk **K5b** (tile-generation throughput). §3.1 notes that the spike de-risks the near-critical rendering lane | Every new criterion has an owner WP and a phase exit; the reviewer asked for a Phase 0 spike and a K5b trigger |
| 9.2 | 01-vision-and-scope.md (AAA-REN-6) | The Ph3 set adds DDGI (REF) with relit assembly-time probes (MIN), vegetation, and weather effects | GI, vegetation and weather quality were never gated, although T18 promises per-biome weather; mirrors RC-11 |
| 9.3 | 02-engine-runtime.md (§5.8 twin row, bytecode bullet) | The Slang twin is 32-bit-only (no `shaderInt64`). Generators take the tile level and skip sub-2× spacing octaves above the collision levels, identically on CPU and GPU. The 0.5 ms/tile figure is validated by WP-0.9c | 03 §5.5a; the CPU VM must match the GPU per level for RC-4 to hold |
| 9.4 | ../PLAN.md (§6 03 summary, §9 criteria list) | "RC-1…13"; the 03 summary names streaming (RC-12), range-limited AS for DDGI, weather effects and vegetation | Keeps the summary truthful |

**Open asks recorded in 03 §9.6 (no edit made):** 06 to own the replicated per-body `PlanetWeather` component,
the front spawner and weather gameplay effects; 07 T05 `FoliageDef`, T18 per-biome weather tables, T28 card-coverage
and `uvDensity` lints; 02 `RenderScene` to carry the `StreamingSource` list and a camera-cut hint.

## 10. Round-1 review fixes owned by 08 (2026-09-25)

08 changed substantively. The new §1.3a covers input latency: measurement points M0–M8, a per-stage budget
table (60 and 120 fps; action, UI and camera), a just-in-time `FramePacer` using `present_wait` with a
GPU-completion fallback, the Phase A/B game-thread split, same-frame 3-batch submission, a speculative cue step
for frames without a command tick, camera late-latch rules, Tracy and telemetry instrumentation,
`helios-latbench` and a weekly photodiode rig. §1.3 updates the threading table. CL-6 is now H-class and gated
from Ph2: action and UI ≤ 40 ms, camera ≤ 33 ms at 60 fps; Ph4 adds action ≤ 28 ms and camera ≤ 25 ms at
120 fps. §4.4 gains an evidence-class column for all CL criteria. The edits below are the smallest consistent
changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 10.1 | 02-engine-runtime.md (§2.4 diagram and budgets) | The game thread runs Phase A (input-independent, ≤ 3 ms), then the JIT sleep, then Phase B (input, speculative cues, ticks, local update and HUD, ≤ 4 ms), then extract. The OS thread blocks in `SDL_WaitEventTimeout(1 ms)` and feeds a mouse-delta atomic. Render and GPU rows show low-latency mode (same-frame N) and throughput mode (N−1/N−2). `core` hosts `LatencyMarkers` | The 8 ms game frame on the critical path breaks CL-6's 40 ms at 60 fps (42.5 ms). Totals are unchanged: ticks ≤ 3, update ≤ 4, extract ≤ 1 |
| 10.2 | 03-rendering.md (§1.2 table, §2.5, §4.5 CSM bullet) | Adds `VK_NV_low_latency2` and `VK_AMD_anti_lag` as optional features. §2.5 labels its diagram throughput mode and adds a low-latency-mode bullet: same-frame render, 3 ordered submit batches, latched view constants used by GPU culling and TAA, a guard-banded CPU cull. CSM cascades are stated as sphere-fitted (rotation-invariant) | Reviewer: 03 §2.5's N / N−1 / N−2 pipeline was never reconciled with CL-6 |
| 10.3 | 09-roadmap-and-process.md (WP-2.13, WP-3.8, Ph2 exit) | WP-2.13 owns the pacer, split, speculative cues, markers and `helios-latbench`, and lists CL-6 (Ph2). WP-3.8 adds the vendor providers and BENCH-4 latency runs. The Phase 2 exit adds CL-6 (Ph2). WP-4.2, WP-4.9 and the Ph4 exit keep the full CL-6 | Reviewer asked for an H-class nightly criterion from Phase 2 |

**No change needed:** 04 (the speculative step adds no wire field; lag compensation still uses
`view_tick + fraction`); 06 (it uses only the ASM and key-deduplicated cues of 06 §1.4–1.5).

## 11. Round-1 review fixes owned by 06 (2026-09-25)

06 changed substantively in these places:
- **§1.2:** cross-language float rules for HXL. Go may fuse `x*y + z` into an FMA on arm64 and at
  `GOAMD64=v3`. The rules are explicit `float64()` rounding, no constant arithmetic in the `det` port, the
  `hxlfloat` lint, a pinned `GOAMD64=v1` checked at start, and FMA-sensitive corpus vectors run on v1, v3
  and arm64.
- **§5.2–5.3 (new):** account progression (achievements, collections, codex, legacy) and seasons, built as
  `ProgressionGraphDef` kinds with a Legacy (account × shard) scope and driven by the quest event index.
- **§7.3 (new):** owned NPCs (companions, pets, hirelings, crew, drones, fighters). They join the owner's AG,
  receive commands on a `Followers` channel, keep gear in ledger items, and run crew missions as Industry jobs.
- **§9.6–9.9 (new):** the interaction framework (verbs, validation, doors, pressure, terminals, loot
  rights), emotes and performances, map data, and weather gameplay.
- **§12:** GP-1 extended; GP-4 moves to the 2 Hz fleet-battle tick; new criteria GP-10…13; three ladder
  rows and four risks.

§9 and §7 were split into subsections. Every inbound "06 §5/§7/§9" citation still resolves.

| # | File | What changed | Why |
|---|---|---|---|
| 11.1 | 05-backend-services.md (§1.5, §8, §13 row) | The Character service owns follower progression (keyed by device item) and per-`(account, shard)` account progression, with `ApplyAccountProgression` (idempotent by flush sequence, ≤ 32 KiB per account, 2k flushes/s), legacy-bank owner kind and entitlement mirroring. `pkg/hxl` states the FMA rules and the GOAMD64 v1/v3/arm64 corpus matrix | The Character service stored progression per character only. Go-side HXL could silently diverge under FMA fusion |
| 11.2 | 04-networking-and-servers.md (§6.1) | New bullet: owned NPCs are authority members of the owner's AG with no fence row; they travel in the owner's `HandoffOffer`; abandoned drones become roots via `CreateAg` + `MoveItem` | 04's table classed every NPC as transient with no custody, which would contradict followers that hold gear |
| 11.3 | 07-editor-and-tools.md (T09, T13, T23) | T09: account-progression and season-track editors, crew-mission tables. T13: companion influence deltas, rank-gated conversations. T23: follower records, `CommandSelector` template, leash, control-range and bandwidth checks | The reviewer asked for authoring in T23 and T13 |
| 11.4 | 08-client-and-launcher.md (§1.7 panel table) | HUD adds interaction prompts and a follower command bar. Maps adds the minimap and the surface and interior maps. Chat/social adds emotes. New Progression panel row | 08 had only the orrery and galaxy map, and no prompt, emote or achievement UI |
| 11.5 | 09-roadmap-and-process.md (WP-1.16, 2.9, 3.6, 4.5; Ph1–4 exits; §3.2 #15; determinism test row) | GP-12 goes to Ph1 (WP-1.16); GP-10, 11 and 12 to Ph2 (WP-2.9); GP-10, 11 and 13 to Ph3 (WP-3.6); GP-11's seasons clause to Ph4 (WP-4.5). WP-3.6's "1 Hz command flight" becomes the 2 Hz fleet-battle tick. The determinism row lists the Go GOAMD64 v1/v3 and arm64 runs | Every new criterion has an owner WP and a phase exit. Fixes a stale 1 Hz left by fix 6.4 |
| 11.6 | 03-rendering.md (§9.6) | The round-1 ask to 06 (`PlanetWeather`, front spawner, weather gameplay, wind-emitter cues) is marked resolved, with a pointer to 06 §9.9 and §1.5 | Answered in 06 |
| 11.7 | ../PLAN.md (§6 06 summary, §9 criteria list) | 06 summary names account progression, owned NPCs, interaction, emotes, maps, weather and the FMA-safe Go interpreter. `GP-1…13` | Keeps the summary truthful |
| 11.8 | This file (§1 checks) | `GP-1…13` | New criteria |

**Out of scope here (code and CI, owned by WP-0.19 and WP-0.1):** add the `hxlfloat` analyzer and
`hxl-gen-consts` to `services/`, export `GOAMD64=v1` in the service build scripts, Dockerfiles and CI, and add
`GOAMD64=v3` and native `linux/arm64` jobs for the HXL corpus to `.github/workflows/ci.yml`.

## 12. Round-1 review fixes owned by 09 (2026-09-25)

09 changed substantively: a developer-experience track (§2.7: docs pipeline, Project Browser and New Project,
binary SDK, semver and deprecation policy, `helios-tool upgrade-project`, five starter templates) with WP-1.24,
WP-2.16, WP-3.12 and WP-4.12; a costed sponsor budget for H1–H8, agent compute and CI with funding gates F0–F4
(§4.3); mocap and facial data added to H5 with provenance rules and a procedural/audio-viseme fallback (§4.3.3,
WP-3.13); a self-hosted runner policy (§5.4a); WP-0.17's SDL3 renderer and Wayland fix with an import audit;
DoD item 9 covers docs and migrations; risks K7, K23 and K30 are tied to the budget, and K33–K36 are new
(runner exposure, funding, API churn, mocap data). K32 was already taken by privacy, so the runner risk the
reviewer called "K32" is K33. The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 12.1 | 01-vision-and-scope.md (§3.9, §4.1, §5.4) | New **AAA-TOOL-7** (documentation coverage and executable tutorials, Ph1–3), **AAA-TOOL-8** (project upgrade N → N+1 with zero manual edits, Ph2; all templates and N−2 → N, Ph4), **AAA-TOOL-9** (five starter templates proven by scripted builds, Ph3–4). Foundation layer lists the starter templates. §5.4 gates add TOOL-7 (Ph1), TOOL-7/8 (Ph2), TOOL-7/9 (Ph3) | The north star (01 §1.1) had no gate for docs, templates or upgrades; the scorecard is 01's |
| 12.2 | 02-engine-runtime.md (§3.5) | New `--emit docs` row (JSON doc model; fails on undocumented public symbols) | WP-1.24 needs a schemac emitter, which 02 owns |
| 12.3 | 07-editor-and-tools.md (§5.1, §5.5) | Delivery row "Docs, projects, templates"; the gate row adds TOOL-7/8/9 by phase; the 09 dependency bullet names the DX track | 07's phase table is the tool-side view of the same gates |
| 12.4 | ../PLAN.md (§2, §6 09 summary, §8.1, §9, §10, §12) | 63 criteria, TOOL family 9 with the new examples; "engine as a product" in the requirements table; the sponsor budget (≈ $2.2–13M) and funding gates in §8.1 and the 09 summary; 37-entry register, K1–K36 | Keeps the summary truthful |
| 12.5 | This file (§3) | SDL3 vendoring and runner-policy rows | Code follow-ups owned by WP-0.17 and WP-0.4 |
| 12.6 | README.md (section index) | The 09 row names the DX track and the sponsor budget | Index matches 09's new scope |

## 13. Round-1 review fixes owned by 07 (2026-09-25)

07 changed substantively in these places:
- **§1.6.1 (new):** multi-cell PIE. "Cells: N" runs a gateway and N cells on a debug or authored partition,
  with per-cell kill and freeze, a forced-handoff hotkey, ping-pong, trunk NetSim, the T26 authority overlay,
  an effect log with seam errors, a DAP port per cell with a zone-leader debug hold, and `.hrepro` bundles.
  A Ph2 "two zones" mode exercises the v0 handoff seam. New ED-14.
- **§1.7.1–1.7.2 (new):** Helios projects are git-first. Scale set-up (sparse index, fsmonitor, on-demand LFS),
  a synthetic 500 GB reference repository with budgets (new ED-18), and a fixed Perforce parity scope for the
  optional Ph4 adapter, promoted if ED-18 fails twice.
- **§1.8:** one session subject (`collab.<session>.tx`, one message per transaction) replaces per-document
  subjects, so multi-document transactions are atomic under the session's single-writer actor; transaction
  groups over 1 MB commit on `.ctl`; sessions rebase onto a moving `main` with the merge driver; publish is one
  linearized branch and one PR with per-commit authorship, and partial publish must be dependency-closed.
- **§2.6 (new):** physics, volume and partition authoring: `CollisionProfileDef`, `PhysicalMaterialDef`,
  `HitboxSetDef` (≤ 24 `HitZone`-tagged capsules), `RagdollDef`, `SecondaryChainDef`, `ClothDef` and
  `ZonePartitionDef`; T15's Physics Asset tab; T01's Volumes mode (portal-cell generation, grid-volume
  hysteresis visualizer and transfer probe, Partition tool); 16 `phys.*`/`zone.*` T28 rules. New ED-16 (Ph1,
  Harrow High interior) and ED-17 (Ph2, Hollow drone hit zones via NS-2.2's harness).
- **§4.4 (new):** `helios-uitest`: item table, `ui.*` input injection over the remote-control socket, ꟻLIP
  goldens of every tool at 100/200 % in dark and high-contrast, layout lints, dual-path scenarios and nightly
  UI-level replays of ED-2, 7, 10, 16 and 17. Dear ImGui Test Engine is not vendored (licence). New ED-15.
- **§5:** delivery rows for T01, T15 and the framework; ED-14…18; four risks; traceability to 04, 06 and
  R08 T15; dependency bullets.

| # | File | What changed | Why |
|---|---|---|---|
| 13.1 | 02-engine-runtime.md (§5.1 table, §5.5, §7.1, §8.1) | Portal cells gain a `roofed` flag and a "Source" row (T01 Volumes mode); `ownedRegions` is built from `ZonePartitionDef` with `regionAt()`; §7.1 names the authoring records, hitbox capsules come from `HitboxSetDef`, and a client-only cosmetic `PhysicsSystem` hosts ragdolls (Ph2) and cloth; the cloth decision (Ph2 bone chains, Ph4 Jolt soft bodies with skinned constraints, no GPU cloth) with budgets; ladder rows updated | 06 §9.9 already used a "roofed" flag; 03 §5.6a asked for "02's CPU cloth", which had no decision; v1 partitions had no authored source; ragdolls must not perturb bit-identical hull prediction |
| 13.2 | 00-decisions.md (ADR-013 Physics notes) | Ragdolls and cloth are client-only in a cosmetic `PhysicsSystem`; cloth is Jolt soft bodies in Ph4, bone chains before; no GPU cloth | The reviewer asked for the cloth decision in 00/02. It extends ADR-013 without changing a choice |
| 13.3 | 04-networking-and-servers.md (§10.3) | Dev-only multi-cell PIE hooks: `DevForceHandoff`, zone-leader debug hold, `DebugPaused` exemption from suspect/failure detection, trunk NetSim; per-cell replay | 07 §1.6.1 needs these; compiled out of shipping builds |
| 13.4 | 05-backend-services.md (§1.4.2) | `CreateInstance` creates one `region_lease` per `ZonePartitionDef` region of the pinned build | Regions had no stated source |
| 13.5 | 09-roadmap-and-process.md (WP-0.18, 1.17, 1.19, 2.11, 2.12, 3.7, 4.7; Ph1–3 exits; §6 table) | `helios-uitest` in WP-0.18; Volumes mode and UI goldens in WP-1.17; fit-hitboxes in WP-1.19; Physics Asset tab in WP-2.11; LFS scale and two-zone PIE in WP-2.12; multi-cell PIE, collab rebase/publish and Partition tool in WP-3.7 (now depends on 3.1); optional Perforce in WP-4.7. Exits add ED-15 (Ph1), 16; ED-15 (Ph2), 17, 18; ED-14, 15 (Ph3). New "Editor UI" and "Source-control scale" test rows | Every new criterion has an owner WP and a phase exit |
| 13.6 | ../PLAN.md (§6 07 summary, §9 criteria list) | PIE on N cells, scale budgets, atomic collab publish, physics and volume authoring, UI harness; `ED-1…18` | Keeps the summary truthful |
| 13.7 | This file (§1 checks) | ED count 13 → 18 | New criteria |

**Manifest ask (WP-2.11):** vendor V-HACD 4 (BSD-3-Clause, header-only, tools only) in `third_party/MANIFEST.md`.

## 14. Integration pass 2: round-1 revisions and the PLAN.md review gap (2026-09-25)

Inputs: a full re-read of PLAN.md, 00–09, README and this log after every section's round-1 revision, plus
one reviewer finding filed against PLAN.md itself: the Star Citizen caveat in PLAN.md §7 and §4 above was
factually wrong, and the Phase 4 SC bar was weaker than claimed (AAA-SRV-9 Ph4 lost 30 s of state, and hot
standby covered only "hot zones"). **Decision: option (a)**, recorded in ADR-007. A minimal stateful
replication tier (replicants) arrives in Phase 4 for every multi-cell zone, AAA-SRV-9 Ph4 becomes ≤ 1 s there,
and the Improbable review moves ahead of WP-4.3. Option (b), a mandatory hot standby with the tier left in
Phase 5, was rejected in ADR-007.

**Replicant tier (the reviewer gap).**

| # | File | What changed | Why |
|---|---|---|---|
| 14.1 | 00-decisions.md (ADR-007) | Roadmap v0 → v1 → **v1.5 (Phase 4) replicant tier** → v2. A new bullet specifies the tier (the cell binary in `--replicant` mode, one per ≤ 4 cells on another host, fed by the existing serialize-once chunks of every audience, fenced by `(region, lease_gen)` and epoch, no authority, no clients, no persistence writes). It records why (SC's shipped 3.18/4.0 state; the loop stops at Phase 4), the scope difference from SC (client replication stays in cells until v2), the rejected alternatives ((b) mandatory hot standby; state in gateways) and the legal gate | Reviewer finding; one decision owner |
| 14.2 | 01-vision-and-scope.md (§2 d, §2.6 S02, §3.4 SRV-9 and a new measurement rule, §5.2, §5.4 Ph4, §6) | (d) gains SC's shipped state and the Helios form. S02 becomes "…; replicant crash recovery", Ph "3 (replicants: 4)". SRV-9 is split into multi-cell / other zones: Ph4 **1 s** / 30 s, Ph5 1 s / 10 s, with a rule that defines the measured rollback. The Improbable review is before WP-4.3, with a follow-up before WP-5.2. The Ph4 done-means adds the Harrow-orbit cell kill. The glossary adds **Replicant** | 01 owns the scorecard and the patent schedule |
| 14.3 | 04-networking-and-servers.md (§1 process and deployment tables, §6.4, §6.5, §10.3, §11.1, §11.3, §11.4, §11.5) | A Replicant process row. §6.4 replaces the one-line "hot standby (Phase 4, hot zones), 2 Hz" with the **replicant tier**: process and placement, stream (≤ 20 Mbit/s and ≤ 0.3 ms per 500-player cell), fenced state, recovery from RAM (step 3 pulls from the replicant, with a per-AG checkpoint fallback), replicant failure and scope. §6.5 adds stage **v1.5** and says v2 builds on the tier; the R04-P0-7 note now states SC's shipped state correctly. New metric `replicant_lag_ms`, `--replicant` in the layout, ladder rows, the appended criterion **NS-4.6** and a risk row | The reviewer's option (a); new criteria are appended, never renumbered |
| 14.4 | 05-backend-services.md (§1.4 API, §1.4.6, §6.4, §6.7, §6.8, §9, §13) | `AssignReplicant` (Phase 4). Recovery restores from the replicant in multi-cell zones, else from checkpoints. The capacity assumptions add one 4-vCPU replicant per 4 cells. The DR row and the cell-crash runbook mention replicants, and a replicant-crash runbook row is added. Ladder: replicant placement (orchestrator), and "hot standby (04)" → the replicant tier (persistence) | 05 owns placement and runbooks |
| 14.5 | 06-gameplay-framework.md (§7.3 Recovery, GP-10a, GP-11a) | Followers restore from the same checkpoint *or replicant state* as their owner; counter loss is bounded by the owner's state-loss window (AAA-SRV-9) | Both texts assumed checkpoint-only recovery |
| 14.6 | 07-editor-and-tools.md (§1.6.1 Kill cell k) | From Ph4 PIE starts a replicant, so the standby restores from it, and *Kill replicant* exercises the resync | The seam debugger should cover the Phase 4 recovery path |
| 14.7 | 09-roadmap-and-process.md (M4, WP-4.3, WP-5.2, Ph4 exit, §3.1, §3.2 #21, §4.3, K18) | WP-4.3 becomes "Battle scale, replicant tier", depends on the Improbable review and carries NS-4.6 and AAA-SRV-9 (Ph4); the Ph4 exit lists NS-4.1–4.6 and SRV-9 ≤ 1 s. WP-5.2 builds on the tier and needs a patent follow-up. H8 is needed in Ph3; its budget row becomes Ph3 25–70k (the Improbable review moved from the Phase 5 extras, which keep a $5–15k follow-up), so Ph3 totals 0.71–4.19M and Ph0–4 ≈ $2.2–13.1M. K18 covers both tiers, with a design-around. Reconciliation row 21 records the change | Every new criterion has an owner WP and a phase exit; the moved review has an envelope and a gate (F2) |
| 14.8 | README.md (phase vocabulary) | Ph4 adds the replicant tier; Ph5 says the gateway layer sits on it | Single phase vocabulary |
| 14.9 | ../PLAN.md (§1, §2, §3 R04, §4.1, §5 ADR-007, §7, §8.1, §9, §10, §12) | The corrected SC caveat (SC's shipped replication layer and static meshing; the replicant tier; the one structural difference; the patent schedule); S02 row and the SC proof row; topology stage v1.5 and replicants in the process diagram; SRV row; K17/K18 row | The reviewer asked for PLAN.md §7 and this log's §4 to be corrected |

**New cross-section inconsistencies found in the re-read.**

| # | File | What changed | Why |
|---|---|---|---|
| 14.10 | 00-decisions.md (ADR-004) | "entity IDs Snowflake-style" → time-prefixed (Snowflake-style) 63-bit IDs from PG-allocated blocks, 41/5/17, with no node IDs | 05 §1.4.5 and 02 §4.1 moved to block IDs in round 1 (fix 7.2), but the binding ADR still implied Snowflake node IDs |
| 14.11 | 02-engine-runtime.md (§1.1, §1.3) | New L2 HEADLESS module **`replay`** (`SimInbox`, recorder, log format, replayer driver; state hashes supplied by `replication`); the cell target notes `--replicant` | 04 §11.1 lists `engine/replay/` since its round-1 replay contract, but the CI-enforced layering DAG had no such module |
| 14.12 | 04, 07, 08 headers | 04: "Draft" → "Draft v2", ADR-011–015 → 011–016 (its dev-only PIE hooks cite ADR-016). 07: adds ADR-016 (game-DLL reload, fix 8.7). 08: adds ADR-015 (voice settings, fix 5.6) | Conformance lists lagged the round-1 edits |
| 14.13 | 09-roadmap-and-process.md (header) | `NS-p.k` is "the row with that ID in 04 §11.4", not "clause k split at semicolons" | 04 §11.4 has listed one ID per row since round 1 |
| 14.14 | 09-roadmap-and-process.md (§4.3 budget) | Ph0 total 16–93k → 16–89k | Arithmetic: the Ph0 rows sum to 16–89k (C1 9–54k + C2 7–35k) |
| 14.15 | ../PLAN.md | Status v1 → v2; "60 measurable criteria" → 63; 02 summary "17 acceptance criteria" → 19 and §9 "RT-01…17" → "RT-01…19"; 04 summary "eight Helios channels" → nine (VOICE is channel 8); §4.2 adds `replay` (L2), `voice` (L3), `helios-voice` (L5) and voice to the HEADLESS exclusion list, and a voice-forwarder row; the diagram adds the voice forwarder; §1 and §11 note the in-progress RHI, `reflect`, ECS and schemac in the working tree (09 §8.1); the 02, 04 and 08 summaries name the round-1 additions (ADR-016 link model, three-thread client, bubble seams RT-19, fuel-metered Luau; zone leader, replay contract, voice; the CL-6 input-latency budget); §8.1 gates add RT-13/18 and F0 (Ph0), TOOL-7 (Ph1), TOOL-7/8 and CL-6 (Ph2), TOOL-7/9 (Ph3); §9 adds the funding gate to phase completion and tutorials and spend to the round audit | Summaries had not caught up with 01, 02, 04, 08 and 09 after round 1 |
| 14.16 | This file (§1, §3, §4) | NS counts 7/5/7/9/6/4; `replay` and `voice` join the module-order follow-up; the §4 SC variance is corrected | Keeps the log truthful |

**Out of scope here (code, owned by WP-0.2 and WP-4.3):** add `replay` and `voice` to `HELIOS_MODULE_ORDER` and
the layer checks (WP-0.2); implement `--replicant` and `AssignReplicant` (WP-4.3).

## 15. Round-2 review fixes owned by 04 (2026-09-25)

Three backend-lens gaps in 04: v1 cross-cell visibility was undefined and unbudgeted (major), AGs had no
dormancy (minor), and a gateway-box failure at Phase 4 scale was unsized and untested (minor).

| # | File | What changed | Why |
|---|---|---|---|
| 15.1 | 04-networking-and-servers.md (§1, §2.5, §2.6, §3.3, §4.3, §4.7, §6.2, §6.3, §6.4 step 5, §6.5, **new §6.6**, §10.3, §11.2–11.5) | **v1 view composition.** A cell replicates only the entities it owns, to local sessions and to *remote viewers* the gateway registers from the home cell's 4 Hz `ViewerState` (viewpoint, instance, layer, `phase_mask`, party and fleet, sensor class, relations), the zone leader's `RegionMap` and the cells' class and party presence, with a hash-level mask. Ghosts are never sent to clients. There is one source per NetHandle per tick (the epoch rule plus the client's per-handle tick rule). A handoff moves per-session resync records (no creates or full-state bursts), and per-(session, cell) state stays put when the viewer's own group hands off. The gateway arbitrates each session's budget by weighted water-filling over the cells' `Demand` reports, with grants applied the next tick and tick-aligned packing. §3.3 has a stage 7 cost model with remote-viewer rows (Harrow orbit ≈ 1.5 ms; a 10-cell, 5,000-player zone ≈ 2.3 ms against 3 ms) and a far-broadcast fallback; §4.7 has trunk rows. New **NS-3.10** (boundary observer, 0 pops, near-tier rate across the seam, SRV-6 downstream, stage 7 budget; Ph4 rerun) | Reviewer: the Star Citizen-class seam had no cross-cell client replication design, no cost model and no criterion |
| 15.2 | 04 (§1, §2.4, §6.1, §6.4 step 2, §7, §10.3, §11.2, §11.4 NS-1.5, NS-2.7) | **AG dormancy.** `Fence.Park(root, e, cell, ckpt_seq)` runs after the final checkpoint on logout, linkdead expiry (5 min), instance teardown, structure or interior unload, or when a ship is stowed. It sets NULL owner, `e+1`, `state = dormant` and `parked_seq`. Recovery matches active rows only; a load accepts dormant rows, and taking over an active row is flagged and triggers `ctl.<shard>.cell.<id>.released`. Dormant rows make custody at rest, so service ledger ops use a dormant-fence precondition. The checkpoint-rejection alert fires only on owner-changing or zombie staleness. The custody audit is restated for active and dormant rows, with an orphan check. NS-1.5 adds relog storms, and NS-2.7 recovers with ≥ 50k parked AGs in the region | Reviewer: offline characters were resurrected by recovery, at-rest custody was undefined, the audit failed for every logged-out player, and superseded owners were never told |
| 15.3 | 04 (§1 deployment, §2.4, §2.6, §6.4, §11.3–11.5) | **Gateway-box failure.** Free slots on other boxes ≥ 1.2 × the most loaded box's sessions, at least half of them in another availability zone (8 boxes at 50k CCU). Clients add 0–2 s full jitter before reconnecting; the Session service must absorb a 10k-ticket burst in ≤ 5 s at p99 < 250 ms, and reconnects bypass admission. New **NS-4.7**: kill a full box at 50k CCU, ≥ 99 % back ≤ 10 s, 0 value errors, no admission pause | Reviewer: an 8k-session box loss was unsized; the only gateway-kill test was STB-5 at 5k CCU |
| 15.4 | 05-backend-services.md (§1.3, §1.13, §3.2, §4.1, §6.4, §6.6 step 4) | Session reconnect burst target (NS-4.7). Fence gains dormancy (`Park`, `state`, `parked_seq`, a partial `owner_region` index over active rows, `released`). The ledger accepts service ops on dormant-fenced items; erasure step 4 waits for its AGs to park. The capacity assumptions add gateway boxes with N+1 headroom | 05 owns the Session service, fence schema, ledger check, erasure saga and capacity model; each edit restates 04's design |
| 15.5 | 01-vision-and-scope.md (§3.2 BENCH-6) | From Ph3, BENCH-6 includes a boundary observer watching the Mule cross a cell boundary (NS-3.10) | The reviewer asked for the observer in BENCH-6; 01 owns the BENCH table |
| 15.6 | 09-roadmap-and-process.md (WP-1.13, WP-3.1, WP-4.3, Ph3 and Ph4 exits) | WP-1.13 builds dormancy. WP-3.1 builds view composition and the observer bot, and carries NS-3.10. WP-4.3 builds box headroom, the reconnect-storm path and the far-broadcast fallback, and carries NS-4.7 and the NS-3.10 Ph4 rerun. The exits become NS-3.1–3.10 and NS-4.1–4.7 | Every new criterion has an owner WP and a phase exit |
| 15.7 | 08-client-and-launcher.md (§1.2 Reconnecting row) | Reconnect retries start after a 0–2 s uniform jitter, and each backoff step is ±50 % | 04 §2.4's reconnect-storm rule is client behaviour |
| 15.8 | This file (§1) | NS counts 7/5/7/10/7/4 | New criteria NS-3.10 and NS-4.7 |
| 15.9 | ../PLAN.md (§4 topology stages, 04 summary) | v1 names gateway view composition (NS-3.10); the 04 summary adds dormant AGs and cross-cell view composition | Keeps the summary truthful |


## 16. Round-2 review fixes owned by 05 (2026-09-25)

Three backend-lens gaps in 05:
- **Major:** a host or rack loss tripped the correlated-silence guard and froze the shard. The exit rule could
  never be met, and reassignment waited on a placer signal that Kubernetes gives minutes late and
  `helios-agent` never gives for a dead VM.
- **Minor:** content-version pinning conflicted with server-only hotfixes and staged client builds.
- **Minor:** order insert, modify and cancel were not owner-fenced.

05 changed as follows:
- §1.4 API: `RegisterProcess` carries a failure domain; `ReportSuspect` lists silent and live trunk peers;
  new `ProbeCell`.
- §1.4.3: failure domains, a blast radius per phase (host, then rack from Ph4), R redefined as NATS ping
  **and** trunk probe, a NATS-isolated state, and a four-check silence classifier (domain loss, wide loss,
  control-plane fault).
- §1.4.4: a domain loss never enters degraded mode; detection keeps running while degraded; the exit ratio
  counts only processes not confirmed dead.
- §1.4.6: Distributed scheduling and spread constraints; a control-plane node pool; a warm pool sized to the
  worst domain (Ph3 ≈ 8 slots / 30 %, Ph4 ≈ 32 slots / 13 %); rack-disjoint replicants; parallel recovery
  budgets; a poison-build brake.
- §1.7: every order write is fenced and written only by the owning actor.
- New §1.14.1: client part, server part and compat epoch; four release kinds; the coordinated epoch flip.
- Supporting edits: §1.2, §1.3, §3.2 `svc_orch.process`, §6.1–6.4, §6.7, §6.8 runbooks, §7 pointer and
  pinning, §8 tests, §9 ladder, §11 risks, §12, §13.
- Criteria: A7 gains order traffic during takeovers; A14 gains a Ph4 live-compatibility clause; A15 gains
  host-loss and NATS-only-partition drills; A19's AZ-loss clause covers cells.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 16.1 | 04-networking-and-servers.md (§2.3) | `protocol_id` derives from the protocol version and the compat epoch; the token's user data carries the content pin `(compat_epoch, manifest_hash)` | Tokens pin only the client part and epoch (05 §1.14.1) |
| 16.2 | 04 (§6.4 detection, warm standby) | R needs the NATS ping **and** a gateway trunk probe to fail; a trunk-only answer is NATS-isolated, not dead. Correlated silence within one host (Ph4: rack) is a domain loss, recovered at once. The Ph3+ warm pool is sized to the largest domain (05 §1.4.6) | The old text implied a host loss enters degraded mode |
| 16.3 | 04 (§7 content-version pinning) | Placement matches the compat epoch, not the client build. Same-epoch server hotfixes swap in running cells; new server binaries roll open-world zones in place; only instanced content starts on a new release; an epoch change is a coordinated flip | "Rollouts start new instances on N+1" duplicated single-shard zones by version |
| 16.4 | 04 (§11.4 NS-4.1) | Appended the Ph4 failure-domain clause: power off a SERVER host with ≥ 8 cells and partition a SERVER rack at its ToR under the NS-4.1 load. Every region served ≤ 10 s after confirmation, no degraded episode > 5 s, no admission pause, 0 value errors | The reviewer asked for a Phase 4 NS clause. Appended to an existing row, so no ID was renumbered and 09's mapping holds |
| 16.5 | 01-vision-and-scope.md (AAA-ITR-8, AAA-STB-6) | Both add "no client disconnected or re-placed and no zone duplicated by version" with a same-epoch client build staged, measured by BE-A14 (Ph4) | The reviewer asked for the test in ITR-8 and STB-6. 01 owns the rows; the edit tightens the measurement only |
| 16.6 | 02-engine-runtime.md (§5.5 Pinning) | A ZoneInstance pins a compat epoch for life; its server part can hot-swap at a tick boundary | "Pins one content version" contradicted live server hotfixes |
| 16.7 | 08-client-and-launcher.md (§2.6 Pre-download) | `next` carries `compat_epoch`. On an epoch flip the client is disconnected with `CONTENT_EPOCH_FLIP` and a reconnect ticket, applies the pre-downloaded build and rejoins without queueing. Same-epoch builds never interrupt a session | The client side of 05 §1.14.1's coordinated flip |
| 16.8 | 09-roadmap-and-process.md (WP-2.6, WP-3.3, WP-4.4, Ph4 exit) | WP-2.6 adds failure-domain registration and the classifier. WP-3.3 adds host-loss chaos, node pools and the domain-sized pool. WP-4.4 adds compat epochs, staged server-part hotfixes, the rack blast radius and BE-A14 (Ph4). The Ph4 exit adds BE-A14 (Ph4) | Every new clause has an owner WP and a phase exit. A15's Ph3 clause is already in the Ph3 exit, and its Ph4 clause runs as NS-4.1 |
| 16.9 | ../PLAN.md (05 summary) | Failure-domain classification and pool sizing; fenced order writes; client and server content parts under a compat epoch | Keeps the summary truthful |

No criterion IDs were added. The §1 counts are unchanged.

## 17. Round-2 review fixes owned by 01 (2026-09-25)

Three minor gaps in 01, one from each of the backend, engine and tools lenses:
- **BENCH-3 meant two things.** It was defined as 1,000 ships, while PLAN.md §7/§8.1 and 09's M4 presented it as the
  2,000-ship battle, and 09's K28 treated it as a server tick measurement.
- **Nothing judged look and feel.** No criterion asked whether Helios looks or feels at the reference class's level;
  09's H6 "feel reviews" had no criterion behind them.
- **No production-scale dogfooding.** No content team ever used the 30 tools at production scale.

01 changed as follows:
- **§3 intro.** It now states the evidence classes (W, M) and marks human-judged criteria **(M)**: TOOL-5, SEC-8 and the
  two new criteria.
- **§3.2 BENCH-3.** It is now the client view of the AAA-SRV-10 / NS-4.2 battle: 2,000 ships, 20 capitals (1 %, as in
  SRV-10), 2,000 brackets and 400k active particles, rising to 3,000 ships in Ph5 as 09 M5 already said.
  - It replays a recorded, tick-stamped gateway capture nightly from Phase 2, and is measured live at the Ph4 exit.
  - Only the server gate is staged by phase.
- **§3.3 AAA-REN-1.** BENCH-3 gains p99 ≤ 33 ms.
- **New AAA-REN-8 (M; Ph1, Ph3, Ph4) and §3.3.1.** A blinded external panel (≥ 3 reviewers at Ph1, ≥ 5 at Ph3 and Ph4,
  under H6) scores fixed captures and a hands-on feel session against current reference titles.
  - Rubric: 14 items (L1–L8 look, F1–F6 feel).
  - Pass: every median ≥ 2.5 at Ph1; ≥ 3.0 at Ph3 (L5 ≥ 2.5); at Ph4 ≥ 3.0 on every item, a mean ≥ 3.3 and ≥ 80 %
    "could ship".
  - Every failing item, and every score ≤ 2, is routed to a named WP.
- **§3.4 SRV-10.** It names BENCH-3 as its client side, and defines K28's half-load early warning: tick p99 > 2.25 s at
  `d` = 0.1, or growth > 2.5× from 500 to 1,000 ships.
- **New AAA-TOOL-10 (M; Ph4) and §3.9.1.** A contracted content team of 3–5 people with no engine engineer builds a new
  zone in ≤ 10 working days from the SDK and docs alone.
  - The zone: terrain, a settlement, spawns, a 5-quest chain with placeholder VO, a cinematic, a UI screen and a ship
    variant.
  - Pass: 0 blocking issues, 0 engineer interventions, 0 engine or backend source edits, ≤ 10 major defects.
  - It adds a Ph3 rehearsal and a nightly UI-provenance report from journal `origin` tags. That report gates a
    ≥ 50 human-`ui`-transactions floor per tool and shows the human-UI share of *Cinder Reach* Ph3+ transactions to
    the auditors.
- **Elsewhere in 01.** §5.3 gains path item 7. §5.4 adds REN-8 and TOOL-10 to the phase gates. The glossary gains
  evidence class, look-and-feel panel and transaction origin. §7 traceability is updated.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 17.1 | 03-rendering.md (§2 GPU scene, §3.4, §6.1, §8.1, §9.2, §9.3) | §8.1's REF BENCH-3 column is rescaled to 2,000 ships and 20 capitals: cull 1.4, prepass 2.0, shadows 2.5, forward 4.5, transparents and particles 3.8, UI 0.8. The sum is 19.9 ms, ~18.4 ms wall, inside the 22.2 ms (45 fps) frame. Fleet geometry ≤ 4.5 ms with ≤ 250 mesh-band hulls; 400k active particles (MIN keeps its top 256k); the transform upload is ~200 KB. RC-5 render CPU ≤ 5 ms (was 4); RC-8 adds BENCH-3 p99 ≤ 33 ms; the Ph4 VFX ladder cell reads 400k | BENCH-3 is now the full 2,000-ship battle |
| 17.2 | 08-client-and-launcher.md (§4.4 CL-7) | BENCH-3: 2,000 brackets ≤ 1.0 ms CPU (was 1,000 ≤ 0.6 ms) | Same |
| 17.3 | 07-editor-and-tools.md (§1.2, §2 T17 note, §5.1 gate row) | `Transaction` gains `origin`, stamped by ToolsFramework from the input path (`ui`, `ui-scripted`, `luau`, `rpc`, `cli`, `import`, `collab`). T17's budgets cite 2,000-ship battles. The Ph4 gate lists TOOL-10 | TOOL-10's provenance report needs the tag, and 07 owns the transaction format |
| 17.4 | 09-roadmap-and-process.md (Ph1, Ph3 and Ph4 exits; WP-1.22, 2.15, 3.9, 4.10, 4.7; §4.2; H6 row; budget table; F3; §5.6; §5.7; K28) | REN-8 is added to the three exits, with the content WPs owning the capture set and panel. WP-2.15 adds the 1k-ship half-load run and the first 2,000-ship BENCH-3 capture; WP-4.10 replaces it with the *Bastion* battle. TOOL-10 is added to WP-4.7 and the Ph4 exit. The H6 row and budget are costed: Ph1 5–11k, Ph3 37–76k, Ph4 89–194k, H6 total 138–295k. Phase totals become 0.17–1.09M, 0.72–4.22M and 0.95–5.51M, the grand total ≈ $2.2–13.2M, and the C1/H5 shares 49–55 % / 28–34 %. F3 names the panel and the team. The §5.6 M class lists REN-8 and TOOL-10, and the §5.7 rubric shows the UI share. K28's trigger is the NS-4.2 half-load server run, not BENCH-3 | Every new criterion needs an owner, an exit, a funded supplier and a gate. K28 now measures the server |
| 17.5 | ../PLAN.md (§1, §2, §6 01 summary, §9) | 63 → 65 criteria; the REN family is now 8 and TOOL 10, with examples; §2's editor and AAA rows name TOOL-10 and REN-8; the REN example says BENCH-3 is the full 2,000-ship battle | Keeps the summary truthful. PLAN.md §7/§8.1's "2,000-ship battle (BENCH-3 ≥ 45 fps)" is now literally true, so it is unchanged |

Two criterion IDs were added (AAA-REN-8, AAA-TOOL-10), so the scorecard has 65 criteria. No RT, RC, ED, CL, NS, BE or
GP IDs were added or renumbered.

## 18. Round-2 review fixes owned by 03 (2026-09-25)

Four minor engine-lens gaps in 03 were closed inside 03:
- **§1.3a:** streaming feedback is keyed by the stable `StreamTexId` in u32 arrays (`fbMip`, `fbCover`), not
  by bindless slot in u8. Touched entries are compacted into a ≤ 128 KB readback list. Mesh `desiredLod` is
  u32 per `GpuMesh` too.
- **New §5.4a:** per-tile horizon maps give far terrain shadows, blended with CSM by `min`, and cover the
  orbit path and froxels. Generation uses only the unused share of the 0.8 ms tile slice, ≤ 0.15 ms;
  lookup costs ≤ 0.05 ms. §4.5 adds `csmFar` per preset.
- **§5.8:** cloud fast mode, with its own §8.1 line (≤ 1.0 ms that replaces the 1.5 ms line, never adds to
  it). It traces 1/8 resolution per axis with 20 steps and one multi-scatter octave, and switches to the 2D
  shell above cloud top + 2 km.
- **New §7.6a:** character composites are tiered by 02's A0–A3 animation-LOD tiers, encoded on the GPU as
  BC7/BC1/BC5, and held in an LRU cache capped per preset (REF 320 MB, MIN 96 MB) inside the texture pool.
  A3 uses crowd-class impostors tinted per character. Worst-case BENCH-1 is 285 MB on REF and 71 MB on MIN,
  against ≈ 4.2 GB naive.

Criteria: RC-8 gates BENCH-2's in-layer segment at 1,500 m/s. RC-10 adds BENCH-1 on MIN with 200 unique
appearances. RC-11 adds Ph1 sunset goldens at 5 km and 50 km and Ph4 cloud fast mode. §8.1 REF BENCH-5's
async row goes from 0.7 to 0.8 (composites), so its sum is 15.8 ms; wall time is unchanged because the work
is async. §8.2 gains three rows: horizon-map resolution, fast-mode steps and composite tiers with their cache
caps. §9.2 gains a Characters row. The smallest consistent edits elsewhere are below.

| # | File | What changed | Why |
|---|---|---|---|
| 18.1 | 09-roadmap-and-process.md (WP-1.8, WP-2.3, WP-3.5) | WP-1.8 adds horizon-map far terrain shadows and RC-11 (Ph1 sunset goldens). WP-2.3 adds character composite tiers, the cache and the GPU BC encoders. WP-3.5 adds cloud fast mode and generic crowd impostors | Every new 03 feature has an owning WP in the phase where 03 §9.2 lands it |
| 18.2 | 07-editor-and-tools.md (T22) | Crowd-bake settings assign each outfit `MeshOption` a silhouette class (≤ 8 per body type) and map `PaletteColor` parameters to ≤ 6 impostor tint slots. The bake produces 03 §7.6a's crowd-class atlases, and the stress grid runs through the runtime BC encoders | 03 §7.6a's A3 impostors depend on T22's bake data |

02 §7.2 (A0–A3 tiers, governor caps, A3's `{impostorPose, heading, phase}`) and 06 §10 (`CharacterAppearance`
parameter kinds) are consumed as written, so neither changed. No criterion IDs were added or renumbered.

## 19. Round-2 review fixes owned by 00 (2026-09-25)

One minor engine-lens gap in 00 was closed. ADR-001 named MSVC/VS 2022 as primary, but `windows-latest` already
builds with VS 2026 (MSVC 14.51), so no job tested the toolset called primary. MSVC's rule that the final linker
must be at least as new as every linked library also meant that a v145-built SDK could not be used from VS 2022,
and the plan never said which toolset builds the SDK.

**New ADR-001a (Windows toolset policy).**
- **Roles.** VS 2026 (v145, unpinned newest update on `windows-latest`) is the primary. VS 2022 17.14 (MSVC 14.44
  exactly, on a pinned `windows-2022` job) is the floor, and it is also the **release toolset**: it builds the
  SDK's static and import libraries and every released Windows binary, and nightly BENCH runs use its builds.
- **Rules.**
  - Consumers may use 14.44 to the newest v145; `HeliosConfig.cmake` and `sdk_config.h` give a clear error below
    19.44/1944.
  - No `/GL` in SDK libraries, checked on the Rich header and `dumpbin` at release.
  - The developer SDK uses the system VC++ runtime. The packaged artist editor carries app-local runtime DLLs
    from the newer of the SDK's validated redistributable and the project toolset's.
  - SDK libraries are IDL 0 with `/MT` and `/MD` flavours. Projects get a `DebugGame` configuration.
  - The IDE presets `windows-vs2026` and `windows-vs2022` become modular in WP-0.6c.
  - The floor moves only by an ADR amendment with two minor releases' notice. v145 becomes the floor at 1.0, or
    earlier on a missing feature or the retirement of the runner image.
  - "Five compilers" counts compiler families, and MSVC means both jobs.
  - Linux has the same rule: release builds in the sniper container with GCC 13.
- **Matrix and acceptance.** The PR matrix has 8 named jobs. AAA-PLT-1 needs all of them green. A version-gate
  script test and a nightly `sdk-consumer` job (WP-2.16) build `starter-blank` with a C++ game module on both
  toolsets.
- **Tidy-up.** ADRs are now in numeric order (012 before 013 and 014), and the file opens with an index table.
  No ADR number changed.

The smallest consistent edits elsewhere are below.

| # | File | What changed | Why |
|---|---|---|---|
| 19.1 | 01-vision-and-scope.md (§1.2, AAA-PLT-1) | §1.2: programmers use VS 2026, or VS 2022 17.14 as the floor. AAA-PLT-1 names both MSVC toolsets (the floor builds the SDK and releases), clang-cl, GCC 13, Clang 17 and MinGW as the 8-job PR matrix | The criterion must test the toolset the plan calls primary, and the one that ships |
| 19.2 | 02-engine-runtime.md (§1.4 link-flavour table) | The shipping column's "optional LTCG" excludes SDK static libraries. The dev column's presets add `windows-vs2026`/`windows-vs2022` | ADR-001a rules 2 and 5 |
| 19.3 | 06-gameplay-framework.md (§1.2 float rules) | "MSVC 2022 contracts only under `/fp:contract`" now covers VS 2022 and VS 2026 | Both toolsets build HXL units |
| 19.4 | 09-roadmap-and-process.md (WP-0.1, §2.7.2, §5.3 DoD 3, §5.4, §5.6 PR tier, §5.9, §8.1 presets and CI rows) | WP-0.1 delivers the floor job and nightly MSBuild jobs, and its acceptance is all 8 PR jobs green. The SDK is built with 14.44 and no `/GL`, with `DebugGame`, and `sdk-consumer` proves both toolsets. DoD 3 names both MSVCs. The PR tier is the 8-configuration ADR-001a matrix (it said 6). §5.9 installs VS 2026 or VS 2022 17.14+. There, `validate.ps1` accepts compiler 19.44 or 19.5x, enters a dev shell through `vswhere` when needed, records the toolset, and uses the `windows-vs2026` IDE preset. §8.1 records that `windows-latest` is VS 2026 and that there is no floor job yet | The user validates on whichever Visual Studio they installed; the SDK remains consumable from both |
| 19.5 | ../PLAN.md (§2 platform row, §5 ADR-001 row, §11) | VS 2026 primary and the VS 2022 17.14 floor/release toolset, with the reason. §11 installs either version, uses `windows-vs2026` (CMake ≥ 4.2), and states that CI now runs VS 2026 and that WP-0.1 adds the floor job | Keeps the summary and the build instructions truthful |

No criterion IDs were added or renumbered. PLAN.md §8.2's "warning-clean on all five compilers" is consistent
as written, because it counts compiler families (ADR-001a rule 7).


## 20. Round-2 review fixes owned by 02 (2026-09-25)

Three minor engine-lens gaps in 02 were closed.
- **§1.1 and §5.8, the pcg AVX2 kernels.** The AVX2 containment rule becomes an **ISA allowlist** with two per-file flag
  entries, `tp_jolt` and `engine/pcg/src/kernels/*_avx2.cpp`. It also lists by symbol the CPUID-guarded
  functions of self-dispatching third-party code (zstd, meshoptimizer). The pcg VM's op bodies are written once against
  `Lanes<W>` and compiled at three widths: AVX2 8-lane (budgeted), SSE4.2 4-lane (a conformance twin, never
  budgeted) and scalar (the reference). The kernel TUs are integer-only. `pcg`'s explicit module entry point
  picks the width by CPUID after `core::cpuGate()`. The kernel TUs have no dynamic initializers and export no
  weak or COMDAT symbols. The WP-0.2 audit checks flags, disassembly and those two properties. Any MIN, REF or
  SERVER bench that did not log `pcg.kernel=avx2` fails, so a silent 4-lane run cannot trip 03 §5.5a's F3
  threshold. RT-04 hashes all three widths, and RT-09 names the allowlist.
- **§2.3 and §2.4, MIN CPU budgets.** Worker count is `min(8, logical − 2)`, so the SMT-less i5-9400F gets 4
  workers; the lab measures it as a Ryzen 5 3600 with SMT off. §2.4 has a per-thread table and a per-system
  core-ms table for REF and for MIN BENCH-1, 2 and 4. On MIN the game thread gets 10.5, 9.5 and 11 ms, the
  render thread 6 ms, and the whole process 45, 45 and 50 core-ms of 100. The per-system rows cover animation,
  physics, cosmetic physics, UI, render jobs, streaming decode, terrain (including the F3 fallback), audio,
  net and other jobs. MIN `cpu.*` settings are defined, and a CPU-bound frame is defined. **RT-12 gains an
  H-class MIN clause**, tracked nightly on BENCH-2 from Ph1, which gates at the Ph3 exit.
- **§3 and §7.4, binding fuel.** A new `scriptlib` declaration kind holds `fn` signatures with
  `@script(cost=n[, each=m, of=result|<arg>])` and `@pure`. The generated glue charges fuel per call, and
  length-dependent builtins get charging wrappers. Scripts are compiled with `disabledBuiltins` so no call
  bypasses a wrapper through `FASTCALL`. `--calibrate-fuel` writes `content/profiles/fuel_costs.jsonc`, which
  goes into the zone profile and, in full, into the replay header. `fuel_kill` = min(≈ 5 ms, 25 % of the
  tick). The lane bound (`fuel_per_tick` + one resume) is stated, as is the stage 4 tick allowance. RT-13
  gains a binding-heavy case.

| # | File | What changed | Why |
|---|---|---|---|
| 20.1 | 03-rendering.md (§5.5a cost model and spike CPU bullet, §8.1 CPU budgets line) | The 8-lane CPU VM is named as `pcg`'s allowlisted `vm_avx2.cpp`, with the SSE4.2 twin never budgeted. A spike run without `pcg.kernel=avx2` is invalid. The CPU budget line is REF / MIN (extract 1.0/1.5, prepare 1.5/2.0, compile 0.3/0.4, recording 2.0/3.0, submit 0.3/0.4, render thread 4/6 ms) and points at 02 §2.4 | 03 sized the spike on AVX2, which 02 §1.1's old containment rule forbade. 03 §8.1 had REF CPU budgets only |
| 20.2 | 04-networking-and-servers.md (§3.3, §10.2 Luau rows, recording header) | A Luau allowance paragraph under the §3.3 total covers one resume at most, ≤ 5 ms at 20 Hz and ≤ 4.2 ms at 60 Hz. The time-slice row adds per-call binding charges and the cost table. `fuel_kill` = min(≈ 5 ms, 25 % of the tick). The backstop row reads "a binding whose real cost far exceeds its calibrated charge". The replay header holds the cost table | The tick budget had no allowance for bindings that fuel did not meter |
| 20.3 | 06-gameplay-framework.md (§11 Luau hosting) | "…and every engine binding call is charged calibrated fuel too" | Keeps 06's summary of 02 §7.4 truthful |
| 20.4 | 09-roadmap-and-process.md (WP-0.2, WP-0.9c, WP-1.6, WP-3.4) | WP-0.2's AVX2 audit uses the allowlist. WP-0.9c runs on the dispatched `vm_avx2.cpp` kernel, with the SSE4.2 twin hashing identically. WP-1.6 builds `scriptlib` glue, builtin wrappers and the calibrated table. WP-3.4 closes the MIN CPU budgets and carries RT-12's MIN clause (Ph3) | Every new clause has an owner WP, and the Ph3 exit already takes every criterion with Ph ≤ 3 |
| 20.5 | ../PLAN.md (§4.2 rules, 02 summary) | The AVX2 rule names the allowlist (Jolt and the pcg kernels with CPUID dispatch and an SSE4.2 twin). The Luau host "also charges each binding call" | Keeps the summary truthful |

No criterion IDs were added or renumbered. RT-04, RT-09, RT-12 and RT-13 gained clauses. RT-12's phase reads
"1–4 (MIN: 3)". 00's ADR-013 ("AVX2 baseline, launcher checks CPUID") and 08 §2.2's CPU gate are consistent
as written: every client and SERVER CPU still passes the AVX2 gate, and the new pcg kernels run only after it.


## 21. Round-2 review fixes owned by 07 (2026-09-25)

One major and two minor tools-lens gaps in 07 were closed.
- **§1.10, editor extension SDK (major).** Gems ship `editor-core` modules (commands, document types,
  importers, builders and cook steps, T28 rules, node libraries and graph families, remote-control verbs,
  settings) and `editor-ui` modules (dock panels and whole tools, viewport modes and gizmos, customizers,
  PIE hooks). They build against a versioned `include/helios/editor/` API (`HELIOS_EDITOR_API`,
  `HELIOS_EDITOR_API_VERSION` `0.x` in Ph2 and `1.0` at the Ph3 exit). Every registration lands in the
  module's `ModuleRegistrationScope`, and every write goes through `TxBuilder`. A `Ui` façade with a `Canvas`
  keeps ImGui types out of public headers (02 §1.1). The Luau `EditorUI` binding gives sandboxed immediate-mode
  panels with fuel budgets. Modules hot-reload under ADR-016 in the editor and assetd. The rules cover crash
  attribution and assetd quarantine, panel budgets and determinism checks for builders. The API joins the
  public surface, doc coverage, the deprecation policy and `upgrade-test` through the `sample-editor-ext` gem.
  T06, T20 and T25 are built against public headers only. **New ED-19.**
- **§1.8.1, collab durability and scale (minor).** Tier C sessions need R3 streams (the `ha` Compose profile
  or Kubernetes); the R1 all-in-one needs `--collab-archive` and shows a banner. Journal segments go to object
  storage every ≤ 30 s with a hash chain. Content-addressed snapshots are written every 1,000 transactions or
  5 min, and a WIP git ref every 10 min. Submits are idempotent by transaction ID, and editors keep their
  transactions until they are archived, which enables tail recovery. A `collab_owner` PG lease gives
  failover in ≤ 8 s. The section has an RPO/RTO table, `helios-tool collab restore` with point-in-time
  `--to-seq`, a nightly restore test and a quarterly drill. The actor is sized at ≥ 500 tx/s through pipelined
  publishes and unstored gesture previews (`.preview`), and the stream is bounded. **New ED-20.**
- **§4.1.1, derived data after edits (minor).** Budgets from commit to live, and the state until then, for
  nav tiles (≤ 2 s p95, with a next-tick `DetourTileCache` obstacle), terrain tiles, portal graphs, scatter
  caches, container HLOD, fleet impostors, assembly-time SH probes (MIN GI), reflection probes and crowd
  impostors. Invalidation comes from transactions. Rebuilds are prioritized and coalesced, and a nightly
  check requires incremental output to equal a clean rebuild. **ED-3 gains a nav clause; new ED-21.**

| # | File | What changed | Why |
|---|---|---|---|
| 21.1 | 09-roadmap-and-process.md (§2.7.1 C++ row and coverage gate, §2.7.2 public API and binary SDK, §2.7.3 `upgrade-test`, §2.7.4 `starter-sandbox`, WP-1.18, WP-2.16, WP-3.7, WP-3.12, Ph2 and Ph3 exits, §5 test matrix) | The editor extension API is public, documented and in the doc-coverage gate from Ph3. The SDK's editor, `helios-tool`, `helios-assetd` and `helios-cook` are dev-flavour builds. `sample-editor-ext` joins the `upgrade-test` snapshots. WP-1.18 adds incremental nav in PIE, WP-2.16 the `0.x` API with Luau panels (ED-19 Luau), WP-3.7 collab durability and asynchronous bakes (ED-20, ED-21), and WP-3.12 API `1.0` (ED-19). The exits list ED-19 (Luau) at Ph2 and ED-19…21 at Ph3. Two test-matrix rows were added. `starter-sandbox` (§2.7.4) ships a Luau *Resource survey* editor panel that its template proof opens | Editor plug-ins were outside TOOL-7, TOOL-8 and the deprecation policy. Every new criterion has an owner WP and an exit |
| 21.2 | 01-vision-and-scope.md (AAA-TOOL-7) | The Ph3 clause covers "the public C++ game-module and editor extension APIs" | Doc coverage extends to the editor API |
| 21.3 | 02-engine-runtime.md (§1.2 gem manifest, §1.4 link-flavour table and registration scope) | The gem manifest gains `kind: editor-core \| editor-ui`. Reloadable modules are `game_<gem>[_client\|_edcore\|_edui]`, and a project's editor modules on the binary SDK are always their own DLLs. The dev flavour is also used by the SDK's `helios-tool`, `helios-assetd` and `helios-cook`. `ModuleRegistrationScope` records editor registrations | Editor modules load and unload through ADR-016's protocol |
| 21.4 | 00-decisions.md (ADR-016) | "Server-side tools" ship statically, while the SDK's editor, `helios-tool`, `helios-assetd` and `helios-cook` are dev-flavour. The module naming gains `_edcore` and `_edui` | "Tools ship statically" contradicted tools that load project editor modules from a binary SDK |
| 21.5 | 05-backend-services.md (§1.14 collab service) | New bullet: R3 `COLLAB_*` streams on the `ha` profile and Kubernetes, object-storage segments and snapshots, the WIP git ref, the `collab_owner` PG lease with term fencing, `collab restore` and the quarterly drill | 05 owns the collab service's deployment and data |
| 21.6 | ../PLAN.md (07 summary, criteria list) | The 07 bullets mention collab durability, the extension SDK and derived-data budgets. The list reads `ED-1…21` | Keeps the summary truthful |
| 21.7 | CONSISTENCY.md §1 (ID check row) | 07 now has 21 ED IDs | New criteria ED-19…21 |

ED-19, ED-20 and ED-21 were added and ED-3 gained a clause. No ID was renumbered.


## 22. Round-2 review fixes owned by 06 (2026-09-25)

One major and two minor gaps in 06 were closed.
- **§8.2, bit-exact FBW (minor, engine lens).** 04 §5.3 relied on bit-for-bit hull prediction, but 06 §8.2 only
  promised "rare" mispredictions, and RT-03 did not hash the controller. §8.2 now states the bit-exact contract:
  quantized inputs only; f64 with strict FP and `hmath`; controller state in the rollback snapshot; thrusters in
  cooked port order; a fixed allocator (a regularized LDLᵀ warm start memoized by a pure key, then at most 8
  active-set iterations that clamp one thruster at a time, ties to the lower index, exact comparisons only);
  health quantized to 1/16 so `B` changes only on replicated, tick-stamped events; spool factors computed once;
  and ambient wind scheduled 250 ms ahead. Corrections are tagged by cause, and `divergence` must be 0.
  **GP-4 is split** into 4a (FBW, with a new bit-exact clause across the 09 determinism toolchains at 1/4/16
  workers, run in RT-03's job; Ph1), 4b (command flight; Ph2) and 4c (EVA; Ph3), matching the 4a/4b split 09
  already used.
- **§6.6–6.13, activities, encounters and match rules (major, tools lens).** §6 is split into 6.1–6.5 (the
  existing quest, mission, event, dialogue and phasing text, unchanged) and new subsections: `ActivityDef` and
  the instance lifecycle; `EncounterDef` with phases, checkpoint contents, wipe and reset, and `RevivePolicy`;
  `LockoutPolicyDef` with loot-once enforced by a ledger guard row committed with the grant, carried
  checkpoints and saved instances; `DifficultyTierDef`, `ActivityModifierDef` and `SyncPolicy` (level sync,
  bolster, power delta, PvP normalization, per-player scaling, solo mode); backfill, join-in-progress, AFK,
  vote-kick and leaver policy; `MatchRulesDef` (teams, objectives, rounds, respawn, mercy, overtime, tie-breaks,
  a tick-stamped event-log score, Weng-Lin rating); opt-in PvP risk zones with extraction (R05-P2-22); and
  `QueueDef`, `LeaderboardDef` and `GroupListing`. **New GP-14** (a)–(h). The ladder gains an Activities / PvP
  row. §5.1's `ActivityDef.powerRules` is now `ActivityDef.sync`.
- **§8.2a, EVA (minor, tools lens).** A new `EVA` movement model: a predicted 6-DoF sphere `CharacterVirtual`
  with suit thrusters on §8.2's allocator, a Δv fuel budget in the predicted state, server-side O2, align
  assists, magboots, grabs as predicted ASM abilities, tethers, zero-g `Exterior` shell grids around hulls,
  and a specified gravity switch (re-orient ≤ 0.5 s with a sphere shape, stand only after a clear capsule
  cast, placement validated against portal cells). GP-4c covers it on BENCH-6.

| # | File | What changed | Why |
|---|---|---|---|
| 22.1 | 02-engine-runtime.md (§5.4 grid kinds, RT-03) | Host grids include a ship's 0 g `Exterior` EVA shell. RT-03's job also runs 06 GP-4a's FBW hash | EVA shells are host grids; the FBW hash needs an owning CI job on the same toolchains |
| 22.2 | 04-networking-and-servers.md (§5.3) | Corrections come from contacts, damage and others' effects. The controller and allocator are held to the same bit-exact bar (06 §8.2, GP-4a). The `Flight` channel sends six 8-bit axes and an 8-bit throttle in the §5.2 input record. EVA characters are predicted in the owning grid's coordinates | 04 claimed bit-exact hull prediction without naming what guaranteed it; no section defined the flight input encoding |
| 22.3 | 05-backend-services.md (§1.6 op list; §1.12 rewritten; §13 06 row) | Ledger op `ClaimGuard(scope, key, period, payload?)` with a period-partitioned `ledger_guard` table. §1.12 now specifies parties, the matchmaker (leader per queue partition, Valkey mirror, tier and latency buckets, a 1 s pass with Hopcroft–Karp role feasibility and exhaustive 6v6 team splits, widening, ready check, backfill, ratings once per `matchId`, capacity per shard), the group finder, leaderboards (PG truth, Valkey cache, write and read targets), activity state with `LockoutView`, and Phase 5 cross-shard federation (value never crosses shards) | Weekly lockouts outlive the 72–96 h idempotency window, so they need a guard entity (05 §3.3). The reviewer asked for a specified matchmaker with capacity figures |
| 22.4 | 01-vision-and-scope.md (§2 (c) and (e); §2.6) | Destiny adds Crucible 6v6 with skill-based matchmaking and risk zones; SWTOR adds tiers, level sync and bolster, solo modes, a group finder and warzones. Both list G19 as critical and name the template proofs. G12's text widens and its owner adds 06. **New capability G19** (PvP match modes and rating, matchmaking, group finder, leaderboards; ● for DST and TOR; owners 05 and 06; Ph3) | The archetypes omitted Crucible and warzones although G12 was critical for both |
| 22.5 | 07-editor-and-tools.md (tool layout table, T12, §2.6.4, §5.1) | T12 gains an Activity mode: encounter graph, activity flow, tier and modifier matrix with a sync preview, lockout editor, match-rule editor with a PIE bot scrimmage, a queue simulator that runs the real Go matchmaker in process (within ±10 % of GP-14a), and validators. §2.6.4 adds a *hull shell* grid-volume shape with a T28 margin rule. §5.1 T12 Ph3 reads "A (incl. Activity mode, queue simulator)" | No tool authored activities, lockouts, match rules or queues |
| 22.6 | 08-client-and-launcher.md (§1.7 panels) | New Activities panel (director, queue and ready check, group finder, tracker, PvP scoreboard, post-match rating, leaderboards; Ph3, ranked seasons Ph4). The HUD adds EVA suit gauges (Ph3) | The client needs surfaces for the new systems |
| 22.7 | 09-roadmap-and-process.md (WP-1.16, WP-3.2, WP-3.6, WP-4.5, WP-5.3; starter templates; §3.2 #15; Ph3, Ph4 and Ph5 exits) | WP-3.2 delivers the matchmaker, group finder, leaderboards and `ClaimGuard` lockouts (GP-14 a, b). WP-3.6 delivers EVA, activities, match rules and risk zones (GP-4c, GP-14 c–f) and depends on WP-3.2. WP-4.5 adds ranked seasons and leaderboard windows (GP-14 g). WP-5.3 adds cross-shard federation (GP-14 h). `starter-shooter` proves a matchmade strike, a checkpoint resume and a rated 6v6 match; `starter-story` proves a flashpoint with tiers, level sync, the group finder and a solo clear. Row #15 lists the new phases. The Ph3, Ph4 and Ph5 exits list GP-4c and GP-14 | Every new criterion has an owner WP and a phase exit |
| 22.8 | ../PLAN.md (capability table; 06 summary; criteria list) | G12 row widened and G19 added; the 06 summary names the activity framework, the bit-exact allocator and EVA; `GP-1…14 (GP-4 as 4a–4c)` | Keeps the summary truthful |
| 22.9 | This file (§1 checks) | `GP-1…14` | New criterion |

No existing criterion was renumbered. GP-4's text now lives in GP-4a and GP-4b with its thresholds unchanged.
R03-P1-8's cross-shard group finder is designed now (05 §1.12) and ships in Phase 5, consistent with 04 §11.3's
and 05 §9's ladders; one 50k-CCU shard meets GP-14a at the Phase 4 bar without it.

## 23. Round-2 review fixes owned by 09 (2026-09-25)

Two tools-lens gaps against 09.
- **Merge policies contradicted each other (minor).** 09 §5.2 squash-merged every PR. 07 §1.8 merges collab
  publishes "never a squash" to keep per-commit authorship and ED-10's check. *Cinder Reach* content shares
  the engine repository's queue. 09 now has §5.2a, a merge method per branch class: WP-class branches squash,
  and `collab/*` rebase-merges and is validated commit by commit (T28 on every commit, the ED-10 diff on the
  head, trailer integrity, content-only paths, and up to date by re-export rather than a GitHub rebase). A
  post-merge `merge-policy` check restores a wrongly squashed publish. `main` requires linear history, so of
  07's two allowed methods the engine repository uses rebase-merge. 07 needs no edit.
- **Stale status, and no propagation of plan changes to code (minor).** 09 §8.1 and PLAN.md §11 said
  `services/` did not exist. The tree has a Go backend (identity, session, orchestrator) that was first
  written to round 1's leases (a 3 s NATS TTL, a `LEASES` bucket, Snowflake node IDs). 09 now has §5.10:
  - declared plan changes (`Plan-Change:`, `PLAN-REV`) and an anchor → path map;
  - conformance-rework WPs (WP-0.15r first) and re-baselining of in-flight WPs, with `Plan-Rev` checked
    in the queue;
  - the CONF-01…10 conformance lint in the PR tier and the full-tree round audit;
  - phase exits blocked by any finding.

  The §5.10.4 table records what the working tree already reworked and what WP-0.15r must still do. §8.1
  and PLAN.md §11 are refreshed, and the Director refreshes them every round (D6).

| # | File | What changed | Why |
|---|---|---|---|
| 23.1 | 09-roadmap-and-process.md (§1 exit rule; WP-0.1, 0.2, 0.15, new 0.15r; Ph0 exit; §3.2 #13, new #22 and #23; §5.1–5.3, new §5.2a; §5.6–5.8; new §5.10; §6; K16, new K37; §8.1, §8.2; §9) | Merge policy by branch class, conformance rework and lint, refreshed status | The two gaps above |
| 23.2 | 05-backend-services.md (§1.4.5 "Who mints") | Identity (account IDs) and Character (character IDs) join the minter list. Session IDs are random 63-bit values, and CONF-05 enforces the list | The in-tree identity service mints account IDs from blocks, and 05 §1.5's `Create` needs character IDs, but neither was listed |
| 23.3 | ../PLAN.md (§8.2 round step 7; §11 current state and backend run line) | Step 7 merges by branch class, and a new "When the plan changes" bullet summarizes 09 §5.10. §11 lists the `services/` backend, the current CI jobs and the Go run command | Keeps the summary truthful |

## 24. Round-2 review fixes owned by 08 (2026-09-25)

08 addressed two minor gaps from the round-2 tools review.
- **§1.7, Foundation UI breadth.** The section now has three parts. §1.7.1 is the panel contract: RML, token-only
  RCSS, view-model schemas with generated adapters, a Luau controller, an input context and a flow test. It
  also covers `ThemeDef` themes and reskinning by theme or by file override, with no engine edit. §1.7.2 is a
  23-row panel table with screens, view-models, phase and owning WP. The new rows are character creator,
  settings, terminal host (with bank), journal, loot, market and contracts, NPC vendor, trade window, mail,
  crafting and industry, survey and harvesters, placement and decoration (structures, cities), organization
  (roles, divisions, wallets, audit), activity director and group finder (with leaderboards), and killmail
  viewer. A coverage paragraph maps M2 and every starter template to panels. §1.7.3 defines the headless Luau
  flows (reject paths plus keyboard-only and gamepad-only passes), the RmlUi adaptation of ED-15's layout lints
  and the `ui-reskin-fixture` proof. **New CL-23.**
- **§2.10, product identity and branded builds.** The `product` block of `helios.project.jsonc` is the only
  source of every path, URI scheme, registry key, credential name, lock, executable name, and crash and
  telemetry endpoint. A scoping table covers Windows and Linux, and a CI grep bans engine-name literals. The
  `helios-tool product init` offline key ceremony produces a root pair (current and pre-committed next) held
  as 3-of-5 Shamir shares, quarterly subkeys, a per-product keyset and a root-signed product block, with
  rotation, revocation, ratchets and a drill. `product stamp` writes the block into reserved sections and
  resources of prebuilt, unsigned SDK binaries in place, before Authenticode signing. T29's *Package &
  publish* profile outputs a signed installer, an AppImage, symbols, an SBOM and a smoke run. §1.4, §2.1–2.5,
  §2.7–2.9 and §3 now use the scoped names. CL-14 adds cross-product, revoked-subkey and ratchet rejection.
  **New CL-24.**

| # | File | What changed | Why |
|---|---|---|---|
| 24.1 | 09-roadmap-and-process.md (WP-1.20, WP-2.7, WP-2.13, WP-2.16, WP-3.8, WP-4.9; Ph1–Ph4 exits; §2.7.2 project layout; §2.7.4 `template-proof`; §6 test matrix) | WP-1.20, 2.13, 3.8 and 4.9 list their §1.7.2 panels and CL-23 per phase; WP-2.13 now depends on 2.8 and 2.9, WP-3.8 on 3.2 and 3.6, WP-4.9 on 4.5. WP-2.7 owns product scoping and the keyset ratchets. WP-2.16 owns the `product` block, `product init` and `stamp`, prebuilt stampable binaries and *Package & publish*, with CL-24 (Ph2); WP-3.8 has the AppImage clause. The Ph1–4 exits list CL-23 (and CL-24 in Ph2 and Ph3). The "launcher branding kit" of §2.7.2 is now the `product` block. `template-proof` packages through the profile and runs the class's panel flows. Two test-matrix rows were added ("Client UI", "Product packaging") | Every new criterion has an owner WP and a phase exit; the panels depend on the gameplay WPs that deliver their data |
| 24.2 | 02-engine-runtime.md (§1.3 launcher row; §2.5 CVar layering; §7.5 view-models; §7.6 user settings) | The launcher row says the SDK's `Helios.exe`/`HeliosLauncher.exe` are stamped and renamed per product. The settings paths use `<Install>`/`<productId>`. View-model fields annotated `@source(Component.field \| Service.Stream)` get schemac-generated adapters | Hard-coded `Helios` paths would collide between two products; panels must not need C++ |
| 24.3 | 05-backend-services.md (§7 CDN layout) | `/keys/<product>/keyset.json`, max-age=60 | Keysets are per product, like the manifests and channels already were |
| 24.4 | 06-gameplay-framework.md (§3 weighted quality; §9.2 placement validation) | `CraftQuality` and `PlacementCheck` are named pure functions shared with 08's crafting estimate and placement ghost | The client previews use the cell's code, as `FittingValidator` does |
| 24.5 | 07-editor-and-tools.md (T29 features) | Adds the *Package & publish* profile | 08 §2.10.5 defines it; T29 owns build profiles |
| 24.6 | ../PLAN.md (08 summary; criteria list) | Two 08 bullets (Foundation UI, product identity). The list reads `CL-1…24` | Keeps the summary truthful |
| 24.7 | CONSISTENCY.md §1 (ID check row) | 08 now has 24 CL IDs | New criteria CL-23 and CL-24 |

CL-23 and CL-24 were added and CL-14 gained a clause. No ID was renumbered. **No change needed:** 04 (each
product runs its own backend with its own shard keys, so connect tokens are already product-isolated).

## 25. Integration pass 3: round-2 revisions (2026-09-25)

Inputs: a full re-read of PLAN.md, README, 00–09 and this log after every section's round-2 revision
(§15–§24). No reviewer gap was filed against PLAN.md itself in round 2. Each section's own round-2 fixes had
already touched the PLAN.md lines for that section; this pass reconciles what fell between them.

**New cross-section inconsistencies found in the re-read.**

| # | File | What changed | Why |
|---|---|---|---|
| 25.1 | 05-backend-services.md (§1.12 Matchmaker) | "One leader per queue partition, elected through a NATS KV lease" → ownership is a PG row `mm_partition(queue, partition, owner_replica, owner_gen)`, assigned by rendezvous hashing and taken over by a CAS on `owner_gen`, as for market partitions (§1.7). A deposed leader stops when a write at its old generation fails. No NATS KV lease | The text added by fix 22.3 contradicted 05 §0 rule 8, §1.4.1 and §2.3 ("no `LEASES` bucket: generations and leadership live in PG"), and 09's CONF-01/02 would flag the code it describes |
| 25.2 | 02-engine-runtime.md (§7.1 determinism bullet, RT-03, §8.3, §8.4) | "4 compilers" → every determinism toolchain: both MSVC toolsets, clang-cl, GCC, Clang and MinGW (09 §6, ADR-001a rule 7) | Fix 22.1 runs 06 GP-4a's FBW hash in RT-03's job, and GP-4a requires MSVC (VS 2022 and VS 2026), clang-cl, GCC, Clang and MinGW. ADR-001a rule 7 and 09 §5.6, §6 and K4 count five families |
| 25.3 | 09-roadmap-and-process.md (§5.2 step 5) | The PR tier is the ADR-001a 8-configuration matrix (`windows-msvc`, `windows-msvc-dev`, `windows-msvc-floor`, `windows-clang-cl`, `linux-gcc`, `linux-clang`, `linux-headless`, `cross-mingw`) plus the conformance lint on changed paths | Fix 19.4 updated §5.6 to 8 configurations, but step 5 still listed six jobs, without the floor or modular MSVC jobs and without the CONF lint that §5.10.3 puts in the PR tier |
| 25.4 | 09 (§3.2 #23, WP-0.15r, §5.10 intro, §5.10.4 intro, K37, §8.1); 05 (§1.4 "Why the design changed"); ../PLAN.md §11 | "Round 2 replaced round 1's NATS-KV leases", "round-2 plan" and "round 1's lease design" → draft v1 (the first draft), draft v2 (the round-1 review fixes, §7 and §14.10 here) and draft v3 (the current plan, which added failure domains) | This log dates the replacement of NATS leases and node IDs to the round-1 fixes. "Round 2 replaced" read as a round-2 change, and WP-0.15r's open rows include draft v3's failure domains, so "the round-2 plan" was ambiguous |
| 25.5 | 09 (§1 overview table; Ph3 exit) | Key gates add REN-8 (Ph1 panel) and REN-8 (Ph3 panel). The Ph3 exit list names RT-12 (MIN clause) | 01 §5.4 gates REN-8 at the Ph1 and Ph3 exits (§17); the overview omitted the new human-judged gate. RT-12's MIN clause gates at Ph3 (fix 20.4), and PLAN.md §8.1 now names it, so the exit list names it too |
| 25.6 | 03-rendering.md (§8.2 Particles row) | The row is the active cap per preset. Low and Medium rise to 256k in 2 Hz fleet-battle zones, so MIN's BENCH-3 significance pass keeps its top 256k (§6.1) | 01 §3.2 and 03 §6.1 (fix 17.1) have MIN keep 256k particles in BENCH-3, but MIN runs Low, whose cap was 64k |
| 25.7 | 03, 06 and 08 headers | 03: "draft v2" → "draft v3". 06: the conformance list adds ADR-001a, cited in §1.2 since fix 19.3. 08: the conformance list adds ADR-016, cited by §2.10's C++ route | Status and conformance lists lagged the round-2 edits, as fix 14.12 found after round 1 |
| 25.8 | README.md (phase vocabulary) | Ph2 adds the editor extension API preview and product-branded packaging. Ph3 adds activities with matchmaking, the group finder and PvP match rules, EVA, and the stable editor extension API. Ph4 adds the human-judged gates REN-8 and TOOL-10 | The single phase vocabulary lagged 06 §6.6–6.13 and §8.2a, 07 §1.10, 08 §2.10 and 01 §3 |
| 25.9 | ../PLAN.md (status; §1; §2; §3 R05; §4.2; §5 ADR-016; §6 00–05, 07 and 09 summaries; §7 proofs; §8.1 gates and next WPs; §8.2; §9; §10; §11; §12) | Status v2 → v3. §1: the eight-job CI matrix; a "humans judge what bots cannot" bullet (REN-8, TOOL-10); "where we are" says two review rounds, uses 09 §8.1's test counts (141/141, 104/104; it said 133/133 and 101/102, contradicting §11) and names the Go backend and WP-0.15r; the panel and content team join the gates the loop cannot compress. §2: backend (activities, failure domains, compat epochs), editor (extension SDK, ED-19), T12's Activity mode, client (CL-23), launcher (CL-24), 51 → 52 capabilities. §4.2 and ADR-016: editor-extension modules and dev-flavour SDK tools. §6: ADR-001a; 01 (52 capabilities, BENCH-3 as the battle's client view, evidence classes, REN-8, TOOL-10); 02 (MIN CPU budgets, the ISA allowlist, calibrated binding fuel); 03 (horizon maps, `StreamTexId` feedback, character composites, cloud fast mode); 04 (gateway boxes N+1, NS-4.7); 05 (`ClaimGuard`, the matchmaker, group finder and leaderboards, collab durability); 07 (T12 Activity mode); 09 (merge policy, plan-change propagation, the panels and team in H6, 38 risks). §7: the `starter-shooter` and `starter-story` proofs. §8.1: gates for Ph0 (conformance lint), Ph1 (REN-8, GP-4a), Ph2 (CL-24, ED-19 Luau), Ph3 (REN-8, NS-3.10, RT-12 MIN, GP-14, ED-19…21) and Ph4 (NS-4.7, BE-A14, REN-8, TOOL-10); the next WPs include WP-0.15r. §8.2: the PR tier and DoD (five compiler families, conformance). §9: ITR, TOOL and PLT examples; the M class; phase completion needs a clean conformance lint; the auditor rubric (human-UI share, REN-8 trend, code health). §10: 38 entries, a K24/K37 row, K28's half-load trigger. §12: ADR-001a, three glossary terms, K1–K37 | Each section's round-2 fix touched only its own PLAN.md lines. 03's fixes, 09's K37 and budget changes, and several new gates were never reflected, and §1 contradicted §11 |
| 25.10 | This file (§1) | Four check rows: links and anchors, the PLAN.md §7 matrix, NATS leases, determinism toolchains | Keeps the automated checks current |

No criterion IDs were added or renumbered. RT-03's threshold and scope are unchanged; only its toolchain list
now matches ADR-001a. **Checked and consistent, no edit:** the 09 §4.3 budget (column sums, the ≈ $2.2–13.2M
total and the C1/H5 shares recompute exactly); 03 §8.1's four column sums; every `AAA-*`, RT, RC, NS, BE, GP, ED
and CL ID cited in any file resolves, as do every `WP-*` and `K*` ID; each new criterion of round 2 has an
owner WP and a phase exit in 09.

## 26. Round-3 review fixes owned by 05 (2026-09-25)

Gaps in 05 from two reviewer lenses:
- **Major (tools):** player fleets were cited (04 §2.7 fleet voice, NS-3.9, 06 §1.2's `Fleet` domain, 06 §7.3
  drone assist) but never specified, while 05 §1.12 capped groups at 16. No fleet hierarchy, roles, fleet
  warp, broadcasts, command bursts, adverts, panel or criterion existed, and the 2,000-ship battle used only
  bots.
- **Minor (tools):** studios had no backend extension surface. Go services are a fixed catalogue, cell Luau
  reaches only `WorldFlagDef` scalars, and 01 §1.1 forbids backend source edits.
- **Minor (backend):** gateway capacity after an availability-zone loss was unsized (04 §2.6 covered one box,
  05 §6.7 put gateways in 3 zones), and A19 did not check gateways.
- **Minor (backend):** four stale cross-references (05 §1.13's fence columns, 05 §8's `Fence` interface,
  08 §1.1 step 7, 09 §4.3.2's warm pool).

05 changed as follows:
- **New §1.12.1, fleets:** a fleet group kind of ≤ 256 members (fleet → ≤ 5 wings → ≤ 5 squads of ≤ 10);
  boss, FC, WC and SC roles with delegate rights; invites, `ConvertToFleet`, CAS-versioned roster edits
  (`ROSTER_STALE`), succession, fleet adverts with a join ACL in the group finder, and roster fan-out through
  KV `GROUP` to voice, chat, cells and clients (p99 ≤ 1 s). Supporting edits in §1.12's title, §2.2, §2.3,
  §2.4, §3.1, §3.2, §3.6, §9, §11, §12 and §13.
- **New §1.23, world scripts (option (a) of the review):** a shard-scope Luau host (`helios-cell --role
  world-script`) whose partitions are lease-fenced orchestrator regions; declared `worldscript` tables,
  indexes, RPCs, events, timers and reason codes in the project schema; one generic `svc_worldscript` store
  (CONF-06 unchanged) with inbox dedup and `lease_gen`-fenced commits; escrow-only value (cells escrow, scripts
  release, sink or grant within faucet caps) with an hourly escrow-backing audit; quotas, fuel and a kill
  switch; hot swap and `migrate` handlers; privacy participation. Option (b), a Go module API, is rejected
  for now because Go plugins do not work on Windows. The service count is 23 (§1). **New BE-A20** (Ph3).
- **§6.7, gateway zone-loss policy ("size for it"):** gateways spread evenly over 3 zones; the zone rule
  (the slots outside any zone hold every session and still meet the box rule), so 6 boxes at Ph3 and 12 at
  Ph4, with the Ph4 survivors equal to 04 §2.6's 8-box configuration; admission capped by the rule; a
  reconnect lane as a backstop; the Session service, Valkey Sentinel and voice spread over zones; player-visible
  recovery ≤ 20 s (gateway lost) and ≤ 5 min (cell lost). Supporting edits in §1.2, §1.3 (a 20k-ticket burst in
  ≤ 8 s), §6.1, §6.2, §6.4, §6.8 (two alert rows), §8 tests, §9 and §11. **A19** gains a gateway clause.
- **Cross-references:** §1.13 lists `state` and `parked_seq`; §8's Go `Fence` gains
  `Park(ctx, root, e, owner, finalCkptSeq)` (04 §11.2's `IFence.park`), plus a `WorldScriptStore` interface.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 26.1 | 04-networking-and-servers.md (§1 gateway row; §2.6; §11.4 NS-4.2; §11.5 risks) | The gateway row and §2.6 add the zone rule (6 boxes at Ph3, 12 at Ph4, spread over the shard's 3 zones; 05 §6.7). NS-4.2's bots fight as 8 player fleets of 250 with bot commanders, broadcasts, fleet warps and bursts, meeting GP-15's landing rule. A risk row for a zone loss | 04 §2.6 said "8 boxes over 2 zones", which contradicted 05 §6.7's 3 zones and could not absorb a zone's ≈ 17k sessions; the reviewer asked for NS-4.2 to exercise player-fleet mechanics |
| 26.2 | 06-gameplay-framework.md (§1.3 `stacking.keep`; §6.13 `GroupListing`; new §8.3a; §12.1 Flight/combat row; §12.2 preamble and **new GP-15**; §12.4) | New §8.3a: `FleetDef`, `FormationDef`, `BroadcastDef`, `CommandBurstDef` and `FleetMembership`; fleet warp as a group `CommandKinematic` command with eligibility, deterministic fixed-point formation slots and one landing tick (slowest warp speed, stretched profiles); schema'd, rate-limited broadcasts delivered on the fleet chat subject in ≤ 1 tick + RTT; command bursts through the `Fleet` modifier domain with range snapshots and strongest-wins stacking (`keep: Strongest`, default `Newest`); killmails carry `fleetId` and fleet damage shares; a bot fleet-commander behaviour. `GroupListing` gains `kind: Fleet`, a goal tag and a join ACL (`activity` and `tier` become optional). GP-15 (a–d): fleet warp, broadcasts, bursts, roster and attribution; Ph3 at 500 ships, Ph4 inside NS-4.2 | 06 owns the records and cell side of 05 §1.12's groups (as for activities); the reviewer asked for a new §8.3a and GP-15 |
| 26.3 | 08-client-and-launcher.md (§1.1 step 7 and the no-launch-code note; §1.2 Reconnecting row, which shows a reconnect-lane ETA; §1.7.2 new **Fleet** panel, killmail fleet lists, `starter-fleet` coverage; §4.3 ladder) | Step 7 gates only on the compat epoch: a same-epoch staged or older build is admitted, a flip applies the pre-downloaded build (`CONTENT_EPOCH_FLIP`), and only another epoch or a build below `min_client` hands off to the launcher. The Fleet panel (hierarchy tree with drag and drop and `ROSTER_STALE` refresh, broadcast bar and history, fleet warp with refusal reasons, adverts, burst range rings; `FleetVM`, `FleetMemberVM`, `BroadcastVM`, `FleetAdvertVM`) is Ph3 (WP-3.8) and so falls under CL-23's Ph3 flows | Step 7 still said a `content_build` mismatch hands off, although tokens gate only on the epoch (05 §1.14.1); fleets had no UI |
| 26.4 | 09-roadmap-and-process.md (WP-3.2, 3.6, 3.8, 3.12, 4.3, 4.5; Ph3 and Ph4 exits; §2.7.1–2.7.4; §3.2 #15; §4.3.2) | WP-3.2 builds the fleet roster (GP-15 d); WP-3.6 the fleet mechanics (GP-15 a–c, Ph3); WP-3.8 the fleet window; WP-3.12 the world-script API `1.0`, its reference and tutorial and the bounty board in `upgrade-test` (BE-A20); WP-4.3 zone-loss gateway sizing; WP-4.5 fleets at 2,000 ships (GP-15 Ph4). Exits add GP-15 and BE-A20. §2.7 lists the `world` realm and `worldscript` block in the docs, the public API and `upgrade-test`, and `starter-sandbox` gains the cross-zone bounty board with an escrow and exactly-once proof; `starter-fleet`'s battle is fought as player fleets. §4.3.2 recomputes the soak with the domain-sized pool (100–200 cells + 15–27 pool slots + ≈ 5 % replicants ≈ 120–240 cell-equivalents, cost unchanged at $1.7–7.8k) and 12 gateway boxes ($0.15–0.36k); the ≈ $3–12k total holds | Every new criterion has an owner WP and a phase exit; the cost model cited a 20 % pool that 05 §1.4.6 and §6.4 had replaced |
| 26.5 | ../PLAN.md (§2 backend row; 04 and 05 summaries; §9 criteria list) and README.md (Ph3 vocabulary) | 23 services; parties and player fleets; world scripts; the zone-loss gateway sizing; `BE-A1…A20` and `GP-1…15`. Ph3 adds player fleets and the world-script API | Keeps the summaries truthful |
| 26.6 | This file (§1 criteria check row) | `BE-A1…A20` and `GP-1…15` | New criteria BE-A20 and GP-15 |

BE-A20 and GP-15 are new; A19 gained a clause and NS-4.2 a bot-fleet clause. No ID was renumbered.
**Checked, no edit needed:** 04 §2.7's fleet voice channel (its hierarchy and talk levels match §1.12.1);
04 §11.2's `IFence.park` signature (05 §8 now mirrors it); 05 §1.14.1 and 08 §2.6 (already epoch-gated);
01 AAA-SRV-10 and AAA-TOOL-9 (their thresholds are unchanged; GP-15 and BE-A20 add evidence).

## 27. Round-3 review fixes owned by 04 (2026-09-25)

One major and three minor backend-lens gaps in 04: planned zone and region migration had no protocol (major);
lag-compensated hits across a cell boundary were unspecified; a gateway loss under v1 view composition was
unhandled; and a replicant restore was not tick-consistent.

| # | File | What changed | Why |
|---|---|---|---|
| 27.1 | 04-networking-and-servers.md (§1 touchpoints, §3.1, §3.5, §5.1, §6.5, **new §6.7**, §7, §8, §10.3, §11.2–11.6) | **Planned region migration.** P pre-copies the region to a warm target Q in the replicant stream format; a quiesce window stops fence ops and handoffs; at tick T P freezes and sends a ≈ 6 MB residual (last tick, Jolt state, AG table, handle blocks, leader state, effect outboxes, pending calls, compact per-session records); Q checks the tick-T state hash; one term-fenced PG transaction bumps `region_lease` to g+1 and moves the zone leader and handle blocks (the commit point); Q resumes, sends `RouteUpdate`s and `ClockReset`; `Fence.MigrateRegion` (cooperative `AdvanceOwnedBy`, countersigned by P's manifest) runs off the hitch. Abort before commit, Q death after commit (P's frozen copy acts as replicant), replay chaining, multi-cell zone-wide hold, scripts (`Authority.Adopted`). `Drain` (descending CCU, ≤ 4 at a time, packed process ≤ 60 s) and `PreProvision` (≥ 10 min ahead, abandoned at T−5 min) are defined as migrations. Budgets per profile; hitch bound ≤ 1 s p99. New **NS-3.11** | Reviewer: rolling restarts, epoch flips, `PreProvision` and `Drain` rested on "bulk AG handoff", which single-cell zones and transients cannot use; no hitch or state-loss bound; no Phase 3 criterion for WP-3.1's zone migration |
| 27.2 | 04 (§5.6, §6.2, §11.4) | **Lag compensation across a boundary.** One rule: the shooter's cell resolves every shot against local bodies and tick-rate ghost hit history (`HitPose`, same quantized values as the owner's history); a cook check sets the hitboxed ghost margin to `max(M, M_hit)` and caps `M_hit` (1 km ground, 10 km space); damage lands as idempotent `ApplyDamage`. Effects de-duplicate by per-pair sequence in a server-audience `EffectInbox` that moves with the AG; senders keep a ≥ 15 s per-region outbox and re-send to the region's new owner after recovery or migration. Forwarding `ResolveShot` to the owner is rejected. New **NS-3.12** (Ph4 rerun with cell kills) | Reviewer: ghost history rate and fidelity, M vs weapon range, and delivery of effects across a recovery longer than the 10 s dedup window were undefined; no seam-combat criterion |
| 27.3 | 04 (§2.4, §2.6, §6.6 Failures, §11.4 NS-4.4, NS-4.7) | **Gateway loss under view composition.** Cells stop writing and free per-(session, cell) state 2 s after a gateway trunk goes silent; the client sends `HeldEntities` on rejoin, and the new gateway sends `ClientRebind` with it to every contributing cell, which rebuilds state with full `lost_mask` (or ORs in-flight masks into `lost_mask` if it kept suspended state). Gateway load under composition is sized. NS-4.4 is measured in a 10-cell zone; NS-4.7 adds a field-state audit | Reviewer: rebind reached only one cell, in-flight chunks through a dead gateway were never repaired, dead registrations were never collected, and capacity was untested under composition |
| 27.4 | 04 (§6.4, §11.4 NS-4.6) | **Tick-consistent restore.** Every replicant stream tick ends with `TickFlush{tick, chunk_count, state_hash}`; the replicant commits whole ticks only. The standby restores to one tick R: command replay for `CommandKinematic` ships, dead reckoning for other bodies, a bounded depenetration pass (5 cm, 4 iterations, then pair collision disabled), speed clamps. NS-4.6 adds 0 interpenetrations > 5 cm, 0 explosions and transient positions within v × 200 ms of a truth tap | Reviewer: the restored region mixed ticks (60–300 m of relative error at 300–1,500 m/s) |
| 27.5 | 01-vision-and-scope.md (AAA-STB-6) | Adds a per-zone migration hitch p99 ≤ 1 s and 0 state loss (04 §6.7, NS-3.11) | The reviewer asked for hitch and state-loss bounds; 01 owns the row |
| 27.6 | 05-backend-services.md (§1.4 API, §1.13 fence, §1.14.1 release table and flip step 4, §6.3 cells, fence call table, Go `Fence` interface, A14, 04 interface row) | `Drain` and `PreProvision` defined as migrations with the `region_migration` row and one commit transaction; `Fence.MigrateRegion`; rolling restarts and the epoch flip use planned region migration instead of handoff; A14(b) adds hitch p99 ≤ 1 s and 0 state loss per zone | 05 owns the orchestrator API, fence, release process and BE-A14; each edit restates 04 §6.7 |
| 27.7 | 06-gameplay-framework.md (§11 Luau hosting) | Coroutines never cross processes; the new owner raises `Authority.Adopted{entity, cause}` after handoff, migration or recovery, and modules re-arm from `ScriptState`; the analyzer flags long waits with no handler | 04 §6.7 needs a script rule for what cannot be serialized; 06 owns the scripting API |
| 27.8 | 09-roadmap-and-process.md (WP-3.1, WP-4.3, Ph3 exit) | WP-3.1 builds migration, seam lag compensation and exact effects, and carries NS-3.11 and NS-3.12; WP-4.3 builds the tick-consistent restore and the gateway-loss rebind and carries the NS-3.12 Ph4 rerun; the Ph3 exit becomes NS-3.1–3.12 | Every new criterion has an owner WP and a phase exit |
| 27.9 | This file (§1); ../PLAN.md (04 summary) | NS counts 7/5/7/12/7/4; the 04 summary names exactly-once effects, seam lag compensation and planned migration | Keeps the log and summary truthful |
| 27.10 | 08-client-and-launcher.md (§1.2 Reconnecting row) | On rejoin the client sends its entity table once as `HeldEntities`, and it re-bases its command clock on `ClockReset` | 04 §6.6's gateway-loss rebind and §5.1's hard resync are client behaviour; 08 owns the state machine |

New IDs: NS-3.11 and NS-3.12. NS-4.4, NS-4.6 and NS-4.7 gained appended clauses. No ID was renumbered.

## 28. Round-3 review fixes owned by 08 (2026-09-25)

Two minor gaps in 08:
- **Engine lens.** The launcher's ISA claim was inconsistent and untested. 08 §2.1 said baseline SSE2 and
  PLAN §4.2 said "any x86-64 CPU", but the launcher linked x86-64-v2 code. CL-17 emulated only Nehalem, which
  has SSE4.2 and POPCNT, so a Core 2 or Phenom II would fault instead of seeing the AVX2 message.
- **Tools lens.** The Foundation UI had no store (season premium lanes, collection reacquire, 05 §1.20
  entitlements, the 13–17 spending caps), no report-player or support-ticket panel, and no panel for
  `TerminalDef`'s Customization service. T27 and the admin console had no case queue.

08 changed as follows:
- **New §2.1.1 (the reviewer's option 1, "keep the claim").** 02's concurrent round-3 rewrite already builds
  whole images at one ISA level, with separate `<module>@base` builds for the launcher. 08 makes `base` truly
  x86-64-v1 on every compiler. Wide code in the launcher is allowed only in listed CPUID-dispatched functions
  (BLAKE2b SSE4.1/AVX2, zstd BMI2, SDL3 blitters and converters), so CL-10 and CL-11 are unaffected. The audit
  rejects SSE3-or-later, POPCNT, LZCNT, MOVBE, CMPXCHG16B, VEX, EVEX and BMI encodings outside those symbols.
  The rejected alternative (declaring v2 the floor) is recorded. §2.2 lists BMI2, which the `avx2` level
  compiles for, and uses exit code 78 and 02's gate placement (`init_seg(compiler)`, `.preinit_array`).
  **CL-17** is rewritten: pre-v2 runs under SDE `-mrm`/`-pnr` (`-chip-check-exe-only`) and qemu
  `Opteron_G1`/`core2duo`/`phenom` for the launcher, setup, bootstrap and every `avx2` image, plus the
  v2-without-AVX2 runs and the audit clause.
- **§1.7.2, three new panels (27 in total), each under CL-23's flows.**
  - *Store* (Ph4, WP-4.9): catalogue, owned entitlements and receipts, real-money checkout in the system
    browser or the storefront overlay, grants only by provider webhook, premium-currency purchases as
    protected actions, and 13–17 allowance display with randomized offers hidden.
  - *Support and report* (Ph3, WP-3.8): reports from any name or the 60 s voice speaker list, with
    tag-verified chat lines and forwarder voice snapshots; support tickets with receipts, ledger references
    and logs; a "My cases" thread with SLA expectations.
  - *Customization* (Ph3, WP-3.8): restyle and liveries.

  The Progression row gains the reacquire flow and *Unlock premium*. §1.7.3 gains reject paths and the
  fake-provider and admin-queue fixtures. §1.12's protected actions add store purchases, restyles,
  reacquires and reports. §2.3 gains a *Can't sign in?* link and the product descriptor gains
  `endpoints.support`. §4.3–4.8 are updated to match.

| # | File | What changed | Why |
|---|---|---|---|
| 28.1 | 02-engine-runtime.md (§1.1 ISA table `base` row; self-dispatch bullet; audit checks 2, 4 and 5) | `base` is x86-64-v1 (`-march=x86-64 -mtune=generic`, no `-mcx16`; MSVC default). Check 2 (gate object) and check 4 (`base` images) also reject SSE3–SSE4.2, POPCNT, LZCNT/TZCNT, MOVBE and CMPXCHG16B. Check 5 adds SDE `-mrm`/`-pnr` and qemu `Opteron_G1`. The self-dispatch list names BLAKE2b's SSE4.1/AVX2 and SDL3's CPUID-guarded functions | GCC/Clang `base` was `-march=x86-64-v2` while MSVC's was SSE2, and neither the audit nor the emulator could catch a v2 instruction. 02's RT-09 row and its risk row were reworded by 02's own concurrent round-3 pass and already name the x86-64-v1 gate object, the `base`-image check and the SDE and qemu runs; 00 ADR-011's amendment likewise says `base` is x86-64-v1 |
| 28.2 | ../PLAN.md (§4.2 ISA bullet; §2 Client row; 08 summary) | ISA levels as in 02 §1.1, with the launcher and the gate at x86-64-v1 and pre-v2 emulation behind the "any x86-64 CPU" claim. 27 Foundation panels, including the store, reports and support tickets | The old bullet described the removed per-file allowlist; the panel count was 23 (24 after 05's Fleet row) |
| 28.3 | 05-backend-services.md (§1.10 evidence tags; §1.16 Trust phases; §1.17 **cases**; §1.20 **Store API** and default caps; §5 admin console) | Chat messages carry `msg_id` and a keyed BLAKE2b-128 evidence tag (daily key, kept 30 d). Trust stores report evidence from Ph3. The case model covers reports, tickets, trust hits and appeals, with P1–P4 SLA targets (first response 1 h / 8 h / 24 h / 72 h; resolve 24 h / 72 h / 5 d / 14 d), pause in `waiting_player`, breach flags and `senior_gm` escalation, player and staff APIs, `OpenAccessTicket`, limits and merging. Store: `ListOffers`, `CreateCheckout`, `CheckoutStatus`, `SpendWithPremium`, `ListEntitlements`, `ListReceipts`, `GetSpendStatus`; €50/€100 monthly caps for 13–15/16–17; randomized offers refused for minors and in listed countries; a dev fake provider. The admin console gets a case queue with SLA columns | The panels need server calls. Local chat is never stored, so evidence must be verifiable without history. 04 §2.7 sends voice snapshots to Trust in Ph3, but Trust was Ph4 |
| 28.4 | 06-gameplay-framework.md (§10) | `Restyle{vector, quoteVersion}` inside a `TerminalSession` (species and body type locked, creation range and unlock checks, a quoted `Sink.Customization.Restyle`), and `SetLivery{ship, livery}` on per-instance livery data | The `Customization` terminal service had no server semantics |
| 28.5 | 07-editor-and-tools.md (T27) | A Ph3 case queue: filters, due times, time left and breach badge, evidence viewer, *Go to subject*, *Claim*, macros, *Link sanction*, audited evidence views | The reviewer asked for a case queue with SLA fields in T27 |
| 28.6 | 09-roadmap-and-process.md (WP-0.17, WP-3.3, WP-3.8, WP-4.4, WP-4.9) | WP-0.17: an x86-64-v1 launcher with pre-v2 emulator runs. WP-3.3: cases, tickets, evidence tags. WP-3.8: collections reacquire, support and report, customization; now depends on 3.3 and 3.4. WP-4.4: the Store API, webhooks and caps. WP-4.9: the Store panel; now depends on 4.4 | Every new panel and service has an owning WP and its dependencies |

No criterion ID was added or renumbered. CL-17 was rewritten in place, and CL-23 gained a clause naming the
Store and Support fixtures.

## 29. Round-3 review fixes owned by 02 (2026-09-25)

One major tools-lens gap and two minor engine-lens gaps in 02:
- **Major: studio schema types had no data-only path.** Every `.hschema` package compiled to C++ and Go. One
  new record type, `ScriptState` component or view-model therefore needed a compiler on every designer
  machine, a ≤ 30 s DLL reload, a studio-linked client and cell, and a rebuilt `helios-backend`. That
  contradicted 01 §1.1, 08 §2.10.4, 09 §2.7.4 and AAA-ITR-6, and no criterion added a type.
- **Minor: §1.1's ISA allowlist had a physics-shaped hole.** Jolt's headers pick AVX paths inline and
  `third_party/CMakeLists.txt` makes `tp_jolt`'s ISA options PUBLIC. So every `physics` TU needed AVX2, and its
  COMDAT instantiations could win link-time picks that no per-object check sees. 09 §8.1 listed no delta.
- **Minor: cell-side terrain collision tiles had no workload model and no consistency rule.** A fixed 2 km
  radius at ~72–128 m tiles is thousands of tiles per player. No stage budgeted it, and a coarser fallback
  on either side would break §5.4's bit-identical client/cell contacts.

02 changed as follows:
- **§1.1: whole-image ISA levels (the reviewer's option 3, recorded as an ADR-011 amendment).** Every image
  that links runtime modules is built at `avx2` (AVX2, BMI1/2, LZCNT, POPCNT, F16C, no FMA). The launcher and
  bootstrap are `base`, and so is one TU inside every `avx2` image: the CPU gate. The gate runs before any
  other Helios-compiled code (`init_seg(compiler)` in the first-initialized image, `.preinit_array` on
  Linux), uses no STL, defines no COMDAT or weak symbol and calls only allowlisted OS entry points.
  - Rationale: after the gate every CPU has AVX2, so an AVX2 COMDAT pick is harmless there. Only the pre-gate
    path and `base` images need auditing.
  - The audit has five checks: flags per level; the gate object; the pre-gate path of every `avx2` image (no
    TLS dynamic initializers, IFUNCs or low-priority constructors, and a `.dynsym` allowlist); post-link
    mapping of every wide instruction in `base` images to its symbol through the PDB or DWARF; and emulation.
  - `pcg`'s kernels need no per-file flags and no CPUID dispatch; `pcg.kernel` selects the width.
  - §7.1 adds small-bubble stepping, with per-worker temp allocators for bubbles of ≤ 64 bodies.
  - 08's concurrent round-3 pass (§28) then set `base` to x86-64-v1. This pass aligned check 1 and RT-09 with
    that.
- **§5.8a: terrain collision tiles.**
  - *Collision level:* one level per body, with a spacing in (0.75, 1.5] m (Harrow: level 15, ≈ 72 m
    tiles).
  - *Tile sets:* the step box, the contact set and a lookahead set (2 s of travel, grown by ½·a_max·T²),
    each a pure function of body state, and filtered by a height slab from the level-(L_c − 3) ancestor
    widened by a static `slabMargin`.
  - *No substitution:* physics never uses a render tile, a GPU tile or a coarser ancestor. A stage 3
    `TerrainFence` builds missing contact tiles synchronously, which changes no result. Past its cap
    (16 tiles per tick on cells), bodies are held, counted in `physics.collision_tile_miss` and logged
    as `CollisionHold` replay events. The predicting client suspends prediction instead of holding.
  - *Queries:* terrain casts truncate deterministically at 16 visited tiles.
  - *Budget and cache:* prefetch gets ≤ 1,000 core-ms/s on the ground-hub profile, with a degradation
    ladder. The LRU cache is capped at 0.4 GB inside the PCG tag.
  - *Workload model:* ≈ 370 tiles/s and ≈ 0.25 GB for 500 dispersed players, 50 vehicles, 10 ships in low
    flight and 2,000 NPCs.
  - *Contract:* §5.8's visual-versus-collision contract gains an interpolation bound, checked by T04.
  - *Criteria:* new **RT-20**; RT-06 and RT-11 require 0 misses.
- **§3.8: project schema packages.** Packages are native (C++ and Go) or dynamic. Dynamic is the default for
  project packages.
  - `schemac --emit types` writes a `.htypes` bundle, split into client and server parts.
  - `reflect` registers dynamic `TypeInfo` with a load-time layout, type-erased containers and generic
    `TypeOps`.
  - flecs runtime components; `DynMut` dirty bits; `repl::describeDynamic`; Luau userdata with atoms and
    generated `.d.luau`; `DynRecordView` and HXL field triples; `ui::DynViewModel` with table-driven
    `@source` adapters.
  - Go `pkg/htypes` gives the prebuilt backend and the collab service readers and validators.
  - Native-only constructs are rejected, and the `schema.dynamic-hot` lint sets CI budgets.
  - A data-only change is live in ≤ 5 s with no DLL. §6.4, §7.4 and §7.5 point at §3.8.
  - New **RT-21**.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 29.1 | 00-decisions.md (ADR-011) | New bullet: ISA levels (`avx2` images; `base` launcher, bootstrap and gate TU; gate placement and rules). It replaces the per-file allowlist | The reviewer asked for the whole-image model to be recorded as an ADR-011 amendment; ADR-013's "AVX2 baseline (launcher checks CPUID)" already agrees |
| 29.2 | 03-rendering.md (§5.5a CPU cost model) | The 8 lanes are `pcg`'s default `vm_avx2.cpp` kernel in an `avx2` image after the gate; the 4-lane twin is never budgeted | "Allowlisted" and "CPUID selects" no longer hold |
| 29.3 | 04-networking-and-servers.md (§3.3 stage 3; §10.2 table) | Stage 3 starts with `TerrainFence` (≤ 1 ms p99). A replay row: synchronous tile builds log nothing, and holds log `CollisionHold{tick, entity}` | 04 owns the tick graph and the replay contract |
| 29.4 | 05-backend-services.md (§1.14 collab service) | Validates native packages with generated Go and dynamic packages with `pkg/htypes` | The prebuilt collab service must accept project types |
| 29.5 | 07-editor-and-tools.md (T08, T04, T19, §5.1, **new ED-22**) | T08 gains a governed schema editor for project packages (transactions under a T30 lock, migration preview, compat-epoch flag, lint gate, publish PR review, live in ≤ 5 s). T04's budget analyzer checks the collision interpolation bound and `slabMargin`. T19 binds dynamic view-models. ED-22 (Ph2): with no compiler, on the packaged artist editor, a designer adds a record type, a `ScriptState` component used by Luau and a view-model bound in a new T19 screen, all live in PIE in ≤ 5 s, then packages with the prebuilt stamped client and cell (the CL-24 route) | The reviewer asked for the T08 editor and ED-22; the read-only browser contradicted HeroEngine's DOM Editor (R03 §2.4) |
| 29.6 | 08-client-and-launcher.md (§2.10.4; §4.8) | The prebuilt client covers project schema types shipped as `.htypes` data. §4.8's "keep Jolt's AVX2 flags PRIVATE, or exempt the gate TU" is marked resolved | Keeps the "enough for Luau-only projects" claim true. (08's own §28 already aligned §2.1–2.2 with 02 §1.1) |
| 29.7 | 09-roadmap-and-process.md (WP-0.2, WP-0.9, WP-1.3, WP-2.2, WP-2.16; Ph1 and Ph2 exits; §2.7.4; §8.1) | WP-0.2 builds the ISA levels and the five-check audit. WP-1.3 carries §5.8a and RT-20's Ph1 slice. WP-2.2 carries small-bubble stepping and the full RT-20. WP-2.16 carries the dynamic packages, T08's schema editor, RT-21 and ED-22. The Ph1 exit adds RT-20 (slice); the Ph2 exit adds RT-20, RT-21 and ED-22. Every template's gem declares a dynamic project type, and each `template-proof` adds one through T08 and fails if a template package is native. §8.1 gains an **ISA levels** row with the WP-0.2 deltas: `tp_jolt`'s PUBLIC `/arch:AVX2`/`-mavx2 …` options, no `avx2` or `base` flag set anywhere in `CMakeLists.txt`, `cmake/` or `CMakePresets.json`, no `-mbmi2`/`-mno-fma`, and no gate TU or audit | Every new clause has an owning WP and a phase exit; the reviewer asked for the deltas in 09 §8.1 |
| 29.8 | 01-vision-and-scope.md (§3.9.1 TOOL-10 brief) | The UI-screen deliverable binds a new project view-model, and the zone declares a new record type and `ScriptState` component through T08's schema editor | The reviewer asked for the TOOL-10 brief to declare at least one project type |
| 29.9 | ../PLAN.md (02 summary; §9 criteria list) | The 02 summary names whole-image ISA levels, dynamic project packages (RT-21) and terrain collision tiles (RT-20). The criteria list reads RT-01…21 and ED-1…22 | Keeps the summary truthful |

New IDs: RT-20, RT-21 and ED-22. RT-06, RT-09 and RT-11 gained clauses. No ID was renumbered.
**Checked, no edit needed:** 03 §5.5a's level-adaptive rule (levels coarser than the collision level skip
octaves) matches §5.8a's single collision level, whose spacing is ≤ 1.5 m < 2 m. 04 §3.1's `ScriptState` rule
holds for dynamic components. 06 §11 needs no change, because its scripts read components through the same
Luau API.

## 30. Round-3 review fixes owned by 06 (2026-09-25)

One minor tools-lens gap in 06, with three parts:
- **No controller spec.** Ground vehicles and mounts were one line in §8.1 ("`Hover`, `Wheeled` and
  `Mount`"): no engine or gear model, suspension, hover height, boost, or animation-driven mount locomotion.
- **No input channel and no prediction contract.** 04 §5.3 covered only hulls and characters, and no GP
  criterion measured vehicles.
- **Phases disagreed.** M03 (critical for SWG, DST, SC and TOR) was Ph1 in 01 §2.6 and claimed by WP-1.16,
  yet vehicles sat in 02 §8.1's Ph2 column and in WP-2.2.

06 now has:
- **§8.1a Ground vehicles and mounts.** A `VehicleDef` with three models:
  - `Wheeled`: Jolt `VehicleConstraint` with engine, gearbox, differential, wheel, anti-roll and tester
    records. It has one chassis body, and wheels are casts. There is a governor, boost, traction assist and
    auto-reverse, and tracked and motorcycle drivetrains come in Ph3.
  - `Hover`: ray suspension with explicit spring and damper formulas, trim-adjustable height, `rayLength`
    (no lift beyond it), tilt and slope limits, grip and drift, yaw control, and drag against §8.2's
    scheduled wind.
  - `Mount`: a `CharacterVirtual` mount mode on the shared mover, with gait tables cooked from clip root
    motion (animation-driven), stamina, a front probe, jumps on cooked root-motion tracks, rider attach and
    dismount, and rider and mount animation sets with IK.
- **Shared rules.** An envelope (≤ 100 m/s, ≤ 8 m/s², matching 02 §5.8a's workload row); 60 Hz collision
  steps in vehicle bubbles; seats; mounted-combat modes; server-only impacts; replication (≈ 26 B per
  update); 04 §5.5 caps; ownership as an authority attachment, with passengers on `Fence.Join`; call and
  store; per-model µs costs; and authoring.
- **The `Drive` input channel.** 6 bytes, in place of `Flight`'s axes, with device shaping before
  quantization.
- **A prediction contract** under §8.2's bit-exact rules. Wheeled and hover vehicles go through
  `SingleBodyPredictor`, and mounts through the character mover. The constraint's `SaveState` is in the
  snapshot, and cast ties break by `TileKey` or entity ID, never `BodyID`. A 32-bit state hash rides in each
  owner snapshot, with the full state attached on a mismatch. Corrections are tagged by cause, and `terrain`
  and `divergence` must be 0.
- **GP-4d (Ph2).** A 6-layout hash (8 layouts in Ph3) across every determinism toolchain, at 1, 4 and 16
  workers, and between cell and predictor, run in RT-03's job. A 30-min Harrow soak of 100 bot speeders,
  20 rovers and 30 riders at 100 ms RTT and 1 % loss: < 1 % mispredicted ticks, 0 `terrain` and 0
  `divergence` corrections, and 0 vehicles under the surface. A 60 Hz, 32-rider swoop race.
- **Elsewhere in 06.** §8.4 lists `Drive`. §9.5 covers vehicle deeds and creature mounts. §1.4 names the
  `SetMovementOverride` modes. §12.1–12.4 are updated: ladder, GP-4d, a new risk row, traceability and
  dependencies.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 30.1 | 01-vision-and-scope.md (§2.6 M03) | Ph becomes "1 (vehicles, mounts: 2)" | On foot ships in Ph1 (WP-1.16); vehicles and mounts in Ph2 (02 §8.1, WP-2.2, WP-2.9). 01 owns phases, and this matches the rest of the plan |
| 30.2 | ../PLAN.md (§7 M03 row; 06 summary; §9 criteria list) | Ph becomes "1 (vehicles, mounts 2)". The 06 summary gains a ground-vehicle and mount bullet, and the criteria list reads GP-4 as 4a–4d | Keeps PLAN.md §7 row for row with 01 §2.6, and the summary truthful |
| 30.3 | 09-roadmap-and-process.md (WP-1.16, WP-2.2, WP-2.9, WP-3.6; Ph2 and Ph3 exits; §3.2 #15) | WP-1.16 claims M03 on foot only. WP-2.2's "vehicles" names the Jolt integration, the 60 Hz vehicle steps and `SingleBodyPredictor`'s constraint state. WP-2.9 delivers the three models, `Drive`, prediction, vehicle deeds and creature mounts, depends on WP-2.2, and accepts GP-4d and M03 (vehicles, mounts). WP-3.6 adds tracked and motorcycle drivetrains, mounted combat, vehicle turrets and hangar drive-out, with GP-4d's Ph3 layouts. The Ph2 exit lists GP-4d, and the Ph3 exit lists 4d (Ph3 layouts). Row #15 records the phases | Every new criterion has an owner WP and a phase exit, and M03 no longer has two phases |
| 30.4 | 02-engine-runtime.md (§7.1 Vehicles and Netcode hooks; §8.1 Physics row; RT-03) | Vehicles: Phase 2, single-body step listeners, 60 Hz collision steps in vehicle bubbles, and cast ties by `TileKey` or `EntityId`. `SingleBodyPredictor` gains ground-vehicle prediction through Jolt's `SaveState`/`RestoreState`. RT-03's job also runs GP-4d's hash from Ph2 | 02 owns the physics integration that 06 §8.1a and GP-4d rely on |
| 30.5 | 04-networking-and-servers.md (§5.3; §3.3 stage 3) | §5.3 gains a ground-vehicle and mount paragraph: `Drive` in the input record, `SingleBodyPredictor` with constraint state, mounts on the character mover, and a hash-then-full-state owner correction. Stage 3 substeps vehicle bubbles at 60 Hz | The reviewer noted that 04 §5.3 covered only hulls and characters |
| 30.6 | 07-editor-and-tools.md (T21, T15, §2.6.6) | T21 gains vehicle ports, drivetrain plots and the envelope preview (Ph2 MVP: speeder bike, landspeeder, rover). T15 gains mount and rider sets with gait extraction and cooked root-motion tracks. New T28 rules: `phys.vehicle.envelope` and `phys.vehicle.def` (Error, Ph2) | The authoring side of §8.1a, and the lint behind its envelope |

New criterion: **GP-4d**. No ID was renumbered.
**Checked, no edit needed:** 02 RT-20 already puts 50 ground vehicles at 100 m/s in its full (Ph2) run and none
in its Ph1 slice. 08 §1.5 already has a `Vehicle` input context. 00 ADR-013's Jolt settings
(`JPH_CROSS_PLATFORM_DETERMINISTIC`, double precision) cover `VehicleConstraint` unchanged.

## 31. Round-3 review fixes owned by 03 (2026-09-25)

Two minor engine-lens gaps were closed.

**Budgets did not cover every gated scene, tier and frame rate.**
- **§8.1 is restructured.** A frame-target table maps each AAA-REN-1/2/3 gate to its scenes, preset, frame
  period, p99 and GPU wall target.
- **§8.1.1 (REF):** six columns: BENCH-1, 2, 3 (45 fps), 4, 4 in Performance mode (120 fps; 7.8 ms sum, ~7.2 ms
  wall, which plus CL-6's 0.3 ms slack is 08 §1.3a's 7.5 ms) and 5.
- **§8.1.2 (MIN):** five columns: BENCH-1, 2, 3 (30 fps), 4 and 5 (30 fps).
- **Characters row:** the old "Scatter, skinning, cull" row is split into cull and Characters (skinning,
  FACS shapes, rest-mesh bakes). Existing sums are unchanged: REF BENCH-2/3/5 stay 15.3/19.9/15.8 and MIN
  BENCH-2 stays 12.1.
- **MIN BENCH-3 has its own plan:**
  - the mesh band starts at 64 display px on Low (≤ 120 hulls), and fleet geometry is ≤ 6.5 ms;
  - capital maps are 2 × 2048² D16;
  - the 256k significance pass stays;
  - an 18.2 ms sum and a volley p99 ≤ 28 ms.
- **§8.1.4:** a MIN VRAM check fills every §1.3 line for BENCH-1 (≤ 4.2 GB) and BENCH-3 (≤ 4.35 GB).
- **§4.5:** capital maps per preset, all D16 (4 × 4096² High/Ultra, 4 × 2048² Medium, 2 × 2048² Low). The
  local-light atlas is D16, and a shadow-memory table shows Low 60 MB / 0.25 GB, High 320 MB / 0.5 GB and
  Ultra VSM 392 MB. The old 4 × 4096² D32 capital maps alone were 256 MB.
- **§8.1.5:** render CPU budgets at 60, 45, 30 and 120 fps. The render thread gets ≤ 4, ≤ 5, ≤ 3.5, ≤ 6 and
  ≤ 7 ms.
- **§8.1.6:** Performance mode is specified.
- **§1.3:** the MIN upload ring is 64 MB.
- **§8.2** gains rows for capital maps, the local-light atlas, the fleet mesh band, character geometry caps
  with F0 faces, and skin.

**Character geometry and the facial runtime were underspecified.**
- **§7.6a** gains *Geometry at scale*:
  - `Morph` and `DnaBlend` are baked once per appearance and LOD into a 16 B/vertex rest-mesh cache (LRU in
    the geometry pool; REF 96 MB, MIN 48 MB). `BoneScale` goes into 02's appearance skeleton.
  - Per-tier vertex maxima are set.
  - A tiered skinned-output ring holds 32 B/vertex with previous positions for hero and A0, and 20 B for
    A1–A2 (REF 87 MB and MIN 38 MB worst case in BENCH-1).
  - Staggered 30/15 Hz reskins cost ≤ 0.6 ms (REF 2.3 M and MIN 0.93 M vertices per frame).
  - A base-mesh fallback covers the time until a bake lands.
- **§1.3** gains a geometry sub-line for these caches.
- **New §7.6b (faces, GPU side):** 52 sparse FACS deltas per head basis (≈ 1.4 MB), evaluated only for
  F0 faces (≤ 8 REF, ≤ 4 MIN/Performance), ≤ 0.15/0.1 ms, with wrinkle maps, eye shells and goldens.

**Criteria.** RC-8 covers all REF columns. RC-9 covers MIN and Performance columns, the VRAM check, MIN
BENCH-3 p99 ≤ 50 ms and the volley peak, and RT-12's clauses. RC-10 adds the ring and rest-mesh caches and
baked shapes ≤ 1 s. RC-11 Ph4 adds FACS goldens and the face budget. §9.2, §9.4, §9.5 and §9.6 are updated.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 31.1 | 02-engine-runtime.md (§2.4) | New per-thread and per-system tables for REF BENCH-3 (45 fps), MIN BENCH-3 and 5 (30 fps) and REF BENCH-4 Performance (120 fps). The 120 fps game thread is ≤ 6.0 ms on tick frames and ≤ 4.0 on others (p99 ≤ 8.0), the render thread ≤ 3.5, and the whole process ≤ 30 of 67 core-ms. The tick/Phase A interleave (60 Hz ticks on alternate frames) and performance `cpu.*` settings come with them. The CPU-bound frame definition uses each scene's frame period | 02 §2.4 owns the CPU tables. The REF 8 ms game thread could not fit an 8.33 ms frame |
| 31.2 | 02-engine-runtime.md (RT-12; §8.1 ladder; §8.4 risks; §8.6) | RT-12 gains a **30/45 fps clause** (nightly from Ph2/Ph3, gates at the Ph3 exit on the WP-2.15 capture, re-run at Ph4) and a **120 fps clause** (nightly from Ph3, gates at the **Ph4 midpoint**) | REN-2/3's CPU side is measured before the Ph4 exit |
| 31.3 | 02-engine-runtime.md (§7.2; new **RT-22**) | A1/A2 updates are phase-staggered. `BoneScale` is applied once per appearance in the bind skeleton. The **facial runtime** adds FACS curve clips, a viseme layer from assetd phoneme alignment with coarticulation, an emotion layer, eyes (look-at, saccades, blinks, seeded), a clock tied to the voice playback at display time, face LOD F0/F1/F2, and 52 unorm8 weights per F0 face in the extract packet. RT-22 (Ph4) sets lip-to-audio p95 ≤ ±40 ms and eye statistics, a cost of ≤ 0.05 ms per F0 face, and holds with the viseme fallback | R04 and REN-8 L5 judged a runtime that had no spec, budget or test |
| 31.4 | 09-roadmap-and-process.md (WP-2.3, WP-3.4, WP-4.1, WP-4.6; Ph4 exit) | WP-2.3 adds rest-mesh bakes and the ring. WP-3.4 carries RT-12's 30/45 fps clause and WP-4.1 its 120 fps clause. WP-4.6 names the 02 §7.2 and 03 §7.6b runtime and RT-22, which joins the Ph4 exit list | Every new clause and criterion has an owning WP |
| 31.5 | 07-editor-and-tools.md (T22) | A per-tier vertex lint against 03 §7.6a's maxima | The skinning and ring budgets rely on it |
| 31.6 | 08-client-and-launcher.md (§1.3a latency table, row 6) | The GPU row cites 03 §8.1.1's walls (REF 60 fps ~13.0–14.5 ms, Performance ~7.2 ms); the row's values (14.5 and 7.5 ms) are unchanged | The old citation predated the BENCH-1/4 and 120 fps columns |
| 31.7 | ../PLAN.md (02 and 03 summaries; criteria list) | The summaries name the new frame-rate columns, the character geometry twin and faces. The list reads RT-01…22 | Keeps the summary truthful |

New ID: **RT-22**. RT-12 gained two clauses; RC-8…11 gained clauses. No ID was renumbered.
**Checked, no edit needed:** 08 §1.3a's 120 fps rows 3–6 (Phase B 4.0, extract 1.0, render critical path
2.2, GPU 7.5 ms) hold under the new budgets: Phase B ≤ 3.5, extract ≤ 0.8, prepare + compile + batch 0
≤ 1.8, and a GPU wall ≈ 7.2 + 0.3. 08 CL-7's 2,000 brackets ≤ 1.0 ms CPU is the UI row of the REF BENCH-3
column. 04 §3.4's zone rates (fleet 2 Hz, hub 20 Hz, activity 60 Hz) set the tick shares. 06 §10's parameter
kinds are consumed as written.

## 32. Round-3 review fixes owned by 07 (2026-09-25)

One minor tools-lens gap in 07: **collab session scope for documents with no zone.** §1.8.1 gave one actor
per zone session, and §1.9's web tools submitted `collab.submit` without naming a session. Items,
abilities, loot tables, quests and dialogue are global, so two zone sessions could each accept conflicting
edits to one record within 1 s. Soft locks lived in `LOCKS_<session>`, so a lock in one session did not
protect the record in another, and the clash surfaced only later, as a 3-way merge at rebase or publish.

07 now has **§1.8.2 Session scope: zone sessions, the data session and document homes**:
- **A project-wide data session.** `data` sits beside one `zone-<zoneId>` session per edit instance.
  Every editor joins it at project open. In Ph2 it carries the Tier B soft locks and presence; in Ph3 it
  also has a transaction stream.
- **Document classes.** `spatial` documents (under `content/zones/<zone>/`) always live in their zone
  session. `global` documents default to `data`, and each may be checked out to exactly one zone session.
  `pinned` documents (schemas, tags, physics layers, settings) never leave `data`. The class is declared by
  `DocumentTypeDesc.scope` (§1.10).
- **Routing.** Editors send each transaction to the one session that homes all of its documents. A
  transaction that spans a zone's spatial documents and global documents first checks the global ones out,
  all or none. An actor rejects any document it does not home with `moved` or `inTransit`.
- **Handoff protocol.** Every home change goes through the data actor, so `COLLAB_data` orders them all.
  The releasing side's `handoff` or `release` event is the commit point, and the other side adopts it
  idempotently, including after a takeover. Request → adopt is ≤ 500 ms p95. Automatic check-in happens
  after 30 min idle, on publish and at session end. `HOMES_data` is a read-only KV mirror, so no ownership
  state lives in NATS KV (05 §0 rule 8).
- **Locks and presence.** Soft locks on global documents are project-wide in `LOCKS_data`, and presence
  for them goes on `collab.data.presence`.
- **Cross-stream order.** `deps{session → seq}` gives causal order between streams. Composed overlays
  (base + data + zone) drive edit instances, late join and publish preview. A delete of a document that
  another session references needs confirmation.
- **Publishing.** Carry publishes of the earlier holder's dependency-closed prefix are queued first, and
  an adopted document's merge base is its adopt content. Two sessions therefore never merge against each
  other.

§1.8 (sessions bullet, channel table, submit step 1, locks, late join, rebase, publish and preview), §1.8.1
(R3 buckets, one actor per session, writer-based warning), §1.9 (the web tools submit to the data session),
§1.10, T08, T30, §2's common contract, §4.1, §5.1, §5.3 and §5.5 now point to it. **ED-20** gains a
cross-session clause: a second zone session and the data session edit 2,000 overlapping global documents
beside Saltmarch for 4 h, with checkouts every 10 s and two data-actor kills. It requires 0 publish-time or
rebase conflicts on documents held under another session's home or lock, a home audit with 0 overlapping
write epochs, 0 dangling references and export diff = 0 per session. A seeded direct commit must produce
exactly one conflict, as a negative control. A 30-minute cut runs nightly.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 32.1 | 05-backend-services.md (§1.14 collab service) | The collab service is also the only writer of `HOMES_data`. There is one session per zone edit instance plus one project-wide data session that homes documents with no zone and holds their locks and presence. Home changes are fenced handoff and adopt events (07 §1.8.2) | 05 lists what the collab service writes and hosts |
| 32.2 | 09-roadmap-and-process.md (§5.2a content tier step 1; WP-2.11; WP-3.7; §6 test-matrix collab row) | A publish that depends on another session's journal enters the queue only after its `collab/<session>-carry-<n>` PRs merge, and carries go to the head of the queue. WP-2.11's T30 locks and presence are project-wide in the data session. WP-3.7 delivers the data session, document homes and carries. The test-matrix row adds the cross-session runs | 09 owns the merge queue, WP scope and the test schedule behind ED-20 |

No criterion ID was added or renumbered (ED-20 was extended).
**Checked, no edit needed:** 09 §5.2a steps 2–5 already hold for carries: they touch only `content/**`, and
their `Helios-Tx` ranges are a dependency-closed partial publish that the service recorded. 00 ADR-009 and 01
§1.3 pillar 3 describe edit instances generally and stay true. 02 §3.8's dynamic packages are validated by the
data session's actor like any other session's, through `pkg/htypes`.

## 33. Integration pass 4: round-3 revisions (2026-09-25)

Inputs: a re-read of PLAN.md, README, 00–09 and this log after every round-3 revision (§26–§32), plus
automated checks. No reviewer gap was filed against PLAN.md itself in round 3. Each section's round-3 fix had
touched only its own PLAN.md lines (and 07's §32 touched none); this pass reconciles what fell between them.

**New cross-section inconsistencies found in the re-read.**

| # | File | What changed | Why |
|---|---|---|---|
| 33.1 | 02-engine-runtime.md (§3.1 declaration kinds; §3.2 Script row) | `worldscript` joins the declaration kinds, with a pointer to 05 §1.23 for its members; `@realm` gains `world` (world-script hosts) | 05 §1.23 declares a `worldscript` block that schemac validates and a `world` Luau realm, 09 §2.7.1 documents both, and 02 §3.8 already allowed `worldscript` in dynamic packages, but 02's normative grammar listed neither |
| 33.2 | 02 (§1.3 cellserver row; §7.4 VMs) | `--role world-script` runs `helios-cell` as a world-script host with no zone simulation (Phase 3). The host runs one VM per world-script partition under the cell sandbox, fuel and heap rules, with the `world` realm's API | 05 §1.23 and §13 make the cell binary the world-script host "with the 02 §7.4 sandbox", but 02's build targets and script host did not know the mode |
| 33.3 | 04-networking-and-servers.md (§1 process table; §11.1 layout) | A **World-script host** row (owns partitions under region leases; never zone simulation, clients, SQL or debiting a character), and `--role world-script` in `apps/cellserver` | 04 §1 lists every process role and what it must never do |
| 33.4 | 04 (§2.6 box-loss bullet) | "At 50k CCU this means 8 boxes over 2 zones" → the box rule alone needs 8 boxes over 2 zones; the zone rule raises the deployment to 12 over 3, and the 8 are what survive a zone loss | Read as a deployment size, the sentence contradicted the zone-rule bullet below it, 04 §1's gateway row, 05 §6.7 and 09 WP-4.3. Fix 26.1 added the zone rule but left the sentence |
| 33.5 | 01-vision-and-scope.md (AAA-TOOL-7) | The Ph3 clause adds 100 % of the world-script API (the `world` realm and the `worldscript` block, 05 §1.23) | Fix 26.4 put the world-script API in 09 §2.7.1's Ph3 coverage gate and WP-3.12 accepts TOOL-7 (Ph3) with it, but 01's row named only the C++ game-module and editor extension APIs |
| 33.6 | 01 (§1.1 Backend row and north star; §3.9.1 Spawns row; §6 glossary) | The backend part lists world scripts. The north star names the two round-3 mechanisms that keep it true beyond content: dynamic project types (02 §3.8, RT-21) and world scripts (05 §1.23, BE-A20). "≥ 3 AI archetypes" → "≥ 3 AI record templates". Glossary entries: project schema package, world script, planned migration, player fleet, document home | The glossary had none of the round-3 terms that four sections now use; "AI archetypes" broke ADR-004's terminology rule (§1's check) |
| 33.7 | 00-decisions.md (ADR-007 roadmap; ADR-008) | ADR-007's v1 adds planned region migration (≤ 1 s hitch, no state loss; 04 §6.7). ADR-008 gains a round-3 amendment: studios extend the backend with world scripts (sandboxed Luau, declared tables, escrow-only value); a public Go module API is rejected because Go plugins do not work on Windows and would bypass the sandbox | Both are architecture decisions made inside sections in round 3. Fix 29.1 recorded 02's ISA decision in ADR-011 the same way |
| 33.8 | 00 (ADR-013 Physics row) | The golden-hash test runs on every determinism toolchain (both MSVC toolsets, clang-cl, GCC, Clang, MinGW; 02 RT-03), and the CPUID check is the launcher's and each image's CPU gate (ADR-011) | ADR-013 still said MSVC/clang-cl/GCC/Clang, while RT-03, GP-4a and the new GP-4d hash on six toolchains; ADR-011's amendment put a CPU gate in every `avx2` image |
| 33.9 | 03, 05, 06, 07 and 09 status headers; 09 §8.1 Plan row | Draft v4, each listing its round-3 changes, including those that other sections' fixes made there (05: migrations, cases, store, `pkg/htypes`, data session; 06: §8.1a, §8.3a, §10, §11; 07: ED-22, T27, T21/T15, T04, T22; 09: new criteria owned, ISA deltas, carries). 09 §8.1: "Draft v4 (round-3 fixes)" | 02, 04 and 08 already said v4. 06 and 09 listed no round-3 change although §26–§32 edited them, as fix 25.7 found after round 2 |
| 33.10 | 02, 04, 06, 07 and 08 conformance lists | Each adds the ADRs it cites: 02 ADR-001a, -007, -008, -014; 04 ADR-009, -010; 06 ADR-010, -013 (cited by §8.1a's Jolt vehicle determinism); 07 ADR-003, -010; 08 ADR-003 | Same rule as fix 25.7 |
| 33.11 | 07-editor-and-tools.md (§5.1 T15, T21 and T27 cells) | T15 Ph2 adds mount and rider sets; T21 Ph2 "M (incl. ground vehicles)"; T27 Ph3 "GM client, case queue" | The tool text had them (fixes 28.5, 30.6), but the delivery table that 09's WPs read did not |
| 33.12 | 09-roadmap-and-process.md (Ph3 exit) | "RT-12 (MIN clause)" → "RT-12 (MIN and 30/45 fps clauses)" | Fix 31.2 gates the 30/45 fps clause at the Ph3 exit and WP-3.4 carries it, but the exit list omitted it |
| 33.13 | 09 (WP-0.15r; §5.10.4 intro) | The current plan is draft v4. Round 3 touched 05 §1.4 only with scope `services/` has not built (`Drain`, `PreProvision`, `region_migration`, world-script partitions), so it adds no rework row | §5.10.2 requires every plan change to declare its effect on existing code; WP-0.15r still said "draft v3" |
| 33.14 | README.md (phase vocabulary) | Ph2 adds project schema types with no compiler and ground vehicles and mounts. Ph3 adds planned region migration and player reports and support cases. Ph4 adds the in-game store and facial animation and lip-sync | The single phase vocabulary lagged the round-3 features, as fix 25.8 found after round 2 |
| 33.15 | ../PLAN.md (status; §1; §2; §4.1; §4.2; §5; §6; §7; §8.1; §9; §11; §12) | Status v3 → v4. §1: three review rounds; the backend part is extended by world scripts. §2: backend (planned migration, cases, store), editor (ED-22, the data session), scripting (world-script hosts), object editors (T08's schema editor), animation (mount sets, RT-22), launcher (CL-17). §4.1: the world-script host line; v1 gains planned migration (NS-3.11). §4.2: the cell's `--role world-script`. §5: ADR-007, -008 and -011 rows. §6: 00 (the two amendments); 01 (north star, TOOL-10 project types, STB-6); 02 (vehicles, the Luau host for world scripts, 22 criteria, not 21); 04 (gateway-loss rebind, tick-consistent restore); 05 (`Drain` and `PreProvision` as migrations, data session, player support and store); 06 (player fleets, restyles and liveries, `Authority.Adopted`); 07 (document homes, T08's schema editor, T21, T15, T27); 08 (CL-17); 09 (round-3 criteria owned, carries, world-script docs). §7: SWG proofs add vehicles and mounts (GP-4d) and the bounty board (BE-A20); EVE proofs add the Ph3 fleet battle (GP-15) and the Ph4 battle as eight player fleets (NS-4.2). §8.1 gates: Ph1 RT-20 slice; Ph2 RT-20, RT-21, ED-22, GP-4d; Ph3 NS-3.11/3.12, RT-12 30/45 fps, GP-15, BE-A20; Ph4 RT-22 and RT-12's 120 fps midpoint. §9: STB-6's migration bound; TOOL-7's world-script API. §11: the known ISA and SDL3 deltas (09 §8.1). §12: five glossary terms | The summaries, gates and proofs lagged §26–§32 |
| 33.16 | This file (§1) | Check rows updated (criterion IDs, links) and added (§-references, `helios-cell` modes, grammar kinds and realms, headers and conformance lists) | Keeps the automated checks current |

No criterion ID was added or renumbered, and no threshold changed. **Checked and consistent, no edit:**
PLAN.md §7 matches 01 §2.6 row for row (52 capabilities, including fix 30.1's M03 phase); all 1,656
`0N §x.y` references and 133 links resolve; the scorecard still has 65 criteria (REN 8, SRV 12, ITR 8, STB 6,
CNT 7, SEC 8, TOOL 10, PLT 6); the register still has 38 risks, and PLAN.md §10's ten are unchanged; 09 §4.3's
≈ $2.2–13.2M total is unchanged; 08 §1.7.2 has 27 panels; 05 has 23 services; parties (≤ 16) and fleets (≤ 256;
5 wings × 5 squads × 10) agree across 04 §2.7's fleet voice channel, 05 §1.12.1, 06 GP-15 and 08's Fleet panel;
gateway box counts (6 at Ph3, 12 at Ph4, over 3 zones) agree across 04 §1, §2.6, §11.3, 05 §6.1, §6.4, §6.7, §9
and 09 WP-4.3 and §4.3.2; 03 §8.1.5's render-thread walls (REF 4, 5 and 3.5 ms; MIN 6 and 7 ms) match RT-12's
clauses; 04 §6.7's "06 §11 rule 7" is the `ScriptState` rule; and every round-3 criterion has an owning WP and
a phase exit (RT-20: WP-1.3 and 2.2; RT-21 and ED-22: 2.16; RT-22: 4.6; GP-4d: 2.9 and 3.6; GP-15: 3.2, 3.6
and 4.5; BE-A20: 3.12; NS-3.11 and 3.12: 3.1, with NS-3.12's Ph4 rerun in 4.3).

## 34. Round-4 review fixes owned by 05 (2026-09-25)

Gaps in 05 from the backend lens:
- **Minor:** gateway deploys and host maintenance had no protocol. The §1.14.1 release table said players see
  nothing, but netcode has no session migration, so stopping a gateway was NS-4.7's kill (up to 8k linkdead
  sessions for ≈ 6–10 s). A14 and AAA-STB-6 tested only cell rolls.
- **Minor:** fence write load on the shard primary was unbudgeted. §3.6 sized only ledger and checkpoint
  traffic, and A5 tested the ledger alone. Tree-wide `FOR UPDATE` on a busy tree contends with its members'
  `FOR SHARE` ledger checks.

05 changed as follows: a new §6.3.1 (make-before-break relocation, `DrainGateway`, the Phase 3 paced-reconnect
path); `MintRelocation` in §1.3; `DrainGateway` in §1.4; §1.13's write-load and contention rules; §1.14.1's
release table; §2.4, §3.6, §4.1, §6.2, §6.7, §6.8, §8, §9, §10 (A5, A14), §11, §12, §13 and the status
header. Edits in other sections:

| # | File | What changed | Why |
|---|---|---|---|
| 34.1 | 04-networking-and-servers.md (§2.4, new bullet) | **Planned relocation (Phase 4):** a drained gateway sends `Relocate{token}` on CONTROL. The client connects to the new gateway while the old connection keeps playing and dual-sends input. The new gateway sends `ClientRebind{s, session_epoch + 1, mode = relocate}` to every contributor. The old gateway forwards until each contributor's `RebindFence`, then sends `PathClosed`. The bullet points to 05 §6.3.1 for steps, pacing, failures and acceptance | 04 owns the wire protocol and the session lifecycle; without it, 05's "players see nothing" for gateway binaries was false |
| 34.2 | 04 (§6.6 Failures, new case) | *The gateway is drained*: the contributor re-keys its per-(s, cell) state to the new trunk, **keeps acked baselines**, ORs the old path's in-flight chunk masks into `lost_mask` and sends `RebindFence`. No `HeldEntities`, full mask, create or destroy | The view-composition failure list covered only gateway death and NAT rebinding |
| 34.3 | 04 (§6.1 Consequences, new bullet) | **Fence gate:** while a fence operation on a tree is in flight, its owner cell holds new ledger requests for the tree and stamps them with the returned epoch | PostgreSQL lets share lockers pass a waiting `FOR UPDATE`, so a busy tree (a 60-member carrier) could starve its own advance; 05 §1.13 holds the shard-primary half of the rule |
| 34.4 | 08-client-and-launcher.md (§1.2 state table) | A **Relocating** row: invisible, two connections, input on both, STATE from both under the per-handle tick rule, EVENT_R held until `PathClosed`, pushes de-duplicated by message ID, fallback to Reconnecting | The client must hold two netcode connections during a relocation |
| 34.5 | 01-vision-and-scope.md (AAA-STB-6) | Adds gateway rolls by make-before-break relocation with 0 linkdead sessions and input gap p99 ≤ 250 ms (05 §6.3.1, BE-A14 Ph4) | The scorecard tested only cell rolls. No criterion was added, so the scorecard still has 65 |
| 34.6 | 09-roadmap-and-process.md (WP-4.3, WP-4.4) | WP-4.3 builds relocation (`Relocate`, `ClientRebind{relocate}`, `RebindFence`). WP-4.4 builds `DrainGateway`, `MintRelocation`, the fence load budget, the fence gate and the lock rules. Their acceptance lists already hold BE-A5 (10k) and A14 (Ph4) | Every new deliverable has an owning WP and a phase exit |

No criterion ID was added or renumbered. Changed thresholds: A5 gains a Phase 2 fence mix and a Phase 4 fence
clause (`Fence.Advance` p99 < 5 ms, ledger p99 < 25 ms and handoff p99 < 100 ms with a 60-member carrier).
A14 (Ph4) gains clause (c): a 12-box gateway roll under 50k bots with 0 linkdead sessions and an input gap
p99 ≤ 250 ms. §3.6's Phase 4 design shard-primary WAL rises from 15 to 27.6 MB/s: ledger 15, fence 7.6, and
world scripts at their 5 MB/s cap, which were previously counted only in prose. The WAL archive estimate
rises from ≈ 11 TB to ≈ 16 TB raw. Checked and consistent, no edit: 04 §2.6's box rule and 05 §6.7's zone
rule hold during a roll, because `DrainGateway` opens the zone's pre-racked warm spare first. Relocation is
Phase 4, and Phase 3 gateway rolls are paced reconnects, which matches 04 §2.4's reconnect budget. 00 has no
statement about gateway draining or fence locking.

## 35. Round-4 review fixes owned by 04 (2026-09-25)

Gaps in 04 from the backend and engine lenses:
- **Major:** co-location had one sentence: no cascade or load bound, no definition of "cheaper", no distance
  bound (a leased AG deep in a neighbour region fell outside viewer registration and ghost margins, which were
  measured from the owner's regions and boundary), no lease lifecycle and no criterion.
- **Minor:** multi-cell migrations used §10.3's dev-only debug hold in production, the hold had no maximum (a
  control-plane fault after `MigrateAck` froze the whole zone, against 05 principle 8), and "one hitch per
  rollout" was wrong for multi-cell zones.
- **Minor:** N↔N+1 residuals carried third-party binary state (Jolt `SaveState`) that `.hschema` rules do not
  govern, and the verify hash could not reliably catch a format change.
- **Minor:** replicant recovery covered only multi-cell zones, so most Phase 4 players (single-cell hubs,
  stations, systems) still lost up to 30 s on a cell crash.
- **Minor:** the gateway↔cell trunk mesh was unmodelled: one IO thread and 4,096-entry buffers per trunk, no
  zone-instance affinity, and NS-4.4 measured only sessions homed in one zone.
- **Minor:** Luau math (UCRT vs glibc libm), ozz's `rsqrtps` estimates on the cell hitbox path, STL sort ties
  and MXCSR state escaped the `det::` rules.

04 changed as follows: a new §6.2a (co-location protocol: coupled pairs with cook-checked coupling distances,
transitive sets capped at 32 roots, 128 rows and 5 % of the host's tick, refusal at 70 % p95 or overload stage
≥ 2 with effect-only fallbacks, leased-in load ≤ 10 % per cell, the "cheaper" rule, the 2H leash with whole-set
handoff by centroid, lease start, renewal, split, release, migration and crash rules); §6.2 ghost margins
measured from the receiving cell's regions; §6.6 class presence per occupied region; §6.7 `MigrationHold`
(production, `max_ms` = 1,000, self-computed release anchor, ρ as a suspect contributor, late-cell catch-up or
rejoin at the current tick), hitches per rollout (1, or n for an n-cell zone: 4 in Harrow orbit, 10 in a Ph4
10-cell zone; cumulative ≤ 1.6 s and ≤ 4 s p99), `sim_abi` with the portable physics path; §6.4 replicants for
every persistent world zone (2 vCPU, ≤ 4 cells across zones) and the rejoin rule in step 5; §3.5 late-cell
exception and hold rule; §2.6 trunk IO pool (epoll/`recvmmsg`, RIO), per-trunk buffer sizing, a per-box trunk
budget (≤ 1,024 connections, ≤ 64 MB), per-pair K, zone-instance affinity with drift repair by relocation, and
a mesh model; §10.2 rows for Luau math, hitbox sampling, sorting, the FP environment and region rejoin, new
`simdet` rules, cross-vendor replay; §10.3 tug bot, ghost audit, metrics; §11 interfaces, ladder, criteria and
risks; the status header. Edits in other sections:

| # | File | What changed | Why |
|---|---|---|---|
| 35.1 | 01-vision-and-scope.md (AAA-SRV-9 row and measurement rule; §6 glossary "Replicant") | SRV-9's Ph4 split becomes "persistent world zones / activity, phase and housing instances" (1 s / 30 s, values unchanged); the measurement rule names world zones and NS-4.6's single-cell Harrow High case; the glossary's replicant holds up to 4 world-zone cells of one zone or several | Replicant recovery is now the default for every world-zone profile (04 §6.4), so the Ph4 1 s bar covers where nearly all players are, as SC's shipped recovery covers every server of a shard |
| 35.2 | 00-decisions.md (ADR-007 roadmap and replicant bullet) | v1.5 is a replicant tier "for every persistent world zone"; one 2-vCPU replicant per ≤ 4 world-zone cells from one zone or several; default for every world-zone profile, mandatory for multi-cell zones, opt-in for activity, phase and housing instances, with the round-4 reason | The ADR said single-cell zones only opt in |
| 35.3 | 05-backend-services.md (§1.3 tokens; §1.4 `AssignReplicant`; §1.4.3 NATS-isolation cost; §1.4.4 co-location pointer to 04 §6.2a; §1.4.6 recovery step 5; §1.14.1 compat fingerprint; §6.4 cost assumptions; §1.14.1 release table; §6.7 DR table; §6.8 runbook; §9 Persistence ladder) | Tokens choose instances by zone-instance affinity, then free slots. `AssignReplicant(cells[], replicant)` covers ≤ 4 world-zone cells across zones. Recovery restores from the replicant in every world zone. The compat fingerprint names the physics library build (Jolt version and determinism defines, 04 `sim_abi.physics`). Cost: one 2-vCPU replicant per ≤ 4 world-zone cells, ≈ 6 % more compute (was one 4-vCPU per 4 multi-cell-zone cells, ≈ 5 %). The release table's cell-roll hitch is per region moved (n per n-cell zone), not per zone | 05 owns the Session service, placement and the cost model; clients predict hulls bit-exactly with Jolt (04 §5.3), so a Jolt change is a client↔server contract change and arrives with an epoch flip, whose step 4 then uses 04's portable physics path |
| 35.4 | 02-engine-runtime.md (§7.2 Server; §7.4 Sandbox; RT-03; RT-04) | Cells and clients pose hitboxes with `det::HitboxSampler`, never ozz's estimate-based SIMD jobs. The sandbox notes the `det-math` patch (transcendentals, `^`, fastcalls, `vector`, codegen libm pointers, compiler folding → `det::`). RT-03 and RT-04 add AMD and Intel CPUs | 02 owns anim and the script host; the replay contract (04 §10.2) and NS-3.8 now require cross-vendor identity |
| 35.5 | 09-roadmap-and-process.md (§4.3.1 Intel lab box; §4.3.2 per-run model; §6 Determinism row; WP-1.5, 1.6, 2.4, 3.1, 4.3) | The Intel lab box has an Intel Core CPU and runs the cross-vendor determinism nightly. The per-run model counts replicants for every world zone (≈ 6 %, 6–13 cell-equivalents; the ≈ 120–240 total is unchanged). The Determinism row adds the Luau corpus, `det::HitboxSampler`, the `simdet` self-test and AMD/Intel CPUs, and cites NS-3.8. WP-1.5 adds `det::HitboxSampler` and Recast's `det::sort` patch; WP-1.6 the `det-math` patch and `simdet` rules; WP-2.4 the trunk IO pool, `det::sort` and the MXCSR policy; WP-3.1 location-based ghosts, the co-location protocol, `MigrationHold`, `sim_abi`, cross-vendor replay, tug bots and the ghost audit; WP-4.3 replicants for every world zone, zone-instance affinity and the trunk budget | Every new deliverable has an owning WP and a phase exit (NS-3.8, 3.10–3.12 in WP-3.1; NS-4.4, 4.6 in WP-4.3) |
| 35.6 | 06-gameplay-framework.md (§7.3 Recovery) | Followers restore from the owner's replicant state "in world zones" from Phase 4 (was "in multi-cell zones") | Follows 35.1 |
| 35.7 | ../PLAN.md (§4.1 v1.5; §5 crash-recovery bullet; §8.1 Ph4 exit; §9 SRV row) | "multi-cell zone(s)" → "persistent world zone(s)" / "world zones" for the replicant tier and SRV-9 | Follows 35.1 |

No criterion ID was added or renumbered. Changed or extended criteria, all in 04: NS-3.8 adds Intel↔AMD replay,
the Luau math corpus, hitbox sampling, byte-identical cooked bytecode and the `simdet` self-test; NS-3.10 adds
a 20 km co-located tow with 0 pops and 0 missing ghosts; NS-3.11 adds 10 s and 30 s control-plane outages after
the ACK in (c) (other cells resume ≤ 1 s, ρ rejoins ≤ 3 s after recovery, 0 double writes, 4 hitches
cumulative ≤ 1.6 s per rollout) and case (e), a `sim_abi.physics` change on the portable path; NS-3.12 adds (c),
a 100-ship seam furball (sets ≤ 32 roots, leased-in ≤ 10 %, every cell's tick p99 ≤ 35 ms, effect-only
momentum within 5 %); NS-4.4 becomes (a)–(c) with a scattered 50k-CCU mesh (≤ 1,024 trunks, ≤ 64 MB); NS-4.6
adds single-cell Harrow High and a replicant CPU cap. The AAA-SRV-9 thresholds are unchanged; only their
scope label moved. Checked and consistent, no edit: 05 §1.4.4's degraded mode ("co-location leases extend")
matches §6.2a's unconditional renewal; 05 §1.4.6's rack-disjoint replicant rule and its "≈ 34 processes with
replicants" per 27-cell rack still hold at one replicant per 4 cells; 05's new §6.3.1 relocation (§34 of this log) is
what 04 §2.6 uses to repair affinity drift; 06 §7.3's follower leash ≤ M still guarantees a neighbour ghost,
since margins are now measured from the receiving cell's regions, which only widens coverage; AAA-STB-6's
"hitch p99 ≤ 1 s" is per region moved, which the new per-rollout counts respect.


## 36. Round-4 review fixes owned by 02 (2026-09-25)

Gaps in 02 from the engine lens (both minor):
- **The Windows CPU gate ran after third-party code.** The gate sat at `init_seg(compiler)` (`.CRT$XCC`).
  The vendored mimalloc's default `MI_WIN_INIT_USE_CRT_TLS` mode registers a TLS callback in `.CRT$XLB` and a
  C initializer in `.CRT$XIB`. Tracy uses `init_seg(".CRT$XCB")` and has dynamic `thread_local`s, which run
  from the CRT's `__dyn_tls_init` TLS callback. All of that is `avx2` code, and all of it ran first, so a
  directly started client faulted in mimalloc on a pre-AVX CPU. Check 3 scanned only `.CRT$XCA`–`.CRT$XCT`
  and `.CRT$XD*`, so it would have failed on Tracy in every `HELIOS_PROFILE` build with no fix planned. SDK
  game modules were not forced to `avx2`. The landed hook (`engine/core/src/platform/win32/cpu_gate_hook.c`,
  `.CRT$XIB`) has the same ordering flaw.
- **The fence cap did not fit its wall budget.** 16 tiles at ≤ 1 ms each is 16 core-ms, or ≈ 2 ms of wall on 8
  workers, not ≤ 1 ms. The client's 4-tile cap (up to 4 core-ms inside Phase B's fixed ticks) was in no row
  of §2.4.

02 changed as follows:
- **§1.1 gate placement.** On Windows the gate is now the first TLS callback, in `.CRT$XLA0` right after the
  CRT's `__xl_a`, in the first-initialized image: the executable when shipping, `helios_runtime.dll` in
  modular builds. A table lists every pre-`main` mechanism, with the in-tree users of each. The failure path
  runs under the loader lock: stderr, then a dialog from a `user32` loaded from System32 on GUI images, then
  `TerminateProcess(78)`, never `ExitProcess`, which would run mimalloc's `.CRT$XLY` detach hook. `core`
  asserts that the gate ran. The gate objects are named by their real paths, and their rules are updated:
  `/GS-`, no sanitizers, three exports, and an allowlist that adds `GetModuleHandleW`,
  `GetEnvironmentVariableW`, `GetCurrentProcess` and `TerminateProcess`. Building mimalloc and Tracy at
  `base` was considered and rejected. Linux is unchanged: `.preinit_array` already precedes mimalloc's
  `constructor(101)` and Tracy's `init_priority` objects.
- **§1.1 check 3.** It now enumerates every entry in `IMAGE_TLS_DIRECTORY.AddressOfCallBacks`, every
  `.CRT$XI*`, `.CRT$XC*` and `.CRT$XD*` entry, and `_pRawDllMain`. The gate must be callback 0. Every other
  entry must be CRT-owned or listed in `cmake/pre_main_allowlist.cmake`, which covers mimalloc, Tracy and
  Helios' own `.CRT$XCU`. It checks the import-closure load order and fails on four seeded canaries.
- **§1.1 check 5.** SDE `-nhm` and `-snb` also run on the modular `HELIOS_PROFILE=ON` editor, PIE client, bot
  and `helios-tool`. The full-process chip check runs in report mode, filtered to Helios-built images,
  because `-chip-check-exe-only` would skip `helios_runtime.dll`.
- **§1.1 SDK consumers.** `HeliosConfig.cmake` sets the `avx2` flags as `INTERFACE` options, and the SDK's
  `helios_game_module()`/`helios_executable()` apply them to the link closure. `sdk_config.h` adds `#error`s
  for missing `__AVX2__`, BMI, LZCNT, POPCNT and F16C, or for FMA contraction. `helios_module_info()` gains
  `isa` (§1.4), and the loader refuses non-`avx2` modules. `sdk-consumer` checks each object's recorded
  command line (`LF_BUILDINFO` or `.GCC.command.line`), runs check 3 and runs SDE `-nhm`.
- **§5.8a fence cap.** The cap is now a cost, `pcg.collision.fenceCoreMs`. Cells get 8 SERVER-core ms per
  tick, which is 8 full builds (≤ 1.0 ms each), 16 shape-only builds (≤ 0.5 ms each) or a mix, dispatched
  longest first. That gives ≤ 1.2 ms p99 wall (scan ≤ 0.2 + builds ≤ 1.0), and ≤ 0.2 ms on ticks that build
  nothing. Clients get 2 per frame: ≤ 1.0 ms wall on REF, ≤ 1.4 ms on MIN. Fence builds count against the
  prefetch budget. Spawns and warp exits warm their destination sets. Past its cap the client suspends
  prediction and logs `clientcore.prediction_suspended{reason=terrain}`.
- **§2.4.** Terrain-fence lines go in the per-thread tables (REF, MIN BENCH-2, MIN BENCH-5) and the per-system
  tables (≤ 2 or ≤ 2.8 core-ms on a frame that builds), with a note on slack (MIN BENCH-2 keeps ≥ 5.8 ms). The
  cell-tick text states the new fence numbers.
- **Criteria.** RT-12 adds a **landing fence case** (MIN BENCH-2 on both boxes, and REF): prefetch throttled,
  the fence ≤ 1.4 ms p99 on MIN and ≤ 1.0 ms on REF, prediction suspends past the cap, 0 terrain corrections,
  and no CPU-bound frame. RT-20's fence bound becomes ≤ 8 core-ms per tick and ≤ 1.2 ms p99, and it adds a
  fence-cap variant. RT-09 names the enumerated pre-gate path, the mimalloc and Tracy cases, the canaries, the
  modular SDE run and the SDK object check.
- **§8.** §8.3, the risks and §8.6 (04 and 08 rows) are updated to match, and so is the status header.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 36.1 | 00-decisions.md (ADR-011 ISA amendment) | The gate "runs before any other code in the image, third-party pre-`main` hooks included". It is the first TLS callback (`.CRT$XLA0`) on Windows, ahead of mimalloc's `.CRT$XLB`/`.CRT$XIB` and Tracy's `.CRT$XCB`/`thread_local`s, and `.preinit_array` on Linux. The audit enumerates every pre-C++ entry. Replaces the `init_seg(compiler)` wording | The ADR said nothing Helios-compiled runs earlier, which was false for Helios-compiled third-party code |
| 36.2 | 00-decisions.md (ADR-001a rule 1; acceptance `sdk-consumer` bullet) | Rule 1: `HeliosConfig.cmake` exports the `avx2` flag set as `INTERFACE` options, and `sdk_config.h` `#error`s without `__AVX2__` (and, off MSVC, BMI, BMI2, F16C, LZCNT, POPCNT) or with `__FMA__`, `_M_FP_FAST` or `_M_FP_CONTRACT`. The `sdk-consumer` job also checks every game-module object's level, runs 02's pre-gate check and runs SDE `-nhm` (exit 78) | The SDK previously checked only the compiler version |
| 36.3 | 04-networking-and-servers.md (§3.3 stage 3) | "≤ 1 ms p99" becomes "≤ 8 core-ms of builds (≤ 8 full or ≤ 16 shape-only tiles); ≤ 1.2 ms p99 wall at that cap, ≤ 0.2 ms when nothing is built". Stage 3's 10 ms budget is unchanged | 04 owns the tick graph and cited the old number. This supersedes §29.3's "≤ 1 ms p99" and §29's "16 tiles per tick" |
| 36.4 | 08-client-and-launcher.md (§2.2 client gate bullet) | The gate runs before any other code in the image: the first TLS callback (`.CRT$XLA0`) of the first-initialized image on Windows, ahead of mimalloc and Tracy, exiting via `TerminateProcess`; `.preinit_array` on Linux. Replaces "`init_seg(compiler)` on MSVC" | 08 restated the old placement for the storefront SKUs |

No criterion ID was added or renumbered. Changed criteria, all in 02: RT-09, RT-12 (the landing fence case) and
RT-20 (the fence bound and the fence-cap variant). Checked and consistent, no edit:
- 08 CL-17 already runs the editor under SDE `-nhm`/`-snb` through 02's check 5, which now names the modular
  profiling flavour.
- 09 WP-0.2's "five-check audit … the pre-gate path of every `avx2` image" covers the extended check 3.
- 09 §8.1's ISA delta row (4) says the gate "does not exist". The tree now has
  `engine/core/src/cpugate/cpu_gate.c` and the per-OS hooks, and 02 §1.1 records the `.CRT$XIB` placement
  as the delta WP-0.2 closes. 09 owns the refresh of that row.
- 03 §5.5a's per-tile CPU numbers (VM ≤ 0.5 ms) are unchanged.
- 06 has no fence or gate numbers.

## 37. Round-4 review fixes owned by 09 (2026-09-25)

09 addressed three gaps from the round-4 review.

1. **ISA drift was handled as plan drift (§5.10).** The in-tree ISA and gate code is uncommitted and dated
   2026-09-25. It comprises `cmake/HeliosIsa.cmake`, `cmake/isa_allowlist.cmake`,
   `tools/lint/isa_audit.cmake`, `tools/ci/msvc_gate_audit.ps1`, `engine/core/src/cpugate/*` and the per-OS
   `cpu_gate_hook.c`. It implements the per-file AVX2 allowlist that the ADR-011 amendment retired, and it
   puts the Windows gate in `.CRT$XIB`.
   - §5.10.4(b) adds ten delta rows.
   - Two rework WPs are added. WP-0.2r moves the code to image levels, `<module>@base` builds and the
     five-check audit. WP-0.5r moves the gate to `.CRT$XLA0` with `TerminateProcess`, `/GS-`, three exports
     and the verdict check, following 02's round-4 §1.1.
   - WP-0.2 and WP-0.5 now exclude that code.
   - CONF-11 rejects per-target and per-file ISA grants, and CONF-12 rejects gate misplacement.
   - The new rule D7 makes working-tree code and review rounds subject to D3, with `Plan-Rev` recorded in
     module READMEs.
   - K37 is extended, and reconciliation #25 records the case.
   - §8.1 is refreshed from a headless GCC 13.3 build of the working tree on 2026-09-25, with 0 warnings:
     `core_tests` 174/174 (11 gate cases), `math_tests` 115/115 and 39/39 `lint` tests. The rows now list
     the layering, WP-0.5 and render-graph code that is in the tree. §8.2 now lists 12 WPs.
2. **Project schema publishes (§5.2a).** Rule 2 adds a *project dynamic schemas* path class.
   - It covers non-native project packages, non-Foundation gem schemas and their lock entries.
   - Checks run per commit: `--check-lock`, the native-construct and SEC-1 lint, and the compat
     classification against a new `Helios-Compat` trailer. `schema.dynamic-hot` runs on the head.
   - A code-owner approval is required.
   - Engine, Foundation and `schemas.native` packages and `helios.project.jsonc` stay WP-only.
   - The WP-2.16e fixtures are seeded, the §5.6 and §6 rows are updated, and reconciliation #24 records the
     change.
3. **WP-2.16 split (§2.3a).** It becomes ten sub-WPs of 4–6 engineer-weeks, 48–58 in total, each with its
   own dependencies and acceptance.
   - 2.16c2 now depends on WP-1.10 and 2.16c3 on WP-1.6.
   - §1 adds the rule that any Phase 0–2 scope over 6 engineer-weeks is split.
   - §3.1 gains the ISA-rework and DX schema lanes, with a slack table (tightest: 4.5 months, ED-22's
     packaging clause).
   - K38 mirrors 02 §8.4's dynamic-path risk, with RT-21's per-type targets as triggers.
   - WP-3.12's dependencies are updated, and reconciliation #26 records the split.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 37.1 | 02-engine-runtime.md (§1.1 build-rules bullet on `tp_jolt`; "Why the old placement failed"; the audit heading; §8.6 09 row) | "WP-0.2 delta" → "WP-0.2r removes them". "WP-0.2 moves it to `.CRT$XLA0`" → "WP-0.5r … (09 §5.10.4)". "The audit (WP-0.2, RT-09)" → "(WP-0.2r, RT-09)". The §8.6 row names WP-0.2r and WP-0.5r and the §5.10.4 deltas, and it assigns RT-21 to WP-2.16c1–c3 and 2.16d and ED-22 to WP-2.16e | 09's WP-0.2r and WP-0.5r rework WPs and the WP-2.16 split |
| 37.2 | 08-client-and-launcher.md (§2.10.5 phasing; §4.8 09 row) | WP-2.16 → WP-2.16b for the Windows installer, the Linux tarball layout and CL-24 (Ph2) | WP-2.16 split |
| 37.3 | 00-decisions.md (ADR-001a acceptance) | The version-gate script test and the `sdk-consumer` job come from WP-2.16a2 | WP-2.16 split |
| 37.4 | docs/PLAN.md (§8.1 next WPs; §11 backend link, tests and known deltas) | "Next twelve" with the new §8.2 anchor. The §5.10.4 anchor follows the renamed heading. Test counts are 174/174 and 115/115, and all 39 lint tests pass. The ISA delta now says the code exists, is non-conforming, and is reworked by WP-0.2r and WP-0.5r | D6 (status kept true) |

Checked and consistent, no edit:
- 07 T08 ("reaches `main` only through the publish PR, where CI reruns schemac's `--check-lock` and lint and
  the project's reviewers approve it") matches rules 2, 3 and 6.
- 07 §1.8.2's pinned project schema packages live in `schemas/<pkg>/`, which is inside the new path class.
- 02 §3.4 (`schema.lock.jsonc`, append-only, tombstones) matches rule 3's lock-diff check.
- 05 §1.14.1's compat fingerprint is the one the classification computes.
- 02's round-4 §1.1 (`.CRT$XLA0`, `TerminateProcess`, three exports, `pre_main_allowlist.cmake`, check 3 (a)–(d)
  and the C gate files) is what WP-0.2r and WP-0.5r deliver. 02's "09 owns the refresh of that row" (§36) is
  done in §8.1 and §5.10.4(b).


## 38. Round-4 review fixes owned by 03 (2026-09-25)

The round-4 engine reviewer found that §7.6a gave A1 and A2 characters no per-vertex motion vectors: their
motion came "from the instance transform only". Under FSR 2 at MIN's 1.5× and Performance mode's 1.7×, each
30 Hz or 15 Hz reskin step would leave limb trails, which is AAA-REN-8 L7. The walk-away golden ran only
Helios TAA on lavapipe, so it could not catch this.

03 changed as follows:
- **§7.3.** A motion-vector contract: every opaque surface, reduced-rate skinned tiers included, writes exact
  motion between the geometry displayed this frame and last frame. The reactive mask is for transparents,
  particles, beams and holograms. The composition mask is for shading that moves without geometry. Opaque
  skinned characters write neither mask.
- **§7.6a ring.** Every skinned tier (hero, A0, A1, A2) uses 32 B per vertex, with two ping-ponged position
  slots. Slots flip on `AnimLod.poseSerial` changes. Hold frames reuse the displayed slot, which gives zero
  pose motion, and that is exact. A slot acquire skins the last-displayed palette into the second slot. The
  reactive-mask alternative was rejected. Worst case in BENCH-1: REF 108.5 MB (was 87) against a **112 MB** cap
  (was 96), and MIN 49.9 MB (was 38) against a **56 MB** cap (was 48). The §8.2 ring caps become Low 56,
  Medium 80, High 112 and Ultra 144 MB. §1.3's character line becomes 0.21 GB (REF) and 0.1 GB (MIN). §8.1.4's
  MIN geometry row stays at 0.55 GB. Skinning cost is unchanged, and the prepass adds ≤ 0.02 ms.
- **§8.4a (new): the temporal-stability check.** Motion-vector consistency against a first-principles
  reference motion must be ≥ 99.5 %. The limb-mask ꟻLIP against a 16-sample reference must have a p95
  ≤ 1.15 × a full-rate control (`anim.lodRate=full`).
- **Walk-away golden (RC-1, per commit from Ph2).** It runs through Helios TAA 1.0×, FSR 2 Quality 1.5× and
  FSR 2 Balanced 1.7×, each at a 60 and a 120 fps cadence, with pixel scale matched to 1080p. A negative
  control (`render.skin.transformOnlyMotion=1`) must fail.
- **RC-10.** The ring limit is ≤ 56 MB on MIN and ≤ 112 MB on REF. A BENCH-1 crowd capture runs through FSR 2
  on MIN at 1.5× and on REF at Performance mode's 1.7×, and it must pass §8.4a.
- **RC-9.** An image-stability clause: MIN BENCH-1/2/4 and REF BENCH-4 Performance pass §8.4a through their
  shipping FSR 2 mode.
- **§9.2 and §9.4.** The ladder's Ph2 character cell and the risk table are updated to match.

The edit below is the smallest consistent change elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 38.1 | 02-engine-runtime.md (§7.2 crowd and animation-LOD contract) | Adds rendertest-only debug cvars that shipping builds compile out: `anim.forceTier`, `anim.forcePhase` and `anim.lodRate=full` | 03 §8.4a's full-rate control and the walk-away golden need pinned tiers, stagger phases and a full-rate replay from 02's animation runtime |

§31's round-3 numbers for the ring are superseded: 20 B per vertex for A1–A2 and 87/38 MB worst case. No criterion
ID was added or renumbered. Changed criteria, all in 03: RC-9 (the image-stability clause) and RC-10 (the ring
caps and the FSR 2 crowd capture). Checked and consistent, no edit:
- 09 WP-2.3 already names "the tiered skinned-output ring (03 §7.6a)" and `IUpscaler` + FSR 2, so the
  walk-away golden's Ph2 start needs no roadmap change.
- 01 §3.3.1's L7 wording ("TAA ghosting and upscaler artefacts, on REF and on MIN") is what §8.4a measures.
- 02 RT-12 and 02 §2.4 are unaffected: palette rates and CPU costs do not change.

## 39. Round-4 review fixes owned by 07 (2026-09-25)

One minor tools-lens gap in 07: **world scripts (05 §1.23) had no editor integration.** PIE's backend spawned
only a gateway and a cell, T08's schema editor did not list `worldscript` blocks although 02 §3.8 allows them
in dynamic packages, tables and invocations could be browsed only with `helios-admin ws rows`, no hot-reload
budget covered world scripts, and no ED criterion covered a designer building a cross-zone system.

07 changed as follows:
- **§1.6.2 (new): world scripts in PIE and the editor.** Every PIE mode that runs cells (Play, Simulate, Two
  zones, Multi-cell) launches one WSH (`helios-cell --role world-script --dev`) when the project declares a
  `worldscript` block. Its partitions are `ws` regions leased by the dev orchestrator, so fencing, inbox dedup
  and commits are the production path, and rows live in the PIE data directory's embedded PG, with *Reset*,
  *Load fixture* (escrow funded), *Snapshot* and *Restore*. An optional warm standby makes *Kill WSH* run 05's
  ≤ 10 s takeover. Each partition VM announces its own DAP port, as cell VMs do. A stop pauses only that
  partition, and dev-only hooks keep it alive: `DebugPaused` suspends detection, `World.call`s wait with no
  deadline, held JetStream deliveries get in-progress acks and timers fire on resume. Other additions: a
  *Hold zones on world-script break* option, *Break on world-script failure*, and *Re-run under debugger*
  from records of the last 10k invocations. A hot-reload table classifies edits as hot (handlers ≤ 2 s p95,
  additive block edits ≤ 5 s p95), migrate (dev-data migration ≤ 60 s per 100k rows, RPCs served) or refused.
  The **World scripts panel** (T26 for PIE and dev, T27 for shards) has Scripts, Tables (index queries only),
  Invocations (fuel against 5 ms, row operations against 256, outcome), Dead letters and Audit tabs. Row edits
  are audited GM invocations that may not touch `@currency` or escrow-condition fields. There are nine T28 rules
  (`ws.quota`, `ws.escrow.backing`, `ws.escrow.flow`, `ws.partition`, `ws.index`, `ws.privacy`, `ws.compat`,
  `ws.globals`, `ws.budget`).
- **T08** edits `worldscript` blocks as forms (header, escrows, tables with indexes and `@escrowBacked`, RPCs,
  events, timers and reasons), generates handler stubs, shows a quota meter and blocks the save on the `ws.*`
  rules. Its migration preview counts affected PIE rows and offers the panel's dry run. **T10** serves DAP from
  WSH partition VMs, and its Ph3 code pane is where handlers are written. **T26/T27** host the panel, T27 adds
  world-script hot swaps to the hotfix stages, and **T28** lists the `ws.*` rules.
- **§1.6** (Play, Multi-cell and Two zones rows; hot swap; debugging), **§4.1** (two budget rows), **§4.4** and
  **ED-15** (ED-23 replay), **§5.1** (Framework Ph3, T08, T10, T26, T27 and T28 cells; Ph3 gate), §5.3 (a risk
  row), §5.4 (05 asks) and §5.5 (02, 05 and 09 rows) are updated to match.
- **ED-23 (new, Ph3).** In a `starter-sandbox` project, a designer uses only the editor. They add a
  `claimZone` field and a `Cancel` RPC handler to the bounty board, with ≤ 5 s and ≤ 2 s p95 live times and
  two seeded T28 rejections. In two-zone PIE they post a bounty in zone A and claim it in zone B, and cancel a
  second bounty across zones: each pays or refunds exactly once. They hold the `Killmail` handler at a DAP
  breakpoint for 60 s, which gives one payout and no takeover. They see the row, the invocations with fuel and
  a matching escrow check in the panel, and an `amount` edit is refused. Finally they split `status` into
  `status` and `outcome` with a `migrate` handler, dry-run it, and apply it to 100k fixture rows in ≤ 60 s while
  bots keep posting. The result has 0 `ws_dead` rows, a matching backing check, and row hashes equal to
  `upgrade-project`'s. Nightly as a dual-path UI replay on Windows and Linux.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 39.1 | 05-backend-services.md (§5 `--spawn` row) | Default `gateway,cell,ws`: `ws` runs one `helios-cell --role world-script` when the project declares a `worldscript` block. The editor's PIE passes `--spawn none` and launches its own cells and WSH | §1.23 item 11 already said `helios-dev up` starts a WSH, but the flag's default spawned only a gateway and a cell |
| 39.2 | 05-backend-services.md (§1.23 item 7, GM tools) | The editor's World scripts panel uses the same GM API. The service refuses a GM edit that changes a `@currency` field or a field an `@escrowBacked` condition reads; value corrections go through handler intents | Without the guard, a GM row edit could break the escrow-backing audit (SEV1) that item 6 relies on |
| 39.3 | 05-backend-services.md (§1.23 item 11, Dev) | Every editor PIE mode starts a WSH. There is one DAP port per partition VM. The `--dev` hooks are `DebugPaused` for a stopped partition, in-progress acks for held deliveries, a per-invocation trace and re-run records of the last 10k invocations, all compiled out of shipping builds | 05 owns the WSH's behaviour, and 07 §1.6.2 depends on these hooks |
| 39.4 | 09-roadmap-and-process.md (WP-3.12 deps, deliverables and acceptance; Phase 3 exit; §6 editor UI row) | WP-3.12 delivers the world-script editor integration and owns **ED-23**. It depends on WP-3.7 for ED-23's code pane (a clause dependency, like 2.16b for ED-22). The Ph3 exit lists ED-23, and the nightly UI replays include it | Every new criterion needs an owning WP and a phase exit (check row 1) |

New ID: **ED-23** (Ph3, WP-3.12). ED-15 now lists ED-23 among its replays. No ID was renumbered.
**Checked, no edit needed:**
- 02 §3.8 already allows `worldscript` in dynamic packages. 02 §7.4's DAP adapter serves cells, and the WSH is
  the cell binary, so per-partition ports need no runtime change.
- 04's WSH row (§1 process table) is unchanged: PIE's WSH is the same role with `--dev`.
- 00's round-3 amendment and 01's north star already name world scripts.
- ../PLAN.md §2's editor line could mention the World scripts panel and ED-23 at the next integration pass.
- Check row 1's ED range becomes ED-1…23 at that pass.


## 40. Round-4 review fixes owned by 06 (2026-09-25)

The round-4 tools reviewer found a major gap: SWG player cities and housing (G07, critical for SWG) and EVE
territory and sovereignty (G08, critical for EVE) were designed but neither owned nor measured.
- **No owner for city state.** 06 §9.2 put "elections (Go)" in `CityDef`, but no 05 service had city,
  citizen, election or treasury tables. World state held only flags, influence and sovereignty, and nothing
  said how cells enforce a city's radius and zoning across cells and zones.
- **No acceptance.** No GP or BE criterion covered placement parity, lot and item caps under concurrency, the
  maintenance → decay → condemnation → reclamation chain, city ranks, citizenship, elections, city taxes, or
  the territory lifecycle. GP-7 covered only timer firing, and CL-23 one placement rejection.
- **Render-only proof.** 01 §2 and PLAN.md §7 proved SWG settlements only with BENCH-5, a rendering benchmark.

06 now has:
- **§9.2 ownership table.** Structures and their contents belong to the ledger in the structure AG's custody.
  Ground is ledger plot rows, lots are the bound currency `Lot`, upkeep is the Industry service's upkeep
  worker, cities are the `CityGovernance` world script, and territory is the World State service.
- **§9.2.1 placement.** A `StructureDef` record and seven numbered `PlacementCheck` rules with reason codes,
  computed in integers or strict FP so client and cell agree bit for bit. One ledger transaction commits a
  placement: fee, `Lot` transfer, deed → structure, `CreateAg`, `OccupyPlot`. The ledger's exclusion constraint
  arbitrates concurrent placements across region seams. A zone-wide `ZoneStructures` manifest gives every cell
  and client the same plots and terrain stamps. Housing zones never open overflow layers.
- **§9.2.2 caps and access.** The owning cell is the single writer of an interior and counts in-flight slots.
  ACLs can name city roles. Vendors close at condemnation.
- **§9.2.3 upkeep chain.** Stages Paid → Decay 1–3 → Condemned → Reclaimed, with numbers. Each settle is a
  guarded ledger transaction, key `upkeep:<structure>:<seq>`, followed by one PG commit of `seq`, stage and
  next timer. Reclamation is one transaction, run through the owning cell or the dormant precondition.
- **§9.2.4 cities.** `CityDef` (SWG rank table 5/150 m … 40/450 m, weekly cycle, 21-day founding, 3-week
  terms) and the `CityGovernance` worldscript block: `City`, `Citizen` (with tombstones), `Residence`,
  `Election`, `Ballot`, `Permit` and `Journal` tables, RPCs, events, timers and reason codes.
  - Founding is reserve-then-confirm, serialized per zone partition.
  - Residence is one per account, with a partition-crossing event protocol.
  - Rank-up is immediate and a downgrade waits two cycles.
  - Elections settle exactly once, guarded by `status`.
  - The treasury is escrow-backed, with a nine-row tax and flow table (property and residency legs from the
    upkeep worker; sales, travel and service legs from cells).
  - Footprints are projected as keyed world flags into KV `WORLD`, enforced by every cell of the zone and
    replicated to clients.
  - Terminals call the script through the cell, failures fail closed, and scale is stated.
- **§9.3 territory.** A `StructureLifecycleDef` and its state table. The design covers owner vulnerability
  windows with a change delay, reinforcement exits snapped into the window with a seeded offset, and an
  entosis-style capture channel in zone time (so TiDi applies). Transitions are CAS on the World State service
  with an audit row. `PreProvision` requests go out in the reinforce transaction. Influence uses up/down
  hysteresis. Phase 4 adds sovereignty upgrades against region power and workforce, with weekly fuel and an
  occupancy multiplier.
- **Criteria.** **GP-16** (Ph3), housing and cities:
  - (a) 10k-placement parity across four toolchains;
  - (b) caps under concurrent placement;
  - (c) the upkeep chain exactly once under chaos;
  - (d) city governance under WSH kills (WP-3.12);
  - (e) BENCH-5's 300 structures placed through the rules;
  - (f) housing instances (WP-3.2).

  **GP-17** (Ph3; sovereignty Ph4), territory: (a) windows, (b) capture, (c) reinforcement and `PreProvision`,
  (d) influence hysteresis and (e) sovereignty, each with the audit fold.
- **Elsewhere in 06.** GP-7 points to GP-16 (c) and GP-17 (c). `TerminalDef` gains `Structure` and `City`.
  §11's `World` row names `World.call`. §12.1–12.4 get the ladder (pre-provisioning moves to Ph3 with
  WP-3.1's `PreProvision`), four risk rows, two traceability rows and the dependency lists.

The edits below are the smallest consistent changes elsewhere.

| # | File | What changed | Why |
|---|---|---|---|
| 40.1 | 05-backend-services.md (§1.6) | Ledger plot ops `OccupyPlot`/`VacatePlot`/`ListPlots` over `ledger_plot`. The table has an `EXCLUDE USING gist` integrity constraint and a per-zone count (`ZONE_FULL`) | Concurrent placements from two cells must be arbitrated atomically with the fee and the lots |
| 40.2 | 05 (§1.8) | Structure upkeep: the `structure_upkeep` row, settle timers and guarded settle transactions, `evt.<shard>.structure.stage`, and reclamation through the owning cell or the dormant precondition | 06 §9.2.3's chain needs a service owner that survives restarts |
| 40.3 | 05 (§1.21) | `territory_structure` and `territory_audit` rows. `Territory.Transition`, `SetWindow`, `InstallUpgrade` and `RemoveUpgrade`. Keyed world-flag defs (`zone.<zone>.City.<id>`, `zone.<zone>.Territory.<id>`), with world-script writers. `PreProvision` requests in the reinforce transaction | The world-state service owned "territory" in its title but had no rows or API for it |
| 40.4 | 05 (§1.23 items 6, 7 and new 13) | System jobs may add an escrow-claim leg from a pool they already debit, delivered as `ws.<script>.claim`. `structure.stage` becomes subscribable. New item 13: the Foundation `CityGovernance` world script | City property tax comes from structure upkeep pools, not from a character's wallet, and cities need a named owner |
| 40.5 | 05 (status header; §13 06 row) | Lists these additions | Keeps 05's own summary true |
| 40.6 | 09-roadmap-and-process.md (WP-3.2, WP-3.6, WP-3.12, WP-4.5; Ph3 and Ph4 exits; §2.7.4 `starter-sandbox`; §3.2 #15) | WP-3.6 delivers housing, the cell side of cities and territory, and accepts GP-16 (a–c, e) and GP-17 (a–d). WP-3.2 accepts GP-16 (f). WP-3.12 builds `CityGovernance` on its world-script host and accepts GP-16 (d); this avoids a cycle, because WP-3.12 depends on WP-3.6. WP-4.5 accepts GP-17 (e). The exits list GP-16 and 17 (Ph3) and GP-17 (e). The `starter-sandbox` proof gains a housing plot, a city founding, an election under a WSH kill, a sales tax and a reclamation | Every new criterion has an owning WP and a phase exit, and the template proves the SWG loop |
| 40.7 | 08-client-and-launcher.md (§1.7.2 Placement row) | The ghost's pre-check names the `ZoneStructures` manifest and city footprints. The City panel names the treasury journal, stand and vote, the mayor's policy and its binding to `CityGovernance` | 08's ghost and City panel use the data §9.2 defines |
| 40.8 | 01-vision-and-scope.md (§2 (a) and (b) proofs) | SWG: settlements built through the housing and city rules (BENCH-5, GP-16). EVE: territory and reinforcement timers (Ph3, GP-17), sovereignty (Ph4) | The proofs cited only a rendering benchmark |
| 40.9 | ../PLAN.md (§7 SWG and EVE proof rows; the 06 summary; §8.1 Ph3 gates; §9 criteria list) | The same proofs. The summary covers plots, lots, the upkeep chain, `CityGovernance` and territory. Ph3 gates list GP-16 and GP-17, and the criteria list reads GP-1…17 | PLAN.md mirrors 01 and the criteria set |
| 40.10 | README.md (Phase 3 and Phase 4 vocabulary) | Phase 3 names housing and player cities (`CityGovernance`) and territory with reinforcement timers; Phase 4 names sovereignty | The phase vocabulary matches 06 §12.1 |

New criteria: **GP-16** and **GP-17**. No ID was renumbered, and §1's check row now covers GP-1…17 at the next
integration pass. Checked and consistent, no edit:
- 04 §6.7's `PreProvision(zone, time, size)` (lead ≥ 10 min, 30 min default) and 04 §7's housing instances and
  `Fence.Park` triggers are what §9.2–9.3 use.
- 02 §5.8's stamps are "persisted, replicated and applied last, in id order (06 §9)". §9.2.1 now says how:
  through the manifest, in structure-ID order.
- ADR-008's world-script amendment (escrow-only value, no Go plug-ins) holds, because `CityGovernance` is Luau
  content, and system-job claims come from system pools, never from characters.
- 00 has no statement on housing, cities or territory beyond ADR-007's housing instances.

## 41. Integration pass 5: round-4 revisions (2026-09-25)

Inputs: a re-read of PLAN.md, README, 00–09 and this log after every round-4 revision (§34–§40), plus the
automated checks of §1. No reviewer gap was filed against PLAN.md itself in round 4. Round 4's own fixes
touched PLAN.md only in §35.7, §37.4 and §40.9; this pass reconciles what fell between the seven revisions.

**New cross-section inconsistencies found in the re-read.**

| # | File | What changed | Why |
|---|---|---|---|
| 41.1 | 02-engine-runtime.md (RT-09) | "`sdk-consumer`, from WP-2.16" → "from WP-2.16a2" | 09's split (§37) gives the `sdk-consumer` job and the SDK's hold on consumers' ISA level to WP-2.16a2. Fix 37.1 renamed the WP in 02 §1.1 and §8.6 but not in RT-09 |
| 41.2 | 09-roadmap-and-process.md (Phase 4 exit) | "SRV-9 (Ph4, ≤ 1 s in multi-cell zones)" → "in persistent world zones" | Fix 35.1 widened SRV-9's Ph4 bar to every persistent world zone, and 35.7 updated PLAN.md, but the exit list 09's auditors score kept the old scope |
| 41.3 | 06-gameplay-framework.md (GP-16 (a)) | Placement parity runs on every determinism toolchain: the client builds (both MSVC toolsets, the floor being the release toolset, and clang-cl), the cell builds (GCC and Clang) and MinGW. It named four builds | 06 §9.2.1 promises a bit-identical verdict "on every determinism toolchain (ADR-001a rule 7)", and players' clients are built by the floor toolset that the four-build list left out |
| 41.4 | 01-vision-and-scope.md (§2 (d) proof; §6 glossary) | The SC proof adds cell kills in single-cell Harrow High (NS-4.6 (b)). "Planned migration" says the ≤ 1 s hitch is per region moved. New entries: **Persistent world zone** and **Gateway relocation** | Fixes 35.1 and 34.5 made both terms scopes of SRV-9 and STB-6, and 00, 04, 05, 06, 08, 09 and PLAN.md use them; the glossary had neither |
| 41.5 | README.md (Phase 3 and 4 vocabulary) | Ph3 adds co-location across cell seams (04 §6.2a) and world scripts in PIE and the editor (07 §1.6.2). Ph4: the replicant tier "for every persistent world zone" (was "for multi-cell zones") and gateway deploys by make-before-break relocation (05 §6.3.1) | The single phase vocabulary lagged round 4, as fixes 25.8 and 33.14 found after earlier rounds |
| 41.6 | 09 (§3.2 #21) | Notes that round 4 widened the replicant tier to every persistent world zone (§35) | The reconciliation row still read "every multi-cell zone" as the current rule |
| 41.7 | 04-networking-and-servers.md (§11.3 Gateway row) | Zone-affinity token assignment moves from the Phase 3 cell to Phase 4, beside relocation drift repair and the scattered-mesh trunk budget | Fix 35.5 assigns zone-instance affinity to WP-4.3 with NS-4.4 (c), and §2.6's mesh model and drift repair are Phase 4. The ladder said Phase 3 |
| 41.8 | 04 (§11.2 `on_rebind`) | Gains a `RebindMode`: loss, with the held set, or relocate, which keeps acked baselines (§2.4) | Fix 34.2 made the drained gateway a separate §6.6 rebind case, but the interface sketch had only the gateway-loss form |
| 41.9 | 08-client-and-launcher.md (§4.3 Client shell, Ph4) | Adds the Relocating state for gateway deploys (§1.2; 05 §6.3.1) | Fix 34.4 added the state to §1.2's table but not to the ladder that 09's WPs read |
| 41.10 | 09 (WP-0.15r; §5.10.4 (a)) | "draft v4" → "draft v5". §5.10.4 (a) now says why round 4 adds no row: it touched 05 §1.3 and §1.4 only with Phase 3–4 scope (`MintRelocation`, `DrainGateway`, `AssignReplicant` across zones, zone-instance affinity), and the in-tree `pickGateways` already implements the free-slot order that affinity puts second | §5.10.2 requires every plan change to declare its effect on existing code. WP-0.15r asserted that round 4 added no delta, but §5.10.4 (a) explained only round 3 |
| 41.11 | 02, 04, 05, 06, 07, 08, 09 status headers; 09 §8.1 Plan row | Every section with round-4 changes says draft v5 (03 and 04 already did). Headers list the round-4 changes other sections' fixes made there: 02 (35.4, 37.1, 38.1), 04 (36.3), 05 (35.3, 39.1–39.3), 06 (35.6), 08 (34.4, 36.4, 37.2, 40.7, and 41.9), 09 (34.6, 35.5, 39.4, 40.6). 09 §8.1: "Draft v5 (round-4 fixes)" | Same rule as fixes 25.7 and 33.9. 08 had no round-4 entry although four fixes edited it |
| 41.12 | 09 (K4) | The mitigation adds the AMD/Intel cross-vendor replay nightly (§6, NS-3.8) | Fix 35.5 extended §6's Determinism row, but the register row lagged |
| 41.13 | ../PLAN.md (status; §1; §2; §4.1; §4.2; §5; §6; §7; §8.1; §8.2; §9; §10; §11; §12) | Status v4 → v5. §1: four review rounds; test counts 174/174 and 115/115 (§1 still said 141/141 and 104/104, contradicting §11); the working-tree list and the ISA rework. §2: backend (co-location, per-region migration hitch, replicants in every persistent world zone, gateway relocation, housing plots, structure upkeep, territory, the fence load, city governance as a world script); editor, scripting and object editors (world scripts in PIE, the World scripts panel, ED-23, T08's `worldscript` forms). §4.1: the replicant stream's scope, co-location and the bounded migration hold (v1), gateway relocation (v1.5), a world-script host in PIE. §4.2: the gate before third-party pre-`main` hooks; SDK consumers at `avx2`. §5: ADR-001, -007 and -011 rows. §6: 00 (the three round-4 amendments); 01 (per-region STB-6, gateway rolls, SRV-9 scope, GP-16/17 proofs); 02 (gate, fence cap, `det::HitboxSampler`, `det-math`); 03 (previous positions on every skinned tier, the temporal-stability check and walk-away golden through FSR 2, RC-9/10); 04 (trunk IO pool and affinity, relocation, location-based ghost margins, co-location, `MigrationHold`, `sim_abi`, cross-vendor replay); 05 (`DrainGateway`, plot rows, fence budget, upkeep worker, territory, `CityGovernance`); 07 (world scripts in PIE, ED-23); 08 (Relocating); 09 (round-4 criteria owners, the split rule, the two new lanes, project-schema publishes, D7, 39 risks, twelve next WPs). §7: the SC proof and caveat add single-cell Harrow High and gateway relocation. §8.1: Ph0 exit adds WP-0.2r and 0.5r; Ph3 ED-23; Ph4 GP-17 (e) and BE-A14's gateway rolls; the critical path names the ISA-rework and DX schema lanes. §8.2: project-schema publishes in the content tier; CONF rules for ISA grants and gate placement; D7. §9: STB-6's gateway clause; ED-1…23. §10: 39 entries; K24/K37 and K4 rows; a note on K38. §11: the working-tree list. §12: K1–K38 and the two glossary terms | The summaries, tables, gates, risk list and status lagged §34–§40 |
| 41.14 | This file (§1) | Check rows updated (ED-1…23, GP-1…17, links, references, the determinism list, headers) and added (replicant scope, split-WP references, ladder phases) | Keeps the automated checks current |

No criterion ID was added or renumbered in this pass, and no threshold changed. **Checked and consistent, no
edit:**
- The scorecard still has 65 criteria (REN 8, SRV 12, ITR 8, STB 6, CNT 7, SEC 8, TOOL 10, PLT 6). PLAN.md §7
  matches 01 §2.6 row for row (52 capabilities; critical-for, owner and phase). RT-01…22, RC-1…13, ED-1…23,
  CL-1…24, the 42 NS IDs (7/5/7/12/7/4 for Ph0–5), BE-A1…A20 (with A3a/A3b) and GP-1…17 all resolve. The
  remaining matches of the ID check are clause shorthands (`GP-14a` for GP-14 (a)) and 09 §3.2 #7's
  historical `BE-A3`.
- The register has 39 risks (K1–K38 plus K5b); PLAN.md §10's ten are unchanged in membership, and 09 §4.3's
  ≈ $2.2–13.2M total is unchanged. 05 still has 23 services and 08 §1.7.2 still has 27 panels.
- Every conformance list covers the ADRs its section cites (round 4 added no new citation outside a list).
- Every round-4 criterion change has an owning WP and a phase exit: ED-23 (WP-3.12, Ph3); GP-16 (a–c, e)
  WP-3.6, (d) WP-3.12, (f) WP-3.2 (Ph3); GP-17 (a–d) WP-3.6 (Ph3) and (e) WP-4.5 (Ph4); BE-A5's fence clauses
  (WP-2.5 at 2k, WP-4.4 at 10k); BE-A14 (c) (WP-4.4, Ph4); RT-12's landing fence case (inside the MIN clause,
  WP-3.4, Ph3); RT-20's fence bound (WP-1.3, 2.2); RC-9 and RC-10 (WP-4.2, Ph4); NS-3.8 and 3.10–3.12
  (WP-3.1); NS-4.4 and 4.6 (WP-4.3).
- 03's new ring caps (56/112 MB) appear only in 03 (§1.3, §7.6a, §8.1.4, §8.2, RC-10), and 02 RT-12 and §2.4
  are unaffected, as fix 38 found. 02's new fence numbers appear in 02 and 04 §3.3 only.
- 06's `CityGovernance` claims use 05 §1.23 item 6's system-job claim leg, which 07's `ws.escrow.flow` and
  `ws.escrow.backing` rules accept, and 07's GM row-edit guard (05 §1.23 item 7) protects the treasury's
  `@currency` fields.
- The WSH's `--dev` flag is 04 §10.3's existing dev-process flag, so 02 §1.3 and 04 §1 need no new mode.
- 05 §6.3.1's pacing (≤ 500 relocations/s per box for deploys) and 04 §2.6's drift repair (≤ 50/s) are
  different uses of the same protocol, and both keep baselines.

## 42. Round 5 minor revisions (2026-09-25)

### Round 5 minor revisions owned by 05 (2026-09-25)

These are the minor gaps `_round5-gaps.md` filed against 05:
- **Detection.** Nothing detected aimbots, triggerbots, input automation or farm bots. Trust's Phase 4 rules
  covered only the economy, AAA-SEC-3 measures only speed and teleport hacks, and the anti-cheat vendor
  decision waited until Phase 4.
- **Capacity.** §3.6's shard-primary WAL and IOPS, and A5's ±20 % assertion, counted only the ledger, the
  fence and world scripts.
- **Replicants.** They were host- and rack-disjoint but not zone-disjoint. §6.7 nevertheless claimed that
  cells lost with an availability zone restore from replicants, and A19 did not measure state loss.
- **NATS.** §6.5's permissions contradicted subjects the plan uses (`rpc.<shard>.ws.…`, KV `GROUP`,
  `ctl.<shard>.cell.all.gateway_dead`) and omitted four roles. Per-role credentials also left the
  `Helios-Fence` cell unauthenticated.
- **Maintenance.** No workflow covered SERVER node maintenance.

| # | File | What changed | Why |
|---|---|---|---|
| R5-05.1 | 05-backend-services.md (§1.16; new §1.16a; §6.2; §6.6 retention row; §6.8 `TrustDetectorPaused`; §8 `pkg/trust`; §9 Social row; new A21; §11; §12; §13) | **Trust detection framework.** <br>• **Cell features** from data the cell already has. Aim: snap angle, time to target in the rewound view, on-target dwell before fire, hit and precision rate against occlusion, silent-aim mismatch. Input: interval entropy, exact repeats, jerk spectrum, `device_class` consistency. Routine: loop periodicity, cadence, stimulus response, session length. Economy: yield. Cost ≤ 1 % of tick. <br>• **Scoring:** streaming rules (≤ 15 min) and daily gradient-boosted models (≤ 24 h). <br>• **Corpus:** red-team seam-fighter aim cheats, input macros and farm bots at humanization levels 0–3. Honest populations: the NS-4.1 swarm in `--human-model` mode and ≥ 5k reviewed human sessions. A nightly regression gate. <br>• **Ban waves:** delayed 7–21 days on §1.17 cases, with a dry run, two-person approval and ledger remediation under `Sink.Trust.Remediation`. A detector pauses when appeals overturn > 2 % of a wave. <br>• **New BE-A21 (Ph4)** | Gap 1 asked for a design and an acceptance criterion |
| R5-05.2 | 04-networking-and-servers.md (§9 anti-tamper bullet), 08-client-and-launcher.md (§1.14 "Optional vendor" row), 09-roadmap-and-process.md (WP-3.11, WP-4.8 rows) | The vendor is chosen in Phase 3 by WP-3.11, with native Linux support a criterion, and integrated in Phase 4. WP-3.11 also owns the cell features and corpus v1. WP-4.8 owns the detectors, the ban waves and BE-A21, and cites 05 §1.16a. Each edit changes one line | Gap 1 moves the vendor decision to the Phase 3 security WP. The three sections said "Phase 4" |
| R5-05.3 | 05 (§1.5, §1.12, §1.13, §2.4, §3.1, §3.6, A5, A9, §8, §9, §11, §13) | **§3.6 write mix:** one row per shard-primary writer (ledger, fence, world scripts, market, timers, account progression, character, mail, world state, groups, industry, orchestrator), with rows/s, WAL and IOPS at expected and design rates. <br>• **Phase 4 totals:** expected WAL 8.1 MB/s (was 5.3). Design WAL 34.5 MB/s (was 27.6) and ≈ 7.5k IOPS (was ≈ 5k). <br>• **Replay budget:** the design mix must stay ≤ 2/3 of the replay capacity that `helios-loadgen replay` measures (planning figure 60 MB/s, so 40 MB/s). <br>• **Account progression** moves to append-only deltas with folds, keeping 06 §5.2's `(account, incarnation, flushSeq)` contract: 1.1 MB/s instead of ≈ 6 MB/s at design. <br>• **Leaderboards and activity snapshots** move to the persistence cluster: 6.2 MB/s, fed from the outbox with an inbox and a 5 min `EVT` rewind after failover. Cells update `ACTIVITY` through `Activity.Update`. Without these moves the primary would carry 40.7 MB/s, or 45.6 MB/s with whole-row progression. <br>• **Persistence cluster:** Phase 4 design WAL 74 → 80 MB/s, sized for ≥ 90 MB/s (was ≥ 80). <br>• **WAL archive:** ≈ 16 → ≈ 24.5 TB raw. <br>• **Next moves, in order:** a world-script cluster, then the Phase 5 ledger split. <br>• **A5 (Ph4)** runs the whole mix with per-writer ±20 %. **A9** adds the board and snapshot writers | Gap 2 |
| R5-05.4 | 05 (§1.4 `AssignReplicant`; §1.4.6 placement and recovery step 5; §6.4; §6.7 RPO table and "what players see"; A19; §9 Orchestrator and Persistence rows) | **Option (b).** Replicants stay host- and rack-disjoint and are kept in their cells' availability zone. An AZ loss restores that zone's world cells from checkpoints (≤ 30 s of non-value state, 0 value). A19 asserts ≤ 30 s p99 and ≤ 35 s max against the truth tap, and ≤ 1 s for AGs whose replicant survived. Option (a), zone-disjoint replicants, is recorded as rejected: ≈ 4 Gbit/s of cross-zone streams at Phase 4 peak (≈ $7–13k/month in cloud) to save ≤ 29 s of rollback in an event whose recovery takes up to 5 min | Gap 3: choose one option and make it consistent |
| R5-05.5 | 05 (§2.1, §2.2, §1.13, §6.5, §6.8, §8 integration tests, §9 Runtime row, §11, §13) | **Generated NATS permission matrix.** `schemac` emits `nats-perms.json` for the cell, replicant, WSH, gateway, voice and Go-service roles. CI fails on any permission violation, and a static check requires every subject in code to be declared. The matrix grants the three missing subjects. <br>• **Two accounts per environment** (`svc`, `sim`), with `share: true` service exports, so the server stamps `Nats-Request-Info`. <br>• **Per-process credentials (Phase 3):** nkey user JWTs tagged `proc:<id>`, issued against a projected service-account token and revoked on confirmed death. <br>• **Caller checks:** the ledger, persistence and the WSH reject a request whose `Helios-Fence` cell (or `persist` subject token) is not the authenticated process, and the fence check requires the header AG's `owner_cell` to be that process. Only gateways may inject a player identity. `NatsCallerMismatch` is SEV1 | Gap 4 |
| R5-05.6 | 05 (§6.1; new §6.1a; §6.2; §6.4; §6.8 `NodeDrainOverdue`; §8 chaos; §9 Runtime row; A14 (d); §11; §13) | **`helios-nodemaint`:** request, then admission (one blast-radius domain; one batch at a time; normal mode; no `WarmPoolBelowDomain` or `FailureDomainLost`; no `PreProvision` or epoch flip within 1 h), then surge first, then cordon and `Drain`. Cells move by migration, replicants make-before-break, and WSH leases hand over. Agones `eviction.safe: Never`. Then self-test and uncordon. <br>• **Surge:** colo keeps one spare SERVER host per shard (≈ 3 %); in cloud, the autoscaler. <br>• **Times:** ≈ 7.5 h with one-host batches, ≈ 3 h with rack batches. <br>• **A14 (d) (Ph4):** hitch p99 ≤ 1 s per region moved, 0 disconnects, 0 state loss, the pool never below its bound, an unadmitted `kubectl drain` evicts nothing, done in ≤ 8 h | Gap 5 |
| R5-05.7 | ../PLAN.md (§9 criteria list); this file (§1 criteria check row) | `BE-A1…A20` → `BE-A1…A21` | New criterion BE-A21 |

**Criteria.**
- New: **BE-A21** (Ph4, WP-4.8).
- **A5 (Ph4)** now runs the whole §3.6 write mix. It asserts each writer's WAL within ±20 % of its row, total
  WAL (34.5 MB/s) and IOPS (≈ 7.5k) within ±20 %, the mix ≤ 2/3 of measured replay capacity, and each
  writer's SLO.
- **A9** adds 10k leaderboard upserts/s and 500 activity snapshots/s (≈ 80 MB/s of WAL).
- **A14** gains clause (d).
- **A19** gains the zone-loss state-loss bound.
- No ID was renumbered.

**Left to the integration pass or other owners.**
- *AAA-SEC-9.* The reviewer proposed it, but a scorecard row would change 01 §3.8 and the 65-criterion count
  that PLAN.md §1 and §3 and the §41 check state, and 01 owns those. The criterion is therefore BE-A21, owned by
  WP-4.8. 01's owner may promote it.
- *09's Phase 4 exit* reads "every criterion with Ph ≤ 4", which covers BE-A21, but it is not in the named
  list.
- *02 and 04 may mirror 05 §13.* 02: schemac's `nats-perms.json` emitter. 04 §10.3: `helios-bot`'s red-team
  variants and `--human-model`. 04 §10.2: the trust pre-filter's ring flush.
- *05's status header* lists the round-5 fixes. Its version ("draft v5") is left to the integration pass, as
  in fix 41.11.

**Checked and consistent, no edit.**
- 00 ADR-007 and 04 §6.4 place replicants "on a different host (and rack)", and ADR-007's ≤ 1 s is for a cell
  crash. Both hold under option (b).
- 01 AAA-SRV-9 is "on cell crash".
- 06 §5.2's flush contract and 06 §6.7's `ACTIVITY` key are unchanged: the service now writes the key for
  the cell.
- 06 GP-11 (c)'s account-row limit (≤ 32 KiB) still holds for the base rows, and its 2k flushes/s with p99 ≤ 20 ms is the delta insert.
- 09 WP-3.3 (cites 05 §6, §9) owns §6.1a and the per-process credentials. WP-4.4 (cites 05 §9) owns rack
  batches, A14 (d) and the full A5 mix. WP-3.2 (cites 05 §1.12) owns the leaderboard and snapshot move. All
  of these criteria are already in those WPs' acceptance lists.

### Round 5 minor revisions owned by 02 and 04 (2026-09-25)

Round-5 gaps addressed: engine/02 (Jolt ordering independence; gate spec lag), engine/04 (replay keyframes for
long-lived zones), backend/04 (load-dependent decisions missing from the replay contract), plus the parts of the
engine/09 and tools/09 gaps whose fixes name 02 §2.2 or 04 §10.2.

| # | File | What changed | Why |
|---|---|---|---|
| R5-02.1 | 02-engine-runtime.md (§1.1 backstop; mimalloc row of the pre-`main` table; check 5) | The illegal-instruction backstop decodes the faulting opcode. `ud2`/`ud1`/`ud0` and every #UD that is not an ISA fault pass through (Windows `EXCEPTION_CONTINUE_SEARCH`; Linux returns after `SA_RESETHAND` restored `SIG_DFL`). Only a VEX/EVEX lead byte (`C4`, `C5`, `62` after optional prefixes) or POPCNT gets the CPU message and exit 78. When the crash handler installs it takes #UD over: on Linux its `sigaction` replaces the backstop and its re-raise restores `SIG_DFL` (today's `posix_crash.cpp` restores the backstop); on Windows it adds a first-in-chain vectored handler that sends non-trap #UD to the dump path, so the gate keeps three exports. Dumps carry a `cpu_gate` annotation. Check 5 gains ud2 and EVEX (SDE `-hsw`) fixtures before and after crash-handler install. The mimalloc row no longer claims `tp_mimalloc` pins `MI_WIN_INIT_USE_CRT_TLS=1` (`third_party/CMakeLists.txt` defines only `MI_STATIC_LIB`): it relies on mimalloc's default, which is that mode on every Helios toolchain, and check 3 fails the one unsafe mode through a non-CRT `_pRawDllMain` | A real crash could end as "CPU unsupported" with no dump; the working tree already passes the three traps (`hcg_is_deliberate_trap`), so the spec lagged the code; the mimalloc claim was false and unowned |
| R5-02.2 | 02 (§2.2) | Heap lifetime rule (a heap used by another thread is never freed, only pooled or quarantined; `destroyAll` only for thread-confined heaps; no `mi_theap_t*` across a job boundary), and sharded, batched tag accounting (32 shards, 256 KiB forwarding, peak touched only by a raising batch, `flushAccounting()` per tick or frame, hard budgets exact to 256 KiB per thread), with a `core_tests` recycling regression and a ≤ 3× `mi_malloc` cost target at 4 and 16 threads | engine/09 gap: fold spike (a)'s heap-recycling hazard (200/200 misrouted allocations) and the 460–890 ns exact-accounting collapse into 02 §2.2, which only 02 can edit |
| R5-02.3 | 02 (§5.4; §7.1 Determinism; RT-03; RT-19; §8.3; §8.4) | **Ordering independence:** stable body keys in `mUserData` (EntityId; packed `TileKey` for tiles; PCG instance key for asteroids and scatter); vendored `third_party/jolt/patches/stable-order` keys the contact sort key and tie-break, the equal-motion-type body-1 choice and `CharacterVirtual`'s contact predicate by `(layer, key)`; `Body::EFlags::NoCrossUpdateCache` on ShipHull and Vehicle bodies (no manifold reuse or warm start on the first collision step of each `Update`; persisted-contact events unchanged); wheel contact IDs are replaced by Jolt's default full wheel test before use; keyframes and residuals re-create bodies with `CreateBodyWithID` from a body table. Deterministic `BodyID`s everywhere rejected. RT-03 gains a permuted-ID variant (must fail without the patch) and a resting-creep bound (< 1 mm in 60 s); RT-19 gains a ≥ 3-tile contact case with permuted IDs and a 10-tick rollback | engine/02 gap. Verified in the vendored Jolt 5.6.0: `ContactConstraintManager.cpp` (`mSortKey` from `SubShapeIDPair` with body IDs; `SortContacts`), `PhysicsSystem::ProcessBodyPair` (lower ID is body 1), `CharacterVirtual::ContactOrderingPredicate` (`mBodyB`, `mCharacterIDB`). The gap's `EStateRecoverType::Contacts` is `EStateRecorderState::Contacts`; putting the contact cache into the rollback snapshot was not chosen, because corrections restore the cell's state and the cell's cache is keyed by cell `BodyID`s, so the flag removes that state instead |
| R5-02.4 | 02 (§7.4; §8.3 Script; §8.1 Physics and Script ladder rows) | Native codegen stays off on cells and world-script hosts until `third_party/luau/patches/codegen-fornloop-fuel` lands (`VmConfig` refuses it; today it warns); `third_party/luau/patches/fuel-counter` is required (measured 12–17 % against ≤ 10 %); both join `det-math` in `sim_abi.script`. A replay-keyframe rebase bullet mirrors 04 §10.2 | tools/09 gap (b): the patches had no home in the plan text |
| R5-02.5 | 02 (§8.4 flecs row; §8.6 04 row; status header) | The flecs risk row records RT-01's Phase 0 pre-bench (structural ops 3.4–6.7 ms against 1.5 ms; raw flecs ≈ 0.9–1.7 ms; the cost is wrapper bookkeeping a custom ECS would also need), so wrapper optimization comes first; §8.6 and the header list the round-5 changes | Keeps 02's risk row consistent with the committed spike; the decision record itself is 09's and 00's |
| R5-04.1 | 04-networking-and-servers.md (§10.2 table; §6.2a item 4; §6.3; §11.2) | New replay rows `ColocDecision{tick, set_id, pair, outcome, reason, inputs}` and `HandoffDecision{tick, root, role, peer, epoch, reason}` (each side), and `Keyframe{tick, cause}`. The overload row now says the stage and measured tick times do change sim state, but only through logged decisions (`JobResult`, `ColocDecision`, `HandoffDecision`). §6.2a and §6.3 say each decision is logged; the replayer applies logged outcomes and never reads a load measurement | backend/04 gap: host choice, refusals and NACKs depended on load but were not replay events, and the overload row said "never" |
| R5-04.2 | 04 (§10.2 contract, Recording, new *Replay keyframes* and *Script rebase*, Replay; §3.1; §6.7 residual row and Replay bullet; §10.3 metrics; §11.1; §11.2 `Recorder::keyframe`) | A zone replays from any keyframe. Keyframes are cut at start, migration (Q), recovery or rejoin (S), and every 30 min by a script rebase. The contents table lists ECS at full precision, registry and handle blocks, AG table, effect inboxes and outboxes, co-location leases, ghosts and hit history, per-grid body and constraint tables with `SaveState(All)` incl. contacts, tile bodies and holds, nav inputs, RNG and clock, inbox and in-flight requests, and the lane's C++ state; no Luau state. The rebase waits (≤ 60 s) for no suspended coroutine of a module without an `Adopted` handler, ends all coroutines, swaps a pre-loaded fresh VM, runs module chunks and raises `Adopted{cause = rebase}`. The ring keeps the log back to the newest keyframe ≥ 30 min old (≤ 1 GB at 500 players). The migration residual carries the body table so Q can `CreateBodyWithID` before `RestoreState` | engine/04 gap: no mid-session keyframe existed, Luau threads cannot be serialized, and the 30 min ring could not be replayed alone for a long-lived zone. Jolt's `RestoreState` needs matching IDs, which the residual did not provide either |
| R5-04.3 | 04 (NS-2.4; NS-3.8) | NS-2.4 **keyframe clause**: a 70 min run replays its 10 min window from the rebase keyframe cut ≥ 60 min in, with that keyframe and the log alone; ≥ 1,000 coroutines re-armed, ≥ 20 in `awaitService`; keyframe tick ≤ 3 ms extra; 0 script errors and lost results; a handler-less 2 s wait defers the rebase. NS-3.8 **multi-cell clause**: every cell of an NS-3.12(c) furball and of NS-3.11(c)'s migrations (incl. a 30 s outage with an expired hold) replays bit-exact one cell at a time, also GCC↔MSVC, with load measurements forced to other values; `MigratedFrom` chains verified; `ZoneRejoin` present. **Physics-order clause**: a keyframe replay whose post-K bodies take other slots stays bit-exact | Both gaps' acceptance asks. IDs unchanged (clauses appended in place); owners stay WP-2.4 (NS-2.4) and WP-3.1 (NS-3.8) |
| R5-04.4 | 04 (§5.3; §10.2 physics row; §10.2 lint paragraph; §10.2 Replay; §11.1; §11.3 Tooling; §11.5 replay risk; status header) | Mirrors 02 §7.1's ordering rule; names `codegen-fornloop-fuel` (the `FORNLOOP` interrupt in `CodeGen/src/IrTranslation.cpp`) and `fuel-counter`, with cells interpreter-only until the first lands; ladder, layout and risk rows updated | engine/02 gap ("mirror in 04 §10.2"); tools/09 gap (b) ("name the CodeGen patch in 04 §10.2") |
| R5-06.1 | 06-gameplay-framework.md (§8.2 rule 9; §11 *Coroutines are tasks*) | One line each: rule 9 cites 02 §7.1's stable order and cache rule; §11 notes that a cell's replay-keyframe rebase raises `Adopted` with `cause = rebase` | The engine/02 gap asks for the mirror in 06 §8.2; designers read 06 §11 to know when `Adopted` fires |

**Hand-offs (not edited here; other owners' files):**
- **09 §5.10.4 (b), "Windows failure path and gate-object rules" row:** "The objects keep `/GS` (the allowlist
  admits `__security_check_cookie`)" is stale. The working tree's `helios_cpu_gate_sources()`
  (`cmake/HeliosIsa.cmake`) applies `/GS-` and `-fno-stack-protector`, and `cmake/isa_allowlist.cmake` admits no
  cookie symbol. The CONF table should also record the trap passthrough (present in both hooks) and, as open
  WP-0.5r deltas, the VEX/EVEX classifier, the crash-handler handover (`posix_crash.cpp` must re-raise with
  `SIG_DFL`; a first-in-chain Windows vectored handler) and check 5's new fixtures (02 §1.1).
- **09 WP owners:** `third_party/jolt/patches/stable-order` and the `NoCrossUpdateCache` flag are needed by
  RT-03's permuted variant (Ph0–1) and RT-19 (WP-1.2); the two Luau patches need the WP the tools gap proposes
  (WP-0.10r or WP-1.6); the sharded tag accounting is a `core` change in WP-0.5's scope; keyframes and the
  rebase fall under WP-2.4's "`SimInbox` + replay".

No criterion ID was added or renumbered. Extended in place: RT-03, RT-19 (02); NS-2.4, NS-3.8 (04). Checked and
consistent, no edit: 06 §8.1a rule 3's snapshot needs no contact state under the new flag, and its rule 6 full-state
correction restores correctly (wheel contact IDs above); 04 §6.7's Scripts rule is the model the rebase reuses;
05's world-script hosts inherit the interpreter-only rule through 02 §7.4's "cell rules"; 07 §1.6's per-cell PIE
replay logs gain keyframes with no tool change.

### Round 5 minor revisions owned by 07 and 08 (2026-09-25)

These are the four tools-lens gaps `_round5-gaps.md` filed against 07 and 08:
- **08, group-content HUD.** §1.7.2 had no party or raid frames, no encounter HUD, no PvP match HUD or scoreboard,
  and no AFK, vote-kick or deserter prompts. A healer could not play GP-14 (a)'s flashpoint or operation, and 06
  §12.4's scoreboard had no panel.
- **07, group content at a desk.** No PIE mode ran an activity through the activity service end to end, there
  were no dev controls for tier, modifiers, seed, checkpoints, wipes, role bots or lockout periods, and no ED
  criterion timed an encounter edit-and-retest loop. TOOL-10's brief had no activity.
- **07, wall-clock systems.** Neither PIE nor `helios-backend` could move game-calendar time, so upkeep,
  reinforcement, lockouts, industry and resource shifts could only be tested with compressed record values.
- **07, structure authoring.** 07 never mentioned `StructureDef`, `CityDef`, `StructureLifecycleDef` or
  `ZoneRulesDef`; nothing produced 06 §9.2.1's cooked no-build polygons; there was no placement preview, heat map
  or T28 rule family; and BENCH-5 was defined twice (ED-10's editor-built structures against GP-16 (e)'s
  rule-placed snapshot).

| # | File | What changed | Why |
|---|---|---|---|
| R5-07.1 | 07-editor-and-tools.md (§1.6 modes table; new §1.6.3; T12; §4.1; §5.1; §5.3; §5.4; §5.5; new ED-24) | **Activity PIE (Ph3, `--dev` only).** Gateway, origin-zone cell and the dev backend's activity service, matchmaker, ledger and orchestrator; instance cells from a warm pool of two, so `CreateInstance` → Loading ≤ 2 s (SRV-11). The queue → ready check → launch → encounters → `ClaimGuard` rewards → Closing path is production code. T12's **Activity panel**: director, *Launch premade*, *Queue*, *Fill roles* with Foundation role-behaviour bots, live `ActivityState`, roster policy and claims. Dev controls as audited GM commands accepted only by `--dev` processes: tier, any modifier set and a pinned seed; *Jump to encounter or phase checkpoint* from a checkpoint library, restored in place by 06 §6.7's replacement-cell rehydrate (≤ 1 s; missing phases fall back with a warning; synthesized jumps are `devSkipped`); *Force wipe*; *Kill instance cell*; *Advance lockout period* through the dev shard clock (guard rows stay immutable per period); *End run*. Save → phase re-engaged ≤ 10 s p95. **ED-24 (Ph3):** on the *Hollow Vault*, a queued premade plus role bots, a matchmade tier with a forced modifier and pinned seed, 20 T12/T23 edit iterations at ≤ 10 s p95, wipe and cell-kill resume, exactly one claim per character per period and one more after *Advance lockout period*, and the return to the origin zone | Gap "designers cannot iterate on instanced group content at a desk" |
| R5-07.2 | 07 (new §1.6.4; T26; T27; §4.1; §5.1; §5.3–5.5; new ED-25) | **Dev shard clock (Ph3).** `g = g₀ + (t − t₀) × rate`, monotonic (no rewind; rate 1–3,600), published as the dev-only KV `CONFIG` key `dev.clock`. The timer worker fires due timers in due order with catch-up; the calendar publishes crossed events in order; `ZoneClock::wall_now()` returns `g` in `--dev` cells (Wall effects, weather epochs, wall deadlines); WSHs and offline-progress services use it. Leases, heartbeats, tokens, JetStream windows, idempotency windows and TTLs stay on real time. Replay needs no change because 04 §10.2 logs `WallDeadline{tick, id}`; `.hrepro` manifests record clock epochs. Refused when `env` is `staging` or `live`, and compiled out of shipping cells. Controls on the PIE *Clock* menu, T27 on dev shards (GM role `dev.clock`), `Editor.cmd` and `helios-admin clock`: *Advance 1 h / 1 d / 1 w*, *Run to next due timer* (optionally for the selected entity), *Fire calendar event* (advances to it), *Rate*; a T26 **Timers** tab. Budgets ≤ 1 s and ≤ 30 s. **ED-25 (Ph3):** with *Cinder Reach*'s shipped (uncompressed) values, a House runs Paid → Decay 1–3 → Condemned → Reclaimed and a territory structure runs anchoring → Reinforced(1) (one `PreProvision`) → Vulnerable(1) inside the owner's window, within 10 min of wall time, with GP-16 (c)'s and GP-17 (c)'s audit folds at 0 mismatches and the guards checked | Gap "wall-clock systems cannot be exercised in PIE" |
| R5-07.3 | 07 (new §2.7; T01; T21; T28; §4.1; §4.1.1; §5.1; §5.3–5.5) | **Structure, zone-rules, city and territory authoring.** The editor never places a player structure (plot rows only through the runtime path). §2.7.1: `ZoneRulesRegion` entities in T01 Volumes mode, and an assetd `zonerules` builder that generates the no-build set as the union of `Region.NoBuild` regions, `Settlement`-tagged authored settlements (+32 m), T06 `Road` splines (half-width + 8 m), lairs (spawn radius + 16 m) and `TravelPoint`s (64 m), stored per cube face in integer centimetres and hashed into the zone content; ≤ 10 s full, ≤ 2 s p95 incremental (a §4.1.1 row). §2.7.2: T21's **Structure tab**: footprint generated from collision (≤ 16 vertices, editable), clearance and neighbour gap, terrain stamp preview on real `pcg`, and a *Try on terrain* preview that runs the runtime `PlacementCheck` with 08's reason codes. §2.7.3: the T01 **buildable-area heat map** (Ph3; ≤ 2 s for 4 × 4 km at 16 m; buildable km² and a plot-count estimate against `maxStructures`). §2.7.4: `CityDef` and `StructureLifecycleDef` customizers. §2.7.5: nine T28 rules (`structure.footprint`, `.stamp`, `.kind`, `.placeable`; `zone.nobuild.stale`; `zone.structures.layers`, `.max`; `city.ranks`; `territory.lifecycle`). Ph2 for regions, the cook, the Structure tab and the `structure.*`/`zone.*` rules (M2's harvesters place through `PlacementCheck`); Ph3 for the heat map and the city and territory parts | Gap "round 4's structure systems have no authoring workflow" |
| R5-07.4 | 07 (ED-10); 01-vision-and-scope.md (§3.2 BENCH-5 row); 09-roadmap-and-process.md (§2.4 M3 paragraph); ../PLAN.md (§8 M3 row) | **BENCH-5 settled.** ED-10 co-edits an authored, `Settlement`-tagged town at BENCH-5 scale, then checks the regenerated no-build set: a bot placement just inside is refused with `NO_BUILD` and one just outside is accepted, as the heat map predicted. BENCH-5's measured scene is GP-16 (e)'s rule-placed snapshot, as 01's BENCH-5 row and 09's M3 sentence now say (one line each) | Gap's last bullet: BENCH-5 was defined twice |
| R5-07.5 | 01-vision-and-scope.md (§3.9.1 brief table, one row) | TOOL-10's brief gains a **Strike** deliverable: a 2-encounter strike with 2 tiers, a phase checkpoint, a weekly lockout and a `QueueDef`, iterated in Activity PIE and cleared through the queue (T12, T23, T09, T01) | Gap asked for an activity deliverable in TOOL-10's brief |
| R5-07.6 | 05-backend-services.md (§5 flag table, one row) | `--dev-clock`: dev flavour only, refused for `staging`/`live`; the `dev.clock` key and its consumers; real-time leases, tokens, TTLs and idempotency windows; points to 07 §1.6.4 | The gap names `helios-backend --dev-clock`, and §5 lists every backend flag |
| R5-07.7 | 09-roadmap-and-process.md (WP-3.7 row) | Own adds 07 §1.6.3, §1.6.4 and §2.7; Deps add "3.2 and 3.6 for ED-24 and ED-25" (the activity service, encounters, housing and territory); Deliverables add Activity PIE, the dev shard clock (with `--dev-clock`) and the heat map; Acceptance adds ED-24 and ED-25 | Every new criterion needs an owning WP (check row 1). The Phase 3 exit already covers every Ph ≤ 3 criterion |
| R5-08.1 | 08-client-and-launcher.md (header; §1.7.2 two new rows and the Activity director row; coverage paragraph; §1.7.3 reject paths and a new group-content fixtures bullet; §1.12 protected actions; §4.3 Game UI row; CL-23; §4.5; §4.6; §4.7; §4.8) | **Group and raid frames** (`GroupFramesVM`, `UnitFrameVM`; party ≤ 6 in Ph2, raid ≤ 16 in Ph3): role, sync, health, shields, resource, role-filtered effects, threat, downed/reviving/dead/AFK/linkdead/out-of-range states, presence only outside the interest set (04 §9), click targeting and **gamepad frame focus**, range dimming, protected targeting. **Encounter and match HUD** (`EncounterVM`, `MatchStateVM`, `ScoreboardVM`, `VoteKickVM`, `RosterPolicyVM`; Ph3): boss bars (`HUD.Boss`), phase, enrage, revive tokens, checkpoint, wipe and resume banners, the downed screen; `MatchState` round, clock, scores, phase banners, medals, mercy warning and end screen with cause; a scoreboard from the cell's per-player standings (the event-log fold); AFK warning, vote-kick prompt with server refusal reasons, deserter confirmation and rejoin prompt. Flows: a gamepad-only heal-targeting pass over 16 frames, encounter banners through a checkpoint, a forced wipe and a cell kill, a 6v6 scoreboard equal to the offline fold after a cell kill, and AFK, vote-kick and deserter prompts (driven by 07 §1.6.3's dev commands). The panels are listed in the `starter-shooter` and `starter-story` coverage and in CL-23. Vote-kick starts and votes join the protected actions | Gap "§1.7.2 has no group-content HUD" |
| R5-08.2 | ../PLAN.md (§2 "Client" row; §6 08 bullet; §9 criteria list) | 27 → 29 Foundation UI panels (twice); `ED-1…23` → `ED-1…25` | Keeps the summary truthful for the two new panels and two new criteria |

**Criteria.**
- New: **ED-24** and **ED-25** (both Ph3, WP-3.7), nightly as dual-path `helios-uitest` replays (§4.4); ED-24 also
  runs headless through `helios-tool pie --activity`.
- Reworded in place: **ED-10** (an authored town at BENCH-5 scale plus the no-build placement clause).
- Extended in place: **ED-15** (replays of ED-23…25), **CL-23** (the group-content fixtures).
- No ID was renumbered.

**Hand-offs (other owners' files, not edited here).**
- **06:** §6.7 a replicated, instance-audience encounter state sent on change like `MatchState` (engaged encounter,
  phase, enrage deadline tick, revive tokens left per team, wipe count, checkpoint `seq`), and the downed and
  reviving states with their deadline ticks on player entities; §6.11 per-player standings (the running fold,
  restored after a cell kill) and the match end cause; §6.10 the seconds left on the AFK warning cue, vote-kick
  start and cast intents with refusal reasons, and the vote's replicated state; a `HUD.Boss` tag; the dev-only
  activity GM commands (checkpoint restore, force wipe) accepted only by `--dev` cells; §9.2.1 may list
  `ZoneRulesDef`'s `noBuild` margins block, the `Settlement` and `Road` tags and the `TravelPoint` component.
- **05:** §1.12 the activity service's dev launch overrides (any modifier set, pinned seed) and the dev
  orchestrator's warm instance pool; §1.8 and §1.15 the dev clock's catch-up and crossed-event rules, which the
  new §5 row summarizes.
- **04:** §11.2's `ZoneClock::wall_now()` gains the `--dev` path; §10.3's dev-only hook list may add the activity
  GM commands; the §10.2 recording header may carry the dev-clock epoch (07 records it in the `.hrepro` manifest).
- **09:** §6's editor-UI test-matrix row can add the ED-24 and ED-25 replays; the WP-2.13 and WP-3.8 parenthetical
  panel lists can name the new panels (both rows already own "the Ph2/Ph3 panels of 08 §1.7.2"); the Phase 3
  exit's "notably" list can name ED-24 and ED-25.
- **01:** TOOL-10's brief grew by one deliverable, so its "about 7 working days" sizing should be rechecked.
- **This file:** check row 1's ED range becomes ED-1…25 at the integration pass.

**Checked and consistent, no edit.**
- 06 §9.2.1 already calls the no-build polygons cooked zone data, and `PlacementCheck` rule 2 is unchanged:
  §2.7.1 only produces its input.
- 06 GP-16 (e) already makes BENCH-5's snapshot rule-placed, and 01's Phase 3 proof line already says "built
  through the housing and city rules". TOOL-10's "inside the BENCH-5 frame budget" still measures an authored
  settlement against BENCH-5's frame budget.
- 04 §10.2 already logs `WallDeadline{tick, id}`, so the dev clock needs no replay change; 06 §11 rule 4 confines
  wall time to durable timers, `Wall` effects and calendar resets, which are exactly the clock's consumers.
- 06 §6.13's "activity director (08 §1.7)" and §12.4's Activities panel with a scoreboard are now covered by the
  Activity director and Encounter and match HUD rows.
- 08 §1.14 and 05 §1.16a are unaffected by the protected-action addition: vote-kicks were already reported to
  Trust (06 §6.10).

### Round 5 minor revisions owned by 09 and PLAN.md (2026-09-25)

`_round5-gaps.md` filed three minor gaps against 09 and PLAN.md:
- **backend/09.** §8.1 said "cell and gateway not started" and listed no HXL work, although `engine/server`,
  `engine/authority`, `engine/hxl`, `services/pkg/hxl` and `nats.c` were in the tree.
- **engine/09.** RT-01's committed pre-bench FAIL (`engine/ecs/SPIKES.md`) was missing from §8.1. §8.2 still
  said WP-0.6 "retires K2 and K3", and K2 was an open risk rather than a fired trigger.
- **tools/09.** Four items:
  - (a) RT-01 and K2, with no `docs/adr/` entry;
  - (b) the RT-13 overhead and the codegen fuel mismatch, with no owning WP for the two Luau patches;
  - (c) the mimalloc heap hazard and the `alignedAlloc` contention;
  - (d) six modules ahead of the round order, no README `Plan-Rev`, and D6 not mechanical.

This pass also takes three other inputs:
- item (3) of the engine/02 gate gap (09 §5.10.4 (b)'s stale `/GS` row);
- the hand-offs to 09 in the three subsections above;
- the lead's tasks: rebuild the status from the verified facts in `_round5-gaps.md`, record the fired
  triggers with the lead's disposition, open ADR-004a, and add PLAN.md's review record.

| # | File | What changed | Why |
|---|---|---|---|
| R5-09.1 | 09-roadmap-and-process.md (§8.1) | Rebuilt from the lead's verified facts only. **Committed:** WP-0.1 partial; core 141 and math 104 tests; WP-0.7 (121); the ECS (90); WP-0.10 (79, except two RT-13 clauses); WP-0.11 (51, Slang pinned); WP-0.13 (84, NS-0.4 partial); WP-0.15 (124). **In progress:** WP-0.2 and 0.5, 0.12, 0.14 and 0.19. **Not started:** 0.3, 0.4, 0.6c, 0.9, 0.16–0.18 and 0.20. Terms are defined: "committed" stands in for "merged" until the queue exists. Rows marked *re-checked* were confirmed by reading the tree: the presets' 3.24 minimum; SDL3 renderers and Wayland off; no merge-queue script; the ISA and gate state; WP-0.15r's open rows. WP-0.15 was committed ahead of WP-0.15r, so D4 holds the next backend WPs. New rows: fired risks, and a tree inventory that names every module path with its WP | backend/09, engine/09 and tools/09 (a) (c) (d); the lead's task (1). The older counts (174/115 tests; "cell and gateway not started") are superseded |
| R5-09.2 | 09 (§8.2) | First the in-progress WPs finish (0.14, 0.19, 0.12, 0.2, 0.5). Then, in order: 0.15r, 0.10r, **1.1a**, the rest of 0.1, 0.2r, 0.3 (with NS-0.4's libFuzzer nightly), **0.7b**, the rest of 0.8, 0.6c, 0.5r, 0.9 and 0.16. WP-0.6 no longer claims to retire K2 and K3, since (a) and (b) are done | engine/09 gap; the lead's task (1) |
| R5-09.3 | 09 (§7 intro, **K2**) | K2 is **fired**, with the numbers. Disposition, as the lead gave it: option A, wrapper optimization in WP-1.1a, keeping flecs, because raw flecs meets the budget with 40 % headroom; a custom ECS only if the optimized wrapper still fails RT-01 at the Phase 1 gate. The mitigation is rewritten, because a custom ECS alone would not help (SPIKES §3.4). Next triggers: the Ph1-midpoint indicator and the Ph1 gate. The intro now defines how a fired row is written | engine/09, tools/09 (a); the lead's task (1) |
| R5-09.4 | 09 (§7 **K3**) | Spike (a) findings. The recycling hazard (200/200 misrouted allocations and one crash in the raw API; 0/200 with pooled heaps), exact accounting at 460–890 ns per pair, and `alignedAlloc` ≈ 30× mimalloc. Mitigation: 02 §2.2 (R5-02.2) in WP-0.5. New triggers: a wrong-heap allocation, or > 3× `mi_malloc` | tools/09 (c) |
| R5-09.5 | 09 (§7, new **K39**) | Luau fuel metering, **fired**: codegen charges +1 fuel per `for` exited by `break` or `return`, and the interrupt overhead is ≈ 12–15 % against ≤ 10 %. Mitigations: the `codegen-fornloop-fuel` patch (interrupt in `FORNLOOP`), cells interpreter-only until then with `VmConfig` refusing, and the `fuel-counter` inline-counter patch, in WP-0.10r. The register now has 40 entries (K1–K39 plus K5b) | tools/09 (b); the lead's task (1) |
| R5-09.6 | **New `docs/adr/ADR-004a-ecs-rt01-structural-ops.md`**; 00-decisions.md (index; ADR-004 bullet) | **ADR-004a, status Open.** <br>• **Context:** the SPIKES.md numbers, per clause, run and op, with the callgrind breakdown. <br>• **Options:** A, optimize the World wrapper on flecs (SPIKES §3.4 items 1–5); B, a custom archetype ECS; C, raw flecs for hot structural paths. <br>• **Decision so far:** A, time-boxed to WP-1.1a, re-evaluated at the Phase 1 RT-01 gate. <br>• **Burst budget:** per sync point. The "per two sync points" reading is rejected, because it would relax a threshold. <br>• **Measured storage:** the verdict takes the worse of tag and DontFragment toggles. <br>• **Midpoint check:** if M1 is above 2.5×, a scoping spike for B. <br>• **Gate outcomes:** A accepted; B decided; or, with SERVER unmeasured, the ADR stays open. <br>• **Consequences**, and **closing measurements M1–M5.** <br>00 lists ADR-004a as open under ADR-004 | engine/09 ("open the ADR-004 decision in 00"); tools/09 (a) ("ADR-004a … an explicit call on the burst budget"); the lead's task (2) |
| R5-09.7 | 09 (§2.1 WP-0.6 acceptance; §2.2 WP-1.1 and new **WP-1.1a**; Phase 0 exit; §3.2 #4 and #5) | WP-1.1a delivers option A. Its only dependency is WP-0.8's committed ECS, so it starts now. It is accepted on M1 (World ≤ 1.6× raw flecs) with the tests and the hash unchanged. WP-1.1 runs the formal SERVER run (asserts off, no other load) and closes ADR-004a. Outcome notes go on WP-0.6 and reconciliations #4 and #5. The Phase 0 exit gains WP-0.10r and "option A under way" | engine/09 ("a WP for wrapper structural-op overhead, with a SERVER re-measure … as the formal RT-01 run"); the lead placed it in WP-1.1 |
| R5-09.8 | 09 (§2.1 WP-0.10 note; new **WP-0.10r**) | The owning WP for both Luau patches: `third_party/luau/patches/`, the `VmConfig` refusal, and `sim_abi.script`. Acceptance: RT-13's fuel identity (the pinned test flips, and fails with the patch reverted), ≤ 10 % overhead, and a refused cell codegen config. It is a D3 rework, because R5-02.4 made the refusal normative | tools/09 (b) (WP-0.10r or WP-1.6: RT-13 gates Phase 0, so WP-0.10r); the 02/04 hand-off |
| R5-09.9 | 09 (§2.1 WP-0.7 note; new **WP-0.7b**) | WP-0.7 is closed with the `cpp`, `go` and JSON emitters. `luau`, `sql`, `repl` and `lint` move to WP-0.7b, because 01 §5.4's Phase 0 clause needs Luau and SQL. `records`, `editor`, `docs` and `proto` go with their consumers | The lead's status (the emitters are stubs, "later WPs") against WP-0.7's deliverables and 01 §5.4 |
| R5-09.10 | 09 (§5.10.2 D1, D6, D7; K37); **new `docs/plan/PLAN-REV`** (6); **new `tools/status/check_status.cmake`** | **D1:** the counter starts at 6. Revisions 1–5 are drafts v1–v5, and 6 is this round's minor revisions. **D6 made mechanical before WP-0.3:** a CMake script, with no build and no network, fails the round audit when a module directory under `engine/`, `apps/`, `tools/` or `services/{cmd,internal,pkg}/` is not named in §8.1, or when a module README lacks `Plan-Rev` or exceeds `PLAN-REV`. WP-0.2 registers it and WP-0.3 absorbs it. **D7:** a module with an open delta keeps the revision before the change that opened it | tools/09 (d) ("make D6 mechanical: a script that diffs engine/, apps/ and services/ against §8.1") |
| R5-09.11 | 09 (§5.10.4, new part **(c)**); **20 module READMEs**, each with a "Plan conformance" section: `engine/{authority,core,ecs,gameplay,hxl,math,net,reflect,render,rhi,script,server}`, `apps/{cellserver,gateway}`, `services/`, `services/pkg/hxl`, `tools/{lint,rendertest,schemac,shaderc}` | D7 by hand at revision 6, one row per module group. **`Plan-Rev` values:** 6 for most modules; 5 for `engine/script` (WP-0.10r); 3 for `engine/core` and `tools/lint` (§5.10.4 (b)); 1 for `services/` (WP-0.15r). **Result:** `engine/authority`, `engine/server` and the apps conform (CONF-01/02/04/08 read). `LeaseHolder` follows the holder rule, but the C++ test partitions for 20 s where CONF-03 asks 60 s, so WP-0.14 lengthens it. `engine/gameplay`'s two extra record fields go to 06's owner. Directories without a README (`engine/platform`, `apps/samples`, `tools/ci`, `tools/prebuilt`, `tools/vendor`, and the Go packages under `services/`) are named in §8.1 without a `Plan-Rev` | backend/09 ("record each new module against its WP … run the D7 check … `LeaseHolder`"); tools/09 (d) ("Plan-Rev in every in-tree module README, plus a §5.10.4 (c) table") |
| R5-09.12 | 09 (§5.10.4 (b): the gate-object row; new "Illegal-instruction backstop" row) | `/GS-`, `-fno-stack-protector` and `-fno-sanitize=all` on the gate objects are now **conforming**, and the allowlist admits no cookie symbol. New row: trap passthrough is present in both hooks. The VEX/EVEX classifier, the crash-handler handover and check 5's fixtures stay open for WP-0.5r | engine/02 gap item (3); the 02/04 hand-off. The tree was re-checked: `cmake/HeliosIsa.cmake`, `isa_allowlist.cmake`, both `cpu_gate_hook.c` |
| R5-09.13 | 09 (WP-0.5, WP-0.5r, WP-0.9, WP-1.5, WP-2.2, WP-2.4, WP-3.1) | **WP-0.5:** 02 §2.2's sharded accounting and heap rule, with the recycling regression and ≤ 3× `mi_malloc`. **WP-0.5r:** the round-5 backstop rules and a minidump acceptance. **WP-0.9:** stable body keys and the `stable-order` patch. **WP-1.5:** `NoCrossUpdateCache` on hulls, and RT-03's permuted variant. **WP-2.2:** the same flag on vehicles. **WP-2.4:** replay keyframes and the script rebase (NS-2.4's keyframe clause). **WP-3.1:** the `ColocDecision` and `HandoffDecision` rows (NS-3.8's multi-cell clause) | The hand-offs to "09 WP owners" in the 02 and 04 subsection |
| R5-09.14 | 09 (WP-2.13, WP-3.8, Phase 3 exit, §6 Editor UI row; Phase 4 exit) | WP-2.13 gains party frames. WP-3.8 gains raid frames, the encounter and match HUD, and the AFK and vote-kick prompts. The Phase 3 exit adds ED-24 and ED-25, the §6 UI replays add ED-24 and ED-25, and the Phase 4 exit adds BE-A21 | Hand-offs in the 07/08 and 05 subsections |
| R5-09.15 | 09 (status header; §9 traceability row) | The round-5 changes are listed, and ADR-004a is added to the conformance list. A trace row covers the fired triggers | Same rule as fixes 33.9 and 41.11. The version label is left to the integration pass, as the 05 subsection does |
| R5-09.16 | ../PLAN.md (status; §1 "Where we are"; §5 ADR-004 row; §6 00 and 09 bullets; §8.1 Phase 1 row and the next-WP sentence; §10 intro, K2 row and a "Fired in Phase 0" note; §11 current state; §12 document map and K1–K39; **new §13 Review record**) | The status is **approved in round 5, with the minor revisions applied (revision 6)**, and two triggers have fired. §1 and §11 are rebuilt from the verified facts: committed and in-progress lists, test counts, failing gates, known deltas and CI, with `check_status.cmake` named. §10 and §5 match K2 and ADR-004a, and K39 and K3 are noted. §13 gives the five rounds' scores (8.6/8.7/8.5; 8.7/9.1/8.7; 8.8/9.1/8.7; 8.8/9.2/8.8; 9.2/9.0/9.0, approved with minor revisions), each round's §-range in this log, and a note that the round-5 minor revisions were applied afterwards (this §42) | The PLAN.md parts of all three gaps; the lead's task (4) |

**Criteria and IDs.**
- No criterion was added, renumbered or relaxed. RT-01 and RT-13 are unchanged. ADR-004a interprets RT-01's
  burst per sync point and proposes no change.
- New WPs: WP-0.7b, WP-0.10r and WP-1.1a.
- New risk: K39, so the register has 40 entries.
- New files: `docs/adr/ADR-004a-ecs-rt01-structural-ops.md`, `docs/plan/PLAN-REV` and
  `tools/status/check_status.cmake`.

**Hand-offs (other owners' files, not edited here).**
- **02:**
  - RT-01's text may state ADR-004a's measurement protocol: one sync point, the median of 7 warm bursts, the
    worst worker configuration, and the worse toggle storage.
  - §8.4's flecs row and §8.6's 09 row may name ADR-004a, WP-1.1a and WP-0.10r.
  - §7.4 quotes the interrupt overhead as 12–17 % (WP-0.10's runs), where the lead's verified figure is
    ≈ 12–15 %. Both exceed ≤ 10 %, and nothing depends on the difference.
- **06:** `engine/gameplay`'s `AttributeDef.id` and `ModifierDef.priority`, which 06's snippets lack.
- **WP-0.14:** lengthen the C++ holder-rule partition to CONF-03's 60 s.
- **WP-0.2:** add `tools/status/check_status.cmake` to `tools/ci/run_lints.cmake` once the in-progress
  modules are named, so it does not fail parallel work mid-round.
- **Integration pass:** move the sections' "draft v5" labels to revision 6; add a register-count check row
  (40) beside §41's historical "39 risks".

**Checked and consistent, no edit.**
- 02 §8.4's flecs row (R5-02.5), 02 §7.4 (R5-02.4) and 04 §10.2 (R5-04.4) describe the same disposition and
  the same two Luau patches as K2, K39, ADR-004a and WP-0.10r.
- 02 §2.2 (R5-02.2) is the rule that K3 and WP-0.5 cite.
- PLAN.md §8.1's Phase 0 exit gates are unchanged. RT-13 stays a Phase 0 gate, and RT-01 a Phase 1 gate.
- Links: 154 relative links and anchors in PLAN.md, 00, 09, ADR-004a and this file resolve (GitHub slug
  rules).
- `cmake -P tools/status/check_status.cmake` passes: 42 module directories, PLAN-REV 6.

## 43. Merged plan changes (09 §5.10.2 D1)

*The Integrator appends one row per anchor of each merged `Plan-Change:`, when it raises
`docs/plan/PLAN-REV`, and names the rework WPs that the change opened (D3).*

| PLAN-REV | Anchor | PR, WP | Change | Rework WPs opened | Conformance test (D5) |
|---|---|---|---|---|---|
| 7 | 06 §1.2 | #12, WP-0.19 (merged 2026-09-26) | `AttributeDef` gains `id: Name`, the identifier HXL and the slot constants use (`attr(e, Shield.Max)`). The modifier tuple gains `priority`, which picks the `PreAssign` and `PostAssign` winner; on a tie, the larger value wins. The HXL built-in list names comparisons, short-circuit `&& \|\| !` and `abs floor ceil` (exact IEEE operations). This closes §42's hand-off to 06 (R5-09.11) | None. The only code under the anchor (`engine/hxl`, `engine/gameplay`, `services/pkg/hxl`, all WP-0.19) came in the same PR and implements the new text; their READMEs record `Plan-Rev: 7` | `attributes: Dogma operator order` (`engine/gameplay`, priority and tie rule); the built-ins through the shared corpus `tests/corpus/hxl` (GP-1) |
| 8 | 02 §7.4 | #9, WP-0.10r (merged 2026-09-26) | Native codegen stays off on cells and world-script hosts: `VmConfig` refuses it (the text said "until `codegen-fornloop-fuel` lands … (today it warns)"). The vendored `third_party/luau/patches/0001-codegen-fornloop-fuel.patch` is the precondition for lifting the refusal, which is §8.1's P3 "codegen opt-in on cells" item behind 04 §10.2's interpreter-versus-native corpus run. The two patches are named by their files (`0001-codegen-fornloop-fuel.patch`, `0002-fuel-counter.patch`) | None. The code under the anchor (`engine/script`, `third_party/luau/patches/`) came in the same PR and implements the new text; `engine/script`'s README records `Plan-Rev: 8` | `compiler: a cell VmConfig with native codegen fails create()` (`engine/script`); fuel parity: `determinism: numeric for loops left early count the same fuel in native code` |
| 8 | 04 §10.2 | #9, WP-0.10r | The same refusal and precondition as 02 §7.4. "WP-0.10 pinned the divergence; WP-0.10r's test asserts parity"; the patch file names; §10.2's Phase 3 replay note says "with `codegen-fornloop-fuel`" | None (as for 02 §7.4) | As for 02 §7.4 |
| 8 | 09 §2.1 WP-0.10r | #9, WP-0.10r | The WP row says the refusal outlasts the patch (the patch is its precondition; lifting it is 02 §8.1's P3 item), where it said "until … merges (today `create()` warns)" | None | None needed (the WP definition follows 02 §7.4) |
| 8 | 09 §7 K39 | #9, WP-0.10r | K39's state: both mitigations are in review (WP-0.10r), and its mitigation text says the refusal stays after the patch | None | None needed (risk register) |
| 8 | 09 §5.10.4 (c) | #9, WP-0.10r | Status row only (not normative, 09 §5.10.1): `engine/script` is conforming with WP-0.10r at `Plan-Rev` 8 | None | None needed (status) |
| 8 | 09 §8.1 | #9, WP-0.10r | Status row only (not normative, 09 §5.10.1): the WP-0.10 row now also covers WP-0.10r and its test count | None | None needed (status) |
