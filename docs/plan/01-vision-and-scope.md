# 01 — Vision, Target Games, the AAA Bar, Scope and Non-Goals

*Helios master plan, section 01. This section follows `00-decisions.md` (ADR-001…014) and the phase vocabulary in `README.md`. Research citations use the key format described in §7.*

---

## 1. Product vision

### 1.1 What Helios is

Helios is **one integrated product for building and operating sci-fi MMOs**. Its six parts share one schema, one runtime and one content model.

| Part | Scope (ADR) |
|---|---|
| **Runtime** | C++20: jobs, archetype ECS, nested frames, Jolt per grid, Vulkan 1.3, Luau, game UI (002–005, 010–011) |
| **Editor** | ImGui over a UI-less ToolsFramework; real renderer and simulation; tools T01–T30; PIE with a real cell server; edit instances (009) |
| **Client** | Runtime + game UI + netcode; one camera from cockpit to on-foot (010) |
| **Launcher** | Login, news, chunk patching, repair, self-update (010) |
| **Server tier** | C++ Gateway + headless Cell servers: Shard → Zone → Cell (007) |
| **Backend** | Go services (auth, orchestrator, ledger, market, chat, social, persistence, telemetry, GM, patch); one `helios-backend` binary for local dev (008) |

**North star:** a studio of 30–150 people on Windows workstations builds, ships and operates an original MMO in the class of Star Wars Galaxies (SWG), EVE Online, Destiny, Star Citizen or SWTOR, or a hybrid of them in one shard, by writing records, Luau and domain graphs, never by modifying engine or backend source. Native C++ game modules remain optional for hot paths.

**The hybrid is the point.** Dust 514 bolted a different engine's FPS onto EVE's persistence and stayed "shallow and one-directional" (R01 §9); SWTOR shipped space as an on-rails minigame (R03 §7.9). Helios runs fleet space, seamless planets, shooter activities, a sandbox economy and cinematic story on one simulation, network and backend.

### 1.2 Who uses it

Studio teams on **Windows 10/11 x64** (ADR-001): programmers (VS 2022, Luau debugger T10, Tracy T26), systems designers (T08, T09, T11), world builders (T01, T04–T07), narrative/localization (T12–T14, T25), animation/VFX/audio/UI artists (T15–T20), live-ops/GM (T27) and SREs. Linux workstations get the same builds; production servers run Linux, and every server also runs natively on Windows.

### 1.3 Design pillars

Each pillar is checked by criteria in §3.

1. **Seamless scale.** One world model from a cockpit cup to a 10¹¹ m system: frame-local f64 positions, nested frames with `Reparent()`, a physics grid per ship/station/planet, object containers as the unit of editing, streaming and persistence (ADR-005/006, R04-P0-1…5). *Test:* AAA-REN-5, BENCH-2/6.
2. **Data-driven everything.** Genres are records, Luau and domain graphs over a C++ gameplay kernel of attributes, effects, tags and abilities (Dogma/GAS model, R09-GP-P0-1); one `.hschema` feeds every consumer (ADR-004). *Test:* AAA-TOOL-5.
3. **Live collaborative authoring.** The editor is a networked client of an **edit instance**; play instances pin published content (R03-P0-2/3); hot reload ≤ 2 s (ADR-012), against Destiny's overnight map load for a half-second change (R05 §6). *Test:* AAA-ITR.
4. **Server-authoritative by construction.** Clients never own value; feel comes from prediction and lag compensation (R05 §8.2); value moves only through the ledger; authority is epoch-fenced (ADR-007/008). *Test:* AAA-SEC.
5. **Windows-first iteration, Linux parity.** MSVC primary; the whole backend runs in-process on Windows without Docker; every commit also builds and tests on Linux (ADR-001/008). *Test:* AAA-PLT.
6. **Budgets are features.** Frame, tick, bandwidth, memory, entity and iteration budgets are declared, CI-enforced and shown in the editor, unlike HeroEngine's "X players for any value of X" (R03 §2.9).
7. **Seams first, generality later.** Each scaling feature ships on day one as an interface with a trivial implementation (one cell per zone, handoff/ghost APIs, activity state outside the cell). Star Citizen spent a decade retrofitting these (R04 §9.1); SpatialOS built the general case first and failed (R07 §1).

---

## 2. Reference game archetypes

Each archetype lists its experiences, research numbers, critical capability IDs (§2.6) and the reference-content proof (§4.2).

**(a) SWG-style sandbox.**
- *Experiences:* classless skill boxes (250-point cap, typed XP); gear crafted from spawned, stat-bearing resources that shift every 6–21 days (survey → harvester → schematic → experimentation → factory); bazaar and player vendors; houses and cities anywhere (150 → 450 m radius, mayors, taxes, elections); entertainers healing fatigue; JTL chassis + components under mass/energy budgets (R02 §6).
- *Numbers:* ten 16 km rule-generated planets; hundreds of structures per city.
- *Critical:* W04 (layer stack + runtime stamps), W08, G03, G04, G06, G07, G09, S06.
- *Lessons:* tuning as data (no NGE rewrite); lazy interiors, item caps, reclaim of abandoned property; dynamic hotspot rebalancing (R02 §8).
- *Proof:* Osk crafting (Ph2), Saltmarch settlements (Ph3).

