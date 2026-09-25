# 01 — Vision, Target Games, the AAA Bar, Scope and Non-Goals

*Helios master plan, section 01. This section follows `00-decisions.md` (ADR-001…016) and the phase vocabulary in `README.md`. Research citations use the key format described in §7.*

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
| **Backend** | Go services (auth, orchestrator, ledger, market, chat, social, persistence, telemetry, GM, patch); world scripts, the studio's sandboxed Luau extension surface (05 §1.23); one `helios-backend` binary for local dev (008) |

**North star:** a studio of 30–150 people on Windows workstations builds, ships and operates an original MMO in the class of Star Wars Galaxies (SWG), EVE Online, Destiny, Star Citizen or SWTOR, or a hybrid of them in one shard, by writing records, Luau and domain graphs, never by modifying engine or backend source. Native C++ game modules remain optional for hot paths. Two mechanisms keep this true beyond content: a studio adds its own record, component and view-model types as dynamic schema packages with no compiler (02 §3.8, RT-21), and its own cross-zone backend systems as world scripts (05 §1.23, BE-A20).

**The hybrid is the point.** Dust 514 bolted a different engine's FPS onto EVE's persistence and stayed "shallow and one-directional" (R01 §9); SWTOR shipped space as an on-rails minigame (R03 §7.9). Helios runs fleet space, seamless planets, shooter activities, a sandbox economy and cinematic story on one simulation, network and backend.

### 1.2 Who uses it

Studio teams on **Windows 10/11 x64** (ADR-001): programmers (VS 2026, or VS 2022 17.14 as the floor, ADR-001a; Luau debugger T10, Tracy T26), systems designers (T08, T09, T11), world builders (T01, T04–T07), narrative/localization (T12–T14, T25), animation/VFX/audio/UI artists (T15–T20), live-ops/GM (T27) and SREs. Linux workstations get the same builds; production servers run Linux, and every server also runs natively on Windows.

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
- *Proof:* Osk crafting (Ph2), Saltmarch settlements built through the housing and city rules (Ph3, BENCH-5 and 06 GP-16).

**(b) EVE-style single-shard economy and fleet warfare.**
- *Experiences:* one universe; command flight (approach, orbit, warp); Dogma fitting with stacking penalties; regional order books; multi-day industry; corporations and sovereignty with reinforcement timers; battles of thousands under time dilation (R01 §3–5).
- *Numbers:* 1 Hz ticks; ~8,000 km grids; TiDi floor 10% with module response < 1 s; ~65k peak CCU. *Helios form:* one 2 Hz fleet-battle profile, whose response is bounded in ticks of game time rather than wall seconds (§3.4, AAA-SRV-10); 50k CCU per shard at the Phase 4 bar (AAA-SRV-3).
- *Critical:* M01, M07, G01, G04, G06, G08, S04–S06, S09, W06, R03, R06.
- *Lessons:* no script on the hot path (R01-P0-4); live zone migration (R01-P0-1); character "brain" travels (R01-P0-2); timers pre-provision capacity (R01-P1-14).
- *Proof:* Osk market (Ph2), Vane territory and reinforcement timers (Ph3, 06 GP-17), sovereignty and the battle (Ph4).

**(c) Destiny-style action looter-shooter.**
- *Experiences:* console-shooter feel in a shared world; matchmade 3-player strikes with difficulty tiers and modifiers; 6-player raids with checkpoints and weekly lockouts; Crucible 6v6 PvP with skill-based matchmaking; opt-in PvP risk zones with extraction; public events; socket/plug loot; per-weapon, per-device aim assist; social hubs; seasons (R05 §3–5).
- *Numbers:* latency-free own avatar; ~10 Hz activity sync; ≥ 500 ms hitbox history; 30–60 Hz instances (R07 §3.7).
- *Critical:* M05, M06, G05, G10, G12, G17, G19, R07, S08.
- *Lessons:* prediction, not client authority; activity progress survives a cell crash (R05-P0-5); loot RNG tested statistically in CI (R05 §5.1).
- *Proof:* Hollow Vault strike (Ph3), a matchmade strike and a rated 6v6 match in `starter-shooter` (Ph3, 06 GP-14), Lattice Heart raid (Ph4).

**(d) Star Citizen-style seamless universe.**
- *Experiences:* hangar on a rotating planet → multi-crew ship → orbit → quantum travel → boarding, with no loading screens; walking in moving ships; physical components with power and heat; persistent entities; server meshing (R04 §1).
- *Numbers:* ~4×10¹⁰ m systems (float32 step ≈ 2.4 km); ~500-player static-mesh shards; authority transfer < 2 ticks (R04 §4.3).
- *Shipped state (R04 §4.1–4.2):* the replication layer (Replicant, Atlas, Scribe and Gateway) went live with Persistent Entity Streaming in Alpha 3.18 (2023), and static server meshing on it in Alpha 4.0 (December 2024). A game-server crash therefore no longer disconnects players: a replacement server resumes from state the replication layer holds. Only dynamic meshing is still in progress. *Helios form:* static multi-cell zones (S02, Ph3) plus the **replicant tier** (Ph4, ADR-007, 04 §6.4) match that crash recovery at the Phase 4 bar, with no disconnect, ≤ 1 s of non-value state lost (AAA-SRV-9) and a ≤ 10 s hitch (AAA-SRV-12). Client replication stays in cells until Phase 5, when the gateway replication layer and dynamic meshing (S03) arrive and the hitch falls to ≤ 1 s.
- *Critical:* W01–W03, W05, M02, M09, G16, S02, S03, S07, R02, R06.
- *Lessons:* seams from day one; a cleanup policy for every persistent record template; gameplay written for split authority; data-driven flight envelopes (R04 §6.1, §9).
- *Proof:* BENCH-2 descent (Ph1), Mule multi-crew (Ph3), cell kills in multi-cell Harrow orbit and single-cell Harrow High with ≤ 1 s of state lost (Ph4, NS-4.6), seamless Lattice travel (Ph5).

