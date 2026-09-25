# 09 — Roadmap, Build Loop, Testing and Risks

> **Status:** draft v1. **Conforms to:** ADR-001…014 and the phase vocabulary in `README.md`.
> **Owns:** the phased roadmap, work packages (WPs), exit criteria, the agent build loop, the test
> strategy, the risk register and the BENCH/scorecard lab (01 §3.2).
> **Criteria IDs.** `AAA-*` (01 §3), `RT-*` (02 §8.2), `RC-*` (03 §9.3), `ED-*` (07 §5.2) and `CL-*`
> (08 §4.4) are used as defined. Three sections number criteria without prefixes, so this section uses
> aliases: **`NS-p.k`** = clause *k* of the Phase *p* row of 04 §11.4 (clauses in written order, split at
> semicolons); **`BE-An`** = 05 §10 row A*n*; **`GP-n`** = 06 §12.2 item *n*.

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
| 1 First Light | Vertical slice: launcher → cell → client → editor | **M1 "Descent"** (BENCH-2) | 12–18 months | REN-5, ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1 |
| 2 Alpha Sandbox | Economy, quests, social; 500/zone; patching | **M2 "Osk Yard"** | 15–21 months | SRV Ph2, SEC-2/3/6, CNT-7, TOOL-2/6 |
| 3 Beta Scale | Multi-cell, instances, 5k CCU, live co-editing | **M3 "Harrow Orbit"** (BENCH-4/5/6) | 18–24 months | SRV Ph3, STB-5, TOOL-3/5, ITR-7 |
| 4 Launch Quality | **The AAA bar** | **M4 "Vane"** (BENCH-1…6, MIN and REF) | 18–24 months | every criterion with Ph ≤ 4 |
| 5 Ambition | Meshing, 100k shard, seamless galaxy, UGC, RT | **M5 "Lattice"** | 18–30 months | SRV Ph5, REN-6 (RT) |

**Phase exit rule (01 §3).** Phase *N* is done when every criterion with Ph ≤ *N* passes three consecutive
nightly runs on Windows and Linux (with the evidence classes of §5.6), and the phase-exit review (§5.7)
scores ≥ 9/10. Phases overlap: a Phase *N+1* WP may start once its dependencies are merged, but Phase *N+1*
cannot exit before Phase *N*.

**Rolling-wave detail.** Phases 0–2 are planned as WPs of 1–6 engineer-weeks; Phases 3–5 are epics,
decomposed when the phase starts. Any WP may split into sub-WPs (`WP-1.7a`) that inherit its criteria.
Rows are grouped by **track**: **CR** Core/Runtime, **RD** Rendering, **NS** Networking/Servers, **BE**
Backend, **GP** Gameplay, **ED** Editor/Tools, **CL** Client/Launcher, **CT** Content/Sample game, **IN**
Infra/CI (including release engineering and legal).

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
| WP-0.1 | IN | CI matrix | 00, 01 §3.10 | — | clang-cl, MinGW, ASan and headless jobs; Go 1.27.1 (Win + Linux); CMake ≥ 3.28 presets; ccache; SARIF | AAA-PLT-1 |
| WP-0.2 | IN | Layering, licence, IP lints | 02 §1.1 | 0.1 | `helios_module(… LAYER n HEADLESS\|EDITOR_ONLY)`; full `HELIOS_MODULE_ORDER`; DAG/HEADLESS/EDITOR_ONLY configure checks; AVX2 audit; licence scanner; name grep (01 §4.1) | RT-09 (layer/AVX2 part); bad module fails configure |
| WP-0.3 | IN | Nightly and scorecard | 09 | 0.1 | `scorecard.jsonc` (criterion → tests, class, platforms, threshold); nightly workflow; perf history and comparator; `tools/milestone/validate.ps1` | Nightly report covers every Ph0 criterion |
| WP-0.4 | IN | GPU runner (**human action**) | 03 §8.3 | 0.3 | User's Windows PC as self-hosted runner `win-gpu`; REF/MIN/SERVER purchase list | Vulkan goldens nightly on a real GPU |
| WP-0.5 | CR | Core and math completion | 02 §2.1 | 0.2 | `core::cpuGate()` in a non-AVX2 TU; CPUID, process spawn, async reads, exe manifests; `det::exp/ln/pow/asinh`, `Q16`, `Q32`, `Fixed64`, integer `rsqrt` | CL-17 (early); `det::` hashes equal on 5 compilers |
| WP-0.6 | CR | Spikes (2 weeks) | 02 §2.2, §4.2; ADR-004 | 0.5 | (a) mimalloc v3 heap thread-affinity under job migration; (b) flecs 4.1.6 `DontFragment` + synthetic 50k-entity pre-benchmark | Decisions in `docs/adr/`; pre-RT-01 within 2× budget, else custom-ECS ADR opened |
| WP-0.7 | CR | `helios-schemac` v0 + `reflect` | 02 §3 | 0.2 | 02 §3.1 grammar; `schema.lock`; emitters `cpp`, `go`, `luau`, `sql`, `repl` (full state), `lint`; `TypeInfo`, JSONC/tagged codecs | 01 §5.4 schemac clause; ≤ 1 s per 2,000 types; lock reuse fails |
| WP-0.8 | CR | ECS, records, assets v0 | 02 §3.3, §4, §6 | 0.7, 0.6 | flecs wrapper (IDs, registry, command buffers); client/server `.hrdb`; `.meta`, local DDC, `.hpak` v0; fuzz harnesses | AAA-SEC-4 on sample records |
| WP-0.9 | CR | Physics and PCG base | 02 §5.8, §7.1 | 0.5; 0.12 (twin) | One-grid `engine/physics`; fixed-point `hnoise` + Slang twin + corpus | RT-03 (Ph0 scope); RT-04 harness |
| WP-0.10 | CR | Luau host v0 | 02 §7.4 | 0.2 | VM, sandbox, interrupt budget, heap caps | RT-13 |
| WP-0.11 | RD | RHI: Vulkan 1.3 + Null | 03 §1 | 0.2, 0.5 | volk/VMA, bindless, SDL3 swapchain (`engine/app`), caps mask, Null traces | RC-1 (triangle, compute, bindless); AAA-REN-7 |
| WP-0.12 | RD | Slang, graph v0, rendertest | 03 §1.7, §2, §8.4 | 0.11 | SHA-pinned Slang bootstrap; `helios-shaderc`; `spirv-val`; graph (barriers, culling, traces); `helios-rendertest` + ꟻLIP | RC-1 (≤ 10 min, render-twice identical) |
| WP-0.13 | NS | HTP transport | 04 §2, §10 | 0.2 | netcode + reliable build, `UdpSocket` Win32/POSIX, channels, NetSim, fuzz targets | NS-0.1, 0.2, 0.4 |
| WP-0.14 | NS | Cell and gateway skeletons | 04 §3, §6.5 | 0.13, 0.8 | `ZoneHost`, `ZoneClock` (TiDi), gateway forwarder, AG/epoch/`IFence` interfaces | NS-0.3, 0.6 |
| WP-0.15 | BE | Go backend skeleton | 05 §1.1–1.4, §5, §8 | 0.1; 0.7 (generated types) | `helios-backend` (embedded NATS, PG, miniredis); identity; token minter + vectors; `LocalProcessPlacer`; ledger schema; testkit | BE-A1, A2; NS-0.5; AAA-PLT-2 (Ph0) |
| WP-0.16 | BE | Patch pipeline v0 | 05 §7; 08 §2.5 | 0.15, 0.5 | FastCDC in Go and C++ with shared vectors; `.hman`; Ed25519 trust chain; local CDN; `helios-patch publish` | CL-14 subset (tampered, expired, rolled back) |
| WP-0.17 | CL | Launcher and client skeletons | 08 §1.1, §2.1–2.2 | 0.11, 0.14, 0.16 | Baseline-x86-64 launcher (SDL3, no RmlUi): gate, login, fetch/verify, launch-code stub; client boots RHI and connects | 01 §5.4 launcher clause; headless login test |
| WP-0.18 | ED | ToolsFramework + shell | 07 §1.2–1.4 | 0.7, 0.8, 0.11 | Documents, commands, property-path transactions, journal, `helios-tool`; ImGui docking shell; property grid | ED-1 |
| WP-0.19 | GP | Kernel v0 | 06 §1.1–1.2, §4 | 0.7, 0.5 | Tags; attributes/modifiers (Dogma order, stacking); HXL in C++ and Go + corpus; `ItemDef`, `ReasonCodeDef` | GP-1 (corpus clauses) |
| WP-0.20 | CT | *Cinder Reach* skeleton | 01 §4.2 | 0.7 | `content/`, `helios.project.jsonc`, Tallis stub, Kestrel record, provenance policy | Records compile; 100 % provenance |