**(b) EVE-style single-shard economy and fleet warfare.**
- *Experiences:* one universe; command flight (approach, orbit, warp); Dogma fitting with stacking penalties; regional order books; multi-day industry; corporations and sovereignty with reinforcement timers; battles of thousands under time dilation (R01 §3–5).
- *Numbers:* 1 Hz ticks; ~8,000 km grids; TiDi floor 10% with module response < 1 s; ~65k peak CCU.
- *Critical:* M01, M07, G01, G04, G06, G08, S04–S06, S09, W06, R03, R06.
- *Lessons:* no script on the hot path (R01-P0-4); live zone migration (R01-P0-1); character "brain" travels (R01-P0-2); timers pre-provision capacity (R01-P1-14).
- *Proof:* Osk market (Ph2), Vane territory and battle (Ph3–4).

**(c) Destiny-style action looter-shooter.**
- *Experiences:* console-shooter feel in a shared world; 3-player strikes; 6-player raids with checkpoints and weekly lockouts; public events; socket/plug loot; per-weapon, per-device aim assist; social hubs; seasons (R05 §3–5).
- *Numbers:* latency-free own avatar; ~10 Hz activity sync; ≥ 500 ms hitbox history; 30–60 Hz instances (R07 §3.7).
- *Critical:* M05, M06, G05, G10, G12, G17, R07, S08.
- *Lessons:* prediction, not client authority; activity progress survives a cell crash (R05-P0-5); loot RNG tested statistically in CI (R05 §5.1).
- *Proof:* Hollow Vault strike (Ph3), Lattice Heart raid (Ph4).

**(d) Star Citizen-style seamless universe.**
- *Experiences:* hangar on a rotating planet → multi-crew ship → orbit → quantum travel → boarding, with no loading screens; walking in moving ships; physical components with power and heat; persistent entities; server meshing (R04 §1).
- *Numbers:* ~4×10¹⁰ m systems (float32 step ≈ 2.4 km); ~500-player static-mesh shards; authority transfer < 2 ticks (R04 §4.3).
- *Critical:* W01–W03, W05, M02, M09, G16, S02, S03, S07, R02, R06.
- *Lessons:* seams from day one; a cleanup policy for every persistent record template; gameplay written for split authority; data-driven flight envelopes (R04 §6.1, §9).
- *Proof:* BENCH-2 descent (Ph1), Mule multi-crew (Ph3), seamless Lattice travel (Ph5).

