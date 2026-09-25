# Helios: Master Plan

*Start here. This document summarizes the Helios master plan and links to everything else. The detail lives
in the ten plan sections under [`plan/`](plan/README.md), and the evidence behind them lives in ten research
reports under [`research/`](research/). Where this summary and a section disagree, the section wins, and
[`plan/00-decisions.md`](plan/00-decisions.md) (the Architecture Decision Record) wins over both.*

**Status:** draft v1, 2026-09-25. Phase 0 (Foundations) is in progress.

---

## 1. Executive summary

**What Helios is.** Helios is one integrated product for building and operating science-fiction MMOs. It has
six parts, and all six share one schema language, one runtime and one content model:

1. a **C++20 engine runtime** (jobs, ECS, nested reference frames, Jolt physics per grid, Vulkan 1.3, Luau,
   game UI);
2. an **editor** with 30 tools, built on the real renderer and simulation;
3. a **game client**;
4. a **launcher/patcher**;
5. a **C++ server tier** (gateways and headless cell servers);
6. a **Go backend** (auth, orchestration, item and currency ledger, market, chat, social, persistence,
   telemetry, GM tools, patch service).

Windows 10/11 x64 is the primary platform for development and play. Linux x64 builds and passes the same tests
on every commit, and it is the production OS for servers.