**Exit.** AAA-PLT-1, PLT-2 (Ph0), REN-7; RC-1; ED-1; NS-0.1–0.6; BE-A1, A2; RT-03 (Ph0 scope); GP-1 (corpus
clauses); the 01 §5.4 Phase 0 demos; both spike decisions recorded.

### 2.2 Phase 1 — First Light

**Goal.** One thin slice through every layer, at 50 players.

**Demo M1 "Descent" (01 §5.4).** The user logs in via the launcher, undocks a *Kestrel* from *Harrow High*,
descends from 400 km through *Harrow*'s atmosphere with no loading screen (**BENCH-2**), lands, walks into
*Saltmarch* and shoots Hollow drones with prediction, alongside 49 bots. Relogging restores position and the
starter loadout. In the editor they place Saltmarch props, edit the Kestrel record and press F5 for PIE
(ED-2).

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-1.1 | CR | ECS production | 02 §4 | 0.8, 0.6 | Access-DAG scheduler, ordered command buffers, `Mut<C>` dirty bits, prefabs | **RT-01** (gates custom-ECS ADR); AAA-SRV-4 (Ph1) |
| WP-1.2 | CR | World model, streaming | 02 §5.1–5.7 | 1.1, 0.9 | `FrameGraph`, `Reparent` + `FrameChanged`, grids and transfers, `ZoneInstance`, containers → `.hcc`, `StreamingManager` | RT-02; AAA-REN-5 (sim); RT-06 (I/O clause) |
| WP-1.3 | CR | PCG for Harrow | 02 §5.8 | 0.9, 1.2 | Terrain-graph VM, stamps, scatter, Scree fields, collision tiles, twin conformance | RT-04; RC-4; AAA-CNT-1 (Ph1), PLT-4 |
| WP-1.4 | CR | Assets, hot reload | 02 §6, §1.2 | 0.8 | `helios-assetd`, glTF/texture import, records/Luau reload, game-DLL reload | RT-05 (Ph1), RT-14; AAA-ITR-1 (Ph1), ITR-5 |
| WP-1.5 | CR | Physics, anim, nav, audio | 02 §7.1–7.3 | 1.2 | Shared `CharacterVirtual` mover, hulls, ozz graphs + hitbox poses, Recast per grid, audio events | RT-03 (full) |
| WP-1.6 | CR | Script, UI, input | 02 §7.4–7.6 | 1.4, 0.10 | Luau tasks, DAP, reload; vendor RmlUi + text stack; view-models; `UiSurface`; input contexts | RT-13; GP-9 (runaway, reload) |
| WP-1.7 | RD | Render pipeline | 03 §2–4 | 0.12, 1.2 | Extract/prepare/submit, `presentation`, GPU scene, 2-phase HZB, MDI, LOD, clustered forward+ PBR, CSM, PSO lists, breadcrumbs | RC-11 (Ph1: forward+, HDR) |
| WP-1.8 | RD | Space and planets | 03 §5 | 1.3, 1.7 | Starfield, nebula IBL, sun, flares, CDLOD + GPU tiles, Hillaire atmosphere, 2D clouds | RC-2, RC-3; AAA-REN-1 (Ph1 slice), REN-5, REN-6 (Ph1) |
| WP-1.9 | RD | VFX, post, UI bridge | 03 §6–7 | 1.7, 1.6 | GPU particles, shields, plumes, beams; exposure, bloom, AgX, TAA; RmlUi renderer; diegetic RTT | AAA-REN-6 (Ph1 particles) |
| WP-1.10 | NS | Replication | 04 §4 | 1.1, 1.2, 0.14 | Interest hash, priority accumulator, change masks, serialize-once chunks, frame quantization, RPCs; SEC-1 lint | NS-1.1, 1.2; AAA-SEC-1, SRV-6 (Ph1) |
| WP-1.11 | NS | Netgame, transitions | 04 §5, §7 | 1.10, 1.5 | Time sync, prediction (characters, ships), interpolation, basic lag comp, movement checks, handoff-seam zone transfer, reconnect | NS-1.3, 1.4; CL-4; AAA-SRV-5 (Ph1) |
| WP-1.12 | NS | Bots, inspector | 04 §10 | 1.11 | `helios-bot`; 16 bots per PR, 50 nightly; packet inspector | AAA-SRV-1/2/3 (Ph1) |
| WP-1.13 | BE | Persistence, fence | 05 §1.5, §1.13 | 0.15, 1.10 | `Fence.Advance`, checkpoints, load barrier, `item_refs` reconcile, character service | BE-A3a, A4 (fence); AAA-SRV-9/12 (Ph1) |
| WP-1.14 | BE | Core ledger, ops basics | 05 §1.1–1.2, 1.6, 1.14–1.17 | 0.15 | Wallets, grants, custody, idempotency, audits (no trading); launch codes; queue cap; live config; GM audit | BE-A4, A14; AAA-SRV-7 (Ph1) |
| WP-1.15 | GP | Kernel runtime | 06 §1.3–1.6, §8.6–8.7 | 0.19, 1.11 | Effects, abilities, ASM compiler (text graphs), prediction keys, cues, damage, respawn, XP | GP-1, GP-2; ED-4 |
| WP-1.16 | GP | Flight, on-foot, AI v1 | 06 §7, §8.1–8.2 | 1.15, 1.5 | FBW + thruster allocation, basic quantum travel, hitscan, BT + perception + navmesh drones, spawners | GP-4a; M02, M03, M05, G13 |
| WP-1.17 | ED | Viewport, world tools | 07 §1.3–1.5, §2.1 | 0.18, 1.2, 1.4, 1.7 | `imgui_impl_helios`, editor passes, T01, T02, T07 (Tallis), T24 MVPs | AAA-TOOL-1 (T01, T24) |
| WP-1.18 | ED | PIE, code tools | 07 §1.6, T10, T26 | 1.17, 1.12, 1.14 | PIE (cell + gateway + backend + clients + bots, NetSim toolbar); VS Code/DAP; Tracy overlays | ED-3; AAA-ITR-3 |
| WP-1.19 | ED | Data and planet tools | 07 T04, T08, T09, T15, T16, T27–T29 | 1.17, 1.3 | T08, T04, T15 (import), T16 (instances), T27 (GM), T28 (≥ 20 rules), T29 MVPs; T09 lite | AAA-TOOL-1; ED-2, ED-5; RT-09 |
| WP-1.20 | CL | Client application | 08 §1 | 1.6, 1.9, 1.11, 1.14 | State machine, threads, settings, gamepad, camera rig, HUD, frontend, map, MFDs | CL-4, CL-20; RT-12 (main thread); AAA-PLT-3; R06 |
| WP-1.21 | CL | Launcher v1 | 08 §2 | 0.17, 1.6 | RmlUi on `SDL_Renderer`, login, launch code over inherited pipe, download, quick verify, dev installer, tarball | CL-1, CL-17; AAA-PLT-3 |
| WP-1.22 | CT | Phase 1 content | 01 §4.2 | 1.3, 1.8, 1.16, 1.19 | Tallis, Harrow, Harrow High, Saltmarch, Scree, Kestrel, drones, 3 weapons (~150 assets); scripted BENCH-2 | M1; BENCH-2 nightly |
| WP-1.23 | IN | Lab, release, legal kickoff | 09; 08 §2.8 | 0.3, 0.4 | BENCH-2 on REF (Win + Linux); symbol store; **HSM code-signing procurement started**; FreeType FTL, SIL OFL, libopus approvals | Sign-offs in `MANIFEST.md` |