**(e) SWTOR-style story-driven themepark.**
- *Experiences:* fully voiced branching stories; dialogue-wheel cinematics; **group conversations** (each player picks, a roll picks whose line plays, effects apply per player); companions; personal story phases; flashpoints (4) and operations (8/16); capacity-driven planet instances (R03 §4–5).
- *Numbers:* 200k+ VO lines, 558k+ strings, 3 launch languages; story instance start < 2 s (R03-P0-6).
- *Critical:* G10–G12, W07, R04, S08.
- *Lessons:* UI independent of the graphics API (SWTOR's DX12 port is blocked on UI); story coverage tooling; global identity; never build the game on an unfinished engine fork (R03 §3, §7).
- *Proof:* "Signal from Saltmarch": runtime (Ph2), group conversation (Ph3), auto-staged and voiced (Ph4).

### 2.6 Capability matrix

Columns: SWG = (a), EVE = (b), DST = (c), SC = (d), TOR = (e). Legend: **●** critical (archetype impossible without it) · **◐** needed for full fidelity · **○** optional · **–** not needed. **Owner** = plan section that must specify it; **Ph** = phase of the first shippable implementation. Sections 02–08 must list the capability IDs they satisfy.

| ID | Capability | SWG | EVE | DST | SC | TOR | Owner | Ph |
|---|---|---|---|---|---|---|---|---|
| W01 | Nested frames, f64 `WorldPos`, `Reparent()` | ◐ | ● | ○ | ● | ○ | 02 | 1 |
| W02 | Per-grid physics; walkable moving ships | ◐ | – | – | ● | ○ | 02 | 1 |
| W03 | Object-container streaming, client + server | ● | ◐ | ◐ | ● | ◐ | 02 | 1 |
| W04 | Deterministic cube-sphere planets, layer stack, runtime stamps | ● | – | ◐ | ● | ◐ | 02, 03 | 1 |
| W05 | Space ↔ surface ↔ interior, no loading screen | ○ | – | ○ | ● | – | 02–04 | 1 |
| W06 | System/galaxy model, jump graph, warp with prefetch | ◐ | ● | ○ | ● | ◐ | 02, 06 | 1 |
| W07 | Instances, layers, phases as interest scopes | ○ | ◐ | ● | ◐ | ● | 04 | 3 |
| W08 | Portal-cell interiors; unified containment | ● | ◐ | ◐ | ● | ● | 02 | 1 |
| M01 | Command flight, 1–2 Hz, thousands of ships | – | ● | – | ○ | – | 06 | 3 |
| M02 | 6-DoF Newtonian flight, flight assist, envelopes | ◐ | – | ○ | ● | ○ | 06 | 1 |
| M03 | Locomotion, mounts, vehicles | ● | ○ | ● | ● | ● | 02, 06 | 1 |
| M04 | Tab-target combat; telegraph shapes | ● | – | ○ | – | ● | 06 | 2 |
| M05 | Predicted gunplay, lag-compensated hits, aim assist | ○ | – | ● | ● | ○ | 04, 06 | 1 |
| M06 | Rollback-safe native ability graphs | ○ | – | ● | ◐ | ◐ | 06 | 1 (full: 3) |
| M07 | Statistical space weapons, sensors, EWAR | ○ | ● | – | ◐ | – | 06 | 3 |
| M08 | Damage pipeline: resists, layered pools, subsystems | ● | ● | ● | ● | ● | 06 | 1 |
| M09 | Multi-crew stations, item ports, boarding, EVA | ◐ | – | – | ● | ○ | 06 | 3 |
| G01 | Gameplay kernel (attributes/effects/tags/abilities) | ● | ● | ● | ● | ● | 06 | 1 |
| G02 | Typed records, client/server split, hash IDs | ● | ● | ● | ● | ● | 02, 07 | 1 |
| G03 | Skills/professions, typed XP | ● | ● | ◐ | ○ | ◐ | 06 | 2 |
| G04 | Resource spawns, harvesters, crafting, industry jobs | ● | ● | ○ | ◐ | ◐ | 05, 06 | 2 |
| G05 | Socketed item instances, tested loot RNG | ◐ | ◐ | ● | ◐ | ● | 05, 06 | 2 |
| G06 | Order-book market, contracts, vendors | ● | ● | ○ | ◐ | ◐ | 05 | 2 |
| G07 | Housing, decoration, player cities | ● | ◐ | – | ◐ | ◐ | 05, 06 | 3 |
| G08 | Territory, standings, PvP flags, timers | ◐ | ● | ○ | ◐ | ○ | 05, 06 | 3 |
| G09 | Social: chat, mail, guilds/corps RBAC, org wallets, hubs | ● | ● | ◐ | ◐ | ◐ | 04, 05 | 2 |
| G10 | Quests, procedural missions, public events | ◐ | ◐ | ● | ◐ | ● | 06 | 2 |
| G11 | Cinematic + group dialogue, VO, companions | ○ | – | ◐ | ○ | ● | 06, 07 | 2 |
| G12 | Activities: fireteams, checkpoints, lockouts | ○ | ○ | ● | ○ | ● | 04, 05 | 3 |
| G13 | AI: BT + utility + perception, AI LOD, spawns | ● | ◐ | ● | ◐ | ● | 06 | 1 |
| G14 | Death pipeline: wrecks, clones, insurance, killmails | ◐ | ● | ◐ | ● | ◐ | 06 | 2 |
| G15 | Character creation: species, morphs, fit | ● | ◐ | ◐ | ◐ | ● | 02, 03, 07 | 4 |
| G16 | Modular ship assembly, fitting budgets, liveries | ● | ● | ○ | ● | ○ | 06, 07 | 3 |
| G17 | Live-ops calendar, seasons, data hotfixes | ○ | ◐ | ● | ◐ | ◐ | 05 | 4 |
| G18 | Background economy/world simulation | ○ | ◐ | – | ◐ | – | 05 | 5 |
| S01 | Gateway + cells, per-zone tick 1–60 Hz | ● | ● | ● | ● | ● | 04 | 1 |
| S02 | Static multi-cell zones, ghosts, fenced handoff | ◐ | ◐ | ○ | ● | ○ | 04 | 3 |
| S03 | Dynamic split/merge, gateway replication | ○ | ◐ | – | ● | – | 04 | 5 |
| S04 | TiDi zone clock, overload policy, reinforcement | ○ | ● | – | ◐ | ○ | 04 | 2 |
| S05 | Battle interest management, 1,000+ participants | ◐ | ● | – | ◐ | ◐ | 04 | 4 |
| S06 | Double-entry item/currency ledger | ● | ● | ● | ● | ● | 05 | 2 |
| S07 | Write-behind persistence + cleanup policy | ● | ● | ◐ | ● | ◐ | 05 | 1 |
| S08 | Global identity, cross-shard social, placement | ◐ | ● | ● | ◐ | ● | 05 | 2 |
| S09 | Economy telemetry; public read API | ● | ● | ◐ | ◐ | ○ | 05 | 2 |
| R01 | HDR space look (nebula IBL, starfield, bloom); shields, plumes, GPU particles | ◐ | ● | ● | ● | ◐ | 03 | 1 |
| R02 | Atmosphere, planet terrain, volumetric clouds | ◐ | ○ | ◐ | ● | ○ | 03 | 1 |
| R03 | Fleet rendering: GPU culling, impostors, brackets | ○ | ● | ○ | ◐ | – | 03 | 4 |
| R04 | Cinematic characters, facial animation, lip-sync | ◐ | ○ | ◐ | ◐ | ● | 02, 03 | 4 |
| R05 | Crowds of 200+ with animation LOD | ● | – | ◐ | ◐ | ● | 02, 03 | 3 |
| R06 | Data-bound game UI, diegetic MFDs, data grids | ◐ | ● | ◐ | ● | ◐ | 08 | 1 |
| R07 | TAA/upscaling, view models, 120 fps mode | ○ | – | ● | ◐ | ○ | 03 | 4 |

**Tool dependencies (R08 §3, owned by 07).** All archetypes need T01, T08–T10, T23, T24, T26–T29; additionally SWG/SC need T04–T06, EVE/SC T07, Destiny/TOR T12–T14 and T25, SWG/EVE/SC T21, SWG/TOR T22.

---

## 3. The AAA bar: measurable acceptance criteria

"AAA" means **every criterion tagged Ph ≤ 4 passes**. The criteria form the **AAA Scorecard**; section 09's build loop runs it as its test suite and always works the highest-priority failing criterion. **Phase N is complete when every criterion with Ph ≤ N passes three consecutive nightly runs on Windows and Linux.**

### 3.1 Reference hardware

| Tier | CPU / RAM | GPU | Target |
|---|---|---|---|
| **MIN** | Ryzen 5 3600 / i5-9400F, 16 GB, SATA SSD, Win 10 22H2 | GTX 1660 SUPER / RX 5600 XT (6 GB) | 1080p Low, upscaled |
| **REF** | Ryzen 7 7700 / i7-12700, 32 GB, NVMe, Win 11 24H2 **and** Ubuntu 24.04 | RTX 4070 / RX 7800 XT | 1440p High |
| **SHOWCASE** | Ryzen 7 9800X3D, 32 GB | RTX 5080 / RX 9070 XT | 4K upscaled + RT (Ph5) |
| **DEV** | Ryzen 9 7950X / i9-14900K, 64 GB, 2 TB NVMe | RTX 4070 Ti SUPER | Iteration budgets |
| **SERVER** | 8 dedicated EPYC 9004-class cores + 32 GB per cell, Ubuntu 24.04 | — | Cell budgets |

All listed GPUs support Vulkan 1.3; MIN lacks mesh shaders and RT, so it exercises fallbacks. Measurements: Release builds, vsync off, warm shader cache, p99 over 5-minute scripted runs.

### 3.2 Benchmark scenes

Scripted, deterministic runs in the reference content, run nightly (owned by 09).

| ID | Scene | Load |
|---|---|---|
| BENCH-1 | Harrow High concourse (hub) | 200 avatars, 60 NPCs, 40 diegetic screens |
| BENCH-2 | Descent from 400 km orbit through atmosphere and clouds, landing, then into an interior | Continuous, no loading screen |
| BENCH-3 | Vane engagement | 1,000 ships (4 capitals), 200k particles |
| BENCH-4 | Hollow Vault encounter | 6 players, 80 AI, 60 Hz cell |
| BENCH-5 | Saltmarch settlement | 300 structures, 3,000 decor items, 150 avatars |
| BENCH-6 | Mule boarding | EVA → airlock → interior at 300 m/s under thrust |

### 3.3 Rendering and client performance

| ID | Criterion | Ph |
|---|---|---|
| AAA-REN-1 | REF at 1440p High: BENCH-1/2/4/5 average ≥ 60 fps with p99 ≤ 20 ms; BENCH-3 ≥ 45 fps. Phase 1 slice: BENCH-2 ≥ 60 fps at 1080p Medium | 4 (1) |
| AAA-REN-2 | MIN at 1080p Low, upscaled: BENCH-1/2/4 ≥ 60 fps with p99 ≤ 33 ms; BENCH-3/5 ≥ 30 fps | 4 |
| AAA-REN-3 | Performance mode: REF ≥ 120 fps in BENCH-4 | 4 |
| AAA-REN-4 | Zero frames > 50 ms caused by PSO creation in a 30-minute scripted run | 3 |
| AAA-REN-5 | Render and simulation test at 10¹³ m shows no jitter (ADR-005); BENCH-2 has no loading screen and no frame > 50 ms | 1 |
| AAA-REN-6 | Features golden-tested: clustered forward+ PBR, Hillaire atmosphere, HDR/bloom, GPU particles (Ph1); cube-sphere planets, froxel fog (Ph3); visibility buffer, virtual shadow maps, volumetric clouds, TAA + `IUpscaler`, HDR10 (Ph4); RT effects on SHOWCASE (Ph5) | 1–5 |
| AAA-REN-7 | Golden-image tests (lavapipe) cover every render feature on every commit | 0 |

### 3.4 Server scale

Targets come from R07 §8.3. R07's P0 / P1 / P2 columns map to Phases 2–3 / 3–4 / 5. They are validated by headless bot swarms on SERVER hardware. Tick profiles:
- strategic zones: 1–2 Hz;
- open world and space: 20 Hz, with 60 Hz physics substeps;
- instanced FPS: 30–60 Hz.

Tick p99 must stay at or below 80% of the tick period. The TiDi floor is 10%.

| Metric | Ph1 | Ph2 | Ph3 | Ph4 | Ph5 |
|---|---|---|---|---|---|
| AAA-SRV-1 Players per cell (20 Hz) | 50 | 500 | 500 | 500 | 1,000 |
| AAA-SRV-2 Players per zone | 50 | 500 | 2,000 | 5,000 | 10,000+ |
| AAA-SRV-3 CCU per shard | 50 | 2,000 | 5,000 | 20–50k | 100k+ |
| AAA-SRV-4 Replicated entities per cell | 5k | 20k | 50k | 50k | 100k |
| AAA-SRV-5 Zone transition / handoff p99 | ≤ 3 s | ≤ 3 s | < 100 ms | < 100 ms | < 50 ms |
| AAA-SRV-6 Client downstream steady/battle (kbit/s) | 256/512 | 256/512 | 256/512 | 256/512 | 256/1,000 |
| AAA-SRV-7 Login admission; login p95 | 5/s; 5 s | 50/s; 3 s | 50/s; 3 s | 200/s; 3 s | 500/s; 3 s |
| AAA-SRV-8 Ledger tx/s per shard | — | 2k | 2k | 10k | 50k |
| AAA-SRV-9 Non-value state lost on cell crash | 60 s | 60 s | 30 s | 30 s | 10 s |
| AAA-SRV-10 Largest battle in one zone at TiDi ≥ 10% (module response < 1 s wall) | — | — | 500 | 2,000 | 3,000+ |
| AAA-SRV-11 Story/strike instance start | — | — | < 2 s | < 2 s | < 2 s |
| AAA-SRV-12 Cell crash impact | reconnect | reconnect | ≤ 10 s hitch | ≤ 10 s hitch | ≤ 1 s hitch |

### 3.5 Iteration speed (DEV hardware, sample project)

| ID | Criterion | Ph |
|---|---|---|
| AAA-ITR-1 | Hot reload into running PIE client and cell, save → visible ≤ 2 s p95: Luau and records (Ph1), then shaders and assets (Ph2) | 1–2 |
| AAA-ITR-2 | Editor cold open to an interactive viewport ≤ 10 s with a warm DDC | 2 |
| AAA-ITR-3 | PIE (cell + in-process backend + 2 clients) starts in ≤ 15 s; placement edits apply live | 1 |
| AAA-ITR-4 | Single-zone incremental cook ≤ 60 s; full nightly cook ≤ 4 h (R05 §6) | 2 |
| AAA-ITR-5 | One gameplay `.cpp` edit → relinked editor ≤ 30 s (MSVC) | 1 |
| AAA-ITR-6 | New engineer from clone to in-game ≤ 30 min (shared cache); artist with prebuilt editor ≤ 10 min | 2 |
| AAA-ITR-7 | Edit-instance change reaches all editors ≤ 1 s; content publish ≤ 5 min | 3 |
| AAA-ITR-8 | Canaried data/Luau hotfix to live, no client patch or downtime, ≤ 15 min | 4 |

### 3.6 Stability

| ID | Criterion | Ph |
|---|---|---|
| AAA-STB-1 | Client crashes per 1,000 play-hours ≤ 10 / 3 / 1 (Ph2 / Ph3 / Ph4); all crashes produce symbolicated minidumps | 2–4 |
| AAA-STB-2 | Editor crashes ≤ 1 per 40 / 100 user-hours (Ph3 / Ph4); the crash journal loses at most one transaction (Ph2) | 2–4 |
| AAA-STB-3 | Cell crashes ≤ 1 per 1,000 / 5,000 cell-hours (Ph3 / Ph4) | 3–4 |
| AAA-STB-4 | Soak for 24 h (Ph2) and 72 h at 50k CCU (Ph4): zero ledger violations, RSS growth ≤ 2%, tick p99 within budget | 2, 4 |
| AAA-STB-5 | Chaos test: `kill -9` of any cell, gateway or service during trades and handoffs creates or destroys no value; players resume within ≤ 10 s | 3 |
| AAA-STB-6 | Rolling zone restarts with N/N+1 protocol compatibility and no disconnects | 4 |

### 3.7 Content scale and streaming

| ID | Criterion | Ph |
|---|---|---|
| AAA-CNT-1 | Systems ≥ 10¹¹ m across; walkable planets up to 6,400 km radius; terrain bit-identical on client and server | 1 (Earth-size: 3) |
| AAA-CNT-2 | Game thread never blocks > 1 ms on I/O; zero residency misses in BENCH-2 at 1,500 m/s; sustained I/O ≤ 150 MB/s | 3 |
| AAA-CNT-3 | Client memory: MIN ≤ 7 GB RAM / 5 GB VRAM; REF ≤ 12 GB / 10 GB | 4 |
| AAA-CNT-4 | Deterministic memory ceiling per cell; ≤ 16 GB for a 50k-entity cell | 3 |
| AAA-CNT-5 | 100k records compile in ≤ 60 s and load in ≤ 2 s (Ph3); 50M persistent entities per shard under lifecycle policies (Ph4) | 3, 4 |
| AAA-CNT-6 | 500k localized strings and 200k VO line IDs without perf cliffs | 4 |
| AAA-CNT-7 | Patch download ≤ 1.5× the changed bytes (CDC); verifying a 50 GB install ≤ 5 min | 2 |

### 3.8 Security

| ID | Criterion | Ph |
|---|---|---|
| AAA-SEC-1 | Every client→server message is classified by a schema lint; clients only *request* value changes, never assert them | 1 |
| AAA-SEC-2 | All value moves go through synchronous, idempotent ledger writes; conservation audit reads zero in every test | 2 |
| AAA-SEC-3 | Speed/teleport bots flagged within ≤ 2 s; stalled clients gain no invulnerability | 2 |
| AAA-SEC-4 | Client cooks contain no `server_only` data (CI; R08-ED-P0-01) | 1 |
| AAA-SEC-5 | Lag-compensation rewind capped (default 200 ms); client-reported hits allowed only as a per-activity PvE policy (R05-P0-4) | 3 |
| AAA-SEC-6 | AEAD UDP with connect tokens expiring in ≤ 45 s; cells never public; Ed25519 manifests; signed binaries | 2 |
| AAA-SEC-7 | Network parsers fuzzed ≥ 24 CPU-hours per release; user text sanitized; per-message rate limits | 3 |
| AAA-SEC-8 | External penetration test with no open critical or high findings | 4 |

### 3.9 Tooling completeness (R08 §3 IDs)

| ID | Criterion | Ph |
|---|---|---|
| AAA-TOOL-1 | MVPs of T01, T04, T08, T10, T15 (import), T16 (instances), T24, T26–T29 | 1 |
| AAA-TOOL-2 | R08-ED-P0-01…16 met; MVPs of T11–T13, T23, T25 | 2 |
| AAA-TOOL-3 | T05–T07, T15, T17, T19, T21 and T30 (live edit instances) complete; 26/30 tools at "AAA prio" (T15's motion matching and facial items follow their Ph4 runtime, 07 §2) | 3 |
| AAA-TOOL-4 | All 30 tools at "AAA prio", including T13 auto-staging, T14, T22 and the T27 web admin | 4 |
| AAA-TOOL-5 | **Designer day:** a designer new to Helios adds a hull variant, a weapon with rolled perks, a 3-step quest with dialogue and a vendor in ≤ 1 day, with no engineer and no restart | 3 |
| AAA-TOOL-6 | Every tool writes through transactions; every record type is editable in T08; every asset class has T28 rules | 2 |

### 3.10 Platform

| ID | Criterion | Ph |
|---|---|---|
| AAA-PLT-1 | Every commit builds and tests on MSVC 2022, clang-cl, GCC 13 and Clang 17, plus the MinGW cross-compile check | 0 |
| AAA-PLT-2 | All servers and `helios-backend` run natively on Windows without Docker (Ph0); Linux production images (Ph2) | 0, 2 |
| AAA-PLT-3 | Editor, client and launcher smoke-tested on Windows 10/11 and Ubuntu 24.04 | 1 |
| AAA-PLT-4 | Procedural generation bit-identical across MSVC and GCC/Clang (hash test; FMA and transcendentals controlled) | 1 |
| AAA-PLT-5 | Linux client within 10% of Windows on REF in every BENCH scene | 4 |
| AAA-PLT-6 | Signed Windows installer (Ph2); Linux AppImage with the same patcher (Ph3) | 2–3 |

---

## 4. Scope

### 4.1 Engine, Foundation layer, game content

| Layer | Contains |
|---|---|
| **Helios engine** (the product) | Everything in §1.1; generic data-configured systems (gameplay kernel, movement models, damage and death pipelines, crafting state machine, resource spawns, market, ledger, dialogue runtime, activity service, AI); build/cook/CI; bot swarm; Helm/Agones manifests; docs |
| **Foundation layer** (ships with the engine, forkable, like HeroEngine's "Clean Engine", R03 §2.5) | Luau, records and UI for common MMO plumbing: login/character select, HUD, inventory/market/chat widgets, default damage types and abilities, starter AI, quest templates, flight envelopes |
| **Game content** (the studio's) | Setting, rules, tuning, art, audio, story, economy parameters, live-ops calendar, monetization |

**Rules.**
1. A system two or more archetypes need, with different parameters, goes in the engine. A system only one game needs goes in the Foundation layer or content.
2. A CI grep keeps reference-content names (`Harrow`, `Kestrel`, …) out of engine and Foundation code.
3. DCC tools, payment processing and anti-tamper are integrated, not built.

### 4.2 Reference content: *Cinder Reach*

An original setting: a frontier cluster of five star systems, reachable again after the relay network called **the Lattice** reawakens. Three factions contest it:
- the **Meridian Directorate**, a chartered government-corporation;
- the **Free Compact**, independent haulers and privateers;
- the **Hollow**, a machine remnant that animates derelicts (the PvE enemy).

Playable species are humans and the digitigrade **Keth**, who exist to test retargeting and clothing fit.

| Ph | Content added | Proves | Unique assets |
|---|---|---|---|
| 1 | **Tallis** system: planet **Harrow** (1,500 km radius, atmosphere, weather), station **Harrow High**, outpost **Saltmarch**, **Scree** belt; **Kestrel** fighter; Hollow drones; 3 weapons | (d), (c) | ~150 |
| 2 | **Osk** system: resources, crafting, Osk Yard market; "Signal from Saltmarch" quests; lairs; guilds | (a), (b), (e) | ~500 |
| 3 | Multi-cell Harrow orbit; **Mule** 3-crew hauler; settlements; **Vane** territory; Hollow Vault strike; group conversation; phased story | all | ~1,000 |
| 4 | **Bastion** 400 m capital; 2,000-ship battle; **Lattice Heart** 6-player raid; seasons; ~300 voiced lines in 2 languages | AAA bar | ~1,500 |
| 5 | **Ember** and **Quiet** systems; seamless Lattice travel; scriptable player structures | Ambition | ~2,000 |

Reference content proves capabilities. It is not meant to be a 500-hour game.

---

## 5. Non-goals and honest constraints

### 5.1 Non-goals (for now)
- **Platforms:** consoles, mobile, macOS, web, cloud streaming, VR. The RHI seam keeps them possible, and D3D12 is decided in Phase 4 (ADR-001/003).
- **Genres:** a general-purpose engine for linear single-player, 2D or mobile games.
- **Networking:** SpatialOS-style generic distribution before Phase 5; P2P; host migration (ADR-007).
- **Visual scripting:** a general Blueprint clone. Graphs stay domain-level (ADR-002).
- **Player content:** UGC and player scripting before Phase 5; after that, sandboxed Luau with quotas. The Foundry lesson applies (R03-P2-1, R04-P2-23).
- **Other:** blockchain; business logic in SQL (ADR-008); replacement DCC or audio tools; operating a commercial game.

### 5.2 IP and licensing
- **No Star Wars IP.** No names, species, factions, silhouettes, sound signatures, Force-like powers or saber-like weapons. The same applies to EVE, Destiny, Star Citizen and SWTOR designs.
- **No forbidden code.** Nothing from the leaked SWG source, and no AGPL/GPL code (Core3 and Holocore are AGPL; SWG:ANH is GPL). Architecture is learned only from public descriptions (ADR-012).
- **Permissive dependencies.** Only MIT, BSD, Apache-2.0, zlib, BSL-1.0, ISC, the PostgreSQL licence and CC0 are allowed, enforced by a CI licence scanner. Proprietary SDKs (e.g. DLSS/Streamline) are optional isolated plugins.
- **Patent reviews.**
  - The SWG terrain patents US 8,115,765 / 8,207,966 / 8,368,686 are reviewed before any Phase 4 release (ADR-006).
  - Improbable's US 11,792,306 is reviewed before the Phase 5 gateway replication layer.
- **No extracted assets.** No assets or data come from commercial games, not even as test fixtures. Every asset's provenance is recorded in the asset DB.

### 5.3 The honest constraint, and how Helios reaches the bar
AAA engines and MMOs took hundreds to thousands of person-years:
- SWTOR: ~6 years, up to 800 developers and ~$200M, on a licensed engine (R03 §3.1).
- Star Citizen: >$800M since 2012, and about a decade retrofitting meshing and Vulkan (R04 §9).
- EVE: CCP maintains ~2.4M lines of Python on a 23-year-old stack (R01 §1).
- Destiny: builds took 24+ hours five years after launch (R05 §2.4).

Helios will not match that breadth early; pretending otherwise is how SWTOR ended up on an unfinished fork. The path:

1. **Vertical slice before breadth.** Phase 1 is thin but end-to-end, from launcher to gateway, cell, client, persistence and editor ("Ship Early", "Bring Up In Order", R05 §2.1).
2. **Seams before depth** (pillar 7).
3. **Reuse what is solved.** Jolt, Luau, volk/VMA, meshoptimizer, Recast/Detour, NATS, PostgreSQL and Valkey. We build what differentiates Helios: the world model, replication, the ledger and the authoring workflows.
4. **Measure, don't assert.** A feature without a passing scorecard criterion is not done.
5. **The sample game ships every phase.** *Cinder Reach* is playable end-to-end on Windows and Linux at the end of each phase.
6. **Phase 4 is the AAA bar; Phase 5 exceeds it.** We claim no Nanite/Lumen parity; the render bar is exactly AAA-REN-6.

### 5.4 What "done" means per phase

| Phase | Done means (demonstrated on Windows and Linux) | Gates (AAA- prefix omitted) |
|---|---|---|
| 0 Foundations | CI green on every toolchain; `helios-schemac` emits C++/Go/Luau/SQL; golden images pass on the Vulkan and Null backends; a headless cell ticks a dilatable zone clock; `helios-backend` serves login without Docker; the launcher fetches a chunked manifest | PLT-1/2, REN-7 |
| 1 First Light | A player logs in via the launcher, undocks a Kestrel, descends to Harrow with no loading screen, lands, walks into Saltmarch and shoots Hollow drones with prediction; 50 players; persistence; the editor places entities, edits records and runs PIE | REN-5, ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1, SRV Ph1 |
| 2 Alpha Sandbox | Osk resources → crafting → market on the ledger; quests and dialogue; lairs; chat and guilds; 500 players per zone; CDC patching; P0 tools complete | SRV Ph2, ITR-2/4/6, STB-4, SEC-2/3/6, CNT-7, TOOL-2/6 |
| 3 Beta Scale | Multi-cell orbit with handoff < 100 ms; 5k CCU; strike; phased story; settlements; Vane; Mule; live co-editing; the designer day passes | SRV Ph3, REN-4, ITR-7, STB-5, CNT-2/4/5, SEC-5/7, TOOL-3/5 |
| 4 Launch Quality | **The AAA bar:** all BENCH scenes within budget on MIN and REF; 2,000-ship battle; raid; seasons; 72 h soak at 50k CCU; clean penetration test; all 30 tools | every criterion with Ph ≤ 4 |
| 5 Ambition | Five systems with seamless Lattice travel; dynamic meshing; 100k-bot single shard; sandboxed player scripting; RT on SHOWCASE | SRV Ph5, REN-6 (RT) |

---

## 6. Glossary

- **Shard:** a regional deployment of gateways, cells and a shard DB presenting one world. Account services are global. From Phase 5 a shard may be the whole world.
- **Zone:** a gameplay space (system space, planet surface, interior, instance) with its own static data, reference frame, tick profile and **zone clock**. The zone clock is dilatable (TiDi) with a 10% floor, and all timed gameplay runs on it.
- **Cell:** a spatial region of a zone, owned by exactly one **cell server** (the headless C++ runtime) at a time. In v0, zone = cell.
- **Gateway:** the C++ edge that terminates encrypted UDP and routes traffic to cells. Clients never talk to cells directly.
- **Orchestrator:** the Go service that assigns cells, issues leases and epochs, creates instances, and sets split/merge and TiDi policy.
- **Authority Group (AG):** a root entity plus its attached hierarchy (e.g. a ship and everything aboard). It is the unit of authority, streaming and persistence.
- **Authority epoch:** a 64-bit fencing counter per AG, bumped on every **handoff** (authority transfer between cells). Writes carrying an older epoch are rejected.
- **Ghost:** a read-only copy of a neighbouring cell's entity, kept within the **interest margin**. The margin is ≥ max(interaction range, v_max × handoff latency).
- **Reference Frame:** the space a transform is relative to: galaxy → system → body (rotating) → grid → interior. Positions are frame-local f64 **WorldPos**.
- **Grid:** a frame with its own Jolt `PhysicsSystem`, such as a planet surface, ship interior or open space.
- **Reparent:** an atomic change of frame that preserves world position and velocity.
- **Object Container:** an authored, nested, streamable unit that is edited, streamed, persisted and hot-reloaded as one.
- **Record:** a typed static-data entry defined by the `.hschema` schema, with a stable hash ID and text source. It may inherit from a **record template**.
- **ECS archetype:** the storage for one exact component set. It never means a content template.
- **Reference archetype:** one of the five target game classes (a)–(e) in §2. It is unrelated to ECS archetypes and record templates.
- **Gameplay kernel:** the shared attributes, effects, tags, abilities and cues.
- **Edit Instance / Play Instance:** the edit instance is the only instance of an area that persists authored changes. Play instances run a pinned **Content Version**, an immutable, hash-identified published snapshot.
- **Instance / Layer / Phase:**
  - an **instance** is an extra copy of a zone;
  - a **layer** is a capacity copy;
  - a **phase** is a visibility filter driven by player or party state.
- **Activity:** bounded, objective-driven content (a mission, strike, raid or public event) whose progress lives in a durable backend service rather than in the cell.
- **Ledger:** the append-only, double-entry item and currency service. It is the only path for **value-bearing** state, meaning anything whose duplication or loss has economic meaning.
- **Tick profile:** a zone's simulation and snapshot rates, from 1 to 60 Hz.
- **Foundation layer:** the forkable starter gameplay that ships with the engine.
- **BENCH-n / AAA Scorecard:** the standard measured scenes, and the full set of AAA-* criteria.

---

## 7. Traceability

**Citation keys:**
- `R0n-P0-k` is requirement k of report R0n. R01–R05 and R07 number requirements continuously across priorities.
- `R06-ENG-nn`, `R08-ED-Px-nn` and `R08-Tnn` are the reports' own IDs.
- `R09-GP-*` and `R09-RD-*` refer to R09's gameplay and renderer lists.

**Requirements addressed here:**
- **R01:** P0-1, P0-2, P0-3, P0-4, P0-8; P1-14, P1-15, P1-17; P2-19, P2-22.
- **R02:** P0-1, P0-5 (with the patent note), P0-7, P0-8; P1-9…P1-15; P2-16…P2-18.
- **R03:** P0-2, P0-3, P0-5, P0-6, P0-8…P0-11; P1-1, P1-2, P1-4, P1-8, P1-9, P1-12; P2-1 (deferred to Phase 5).
- **R04:** P0-1…P0-3, P0-5, P0-7, P0-8; P1-11, P1-13…P1-16; P2-19, P2-23, P2-24.
- **R05:** P0-3…P0-5, P0-8…P0-11; P1-14…P1-16, P1-20; P2-25; the §6 tooling targets.
- **R06:** ENG-02, ENG-07, ENG-17, ENG-27.
- **R07:** the §8.3 scale table (AAA-SRV); §3.7 tick profiles; §8.4 failure handling (STB-5, SRV-12); P0-10, P1-13, P1-14, P2-21…P2-23.
- **R08:** §3 T01–T30 (AAA-TOOL); ED-P0-01 (SEC-4), ED-P0-09 (ITR-3), ED-P2-01 (ITR-7), ED-P2-08.
- **R09:** GP-P0-1…12 (matrix rows W01, M01–M08, G01, G05, G06, G08, G10, G13, G14, G16, S06); RD-P0-1…10 and the RD-P1/P2 lists (AAA-REN-6).