**The goal.** The user asked for an engine at an AAA standard that "could build a game like Star Wars
Galaxies, Destiny, Star Citizen", EVE Online or SWTOR. The plan turns that into a testable target. A studio of
30–150 people should be able to build and run an original MMO of any of those five classes, or a hybrid of
them in one shard, by writing records, Luau scripts and domain graphs, never by modifying engine or backend
source ([01 §1](plan/01-vision-and-scope.md#1-product-vision)). "AAA" is not left as an adjective. It is
defined as **60 measurable criteria**, the *AAA Scorecard*. The criteria cover rendering performance on named
hardware, server scale, iteration speed, stability, content scale, security, tooling completeness and platform
parity, and they are measured in six scripted benchmark scenes. The bar is met when every criterion tagged
Phase ≤ 4 passes on Windows and Linux.

**How we get there.**
- **Six phases.** Phases 0–4 lead to the AAA bar and Phase 5 goes past it. Each phase ends with a playable
  demo of *Cinder Reach*, an original sample game that proves the five game classes one capability at a time.
- **An agent build loop.** A team of agents always works the highest-priority failing scorecard criterion.
  Every change passes adversarial review and a six-configuration CI matrix. Independent auditors must score
  each phase exit at **≥ 9/10**, and the user validates each milestone on Windows.
- **Seams first, depth later.** Each hard scaling feature ships on day one as an interface with a trivial
  implementation, then grows. Examples are server meshing, authority handoff and streaming install. This is
  the main lesson of Star Citizen's decade of retrofits.
- **Reuse what is solved.** Jolt, Luau, flecs, netcode, PostgreSQL, NATS and similar libraries do the solved
  work. The effort goes where Helios is different: the large-world model, replication, the ledger and the
  authoring workflows.

**Honest scale.** A conventional studio would need about **450–730 person-years over 6–8 years, roughly
$80–180M**, to reach the Phase 4 bar ([09 §4](plan/09-roadmap-and-process.md#4-effort-and-staffing-reality-check)).
That is less than SWTOR (~6 years, up to 800 developers, ~$200M on a licensed engine) or Star Citizen
(>$800M), because Helios reuses mature libraries, does not chase Nanite/Lumen parity, and ships proof content
rather than a 500-hour game. The agent loop may compress well-specified engineering 3–10×. It cannot compress
the gates that need people, hardware or lawyers:
- real GPU and server labs;
- artists, animators and voice actors;
- human playtesters;
- a code-signing certificate;
- a penetration test;
- patent counsel.

Until Phase 0 yields velocity data, any calendar for an agent-built Helios is unknown, and the plan says so
rather than guessing.

**Where we are.** The plan is drafted (v1) and goes to independent review. The repository has the CMake presets for every toolchain, 25 vendored
permissive-licence dependencies and CI on Windows and Linux. `engine/core` (133/133 test cases pass) and
`engine/math` (101/102 pass) have landed. Everything else is specified with work packages, owners, dependencies
and acceptance tests ([09 §8](plan/09-roadmap-and-process.md#8-current-status-and-next-steps)).

---

## 2. What the user asked for, and where the plan covers it

| The request | How the plan answers it | Where |
|---|---|---|
| "A full modern MMO backend" | **C++:** a gateway tier (encrypted UDP, connect tokens) and headless cell servers (Shard → Zone → Cell), with authority groups and epoch-fenced handoff, interest management, time dilation and overload policy. **Go:** 20 services, including an append-only double-entry **item and currency ledger**, regional order-book markets, durable timers, write-behind persistence, chat, social, activities, telemetry, GM/admin and patch manifests. **Stores and ops:** PostgreSQL, Valkey and NATS JetStream; Kubernetes + Agones fleets; CDC patching from a CDN | [04](plan/04-networking-and-servers.md), [05](plan/05-backend-services.md), [ADR-007/008](plan/00-decisions.md#adr-007-server--network-architecture) |
| "An editor that contains all tools needed to build a fully functional game" | A UI-less **ToolsFramework** (documents, commands, property-path transactions, crash journal, automation) under an ImGui shell, with **30 tools, T01–T30**. Every record type is editable in the generic inspector from day one. Play-in-Editor runs a real cell server, clients and bots | [07](plan/07-editor-and-tools.md) |
| "Level editors" | T01 World, T02 Prefabs, T03 Blockout, T04 Terrain & Planet (cube-sphere, layer stack), T05 Scatter, T06 Splines, T07 Star System & Galaxy | [07 §2.1](plan/07-editor-and-tools.md#21-world-building) |
| "Scripting" | **Luau** (typed, sandboxed, hot-reloaded in ≤ 2 s) on client, server and editor. A DAP debugger works in VS Code, and luau-lsp gets generated type definitions. Domain visual graphs compile to Luau, native ability state machines or HXL formulas (T10, T11) | [02 §7.4](plan/02-engine-runtime.md#74-scripting-host-luau-0739-r01-p0-4-r08-ed-p0-10), [06 §11](plan/06-gameplay-framework.md#11-scripting-api-graphs-and-determinism), [07 T10–T11](plan/07-editor-and-tools.md#22-data-and-gameplay) |
| "Object editors" | T08 Data & Record-Template Editor (inspector plus a 100k-row spreadsheet grid), T09 Gameplay Systems (attributes, effects, abilities, loot, crafting, fitting sandbox), T21 Modular Assembly (ships, vehicles, stations, housing), T22 Character Customization | [07 §2.2–2.4](plan/07-editor-and-tools.md#22-data-and-gameplay) |
| "Animation" | ozz-animation runtime with anim graphs, IK, cross-species retargeting and crowd LOD. **T15 Animation Suite**, T14 Cinematic Sequencer, and motion matching plus facial lip-sync in Phase 4 | [02 §7.2](plan/02-engine-runtime.md#72-animation-ozz-017-r06-eng-28-r08-ed-p1-08), [07 T14–T15](plan/07-editor-and-tools.md#23-presentation) |
| "Everything you would need to build an MMO game" | Also: materials (T16), VFX (T17), environment/post (T18), UI designer (T19), audio (T20), AI/navigation/spawns (T23), assets (T24), localization (T25), quests (T12), dialogue with VO workflow (T13), profiling (T26), GM/live-ops (T27), validation (T28), build/cook/deploy (T29), live collaboration (T30) | [07 §2](plan/07-editor-and-tools.md#2-the-tool-suite-t01t30) |
| "Client" | Engine + RmlUi game UI + netcode. A table-driven state machine, one continuous camera from cockpit to on-foot, data-bound HUD, diegetic cockpit screens, rebindable input (mouse, gamepad, HOTAS, IME), accessibility, crash reporting, sandboxed UI addons | [08 §1](plan/08-client-and-launcher.md#1-client-application) |
| "Launcher" | A C++ launcher (RmlUi on `SDL_Renderer`). It gates CPU and GPU, logs in, patches content-addressed chunks from a CDN with signed manifests, and supports verify/repair, self-update, play-while-downloading and pre-download, plus a per-user installer with no UAC | [08 §2](plan/08-client-and-launcher.md#2-launcher-and-patcher) |
| "At a AAA style standard" | The **AAA Scorecard**: 60 criteria on five reference hardware tiers and six benchmark scenes. The Phase 4 exit is the bar | [01 §3](plan/01-vision-and-scope.md#3-the-aaa-bar-measurable-acceptance-criteria), §9 below |
| "Could build a game like Star Wars Galaxies, Destiny, Star Citizen", EVE, SWTOR | A **51-capability matrix** scored against the five reference game classes, with an owning section and a phase for each capability, and a *Cinder Reach* proof for each class | [01 §2](plan/01-vision-and-scope.md#2-reference-game-archetypes), §7 below |
| "Deploy a team of agents to research …" | **Ten research reports**: EVE and Carbon, SWG and its emulators, SWTOR and HeroEngine, Star Citizen, Destiny, modern engines (Unreal, Godot, Unity DOTS, O3DE, Bevy, Frostbite, id Tech), MMO backends, editors, sci-fi gameplay and graphics, and technology selection | [`research/`](research/), §3 below |
| "Put together a plan for how you will build this" | This document, the ADR, and sections 01–09, including a work-package roadmap with owners, dependencies and acceptance tests | §6, §8 below |
| "Run on a loop until the Engine is at a AAA style standard" | The **build loop**: director, implementers, adversarial reviewers, integrator and auditors, iterating on failing criteria. It terminates at the Phase 4 exit | [09 §5](plan/09-roadmap-and-process.md#5-the-build-loop) |
| "Have a review agent … rate it a 9 out of 10" | This plan goes to an independent review agent and is not final until it scores ≥ 9/10. The build loop applies the same bar at every phase exit, with two independent auditors | [09 §5.7](plan/09-roadmap-and-process.md#57-termination-condition-and-independent-scoring) |
| "Run this on Windows or Linux … Windows is the preference … compatible/buildable for both" | Windows-first (MSVC 2022 primary; Visual Studio solution preset), Linux parity (GCC 13+, Clang 17+) and a MinGW portability check. The whole backend runs on Windows **without Docker**. Six scorecard criteria (AAA-PLT-1…6) enforce parity | [ADR-001](plan/00-decisions.md#adr-001-platforms), §11 below |

---

## 3. Research synthesis

Ten agents researched the reference games, their engines and modern practice. The reports cite their sources
and flag unverified claims. **IP hygiene:** no code from the leaked SWG source or the AGPL/GPL emulators
(SWGEmu Core3, Holocore, SWG:ANH) was read for implementation or copied. They were studied only through public
descriptions and repository structure.

**[R01 — EVE Online and Carbon](research/01-eve-online.md).** EVE runs a C++ core under ~2.4M lines of Python.
It partitions by solar system, ticks at 1 Hz, and **slows time (TiDi)** rather than dropping updates when a
node is overloaded. The report's list of what CCP had to undo is the most useful part:
- a single-threaded interpreter per node became the bottleneck;
- a solar system cannot move between nodes live, so big fights have to be pre-provisioned;
- broadcast fanout is the wall in large battles;
- putting business logic in the database slowed every change;
- observability had to be retrofitted.

*Adopted:* a dilatable zone clock with a 10 % floor, no script on the hot path, live zone migration (Phase 3),
reinforcement timers that pre-provision capacity, Dogma-style attribute modifiers, reason-coded economy
telemetry, and command-replicated "ball" movement for thousands of ships.

**[R02 — Star Wars Galaxies and its emulators](research/02-star-wars-galaxies.md).** SWG is the sandbox
reference:
- skill-box professions;
- crafting from randomly spawning, stat-bearing resources;
- player cities;
- socially necessary professions;
- a component-based space game (Jump to Lightspeed).

Its **rule-based procedural terrain** (boundaries, filters and affectors, evaluated identically on client and
server) let a small team ship ten 16 km planets. The lessons are these. The NGE shows the cost of redesigning a
live sandbox, so tuning must be data. Economies need instruments. Free housing sprawls, so structures need
lifecycle caps. Static partitions fail under hotspots. None of the emulators rebuilt SOE's multi-server
planets, which is the hard part.
*Adopted:* the layer stack as one view of a generic terrain node graph. The graph form keeps us clear of the
SWG terrain patents, and counsel reviews it before Phase 4. Also adopted: client/server record splits,
resource spawns and the crafting state machine, housing maintenance and reclamation, and patch-pak overlays.

**[R03 — SWTOR, HeroEngine and themeparks](research/03-swtor-heroengine-themepark.md).** HeroEngine's live,
collaborative, server-authoritative in-world editing, with separate edit and play instances, still looks
modern. SWTOR's pain came from forking an unfinished engine while building the game on it, and from UI tied
to DX9, which still blocks its DX12 port. Story at MMO scale (>200k VO lines, >558k strings) is a
content-pipeline problem. Instancing hides scaling limits until a big battle finds them.
*Adopted:* edit instances with pinned content versions, UI that never calls a graphics API, group
conversations with roll resolution, dialogue coverage tooling, web tools for writers, global handles
(`name#1234`), overflow layers and phasing, and ship space and ground play on one simulation (no on-rails
minigame).

**[R04 — Star Citizen and seamless universes](research/04-star-citizen-seamless-universe.md).** SC's
seamlessness rests on four cross-cutting decisions:
1. hierarchical coordinate frames;
2. streamable object containers as the single unit of content;
3. entity authority independent of which process simulates the entity;
4. a replication and persistence layer between servers and clients.

Retrofitting all four onto CryEngine explains most of its decade-long delay. Other lessons: server tick rate
is the real metric, persistence needs garbage collection, split authority is a programming model, and bugs
live at seams. The report also shaped the stack: keep the gateway and replication in C++ (not Go), keep NATS
off the per-tick path, and write entities through a write-behind service.
*Adopted:* all four seams from day one ([ADR-005](plan/00-decisions.md#adr-005-world-model-large-worlds),
[ADR-007](plan/00-decisions.md#adr-007-server--network-architecture)), entity budgets and lifecycle policies,
bots that cross every boundary type, and master modes as data.

**[R05 — Destiny and action MMOs](research/05-destiny-action-mmo.md).** Destiny **splits authority by latency
tolerance**. Own-avatar feel is local, physics and AI run on a simulation host, activity progress lives in
durable cloud services, and loot and progression live in backend services. A simulation-host crash therefore
never costs progress. The Tiger renderer's **extract → prepare → submit** pipeline simulates frame N while
rendering N−1. The warnings: client authority is debt (cheating in Destiny, The Division and PlanetSide 2;
New World's dupes), sequential RNG seeds biased perk rolls, and an 8-hour map load for a half-second change
slowed content for years.
*Adopted:* the Overwatch model (server-authoritative, predicted, lag-compensated) instead of client authority,
a durable activity service outside the cell, predicted abilities compiled to a rollback-safe native state
machine, salted and audited RNG with statistical CI tests, and hot reload in ≤ 2 s as a scorecard criterion.

**[R06 — Modern engines](research/06-modern-engines.md).** Unreal, Godot, Unity DOTS, O3DE, Bevy, Frostbite
and id Tech converge on a common set of patterns:
- handle-based low-level "servers" under the object model, which lets a headless server drop rendering;
- **one** entity model (Unreal and Unity paid for bolting ECS onto an older model);
- reflection as the backbone of serialization, editing, undo, bindings and networking;
- a render graph with bindless, GPU-driven, clustered rendering;
- f64 positions on the CPU with camera-relative floats on the GPU;
- filter, prioritize and serialize-once replication (Replication Graph, Iris);
- content-addressed asset pipelines;
- visual scripting only with high-level nodes (Godot removed VisualScript).

*Adopted:* nearly all of it: flecs as the single model, schema-first reflection, a FrameGraph-style render
graph, Iris-style push replication, an O3DE-style asset daemon with a DDC, gems, and domain-only graphs.

**[R07 — MMO backend architecture](research/07-mmo-backend-architecture.md).** The architectures that worked
for years **decouple the client connection from the simulation process**: EVE's proxy/sol split, SC's
replication layer, BigWorld's base/cell apps. Spatial partitioning fails in the giant battle, and the shipped
mitigations are time dilation, functional offload, pre-allocated hardware and admission caps. SpatialOS
failed by building generic middleware first; its Unreal GDK was archived in 2024. The report's other points:
- dupes are distributed-systems bugs, fixed by leases plus fencing epochs, an append-only ledger and
  idempotency keys;
- netcode.io-style tokens keep game servers away from the auth database;
- content-defined chunking cut League of Legends patch times about 10×;
- egress dominates operating cost.

*Adopted:* the staged v0 → v1 → v2 cell architecture, the five-stage overload policy, the ledger, the tokens,
FastCDC patching and the egress cost model.

**[R08 — Editors and tools](research/08-editors-and-tools.md).** The editor is a **platform**: a
schema-driven object model, one transaction system, text source formats, an asset daemon, PIE on real server
processes, and domain editors on top. SWG shipped 30+ standalone editors plus a "God Client". The flags that
shaped the plan:
- ImGui has no internationalization or accessibility, so it is editor-only and players get a retained-mode UI;
- no general Blueprint clone;
- all authored data is text;
- one schema IDL for C++, Go and scripts;
- ufbx and ozz for import and animation;
- DAP for script debugging.

*Adopted:* the tool suite T01–T30 and the platform-first order of 07. The audio recommendation (F5) was
overridden: ADR-013 makes the Helios event system primary and Wwise/FMOD optional plugins.

**[R09 — Sci-fi gameplay and graphics](research/09-scifi-gameplay-and-graphics.md).** Four gameplay scales
must coexist: strategic space (EVE), tactical 6-DoF flight (Elite, SC), ground and interiors, and seamless
planets. The report's conclusions:
- **build a gameplay kernel, not a genre**: EVE's Dogma and Unreal's GAS both reduce fitting, buffs, skills
  and EWAR to modifiers on attributes, gated by tags;
- movement models are pluggable per entity class;
- the sci-fi look is mostly an HDR problem (one blinding sun, black shadows, emissives, nebula IBL);
- planets need Hillaire 2020 atmosphere LUTs, cube-sphere CDLOD terrain and Nubis-style clouds;
- FSR 2 is the upscaler that has a Vulkan backend;
- the ledger must exist from day one.

*Adopted:* the gameplay kernel of 06 and the space and planet renderer of 03.

**[R10 — Technology selection](research/10-tech-selection.md).** Every version and licence was checked against
upstream on 2026-09-25, and the third-party tree was built. Verdicts:
- replace GLFW with **SDL3** (IME, rumble, gyro, raw mouse);
- move to **Jolt 5.6** with cross-platform determinism;
- take Monocypher 4.0.3 (an EdDSA timing fix) and Tracy 0.14.1;
- use **Go 1.27.1** (1.24 is out of support);
- use **embedded-postgres** rather than SQLite for local services;
- use Slang for shaders;
- ship a self-installing launcher (MSIX and WiX rejected).

The report also found real build bugs: Jolt ISA flags that made Windows and Linux physics differ, an inverted
zstd multithreading define, no chosen MSVC runtime, and `/Zi` defeating compiler caches. All were fixed in the
scaffold.
*Adopted:* [ADR-011](plan/00-decisions.md#adr-011-core-runtime),
[ADR-013](plan/00-decisions.md#adr-013-runtime-libraries-r10-manifest) and
[ADR-014](plan/00-decisions.md#adr-014-backend-toolchain-r10-11).

**Cross-cutting conclusions.**
1. **Seams before scale.** SC, EVE and SWG all paid for adding authority, streaming or migration late.
2. **Session is separate from simulation.** Clients talk only to gateways; cells are private and replaceable.
3. **Server authority plus prediction** gives nearly the feel of client authority at none of its cost.
4. **Value moves only through a ledger.** It is synchronous, idempotent and epoch-fenced; everything else is
   write-behind.
5. **The game is data.** A generic kernel, typed records, one schema and domain graphs make it cheap to retune
   a live game.
6. **Iteration time is a feature** with a budget, not a hope.
7. **UI and tools never bind to one graphics API.**

---

## 4. Architecture overview

### 4.1 Process topology

```
 Player PC (Windows / Linux)                                          Private shard network
 ┌───────────────────────┐  HTTPS: immutable chunks,   ┌─────┐
 │ Launcher              │◄─ signed manifests, pointers│ CDN │◄── helios-patch publish (CI)
 │ RmlUi on SDL_Renderer │                             └─────┘
 └──────────┬────────────┘
            │ one-time launch code over an inherited pipe (never the command line)
 ┌──────────▼────────────┐  HTTPS: login, queue (SSE), connect token, reconnect
 │ Client                │─────────────────────────────────────────────┐
 │ engine + RmlUi + HTP  │                                             ▼
 └──────────┬────────────┘                    ┌────────────────────────────────────────────────┐
            │ HTP: encrypted UDP 7777         │ Go services (one helios-backend binary in dev) │
 ┌──────────▼────────────┐   NATS service     │ identity · login queue · session · orchestrator│
 │ Gateway ×N (C++)      │── lane (chat,     ►│ character · ledger · market · industry/timers  │
 │ tokens · rate limits  │   social, market)  │ mail · chat · social · activity · persistence  │
 │ routes · chunk packing│                    │ content/collab · config · telemetry · GM · API │
 └──────────┬────────────┘                    └───────┬──────────────────────▲─────────────────┘
            │ HTP trunks (private)                    │ leases, routes,      │ nats.c request/reply and
 ┌──────────▼──────────────────────────────┐          │ placement, control   │ JetStream: fence, ledger,
 │ Cell servers (C++ headless engine)      │◄─────────┘                      │ checkpoints, telemetry
 │ ZoneHost → ZoneInstances: flecs ECS,    │─────────────────────────────────┘ (never per tick)
 │ Jolt grids, Luau VM, ZoneClock (TiDi),  │
 │ replication, authority groups           │◄──► other cells: ghosts, effects, handoff
 └─────────────────────────────────────────┘     (HTP trunk, never the NATS bus)

 Data:   PostgreSQL 18 (global · per-shard primary · separate checkpoint cluster) · Valkey (sessions, queue,
         presence; always rebuildable) · NATS JetStream (events, timers, audit) · ClickHouse · S3-compatible
         object storage (backups, CDN origin, crash dumps)
 Fleets: LocalProcessPlacer on a dev PC → VMs (Phases 0–2) → Kubernetes + Agones (Phase 3+)
 Editor: helios-editor ⇄ helios-assetd (import, cook, DDC, hot reload) ⇄ PIE cell + clients + bots
         ⇄ collab service (edit instances, Phase 3)
```

**Invariants** ([04 §0](plan/04-networking-and-servers.md#0-design-rules),
[05 §0](plan/05-backend-services.md#0-principles)):
- **Clients.** They reach only gateways and HTTPS APIs, and they send intents, never outcomes.
- **Authority.** Every replicated entity belongs to one authority group, owned by one cell at one epoch.
- **Value.** It moves only through synchronous ledger calls that carry idempotency keys and fences.
- **The tick.** The simulation tick never waits on a service.

**Topology stages** ([04 §6.5](plan/04-networking-and-servers.md#65-staging-adr-007)):
- **v0 (Phases 1–2):** one cell per zone, with the handoff and ghost APIs present from the start.
- **v1 (Phase 3):** static multi-cell zones with handoff p99 < 100 ms.
- **v2 (Phase 5):** dynamic split/merge, plus a replication layer in the gateway that makes cell crashes
  invisible to players.

On a Windows dev box, `helios-backend.exe` runs every Go service in one process, with embedded PostgreSQL,
embedded NATS and miniredis, and no Docker.

### 4.2 Module layering (a strict DAG, enforced at configure time)

```
L5 apps       helios-client   helios-editor   helios-cell   helios-gateway   helios-launcher   tools
              (schemac · shaderc · assetd · cook · pack · fitsim · bot · helios-tool · rendertest)
                    │               │              │              │                │
L4 framework  world* · gameplay* · assembly* · replication* · netgame* · authority* · clientcore*
              presentation · assetpipe(ED) · toolsfw(ED) · editorui(ED) · edtools/*(ED)
L3 servers    physics* · anim* · nav* · pcg* · script* · net*   │   audio · text · ui · rhi · render
L2 foundation reflect* · hxl* · asset* · records* · ecs* · loc* · telemetry* · patch* · crash*  │  app · input
L1 core       core* · math*

 *  = HEADLESS: linked by cell servers; never reaches app, input, rhi, render, audio, text, ui, presentation
 ED = EDITOR_ONLY: never linked by client, cell or gateway
```

**Rules** ([02 §1.1](plan/02-engine-runtime.md#11-layering-dag-r06-eng-01)):
- A module depends only downward, or on a same-layer module it lists explicitly.
- Configuration fails on a cycle, on a HEADLESS leak or on an EDITOR_ONLY leak.
- No third-party type appears in a public header, which keeps a custom-ECS fallback possible.
- Jolt's AVX2 flags stay private to `physics`, so the launcher and the client's CPU gate run on any x86-64
  CPU and can explain an unsupported one.

`render` defines the `RenderScene` packet, and `presentation` fills it at extract. The render thread never
reads the ECS.

**What links what** ([02 §1.3](plan/02-engine-runtime.md#13-build-targets)):

| Binary | Links |
|---|---|
| Client | The runtime plus graphics, audio, UI, `presentation` and `clientcore`. No shader compiler and no ImGui in shipping builds |
| Editor | The client plus the EDITOR_ONLY modules. It loads Slang at runtime and hot-reloads the game DLL |
| Cell | Only HEADLESS L1–L4 modules (`HELIOS_BUILD_GRAPHICS=OFF`) |
| Gateway | Only `core`, `reflect`, `net` and `telemetry` |

---

## 5. Key decisions

All decisions are binding. Changing one needs a new ADR entry with its rationale.

| ADR | Decision | Main reason |
|---|---|---|
| [001 Platforms](plan/00-decisions.md#adr-001-platforms) | Windows 10/11 x64 primary (MSVC 2022, clang-cl); Linux x64 fully supported (GCC 13+, Clang 17+) and the production server OS; MinGW cross-build check; no macOS/consoles yet | User requirement; RHI seam keeps other platforms possible |
| [002 Languages](plan/00-decisions.md#adr-002-languages) | C++20 for engine, editor, client, launcher, servers and tools; Go 1.27.1 for backend services; **Luau** for gameplay scripting; domain graphs; predicted abilities compile to a native rollback-safe format | Luau: sandboxing, gradual typing, native codegen (R01/R03/R05/R06); Overwatch Statescript lesson (R05) |
| [003 Rendering](plan/00-decisions.md#adr-003-rendering) | Vulkan 1.3 via volk + VMA; Null backend; D3D12 seam with a Phase 4 gate; render graph; extract → prepare → submit; GPU-driven; clustered forward+; reverse-Z camera-relative; **Slang** shaders | Frostbite, Destiny and id Tech patterns (R05, R06, R09) |
| [004 Entity model](plan/00-decisions.md#adr-004-entity-model--reflection) | **flecs 4.1.6** archetype ECS everywhere; one `.hschema` generates C++, Go, Luau, SQL, replication and editor metadata; stable 64-bit IDs; "record template", never "archetype", for content | One model and one schema (R06, R08) |
| [005 Large worlds](plan/00-decisions.md#adr-005-world-model-large-worlds) | Nested frames with f64 `WorldPos` and `Reparent()`; Jolt double precision, one `PhysicsSystem` per grid; a 10¹³ m no-jitter test | SC's retrofit lesson (R04) |
| [006 Content](plan/00-decisions.md#adr-006-content-model) | Object containers (text source, one entity per file); typed record DB with hash IDs; import → cook with a DDC; CDC-chunked paks; deterministic PCG | SC OCS, EVE FSD, Destiny, SWG (R01–R05, R07) |
| [007 Server/network](plan/00-decisions.md#adr-007-server--network-architecture) | Client ↔ gateway ↔ cells; Shard → Zone → Cell; per-zone 1–60 Hz tick with TiDi; single-writer authority groups with epoch fencing; v0 → v1 → v2; Tribes/Iris replication; five-stage overload policy | R02, R04, R07 |
| [008 Backend](plan/00-decisions.md#adr-008-backend-services) | Go services; NATS + JetStream; PostgreSQL as system of record and Valkey as cache; one local binary on Windows; reason code on every value movement | R07, R01 |
| [009 Editor](plan/00-decisions.md#adr-009-editor) | ImGui docking over a UI-less ToolsFramework; PIE on a real cell; HeroEngine-style edit instances; T01–T30 | R03, R06, R08 |
| [010 Client, UI, launcher](plan/00-decisions.md#adr-010-client-game-ui--launcher) | Retained-mode, data-bound game UI (RmlUi); ImGui for editor and debug only; C++ launcher on RmlUi + `SDL_Renderer`; self-installing per-user installer | SWTOR UI lesson (R03), R08 F1, R10 |
| [011 Core runtime](plan/00-decisions.md#adr-011-core-runtime) | Win32/POSIX platform layer; **SDL3 3.4.16**; job system with helping waits; mimalloc 3.5.3 tagged heaps; static `/MT`, `/Z7`; CMake ≥ 3.28 | R10 |
| [012 Quality](plan/00-decisions.md#adr-012-quality-bar--process) | CI on every toolchain; tested performance budgets; hot reload ≤ 2 s; IP hygiene | R05 iteration lesson |
| [013 Runtime libraries](plan/00-decisions.md#adr-013-runtime-libraries-r10-manifest) | Jolt 5.6.0 (deterministic), ozz 0.17, Recast 1.6, miniaudio, RmlUi 6.3 + FreeType/HarfBuzz, JSONC/yyjson, netcode 1.4.8 + reliable 1.4.5, nats.c 3.14, sentry-native 0.17.1, Tracy 0.14.1, libopus | Verified pins and licences (R10) |
| [014 Backend toolchain](plan/00-decisions.md#adr-014-backend-toolchain-r10-11) | Go 1.27.1, pgx, connect-go, nats.go, goose; **embedded-postgres** + embedded NATS + miniredis for Docker-free Windows dev; no CGO; SQLite only for tools | One SQL dialect (R10) |

---

## 6. The plan sections

**[00 — Decisions](plan/00-decisions.md).** The fourteen ADRs summarized in §5. Every section and every work
package must conform.

**[01 — Vision and scope](plan/01-vision-and-scope.md).**
- The six product parts, the north star and seven design pillars ("seams first, generality later", "budgets
  are features").
- The five reference game classes with their experiences, numbers and lessons, and the **51-capability
  matrix**.
- The **AAA Scorecard**: five hardware tiers (MIN, REF, SHOWCASE, DEV, SERVER), six benchmark scenes
  (BENCH-1…6) and 60 criteria.
- *Cinder Reach*, the original reference content: five star systems and three factions, from about 150
  assets in Phase 1 to about 2,000 in Phase 5.
- Non-goals, IP and licence rules (permissive licences only; no Star Wars or other third-party IP; patent
  reviews), a "done means" table per phase, and the glossary.

**[02 — Engine runtime](plan/02-engine-runtime.md).**
- The L1–L5 module DAG, "gems" (plugins), build targets and memory budgets per hardware tier.
- The job system, and the client and cell frame pipelines.
- The normative **`.hschema` grammar** and the `helios-schemac` emitters (C++, replication, Luau, proto, Go,
  SQL, editor, records, lint).
- The flecs wrapper with Iris-style dirty bits.
- The world model:
  - reference frames and portal graphs;
  - `Reparent()` by the transport theorem;
  - Jolt grids with bubble and host kinds;
  - zones;
  - OFPA object containers;
  - streaming with a residency hook;
  - deterministic fixed-point PCG with a Slang twin.
- The asset pipeline (DDC, paks, hot reload in ≤ 2 s).
- Physics, animation with crowd LOD, audio, navigation, the Luau host with its DAP debugger, and the RmlUi
  game-UI runtime.
- 17 acceptance criteria. The most important is RT-01, a 50k-entity zone benchmark whose failure reopens the
  ECS decision.

**[03 — Rendering](plan/03-rendering.md).**
- A handle-based RHI with bindless descriptors, a Null backend and GPU-crash breadcrumbs, and a D3D12 seam
  with a costed Phase 4 gate.
- The Slang → SPIR-V toolchain with recorded PSO lists and fallback PSOs, so pipeline creation never stalls
  the render thread.
- A render graph with aliasing and async compute.
- A GPU scene with **two-level transforms**, so a ship moving at 1,500 m/s updates one record.
- Two-phase HZB culling, fleet impostors and brackets, and a visibility buffer in Phase 4.
- Clustered forward+ PBR, cascaded and later virtual shadow maps, and DDGI.
- The sci-fi set:
  - starfields and nebulae;
  - Hillaire atmosphere;
  - cube-sphere CDLOD planets from fixed-point noise, matched with the server's collision (bit-identical
    target, ≤ 1 cm limit);
  - Nubis clouds and oceans;
  - shields, plumes, warp and GPU particles.
- HDR, TAA with an `IUpscaler` interface (FSR 2 default), per-pass budgets on REF and MIN, and lavapipe golden
  images on every commit.

**[04 — Networking and servers](plan/04-networking-and-servers.md).**
- **HTP**: vendored netcode + reliable, with eight Helios channels and no crypto of our own.
- Connect tokens, reconnect tickets, AIMD bandwidth budgets and gateway trunks.
- The cell tick graph, with budgets per zone profile.
- Iris-style replication: shadow state, serialize-once, deltas against acked baselines, a hierarchical
  interest hash and a priority accumulator, with worked bandwidth math.
- Prediction and reconciliation, lag compensation with capped rewind, and EVE-style command replication for
  large fleets.
- Authority groups, ghosts, effects, the handoff protocol and crash recovery.
- Zones, instances, layers and phasing; the overload ladder; security and anti-cheat; NetSim, deterministic
  replay and bot swarms.

**[05 — Backend services](plan/05-backend-services.md).**
- A 20-service catalogue built as a **modular monolith until Phase 3**.
- The double-entry ledger with custody, fences, escrow and idempotency.
- Market actors, durable timers, persistence and the fence, content publishing and the collab service, live
  config and kill switches, telemetry and trust, and GM audit with a hash chain.
- connect-go for Go and HTTPS; nats.c with generated binary codecs for cells.
- The data model with partitioning and backups.
- Dupe prevention, exactly-once effects through a transactional outbox, sagas and conservation audits.
- The Docker-free Windows developer experience, Kubernetes + Agones operations, SLOs, an egress cost model,
  and FastCDC patching with signed manifests.

**[06 — Gameplay framework](plan/06-gameplay-framework.md).**
- A **kernel, not a genre**: tags, Dogma/GAS attributes and modifiers, effects on a TiDi-safe timing wheel,
  abilities with a native predicted state machine (ASM), cues, and the HXL formula language (C++ and Go
  interpreters, bit-identical).
- Items with ledger custody, sockets and audited RNG.
- SWG-style resources and crafting, and EVE-style industry.
- Economy reason codes, vendors and markets.
- One progression model; quests, missions, public events and group conversations.
- AI with behaviour trees, utility selectors and AI LOD tiers.
- Six-degree-of-freedom fly-by-wire with thruster allocation, command flight, warp, multi-crew seats, sensors
  and EWAR, and one damage pipeline.
- Housing, cities, territory, PvP law and character creation; the Luau API and determinism rules.

**[07 — Editor and tools](plan/07-editor-and-tools.md).**
- The ToolsFramework: documents, a command bus, **property-path transactions**, a crash journal, automation
  and headless `helios-tool`.
- The ImGui shell with role layouts, a shared widget library, and a large-world viewport with a "view as"
  phase debugger.
- PIE on a real cell; git + LFS source control with a JSONC merge driver; live collaboration in edit
  instances; web tools for writers.
- The full **T01–T30** specification, each tool with data flow and MVP and AAA scope.
- The asset daemon, DDC, validation engine, editor budgets, UX and accessibility, and a phase-by-phase tool
  delivery table.

**[08 — Client and launcher](plan/08-client-and-launcher.md).**
- Client startup budgets and state machine, threading (a game thread separate from the OS thread),
  settings, input, the camera rig, RmlUi panels, diegetic UI, accessibility, localization, and sandboxed
  addons with protected actions.
- The anti-cheat stance, and the Windows and Linux specifics.
- The launcher: CPU and GPU gates, login, a signed trust chain, chunk patching, streaming install,
  self-update, the installer and code signing, Linux AppImage packaging, and the crash and symbol pipeline.

**[09 — Roadmap and process](plan/09-roadmap-and-process.md).**
- Phases 0–5 as work packages (WP-0.1 onward): track, owner section, dependencies, deliverables and acceptance
  criteria.
- The critical path, and a reconciliation table for cross-section conflicts.
- The staffing and cost reality check, and what the agent loop does and does not change.
- The build loop: roles, rounds, definition of done, CI tiers, evidence classes, termination and regression
  ratchets.
- The test strategy, a 31-entry risk register, current status and the next ten work packages.

---

## 7. Capability coverage for the five reference game classes

Each capability has an owning section and a phase: the phase of its first shippable implementation, as given
in [01 §2.6](plan/01-vision-and-scope.md#26-capability-matrix). "Critical for" names the classes that are
impossible without it. Class keys: **SWG** sandbox, **EVE** single-shard economy and fleets, **DST**
Destiny-style action looter-shooter, **SC** Star Citizen-style seamless universe, **TOR** SWTOR-style story
themepark.

| ID | Capability | Critical for | Section | Ph |
|---|---|---|---|---|
| W01 | Nested frames, f64 `WorldPos`, `Reparent()` | EVE, SC | 02 | 1 |
| W02 | Per-grid physics; walkable moving ships | SC | 02 | 1 |
| W03 | Object-container streaming, client + server | SWG, SC | 02 | 1 |
| W04 | Deterministic cube-sphere planets, layer stack, stamps | SWG, SC | 02, 03 | 1 |
| W05 | Space ↔ surface ↔ interior, no loading screen | SC | 02–04 | 1 |
| W06 | System/galaxy model, jump graph, warp with prefetch | EVE, SC | 02, 06 | 1 |
| W07 | Instances, layers, phases | DST, TOR | 04 | 3 |
| W08 | Portal-cell interiors | SWG, SC, TOR | 02 | 1 |
| M01 | Command flight, thousands of ships | EVE | 06 | 3 |
| M02 | 6-DoF Newtonian flight with assists | SC | 06 | 1 |
| M03 | Locomotion, mounts, vehicles | SWG, DST, SC, TOR | 02, 06 | 1 |
| M04 | Tab-target combat, telegraphs | SWG, TOR | 06 | 2 |
| M05 | Predicted gunplay, lag compensation, aim assist | DST, SC | 04, 06 | 1 |
| M06 | Rollback-safe native ability graphs | DST | 06 | 1 (full 3) |
| M07 | Statistical space weapons, sensors, EWAR | EVE | 06 | 3 |
| M08 | Damage pipeline: resists, layers, subsystems | all five | 06 | 1 |
| M09 | Multi-crew stations, item ports, boarding, EVA | SC | 06 | 3 |
| G01 | Gameplay kernel | all five | 06 | 1 |
| G02 | Typed records, client/server split, hash IDs | all five | 02, 07 | 1 |
| G03 | Skills/professions, typed XP | SWG, EVE | 06 | 2 |
| G04 | Resources, harvesters, crafting, industry | SWG, EVE | 05, 06 | 2 |
| G05 | Socketed items, tested loot RNG | DST, TOR | 05, 06 | 2 |
| G06 | Order-book market, contracts, vendors | SWG, EVE | 05 | 2 |
| G07 | Housing, decoration, player cities | SWG | 05, 06 | 3 |
| G08 | Territory, standings, PvP flags, timers | EVE | 05, 06 | 3 |
| G09 | Chat, mail, guilds/corps, org wallets | SWG, EVE | 04, 05 | 2 |
| G10 | Quests, procedural missions, public events | DST, TOR | 06 | 2 |
| G11 | Cinematic and group dialogue, VO, companions | TOR | 06, 07 | 2 |
| G12 | Activities: fireteams, checkpoints, lockouts | DST, TOR | 04, 05 | 3 |
| G13 | AI: BT + utility + perception, AI LOD, spawns | SWG, DST, TOR | 06 | 1 |
| G14 | Death pipeline: wrecks, clones, insurance, killmails | EVE, SC | 06 | 2 |
| G15 | Character creation: species, morphs, fit | SWG, TOR | 02, 03, 07 | 4 |
| G16 | Modular ship assembly, fitting, liveries | SWG, EVE, SC | 06, 07 | 3 |
| G17 | Live-ops calendar, seasons, data hotfixes | DST | 05 | 4 |
| G18 | Background economy/world simulation | — | 05 | 5 |
| S01 | Gateway + cells, per-zone tick 1–60 Hz | all five | 04 | 1 |
| S02 | Static multi-cell zones, fenced handoff | SC | 04 | 3 |
| S03 | Dynamic split/merge, gateway replication layer | SC | 04 | 5 |
| S04 | TiDi zone clock, overload policy | EVE | 04 | 2 |
| S05 | Battle interest management, 1,000+ participants | EVE | 04 | 4 |
| S06 | Double-entry item/currency ledger | all five | 05 | 2 (core 1) |
| S07 | Write-behind persistence + cleanup policy | SWG, EVE, SC | 05 | 1 |
| S08 | Global identity, cross-shard social, placement | EVE, DST, TOR | 05 | 2 |
| S09 | Economy telemetry, public read API | SWG, EVE | 05 | 2 |
| R01 | HDR space look, shields, plumes, GPU particles | EVE, DST, SC | 03 | 1 |
| R02 | Atmosphere, planet terrain, volumetric clouds | SC | 03 | 1 |
| R03 | Fleet rendering: GPU culling, impostors, brackets | EVE | 03 | 4 |
| R04 | Cinematic characters, facial animation, lip-sync | TOR | 02, 03 | 4 |
| R05 | Crowds of 200+ with animation LOD | SWG, TOR | 02, 03 | 3 |
| R06 | Data-bound game UI, diegetic MFDs, data grids | EVE, SC | 08 | 1 |
| R07 | TAA/upscaling, view models, 120 fps mode | DST | 03 | 4 |

**How each class is proven in *Cinder Reach*** ([01 §4.2](plan/01-vision-and-scope.md#42-reference-content-cinder-reach)):

| Class | Proof content and phase | All critical capabilities by |
|---|---|---|
| SWG sandbox | Osk crafting and market (Ph2); Saltmarch settlements with 300 structures (Ph3, BENCH-5) | Ph4 (G15 character creation) |
| EVE economy and fleets | Osk Yard order books (Ph2); Vane territory and reinforcement timers (Ph3); 2,000-ship battle at 10 % TiDi (Ph4, BENCH-3) | Ph4 (S05, R03) |
| Destiny action | Hollow Vault strike (Ph3, BENCH-4); Lattice Heart 6-player raid and seasons (Ph4) | Ph4 (G17, R07) |
| Star Citizen seamless | BENCH-2 orbit-to-interior descent with no loading screen (Ph1); Mule multi-crew boarding at 300 m/s (Ph3, BENCH-6); seamless Lattice travel between systems (Ph5) | Ph5 (S03 dynamic meshing) |
| SWTOR story | "Signal from Saltmarch" quests and dialogue (Ph2); group conversation (Ph3); ~300 auto-staged voiced lines in 2 languages (Ph4) | Ph4 (G15, R04) |

**One honest caveat.** Four of the five classes have every critical capability by the Phase 4 bar. For the
Star Citizen class, the Phase 4 bar matches SC's *shipped* architecture: static multi-cell meshing (S02)
behind a gateway, with a ≤ 10 s hitch when a cell crashes. Dynamic split/merge and the gateway replication
layer (S03), which SC itself still has in progress, arrive in Phase 5 and take the class past the bar. The
Phase 5 work also waits on the review of Improbable's patent US 11,792,306.

---

## 8. Roadmap and build loop

### 8.1 Phases ([09 §1–2](plan/09-roadmap-and-process.md#1-roadmap-overview))

| Phase | Goal | Demo milestone (what the user sees) | Human-studio calendar | Key exit gates |
|---|---|---|---|---|
| **0 Foundations** | Every part exists as a skeleton on every toolchain | **M0 "Handshake":** the launcher verifies a signed chunked manifest; the client connects through a gateway to a cell ticking a dilatable clock; the editor edits a record with undo; all on Windows without Docker | 9–12 months | AAA-PLT-1/2, REN-7; RC-1; ED-1; the schemac, spike and backend skeleton tests |
| **1 First Light** | A vertical slice through every layer at 50 players | **M1 "Descent" (BENCH-2):** undock a *Kestrel*, descend from 400 km to Harrow with no loading screen, land, walk into Saltmarch and shoot Hollow drones with prediction beside 49 bots; the editor runs PIE with 2 clients | 12–18 months | REN-5, ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1; RT-01 (the ECS gate) |
| **2 Alpha Sandbox** | Economy, quests, social; 500 per zone; production patching; all P0 tools | **M2 "Osk Yard":** survey, harvest, craft a rifle with rolled perks, sell it on the market to another account, run a quest with dialogue, chat in a guild beside 500 bots; the signed launcher patches by CDC and self-updates | 15–21 months | SRV Ph2, SEC-2/3/6, CNT-7, TOOL-2/6, ITR-2/4/6 |
| **3 Beta Scale** | Multi-cell zones, instances, 5k CCU, live co-editing | **M3 "Harrow Orbit":** four cells, *Mule* boarding across a boundary (BENCH-6), a 5k-CCU bot shard, the Hollow Vault strike in < 2 s (BENCH-4), three designers co-editing a settlement (BENCH-5), the designer day | 18–24 months | SRV Ph3, STB-5, TOOL-3/5, ITR-7, CNT-2/4/5, SEC-5/7 |
| **4 Launch Quality** | **The AAA bar** | **M4 "Vane":** 2,000-ship battle at 10 % TiDi (BENCH-3 ≥ 45 fps on REF), 6-player raid, a season, auto-staged VO, a clean 72 h soak at 50k CCU, a clean pentest, every BENCH scene within budget on MIN and REF, 30/30 tools | 18–24 months | Every criterion with Ph ≤ 4 |
| **5 Ambition** | Beyond the bar | **M5 "Lattice":** seamless travel between systems, dynamic cell split/merge, a ≤ 1 s cell-crash hitch, a 100k-bot shard, player scripting, RT on SHOWCASE | 18–30 months | SRV Ph5, REN-6 (RT) |

**The critical path to M1** is schema → ECS → world model → replication → prediction → gameplay → content.
The rendering lane is near-critical because of the GPU-terrain contract for BENCH-2. From Phase 2 the critical
path is networking scale. The Phase 4 exit is also gated by human-supplied evidence (H1 GPU lab, H6
playtesters, H7 pentest), which the loop cannot shorten. **The next ten work packages** are listed in
[09 §8.2](plan/09-roadmap-and-process.md#82-next-10-work-packages-in-execution-order). The first are the CI
matrix, the layering and licence lints, core/math completion, and `helios-schemac`.

### 8.2 The build loop ([09 §5](plan/09-roadmap-and-process.md#5-the-build-loop))

- **Roles:**
  - a **Director** plans rounds from the scorecard and holds module locks;
  - one **Implementer** per work package works in its own branch and build directory;
  - an **Adversarial reviewer** (a fresh agent with no implementer context) writes failing tests and can
    block;
  - an **Integrator** runs the merge queue;
  - **Auditors** score every round and phase;
  - the **User** validates milestones on Windows and supplies the human-gated items H1–H8.
- **A round:**
  1. Select ready work packages whose criteria fail, by phase, then critical-path slack, then risk.
  2. Land interface-only PRs first for shared contracts.
  3. Implement and self-check on GCC, Clang, MinGW and ASan.
  4. Adversarial review.
  5. Run the CI PR tier: MSVC, clang-cl, GCC, Clang, MinGW, headless, Go on Windows and Linux, lavapipe
     goldens, a 16-bot smoke test.
  6. Rebase and verify in the merge queue.
  7. Squash-merge.
  8. Score in the nightly run.
- **Definition of done:**
  - tests that fail without the change (the reviewer checks by reverting);
  - warning-clean on all five compilers;
  - lints pass;
  - budgets are benchmarked;
  - every criterion is registered in `scorecard.jsonc`;
  - docs and ADRs are updated.
- **Parallelism:** at most six implementers in flight, one queue, and module locks on public headers. It
  scales up only while review rejection stays below 30 % and the PR tier below 45 min.
- **Regression control:**
  - green criteria ratchet into the blocking set;
  - a nightly failure starts a bisect agent;
  - flaky tests count as failing after 7 days;
  - auditors mutation-test samples of merged code to catch test gaming.
- **Termination:** the loop stops at the **Phase 4 exit**. Every Ph ≤ 4 criterion must be green on Windows and
  Linux, two independent auditors must score ≥ 9/10 on closeness to the user's goal, and the user must accept.
  Phase 5 runs only if the user opts in.

---

## 9. The AAA Scorecard and how progress is measured

The scorecard in [01 §3](plan/01-vision-and-scope.md#3-the-aaa-bar-measurable-acceptance-criteria) holds **60
criteria in eight families**. Each criterion has a phase, and all of them are measured on named hardware:

| Family | # | Examples (phase) |
|---|---|---|
| Rendering (REN) | 7 | REF 1440p High: BENCH-1/2/4/5 ≥ 60 fps with p99 ≤ 20 ms and BENCH-3 ≥ 45 fps (4); MIN 1080p Low upscaled ≥ 60 fps (4); zero PSO hitches > 50 ms in 30 min (3); 10¹³ m jitter test and seamless BENCH-2 (1); goldens every commit (0) |
| Server scale (SRV) | 12 | Players per cell 50 → 500 → 1,000; CCU per shard 50 → 2k → 5k → 20–50k → 100k+; handoff p99 < 100 ms (3); ledger 2k → 10k → 50k tx/s; 2,000-ship battle at TiDi ≥ 10 % (4); cell-crash hitch ≤ 10 s (3) → ≤ 1 s (5) |
| Iteration (ITR) | 8 | Save → visible in PIE ≤ 2 s (1–2); editor open ≤ 10 s (2); PIE ≤ 15 s (1); `.cpp` edit → relinked ≤ 30 s (1); clone → in-game ≤ 30 min (2); co-edit ≤ 1 s (3); live hotfix ≤ 15 min (4) |
| Stability (STB) | 6 | ≤ 1 client crash per 1,000 play-hours (4); journal loses ≤ 1 transaction (2); `kill -9` chaos creates or destroys no value (3); 72 h soak at 50k CCU (4) |
| Content (CNT) | 7 | Systems ≥ 10¹¹ m, planets up to 6,400 km (1/3); game thread never blocks > 1 ms on I/O (3); memory ceilings (3–4); 100k records in ≤ 60 s (3); patch ≤ 1.5× changed bytes (2) |
| Security (SEC) | 8 | Schema lint on every client message (1); every value move through the idempotent ledger (2); speedhack flagged ≤ 2 s (2); no server-only data in client cooks (1); fuzzing ≥ 24 CPU-h (3); external pentest clean (4) |
| Tooling (TOOL) | 6 | Tool MVPs (1); P0 tools (2); 26/30 AAA-complete (3); 30/30 (4); **designer day**: a newcomer adds a hull, a weapon, a quest and a vendor in ≤ 1 day without an engineer (3) |
| Platform (PLT) | 6 | Every commit on MSVC, clang-cl, GCC, Clang and MinGW (0); backend on Windows without Docker (0); smoke tests on Win 10/11 and Ubuntu (1); cross-compiler bit-identical PCG (1); Linux within 10 % of Windows (4); signed installer and AppImage (2–3) |

**Below the scorecard**, each section defines its own automated criteria, and 09 maps them onto work packages:
- RT-01…17: runtime;
- RC-1…11: rendering;
- NS-p.k: networking, per phase;
- BE-A1…A14: backend;
- GP-1…9: gameplay;
- ED-1…13: editor;
- CL-1…22: client and launcher.

**How progress is measured** ([09 §5.6–5.8](plan/09-roadmap-and-process.md#56-ci-tiers-and-evidence-classes)):
- **`scorecard.jsonc`** maps every criterion to its tests, evidence class, platforms and threshold. The
  nightly report shows every criterion's state and trend.
- **Evidence classes** decide how a criterion can be shown to pass. None of them relaxes a threshold.
  - **N**: three consecutive nightly passes on Windows and Linux.
  - **H**: the same on lab hardware. With no lab, the criterion is *unmeasured*, which counts as failing.
  - **W**: long-running soaks, with two scheduled passes.
  - **M**: manual or external evidence, such as the designer day, the pentest and legal sign-offs, as a
    signed record.
- **A phase is complete** when every criterion with Ph ≤ N passes, two independent auditors score **≥ 9/10**,
  no critical or high risk is open without an accepted mitigation, and the user has validated on Windows.
- **The per-round auditor score** is 60 % the fraction of passing Ph ≤ N criteria and 40 % a rubric: the
  reference-class proofs are playable, code health, documentation and platform parity. Each report lists the
  top five failing criteria.

---

## 10. Top ten risks

From the 31-entry register in [09 §7](plan/09-roadmap-and-process.md#7-risk-register). L/I is likelihood and
impact.

| # | Risk | L/I | Mitigation |
|---|---|---|---|
| K1 | Effort exceeds capacity (450–730 person-years to the bar) | H/H | Phase gates, seams first, a thin slice every phase; cut Phase 5 scope first, then polish, never MVP items |
| K24 | Agent quality drift, test gaming, architectural erosion | H/H | Adversarial review, tests that must fail without the change, mutation-sampled audits, layering lints, auditors at every phase |
| K7 | No real GPU or server hardware, so H-class criteria cannot be measured | H/H | The user's Windows PC as a self-hosted GPU runner from Phase 0; lab purchase by mid-Phase 1; "unmeasured" counts as failing |
| K23 | Art, animation and VO bottleneck (150 → 2,000 assets per phase) | H/H | Procedural and kitbash content, CC0 assets with provenance, contract artists; content scoped to proving capabilities |
| K29 | Editor widget cost and iteration regressions | H/M | Widget library first under one owner, 25 % reserve, nightly editor-performance CI |
| K2 | flecs fails the 50k-entity benchmark or fragments | M/H | Phase 0 pre-benchmark; flecs confined to `engine/ecs`; a custom ECS behind the same API |
| K4 | Cross-compiler determinism drift (physics, PCG, HXL, replay) | M/H | `det::` math, no FP contraction, fixed-point integrators, five-compiler hash tests on every PR |
| K13 | Handoff duplicates or loses authority groups | M/H | Type-enforced `GhostRef`, mutation only through effects, the fence, torture bots, conservation audits |
| K28 | 2,000-ship fan-out beyond what TiDi absorbs | M/H | Command replication, volley aggregation, fleet proxies, 1k-ship bots from Phase 2 |
| K17/K18 | SWG terrain patents; Improbable's replication-layer patent | L–M/H | Generic node-graph terrain; counsel review before any Phase 4 release and before WP-5.2; v1 does not need the Phase 5 layer |

---

## 11. Building and running today (Windows and Linux)

**Current state (2026-09-25).**
- **Builds today:** the vendored third-party tree, `engine/core` (platform layer, memory, jobs, VFS, CVars,
  crash handling and more) and `engine/math` (vectors, frames, cube-sphere mapping, deterministic noise).
- **Tests:** a headless GCC 13.3 build of the working tree is warning-free. `core_tests` passes 133/133 test
  cases. `math_tests` passes 101/102; one noise-statistics check fails in the in-flight working tree.
- **Not yet present:** no executables, and no `services/` Go module; the Go CI job skips until `go.mod` exists.
- **CI** (GitHub Actions) builds and tests Windows MSVC and Linux GCC/Clang on every push. WP-0.1 adds
  clang-cl, MinGW, ASan and nightly jobs, and moves CI from Go 1.24 to 1.27.1.

**Windows (primary).** Install Visual Studio 2022 with "Desktop development with C++" and "C++ CMake tools".
These provide CMake and Ninja; the top-level `CMakeLists.txt` needs CMake ≥ 3.28. Also install Git for Windows
with LFS. Then, in an **x64 Native Tools Command Prompt for VS 2022**:

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release -LE gpu
```

- **Debug build:** `windows-msvc-debug`.
- **clang-cl:** `windows-clang-cl` (build preset only; run tests with
  `ctest --test-dir build\windows-clang-cl`).
- **Visual Studio solution:** `cmake --preset windows-vs2022`, then open the solution under
  `build\windows-vs2022\`. Run its tests with `ctest --test-dir build\windows-vs2022 -C RelWithDebInfo`.
- **Runtime:** binaries use the static CRT (`/MT`), so no VC++ redistributable is needed.
- **Later:** once the backend lands, install Go 1.27.1 and run
  `cd services && go build -o ..\build\go\ ./cmd/...`. Milestones are then validated with
  `tools\milestone\validate.ps1 -Milestone M<n>` ([09 §5.9](plan/09-roadmap-and-process.md#59-how-the-user-validates-milestones-on-windows)).

**Linux.** You need GCC 13+ or Clang 17+, CMake ≥ 3.28 and Ninja. On Ubuntu 24.04, SDL3 also needs the X11,
xkbcommon, D-Bus and udev development packages listed in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Then:

```sh
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc
```

- **Clang:** `linux-clang`.
- **Sanitizers:** `linux-debug-asan` (Clang with ASan and UBSan).
- **Servers and tools only:** `linux-headless` (`HELIOS_BUILD_GRAPHICS=OFF`; test with
  `ctest --test-dir build/linux-headless`).
- **GPU tests**, once rendering lands, run on software Vulkan (lavapipe) under `xvfb-run -a` and carry the
  CTest label `gpu`.
- **Windows portability check without Windows:** `cmake --preset cross-mingw && cmake --build --preset cross-mingw`.
- **Parallel agents** each use their own build directory (for example
  `cmake -S . -B build/<task> -G Ninja`), per [`CLAUDE.md`](../CLAUDE.md).

**Offline.** Every dependency is vendored as source and pinned in
[`third_party/MANIFEST.md`](../third_party/MANIFEST.md). A build never touches the network. Slang is the one
exception: it is a SHA-256-pinned prebuilt, fetched by a bootstrap when the shader toolchain lands.

---

## 12. Document map and glossary

| Document | Purpose |
|---|---|
| `docs/PLAN.md` (this file) | Entry point and summary |
| [`plan/README.md`](plan/README.md) | Section index and the **phase vocabulary** used everywhere |
| [`plan/00-decisions.md`](plan/00-decisions.md) | Binding architecture decisions (ADR-001…014) |
| [`plan/01-vision-and-scope.md`](plan/01-vision-and-scope.md) … [`plan/09-roadmap-and-process.md`](plan/09-roadmap-and-process.md) | The ten plan sections (§6 above) |
| [`plan/CONSISTENCY.md`](plan/CONSISTENCY.md) | Log of every cross-section consistency fix, plus the out-of-scope follow-ups |
| [`plan/_integration-notes.md`](plan/_integration-notes.md) | Cross-section issues raised by section authors (all resolved) |
| [`research/`](research/) | The ten research reports, R01–R10 |
| [`../CLAUDE.md`](../CLAUDE.md) | Contributor and agent rules: platforms, build, layout, code style, quality gates, IP hygiene |
| [`../third_party/MANIFEST.md`](../third_party/MANIFEST.md) | Vendored dependencies, pins and licences |
| [`../engine/core/README.md`](../engine/core/README.md), [`../engine/math/README.md`](../engine/math/README.md) | APIs and threading rules of the landed modules |

**Glossary.** [01 §6](plan/01-vision-and-scope.md#6-glossary) defines shard, zone, cell, gateway, orchestrator,
authority group and epoch, ghost, reference frame and grid, `Reparent`, object container, record and record
template, ECS archetype and reference archetype, edit and play instances, instance, layer and phase, activity,
ledger, tick profile, Foundation layer, and BENCH/Scorecard.

**Citations.** R01…R10 are the research reports. `R0n-P0-k` is requirement *k* of report *n* (01 §7). `AAA-*`
IDs are scorecard criteria. W/M/G/S/R + two digits are capability IDs (01 §2.6). T01–T30 are editor tools
(07). WP-p.n are work packages, K1–K31 are risks, and H1–H8 are human-gated inputs (09).