**Exit.** Every criterion with Ph ≤ 1: AAA-REN-1 (Ph1 slice), REN-5, REN-6 (Ph1), REN-7; ITR-1 (Ph1), 3, 5;
SEC-1, 4; PLT-1, 2 (Ph0), 3, 4; TOOL-1; SRV-1…7, 9, 12 at Ph1 values; CNT-1 (Ph1); RT-01…05 (Ph1 scope), 09,
12 (main-thread clause), 13, 14; RC-1…4, RC-11 (Ph1); NS-1.1–1.4; BE-A3a, A4, A14; GP-1, 2, 4a, 9 (Ph1
clauses); ED-2…5; CL-1, 4, 17, 20.

### 2.3 Phase 2 — Alpha Sandbox

**Goal.** The sandbox loop on the full ledger, 500 players per zone, production patching, and every P0 tool.

**Demo M2 "Osk Yard".** In *Osk* the user surveys a resource, places a harvester, crafts a rifle with rolled
perks and sells it on the Osk Yard market to a second account (escrow visible in `/admin`). They take
"Signal from Saltmarch" with dialogue, clear a Hollow lair and chat in a guild while 500 bots fill the zone.
The signed launcher then patches a 1 % content change by CDC and self-updates. **BENCH-1** (Harrow High
concourse, 200 bot avatars) joins the nightly lab as the first crowd measurement.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-2.1 | CR | Streaming, cook, paks | 02 §5.6–5.7, §6 | 1.2, 1.4 | HLOD, data layers, residency hook, all-asset reload, shared DDC, paks, zone cook | RT-05 (Ph2), 08, 17; AAA-ITR-2/4 |
| WP-2.2 | CR | Runtime breadth | 02 §4.3, §7 | 1.5, 1.6 | Update LOD, entity budgets, vehicles, IK and retargeting, audio banks/occlusion, nav tiles, IME, loc, rebinding, crashpad | RT-10 (Ph2), RT-16 |
| WP-2.3 | RD | Render Phase 2 | 03 §9.2 | 1.7–1.9 | Meshlet culling, impostor baker, liveries, clustered decals, material graph → Slang, probes, SSR, `IUpscaler` + FSR 2, shader reload, VRAM budgets | RC-5, RC-6; AAA-ITR-1 (Ph2) |
| WP-2.4 | NS | 500 per zone | 04 §2.5, §5.6–5.7, §7–8 | 1.11, 1.12 | AIMD, deltas, LOD groups, lag comp + projectiles, predicted abilities, TiDi, degrade stage, multi-instance cells, overflow layers, replay | NS-2.1–2.5; AAA-SRV (Ph2), SEC-3 |
| WP-2.5 | BE | Economy services | 05 §1.6–1.9, §4 | 1.14 | Full ledger (items, trades, escrow, `AtomicSwap`), market actors, industry, timers, resources, mail | BE-A5 (2k), A6, A7, A10; AAA-SEC-2, SRV-8 |
| WP-2.6 | BE | Social, ops, crashes | 05 §1.10–1.11, §1.16, §6; 08 §3 | 1.14 | Chat, presence, corps RBAC; queue lanes; `crashgw`, Sentry, symbol server; dashboards; warm standby; Linux images | BE-A3b, A8 (50/s); AAA-PLT-2 (Ph2); CL-15 |
| WP-2.7 | BE | Production patching | 05 §7; 08 §2 | 0.16, 1.21, 1.23 | Signed CDN, CDC diff, resume, repair, self-update, live/PTR, **signed installer**, uninstall | AAA-CNT-7, SEC-6, PLT-6 (Ph2); CL-9…12, 14, 18 |
| WP-2.8 | GP | Items, crafting | 06 §2–3 | 2.5, 1.15 | Ledger inventory, sockets and rolls, audited loot, fitting validator + `helios-fitsim`, survey, harvesters, schematics, industry | GP-5 (Ph2), 6, 7 (timers); G04, G05 |
| WP-2.9 | GP | Quests, AI, combat | 06 §5–9 | 1.16, 2.4 | Quests, missions, dialogue runtime; utility AI, lairs; command flight, EWAR, all strategies, seats; progression; death pipeline; PvP flags | GP-3 (Ph2), 4b, 9; M04, G03, G10, G11, G14 |
| WP-2.10 | ED | Graph, narrative tools | 07 T09, T11–T13, T25 | 1.19 | Vendor imgui-node-editor; T11 + logic graph; T09, T12, T13, T25 MVPs | AAA-TOOL-2 (part); ED-7 |
| WP-2.11 | ED | World and art tools | 07 T03, T05, T06, T15, T17–T23, T30 | 1.19, 2.3 | T03, T05, T06, T17–T23 MVPs; T15 state machines; T30 locks/presence; T28 for every asset class | AAA-TOOL-2, TOOL-6 |
| WP-2.12 | ED | Framework hardening | 07 §1, §4.1 | 2.10 | Direct-write detector, Sentry journal link, role layouts, git-CLI source control, nightly editor perf | ED-6, ED-8; AAA-ITR-6, STB-2 (journal) |
| WP-2.13 | CL | Client UI breadth | 08 §1.4–1.10 | 1.20, 2.5, 2.6 | Chat, inventory, market `<datagrid>`, dialogue, galaxy map, queue UX, synced settings, HOTAS, pseudo-loc | CL-2, 3, 5, 8, 16 (Ph2), 19 |
| WP-2.14 | CT | Phase 2 content | 01 §4.2 | 2.8–2.11 | Osk, resources, Osk Yard, "Signal from Saltmarch", lairs, guild (~500 assets); scripted BENCH-1 | M2; ED-7 |
| WP-2.15 | IN | Scale and soak lab | 04 §10; 05 §8 | 2.4 | `helios-swarm` on SERVER hardware, 1k-bot nightly, 24 h soak, 72 h backend chaos, release fuzzing, MIN lab | AAA-STB-1/4 (Ph2); BE-A6 |

**Exit.** Every Ph ≤ 2 criterion, notably AAA-SRV (Ph2); ITR-1, 2, 4, 6; STB-1, 2, 4 (Ph2); SEC-2, 3, 6;
CNT-7; TOOL-2, 6; PLT-2, 6 (Ph2); RT-05, 08, 10 (Ph2), 16, 17; RC-5, 6; NS-2.1–2.5; BE-A3b, A5–A8, A10, A12;
GP-3 (Ph2), 4b, 5 (Ph2), 6, 7 (timers), 9; ED-6…8; CL-2, 3, 5, 8–12, 14–16, 18, 19.

### 2.4 Phase 3 — Beta Scale (epics)

