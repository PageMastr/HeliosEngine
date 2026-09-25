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
| Every `RT-*`, `RC-*`, `ED-*` and `CL-*` ID cited exists in 02 §8.2, 03 §9.3, 07 §5.2 and 08 §4.4 (17, 11, 13 and 22 IDs) | Pass |
| 09's aliases `NS-p.k` resolve to a clause of 04 §11.4 (6/4/5/5/5/4 clauses for Ph0–5); `BE-A1…A14` and `GP-1…9` exist | Pass. `BE-A3a/A3b` were missing before fix 5.4 |
| "archetype" means only ECS storage, or the five reference game classes (01 §2) | 11 stale uses fixed (fixes 0.2, 1.2, 2.3, 2.4, 6.1) |
| No SQLite for services; the dev database is embedded-postgres | Pass after fixes 2.5, 8.3. The remaining SQLite mentions are the tool registry (07 §3.4, allowed by ADR-014) and "no SQLite" notes |
| Gateway game port | UDP 7777 in 04 §1 and 05 §5. No 27015 anywhere |
| Planet library name | `engine/pcg` everywhere (fixes 2.1, 7.1) |
| Library versions match ADR-013/014 and `third_party/MANIFEST.md` | Pass after fixes 0.1, 0.3, 0.4 |
| Go version | 1.27.1 in every plan section (fix 0.1). CLAUDE.md and CI still say 1.24 (§3) |

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

## 3. Out of scope for this pass (outside `docs/plan/`, owned by WP-0.1/0.2)

| Item | Where | Owner |
|---|---|---|
| Go 1.24 → 1.27.1 | `CLAUDE.md` (Build), `.github/workflows/ci.yml` (`go-version`) | WP-0.1 (09 §3.2 #13) |
| `cmakeMinimumRequired` 3.24 → 3.28 | `CMakePresets.json` | WP-0.1 (09 §3.2 #14) |
| Dear ImGui row says "Editor / launcher / debug UI" | `third_party/MANIFEST.md` | WP-0.2: launcher UI is RmlUi on SDL_Renderer (ADR-010) |
| Module order predates 02 §1.1 (no `hxl`, `records`, `pcg`, `assembly`…) | `engine/CMakeLists.txt` `HELIOS_MODULE_ORDER` | WP-0.2 (09 §3.2 #3) |
| One failing math test (noise statistics, `test_noise.cpp:309`) | `engine/math/tests` | The in-flight math WP |

## 4. Accepted variances (checked, deliberately unchanged)

- **`hmath`** (06, 07) is an alias for `helios::det` (02 §8.6 says so).
- **06's citation keys** (`R09-A14`, `R09-G5`, `R02-Q10`) differ from 01 §7's style. 06 defines them in its
  header and they are unambiguous.
- **"Reference game archetypes"** (01 §2, 09 §5.7) is the game-class sense, now in the glossary (fix 1.6).
- **`helios-launcher`** is the build target; players see `Helios.exe` and `HeliosLauncher.exe` (fix 2.7). 09's
  M0 dev command line uses the target name.
- **S03 (dynamic meshing) is ● for Star Citizen but Phase 5.** At the Phase 4 bar the SC class is therefore
  proven with static multi-cell meshing (S02), the architecture SC itself ships; dynamic meshing takes it past
  the bar in Phase 5. [PLAN.md §7](../PLAN.md#7-capability-coverage-for-the-five-reference-game-classes) states this
  explicitly rather than changing the matrix.
- **Stricter-than-scorecard targets** stay as they are, for example 04 Ph1 tick p99 ≤ 25 ms (01 allows 40 ms)
  and 06's 72 h soak (09 gives it phases). A section may be stricter than 01, never looser.