**(e) SWTOR-style story-driven themepark.**
- *Experiences:* fully voiced branching stories; dialogue-wheel cinematics; **group conversations** (each player picks, a roll picks whose line plays, effects apply per player); companions; personal story phases; flashpoints (4) and operations (8/16) with story, veteran and master tiers, level sync and bolster, solo modes and a group finder; 8v8 objective warzones; capacity-driven planet instances (R03 §4–5).
- *Numbers:* 200k+ VO lines, 558k+ strings, 3 launch languages; story instance start < 2 s (R03-P0-6).
- *Critical:* G10–G12, G19, W07, R04, S08.
- *Lessons:* UI independent of the graphics API (SWTOR's DX12 port is blocked on UI); story coverage tooling; global identity; never build the game on an unfinished engine fork (R03 §3, §7).
- *Proof:* "Signal from Saltmarch": runtime (Ph2), group conversation (Ph3), auto-staged and voiced (Ph4); a flashpoint with tiers, level sync and a solo mode in `starter-story` (Ph4).

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
| M01 | Command flight in 2 Hz fleet-battle zones, thousands of ships | – | ● | – | ○ | – | 06 | 3 |
| M02 | 6-DoF Newtonian flight, flight assist, envelopes | ◐ | – | ○ | ● | ○ | 06 | 1 |
| M03 | Locomotion, mounts, vehicles | ● | ○ | ● | ● | ● | 02, 06 | 1 (vehicles, mounts: 2) |
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
| G12 | Activities: fireteams, encounters, checkpoints, lockouts, difficulty tiers, level sync | ○ | ○ | ● | ○ | ● | 04, 05, 06 | 3 |
| G13 | AI: BT + utility + perception, AI LOD, spawns | ● | ◐ | ● | ◐ | ● | 06 | 1 |
| G14 | Death pipeline: wrecks, clones, insurance, killmails | ◐ | ● | ◐ | ● | ◐ | 06 | 2 |
| G15 | Character creation: species, morphs, fit | ● | ◐ | ◐ | ◐ | ● | 02, 03, 07 | 4 |
| G16 | Modular ship assembly, fitting budgets, liveries | ● | ● | ○ | ● | ○ | 06, 07 | 3 |
| G17 | Live-ops calendar, seasons, data hotfixes | ○ | ◐ | ● | ◐ | ◐ | 05 | 4 |
| G18 | Background economy/world simulation | ○ | ◐ | – | ◐ | – | 05 | 5 |
| G19 | PvP match modes and rating, matchmaking, group finder, leaderboards | ○ | ○ | ● | ◐ | ● | 05, 06 | 3 |
| S01 | Gateway + cells, per-zone tick 1–60 Hz | ● | ● | ● | ● | ● | 04 | 1 |
| S02 | Static multi-cell zones, ghosts, fenced handoff; replicant crash recovery | ◐ | ◐ | ○ | ● | ○ | 04 | 3 (replicants: 4) |
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

"AAA" means **every criterion tagged Ph ≤ 4 passes**. The criteria form the **AAA Scorecard**; section 09's build loop runs it as its test suite and always works the highest-priority failing criterion. **Phase N is complete when every criterion with Ph ≤ N passes three consecutive nightly runs on Windows and Linux.** Criteria that cannot run nightly use 09 §5.6's evidence classes and keep their thresholds: **W** (long-running soaks: two consecutive scheduled passes) and **M** (human or external judgement: a signed record in `docs/evidence/` within the exit window). Criteria marked **(M)** below are human-judged: the designer day, the look-and-feel panel, the content-team zone and the pentest.

The scorecard measures two things. Most criteria test **speed, scale and completeness**. Two M-class criteria test whether the result is **at the reference class's level**: AAA-REN-8, where an external panel compares the look and feel with current titles, and AAA-TOOL-10, where a content team with no engine engineers builds a zone with the shipped tools. Passing every number while failing either of these still fails the bar.

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
| BENCH-3 | Vane engagement: the client view of the AAA-SRV-10 / 04 NS-4.2 battle | 2,000 ships (20 capitals, 1 % as in SRV-10, the *Bastion* among them), 2,000 brackets, 400k active particles (REF; MIN's significance pass keeps its 256k pool, 03 §6.1). Phase 5: 3,000 ships and 30 capitals with SRV-10 (09 M5) |
| BENCH-4 | Hollow Vault encounter | 6 players, 80 AI, 60 Hz cell |
| BENCH-5 | Saltmarch settlement, placed through the housing and city rules (the snapshot of 06 GP-16 (e); 07 ED-10 co-edits an authored town at this scale) | 300 structures, 3,000 decor items, 150 avatars |
| BENCH-6 | Mule boarding | EVA → airlock → interior at 300 m/s under thrust; from Ph3 the Mule crosses a cell boundary of multi-cell Harrow orbit while an observer 100 m from the boundary watches it cross, with no pop-in and far-side entities at the near-tier rate (04 NS-3.10) |

**BENCH-3 and the battle gate are one battle.** Each ID means one thing:
- **BENCH-3** measures the **client**: frame rate, brackets, VFX and memory.
- **AAA-SRV-10**, run as 04's **NS-4.2**, measures the **server** in the same 2,000-ship, 20-capital engagement: tick, module response and downstream.
- 09's M4 "2,000-ship battle at 10 % TiDi (BENCH-3 ≥ 45 fps)" is therefore literal. The render gate covers the whole battle the Phase 4 demo claims, not a slice of it.

How it is measured:
- **Nightly (from Phase 2).** BENCH-3 replays a recorded gateway downstream capture of the battle through the real client netcode stack, so the nightly run needs no server rack. The capture is stamped in zone ticks, so it can be recorded at any `d`, including from the 1k → 2k-ship bot battles that run from Phase 2 (09 K28) before the server meets NS-4.2. It plays back at `d` = 1, which is the worst case for effects per wall second.
- **Camera.** A scripted, deterministic path: the cockpit of a frigate in the *Bastion*'s fleet, then a chase camera through the volley exchange, then a tactical zoom-out that frames the whole engagement.
- **Phase 4 exit.** BENCH-3 is also measured live, on a REF client joined to the NS-4.2 run, and must meet the same thresholds.
- **No load staging.** The client benchmark always runs the full count. Only the server gate is staged by phase (SRV-10: 500 → 2,000 → 3,000 ships).
- **Server risk.** K28 (09 §6) is triggered by NS-4.2's half-load server run, never by BENCH-3.

### 3.3 Rendering, client performance, look and feel

| ID | Criterion | Ph |
|---|---|---|
| AAA-REN-1 | REF at 1440p High: BENCH-1/2/4/5 average ≥ 60 fps with p99 ≤ 20 ms; BENCH-3 (the full 2,000-ship battle, §3.2) average ≥ 45 fps with p99 ≤ 33 ms. Phase 1 slice: BENCH-2 ≥ 60 fps at 1080p Medium | 4 (1) |
| AAA-REN-2 | MIN at 1080p Low, upscaled: BENCH-1/2/4 ≥ 60 fps with p99 ≤ 33 ms; BENCH-3/5 ≥ 30 fps | 4 |
| AAA-REN-3 | Performance mode: REF ≥ 120 fps in BENCH-4 | 4 |
| AAA-REN-4 | Zero frames > 50 ms caused by PSO creation in a 30-minute scripted run | 3 |
| AAA-REN-5 | Render and simulation test at 10¹³ m shows no jitter (ADR-005); BENCH-2 has no loading screen and no frame > 50 ms | 1 |
| AAA-REN-6 | Render feature set complete per phase (see the note below): **Ph1** clustered forward+ PBR with CSM; starfield and nebula IBL; HDR, histogram exposure, bloom and tonemap; GPU particles (shields, plumes); **CDLOD cube-sphere planets with GPU terrain tiles (Harrow, 1,500 km); Hillaire atmosphere from orbit to ground**; 2D cloud shell. **Ph3** Earth-size planets (6,400 km radius, AAA-CNT-1) with oceans; froxel fog; DDGI (REF) with relit assembly-time probes (MIN); vegetation (foliage LOD/impostor bands, wind, interaction) and weather effects (precipitation, wetness, lightning). **Ph4** visibility buffer, virtual shadow maps, volumetric clouds, TAA + `IUpscaler`, HDR10. **Ph5** RT effects on SHOWCASE | 1–5 |
| AAA-REN-7 | Every render feature has a lavapipe golden from the commit that ships it, whatever its REN-6 phase; each renders twice bit-identically; a failing golden blocks the merge | 0 |
| AAA-REN-8 | **Look and feel (M).** A blinded external panel scores fixed captures and a hands-on feel session against current reference titles on the §3.3.1 rubric (1–5; 3 = on par with the reference median). Pass at Ph1: every applicable item's median ≥ 2.5. Pass at Ph3: every applicable item's median ≥ 3.0, except L5 ≥ 2.5. Pass at Ph4: every item's median ≥ 3.0, the mean of the item medians ≥ 3.3, and ≥ 80 % of the panel says "yes, this presentation could ship in a current AAA sci-fi game". Every failing item, and every individual score ≤ 2, is routed to a WP | 1, 3, 4 |

*REN-6 and REN-7.* REN-7 is **coverage**: anything that ships is golden-tested on every commit from the day it lands (03 RC-1). REN-6 is **completeness**: by its phase, a feature must meet its 03 specification, stay inside its 03 §8.1 GPU budget in the BENCH scene that uses it, and pass the nightly real-GPU goldens (NVIDIA, AMD, Intel on Windows and Linux; 03 §8.4). Planets are therefore gated in Phase 1, where BENCH-2 needs them; only Earth-size radii wait for Phase 3. Volumetric clouds, likewise, land with lavapipe goldens in Phase 3 and gate completeness in Phase 4 (03 RC-11).

*REN-8.* REN-1…7 prove speed, completeness and the absence of regressions. None of them proves that Helios looks and feels like the games it is measured against. Without REN-8, a phase could exit at 9/10 with correct, fast but mid-tier visuals and floaty controls.

#### 3.3.1 AAA-REN-8: the look-and-feel review

**Panel.**
- **Size.** ≥ 3 reviewers at the Ph1 exit; ≥ 5 at the Ph3 and Ph4 exits.
- **Contracting.** Each panel is contracted for its exit under H6 (09 §4.3) through a vendor. The vendor, not the build loop, recruits, facilitates and records.
- **Mix.** ≥ 1 art director or senior lighting/environment artist and ≥ 1 technical artist or rendering engineer. The rest are designers: ≥ 1 at Ph1; ≥ 2 at Ph3 and Ph4, one with a shooter credit and one with a flight or space-game credit.
- **Eligibility.** Every reviewer has shipped a AAA action, shooter or space title within the last 6 years. Nobody has worked on Helios or *Cinder Reach*.
- **Turnover.** At most 2 of 5 reviewers carry over from the previous exit, so each exit has fresh eyes.

**Materials.** Frozen per exit in `docs/evidence/ren8-ph<N>/`, with the build hash and a hash for every capture.
- **Captures.** They come from a nightly build that passes the phase's other REN gates: the REN-1 slice and REN-5/6 at Ph1, REN-4/6 at Ph3, and REN-1/2/3/6 at Ph4. The panel therefore judges what ships at that frame rate. No offline supersampling, photo mode or hand-placed lights are allowed.
  - Per scene: 4 stills from fixed camera bookmarks and a 60 s clip along the BENCH camera script, on REF at 1440p High.
  - One MIN 1080p Low clip per scene, because the look must survive scaling.
  - Clips are encoded at ≥ 80 Mbit/s, so compression does not dominate.
- **Scenes.**
  - Ph1: BENCH-2 and the BENCH-1 concourse at its Phase 1 population.
  - Ph3: BENCH-1, 2, 4 and 5.
  - Ph4: BENCH-1…5, including the full BENCH-3 battle.
- **Reference set.** 2–3 titles per scene, from a list frozen at the previous exit. Every title was released, or visually overhauled, within 6 years. Examples: Destiny 2 (hub, firefight), Star Citizen (descent, interiors, dogfight), EVE Online (fleet battle), Starfield and Elite Dangerous: Odyssey (planets, stations).
  - The vendor records the footage from retail copies on the same REF machine at the closest matching settings. Where a scene cannot be staged, the publisher's public footage is used.
  - Reference footage stays with the vendor and never enters the repository or the asset DB (§5.2).

**Blinding.**
- Clips play in a randomized, interleaved order. HUDs, logos and title-identifying UI are masked.
- The Helios material carries a code; neither the build nor the team is named.
- Reviewers score independently before any discussion.
- Recognising a famous reference title is expected. The point is that no reviewer scores the candidate knowing that it is the candidate, or who made it.

**Feel session.**
- **Set-up.** Each reviewer plays ≥ 60 min on REF at 1440p, with mouse and keyboard and with a gamepad. Low-latency mode is on, and the netem profile is 80 ms RTT with 1 % loss.
- **Content.**
  - Ph1: the Saltmarch drone firefight and a *Kestrel* dogfight.
  - Ph3: adds the BENCH-4 *Hollow Vault* encounter, played with two other players.
  - Ph4: adds a *Lattice Heart* raid segment and a small-fleet engagement in Vane.
- **Anchors.** In the same session, each reviewer plays two reference titles for 20 min each on the same monitor and devices.
- **Latency data.** The build's measured CL-6 latencies (08 §1.3a) are shown only after scoring.

**Rubric.** Each item is scored 1–5 per scene:
- **1:** a generation behind the reference titles.
- **3:** on par with the median reference title for that scene.
- **5:** ahead of every reference title.

The rubric is published at `docs/evidence/ren8-rubric.md` and frozen one phase ahead, so the bar cannot move during a phase.

| # | Item | Judged in | From |
|---|---|---|---|
| L1 | Lighting and shadows: plausible GI, contact shadowing, exposure adaptation between interiors and space | All scenes | Ph1 |
| L2 | Materials and surface detail: PBR plausibility, texel density, wear, decals, liveries | All scenes | Ph1 |
| L3 | Sky, atmosphere and space: scattering from orbit to ground, clouds, nebula and starfield scale | BENCH-2, 3 | Ph1 |
| L4 | VFX: weapons, shields, plumes, impacts and explosions, lit and integrated with the scene | BENCH-3, 4; feel session | Ph1 |
| L5 | Characters and animation: skin, cloth, locomotion, crowds; facial and lip-sync from Ph4 | BENCH-1, 4, 5 | Ph3 |
| L6 | Scale, density and streaming: sense of size, invisible LOD, impostor and HLOD transitions, no pop-in | BENCH-2, 3, 5 | Ph1 |
| L7 | Image stability: aliasing, shimmer, TAA ghosting and upscaler artefacts, on REF and on MIN | All scenes | Ph1 |
| L8 | Art-direction coherence and HUD legibility | All scenes | Ph1 |
| F1 | Responsiveness: felt input-to-motion, camera and fire latency | Feel session | Ph1 |
| F2 | Gunplay: hit feedback, readable recoil and spread, gamepad aim assist | Feel session | Ph1 |
| F3 | Movement and flight: on-foot traversal, 6-DoF flight with assist, cockpit ↔ on-foot transitions | Feel session | Ph1 |
| F4 | Feedback: hit markers, impacts, damage states, audio mix | Feel session | Ph1 |
| F5 | Network feel at 80 ms RTT: no visible corrections to one's own avatar or ship; hits register as seen | Feel session | Ph1 |
| F6 | Encounter and AI readability: enemies telegraph, flank and react plausibly | Feel session | Ph3 |

**Routing and re-review.**
- **Defects.** Every item whose median misses its threshold becomes a `ren8` defect, carrying the reviewers' timestamped notes. So does every individual score ≤ 2, even when the median passes.
- **Owners.** The Director routes each defect to its owning WP:

  | Items | Owning WPs |
  |---|---|
  | L1–L4, L6, L7 | The phase's render WPs (WP-1.7–1.9, WP-3.5, WP-4.1) |
  | L5 | WP-4.6, plus 02's animation lane |
  | L2 and L8, when the gap is art rather than engine | The phase's content WP (WP-1.22, WP-3.9, WP-4.10) and H5 |
  | F1 | The client WP (WP-1.20, WP-3.8, WP-4.9) |
  | F2–F4, F6 | The gameplay WP (WP-1.16, WP-3.6, WP-4.5) |
  | F5 | The netcode WP (WP-1.11, WP-3.1, WP-4.3) |

- **Priority.** A failed REN-8 is a failing criterion like any other, and the loop works it by priority.
- **Re-review.** Only the failed items are re-reviewed, inside the exit window. The re-review panel has ≥ 3 reviewers, ≥ 2 of them from the original panel, and uses fresh captures.
- **Content tier.** The Lean content tier (09 §4.3.3) lowers no threshold. If art rather than engine features fails L2 or L8, the fix is content, and K23 applies.

**Between exits.**
- The capture set renders nightly. Because it is deterministic, any difference is real.
- The Content lead scores the rubric monthly with H6 playtesters. These internal scores are trend signals only and never count as evidence.

**Cost.** Inside H6 (09 §4.3):

| Exit | Panel | Cost |
|---|---|---|
| Ph1 | 3 reviewers × 1.5 days | ≈ $4–8k |
| Ph3 | 5 reviewers × 2 days, a vendor facilitator and one partial re-review | ≈ $10–20k |
| Ph4 | As Ph3 | ≈ $12–24k |

### 3.4 Server scale

Targets come from R07 §8.3. R07's P0 / P1 / P2 columns map to Phases 2–3 / 3–4 / 5; where R07 gives a range, the scorecard takes its upper end as the pass threshold. They are validated by headless bot swarms on SERVER hardware.

**Tick profiles.** The engine accepts any rate from 1 to 60 Hz (ADR-007), but the scorecard measures exactly these four. A zone's profile is fixed in its zone record; budgets are in 04 §3.4.

| Profile | Sim tick | Movement and physics | Snapshot rate | Used by |
|---|---|---|---|---|
| **Fleet battle** | **2 Hz** × `d` | Every ship is `CommandKinematic` with a 64-bit fixed-point integrator (04 §5.4, 06 §8.1); no 6-DoF prediction | ≤ 2 Hz; commands and vitals, not transforms | Territory (Vane) systems; M01, S04, S05; AAA-SRV-10 |
| Phase / housing | 10 Hz | Jolt per grid | ≤ 10 Hz | Small phased and housing instances |
| Open world and space | 20 Hz | Jolt per grid; space substeps at 60 Hz | ≤ 20 Hz | Hubs, planets, open systems; AAA-SRV-1/2 |
| Activity | 30–60 Hz | Jolt; lag-compensated hits | ≤ tick | Strikes, raids, PvP instances (BENCH-4) |

Tick p99 must stay at or below 80% of the tick period at `d` = 1 in every run except the AAA-SRV-10 battle and the overload tests. The TiDi floor is `d` = 0.1: one fleet-battle tick then lasts 5 s of wall time.

| Metric | Ph1 | Ph2 | Ph3 | Ph4 | Ph5 |
|---|---|---|---|---|---|
| AAA-SRV-1 Players per cell (20 Hz) | 50 | 500 | 500 | 500 | 1,000 |
| AAA-SRV-2 Players per zone | 50 | 500 | 2,000 | 5,000 | 10,000+ |
| AAA-SRV-3 CCU per shard (pass threshold) | 50 | 2,000 | 5,000 | **50,000** | 100,000 |
| AAA-SRV-4 Replicated entities per cell | 5k | 20k | 50k | 50k | 100k |
| AAA-SRV-5 Zone transition / handoff p99 | ≤ 3 s | ≤ 3 s | < 100 ms | < 100 ms | < 50 ms |
| AAA-SRV-6 Client downstream steady/battle (kbit/s) | 256/512 | 256/512 | 256/512 | 256/512 | 256/1,000 |
| AAA-SRV-7 Login admission; login p95 | 5/s; 5 s | 50/s; 3 s | 50/s; 3 s | 200/s; 3 s | 500/s; 3 s |
| AAA-SRV-8 Ledger tx/s per shard | — | 2k | 2k | 10k | 50k |
| AAA-SRV-9 Non-value state lost on cell crash (persistent world zones / activity, phase and housing instances from Ph4) | 60 s | 60 s | 30 s | **1 s** / 30 s | 1 s / 10 s |
| AAA-SRV-10 Ships in one fleet-battle zone (2 Hz) with `d` ≥ 0.1 and module response p95 ≤ 1 tick of game time + RTT, measured at `d` = 0.1 | — | — | 500 | 2,000 | 3,000 |
| AAA-SRV-11 Story/strike instance start | — | — | < 2 s | < 2 s | < 2 s |
| AAA-SRV-12 Cell crash impact | reconnect | reconnect | ≤ 10 s hitch | ≤ 10 s hitch | ≤ 1 s hitch |

**Measurement rules.**
- **CCU (SRV-3)** counts authenticated bot sessions that are in-world at the same moment. The run holds the count for ≥ 1 h with every other metric in its column in budget: login p95 (SRV-7), tick p99 in every zone and ledger throughput (SRV-8). The Phase 4 run at 50,000 CCU is also the 72 h AAA-STB-4 soak. The Ph5 figure is the 100k-bot shard of §5.4.
- **State loss (SRV-9)** is the rollback a cell crash causes, per AG: the wall time between the last authoritative state bots observed before the kill and the state the standby resumes from, p99 over ≥ 20 `kill -9`s under load at `d` = 1 (under TiDi the bound scales with 1/`d`, like every wall-clock figure). From Phase 4 it is measured separately in persistent world zones (hubs, stations, open space and fleet battles, single-cell or multi-cell), which recover from the replicant tier by default (04 §6.4; NS-4.6 in 4-cell Harrow orbit and single-cell Harrow High), and in activity, phase and housing instances, which recover from checkpoints unless their zone record opts in. Value is never rolled back (SEC-2).
- **Battle (SRV-10)** runs bot fleets on a reinforced host that `PreProvision` places, within the 04 §3.4 fleet-battle budget of 16–32 SERVER-class cores and 16 GB. 1% of the ships are capitals (5 at 500 ships, 20 at 2,000). Every ship is fitted, and all ships fire, lock and move under commands for 30 min of wall time:
  - *Capacity.* The zone clock never needs `d` < 0.1: at the floor, tick p99 ≤ 90% of the dilated interval (≤ 4.5 s), so no input backlog builds.
  - *Module response.* This is the zone time from an activation reaching the cell to the snapshot tick that confirms the module cycling, plus the client's RTT. It is measured at `d` = 0.1. Pass: p95 ≤ 1 tick (500 ms of game time) + RTT, and no activation waits more than 2 ticks. Because every timer runs in ticks (04 §3.2), players get the same game-time response at any `d`.
  - *Downstream.* Per-client downstream p95 ≤ 512 kbit/s (SRV-6).
  - *Consistency.* Every client–server drift that the command-replication state hash catches (every 2 s, 04 §5.4) is repaired by the state fallback within the next hash period, and none persists.
  - *Client side.* One REF client watching this battle is BENCH-3 (§3.2), gated by AAA-REN-1/2. It is the same ship and capital count, not a slice of it.
  - *Half-load run (early warning, not a gate).* From Phase 2 a nightly 1,000-ship run (half of NS-4.2) feeds 09's K28 trigger. K28 fires if the run's tick p99 exceeds 2.25 s at `d` = 0.1, which is half of the 4.5 s ceiling. It also fires if the tick cost grows more than 2.5× from 500 to 1,000 ships, because that super-linear growth would not fit the budget at 2,000.
- **Why game time, not wall time.** At the floor one tick lasts 10 × `tick_dt` of wall time: 5 s at 2 Hz, and still 1 s at 10 Hz. A wall-clock "< 1 s" is therefore out of reach for any battle profile cheap enough to hold 2,000 ships. EVE's "< 1 s" (R01 §3.4) contrasts with the 20–600 s backlogs seen before TiDi. What TiDi guarantees, and what Helios gates, is that no command waits more than a tick of game time. For reference, the client-observed wall response is ≤ 2 × `tick_dt` / `d` + RTT: ≤ 1 s + RTT at `d` = 1 and ≤ 10 s + RTT at the floor. Within one frame the client marks the module `pending` through `PredictedIntent` (08 §1.11) and shows the TiDi indicator, so a slow tick never looks like a lost click.

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
| AAA-ITR-8 | Canaried data/Luau hotfix to live, no client patch or downtime, ≤ 15 min; no client disconnected or re-placed and no zone duplicated by version, with a same-epoch client build staged (05 §1.14.1, BE-A14 Ph4) | 4 |

### 3.6 Stability

| ID | Criterion | Ph |
|---|---|---|
| AAA-STB-1 | Client crashes per 1,000 play-hours ≤ 10 / 3 / 1 (Ph2 / Ph3 / Ph4); all crashes produce symbolicated minidumps | 2–4 |
| AAA-STB-2 | Editor crashes ≤ 1 per 40 / 100 user-hours (Ph3 / Ph4); the crash journal loses at most one transaction (Ph2) | 2–4 |
| AAA-STB-3 | Cell crashes ≤ 1 per 1,000 / 5,000 cell-hours (Ph3 / Ph4) | 3–4 |
| AAA-STB-4 | Soak for 24 h (Ph2) and 72 h at 50k CCU (Ph4): zero ledger violations, RSS growth ≤ 2%, tick p99 within budget | 2, 4 |
| AAA-STB-5 | Chaos test: `kill -9` of any cell, gateway or service during trades and handoffs creates or destroys no value; players resume within ≤ 10 s | 3 |
| AAA-STB-6 | Rolling zone restarts with N/N+1 protocol compatibility and no disconnects; with a same-epoch client build staged, no client re-placed and no zone duplicated by version; each zone moves by planned region migration with a **hitch p99 ≤ 1 s** and **0 state loss** (state hash equal on both sides, 0 lost transients, 0 conservation deltas); **gateway boxes** roll by make-before-break relocation with **0 linkdead sessions** and an input gap p99 ≤ 250 ms (04 §6.7, NS-3.11; 05 §1.14.1, §6.3.1, BE-A14 Ph4) | 4 |

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
| AAA-SEC-8 | **(M)** External penetration test with no open critical or high findings | 4 |

### 3.9 Tooling completeness (R08 §3 IDs)

| ID | Criterion | Ph |
|---|---|---|
| AAA-TOOL-1 | MVPs of T01, T04, T08, T10, T15 (import), T16 (instances), T24, T26–T29 | 1 |
| AAA-TOOL-2 | R08-ED-P0-01…16 met; MVPs of T11–T13, T23, T25 | 2 |
| AAA-TOOL-3 | T05–T07, T15, T17, T19, T21 and T30 (live edit instances) complete; 26/30 tools at "AAA prio" (T15's motion matching and facial items follow their Ph4 runtime, 07 §2) | 3 |
| AAA-TOOL-4 | All 30 tools at "AAA prio", including T13 auto-staging, T14, T22 and the T27 web admin | 4 |
| AAA-TOOL-5 | **Designer day (M):** a designer new to Helios adds a hull variant, a weapon with rolled perks, a 3-step quest with dialogue and a vendor in ≤ 1 day, with no engineer and no restart | 3 |
| AAA-TOOL-6 | Every tool writes through transactions; every record type is editable in T08; every asset class has T28 rules | 2 |
| AAA-TOOL-7 | **Documentation:** generated reference covers 100% of public Luau APIs, record types and fields, components, CVars and `helios-tool` commands, and every tool at MVP or better has a manual page (Ph1); every executable tutorial passes nightly on Windows and Linux (Ph2); 100% of the public C++ game-module and editor extension APIs (07 §1.10) and of the world-script API (the `world` Luau realm and the `worldscript` schema block, 05 §1.23), and one tutorial per starter template (Ph3). Pipeline in 09 §2.7.1 | 1–3 |
| AAA-TOOL-8 | **Project upgrades:** `helios-tool upgrade-project` moves *Cinder Reach* and the blank template from engine release N to N+1 with zero manual edits, after which validation, cook and the scripted smoke pass (Ph2); the same for every starter template, plus N−2 → N (Ph4). 09 §2.7.3 | 2, 4 |
| AAA-TOOL-9 | **Starter templates:** New Project from a template reaches PIE in ≤ 20 min on DEV and passes its scripted class proof with zero engine or backend source edits: sandbox, fleet/economy and looter-shooter (Ph3); seamless and story (Ph4). 09 §2.7.4 | 3–4 |
| AAA-TOOL-10 | **Content-team zone (M), plus UI provenance.** A contracted team of 3–5 content developers, with no engine engineer, builds a new playable zone in ≤ 10 working days. It uses only the released SDK, manuals and tutorials (§3.9.1). Pass: 0 blocking issues, 0 engineer interventions, 0 engine or backend source edits, ≤ 10 major non-blocking defects, and the zone passes T28, cook, publish and a 5-player playthrough. Separately, the nightly UI-provenance report shows ≥ 50 human `ui`-origin transactions for every transaction-writing tool, and it reports the share of *Cinder Reach* Ph3+ transactions that come from the UI | 4 |

#### 3.9.1 AAA-TOOL-10: dogfooding at production scale

**Why this criterion exists.** Humans otherwise validate "all the tools needed" only in narrow tasks:
- the one-day designer task (ED-9 / TOOL-5);
- the 2-hour co-edit (ED-10);
- scripted UI replays (ED-15).

Meanwhile, *Cinder Reach*'s levels, quests, dialogue and balance are mostly agent-authored, and they reach the project through JSONC, Luau automation and `helios-tool` rather than the editor UI. Without TOOL-10, no content team would ever build a zone with the 30 tools at production pace, so a missing workflow, a slow panel or an undocumented step could survive to launch.

**Team.**
- **Size and contracting.** 3–5 content developers, contracted under H6 (09 §4.3) through a co-development vendor.
- **Mix.** ≥ 1 level or environment designer, ≥ 1 quest or narrative designer, and ≥ 1 technical designer or technical artist. An environment artist or UI designer is optional.
- **Experience.** Everyone has shipped content in a commercial engine or an in-house MMO toolset.
- **Exclusions.** Nobody has worked on Helios. Nobody is an engine, backend or tools engineer.

**Set-up.**
- **Software.** The released binary SDK (09 §2.7.2) on DEV-class workstations: Windows 11 for everyone except one seat on Ubuntu 24.04 (PLT-3 parity).
- **Project.** A *Cinder Reach* checkout, the Foundation layer, one shared T30 edit instance, and the public docs site.
- **Assets.** The existing *Cinder Reach* kits, plus ≤ 50 new placeholder or CC0 assets that the team imports through T24 with provenance. No new H5 art.

**Brief.** Fixed and published on day 1, and sized so that an experienced team finishes in about 7 working days.

| Deliverable | Minimum | Tools exercised |
|---|---|---|
| Terrain | A new region of ≥ 16 km² on Harrow or an Osk moon: a new biome layer, ≥ 3 stamps, a road spline, an environment profile | T04, T05, T06, T18, T07 |
| Settlement | ≥ 40 structures and ≥ 400 decor items; one walkable interior with portal cells and an airlock; a landing pad | T01, T02, T03, T21, T28 |
| Spawns | ≥ 3 AI record templates (Foundation or new variants), ≥ 2 spawn regions with baked navmesh, one lair | T23, T08, T09 |
| Quest chain | 5 quests with ≥ 12 objectives; branching dialogue with ≥ 1 persistent consequence; rewards through the ledger (loot table or vendor); placeholder VO (TTS); strings in 2 languages (placeholder translation) | T12, T13, T25, T09, T20 |
| Strike | A 2-encounter strike with 2 difficulty tiers, a phase checkpoint, a weekly lockout on its final encounter and a `QueueDef`, iterated in Activity PIE (07 §1.6.3) and cleared through the queue in the play instance | T12, T23, T09, T01 |
| Cinematic | One cinematic of ≥ 60 s: ≥ 4 camera cuts, 2 characters, 1 VO line, a hand-off back to gameplay | T14, T15, T20 |
| UI screen and project types | One new data-bound screen (for example a settlement bulletin board) with gamepad navigation. It binds a **new project view-model**, and the zone declares at least one new record type and one new `ScriptState` component, all created in T08's schema editor as dynamic project types (02 §3.8) with no compiler | T19, T08, T10 |
| Ship variant | One new hull variant with its own loadout and livery, fittable and flyable | T21, T16, T08, T17 |
| Shipping it | Validation, cook, publish to a play instance | T28, T29, T30 |

**Zone acceptance.**
- **Validation and publish.** T28 reports 0 errors, and the zone cooks and publishes (≤ 5 min, ITR-7).
- **Playthrough.** Five H6 playtesters finish the chain in a play instance with no blocker bugs.
- **Performance and smoke.** The settlement stays inside the BENCH-5 frame budget on REF (03 §8.1), and the zone's `helios-bot` smoke passes.
- **Afterwards.** The zone joins *Cinder Reach*, and its captures join the nightly BENCH-5 set.

**Logging.**
- **The log.** A vendor observer records every issue in `docs/evidence/tool10-ph4.md`, with the tool ID and the time lost.
- **Blocking issue.** An issue that stops a deliverable for > 2 working hours with no documented workaround, loses work, or needs an engine, backend or tools change.
- **Major defect.** A non-blocking workflow defect that costs > 30 min.
- **Engineer intervention.** Any help beyond pointing to existing documentation.
- **Support channel.** The loop staffs it, but may answer only with links to existing docs. An answer that needs new documentation counts as a TOOL-7 defect.

**Pass and re-run.**
- **Pass.** All deliverables within ≤ 10 working days, with 0 blocking issues, 0 engineer interventions, 0 engine or backend source edits, and ≤ 10 major defects.
- **Defects.** Every logged issue, passing or not, becomes a defect routed to its tool's WP (07 §5.1 lanes).
- **Re-run.** A failed run is repeated after the fixes, by a fresh team in which nobody repeats.
- **Rehearsal.** At the Ph3 exit, the H6 newcomer designers from the TOOL-5 designer day run a reduced brief over 5 days: terrain, the settlement and 2 quests. The rehearsal routes defects the same way but does not gate Phase 3.

**UI-provenance report.**
- **Origin tags.** From Phase 3, the ToolsFramework stamps every journal transaction with an `origin` taken from its input path (07 §1.2); the caller cannot set it. Values:
  - `ui`: a human's input in the editor or the web tools;
  - `ui-scripted`: `helios-uitest`;
  - `luau`: editor automation;
  - `rpc`: remote control and DCC plug-ins;
  - `cli`: `helios-tool`;
  - `import`: asset import and live links;
  - `collab`: rebase and merge.

  Agent-driven edits therefore land as `luau`, `rpc`, `cli` or `ui-scripted`, never as `ui`.
- **The report.** A nightly `tool-provenance` report gives, for *Cinder Reach* content from Phase 3 on, the share of transactions and of changed properties by origin, per tool and per content type: records, placement, terrain, graphs, dialogue, UI documents and sequences.
- **Where it goes.** The share of human-UI transactions is shown in every auditor round (09 §5.7), so the scorers can see how much content went through the UI.
- **What gates.** Only the per-tool floor gates, at the Ph4 exit: ≥ 50 human `ui` transactions for each tool that writes transactions, drawn from Ph3+ *Cinder Reach* and the TOOL-10 zone. Tools that only read, such as T26 profiling, are exempt.
- **Before the gate.** A tool with no human `ui` transactions by the Ph3 midpoint is flagged "not dogfooded" and is added to the Ph3 rehearsal brief.

**Cost.** Inside H6 (09 §4.3):
- Ph4 run: 3–5 people × 10 days plus the vendor observer, ≈ $15–45k.
- Ph3 rehearsal: ≈ $3–6k.

### 3.10 Platform

| ID | Criterion | Ph |
|---|---|---|
| AAA-PLT-1 | Every commit builds and tests on both MSVC toolsets of ADR-001a (the primary VS 2026 v145, and the VS 2022 17.14 floor at MSVC 14.44, which also builds the SDK and every released Windows binary), clang-cl, GCC 13 and Clang 17, plus the MinGW cross-compile check: the 8-job PR matrix | 0 |
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
| **Foundation layer** (ships with the engine, forkable, like HeroEngine's "Clean Engine", R03 §2.5) | Luau, records and UI for common MMO plumbing: login/character select, HUD, inventory/market/chat widgets, default damage types and abilities, starter AI, quest templates, flight envelopes; the starter templates, one per reference class (09 §2.7.4, AAA-TOOL-9) |
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
  - Improbable's US 11,792,306 is reviewed before WP-4.3 builds the Phase 4 replicant tier (ADR-007), with a follow-up before the Phase 5 gateway replication layer (WP-5.2).
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
6. **Phase 4 is the AAA bar; Phase 5 exceeds it.** We claim no Nanite/Lumen parity. The render *feature* bar is exactly AAA-REN-6. Whether the result looks and feels at the level of the reference class is judged separately, by AAA-REN-8's external panel against current titles, and no count of passing numbers substitutes for it.
7. **Humans judge what bots cannot.** Feel, look and tool usability are gated by paid external humans (REN-8, TOOL-5, TOOL-10) at fixed exits, with published rubrics and pass thresholds, so "AAA" never rests on the loop grading its own work.

### 5.4 What "done" means per phase

| Phase | Done means (demonstrated on Windows and Linux) | Gates (AAA- prefix omitted) |
|---|---|---|
| 0 Foundations | CI green on every toolchain; `helios-schemac` emits C++/Go/Luau/SQL; golden images pass on the Vulkan and Null backends; a headless cell ticks a dilatable zone clock; `helios-backend` serves login without Docker; the launcher fetches a chunked manifest | PLT-1/2, REN-7 |
| 1 First Light | A player logs in via the launcher, undocks a Kestrel, descends to Harrow with no loading screen, lands, walks into Saltmarch and shoots Hollow drones with prediction; 50 players; persistence; the editor places entities, edits records and runs PIE; an external panel rates the descent and the firefight at least near par with reference titles | REN-1 (Ph1 slice), REN-5, REN-6 (Ph1, including CDLOD planets and orbit-to-ground atmosphere), REN-8 (Ph1), CNT-1 (Ph1), ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1, TOOL-7 (Ph1), SRV Ph1 |
| 2 Alpha Sandbox | Osk resources → crafting → market on the ledger; quests and dialogue; lairs; chat and guilds; 500 players per zone; CDC patching; P0 tools complete | SRV Ph2, ITR-2/4/6, STB-4, SEC-2/3/6, CNT-7, TOOL-2/6, TOOL-7/8 (Ph2) |
| 3 Beta Scale | Multi-cell orbit with handoff < 100 ms; 5k CCU; a 500-ship Vane battle in a 2 Hz zone; Earth-size planets; strike; phased story; settlements; Vane; Mule; live co-editing; the designer day passes; the external panel rates look and feel on par with reference titles | SRV Ph3, REN-4, REN-6 (Ph3), REN-8 (Ph3), ITR-7, STB-5, CNT-1 (Earth-size), CNT-2/4/5, SEC-5/7, TOOL-3/5, TOOL-7/9 (Ph3) |
| 4 Launch Quality | **The AAA bar:** all BENCH scenes within budget on MIN and REF; 2,000-ship battle in a 2 Hz zone at `d` ≥ 0.1, rendered by a REF client at ≥ 45 fps (BENCH-3); a cell killed in multi-cell Harrow orbit loses ≤ 1 s of state with no disconnect; raid; seasons; 50k CCU held through the 72 h soak; clean penetration test; all 30 tools; the external panel rates look and feel on par with current AAA sci-fi titles; a contracted content team builds a new zone with the shipped tools and zero blocking issues | every criterion with Ph ≤ 4, including REN-8 (Ph4) and TOOL-10 |
| 5 Ambition | Five systems with seamless Lattice travel; dynamic meshing; 100k-bot single shard; sandboxed player scripting; RT on SHOWCASE | SRV Ph5, REN-6 (RT) |

---

## 6. Glossary

- **Shard:** a regional deployment of gateways, cells and a shard DB presenting one world. Account services are global. From Phase 5 a shard may be the whole world.
- **Zone:** a gameplay space (system space, planet surface, interior, instance) with its own static data, reference frame, tick profile and **zone clock**. The zone clock is dilatable (TiDi) with a 10% floor, and all timed gameplay runs on it.
- **Cell:** a spatial region of a zone, owned by exactly one **cell server** (the headless C++ runtime) at a time. In v0, zone = cell.
- **Gateway:** the C++ edge that terminates encrypted UDP and routes traffic to cells. Clients never talk to cells directly.
- **Replicant:** from Phase 4, a non-simulating process that holds the latest state of the AGs owned by up to 4 world-zone cells (of one zone or several), streamed from their serialize-once gather, so a standby resumes a crashed cell's region from RAM (ADR-007, 04 §6.4). It holds no authority and never talks to clients.
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
- **Tick profile:** a zone's simulation and snapshot rates, fixed in its zone record. The engine accepts 1–60 Hz; the scorecard measures four profiles (§3.4): fleet battle at 2 Hz, phase/housing at 10 Hz, open world and space at 20 Hz, and activities at 30–60 Hz.
- **Foundation layer:** the forkable starter gameplay that ships with the engine.
- **BENCH-n / AAA Scorecard:** the standard measured scenes, and the full set of AAA-* criteria. Each BENCH ID names one scene and load; BENCH-3 is the client side of the same battle that AAA-SRV-10 / NS-4.2 measure on the server.
- **Evidence class:** how a criterion is measured (09 §5.6): **N** nightly, **H** nightly on lab hardware, **W** long-running scheduled runs, **M** manual or external judgement recorded in `docs/evidence/`. The class never changes a threshold.
- **Look-and-feel panel:** the blinded external reviewers of AAA-REN-8 (§3.3.1).
- **Project schema package:** a studio's `.hschema` package. **Native** packages compile to C++ and Go; **dynamic** packages, the default for projects, ship as a runtime type bundle that the prebuilt engine and backend read, so a new type needs no compiler (02 §3.8).
- **World script:** studio Luau that runs at shard scope in a world-script host (`helios-cell --role world-script`), with declared tables, timers, RPCs and escrow-only value; the backend extension surface (05 §1.23).
- **Planned migration:** moving a region of a zone to another cell process with no disconnect and no state loss (a hitch of ≤ 1 s per region moved), used by rolling restarts, `Drain` and `PreProvision` (04 §6.7).
- **Persistent world zone:** a zone that exists for the shard's lifetime (hub, station, open space, fleet battle), single-cell or multi-cell, as opposed to activity, phase and housing instances. From Phase 4 every persistent world zone recovers from the replicant tier (AAA-SRV-9, ADR-007, 04 §6.4).
- **Gateway relocation:** from Phase 4, moving a session make-before-break from a drained gateway to another one: the client holds both connections until every contributing cell has re-keyed, so a gateway deploy leaves no session linkdead (AAA-STB-6, 04 §2.4, 05 §6.3.1).
- **Player fleet:** a group of ≤ 256 players in a fleet → wing → squad hierarchy, with commander roles, broadcasts, fleet warp and command bursts (05 §1.12.1, 06 §8.3a).
- **Document home:** the one collab session (a zone session or the project-wide data session) that accepts edits to a document at a time (07 §1.8.2).
- **Transaction origin:** the input path stamped on every editor transaction (`ui`, `ui-scripted`, `luau`, `rpc`, `cli`, `import`, `collab`), used by AAA-TOOL-10's UI-provenance report (§3.9.1).

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
- **R05:** P0-3…P0-5, P0-8…P0-11; P1-14…P1-16, P1-20; P2-25; the §6 tooling targets; the §8.2 feel lessons, judged by humans in AAA-REN-8 (F1–F5).
- **R06:** ENG-02, ENG-07, ENG-17, ENG-27.
- **R07:** the §8.3 scale table (AAA-SRV); §3.7 tick profiles; §8.4 failure handling (STB-5, SRV-12); P0-10, P1-13, P1-14, P2-21…P2-23.
- **R08:** §3 T01–T30 (AAA-TOOL; production-scale human use in TOOL-10); ED-P0-01 (SEC-4), ED-P0-09 (ITR-3), ED-P2-01 (ITR-7), ED-P2-08.
- **R09:** GP-P0-1…12 (matrix rows W01, M01–M08, G01, G05, G06, G08, G10, G13, G14, G16, S06); RD-P0-1…10 and the RD-P1/P2 lists (AAA-REN-6).