**Demo M3 "Harrow Orbit".** Harrow orbit runs on four cells. A *Mule* hauler crosses a boundary with three
crew and a boarding party walking inside (**BENCH-6**); handoff p99 < 100 ms. A 5k-CCU bot shard runs. Four
players enter the *Hollow Vault* strike (**BENCH-4**) in < 2 s and resolve a group conversation. Three
designers co-edit a Saltmarch settlement live (**BENCH-5**, ED-10). *Vane* runs reinforcement timers. A
newcomer passes the designer day (ED-9).

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-3.1 | NS | Authority v1 | 04 §6 | 2.4 | Ghosts, effects, co-location, handoff, zone migration, gateway merge, key rotation, cross-compiler replay, torture bots | NS-3.1, 3.2, 3.4; AAA-SRV-2/5/10/12 (Ph3); S02 |
| WP-3.2 | NS | Instancing, activities | 04 §7; 05 §1.12 | 3.1 | Activity service, lockouts, parties, matchmaking, phases, housing instances | NS-3.5; AAA-SRV-11; GP-7; W07, G12 |
| WP-3.3 | BE | Backend scale-out | 05 §6, §9 | 2.5, 2.6 | K8s + Agones, 5k CCU, chat 5k msg/s, PG failover, rollback tools, collab service | NS-3.3; BE-A11, A13; AAA-SRV-3/9 (Ph3), STB-5 |
| WP-3.4 | CR | Runtime scale | 02 §8.1 | 2.1, 2.2 | Nested grids, boarding, IORing/io_uring, crowd LOD, voice (libopus), Earth-size PCG, buoyancy, 100k records | RT-06, 07, 11; AAA-CNT-1 (Earth), 2, 4, 5 (Ph3) |
| WP-3.5 | RD | Render Phase 3 | 03 §9.2 | 2.3 | Mesh shaders, D3D12 seam test, impostors and brackets, HLOD, froxel fog, DDGI, volumetric clouds, oceans, PVT, nebula v1 | RC-7, RC-11 (Ph3); AAA-REN-4, REN-6 (Ph3) |
| WP-3.6 | GP | Gameplay Phase 3 | 06 §12.1 | 2.8, 2.9, 3.1 | Boarding/EVA, multi-crew, killmails, housing, cities, territory, crimewatch, group conversations, fleet AI, modular ships, 1 Hz command flight | GP-3 (Ph3), 5, 8; M01, M06, M07, M09, G07, G08, G16 |
| WP-3.7 | ED | 26/30 tools + live edit | 07 §5.1 | 2.10–2.12, 3.3 | T30 edit instances, web Writers' Room and Loc Review, T14 MVP, remote control, AAA polish | AAA-TOOL-3, 5, ITR-7, STB-2 (Ph3); ED-9…12 |
| WP-3.8 | CL | Client/launcher Ph3 | 08 §4.3 | 2.7, 2.13 | Install tiers, pre-download, in-game heal, overview grid, group dialogue UI, internal addons, AppImage, release gates | CL-7, 13, 16 (Ph3); AAA-PLT-6, STB-1 (Ph3) |
| WP-3.9 | CT | Phase 3 content | 01 §4.2 | 3.1–3.7 | Multi-cell orbit, Mule, settlements, Vane, Hollow Vault, phased story (~1,000 assets) | M3; BENCH-4/5/6 nightly |
| WP-3.10 | IN | Scale lab | 04 §10 | 3.1 | 10k-bot weekly chaos, cross-compiler replay CI, colo decision (05 §6.4) | AAA-STB-3 (Ph3) |
| WP-3.11 | IN | Security Ph3 | 04 §9 | 3.1 | 24 CPU-h fuzzing, text sanitization, rate limits, rewind caps, X25519 re-key design | AAA-SEC-5, SEC-7 |

**Exit.** Every Ph ≤ 3 criterion, notably AAA-SRV (Ph3); REN-4, 6 (Ph3); ITR-7; STB-1, 2, 3 (Ph3), 5;
CNT-1 (Earth), 2, 4, 5; SEC-5, 7; TOOL-3, 5; PLT-6 (Ph3); RT-06, 07, 11; RC-7, 11 (Ph3); NS-3.1–3.5;
BE-A11, A13; GP-3 (Ph3), 5, 7, 8; ED-9…12; CL-7, 13, 16 (Ph3).

### 2.5 Phase 4 — Launch Quality: the AAA bar (epics)

**Demo M4 "Vane".** The *Bastion* capital leads a 2,000-ship battle at TiDi 10 % (**BENCH-3** ≥ 45 fps on
REF). Six players clear the *Lattice Heart* raid, a season starts from the calendar, and ~300 voiced lines in
two languages play auto-staged. A 72 h soak at 50k CCU is clean, the pentest has no open critical or high
findings, every BENCH scene meets budget on MIN and REF, and all 30 tools are AAA-complete.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-4.1 | RD | AAA render features | 03 §9.2 | 3.5 | Visibility buffer, VSM, clouds golden, TAA + upscalers, HDR10, DLSS/XeSS plugins, Nanite-lite v1, 120 fps mode; D3D12 gate | RC-8, 9, 11 (Ph4); AAA-REN-1/2/3/6 |
| WP-4.2 | CR | Perf and memory closure | 02 §2.2; 03 §8 | 4.1, 3.4 | BENCH-1…6 on MIN and REF, Windows and Linux | RC-10; RT-10, 12; AAA-CNT-3, PLT-5; CL-6, 21 |
| WP-4.3 | NS | Battle scale | 04 §8 | 3.1 | Fleet aggregation, PVS, offload workers, hot standby, 8k sessions per gateway, XDP | NS-4.1–4.4; AAA-SRV-3/10 (Ph4); S05 |
| WP-4.4 | BE | Backend Phase 4 | 05 §9 | 3.3 | 10k tx/s, 30k AG/s checkpoints, 50M entities under lifecycle, 200/s admission, OIDC/MFA, N/N+1 restarts, DR, public API, hotfix ≤ 15 min | BE-A5 (10k), A8 (200/s), A9; AAA-STB-6, ITR-8, CNT-5 (Ph4); G17 |
| WP-4.5 | GP | Gameplay Phase 4 | 06 §12.1 | 3.6 | AI LOD T0–T3 + HTN, resource network, device aim assist, seasons, sovereignty, insurance, character creation | GP-3; G15, G17 |
| WP-4.6 | GP | Animation, cinematics, VO | 02 §7.2; 07 T13–T15, T22 | 3.7 | Motion matching, FACS facial + lip-sync, T13 auto-staging, T14/T22 AAA, 2-language VO, 500k strings | ED-13; RT-15; AAA-CNT-6; R04 |
| WP-4.7 | ED | 30/30 tools | 07 §5.1 | 4.6 | T27 web admin, staged live-shard edits, accessibility pass | AAA-TOOL-4, STB-2 (Ph4); ED-12 |
| WP-4.8 | IN | Security hardening | 04 §9; 05 §6.5 | 3.11 | X25519 re-key decision and build, anti-cheat decision, DDoS drill, **external pentest** | AAA-SEC-8; NS-4.5; CL-22 |
| WP-4.9 | CL | Client/launcher Ph4 | 08 §4.3 | 3.8 | OIDC/PKCE, storefront plugin, HDR, public addons, gyro, TTS, RTL text, hang dumps | CL-6, 16 (Ph4), 22 |
| WP-4.10 | CT | Phase 4 content | 01 §4.2 | 4.1–4.6 | Bastion, battle scenario, Lattice Heart, seasons, VO (~1,500 assets) | M4 |
| WP-4.11 | IN | Launch soak, legal | 01 §5.2 | all Ph4 | 72 h 50k-CCU soak; crash-rate programme; **SWG terrain patent review signed**; licence sign-offs closed; SBOM | AAA-STB-1, 3, 4 (Ph4) |

