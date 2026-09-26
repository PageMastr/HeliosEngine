# Helios: Master Plan

*Start here. This document summarizes the Helios master plan and links to everything else. The detail lives
in the ten plan sections under [`plan/`](plan/README.md), and the evidence behind them lives in ten research
reports under [`research/`](research/). Where this summary and a section disagree, the section wins, and
[`plan/00-decisions.md`](plan/00-decisions.md) (the Architecture Decision Record) wins over both.*

**Status:** approved in review round 5 (2026-09-25) with minor revisions, which have been applied (plan
revision 6; [§13](#13-review-record)). Phase 0 (Foundations) is in progress. Two Phase 0 risk triggers have
fired: RT-01's structural-ops pre-bench (K2, [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md) open) and
RT-13's Luau fuel metering (K39).

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
   telemetry, GM tools, patch service), which studios extend with sandboxed Luau world scripts rather than Go
   code.

Windows 10/11 x64 is the primary platform for development and play. Linux x64 builds and passes the same tests
on every commit, and it is the production OS for servers.

**The goal.** The user asked for an engine at an AAA standard that "could build a game like Star Wars
Galaxies, Destiny, Star Citizen", EVE Online or SWTOR. The plan turns that into a testable target. A studio of
30–150 people should be able to build and run an original MMO of any of those five classes, or a hybrid of
them in one shard, by writing records, Luau scripts and domain graphs, never by modifying engine or backend
source ([01 §1](plan/01-vision-and-scope.md#1-product-vision)). "AAA" is not left as an adjective. It is
defined as **65 measurable criteria**, the *AAA Scorecard*. The criteria cover rendering performance on named
hardware, server scale, iteration speed, stability, content scale, security, tooling completeness and platform
parity, and they are measured in six scripted benchmark scenes. The bar is met when every criterion tagged
Phase ≤ 4 passes on Windows and Linux.

**How we get there.**
- **Six phases.** Phases 0–4 lead to the AAA bar and Phase 5 goes past it. Each phase ends with a playable
  demo of *Cinder Reach*, an original sample game that proves the five game classes one capability at a time.
- **An agent build loop.** A team of agents always works the highest-priority failing scorecard criterion.
  Every change passes adversarial review and an eight-job CI matrix (both supported Visual Studio toolsets,
  clang-cl, GCC, Clang, headless and MinGW). Independent auditors must score each phase exit at **≥ 9/10**, and
  the user validates each milestone on Windows.
- **Humans judge what bots cannot.** A blinded external panel rates look and feel against current reference
  titles (AAA-REN-8), and a contracted content team with no engine engineer must build a playable zone with
  the shipped tools (AAA-TOOL-10). Passing every number while failing either still fails the bar.
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
- human playtesters, an external look-and-feel panel and a contracted content team;
- a code-signing certificate;
- a penetration test;
- patent counsel.

Until Phase 0 yields velocity data, any calendar for an agent-built Helios is unknown, and the plan says so
rather than guessing.

**Where we are.** After five independent review rounds the plan was approved, in round 5, with minor
revisions, which have been applied ([§13](#13-review-record)). Its cross-section fixes are logged in
[`plan/CONSISTENCY.md`](plan/CONSISTENCY.md).

The repository's state, as the lead verified it on 2026-09-25:
- **Committed**, with the GCC and Clang tests passing, the MinGW cross-builds linking and MSVC in CI:
  - `engine/core` (141 tests) and `engine/math` (104);
  - `helios-schemac` and `engine/reflect` (121);
  - the ECS on flecs (90);
  - the Luau host (79);
  - the Vulkan and Null RHI (51);
  - the HTP transport (84);
  - the Go backend skeleton (124);
  - most of the CI matrix.
- **In progress:**
  - the cell and gateway servers;
  - the gameplay kernel with HXL;
  - the render graph and shader tools;
  - the layering and licence lints, the CPU gate, and core completion.
- **Not started:** the launcher, client, editor, patch pipeline, physics and PCG, and the sample content.

Two measured gates fail, and their risks have fired:
- **RT-01's structural ops (K2).** The fix is optimizing the ECS wrapper first
  ([ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md)).
- **RT-13's Luau fuel metering (K39).** The fix is two vendored Luau patches.

Rework work packages bring earlier in-tree code up to the current plan:
- WP-0.15r, for the backend;
- WP-0.2r and WP-0.5r, for the ISA and CPU-gate code;
- WP-0.10r, for the script host.

Everything else is specified with work packages, owners, dependencies and acceptance tests
([09 §8](plan/09-roadmap-and-process.md#8-current-status-and-next-steps)).

---

## 2. What the user asked for, and where the plan covers it

| The request | How the plan answers it | Where |
|---|---|---|
| "A full modern MMO backend" | **C++:** a gateway tier (encrypted UDP, connect tokens) and headless cell servers (Shard → Zone → Cell), with authority groups and epoch-fenced handoff, bounded co-location of coupled entities that straddle a cell seam (tows, tethers, dogfights), planned region migration so rolling restarts and drains cost a ≤ 1 s hitch per region moved and lose no state (Phase 3), a replicant state tier so a cell crash in any persistent world zone costs ≤ 1 s of state and no disconnect (Phase 4), make-before-break gateway relocation so gateway deploys leave no session linkdead (Phase 4), interest management, time dilation, overload policy and an Opus voice forwarder. **Go:** 23 services, including an append-only double-entry **item and currency ledger** (with housing plots and lots), regional order-book markets, durable timers (including structure upkeep and decay), write-behind persistence with a budgeted fence load, world state (flags, territory and sovereignty), chat, social, activities (parties and player fleets, a matchmaker, group finder, leaderboards, ledger-guarded lockouts), world scripts for studio-defined cross-zone systems (player-city governance is one), telemetry, player reports and support cases with SLA targets, a store API with minors' spending caps, GM/admin and patch manifests. **Stores and ops:** PostgreSQL, Valkey and NATS JetStream; Kubernetes + Agones fleets; failure detection by failure domain, so a host or rack loss is recovered as crashes and never freezes the shard; client and server content parts under a compat epoch, so server hotfixes need no client patch; CDC patching from a CDN; GDPR/CCPA erasure by crypto-shredding, per-store RPO/RTO with regional DR, and on-call runbooks | [04](plan/04-networking-and-servers.md), [05](plan/05-backend-services.md), [ADR-007/008](plan/00-decisions.md#adr-007-server--network-architecture) |
| "An editor that contains all tools needed to build a fully functional game" | A UI-less **ToolsFramework** (documents, commands, property-path transactions, crash journal, automation) under an ImGui shell, with **30 tools, T01–T30**. Every record type is editable in the generic inspector from day one. Play-in-Editor runs a real cell server, clients and bots. Studios extend the editor through a public, versioned **extension SDK** (panels, documents, importers, cook steps, viewport modes, rules and node libraries in C++ or Luau; ED-19), and designers add their own record, component and view-model types in T08's schema editor with no compiler (ED-22). Teams co-edit live in per-zone sessions plus a project-wide data session for records with no zone. World scripts run in every PIE mode and are debugged and inspected in a World scripts panel (07 §1.6.2, ED-23). At the Phase 4 exit a contracted content team with no engine engineer must build a playable zone with them in ≤ 2 weeks and zero blocking issues (AAA-TOOL-10) | [07](plan/07-editor-and-tools.md) |
| "Level editors" | T01 World, T02 Prefabs, T03 Blockout, T04 Terrain & Planet (cube-sphere, layer stack), T05 Scatter, T06 Splines, T07 Star System & Galaxy | [07 §2.1](plan/07-editor-and-tools.md#21-world-building) |
| "Scripting" | **Luau** (typed, sandboxed, hot-reloaded in ≤ 2 s) on client, cell server and editor, and in backend world-script hosts for shard-wide systems (05 §1.23), which PIE runs with hot reload and a debugger per partition (07 §1.6.2). A DAP debugger works in VS Code, and luau-lsp gets generated type definitions. Domain visual graphs compile to Luau, native ability state machines or HXL formulas (T10, T11) | [02 §7.4](plan/02-engine-runtime.md#74-scripting-host-luau-0739-r01-p0-4-r08-ed-p0-10), [06 §11](plan/06-gameplay-framework.md#11-scripting-api-graphs-and-determinism), [07 T10–T11](plan/07-editor-and-tools.md#22-data-and-gameplay) |
| "Object editors" | T08 Data & Record-Template Editor (inspector, a 100k-row spreadsheet grid, and a governed schema editor for new project types and world-script blocks), T09 Gameplay Systems (attributes, effects, abilities, loot, crafting, fitting sandbox), T21 Modular Assembly (ships, vehicles, stations, housing), T22 Character Customization | [07 §2.2–2.4](plan/07-editor-and-tools.md#22-data-and-gameplay) |
| "Animation" | ozz-animation runtime with anim graphs, IK, cross-species retargeting and crowd LOD. **T15 Animation Suite** (including mount and rider sets), T14 Cinematic Sequencer, and in Phase 4 motion matching plus a FACS facial runtime with viseme lip-sync (RT-22) | [02 §7.2](plan/02-engine-runtime.md#72-animation-ozz-017-r06-eng-28-r08-ed-p1-08), [07 T14–T15](plan/07-editor-and-tools.md#23-presentation) |
| "Everything you would need to build an MMO game" | Also: materials (T16), VFX (T17), environment/post (T18), UI designer (T19), audio (T20), AI/navigation/spawns (T23), assets (T24), localization (T25), quests and activities (T12, with encounter, lockout and match-rule editors and a queue simulator), dialogue with VO workflow (T13), profiling (T26), GM/live-ops (T27), validation (T28), build/cook/deploy (T29), live collaboration (T30). **Engine as a product:** generated API and record reference, tool manuals and executable tutorials, a Project Browser with five starter templates (one per reference class), a binary SDK and `helios-tool upgrade-project` (AAA-TOOL-7…9) | [07 §2](plan/07-editor-and-tools.md#2-the-tool-suite-t01t30), [09 §2.7](plan/09-roadmap-and-process.md#27-developer-experience-track-helios-as-a-product) |
| "Client" | Engine + RmlUi game UI + netcode. A table-driven state machine, one continuous camera from cockpit to on-foot, data-bound HUD, diegetic cockpit screens, rebindable input (mouse, gamepad, HOTAS, IME), accessibility, crash reporting, sandboxed UI addons. 29 Foundation UI panels cover every player-facing system, including the store, player reports and support tickets, each with a headless flow test, and reskin by theme with no engine edit (CL-23) | [08 §1](plan/08-client-and-launcher.md#1-client-application) |
| "Launcher" | A C++ launcher (RmlUi on `SDL_Renderer`). It gates CPU and GPU (the gate itself runs on any x86-64 CPU, CL-17), logs in, patches content-addressed chunks from a CDN with signed manifests, and supports verify/repair, self-update, play-while-downloading and pre-download, plus a per-user installer with no UAC. Every path, URI scheme, registry key and signing key is scoped by the project's product ID, so two Helios games install side by side (CL-24) | [08 §2](plan/08-client-and-launcher.md#2-launcher-and-patcher) |
| "At a AAA style standard" | The **AAA Scorecard**: 65 criteria on five reference hardware tiers and six benchmark scenes, including a blinded external look-and-feel panel against current reference titles (AAA-REN-8). The Phase 4 exit is the bar | [01 §3](plan/01-vision-and-scope.md#3-the-aaa-bar-measurable-acceptance-criteria), §9 below |
| "Could build a game like Star Wars Galaxies, Destiny, Star Citizen", EVE, SWTOR | A **52-capability matrix** scored against the five reference game classes, with an owning section and a phase for each capability, and a *Cinder Reach* proof for each class | [01 §2](plan/01-vision-and-scope.md#2-reference-game-archetypes), §7 below |
| "Deploy a team of agents to research …" | **Ten research reports**: EVE and Carbon, SWG and its emulators, SWTOR and HeroEngine, Star Citizen, Destiny, modern engines (Unreal, Godot, Unity DOTS, O3DE, Bevy, Frostbite, id Tech), MMO backends, editors, sci-fi gameplay and graphics, and technology selection | [`research/`](research/), §3 below |
| "Put together a plan for how you will build this" | This document, the ADR, and sections 01–09, including a work-package roadmap with owners, dependencies and acceptance tests | §6, §8 below |
| "Run on a loop until the Engine is at a AAA style standard" | The **build loop**: director, implementers, adversarial reviewers, integrator and auditors, iterating on failing criteria. It terminates at the Phase 4 exit | [09 §5](plan/09-roadmap-and-process.md#5-the-build-loop) |
| "Have a review agent … rate it a 9 out of 10" | This plan goes to an independent review agent and is not final until it scores ≥ 9/10. The build loop applies the same bar at every phase exit, with two independent auditors | [09 §5.7](plan/09-roadmap-and-process.md#57-termination-condition-and-independent-scoring) |
| "Run this on Windows or Linux … Windows is the preference … compatible/buildable for both" | Windows-first: VS 2026 is the primary toolset, and VS 2022 17.14 is the tested floor that builds the SDK and every released binary, so both work; Visual Studio solution presets exist for both. Linux parity (GCC 13+, Clang 17+) and a MinGW portability check. The whole backend runs on Windows **without Docker**. Six scorecard criteria (AAA-PLT-1…6) enforce parity | [ADR-001](plan/00-decisions.md#adr-001-platforms), §11 below |

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

Retrofitting all four onto CryEngine explains most of its decade-long delay. SC shipped the replication
layer (Replicant, Atlas, Scribe and Gateway) with Persistent Entity Streaming in Alpha 3.18 and static server
meshing on it in Alpha 4.0 (December 2024), so a game-server crash no longer disconnects players; only
dynamic meshing is still in progress. Other lessons: server tick rate is the real metric, persistence needs
garbage collection, split authority is a programming model, and bugs live at seams. The report also shaped
the stack: keep the gateway and replication in C++ (not Go), keep NATS off the per-tick path, and write
entities through a write-behind service.
*Adopted:* all four seams from day one ([ADR-005](plan/00-decisions.md#adr-005-world-model-large-worlds),
[ADR-007](plan/00-decisions.md#adr-007-server--network-architecture)), a Phase 4 replicant state tier that
matches SC's shipped crash recovery, entity budgets and lifecycle policies, bots that cross every boundary
type, and master modes as data.

**[R05 — Destiny and action MMOs](research/05-destiny-action-mmo.md).** Destiny **splits authority by latency
tolerance**. Own-avatar feel is local, physics and AI run on a simulation host, activity progress lives in
durable cloud services, and loot and progression live in backend services. A simulation-host crash therefore
never costs progress. The Tiger renderer's **extract → prepare → submit** pipeline simulates frame N while
rendering N−1. The warnings: client authority is debt (cheating in Destiny, The Division and PlanetSide 2;
New World's dupes), sequential RNG seeds biased perk rolls, and an 8-hour map load for a half-second change
slowed content for years.
*Adopted:* the Overwatch model (server-authoritative, predicted, lag-compensated) instead of client authority,
a durable activity service outside the cell (with matchmaking, checkpoints and ledger-guarded lockouts),
predicted abilities compiled to a rollback-safe native state machine, salted and audited RNG with statistical
CI tests, and hot reload in ≤ 2 s as a scorecard criterion.

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
 └──────────┬──────────────────────────────┘     (HTP trunk, never the NATS bus)
            │ HTP trunk: every-audience state stream (persistent world zones, Phase 4)
 ┌──────────▼──────────────────────────────┐
 │ Replicants (helios-cell --replicant)    │ latest AG state in RAM; a standby cell resumes a
 │ one per ≤ 4 cells, no simulation        │ crashed cell's region from it (≤ 1 s of state lost)
 └─────────────────────────────────────────┘

 Data:   PostgreSQL 18 (global · per-shard primary · separate checkpoint cluster) · Valkey (sessions, queue,
         presence; always rebuildable) · NATS JetStream (events, timers, audit) · ClickHouse · S3-compatible
         object storage (backups, CDN origin, crash dumps)
 Fleets: LocalProcessPlacer on a dev PC → VMs (Phases 0–2) → Kubernetes + Agones (Phase 3+)
 Voice:  helios-voice forwarders behind the gateways relay Opus over HTP trunks (Phase 3, ADR-015)
 Script: helios-cell --role world-script hosts run studio world-script partitions and commit through the Go
         worldscript service (Phase 3, 05 §1.23)
 Editor: helios-editor ⇄ helios-assetd (import, cook, DDC, hot reload) ⇄ PIE cells + clients + bots
         (+ a world-script host, Phase 3) ⇄ collab service (edit instances, Phase 3)
```

**Invariants** ([04 §0](plan/04-networking-and-servers.md#0-design-rules),
[05 §0](plan/05-backend-services.md#0-principles)):
- **Clients.** They reach only gateways and HTTPS APIs, and they send intents, never outcomes.
- **Authority.** Every replicated entity belongs to one authority group, owned by one cell at one epoch.
- **Value.** It moves only through synchronous ledger calls that carry idempotency keys and fences.
- **The tick.** The simulation tick never waits on a service.

**Topology stages** ([04 §6.5](plan/04-networking-and-servers.md#65-staging-adr-007)):
- **v0 (Phases 1–2):** one cell per zone, with the handoff and ghost APIs present from the start.
- **v1 (Phase 3):** static multi-cell zones with handoff p99 < 100 ms. Each cell replicates its own entities to
  every session whose view reaches it, and gateways compose the view under one budget, so clients see across
  cell boundaries with no pop-in (04 §6.6, NS-3.10). Entities coupled across a seam (a tow, a tether, a
  close dogfight) are co-located on one cell in bounded sets, with effect-only fallbacks when a host is loaded
  (04 §6.2a, NS-3.12). Any zone's region can move to another process by planned migration, with a ≤ 1 s hitch
  per region moved and no state loss, which rolling restarts, drains and pre-provisioning use; a zone-wide
  migration hold is capped at 1 s (04 §6.7, NS-3.11).
- **v1.5 (Phase 4):** the replicant state tier. Every persistent world zone, single-cell or multi-cell, streams
  its authority groups' state to replicants, so a cell crash loses ≤ 1 s of state with no disconnect and a
  ≤ 10 s hitch (AAA-SRV-9/12). Gateway deploys move each session make-before-break to another gateway, with no
  session linkdead (05 §6.3.1, AAA-STB-6).
- **v2 (Phase 5):** dynamic split/merge, plus a gateway replication layer on the replicant tier that makes
  cell crashes invisible to players (≤ 1 s hitch).

On a Windows dev box, `helios-backend.exe` runs every Go service in one process, with embedded PostgreSQL,
embedded NATS and miniredis, and no Docker.

### 4.2 Module layering (a strict DAG, enforced at configure time)

```
L5 apps       helios-client   helios-editor   helios-cell   helios-gateway   helios-voice   helios-launcher
              tools (schemac · shaderc · assetd · cook · pack · fitsim · bot · helios-tool · rendertest)
                    │               │              │              │                │
L4 framework  world* · gameplay* · assembly* · replication* · netgame* · authority* · clientcore*
              presentation · assetpipe(ED) · toolsfw(ED) · editorui(ED) · edtools/*(ED)
L3 servers    physics* · anim* · nav* · pcg* · script* · net*   │   audio · voice · text · ui · rhi · render
L2 foundation reflect* · hxl* · asset* · records* · ecs* · loc* · telemetry* · replay* · patch* · crash*
              │  app · input
L1 core       core* · math*

 *  = HEADLESS: linked by cell servers; never reaches app, input, rhi, render, audio, voice, text, ui,
      presentation
 ED = EDITOR_ONLY: never linked by client, cell or gateway
```

**Rules** ([02 §1.1](plan/02-engine-runtime.md#11-layering-dag-r06-eng-01)):
- A module depends only downward, or on a same-layer module it lists explicitly.
- Configuration fails on a cycle, on a HEADLESS leak or on an EDITOR_ONLY leak.
- No third-party type appears in a public header, which keeps a custom-ECS fallback possible.
- ISA levels ([02 §1.1](plan/02-engine-runtime.md#11-layering-dag-r06-eng-01)): every runtime image (client,
  cell, editor, gateway, tools) is built for AVX2 as a whole, except one CPU-gate TU that runs before any other
  code in the image, third-party pre-`main` hooks such as mimalloc's and Tracy's included: the first TLS
  callback on Windows, the `.preinit_array` entry on Linux. The SDK holds studios' game modules to the same
  level (ADR-001a rule 1). The launcher, its bootstrap and the gate TU are built for baseline x86-64-v1 (SSE2),
  and their disassembly is audited, with wider code allowed only in listed CPUID-dispatched functions. The launcher and
  the client's CPU gate therefore run on any x86-64 CPU and can explain an unsupported one; emulator runs on
  pre-SSE4.2 CPUs (Core 2, Phenom) check this ([08 §2.1.1](plan/08-client-and-launcher.md), CL-17).

`render` defines the `RenderScene` packet, and `presentation` fills it at extract. The render thread never
reads the ECS.

**What links what** ([02 §1.3](plan/02-engine-runtime.md#13-build-targets)):

| Binary | Links |
|---|---|
| Client | The runtime plus graphics, audio, UI, `presentation` and `clientcore`. No shader compiler and no ImGui in shipping builds |
| Editor | The client plus the EDITOR_ONLY modules. It loads Slang at runtime and hot-reloads game and editor-extension modules through the dev link model (three engine shared libraries, ADR-016); the SDK's editor, `helios-tool`, `helios-assetd` and `helios-cook` are dev-flavour builds for that reason |
| Cell | Only HEADLESS L1–L4 modules (`HELIOS_BUILD_GRAPHICS=OFF`). The same binary runs as a world-script host with `--role world-script` (Phase 3) and as a replicant with `--replicant` (Phase 4) |
| Gateway | Only `core`, `reflect`, `net` and `telemetry` |
| Voice forwarder | Only `core`, `net` and `telemetry` (Phase 3) |

---

## 5. Key decisions

All decisions are binding. Changing one needs a new ADR entry with its rationale.

| ADR | Decision | Main reason |
|---|---|---|
| [001 Platforms](plan/00-decisions.md#adr-001-platforms) | Windows 10/11 x64 primary. MSVC: VS 2026 (v145) is primary, and VS 2022 17.14 (MSVC 14.44) is the CI-tested floor and the release toolset for the SDK and all shipped binaries; the SDK also holds consumers to the `avx2` ISA level. Plus clang-cl. Linux x64 fully supported (GCC 13+, Clang 17+) and the production server OS; MinGW cross-build check; no macOS/consoles yet | User requirement; RHI seam keeps other platforms possible. MSVC's newer-linker rule means only floor-built SDK libraries work with both Visual Studio versions |
| [002 Languages](plan/00-decisions.md#adr-002-languages) | C++20 for engine, editor, client, launcher, servers and tools; Go 1.27.1 for backend services; **Luau** for gameplay scripting; domain graphs; predicted abilities compile to a native rollback-safe format | Luau: sandboxing, gradual typing, native codegen (R01/R03/R05/R06); Overwatch Statescript lesson (R05) |
| [003 Rendering](plan/00-decisions.md#adr-003-rendering) | Vulkan 1.3 via volk + VMA; Null backend; D3D12 seam with a Phase 4 gate; render graph; extract → prepare → submit; GPU-driven; clustered forward+; reverse-Z camera-relative; **Slang** shaders | Frostbite, Destiny and id Tech patterns (R05, R06, R09) |
| [004 Entity model](plan/00-decisions.md#adr-004-entity-model--reflection) | **flecs 4.1.6** archetype ECS everywhere; one `.hschema` generates C++, Go, Luau, SQL, replication and editor metadata; stable 64-bit IDs (runtime IDs from PG-allocated time-prefixed blocks); "record template", never "archetype", for content. **Open: [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md)**. RT-01's structural ops failed the Phase 0 pre-bench because of wrapper overhead, so the wrapper is optimized first; a custom ECS comes only if the optimized wrapper still fails at the Phase 1 gate | One model and one schema (R06, R08) |
| [005 Large worlds](plan/00-decisions.md#adr-005-world-model-large-worlds) | Nested frames with f64 `WorldPos` and `Reparent()`; Jolt double precision, one `PhysicsSystem` per grid; a 10¹³ m no-jitter test | SC's retrofit lesson (R04) |
| [006 Content](plan/00-decisions.md#adr-006-content-model) | Object containers (text source, one entity per file); typed record DB with hash IDs; import → cook with a DDC; CDC-chunked paks; deterministic PCG | SC OCS, EVE FSD, Destiny, SWG (R01–R05, R07) |
| [007 Server/network](plan/00-decisions.md#adr-007-server--network-architecture) | Client ↔ gateway ↔ cells; Shard → Zone → Cell; per-zone 1–60 Hz tick with TiDi; single-writer authority groups with epoch fencing; v0 → v1 (with planned region migration) → **v1.5 replicant state tier for every persistent world zone (Phase 4)** → v2; Tribes/Iris replication; five-stage overload policy | R02, R04, R07; SC's shipped replication layer and static meshing set the Phase 4 crash-recovery bar |
| [008 Backend](plan/00-decisions.md#adr-008-backend-services) | Go services; NATS + JetStream; PostgreSQL as system of record and Valkey as cache; one local binary on Windows; reason code on every value movement; studios extend the backend with sandboxed Luau **world scripts**, not Go plugins (round-3 amendment) | R07, R01; Go plugins do not work on Windows |
| [009 Editor](plan/00-decisions.md#adr-009-editor) | ImGui docking over a UI-less ToolsFramework; PIE on a real cell; HeroEngine-style edit instances; T01–T30 | R03, R06, R08 |
| [010 Client, UI, launcher](plan/00-decisions.md#adr-010-client-game-ui--launcher) | Retained-mode, data-bound game UI (RmlUi); ImGui for editor and debug only; C++ launcher on RmlUi + `SDL_Renderer`; self-installing per-user installer | SWTOR UI lesson (R03), R08 F1, R10 |
| [011 Core runtime](plan/00-decisions.md#adr-011-core-runtime) | Win32/POSIX platform layer; **SDL3 3.4.16**; job system with helping waits; mimalloc 3.5.3 tagged heaps; static `/MT` for shipped binaries (`/MD` in dev builds, ADR-016), `/Z7`; CMake ≥ 3.28; whole-image ISA levels: AVX2 images whose baseline CPU-gate TU runs before any other code in the image (the first TLS callback on Windows), and an x86-64-v1 launcher (round-3 and round-4 amendments) | R10; Jolt's inline AVX headers defeat per-file ISA flags, and mimalloc and Tracy run AVX2 code before `main` |
| [012 Quality](plan/00-decisions.md#adr-012-quality-bar--process) | CI on every toolchain; tested performance budgets; hot reload ≤ 2 s; IP hygiene | R05 iteration lesson |
| [013 Runtime libraries](plan/00-decisions.md#adr-013-runtime-libraries-r10-manifest) | Jolt 5.6.0 (deterministic), ozz 0.17, Recast 1.6, miniaudio, RmlUi 6.3 + FreeType/HarfBuzz, JSONC/yyjson, netcode 1.4.8 + reliable 1.4.5, nats.c 3.14, sentry-native 0.17.1, Tracy 0.14.1, libopus | Verified pins and licences (R10) |
| [014 Backend toolchain](plan/00-decisions.md#adr-014-backend-toolchain-r10-11) | Go 1.27.1, pgx, connect-go, nats.go, goose; **embedded-postgres** + embedded NATS + miniredis for Docker-free Windows dev; no CGO; SQLite only for tools | One SQL dialect (R10) |
| [015 Voice chat](plan/00-decisions.md#adr-015-voice-chat) | Opus over HTP (VOICE channel on UDP 7777) through a C++ `helios-voice` SFU-style forwarder; party, fleet, org, ship-intercom and proximity channels; server-side blocks; report-only 60 s evidence ring | One transport and crypto stack; proximity comes from cells anyway; libwebrtc rejected for the native client |
| [016 Dev vs shipping link model](plan/00-decisions.md#adr-016-dev-versus-shipping-link-model-game-module-hot-reload) | Shipping: monolithic static `/MT`. Dev (`HELIOS_MODULAR`, `windows-msvc-dev`/`linux-dev`): three engine shared libraries with `HELIOS_*_API` exports, `/MD` everywhere, incremental linking; reloadable game and editor-extension DLLs (`_edcore`, `_edui`); the SDK's editor and tools are dev-flavour; Phase 0 spike gates RT-14 | Game-DLL hot reload without split singletons or cross-heap frees; ≤ 30 s relink (AAA-ITR-5) |

---

## 6. The plan sections

**[00 — Decisions](plan/00-decisions.md).** The sixteen ADRs summarized in §5, plus ADR-001a, the Windows
toolset policy (VS 2026 primary; VS 2022 17.14 as the floor and release toolset, so one SDK serves both), and
the round-3 amendments: whole-image ISA levels (ADR-011) and world scripts as the studio's backend extension
surface (ADR-008). Round 4 amended three decisions: the CPU gate runs before any other code in the image,
third-party pre-`main` hooks included (ADR-011); the SDK holds consumers' game modules to the `avx2` level
(ADR-001a rule 1); and the replicant tier covers every persistent world zone, not only multi-cell zones
(ADR-007). Round 5 opened [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md) (`docs/adr/`): RT-01's
pre-bench failed on structural ops, so the flecs wrapper is optimized before any custom ECS is considered.
Every section and every work package must conform.

**[01 — Vision and scope](plan/01-vision-and-scope.md).**
- The six product parts, the north star and seven design pillars ("seams first, generality later", "budgets
  are features"). Dynamic project schema types and backend world scripts keep the north star ("records, Luau
  and graphs, never engine or backend source") true for new data types and cross-zone systems.
- The five reference game classes with their experiences, numbers and lessons, and the **52-capability
  matrix**. The SWG and EVE proofs build settlements, cities and territory through the gameplay rules (GP-16,
  GP-17), not only as rendering benchmarks.
- The **AAA Scorecard**: five hardware tiers (MIN, REF, SHOWCASE, DEV, SERVER), six benchmark scenes
  (BENCH-1…6; BENCH-3 is the client view of the same 2,000-ship battle the server gate measures) and 65
  criteria with evidence classes. Human-judged criteria include a blinded external look-and-feel panel
  (AAA-REN-8, at the Ph1, Ph3 and Ph4 exits) and a contracted content team that builds a zone with the shipped
  tools, declaring its own record, component and view-model types in T08, plus a UI-provenance report of how
  much content went through the editor UI (AAA-TOOL-10). Rolling restarts are bounded per region moved
  (AAA-STB-6: migration hitch p99 ≤ 1 s, no state loss), and gateway deploys relocate sessions make-before-break
  with 0 linkdead sessions. From Phase 4 a cell crash in any persistent world zone loses ≤ 1 s of state
  (AAA-SRV-9).
- *Cinder Reach*, the original reference content: five star systems and three factions, from about 150
  assets in Phase 1 to about 2,000 in Phase 5.
- Non-goals, IP and licence rules (permissive licences only; no Star Wars or other third-party IP; patent
  reviews), a "done means" table per phase, and the glossary.

**[02 — Engine runtime](plan/02-engine-runtime.md).**
- The L1–L5 module DAG, "gems" (plugins), build targets and memory budgets per hardware tier.
- The dev versus shipping link model (ADR-016): monolithic `/MT` shipping builds, and three engine shared
  libraries in dev builds so gameplay C++ modules live-reload in the editor and PIE, gated by a Phase 0 spike
  (RT-18).
- The job system, and the client and cell frame pipelines, including the three-thread client (OS, game and
  render threads) with a just-in-time input sample, and per-thread and per-system CPU budgets on REF and MIN
  at every gated frame rate: 60, 45 (BENCH-3), 30 (MIN BENCH-3/5) and 120 fps (Performance mode). RT-12's MIN
  and 30/45 fps clauses gate at the Phase 3 exit, and its 120 fps clause at the Phase 4 midpoint.
- A Phase 4 facial runtime (FACS curves, visemes from VO phoneme alignment, eyes, face LOD; RT-22).
- Whole-image ISA levels: every runtime image is built for AVX2, and only the CPU-gate TU, the launcher and the
  bootstrap are baseline. The gate runs before any other code in the image, including mimalloc's and Tracy's
  pre-`main` hooks (the first TLS callback on Windows), and an audit checks flags, the gate object, every
  pre-gate entry and the baseline images' instructions. The SDK holds studios' game modules to `avx2`.
- The normative **`.hschema` grammar** and the `helios-schemac` emitters (C++, replication, Luau, proto, Go,
  SQL, editor, records, lint, and `.htypes` runtime type bundles). Project packages are dynamic by default:
  records, `ScriptState` components, events and view-models run from data through generic `TypeOps` and a Go
  interpreter, so a studio adds types with no C++ and sees them live in ≤ 5 s (RT-21).
- The flecs wrapper with Iris-style dirty bits.
- The world model:
  - reference frames and portal graphs;
  - `Reparent()` by the transport theorem;
  - Jolt grids with bubble and host kinds, where bubbles are disjoint clusters whose seams are tested
    (RT-19);
  - zones;
  - OFPA object containers;
  - streaming with a residency hook;
  - deterministic fixed-point PCG with a Slang twin, and terrain collision tiles selected from body state,
    never substituted by a coarser tile, and budgeted at ≤ 1 core for 500 dispersed players (RT-20). A
    cell's tile builds are capped at 8 core-ms per tick (≤ 1.2 ms wall), and a client past its per-frame cap
    suspends prediction rather than predict wrong (RT-12's landing fence case).
- The asset pipeline (DDC, paks, hot reload in ≤ 2 s).
- Physics with physics-asset records, ground vehicles on Jolt's `VehicleConstraint` stepped at 60 Hz in their
  bubbles, and client-only cosmetic ragdolls and cloth; animation with crowd LOD, and hitbox poses that cells
  and clients sample through the deterministic `det::HitboxSampler`; audio, the client voice pipeline,
  navigation, the Luau host for cells, clients, the editor and world-script hosts (deterministic fuel budgets
  that also charge each binding call from a calibrated cost table, a `det-math` patch so Luau math matches on
  every C runtime, no involuntary yields, DAP debugger) and the RmlUi game-UI runtime.
- 22 acceptance criteria (RT-01…22). The most important is RT-01, a 50k-entity zone benchmark whose failure
  reopens the ECS decision.

**[03 — Rendering](plan/03-rendering.md).**
- A handle-based RHI with bindless descriptors, a Null backend and GPU-crash breadcrumbs, and a D3D12 seam
  with a costed Phase 4 gate.
- The Slang → SPIR-V toolchain with recorded PSO lists and fallback PSOs, so pipeline creation never stalls
  the render thread.
- A render graph with aliasing and async compute.
- A GPU scene with **two-level transforms**, so a ship moving at 1,500 m/s updates one record.
- Two-phase HZB culling, fleet impostors and brackets, and a visibility buffer in Phase 4.
- Clustered forward+ PBR, cascaded and later virtual shadow maps, per-tile horizon maps for far terrain
  shadows, and DDGI with range-limited acceleration structures.
- Texture-mip and mesh-LOD streaming from GPU feedback keyed by stable texture IDs and from trajectory
  prediction, gated at 1,500 m/s (RC-12).
- Character composites tiered by animation LOD, GPU-encoded to BC formats in a capped cache, with tinted
  crowd impostors, so 200 unique avatars fit MIN's texture pool. Their geometry twin bakes shapes once per
  appearance into a capped cache and skins into a tiered output ring that keeps previous positions on every
  tier, so characters animated at reduced rates still write exact motion vectors. FACS face shapes run for
  ≤ 8 faces (RC-10, RC-11).
- The sci-fi set:
  - starfields and nebulae;
  - Hillaire atmosphere;
  - cube-sphere CDLOD planets from fixed-point noise, matched with the server's collision (bit-identical
    target, ≤ 1 cm limit);
  - Nubis clouds with a fast mode for 1,500 m/s flight through the layer, weather effects, vegetation and
    oceans;
  - shields, plumes, warp and GPU particles.
- HDR, TAA with an `IUpscaler` interface (FSR 2 default), and per-pass GPU budgets for every gated scene, tier
  and frame rate: six REF columns including 120 fps Performance mode, and five MIN columns with a VRAM check
  for BENCH-1 and the 2,000-ship BENCH-3. Lavapipe golden images run on every commit, including a walk-away
  golden through FSR 2 from Phase 2, and crowd captures must pass a temporal-stability check (motion-vector
  consistency, and limb ꟻLIP against a full-rate control) through the shipping upscaler modes (RC-9, RC-10).

**[04 — Networking and servers](plan/04-networking-and-servers.md).**
- **HTP**: vendored netcode + reliable, with nine Helios channels (one for voice) and no crypto of our own.
- Connect tokens, reconnect tickets, AIMD bandwidth budgets, gateway instances and sharded trunks with early
  throughput gates, a trunk IO pool with a per-box trunk budget and zone-instance affinity for token assignment
  (NS-4.4), make-before-break gateway relocation for deploys (Phase 4), and gateway boxes sized N+1 across
  availability zones so losing one at 50k CCU brings
  ≥ 99 % of its sessions back within 10 s (NS-4.7); two zones hold every session, so an availability-zone
  loss causes no "full" denial either (12 boxes at 50k CCU; 05 §6.7, BE-A19).
- Voice chat (ADR-015): Opus over HTP through a C++ forwarder, with party, fleet, org, ship-intercom and
  proximity channels and server-side moderation.
- The cell tick graph, with budgets per tick profile, and a zone leader that keeps multi-cell zones on one
  schedule and one TiDi factor.
- Iris-style replication: shadow state, serialize-once, deltas against acked baselines, a hierarchical
  interest hash and a priority accumulator, with worked bandwidth math.
- Prediction and reconciliation, lag compensation with capped rewind, and EVE-style command replication for
  large fleets.
- Authority groups and AG trees (parked to dormant fence rows when no cell simulates them), ghosts with margins
  measured from the receiving cell, exactly-once effects, the handoff protocol, co-location of coupled
  entities across a seam in bounded sets with a leash and effect-only fallbacks (04 §6.2a), lag compensation
  across cell boundaries (NS-3.12), planned region migration for rolling restarts, drains and pre-provisioning
  (hitch ≤ 1 s per region moved, a zone-wide hold capped at 1 s, residuals checked by `sim_abi`, no state
  loss; NS-3.11), cross-cell view
  composition (a lost gateway's sessions rebind at every contributing cell), and crash recovery: a warm
  standby from Phase 2, and from Phase 4 the **replicant state tier**, which caps a world zone's loss at
  ≤ 1 s of state with no disconnect and restores the region to one consistent tick (NS-4.6).
- Zones, instances, layers and phasing; the overload ladder; security and anti-cheat; NetSim, a normative
  deterministic-replay contract (every async input enters through a recorded `SimInbox`, and Luau math, hitbox
  sampling, sort ties and the FP environment are pinned, so replays match across compilers and across AMD and
  Intel CPUs; NS-3.8) and bot swarms.

**[05 — Backend services](plan/05-backend-services.md).**
- A 23-service catalogue built as a **modular monolith until Phase 3**, including **world scripts**: the
  studio's backend extension surface (shard-scope sandboxed Luau with declared tables, timers, escrow-only
  ledger intents and cell-callable RPCs; BE-A20). Player cities run on it as the Foundation `CityGovernance`
  script.
- A control plane where safety comes from PG-anchored fences and generations and liveness from heartbeats:
  two-signal failure detection classified by failure domain (a host or rack loss is recovered as crashes,
  never as a shard freeze), a warm pool sized to the largest failure domain, a degraded mode that never
  fences cells on a NATS outage, PG-allocated block IDs, `Drain` and `PreProvision` as planned region
  migrations committed in one fenced transaction, and `DrainGateway`, which rolls gateway boxes by relocating
  each session make-before-break (05 §6.3.1, BE-A14).
- The double-entry ledger with custody, fences, escrow, idempotency, guard rows (`ClaimGuard`) for weekly
  lockouts, and housing plot rows whose exclusion constraint arbitrates concurrent placements; the fence's
  write load is budgeted beside the ledger's on the shard primary (BE-A5).
- Parties and EVE-style **player fleets** (≤ 256 members, fleet → wing → squad, boss and commander roles,
  adverts, roster fan-out; mechanics and GP-15 in 06 §8.3a), a specified matchmaker (PG-fenced leader per
  queue partition, role feasibility, rated team splits, capacity per shard), a group finder, leaderboards and
  activity state (Phase 3; cross-shard federation in Phase 5).
- Fenced market actors (every order insert, modify, cancel and match), durable timers with the structure
  upkeep worker (maintenance → decay → condemnation → reclamation), persistence and the fence, world state
  (flags, meta-events, influence, territory with audited transitions, sovereignty), content publishing with client and server
  parts under a compat epoch (server hotfixes with no client patch, staged client builds that never split a
  zone), the collab service with durable journals, tested restores and a project-wide data session, live
  config and kill switches, telemetry and trust, and GM audit with a hash chain.
- Player support: one case queue for reports, tickets, trust hits and appeals, with P1–P4 SLA targets and
  chat evidence that carries a keyed server tag, so it cannot be forged (Phase 3); and a store API whose
  grants come only from the payment provider's webhook, with monthly spending caps and no randomized offers
  for players aged 13–17 (Phase 4).
- connect-go for Go and HTTPS; nats.c with generated binary codecs for cells.
- The data model with partitioning, bounded retention, a Phase 2/4 storage-growth model and backups.
- Privacy by design: PII only in Identity, crypto-shredding, DSAR export and erasure, age gating and
  versioned ToS acceptance; per-store RPO/RTO, regional DR, on-call and runbooks.
- Dupe prevention, exactly-once effects through a transactional outbox, sagas and conservation audits.
- The Docker-free Windows developer experience, Kubernetes + Agones operations, SLOs, an egress cost model,
  and FastCDC patching with signed manifests.

**[06 — Gameplay framework](plan/06-gameplay-framework.md).**
- A **kernel, not a genre**: tags, Dogma/GAS attributes and modifiers, effects on a TiDi-safe timing wheel,
  abilities with a native predicted state machine (ASM), cues, and the HXL formula language (C++ and Go
  interpreters, bit-identical, with Go multiply-add fusion ruled out by explicit rounding and a lint).
- Items with ledger custody, sockets and audited RNG.
- SWG-style resources and crafting, and EVE-style industry.
- Economy reason codes, vendors and markets.
- One progression model covering skills, achievements, collections, codex, legacy and season tracks
  (account scope stored by the character service); quests, missions, public events and group conversations.
- Activities: encounters with checkpoints, wipes and revive rules; lockouts enforced by ledger guard rows;
  difficulty tiers, modifiers, level sync, bolster and solo modes; backfill, AFK and vote-kick policy; PvP
  match rules with rated matchmaking; opt-in PvP risk zones with extraction; a group finder and
  leaderboards.
- Owned NPCs (companions, pets, hirelings, crew, drones) riding the owner's authority group, with a
  command channel, ledger-backed gear and crew missions on durable timers.
- Player fleets (Phase 3): fleet warp that lands every eligible ship on one tick in fixed-point formation
  slots, rate-limited broadcasts, command bursts through a `Fleet` modifier domain, and killmails with fleet
  damage shares, proven at 500 ships in Phase 3 and inside the 2,000-ship battle in Phase 4 (GP-15).
- An interaction framework (doors, terminals, loot, pressure), emotes and performances, map data, and
  weather gameplay.
- AI with behaviour trees, utility selectors and AI LOD tiers.
- Six-degree-of-freedom fly-by-wire with a bit-exact thruster allocation (so owned-ship prediction matches
  the cell on every compiler), EVA with suit thrusters, magboots, grabs and tethers, command flight, warp,
  multi-crew seats, sensors and EWAR, and one damage pipeline.
- Ground vehicles and mounts (Phase 2): wheeled vehicles on Jolt's `VehicleConstraint`, hover vehicles on ray
  suspension and ridden creatures on an animation-driven mount mover, driven through a `Drive` input channel and
  owner-predicted under the same bit-exact rules (GP-4d).
- Housing on ledger plots and lots with an exactly-once maintenance → decay → condemnation → reclamation
  chain; player cities (ranks, citizens, elections, taxes) as the Foundation `CityGovernance` world script;
  territory with vulnerability windows, capture channels and reinforcement timers that pre-provision the fight,
  and sovereignty in Phase 4 (GP-16, GP-17).
- PvP law, character creation with paid restyles and ship liveries; the Luau API
  and determinism rules, including `Authority.Adopted`, which re-arms scripts after a handoff, a migration or
  a recovery.

**[07 — Editor and tools](plan/07-editor-and-tools.md).**
- The ToolsFramework: documents, a command bus, **property-path transactions**, a crash journal, automation
  and headless `helios-tool`.
- The ImGui shell with role layouts, a shared widget library, and a large-world viewport with a "view as"
  phase debugger.
- PIE on one real cell, or on N cells with forced handoffs and per-cell debuggers, plus a world-script host in
  every mode when the project declares world scripts, with a debugger per partition, hot reload and a World
  scripts panel for tables, invocations and dead letters (07 §1.6.2); git + LFS source control
  with a JSONC merge driver and 500 GB scale budgets; live collaboration in edit instances with atomic
  multi-document transactions, one-PR publish, and a journal mirrored to object storage and git with tested
  restores; web tools for writers. Each document has one home session: its zone's session, or a
  project-wide data session for records, quests and dialogue with no zone, with fenced checkout between them,
  so two sessions never accept conflicting edits to one record (ED-20).
- A public, versioned **editor extension SDK**: gems add panels, documents, importers, cook steps, viewport
  modes, rules and node libraries in C++ or Luau without engine edits, under the same docs and upgrade
  guarantees as the game APIs. Edits keep derived data (navmesh, HLOD, impostors, probes) live within budgets.
- Physics, volume and partition authoring: hitboxes with hit zones, ragdolls and cloth in T15; portal cells,
  grid volumes and zone partitions in T01; a nightly UI-level test harness for every tool.
- The full **T01–T30** specification, each tool with data flow and MVP and AAA scope, including T12's
  Activity mode (encounters, tiers, lockouts, match rules and a queue simulator running the real matchmaker),
  T08's governed schema editor, with which a designer adds a record type, a `ScriptState` component and a
  view-model live in PIE in ≤ 5 s with no compiler (ED-22) and edits `worldscript` blocks, T21's ground-vehicle
  authoring, T15's mount and rider sets, and T27's support case queue. With the editor alone, a designer
  extends a cross-zone bounty board, debugs it and migrates 100k rows while bots play (ED-23).
- The asset daemon, DDC, validation engine, editor budgets, UX and accessibility, and a phase-by-phase tool
  delivery table.

**[08 — Client and launcher](plan/08-client-and-launcher.md).**
- Client startup budgets and state machine (with an invisible Relocating state that keeps two connections
  open through a gateway deploy), threading (a game thread separate from the OS thread),
  settings (including voice chat), input, the camera rig, diegetic UI, accessibility, localization, and
  sandboxed addons with protected actions.
- The Foundation UI: 29 RmlUi panels covering every player-facing system of 06 (crafting, survey and
  harvesters, placement and decoration, trade, mail, vendors, terminals, organizations, fleets, group finder,
  killmails, the character creator, customization and more), plus the store (with 13–17 spending caps) and
  player reports and support tickets with verifiable chat and voice evidence. Each panel has a phase, an
  owning WP, schema view-models and a headless Luau flow, and a theme-token reskin is proven without engine
  edits (CL-23).
- An input-latency budget from input to photons: measurement points, just-in-time frame pacing on
  `present_wait`, same-frame low-latency rendering and a photodiode-validated harness, gated from Phase 2
  (CL-6: ≤ 40 ms at 60 fps, and ≤ 28 ms at 120 fps in Phase 4).
- The anti-cheat stance, and the Windows and Linux specifics.
- The launcher: CPU and GPU gates, login, a signed trust chain, chunk patching, streaming install,
  self-update, the installer and code signing, Linux AppImage packaging, and the crash and symbol pipeline.
  The launcher is built for x86-64-v1, audited for wider instructions and run under emulated pre-SSE4.2 CPUs,
  so it can explain an unsupported CPU instead of crashing (CL-17).
- Product identity: every path, URI scheme, registry key, credential and endpoint is scoped by the project's
  `productId`; `helios-tool product init` runs an offline key ceremony, prebuilt SDK binaries are stamped
  with the product's root keys, and T29's *Package & publish* profile outputs a branded, signed installer and
  AppImage, so two Helios games install side by side on Windows and Linux (CL-24).

**[09 — Roadmap and process](plan/09-roadmap-and-process.md).**
- Phases 0–5 as work packages (WP-0.1 onward): track, owner section, dependencies, deliverables and acceptance
  criteria. Every criterion added in review rounds 3 and 4 (RT-20…22, ED-22, ED-23, GP-4d, GP-15…17, BE-A20,
  NS-3.11/3.12 and RT-12's new clauses) has an owning work package and a phase exit. Phase 0–2 scope over six
  engineer-weeks is split, so WP-2.16 is ten sub-WPs (WP-2.16a1…f).
- The critical path, with an ISA-rework lane and a tracked DX schema lane (its tightest slack is 4.5 months to
  the Phase 2 exit), and a reconciliation table for cross-section conflicts.
- The staffing and cost reality check, and what the agent loop does and does not change.
- The build loop: roles, rounds, definition of done, CI tiers, evidence classes, termination and regression
  ratchets; merge methods by branch class (work packages squash, collab content publishes rebase-merge and are
  validated commit by commit, governed project-schema publishes from T08 ride that tier with a code-owner
  approval, and a publish that depends on another session's journal waits for its carry PRs); and plan-change
  propagation (declared plan changes, conformance-rework work packages in the same round, a conformance lint
  in every PR and round audit, and the same rules for working-tree code and review rounds, D7, which is why
  the in-tree ISA and gate code has rework WPs WP-0.2r and WP-0.5r).
- The developer-experience track: docs pipeline (including the world-script API), SDK, `upgrade-project` and
  five starter templates, each of which declares a dynamic project type through T08.
- The sponsor budget for the human-supplied inputs H1–H8, agent compute and CI (≈ $2.2–13M to the AAA bar),
  including the REN-8 panels and the TOOL-10 content team, with funding gates at every phase exit; patent
  counsel for the Improbable review is funded in Phase 3, ahead of WP-4.3.
- The test strategy, a 40-entry risk register (K1–K39 plus K5b), in which K2 and K39 fired in Phase 0, the
  current status and the next twelve work packages. Every in-tree module README records its `Plan-Rev`, and
  a CMake script checks the status table against the tree at every round audit (D6, D7).

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
| M03 | Locomotion, mounts, vehicles | SWG, DST, SC, TOR | 02, 06 | 1 (vehicles, mounts 2) |
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
| G12 | Activities: fireteams, encounters, checkpoints, lockouts, tiers, level sync | DST, TOR | 04, 05, 06 | 3 |
| G13 | AI: BT + utility + perception, AI LOD, spawns | SWG, DST, TOR | 06 | 1 |
| G14 | Death pipeline: wrecks, clones, insurance, killmails | EVE, SC | 06 | 2 |
| G15 | Character creation: species, morphs, fit | SWG, TOR | 02, 03, 07 | 4 |
| G16 | Modular ship assembly, fitting, liveries | SWG, EVE, SC | 06, 07 | 3 |
| G17 | Live-ops calendar, seasons, data hotfixes | DST | 05 | 4 |
| G18 | Background economy/world simulation | — | 05 | 5 |
| G19 | PvP match modes and rating, matchmaking, group finder, leaderboards | DST, TOR | 05, 06 | 3 |
| S01 | Gateway + cells, per-zone tick 1–60 Hz | all five | 04 | 1 |
| S02 | Static multi-cell zones, fenced handoff; replicant crash recovery | SC | 04 | 3 (replicants: 4) |
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
| SWG sandbox | Osk crafting and market (Ph2); speeders, rovers and creature mounts on Harrow (Ph2, GP-4d); Saltmarch settlements with 300 structures placed through the housing rules, and player cities with citizens, elections and taxes (Ph3, BENCH-5, GP-16); a cross-zone bounty board and a player city as world scripts in `starter-sandbox` (Ph3, BE-A20, GP-16) | Ph4 (G15 character creation) |
| EVE economy and fleets | Osk Yard order books (Ph2); Vane territory with vulnerability windows, capture and reinforcement timers that pre-provision the fight (Ph3, GP-17), and a 500-ship battle fought in player fleets with fleet warp and command bursts (Ph3, GP-15); 2,000-ship battle at 10 % TiDi as eight player fleets (Ph4, BENCH-3, NS-4.2); sovereignty upgrades (Ph4, GP-17) | Ph4 (S05, R03) |
| Destiny action | Hollow Vault strike (Ph3, BENCH-4); a matchmade strike and a rated 6v6 match in `starter-shooter` (Ph3, GP-14); Lattice Heart 6-player raid and seasons (Ph4) | Ph4 (G17, R07) |
| Star Citizen seamless | BENCH-2 orbit-to-interior descent with no loading screen (Ph1); Mule multi-crew boarding at 300 m/s (Ph3, BENCH-6); cell kills in four-cell Harrow orbit and single-cell Harrow High with ≤ 1 s of state lost and no disconnect (Ph4, NS-4.6); seamless Lattice travel between systems (Ph5) | Ph5 (S03 dynamic meshing, which SC has not shipped either) |
| SWTOR story | "Signal from Saltmarch" quests and dialogue (Ph2); group conversation (Ph3); ~300 auto-staged voiced lines in 2 languages, and a flashpoint with tiers, level sync, a group finder and a solo mode in `starter-story` (Ph4) | Ph4 (G15, R04) |

**The Star Citizen caveat.** Four of the five classes have every critical capability by the Phase 4 bar. The
fifth, Star Citizen, has all but S03, dynamic split/merge, and SC has not shipped that either. What SC *has*
shipped is its replication layer (Replicant, Atlas, Scribe and Gateway), live with Persistent Entity
Streaming since Alpha 3.18, and static server meshing on top of it since Alpha 4.0 (December 2024). Since
then a game-server crash no longer disconnects players: a replacement server resumes from state the
replication layer holds ([R04 §4.1–4.2](research/04-star-citizen-seamless-universe.md#4-replication-layer--server-meshing)).

The Phase 4 bar matches that recovery. Static multi-cell zones behind gateways (S02, Phase 3) gain a
**replicant state tier** in Phase 4 ([ADR-007](plan/00-decisions.md#adr-007-server--network-architecture),
[04 §6.4](plan/04-networking-and-servers.md#64-crash-recovery)). It covers every persistent world zone,
single-cell hubs and stations included, where nearly all players are; activity, phase and housing instances
keep checkpoints and may opt in. Each cell streams the
state it already serializes once per tick to a replicant that keeps it in RAM, so after a cell crash the
standby resumes from memory. The player sees no disconnect, loses ≤ 1 s of non-value state (AAA-SRV-9, down
from 30 s) and a hitch of ≤ 10 s (AAA-SRV-12); NS-4.6 proves it in four-cell Harrow orbit and in single-cell
Harrow High. Gateway deploys also leave players connected: sessions move make-before-break to another gateway
(05 §6.3.1, AAA-STB-6). One structural difference remains, and it
is recorded rather than hidden: SC's replicants also stream client replication, while Helios keeps client
replication in cells until the Phase 5 gateway replication layer. That layer and dynamic meshing take the
class past SC's shipped state, and cut the crash hitch to ≤ 1 s. Counsel reviews Improbable's patent
US 11,792,306 before WP-4.3 builds the replicant tier, and again before WP-5.2.

---

## 8. Roadmap and build loop

### 8.1 Phases ([09 §1–2](plan/09-roadmap-and-process.md#1-roadmap-overview))

| Phase | Goal | Demo milestone (what the user sees) | Human-studio calendar | Key exit gates |
|---|---|---|---|---|
| **0 Foundations** | Every part exists as a skeleton on every toolchain | **M0 "Handshake":** the launcher verifies a signed chunked manifest; the client connects through a gateway to a cell ticking a dilatable clock; the editor edits a record with undo; all on Windows without Docker | 9–12 months | AAA-PLT-1/2, REN-7; RC-1; ED-1; RT-13, RT-18 (link-model spike); the schemac, spike and backend skeleton tests; the conformance lint clean on the full tree (WP-0.15r, WP-0.2r, WP-0.5r and WP-0.10r merged); funding gate F0 |
| **1 First Light** | A vertical slice through every layer at 50 players | **M1 "Descent" (BENCH-2):** undock a *Kestrel*, descend from 400 km to Harrow with no loading screen, land, walk into Saltmarch and shoot Hollow drones with prediction beside 49 bots; the editor runs PIE with 2 clients | 12–18 months | REN-5, REN-8 (first look-and-feel panel), ITR-1/3/5, SEC-1/4, PLT-3/4, TOOL-1, TOOL-7 (Ph1); RT-01 (the ECS gate, which closes ADR-004a); GP-4a (bit-exact flight); RT-20 (terrain-collision slice) |
| **2 Alpha Sandbox** | Economy, quests, social; 500 per zone; production patching; all P0 tools | **M2 "Osk Yard":** survey, harvest, craft a rifle with rolled perks, sell it on the market to another account, run a quest with dialogue, chat in a guild beside 500 bots; the signed launcher patches by CDC and self-updates | 15–21 months | SRV Ph2, SEC-2/3/6, CNT-7, TOOL-2/6, TOOL-7/8 (Ph2), ITR-2/4/6; CL-6 (input latency); CL-24 (branded products side by side); ED-19 (Luau editor extensions); RT-21 and ED-22 (project types with no compiler); RT-20 (surface collision load); GP-4d (vehicles and mounts) |
| **3 Beta Scale** | Multi-cell zones, instances, 5k CCU, live co-editing | **M3 "Harrow Orbit":** four cells, *Mule* boarding across a boundary (BENCH-6), a 5k-CCU bot shard, the Hollow Vault strike in < 2 s (BENCH-4), three designers co-editing a settlement at BENCH-5 scale (ED-10), the designer day | 18–24 months | SRV Ph3, REN-8 (panel on par), STB-5, TOOL-3/5, TOOL-7/9 (Ph3), ITR-7, CNT-2/4/5, SEC-5/7; NS-3.10 (no pop-in across cells), NS-3.11 (planned migration), NS-3.12 (seam lag compensation); RT-12 (MIN and 30/45 fps CPU); GP-14 (activities, matchmaking); GP-15 (player fleets); GP-16 (housing and cities); GP-17 (territory); BE-A20 (world scripts); ED-19…21, ED-23 (world scripts in the editor) |
| **4 Launch Quality** | **The AAA bar** | **M4 "Vane":** 2,000-ship battle at 10 % TiDi (BENCH-3 ≥ 45 fps on REF), 6-player raid, a season, auto-staged VO, a killed Harrow-orbit cell resuming from its replicant with ≤ 1 s of state lost, a clean 72 h soak at 50k CCU, a clean pentest, every BENCH scene within budget on MIN and REF, 30/30 tools | 18–24 months | Every criterion with Ph ≤ 4, including SRV-9 ≤ 1 s in persistent world zones (NS-4.6), NS-4.7 (gateway-box loss at 50k CCU), BE-A14 (live hotfixes and gateway rolls with no disconnect), GP-17 (e) (sovereignty), RT-22 (facial runtime), REN-8 (external panel) and TOOL-10 (content-team zone); RT-12's 120 fps clause at the Phase 4 midpoint |
| **5 Ambition** | Beyond the bar | **M5 "Lattice":** seamless travel between systems, dynamic cell split/merge, a ≤ 1 s cell-crash hitch, a 100k-bot shard, player scripting, RT on SHOWCASE | 18–30 months | SRV Ph5, REN-6 (RT) |

**The critical path to M1** is schema → ECS → world model → replication → prediction → gameplay → content.
The rendering lane is near-critical because of the GPU-terrain contract for BENCH-2, and the ISA-rework lane
(WP-0.2 → 0.2r → 0.5r) must land before WP-0.9's `avx2` kernels and WP-0.17's `base` launcher build on it.
The DX schema lane (WP-2.16c1 → c3 → e), which carries RT-21, ED-22 and CL-24 to the Phase 2 exit, is tracked
like a critical chain; its tightest slack is 4.5 months (ED-22's packaging clause, on the short calendar).
From Phase 2 the critical path is networking scale (2.4 → 3.1 → 4.3), and WP-4.3 also waits on counsel's
Improbable review, which is therefore funded in Phase 3. The Phase 4 exit is also gated by human-supplied evidence (H1 GPU lab, H6
playtesters, H7 pentest), which the loop cannot shorten. What those inputs and the loop itself cost a single
sponsor, about **$2.2M (Lean) to $13M (Full)** through the AAA bar, is itemized per phase in
[09 §4.3](plan/09-roadmap-and-process.md#43-sponsor-budget-h1h8-and-the-loops-running-costs); every phase exit is also a
funding gate. **The next twelve work packages** are listed in
[09 §8.2](plan/09-roadmap-and-process.md#82-next-12-work-packages-in-execution-order), after the in-progress WPs
(cell and gateway, gameplay kernel, render graph, lints and core completion) merge. The first are the
conformance rework of the in-tree backend (WP-0.15r), the Luau fuel patches (WP-0.10r), ADR-004a's wrapper
optimization (WP-1.1a) and the rest of the CI matrix.

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
  5. Run the CI PR tier: the eight-configuration compiler matrix of ADR-001a (primary, floor and modular
     MSVC, clang-cl, GCC, Clang, headless, MinGW), the conformance lint on changed paths, Go on Windows and
     Linux, lavapipe goldens and a 16-bot smoke test.
  6. Rebase and verify in the merge queue.
  7. Merge by branch class ([09 §5.2a](plan/09-roadmap-and-process.md#52a-merge-policy-by-branch-class)):
     work packages squash-merge; the collab service's `collab/*` content publishes rebase-merge, are never
     squashed, and pass validation on every commit, so each author's commits and ED-10's check survive.
     Governed project-schema edits from T08 ride that content tier with lock, lint and compat checks per
     commit and a code-owner approval; engine, Foundation and native schema packages stay WP-only.
  8. Score in the nightly run.
- **Definition of done:**
  - tests that fail without the change (the reviewer checks by reverting);
  - warning-clean on all five compiler families (both MSVC toolsets, clang-cl, GCC 13, Clang 17, MinGW);
  - lints pass;
  - budgets are benchmarked;
  - every criterion is registered in `scorecard.jsonc`;
  - docs and ADRs are updated;
  - the branch's `Plan-Rev` is current for the paths it touches, and the conformance lint is clean there.
- **Parallelism:** at most six implementers in flight, one queue, and module locks on public headers. It
  scales up only while review rejection stays below 30 % and the PR tier below 45 min.
- **Regression control:**
  - green criteria ratchet into the blocking set;
  - a nightly failure starts a bisect agent;
  - flaky tests count as failing after 7 days;
  - auditors mutation-test samples of merged code to catch test gaming.
- **When the plan changes** ([09 §5.10](plan/09-roadmap-and-process.md#510-plan-changes-conformance-rework-and-the-conformance-lint)):
  - every plan PR declares the decisions it changes;
  - in the same round the Director opens conformance-rework work packages for code that already exists
    and re-baselines work in flight;
  - a conformance lint flags banned constructs, such as NATS lease TTLs, node-ID minters, per-file ISA
    grants or a misplaced CPU gate, in every PR and in every round audit;
  - the same rules cover working-tree code that has not merged and every review round (D7), which is how the
    in-tree ISA and gate code got rework WPs WP-0.2r and WP-0.5r;
  - no phase exits while a finding is open.
- **Termination:** the loop stops at the **Phase 4 exit**. Every Ph ≤ 4 criterion must be green on Windows and
  Linux, two independent auditors must score ≥ 9/10 on closeness to the user's goal, and the user must accept.
  Phase 5 runs only if the user opts in.

---

## 9. The AAA Scorecard and how progress is measured

The scorecard in [01 §3](plan/01-vision-and-scope.md#3-the-aaa-bar-measurable-acceptance-criteria) holds **65
criteria in eight families**. Each criterion has a phase, and all of them are measured on named hardware:

| Family | # | Examples (phase) |
|---|---|---|
| Rendering (REN) | 8 | REF 1440p High: BENCH-1/2/4/5 ≥ 60 fps with p99 ≤ 20 ms and BENCH-3, the full 2,000-ship battle, ≥ 45 fps (4); MIN 1080p Low upscaled ≥ 60 fps (4); zero PSO hitches > 50 ms in 30 min (3); 10¹³ m jitter test and seamless BENCH-2 (1); goldens every commit (0); **look and feel**: a blinded external panel rates captures and a firefight on par with current reference titles (1, 3, 4) |
| Server scale (SRV) | 12 | Players per cell 50 → 500 → 1,000; CCU per shard 50 → 2k → 5k → 50k → 100k; handoff p99 < 100 ms (3); ledger 2k → 10k → 50k tx/s; 2,000-ship battle in a 2 Hz zone at TiDi ≥ 10 % with module response ≤ 1 tick of game time + RTT (4); cell-crash hitch ≤ 10 s (3) → ≤ 1 s (5); state lost on a cell crash 30 s (3) → ≤ 1 s in world zones via replicants (4) |
| Iteration (ITR) | 8 | Save → visible in PIE ≤ 2 s (1–2); editor open ≤ 10 s (2); PIE ≤ 15 s (1); `.cpp` edit → relinked ≤ 30 s (1); clone → in-game ≤ 30 min (2); co-edit ≤ 1 s (3); live hotfix ≤ 15 min with no client disconnected or re-placed (4) |
| Stability (STB) | 6 | ≤ 1 client crash per 1,000 play-hours (4); journal loses ≤ 1 transaction (2); `kill -9` chaos creates or destroys no value (3); 72 h soak at 50k CCU (4); rolling restarts with no disconnect, each region migrating with a hitch p99 ≤ 1 s and no state loss, and gateway boxes rolling by make-before-break relocation with 0 linkdead sessions (4) |
| Content (CNT) | 7 | Systems ≥ 10¹¹ m, planets up to 6,400 km (1/3); game thread never blocks > 1 ms on I/O (3); memory ceilings (3–4); 100k records in ≤ 60 s (3); patch ≤ 1.5× changed bytes (2) |
| Security (SEC) | 8 | Schema lint on every client message (1); every value move through the idempotent ledger (2); speedhack flagged ≤ 2 s (2); no server-only data in client cooks (1); fuzzing ≥ 24 CPU-h (3); external pentest clean (4) |
| Tooling (TOOL) | 10 | Tool MVPs (1); P0 tools (2); 26/30 AAA-complete (3); 30/30 (4); **designer day**: a newcomer adds a hull, a weapon, a quest and a vendor in ≤ 1 day without an engineer (3); 100 % of public APIs and record types documented, the C++ game-module, editor extension and world-script APIs included (1–3); projects upgrade across an engine release with zero manual edits (2, 4); five starter templates, one per reference class, each proven by a scripted build (3–4); **content-team zone**: 3–5 contracted content developers with no engine engineer build a playable zone in ≤ 2 weeks with zero blocking issues, plus a UI-provenance report (4) |
| Platform (PLT) | 6 | Every commit on both MSVC toolsets (VS 2026 primary, VS 2022 17.14 floor), clang-cl, GCC, Clang and MinGW (0); backend on Windows without Docker (0); smoke tests on Win 10/11 and Ubuntu (1); cross-compiler bit-identical PCG (1); Linux within 10 % of Windows (4); signed installer and AppImage (2–3) |

**Below the scorecard**, each section defines its own automated criteria, and 09 maps them onto work packages:
- RT-01…22: runtime;
- RC-1…13: rendering;
- NS-p.k: networking, per phase;
- BE-A1…A21: backend;
- GP-1…17 (GP-4 as 4a–4d): gameplay;
- ED-1…25: editor;
- CL-1…24: client and launcher.

**How progress is measured** ([09 §5.6–5.8](plan/09-roadmap-and-process.md#56-ci-tiers-and-evidence-classes)):
- **`scorecard.jsonc`** maps every criterion to its tests, evidence class, platforms and threshold. The
  nightly report shows every criterion's state and trend.
- **Evidence classes** decide how a criterion can be shown to pass. None of them relaxes a threshold.
  - **N**: three consecutive nightly passes on Windows and Linux.
  - **H**: the same on lab hardware. With no lab, the criterion is *unmeasured*, which counts as failing.
  - **W**: long-running soaks, with two scheduled passes.
  - **M**: manual or external evidence, such as the designer day, the look-and-feel panel (REN-8), the
    content-team zone (TOOL-10), the pentest and legal sign-offs, as a signed record.
- **A phase is complete** when every criterion with Ph ≤ N passes, two independent auditors score **≥ 9/10**,
  no critical or high risk is open without an accepted mitigation, the full-tree conformance lint has no
  finding and no conformance-rework work package is open, the user has validated on Windows, and the user has
  decided the phase's funding gate.
- **The per-round auditor score** is 60 % the fraction of passing Ph ≤ N criteria and 40 % a rubric: the
  reference-class proofs are playable, code health, documentation and tutorials, the human-UI share of content
  from TOOL-10's provenance report, the internal REN-8 rubric trend, and platform parity. Code health scores 0
  while a conformance finding has no rework work package or the status in 09 §8.1 is older than the round.
  Each report lists the top five failing criteria, the open conformance findings and the spend against the
  approved envelope.

---

## 10. Top ten risks

From the 40-entry register in [09 §7](plan/09-roadmap-and-process.md#7-risk-register). L/I is likelihood and
impact. **Fired** marks a risk whose trigger has already fired.

| # | Risk | L/I | Mitigation |
|---|---|---|---|
| K1 | Effort exceeds capacity (450–730 person-years to the bar) | H/H | Phase gates, seams first, a thin slice every phase; cut Phase 5 scope first, then polish, never MVP items |
| K24/K37 | Agent quality drift, test gaming, architectural erosion; merged or working-tree code that keeps a superseded plan decision (as the in-tree backend kept draft v1's NATS leases, and the in-tree ISA code the retired per-file AVX2 allowlist) | H/H; H/M | Adversarial review, tests that must fail without the change, mutation-sampled audits, layering lints, auditors at every phase; declared plan changes, conformance-rework work packages in the same round (WP-0.15r, WP-0.2r, WP-0.5r, WP-0.10r), working-tree code and review rounds under the same rules (D7), and the conformance lint in every PR and round audit (09 §5.10) |
| K7 | No real GPU or server hardware, so H-class criteria cannot be measured | H/H | The user's Windows PC as a self-hosted GPU runner from Phase 0; lab purchase by mid-Phase 1; "unmeasured" counts as failing |
| K23 | Art, animation and VO bottleneck (150 → 2,000 assets per phase) | H/H | Procedural and kitbash content, CC0 assets with provenance, contract artists; content scoped to proving capabilities |
| K29 | Editor widget cost and iteration regressions | H/M | Widget library first under one owner, 25 % reserve, nightly editor-performance CI |
| K2 | The flecs-based ECS misses the 50k-entity benchmark (RT-01) | **Fired** (2026-09-25); H/H | The Phase 0 pre-bench passed every RT-01 clause except structural ops: 3.4–6.7 ms for 9k ops against 1.5 ms. Raw flecs does the same ops in ≈ 0.9 ms, so the overhead is Helios' wrapper (identity maps, structural log, command fusion), which a custom ECS would also need. [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md) (open): optimize the wrapper first, in WP-1.1a, keeping flecs and the per-sync-point budget. A custom ECS behind the same API comes only if the optimized wrapper still fails RT-01 on SERVER at the Phase 1 gate |
| K4 | Cross-compiler and cross-CPU determinism drift (physics, PCG, HXL, Luau math, hitbox sampling, replay) | M/H | `det::` math, no FP contraction, fixed-point integrators, five-compiler hash tests on every PR, AMD/Intel cross-vendor replay nightly (NS-3.8) |
| K13 | Handoff duplicates or loses authority groups | M/H | Type-enforced `GhostRef`, mutation only through effects, the fence, torture bots, conservation audits |
| K28 | 2,000-ship fan-out beyond what TiDi absorbs | M/H | Command replication, volley aggregation, fleet proxies; a nightly 1,000-ship half-load server run from Phase 2 is the early warning (BENCH-3, the client gate, never triggers it) |
| K17/K18 | SWG terrain patents; Improbable's view-replication patent vs the Phase 4 replicant tier and the Phase 5 gateway layer | L–M/H | Generic node-graph terrain; counsel review before any Phase 4 release, before WP-4.3 (funded in Phase 3) and again before WP-5.2; replicants keep state for recovery only; design-around is a per-zone hot-standby cell on the same stream |

Round 4 added K38: the dynamic project-type path (02 §3.8) could miss its per-type cost targets or slip past
the Phase 2 exit, failing RT-21, ED-22 and the no-C++ templates. It is M/H and mitigated by byte-identical
native and dynamic formats (a hot package goes native with one line), CI cost budgets and the split, tracked
WP-2.16 lane (09 §2.3a, §3.1).

**Fired in Phase 0 (round 5).** Besides K2 above, **K39** fired: Luau fuel metering misses RT-13, which gates
the Phase 0 exit (H/M). Luau 0.739's native codegen charges one extra fuel per numeric `for` left by `break`
or `return`, which breaks the fuel identity replay needs, and the metering hook costs ≈ 12–15 % against
≤ 10 %. WP-0.10r mitigates both:
- a vendored CodeGen patch that moves the loop interrupt into `FORNLOOP`, with cells and world-script hosts
  interpreter-only until it lands;
- a vendored inline-counter VM patch for the fuel.

Spike (a) also found a mimalloc heap-recycling hazard, now avoided by pooled heaps, and tag-accounting
contention. Both are recorded under K3 and fixed by 02 §2.2's sharded, batched accounting in WP-0.5.

---

## 11. Building and running today (Windows and Linux)

**Current state (2026-09-25, round 5).** The Director refreshes this list every round
([09 §5.10.2](plan/09-roadmap-and-process.md#5102-rules-for-the-director), rule D6), and
[09 §8.1](plan/09-roadmap-and-process.md#81-phase-0-status-repository-on-2026-09-25) has the detail. The
facts below were verified by the lead: the GCC and Clang tests pass, the MinGW cross-builds link, and MSVC is
built in CI. `cmake -P tools/status/check_status.cmake` checks that 09 §8.1 names every module in the tree.
- **Committed and building:**
  - the vendored third-party tree;
  - `engine/core` (141 tests) and `engine/math` (104), complete for Phase 0 except WP-0.5's additions;
  - `tools/schemac` with `engine/reflect` (121 tests: C++, byte-identical Go and JSON output; the other emitters
    are stubs);
  - `engine/ecs` on flecs (90 tests; `ecs_bench` runs the RT-01 zone and the spikes in `engine/ecs/SPIKES.md`);
  - `engine/script`, the Luau host (79 tests);
  - `engine/rhi`, Vulkan and Null (51 tests, lavapipe goldens, the SDL3 swapchain, the `rhi_triangle` sample;
    Slang v2026.18.2 is fetched with a pinned SHA-256);
  - `engine/net`, the HTP transport (84 tests; NS-0.1, NS-0.2 and NS-0.7 pass, and NS-0.4 is owed in the
    nightly).
- **Backend (committed):** the `services/` Go module builds `helios-backend`, with 124 tests on Go 1.27.1
  through `go.mod`. It runs identity, sessions with netcode connect tokens that are byte-exact with the C
  implementation, and the orchestrator (PG leadership, ID blocks) in one process, on embedded PostgreSQL 18,
  NATS and miniredis. Its conformance rework, WP-0.15r ([09 §5.10.4](plan/09-roadmap-and-process.md#5104-applications-on-the-repository-of-2026-09-25-wp-015r-wp-02r-and-wp-05r)), is still open.
- **In progress** (working tree, not merged):
  - the cell and gateway (`engine/server`, `engine/authority`, `apps/cellserver`, `apps/gateway`, with
    `nats.c`; to try them, see "Run it locally" in `engine/server/README.md`);
  - the gameplay kernel (`engine/gameplay`, `engine/hxl`, `services/pkg/hxl`);
  - the render graph with `helios-shaderc` and `helios-rendertest`;
  - WP-0.2's lints and WP-0.5's core and math completion, CPU gate included.
- **Not yet present:** the launcher, client and editor, the patch pipeline, physics and PCG, the link-model
  spike, the scorecard and nightly perf, and the *Cinder Reach* `content/`.
- **Failing gates:**
  - RT-01's structural ops: 3.4–6.7 ms for 9k ops against 1.5 ms (K2;
    [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md), WP-1.1a);
  - RT-13's fuel identity between the interpreter and native codegen, and its ≤ 10 % overhead (≈ 12–15 %
    measured; K39, WP-0.10r). Until WP-0.10r lands, keep native codegen off on cells; `create()` warns
    when it is on.
- **Known deltas from the plan** (09 §8.1, §5.10.4):
  - the vendored SDL3 is built without its renderers and Wayland (WP-0.17);
  - the in-tree ISA and CPU-gate code still uses the per-file AVX2 allowlist and the `.CRT$XIB` entry
    (WP-0.2r, WP-0.5r);
  - the backend's schema names, e-mail columns and `region_lease` (WP-0.15r);
  - the script host warns instead of refusing codegen on cells (WP-0.10r).
- **CI** (GitHub Actions) runs on every push:
  - Windows: MSVC with VS 2026 (the primary) and with VS 2022 at MSVC 14.44 (the floor), and clang-cl;
  - Linux: GCC and Clang (GPU tests on lavapipe), headless, and the MinGW cross-build;
  - the Go backend on Windows and Linux, plus its integration suite (embedded PostgreSQL, non-root).

  The nightly adds ASan and the MSBuild builds for VS 2026 and VS 2022. SARIF and the independent post-merge
  `merge-policy` check are present. Still missing: the libFuzzer nightly, the nightly scorecard report and perf
  (WP-0.3), the merge queue and `main` ruleset, and the modular MSVC job (after WP-0.6c).

**Windows (primary).** Install **Visual Studio 2026** (recommended) or **Visual Studio 2022 17.14 or later**,
with "Desktop development with C++" and "C++ CMake tools"
([ADR-001a](plan/00-decisions.md#adr-001a-windows-toolset-policy-review-round-2)). These provide CMake and
Ninja; the top-level `CMakeLists.txt` needs CMake ≥ 3.28. CI tests both: VS 2026 on `windows-latest`, and VS 2022
at MSVC 14.44 on a pinned `windows-2022` image. The floor also builds the SDK and every released binary. Also
install Git for Windows with LFS. Then, in the **x64 Native Tools Command Prompt** of that Visual Studio:

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release -LE gpu
```

- **Debug build:** `windows-msvc-debug`.
- **clang-cl:** `windows-clang-cl` (build preset only; run tests with
  `ctest --test-dir build\windows-clang-cl`).
- **Visual Studio solution:** `cmake --preset windows-vs2026` (needs CMake ≥ 4.2, the first release with the
  VS 2026 generator), then open the solution under `build\windows-vs2026\`. Run its tests with
  `ctest --test-dir build\windows-vs2026 -C RelWithDebInfo`. On VS 2022, use `windows-vs2022` in the same way.
- **Runtime:** binaries use the static CRT (`/MT`), so no VC++ redistributable is needed.
- **Backend (works today):** with any Go ≥ 1.21 installed (`go.mod` fetches the pinned 1.27.1 on first use),
  run `cd services` and then `go run ./cmd/helios-backend --seed dev`. Log in as `dev1#0001` with password
  `dev`; [`services/README.md`](../services/README.md) has the API and flags. To build the executable, run
  `go build -o ..\build\go\ ./cmd/...`.
- **Later:** milestones are validated with `tools\milestone\validate.ps1 -Milestone M<n>`
  ([09 §5.9](plan/09-roadmap-and-process.md#59-how-the-user-validates-milestones-on-windows)) once WP-0.3
  lands. It accepts either Visual Studio version and records the compiler in its report.

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
| [`plan/00-decisions.md`](plan/00-decisions.md) | Binding architecture decisions (ADR-001…016, with ADR-001a) |
| [`adr/`](adr/) | Decision records opened by the build loop: [ADR-004a](adr/ADR-004a-ecs-rt01-structural-ops.md), ECS structural ops against RT-01 (open) |
| [`plan/01-vision-and-scope.md`](plan/01-vision-and-scope.md) … [`plan/09-roadmap-and-process.md`](plan/09-roadmap-and-process.md) | The ten plan sections (§6 above) |
| [`plan/CONSISTENCY.md`](plan/CONSISTENCY.md) | Log of every cross-section consistency fix, plus the out-of-scope follow-ups |
| [`plan/_integration-notes.md`](plan/_integration-notes.md) | Cross-section issues raised by section authors (all resolved) |
| [`research/`](research/) | The ten research reports, R01–R10 |
| [`../CLAUDE.md`](../CLAUDE.md) | Contributor and agent rules: platforms, build, layout, code style, quality gates, IP hygiene |
| [`../third_party/MANIFEST.md`](../third_party/MANIFEST.md) | Vendored dependencies, pins and licences |
| Module READMEs, for example [`../engine/core/README.md`](../engine/core/README.md) and [`../engine/ecs/README.md`](../engine/ecs/README.md) (with [`SPIKES.md`](../engine/ecs/SPIKES.md), the RT-01 pre-bench) | Each module's API, threading rules and `Plan-Rev` (09 §5.10.2 D7) |

**Glossary.** [01 §6](plan/01-vision-and-scope.md#6-glossary) defines shard, zone, cell, gateway, replicant,
orchestrator, authority group and epoch, ghost, reference frame and grid, `Reparent`, object container, record
and record template, ECS archetype and reference archetype, edit and play instances, instance, layer and
phase, activity, ledger, tick profile, Foundation layer, BENCH/Scorecard, evidence class, look-and-feel panel,
project schema package, world script, planned migration, persistent world zone, gateway relocation, player
fleet, document home and transaction origin.

**Citations.** R01…R10 are the research reports. `R0n-P0-k` is requirement *k* of report *n* (01 §7). `AAA-*`
IDs are scorecard criteria. W/M/G/S/R + two digits are capability IDs (01 §2.6). T01–T30 are editor tools
(07). WP-p.n are work packages, K1–K39 are risks, and H1–H8 are human-gated inputs (09).

---

## 13. Review record

Independent reviewers scored the plan in five rounds, each through three lenses (backend, engine, tools) out
of 10. The plan's bar was ≥ 9/10 from every lens (§2).

| Round | Backend | Engine | Tools | Result | Fixes logged in [`CONSISTENCY.md`](plan/CONSISTENCY.md) |
|---|---|---|---|---|---|
| 1 | 8.6 | 8.7 | 8.5 | Below the bar; revised (draft v2) | §5–§13; integration pass 2 (§14) |
| 2 | 8.7 | 9.1 | 8.7 | Below the bar; revised (draft v3) | §15–§24; integration pass 3 (§25) |
| 3 | 8.8 | 9.1 | 8.7 | Below the bar; revised (draft v4) | §26–§32; integration pass 4 (§33) |
| 4 | 8.8 | 9.2 | 8.8 | Below the bar; revised (draft v5) | §34–§40; integration pass 5 (§41) |
| 5 | **9.2** | **9.0** | **9.0** | **Approved with minor revisions** | §42 |

**After the approval.** The round-5 minor revisions were applied afterwards, as plan revision 6. They are
logged in [`CONSISTENCY.md` §42, "Round 5 minor revisions"](plan/CONSISTENCY.md#42-round-5-minor-revisions-2026-09-25),
one subsection per owner:
- **05:** Trust's cheat and bot detection and BE-A21; the shard primary's full write mix; replicant zone
  placement; the generated NATS permission matrix; node maintenance.
- **02 and 04:** Jolt ordering independence; the CPU-gate backstop; replay keyframes and load-dependent
  decisions as replay events; homes for the Luau patches.
- **07 and 08:** Activity PIE and ED-24; the dev shard clock and ED-25; structure authoring and BENCH-5;
  group and encounter HUD panels.
- **09 and this summary:**
  - the status was rebuilt from verified facts;
  - K2 and K39 were recorded as fired, and ADR-004a was opened;
  - D7's `Plan-Rev` was added to every module README, with a mechanical D6 check;
  - this record was added.