**Exit — the AAA bar.** Every criterion with Ph ≤ 4, including AAA-REN-1, 2, 3, 6; CNT-3, 5, 6; STB-1…4
(Ph4), 6; SEC-8; TOOL-4; ITR-8; PLT-5; RT-10, 12, 15; RC-8…11; NS-4.1–4.5; BE-A5 (10k), A8, A9; GP-3;
ED-12, 13; CL-6, 9, 16, 21, 22; plus the patent and licence sign-offs as M-class evidence.

### 2.6 Phase 5 — Ambition (epics)

**Demo M5 "Lattice".** *Ember* and *Quiet* join. The user flies seamlessly through the Lattice between
systems. A moving battle splits and merges cells, and a killed cell costs ≤ 1 s of hitch. BENCH-3 scales to
3,000 ships, a 100k-bot shard runs, players script structures, and RT effects run on SHOWCASE.

| WP | Tr | Scope | Own | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-5.1 | NS | Dynamic meshing (v2) | 04 §6.5 | 4.3 | Split/merge, cluster partitioning, handoff < 50 ms | NS-5.1, 5.2; AAA-SRV-5 (Ph5); S03 |
| WP-5.2 | NS | Gateway replication layer | 04 §6.5 | 5.1, **patent review** | Gateway-owned interest, priority, ghost records | NS-5.3; AAA-SRV-12 (Ph5) |
| WP-5.3 | BE | 100k shard | 05 §9 | 4.4 | Partitioned ledger 50k tx/s, 500/s admission, 10 s checkpoints | NS-5.4; BE-A8 (500/s); AAA-SRV (Ph5) |
| WP-5.4 | GP | Seamless galaxy | 02 §8.1; 06 §4, §7 | 5.1 | Lattice travel, Ember and Quiet, economy sim, ecosystems | W06, G18 |
| WP-5.5 | GP | Player scripting | 06 §11; 08 §1.12 | 4.8 | Metered UGC Luau VMs, addon portal, player blueprints | Security review; hostile-UGC suite |
| WP-5.6 | RD | Ray tracing | 03 §9.2 | 4.1 | RT shadows, ReSTIR, Nubis³, volumetric nebulae, frame generation | RC-11 (Ph5); AAA-REN-6 (Ph5) |
| WP-5.7 | CR | Mods, destruction | 02 §1.2 | 4.2 | Stable C ABI, learned motion matching, destruction hooks | R06-ENG-32 tests |

**Exit.** AAA-SRV (Ph5), REN-6 (RT); RC-11 (Ph5); NS-5.1–5.4; BE-A8 (500/s); the WP-5.5 security review.

---

## 3. Dependency graph, critical path and reconciliations

### 3.1 Graph

```
LANE          PHASE 0                                  PHASE 1
Spine         0.1 ─► 0.2 ─► 0.7 ─► 0.8 ──────────────► 1.1 ─► 1.2 ─► 1.10 ─► 1.11 ─► 1.15 ─► 1.16 ──┐
Core side           0.2 ─► 0.5 ─► 0.6 ─(ECS decision)─► 1.1      1.2 ─► 1.4, 1.5, 1.6               │
Rendering           0.2 ─► 0.11 ─► 0.12 ─► 0.9 ─► 1.3 ─┐                                            │
                                   0.12 ─► 1.7 ────────┴─► 1.8 ─► 1.9 ──────────────────────────────┤
Servers             0.2 ─► 0.13 ─► 0.14 ──────────────► 1.10                                        ├─► 1.22 ─► M1
Gameplay            0.7 ─► 0.19 ─────────────────────► 1.15                                         │
Editor              0.8 + 0.11 ─► 0.18 ─► 1.17 ─► 1.18, 1.19 ───────────────────────────────────────┤
Backend/client      0.1 ─► 0.15 ─► 0.16 ─► 0.17 ─► 1.13, 1.14 ─► 1.20, 1.21 ────────────────────────┘
Later chains  net 2.4 ─► 3.1 ─► 4.3 ─► 5.1 ─► 5.2 │ ledger 1.14 ─► 2.5 ─► 3.3 ─► 4.4 ─► 5.3
              render 1.8 ─► 2.3 ─► 3.5 ─► 4.1 ─► 4.2 ─► 5.6 │ tools 1.19 ─► 2.10/2.11 ─► 3.7 ─► 4.6 ─► 4.7
Human-gated   H1 GPU lab (RC-3/4) · H2 SERVER host (RT-01, SRV) · H3 HSM cert (2.7) · H4 licence sign-offs
              H5 art/VO · H6 playtesters · H7 pentest (4.8) · H8 patent counsel (4.11, 5.2)
```

**Critical path to M1** is the spine: schema → ECS → world → replication → prediction → gameplay → content.
**Near-critical** is the rendering lane, which must meet 1.3's
GPU-terrain contract for BENCH-2. The cheapest schedule protection is landing 0.7's grammar and 1.1's ECS API
as interface WPs early. **From Phase 2**, the critical path is networking scale (2.4 → 3.1 → 4.3), and the
Phase 4 exit is also gated by H1, H6 and H7, which the loop cannot shorten.

### 3.2 Reconciliation of cross-section conflicts

| # | Conflict | Resolution (binding for the loop) |
|---|---|---|
| 1 | Ledger: README puts it in Ph2; 05 has a core ledger in Ph1 | **Ph1 core ledger** (wallets, grants, custody, idempotency, audits) for starter loadouts and bounties; no player-to-player value. **Ph2 full economy** (trades, escrow, market, mail). S06 and AAA-SEC-2 stay Ph2 |
| 2 | AAA-TOOL-3 has T15 complete in Ph3; facial animation (R04) is Ph4 | 07 §2 rule: T15 is AAA-complete in Ph3 for every item whose runtime has shipped. Motion matching (02 §7.2), FACS facial and lip-sync go with R04 in WP-4.6 and gate TOOL-4 (01 and 07 updated) |
| 3 | Module order and LAYER checks absent from `HeliosModule.cmake` | WP-0.2 implements 02 §1.1. `linux-headless` is the server preset (02 §1.1 now uses this name) |
| 4 | mimalloc heap thread affinity | WP-0.6a spike before per-tag heaps; fallback is header-based tag accounting (today's design) |
| 5 | flecs `DontFragment`; RT-01 timing | WP-0.6b pre-benchmark in Ph0, so the custom-ECS decision cannot surprise Ph1. RT-01 on SERVER hardware stays the formal gate |
| 6 | libopus (02 §6.2) | **Approved** (BSD-3). Recorded by WP-1.23; vendored with voice (WP-3.4) or VO cooking (WP-4.6) |
| 7 | BE-A3 (Ph1, standby ≤ 10 s) vs warm standby Ph2 (04) and SRV-12 Ph1 = reconnect (01) | **BE-A3a (Ph1):** cell kill → restart from checkpoint, 0 item deltas, loss ≤ 60 s, players reconnect. **BE-A3b (Ph2):** warm standby ≤ 10 s. 01's ≤ 10 s hitch *without* reconnect stays Ph3 (05 §10 updated) |
| 8 | BE-A5 2k tx/s at Ph3 vs AAA-SRV-8 at Ph2 | 01 wins: 2k at Ph2, 10k at Ph4 (05 updated) |
| 9 | BE-A8 50/s at Ph3 vs AAA-SRV-7 at Ph2 | 01 wins: 50/s Ph2, 200/s Ph4, 500/s Ph5 (05 updated) |
| 10 | BE-A12 weaker than AAA-CNT-7 | CNT-7, CL-9 and CL-10 govern; A12 now restates CNT-7 |
| 11 | 08 has RmlUi in Ph0; 03 has it in Ph1 | Ph1 (WP-1.6, 1.9); the Ph0 launcher is a plain SDL3 window (08 §4.3 updated) |
| 12 | Gateway port 7777 vs 27015 | UDP 7777 |
| 13 | Go 1.24 (CI, CLAUDE.md, container) vs 1.27.1 (ADR-014) | WP-0.1 moves CI to 1.27.1 and fixes CLAUDE.md; code stays 1.24-compatible meanwhile (K16) |
| 14 | CMake 3.24 in presets vs ≥ 3.28 (ADR-011) | 3.28 (WP-0.1) |
| 15 | 06 §12.2 criteria have no phases | GP-1, 2 Ph1. GP-4a (FBW) Ph1, 4b (command flight) Ph2. GP-3: paths and 1k NPCs Ph2, moving-ship paths Ph3, LOD Ph4. GP-5: 24 h/500 bots Ph2, 72 h/5k Ph3. GP-6 Ph2. GP-7: timers Ph2, activity resume Ph3. GP-8 Ph3. GP-9: runaway/reload Ph1, 5k quests Ph2 |
| 16 | "3 consecutive nightlies" cannot literally cover 72 h soaks, pentests or designer days | Evidence classes (§5.6), with no threshold relaxed |
| 17 | imgui-node-editor, nats.c in the editor, libgit2 | Vendor imgui-node-editor with WP-2.10; allow nats.c in the editor (WP-0.2 MANIFEST note); git CLI only |

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
Gameplay 10–12, Editor/Tools 12–16 (a quarter on widgets, 07 §5.3), Client/Launcher 4–6, Infra/CI/Release
4–6, test automation 3–5, security 1–2. At $180–250k per fully loaded person-year, Phases 0–4 cost
roughly **$80–180M**, plus lab hardware, load-test compute and a pentest.

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
| Feel, fun, crash-rate hours (STB-1), designer day (TOOL-5) | Bots measure; they cannot judge | **Human playtesters and a newcomer designer** (H6) |
| Legal and trust | Scanners, checklists, SBOMs | **Counsel** (patents, licences), **HSM certificate** tied to a legal entity, **pentest vendor** (H3, H4, H7, H8) |

**Honest forecast.** Pure-engineering WPs may compress 3–10× against the human baseline. Phase exits will
not compress that much, because from Phase 1 on each needs H1–H8 evidence. At the end of Phase 0 the loop
reports WPs merged per week, review rejection rate and escaped defects, and re-forecasts Phases 1–4 from
those numbers. Until then, any calendar for an agent-built Helios is unknown.

---

## 5. The build loop

### 5.1 Roles

- **Director** (orchestrating agent): plans rounds from the scorecard, assigns WPs, holds module locks, runs
  the merge queue and escalates human-gated items.
- **Implementer** (one agent per WP): tests first where possible; works in branch `wp/<id>` and its own build
  directory `build/<wp-id>`.
- **Adversarial reviewer** (fresh agent, no implementer context): tries to break the change, writes extra
  failing tests, and checks CLAUDE.md rules, determinism, threading, parser bounds, Windows portability,
  licences and budget claims. It can block.
- **Integrator:** rebases, runs the PR tier, merges, updates `scorecard.jsonc`.
- **Auditor** (independent, per round and per phase): scores the scorecard (§5.7) and deep-audits two random
  merged WPs per round.
- **User** (human, Windows): validates milestones (§5.9), provides H1–H8 and accepts phase exits.

### 5.2 Rounds

1. **Select.** Rank failing criteria by current phase, then critical-path slack, then risk. Pick ready WPs
   (dependencies merged) whose criteria fail, within §5.5 limits.
2. **Interface pass.** A WP that changes a shared contract first lands a header- or schema-only PR.
3. **Implement and self-check** locally: `linux-gcc`, `linux-clang`, `cross-mingw`, `linux-debug-asan` for
   touched modules, `go test ./...` if `services/` changed, and `xvfb-run -a ctest --preset linux-gcc -L gpu`
   for render changes.
4. **Adversarial review**, at least one round. Each blocking finding is fixed or rebutted with evidence.
5. **CI PR tier** (§5.6): `windows-msvc-release`, `windows-clang-cl`, `linux-gcc`, `linux-clang`,
   `cross-mingw`, `linux-headless`, Go on Windows and Linux, lavapipe goldens, 16-bot smoke.
6. **Integration verify.** The merge queue rebases on `main` and reruns the PR tier; failure bounces the WP.
7. **Commit.** Squash-merge with the WP ID and criteria in the message.
8. **Score.** The nightly run and the round audit feed the next selection.

A round ends when every selected WP is merged or bounced (target: 1–3 days).

### 5.3 Definition of done (per WP)

1. Scope delivered; public APIs documented with threading rules.
2. New behaviour has tests that fail without the change (the reviewer spot-checks by reverting).
3. Warning-clean on MSVC, clang-cl, GCC 13, Clang 17 and MinGW; no OS-specific code outside `platform/`.
4. `ctest` green, including `gpu` on lavapipe and sanitizer runs of touched modules.
5. Layer, licence, schema-lock, AAA-SEC-4 and reference-name lints pass.
6. Performance-sensitive code states a budget with a benchmark and stays within thresholds.
7. Golden updates carry a justification the reviewer approves.
8. Every acceptance criterion is automated and registered in `scorecard.jsonc` with its evidence class.
9. Module README and affected plan sections are updated; a changed decision gets an ADR entry.
10. No open blocking review findings; integration verify green.

### 5.4 Cross-platform policy

Agents run in Linux containers, so MSVC and clang-cl are verified **only in CI** on `windows-latest`, and
local MinGW stands in for Win32 API portability. A WP touching `platform/win32`, the installer, WinHTTP,
Credential Manager or DPI must add a Windows-runner test that exercises that code. The nightly run on the
user's PC (WP-0.4) covers the Windows Vulkan path.

### 5.5 Parallelism limits

- **Per container** (4 vCPU today): at most 2 concurrent full builds; shared ccache.
- **In flight:** at most 6 implementer WPs, about one per track with ready work, spread across sessions.
- **Module locks:** one WP at a time may change a module's public headers.
- **Merges** go through one queue. Wire formats, schema grammar and the RHI API change only via interface WPs.
- Scale up only while review rejection stays below 30 % and the PR tier below 45 min p50.

### 5.6 CI tiers and evidence classes

| Tier | When | Contents | Budget |
|---|---|---|---|
| **PR** | Every push | 6-configuration matrix, unit tests, lints, Null traces, lavapipe goldens, Go tests (Win + Linux), 16-bot smoke, fixed-runner micro-benchmarks | ≤ 45 min |
| **Nightly** | Daily | All N- and H-class criteria, BENCH on the lab, 1 h soak with 50 → 1k bots, 1 h fuzz per target, ASan/UBSan/TSan, 5-compiler determinism hashes, editor perf | ≤ 8 h |
| **Weekly** | Weekly | 10k bots with chaos (Ph3+), 24 h and 72 h soaks, full cook, cross-compiler replay | ≤ 3 days |
| **Release** | Phase exits | ≥ 24 CPU-h fuzzing, pentest (Ph4), legal checklist, signing verification | — |

**Evidence classes** apply 01's rule to criteria that cannot run nightly, relaxing no threshold:
- **N (nightly):** 3 consecutive passing nightly runs on Windows and Linux (01's rule, literally).
- **H (hardware):** as N, on lab hardware. With no lab the criterion is *unmeasured*, which counts as failing.
- **W (long-running):** 2 consecutive scheduled passes, the latest within 14 days of the exit streak. Server
  soaks run on Linux, plus one Windows run where PLT-2 parity applies.
- **M (manual or external):** a signed record in `docs/evidence/` within the exit window (designer day,
  pentest, legal). Rate criteria need ≥ 3 ÷ target-rate observed hours: STB-1 at Ph4 (1 per 1,000 h) needs
  ≥ 3,000 play-hours.

### 5.7 Termination condition and independent scoring

- **Every round** an auditor scores 0–10: 60 % is the fraction of Ph ≤ *N* criteria passing under §5.6, and
  40 % is a rubric (01 §2 archetype proofs playable, code health, docs, platform parity). It lists the top five
  failing criteria.
- **Every phase exit** needs two independent auditors at **≥ 9/10**, every criterion green, no open critical or
  high risk without an accepted mitigation, and the user's Windows validation.
- **Loop termination ("AAA reached")** is the Phase 4 exit: every Ph ≤ 4 criterion green on Windows and Linux,
  two auditors at ≥ 9/10 on closeness to the user's goal (a studio could build SWG-, EVE-, Destiny-, Star
  Citizen- and SWTOR-class games, shown by the five archetype proofs in *Cinder Reach*), and user acceptance.
  Phase 5 runs only if the user opts in.
- **Pauses.** A criterion blocked on H1–H8 is parked and reported; the loop works elsewhere and never marks it
  green.

### 5.8 Preventing regressions

- **Ratchet.** A criterion that turns green joins its tier's blocking set; a PR that turns it red cannot merge.
- **Nightly failures** start a bisect agent, which reverts the culprit or opens a P0 fix WP that pre-empts the
  next round.
- **Budgets** are tracked per commit: ±5 % for render passes, ±10 % for backend, editor and iteration.
- **Goldens and determinism hashes** change only with justification; a hash change needs an ADR-level reason.
- **Flaky tests** are quarantined for at most 7 days and then count as failing; no skip without a linked issue.
- **Anti-gaming.** Auditors check that tests exercise real paths (bots use the real client core) and
  mutation-test samples of merged code.

### 5.9 How the user validates milestones on Windows

Install Visual Studio 2022 with "Desktop development with C++" and "C++ CMake tools" (these provide CMake and
Ninja), Git for Windows with LFS, Go 1.27.1 and a Vulkan 1.3 driver. In the **x64 Native Tools Command Prompt
for VS 2022**:

```bat
git pull
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
cd services && go build -o ..\build\go\ ./cmd/... && cd ..
powershell -ExecutionPolicy Bypass -File tools\milestone\validate.ps1 -Milestone M1
```

`validate.ps1` (WP-0.3) starts `build\go\helios-backend.exe run` and the servers in
`build\windows-msvc-release\bin\`, runs scripted checks, opens the launcher for the interactive part, and
writes `milestone-M1.txt` for the user to paste back. For the IDE, run `cmake --preset windows-vs2022` and
open the `.sln` under `build\windows-vs2022\`.

| Milestone | The user does | The user should see |
|---|---|---|
| M0 | Log in as `dev1`/`dev`; edit and undo the Kestrel record | Manifest verified; test scenes; "tick N, dilation 1.00"; clean undo |
| M1 | Launcher → Play; editor → F5 | BENCH-2 descent, no loading screen, ≥ 60 fps at 1080p Medium on REF-class hardware; PIE with 2 clients |
| M2 | Trade between two accounts; patch | Escrow settles; download ≈ changed bytes; signed installer |
| M3–M4 | Scripted BENCH tours, free play | In-game budget overlay matches the nightly report |

Linux uses the same flow with the `linux-gcc` preset (configure, build, `ctest`).

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
| Determinism | Physics, PCG + `hnoise` twin, HXL (C++ = Go), `det::`, `Fixed64`, tick replay, ASM rollback fuzz; MSVC, clang-cl, GCC, Clang, MinGW; 1/4/16 workers | Hashes per PR; full nightly | AAA-PLT-4, RT-03/04 |
| Performance | Micro-benchmarks per PR on a fixed runner; BENCH scenes nightly on the lab; per-pass GPU ms; ±5/±10 % gates | PR, nightly | REN-*, RT-12, 07 §4.1 |
| Fuzzing | libFuzzer on netcode/reliable reads, channels, generated decoders, JSONC/tagged/cooked/`.hpak` readers, manifests, hot-reload protocol; `go test -fuzz` on tokens, manifests, CDC | 1 h per target nightly; ≥ 24 CPU-h per release | AAA-SEC-7, CL-14 |
| Statistics | Chi-square and perk-pair independence at 10⁶ rolls per loot, plug and resource table | Nightly | GP-6 |
| Chaos | `kill -9` of cells, gateways, services mid-trade and mid-handoff; NATS loss; PG switchover; installer `FaultyFs`/`FaultyHttp` | Nightly, weekly | STB-5, CL-12, BE-A6 |
| Sanitizers, content | ASan/UBSan nightly; TSan on jobs, ECS, streaming; T28 rules with SARIF annotations | Nightly, PR | AAA-TOOL-6 |
| Security reviews | Threat model per phase; internal red team Ph3; external pentest Ph4 | Phase exits | AAA-SEC-8 |

**Lavapipe limits.** It rasterizes on the CPU: it proves correctness and synchronization at small sizes and
covers cap-masked fallbacks, but says nothing about performance or vendor drivers, so every H-class
criterion needs the real-GPU lab.

---

## 7. Risk register

L/I = likelihood/impact (H, M, L). Owners are roles; "User" is the human sponsor.

| ID | Risk | Cat | L/I | Mitigation | Owner | Trigger |
|---|---|---|---|---|---|---|
| K1 | Effort exceeds capacity (§4) | Scope | H/H | Phase gates; seams first; cut Ph5, then polish, never MVP | Director | Velocity < 60 % of forecast |
| K2 | flecs fails RT-01 or fragments | Tech | M/H | WP-0.6b pre-benchmark; flecs confined to `engine/ecs`; custom ECS behind its API | Runtime lead | Pre-bench > 2× budget |
| K3 | mimalloc heaps break under job migration | Tech | M/M | WP-0.6a spike; header-based tag accounting fallback | Runtime lead | Cross-thread free faults |
| K4 | Cross-compiler determinism drift | Tech | M/H | `det::`, no FP contraction, 5-compiler hashes, fixed-point integrators | Runtime lead | Any hash mismatch |
| K5 | GPU/CPU terrain mismatch on a vendor | Tech | M/H | Fixed-point `hnoise` twin; CPU-tile fallback; vendor lab | Render lead | RC-4 > 1 cm |
| K6 | Lavapipe limits (slow, unrepresentative) | QA | H/M | Small goldens, real-GPU nightly, pinned Mesa | Render lead | Suite > 10 min; HW-only regression escapes |
| K7 | No real GPU or server hardware | Org | H/H | User's PC as runner from Ph0; lab bought by mid-Ph1; unmeasured = failing | User | RC-3/4 unmeasured at Ph1 midpoint |
| K8 | Windows Vulkan driver defects | Tech | M/M | Caps workarounds, min-driver check, D3D12 seam/gate | Render lead | Any 03 §1.6 trigger |
| K9 | Shader/PSO stutter | Perf | M/H | Closed shading models, recorded PSO lists, RC-7 | Render lead | Any PSO frame > 50 ms |
| K10 | Upstream churn or stalls (Slang, flecs, RmlUi, SDL3, Luau) | Tech | M/M | Hash pins, quarterly bumps, confinement, glslang plan B | Runtime lead | Upstream blocker > 1 month |
| K11 | netcode lacks forward secrecy | Security | M/H | Secret-store key, daily rotation; X25519 re-key designed Ph3, built for the Ph4 review | Net lead | Key exposure; pentest finding |
| K12 | Single-threaded netcode limits gateways | Perf | M/M | Instance per worker/port; measure Ph2; replace L1 behind `Endpoint` | Net lead | < 256 slots per instance |
| K13 | Handoff dupes or lost AGs | Tech | M/H | `GhostRef`, effects-only mutation, fence, torture bots | Net lead | Non-zero conservation audit |
| K14 | Ledger throughput, hot rows | Perf | M/H | Unmaterialized system accounts, partitions, load tests | Backend lead | BE-A5 over budget |
| K15 | embedded-postgres fragile on Windows | Tech | M/M | Cached binaries, `pg-install --from`, cold-start CI | Backend lead | Weekly cold-start failures |
| K16 | Go 1.27.1 missing in agent containers (1.24.7 today) | Infra | H/L | 1.24-compatible code; cached toolchain; CI enforces 1.27.1 | Infra | `go build` fails in a container |
| K17 | SWG terrain patents US 8,115,765 / 8,207,966 / 8,368,686 | Legal | L/H | Generic node DAG; counsel review before any Ph4 public release | User (counsel) | Ph3 exit; any public build |
| K18 | Improbable US 11,792,306 vs the gateway replication layer | Legal | M/H | Review before WP-5.2; design-around; v1 does not need it | User (counsel) | WP-5.2 planning |
| K19 | Pending sign-offs: FreeType FTL, Inno Setup, EOS/EAC, Sentry FSL, SIL OFL | Legal | M/M | Scanner; isolated plugins; GlitchTip fallback; sign off before vendoring | User (counsel) | WP-1.6, 2.6, 4.8, 4.9 start |
| K20 | HSM code-signing certificate slow (CA/B HSM rules; eligibility unverified) | Legal | H/M | Procurement starts Ph1 (WP-1.23); unsigned dev builds; cloud-HSM OV fallback | User | Not issued 3 months before Ph2 exit |
| K21 | GPL/AGPL contamination (leaked SWG, Core3, SWG:ANH, Blender add-on, libgit2) | Legal | L/H | Scanner, provenance, review checklist | Director | Any hit |
| K22 | Third-party IP in content | Legal | L/H | Name grep, provenance, art review | Content lead | Grep hit |
| K23 | Art, VO, content bottleneck | Scope | H/H | Procedural/kitbash content, CC0 with provenance, contract artists | User | < 70 % of assets 2 months before exit |
| K24 | Agent quality drift, test gaming, architectural erosion | Process | H/H | Adversarial review, fail-without-change tests, mutation-sampled audits, layering lints | Director | Rejection > 40 %; > 2 escaped defects per round |
| K25 | Parallel-agent integration conflicts | Process | M/M | Module locks, interface WPs, merge queue | Director | > 2 rebase failures per round |
| K26 | CI capacity and wall time | Infra | M/M | ccache/sccache, `/Z7`, split jobs, self-hosted runners | Infra | PR tier > 45 min p50 |
| K27 | BENCH budgets missed at Ph4 | Perf | M/H | Budgets from Ph1, per-pass tracking, visibility buffer | Render lead | BENCH > 10 % over for 2 weeks |
| K28 | 2,000-ship fan-out beyond TiDi | Perf | M/H | Command replication, aggregation, 1k-ship bots from Ph2 | Net lead | BENCH-3 tick over budget at half load |
| K29 | Editor widget cost; iteration regressions | Scope | H/M | Widget library first, one owner, 25 % reserve; nightly editor perf | Tools lead | MVP slips a round; ITR red 3 nights |
| K30 | Hosting, egress and load-test cost | Org | M/M | Rerun 05 §6.4 per gate; colo decision Ph3; spot capacity | User | Forecast over budget |
| K31 | User validation bandwidth (one person) | Org | H/M | One script per milestone; milestones only per phase and mid-phase | Director | Milestone unvalidated > 2 weeks |

---

## 8. Current status and next steps

### 8.1 Phase 0 status (repository on 2026-09-25)

| Item | Status | Evidence / gap |
|---|---|---|
| Plan: ADR 00, sections 01–09, `docs/PLAN.md` | Draft v1 | Integration notes reconciled in §3.2; cross-section fixes logged in `CONSISTENCY.md` |
| CMake presets (MSVC, VS 2022, clang-cl, GCC, Clang, ASan, headless, MinGW) | Done | Minimum 3.24 → 3.28 (WP-0.1) |
| 25 vendored dependencies with `MANIFEST.md` | Done | RmlUi, text stack, nats.c, sentry, Slang, libopus due by phase |
| CI: Windows MSVC, Linux GCC/Clang, Go (Win + Linux) | Partial | No clang-cl, MinGW or ASan jobs; Go 1.24; no nightly |
| `engine/core` | Mostly done | 133/133 test cases pass (GCC 13.3, headless, warning-free, working tree incl. in-flight edits). Missing CPU gate, CPUID, process spawn, async reads, exe manifests |
| `engine/math` | Mostly done | 101/102 test cases pass; one noise-statistics check (`test_noise.cpp`) fails in the in-flight working tree. Missing `det::exp…`, `Q16/Q32/Fixed64`, integer `rsqrt` |
| LAYER/EDITOR_ONLY checks; module order | Not started | Order predates 02 §1.1 |
| schemac, reflect, ECS, records, assets, physics, PCG, script | Not started | WP-0.6–0.10 |
| RHI, render graph, Slang, rendertest | Not started | WP-0.11–0.12 |
| Net, cell, gateway; `services/`; patching | Not started | WP-0.13–0.16 |
| Launcher, client, editor; `schemas/`; `content/` | Not started | WP-0.17–0.20 |
| Scorecard, nightly, GPU runner | Not started | WP-0.3, 0.4 |

### 8.2 Next 10 work packages, in execution order

| # | Round | WP | Why now |
|---|---|---|---|
| 1 | 1 | **WP-0.1** CI matrix | AAA-PLT-1 gates Phase 0; every WP needs the MSVC and clang-cl signal |
| 2 | 1 | **WP-0.2** Layering, licence, IP lints | Must exist before modules multiply (reconciliation #3) |
| 3 | 1 | **WP-0.5** Core and math completion | CPU gate, `det::` extras and `Fixed64` feed PCG, HXL and command flight |
| 4 | 2 | **WP-0.7** schemac + reflect (interface first) | Head of the critical path |
| 5 | 2 | **WP-0.6** Spikes | Retires K2 and K3 before code depends on flecs or heaps |
| 6 | 2 | **WP-0.11** RHI Vulkan + Null | Head of the rendering lane; AAA-REN-7 |
| 7 | 2 | **WP-0.13** HTP transport | Independent; NS-0 criteria |
| 8 | 2 | **WP-0.15** Go backend skeleton | Independent; hand-written types until 0.7's Go emitter lands (K16) |
| 9 | 3 | **WP-0.3** Nightly, scorecard, `validate.ps1` | Phase exits cannot be measured without it |
| 10 | 3 | **WP-0.12** Slang, graph, rendertest | Completes RC-1; unblocks 0.9, 0.18 and Phase 1 rendering |

**In parallel, from the user:** register the Windows PC as a self-hosted GPU runner (WP-0.4) and install Go
1.27.1.

---

## 9. Traceability

| Requirement | Where |
|---|---|
| 01 §3 scorecard as terminator, §3.2 BENCH lab, §5.2 patents, §5.4 gates | §1, §2, §5.6–5.7, K17–K18 |
| Cross-section asks: 02 §8.6, 03 §9.6, 04 re-key, 05 §13, 07 §5.5, 08 §4.8 | §3.2; WP-0.2, 0.4, 0.6, 1.1, 1.23, 2.15; K11, K19–K20, K29 |
| R01-P2-22, R05-P1-20, R06-ENG-17/27 (bot swarms, all-target CI) | §5.6, §6 |
