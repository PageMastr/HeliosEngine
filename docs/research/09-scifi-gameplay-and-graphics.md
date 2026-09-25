# 09 — Sci-Fi MMO Gameplay Mechanics & the Graphics Behind the Sci-Fi Look

*Research report: engine and backend requirements drawn from EVE Online, Star Wars Galaxies (incl. Jump to Lightspeed), Elite Dangerous, Star Citizen, Destiny, Guild Wars 2 and SWTOR, plus the rendering literature (SIGGRAPH "Advances", EGSR, GDC, JCGT).*

> **Method note.** Web searches plus direct reads of GitHub-hosted code and docs (Jolt, `sebh/UnrealEngineSkyAtmosphere`, SWGEmu Core3, GAS docs, AMD FidelityFX). The proxy blocked most other domains, so for established literature I cite canonical URLs and flag anything unverified.

---

## Overview

A "Galaxies/EVE/Destiny/Star Citizen-capable" engine has to cover four gameplay scales that normally live in different engines:

1. **Strategic space** (EVE): thousands of ships, command-based movement, 1 Hz server logic, spreadsheet-deep fitting and economy.
2. **Tactical space and vehicles** (SWG JTL, Elite, Star Citizen): 6-DOF Newtonian flight with fly-by-wire, subsystem damage, multi-crew.
3. **Ground and interiors** (SWG, SWTOR, Destiny, Star Citizen FPS): tab-target, action or FPS combat, housing, cities, walking around inside moving ships.
4. **Planetary** (Star Citizen, Elite Odyssey): seamless space-to-surface travel with atmosphere, clouds and procedural terrain.

**Key conclusions**

- **Build a gameplay kernel, not a genre.** EVE's "Dogma" system and Unreal's Gameplay Ability System (GAS) show that fitting bonuses, EWAR, buffs, skills and implants are all data-driven *modifiers on attributes, gated by tags*. One kernel serves ships, characters, structures and NPCs.
- **Movement is pluggable per entity class.** EVE command flight is a cheap analytic model that scales to thousands of ships; Elite/Star Citizen flight is a rigid body driven by PID flight-assist and thruster allocation. Don't force everything through Jolt rigid bodies.
- **Precision and reference frames are foundational.** Jolt float mode is accurate to ~5 km; double mode costs ~5–10% and multiple `PhysicsSystem`s are supported. Use 64-bit positions, hierarchical frames (system → planet → ship grid), camera-relative rendering and reverse-Z.
- **The sci-fi look is mostly an HDR problem**: one blinding sun, black shadows, emissives, stars, nebulae. Physically based exposure, bloom, flares, tone mapping and an HDR nebula skybox used as IBL carry most of the "EVE look".
- **Planets need three research-grade systems**: Hillaire 2020 atmosphere LUTs, cube-sphere CDLOD terrain with GPU tiles, and Nubis-style clouds. All have open references or published costs.
- **Stack check**: the stack is appropriate. Flags:
  - Vulkan 1.4 shipped Dec 2024; keep 1.3 as baseline and use 1.4 opportunistically.
  - Mesh shaders and ray tracing are extensions on 1.3 and need fallbacks.
  - The newest FidelityFX SDK README lists "Vulkan is currently not supported in SDK"; FSR 2 (MIT) has a Vulkan backend. Build an upscaler abstraction.
  - EVE-scale fleet fights need a non-rigid-body movement path and **time dilation**.
  - Currency and items need a **double-entry ledger** from day one.

---

## Part A — Gameplay Systems

### A0. The cross-cutting gameplay kernel

Every system below runs on four primitives, all server-authoritative and replicated:

| Primitive | Reference design | Engine need |
|---|---|---|
| **Attributes** (floats with base and current value) | GAS `AttributeSet`; EVE Dogma type attributes | Declared in data; per-entity storage in ECS; dirty-flag recompute; replication with relevance filtering |
| **Modifiers / Effects** (instant, duration, infinite, periodic) | GAS `GameplayEffect`; aggregation `((Base + Σadd) × mult) / div`, where the multipliers are *summed* (two 1.5× give 2.0×) | Ordered operator pipeline (pre-assign, add, multiply, post-multiply, override), **stacking rules** (by source or target, max stacks), **stacking penalties** (EVE: the nth bonus is scaled by `e^{-(n/2.67)^2}`), resistances |
| **Tags** (hierarchical, e.g. `State.Debuff.Stun`, `Ship.Class.Frigate`) | GAS `GameplayTag` | Interned hierarchical tag IDs; tag queries gate abilities and effects; compact replication |
| **Abilities / Activations** (modules, spells, weapons) | GAS `GameplayAbility`; EVE module cycles | Cost, cooldown, cycle time, activation requirements (tags, resources), client prediction with prediction keys, cues for VFX/SFX |

The GAS documentation says clients predict ability activation, attribute changes, tags, cues and movement, but *not* effect removal or periodic effects ([tranek/GASDocumentation](https://github.com/tranek/GASDocumentation)). That is a good baseline prediction policy. EVE's Dogma is the proof that a single data-driven attribute/effect graph can drive thousands of item types. The open-source fitting tool [Pyfa](https://github.com/pyfa-org/Pyfa) reimplements it and is a good study reference.

**Backend:** definitions (types, effects, formulas) are content data. Author them in the editor, version them in Git, and compile them to a binary blob shared by client, zone server and Go services. Formula expressions should be data (a tiny expression VM or LuaJIT), not hard-coded C++.

### A1. Flight models

| Model | Mechanics | Engine implementation |
|---|---|---|
| **EVE command flight** | Point-and-click *approach, orbit, keep at range, align, warp*. Velocity approaches max exponentially; time to 75% of max is `ln(4)·I·m/10⁶` s (≡ `ln(2)·I·m/500000`) ([EVE Uni: Acceleration](https://wiki.eveuniversity.org/Acceleration), [EVE dev formulae](https://developers.eveonline.com/docs/guides/useful-formulae/)). Warp needs alignment and ≥75% max velocity ([EVE Academy](https://www.eveonline.com/eve-academy/ships/flying-your-ship), [EVE Support](https://support.eveonline.com/hc/en-us/articles/115004925685-System-Travel-Warping)). Server runs at **1 Hz**; clients interpolate ([High Scalability](https://highscalability.com/eve-online-architecture/)). | Closed-form kinematic "ball" integrator with analytic orbit/keep-at-range steering and bump-sphere collisions. Replicate commands, not transforms. Scales to thousands of ships per node. |
| **SWG Jump to Lightspeed** | Twitch, joystick-capable dogfighting. Chassis plus components (reactor, engine, shield, armor, capacitor, weapons, booster, droid interface), each with HP and armor HP; 0 HP disables it ([SWG Wiki](https://swg.fandom.com/wiki/Jump_to_Lightspeed_basics), [SWG Legends](https://swglegends.com/wiki/index.php?title=Jump_to_Lightspeed_Basics)). | Arcade 6-DOF with speed and turn caps derived from component attributes (A2 component graph). |
| **Elite Dangerous** | Newtonian physics under a fly-by-wire *Flight Assist* that damps angular and linear velocity. FA-Off is pure Newtonian. The throttle "blue zone" gives the best turn rate ([Flight Assist](https://elite-dangerous.fandom.com/wiki/Flight_Assist), [Flight Model](https://elite-dangerous.fandom.com/wiki/Flight_Model)). | Rigid body plus per-axis PID controllers from stick input to target velocities. A speed-dependent turn-rate curve gives the blue zone. |
| **Star Citizen IFCS / Master Modes** | IFCS allocates thrust across physically placed thrusters. Coupled mode holds velocity; decoupled holds orientation only. **SCM** (weapons and shields on, lower speed) vs **NAV** (weapons and shields off, higher speed, quantum travel) ([Master Modes](https://starcitizen.tools/Master_Modes), [IFCS](https://starcitizen.tools/Intelligent_Flight_Control_System), [guide](https://api.star-citizen.wiki/comm-links/20053)). | Thruster allocation solver (least-squares/QP over thruster directions and limits), so a damaged thruster changes handling. G-limiter. Modes are tag sets that swap attributes, i.e. GAS-style effects. |
| **Destiny Sparrow** | Arcade hover bike with boost. | Raycast spring-damper hover points plus drift and grip curves. Jolt's `VehicleConstraint` is wheel-based, so this needs a custom controller. |

**Generic support:**

- A `MovementModel` interface per archetype: kinematic-command, Newtonian-FBW, character (Jolt `CharacterVirtual`: moving platforms, stairs, wall sliding), hover or wheeled vehicle.
- Per-zone **tick-rate profiles**: 1 Hz strategic, 10–30 Hz tactical, 30–60 Hz FPS.
- **Warp / quantum travel** as a separate state with its own integrator and interest management (effectively a scripted spline).
- **Time dilation**: a per-zone clock scale. EVE slows loaded systems to as low as 10% speed rather than dropping commands.

### A2. Space combat

- **Locking.** Lock time scales with scan resolution and target signature; EVE uses `40000/(scanRes·asinh(sig)²)` (community-documented, [chruker](http://games.chruker.dk/eve_online/eve_math.php)), plus max targets and lock range. Star Citizen uses separate EM/IR/cross-section signatures for detection and missile locks.
- **Turrets.** EVE hit chance is `0.5^((ω·40000/(tracking·sig))² + (max(0,d−optimal)/falloff)²)` ([EVE Uni: Turret mechanics](https://wiki.eveuniversity.org/Turret_mechanics), [EVE Academy](https://www.eveonline.com/eve-academy/ships/combat-mechanics)). This one formula creates the "small ships orbit under big guns" meta.
- **Missiles.** EVE damage is `base × min(1, S/Er, (S·Ve/(Er·Vt))^drf)` (signature, explosion radius/velocity, target velocity). Missiles are real entities with flight time; Star Citizen adds lock types and countermeasures.
- **Capacitor.** EVE capacitor and shield regen follow `dC/dt = (10·Cmax/τ)(√(C/Cmax) − C/Cmax)`, peaking at 25%. Elite uses distributor "pips"; Star Citizen uses power allocation.
- **Layered defenses.** Shield → armor → hull with per-damage-type resists; SWG and Star Citizen add directional shield faces.
- **Subsystem damage.** SWG components have HP plus armor HP; Star Citizen components are physical items in the hull, damaged by hit location.
- **EWAR.** ECM (probabilistic jam vs sensor strength), damps, target painters, tracking disruptors, webs, warp scramblers/disruptors. *Each is just an effect modifying attributes*, with stacking penalties, which is why the A0 kernel matters.

**Engine/backend support:**

- **Damage pipeline**: hit → damage packet (types, amount, source, tags) → resist → pool routing (shield/armor/hull or directional face) → subsystem routing by hit-zone collider tag → events (killmail, cue).
- **Ship component graph**: typed/sized slots and fitting budgets (CPU, powergrid, calibration); optionally a power/heat/coolant network solver at 1–4 Hz.
- **Weapons framework** with three delivery types: statistical (EVE formula), lag-compensated hitscan/ballistic, guided missile entities.
- **Sensor model**: emission attributes per entity; detection = f(emission, distance, sensor strength).

### A3. Ground combat styles

| Style | Examples | Server model | Engine need |
|---|---|---|---|
| **Tab-target** | SWG (pre-CU), SWTOR, classic MMOs | Server validates range, line of sight, facing and resources, then rolls hit, crit and mitigation | Global cooldown and ability queues, auto-attack, threat tables; tolerates 100–200 ms latency |
| **Action / telegraph** | WildStar, GW2, Destiny supers | Server evaluates shapes (cone, circle, line) at resolution time; clients draw telegraphs ahead of time | Shape query library, server-authoritative timing, dodge i-frames as tags |
| **FPS gunplay** | Destiny, Star Citizen FPS, PlanetSide 2 | Client-predicted movement and firing. Server rewinds hitboxes to the shooter's view time ("lag compensation") | Hitbox history buffer (~1 s), rewind queries in Jolt, spread and recoil patterns, aim assist (slowdown and magnetism) for controllers |

Destiny mixes player-hosted physics with server-side "activity" authority ([Truman, GDC 2015](https://www.gdcvault.com/play/1022246/Shared-World-Shooter-Destiny-s)). Lag-compensation references: [Valve](https://developer.valvesoftware.com/wiki/Lag_Compensation), [Bernier 2001](https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization). **Recommendation:** select the resolution strategy per ability (validate, shape query, rewind hitscan) over one damage pipeline, so a game can mix styles as Star Citizen does.

### A4. Boarding and multi-crew

Star Citizen's defining feature is walking inside moving ships (EVA, boarding, crewed turrets, engineering), built on **physics grids** (local reference frames) plus the Replication Layer and server meshing ([Replication layer](https://starcitizen.tools/Replication_layer), [Server Meshing Q&A](https://star-citizen.wiki/Comm-Link:18397/en), [CitizenCon 2025](https://hangarbase.org/news/star-citizen-the-expanded-server-mesh-the-future-of-the-verse-revealed-at-citizencon-2025)).

**Engine support:**

- **Hierarchical transforms** in the entity model and network protocol: positions replicate local-to-parent, and crossing grids (e.g. an airlock) re-parents atomically.
- **Physics per grid.** Jolt supports multiple `PhysicsSystem`s sharing shapes but not bodies ([Jolt Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)). Simulate interiors in ship-local space; inject ship acceleration as fictitious forces or cancel it with "artificial gravity".
- **Stations/seats** bind player input to subsystem controllers (pilot, turret, engineering, scanner), gated by org-backed crew roles; AI crew use the same stations.
- **Boarding**: doors, hackable locks and airlocks as state machines; interiors stream from the ship prefab; hull breach and decompression as effect zones.

### A5. Mining, salvage, exploration and scanning

- **Mining**:
  - EVE: cycle-based lasers move ore into the hold. Belts, anomalies and scheduled moon extractions.
  - Star Citizen: a laser-power "sweet spot" fracture minigame, with resistance and instability, then extraction.
- **Salvage**:
  - EVE: wrecks hold loot plus salvage, and the salvager module rolls a success chance.
  - Star Citizen: hull scraping into material volume, and full-ship breakdown.
- **Exploration and probe scanning**:
  - EVE cosmic signatures (data and relic sites with a hacking minigame, wormholes, gas) are found by positioning probes in a formation. The result strength depends on probe strength vs signature strength, and results resolve from sphere, to ring, to point.
  - Star Citizen uses a radar "ping" against emissions.

**Engine support:**

- A server-side **site spawner** (procedural "anomaly" entities with a lifetime, a signature strength and a content template).
- A generic **scan query** (strength vs signature, with deviation noise decreasing as strength rises).
- **Resource field** definitions (ore composition, depletion, respawn) that the economy telemetry can see.
- **Minigame hosting** in UI plus Lua or visual script.
- **Wreck entities** with loot tables.

### A6. Trading and hauling

EVE trade is a set of regional order books. Hauling emerges from price differentials plus risk: gank losses, and courier contracts with collateral. Star Citizen physicalizes cargo (SCU boxes, tractor beams, freight elevators).

**Engine support:**

- Cargo volume and mass attributes that affect movement.
- Optional physicalized cargo (container entities bound to a grid).
- A **contract service** covering courier, item exchange and auction, with collateral held in escrow.
- Route and risk information: kill-stat heatmaps, security levels.

### A7. Crafting and resource quality (SWG model)

SWG's crafting remains the benchmark:

- Resources spawn server-wide in time-limited "shifts", each with random quality stats. The SWGEmu Core3 server handles `res_quality` (OQ), `res_conductivity` (CD), `res_decay_resist` (DR), `res_heat_resist`, `res_cold_resist`, `res_flavor`, `res_malleability`, `res_potential_energy`, `res_shock_resistance` and `res_toughness`, and tracks whether a spawn is still "in shift" ([Core3 ResourceSpawnImplementation.cpp](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/objects/resource/ResourceSpawnImplementation.cpp)).
- Schematics weight those attributes into item experimental properties.
- Experimentation points and assembly rolls refine results. Factory runs mass-produce from a finished prototype.
- Players survey, harvest (placed harvester structures), stockpile scarce high-quality spawns, and trade them. This creates a genuine crafter economy.

EVE's equivalent uses blueprints (originals and copies, material and time efficiency research), industry jobs in structures, and invention.

**Engine/backend support:**

- **Resource spawn service** (Go): a scheduler creates resource types with stat rolls, per-planet density maps and despawn times.
- **Schematic definitions** in data (inputs, attribute weights, output attribute formulas).
- **Crafting session** state machine with minigame hooks.
- **Item instances** carrying rolled stats (JSONB) and a crafter signature.
- **Industry job queue** (time-based, offline-progressing jobs).

### A8. Player economy

Principles from EVE:

- Nearly everything is player-made.
- Destruction is a sink: ships are *destroyed*, not repaired forever.
- ISK **faucets**: bounties, mission rewards, incursion payouts, insurance payouts.
- ISK **sinks**: broker fees, sales tax, NPC-sold skillbooks and blueprints, LP-store costs, industry fees, structure fuel.
- CCP publishes Monthly Economic Reports that track these flows.

**Backend requirements:**

- **Double-entry currency ledger** in PostgreSQL. Every wallet change is a journal row with a reason code, which is also the telemetry source for faucets and sinks.
- **Order-book market service** in Go: per-region books, buy orders with range (station, system, N jumps, region), matching on place, fees as configurable sinks, expiry, escrow.
- **Tunable economic parameters** (tax rates, NPC price seeds, loot and bounty multipliers), hot-reloadable.
- **Analytics pipeline**: NATS events → warehouse. Dashboards for money supply, velocity, and price indices per item.

### A9. Territory control

- **EVE sovereignty**:
  - Alliances hold systems through structures.
  - Capture uses timers and "vulnerability windows" chosen by the owner, which gives timezone fairness. The Entosis-link capture mechanic dates from 2015.
  - The 2024 *Equinox* redesign added Sovereignty Hubs whose upgrades consume **power** and **workforce**, plus resource-extracting structures (Skyhooks). This ties territory to industry. (Details come from CCP's Equinox material, which I could not re-fetch in this session.)
- **SWG Galactic Civil War**:
  - Players declare Rebel or Imperial and toggle combatant/special-forces status (PvP flagging).
  - PvE and PvP activity earns faction points and ranks.
  - Player faction bases can be attacked, and planetary control shifts from aggregated activity.

**Engine support:**

- **Structure entities** with a lifecycle state machine: anchoring, online, reinforced, vulnerable, destroyed.
- A **scheduled timer service** (orchestration): timers must survive restarts and fire across servers.
- An **influence model** per region: accumulators with decay.
- **Faction and PvP-flag state** (see A16).
- Large-battle support: time dilation, and pre-scaling zone capacity via orchestration when a timer is known in advance.

### A10. Player housing, cities and bases

- **SWG**: houses placed almost anywhere on open terrain, fully decoratable interiors (free-transform furniture), vendors inside houses, and **player cities** (mayor elections, civic structures unlocked by population rank, taxes, zoning).
- **EVE**: Upwell structures anchored in space.
- **Star Citizen**: planned base building.

**Engine support:**

- **Runtime placement validation**: footprint, slope, and no-build zones.
- **Terrain stamps** that flatten or deform procedural terrain; they must feed terrain LOD and collision.
- **Persistent placed-object streaming** and HLOD for dense towns.
- **Decoration**: item-in-world with transform, stored in the item service.
- Permission ACLs, maintenance costs (sinks), decay and condemnation.
- **City governance data** (votes, taxes, ranks) in a Go service.

### A11. Guilds and corporations

EVE corporations and alliances are the gold standard:

- Roles and titles with fine-grained permissions (hangar divisions, wallet divisions, structure access).
- Corp taxes, shareholder model, war declarations, recruitment.
- Guilds own assets, structures and ships.

**Backend:**

- A guild service with hierarchical membership (corp → alliance/coalition) and role-based ACLs, evaluated by the item, market and structure services. The role model should be shared, not reimplemented per service.
- Org-owned wallets on the same ledger.
- Audit logs.
- Chat channels auto-provisioned.

### A12. Quests and dynamic events

- **Guild Wars 2**:
  - No quest giver: events start in the open world and anyone nearby joins.
  - Events scale with participant count and branch on success or failure into **chains** and **meta-events**.
  - Rewards are graded by **contribution** tiers ([GW2 Wiki: Event](https://wiki.guildwars2.com/wiki/Event)).
- **Rift**: rifts and invasions open dynamically across zones.
- **EVE**: Incursions, and the Triglavian invasion (2019–2020), a months-long live campaign that moved NPC fleets system to system.
- **Star Citizen**: mission generation driven by the economy simulation (A13).

**Engine support:**

- An **event director** (server-side): an authored graph per event (visual script plus Lua) with phases, objectives, participant registry and contribution scoring.
- Scaling curves driven by participant count.
- **World-state flags** persisted and replicated, e.g. "outpost captured" changes vendors and spawns.
- **Cross-zone meta-event coordination** via NATS.
- Instanced quests (SWTOR-style phasing) as a layer: per-player visibility filters in interest management.

### A13. NPC AI

| Technique | Use | Reference |
|---|---|---|
| **Behavior trees** | Moment-to-moment reactive behavior, designer-authorable | Isla, "Handling Complexity in the Halo 2 AI" ([GDC 2005](https://www.gamedeveloper.com/programming/gdc-2005-proceeding-handling-complexity-in-the-i-halo-2-i-ai)) |
| **Utility AI** (Infinite Axis Utility System) | Choosing among many actions by scoring response curves. Proven at MMO scale in *Guild Wars 2: Heart of Thorns* | Lewis & Mark, "Building a Better Centaur: AI at Massive Scale" ([GDC 2015](https://www.gdcvault.com/play/1021848/Building-a-Better-Centaur-AI)) |
| **GOAP / HTN** | Multi-step planning (e.g. a boarding squad planning a breach). F.E.A.R. used GOAP; Guerrilla's Killzone and Horizon use HTN | Orkin, [GOAP](https://alumni.media.mit.edu/~jorkin/goap.html) |
| **Fleet/squad AI** | EVE's newer NPCs (Sleepers, Incursion Sansha, Drifters, Triglavians) switch targets by threat, remote-repair each other, use EWAR, and act as fleets | CCP expansion notes (qualitative) |
| **Ecosystem / economy simulation** | Star Citizen's "Quantum" models abstract economic agents whose supply and demand drive prices, NPC traffic and missions; X4 simulates NPC trade fleets | CIG CitizenCon material (qualitative) |

**Engine support:**

- An **AI framework** combining a BT runtime, utility scorer and HTN planner, all sharing a **blackboard** and a **perception system**. Perception uses the same sensor/signature model as players.
- **AI LOD**: full AI near players, low-frequency AI in unobserved zones, and statistical "virtual" simulation where nobody is present. This is the Quantum approach, and it is how you afford a living galaxy.
- **Squad/fleet coordinators** that issue orders to members.
- Steering and formation primitives for space (Reynolds-style seek, arrive, orbit), reusing the A1 movement models.
- A server budget per zone, with AI ticks spread across frames.

### A14. Stats, attributes, modifiers and buffs

Covered by the A0 kernel. Design details:

- **Derived attributes** (e.g. `EffectiveHP = f(hp, resists)`) are computed lazily with dependency tracking.
- **Skills** can follow the EVE model, where time-trained skills are permanent modifiers, or SWG's skill boxes.
- **Implants and boosters** are items that apply effects.
- **Formula data** must live in one shared C++ library linked into client, zone server, and a WASM or CLI "fitting simulator" for tools and the website. EVE's third-party ecosystem (Pyfa) shows players will build this anyway.

### A15. Inventory, equipment and loadouts

- **EVE item model**: every item has a type, owner, *location* (another item, station or space) and a *flag* (slot or hangar), plus a quantity for stacks. The result is a single tree of containers.
- **Star Citizen** physicalizes items (persistent entity streaming).
- **Loadouts**: fitting validation against slot, CPU, grid and skill requirements. Saved fittings can be shared (EVE fittings are an exchangeable format).

**Backend:**

- An **item service** (Go + PostgreSQL) with transactional moves.
- **Single-writer ownership**: the zone server owns items in the world, the item service owns items at rest, with optimistic version checks against duping.
- An **audit trail** for every move.
- Stackable vs unique (rolled stats) items.
- Bulk queries for "asset list" UIs.

### A16. Factions, reputation and law

- **EVE standings**: −10 to +10 per NPC corp and faction, with derived effects (agent access, taxes). Player-set standings drive overview colors.
- **Security status and CONCORD** (NPC police in high-security space), with **crimewatch timers**: weapons, suspect, criminal and logoff timers.
- **SWG**: overt, covert and on-leave faction status controls PvP eligibility.

**Engine support:**

- A **standings matrix** service, with propagation rules (hurting faction A raises standing with its enemies).
- A **PvP eligibility function** evaluated server-side for every hostile action: zone security × flags × standings × war declarations.
- A generic **timer framework** attached to entities as tags with expiry.

### A17. Death, respawn, insurance and clones

| Game | Model |
|---|---|
| EVE | Ship destroyed → capsule (pod). Pod destroyed → wake in a medical clone and lose implants. Each fitted item has a ~50% drop chance. Ship insurance pays out (a faucet). Public *killmails*. |
| SWG | Incapacitation then death. Clone at a cloning facility. Item insurance. Wounds and battle fatigue. |
| Star Citizen | Medical-bed imprint respawn locations. Hull insurance with claim timers. Loot remains on bodies and ships. |
| Destiny | Revive tokens and timers, no item loss. |

**Engine support:** a configurable **death pipeline**:

damage → incapacitation (optional) → death → loot resolution (per-item drop rolls, wreck creation) → killmail event → respawn-point selection (bind points, clones) → penalties (as effects) → insurance claim (contract service).

### A18. Character creation and species

- **SWG**: ten playable species with sliders, species-specific skeletons and some species-restricted gear.
- **EVE**: a detailed face sculpting tool.
- **Star Citizen**: "DNA" blending between scanned heads.

**Engine support:**

- Morph targets plus bone-scale parameters, serialized as a compact parameter vector.
- GPU skinning with blend shapes.
- Species definitions in data: skeleton, retarget map, allowed equipment, base attributes.
- **Equipment fitting across body types**: per-species variants or runtime shrink-wrap deformation.
- Skin, tattoo and paint compositing into virtual textures.

---

## Part B — Graphics Techniques for the Sci-Fi Look (Vulkan 1.3)

### B0. Renderer baseline

The baseline is a render graph with automatic barriers (`VK_KHR_synchronization2`, core in 1.3), dynamic rendering (core 1.3), timeline semaphores, buffer device address and descriptor indexing ("bindless"). On top of that sit GPU-driven culling and indirect draws. Mesh/task shaders via [`VK_EXT_mesh_shader`](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_mesh_shader.html) and ray queries are extensions and need fallbacks. zeux's [niagara](https://github.com/zeux/niagara) is a compact Vulkan reference for mesh shaders, meshlet occlusion culling via a depth pyramid, bindless textures, dynamic rendering and ray-traced shadows.

**Lighting architecture recommendation: clustered forward+ with a thin G-buffer (depth, normals, motion) or a visibility buffer.** Sci-fi scenes are dominated by transparents and emissives (shields, holograms, plumes, particles), MSAA-friendly cockpit HUDs, and many small lights. Classic deferred handles those poorly.

### B1. Precision and scale

- **64-bit world positions** (doubles) in simulation. **Camera-relative rendering**: compute `objPos − camPos` in double on the CPU (or with two-float emulation on the GPU for vertex-level precision), then upload float matrices. See Ohlarik's "Precisions, Precisions" ([AGI blog](https://help.agi.com/AGIComponents/html/BlogPrecisionsPrecisions.htm)) and Cozzi & Ring, *3D Engine Design for Virtual Globes* ([virtualglobebook.com](https://www.virtualglobebook.com/)).
- **Reverse-Z with a D32_SFLOAT depth buffer and an infinite far plane** gives near-uniform precision over planetary distances ([NVIDIA: Depth Precision Visualized](https://developer.nvidia.com/content/depth-precision-visualized)).
- **Scaled-space / layered rendering**: draw distant planets, moons and stars in a separate pass with scaled coordinates, then the near scene. Planetary terrain tiles store per-tile origins.
- **Physics**: Jolt float mode is accurate to about 5 km. `JPH_DOUBLE_PRECISION` costs about 5–10% overall, and queries take a base offset ([Jolt Architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)). Combine it with per-grid physics systems (A4).

### B2. Physically based atmospheric scattering

- **Bruneton & Neyret 2008, "new implementation" 2017**: precomputes transmittance, 4D scattering and irradiance textures. It supports ozone and custom density profiles, and fixes the old horizon artifact ([site](https://ebruneton.github.io/precomputed_atmospheric_scattering/), [GitHub, BSD](https://github.com/ebruneton/precomputed_atmospheric_scattering)). Its downsides are an expensive recompute when atmosphere parameters change and artifacts from the high-dimensional LUT.
- **Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"** (EGSR; the UE4/5 SkyAtmosphere) ([paper](https://sebh.github.io/publications/egsr2020.pdf), [EG diglib](https://diglib.eg.org/items/8a3e5350-18b3-46bd-9274-3add5af88c75), [reference code](https://github.com/sebh/UnrealEngineSkyAtmosphere)):
  - **Transmittance LUT**: 256×64 in the paper.
  - **Multiple-scattering LUT**: 32×32. Isotropic multi-scatter is approximated as an infinite geometric series from a second-order evaluation, so there is no iterative precompute.
  - **Sky-View LUT**: 192×108, `R11G11B10_FLOAT`. A latitude/longitude parameterization with nonlinear packing near the horizon, per frame.
  - **Aerial-perspective froxel volume**: 32×32×32, `RGBA16F`.
  - These sizes and formats are confirmed in the reference `Game.cpp`.
  - All LUTs are cheap enough to rebuild **every frame**, so atmosphere parameters (Rayleigh, Mie, ozone/absorption, planet radius) can animate for weather, alien atmospheres or terraforming. From space it raymarches the atmosphere shell directly. This fits a sci-fi engine with many different planets better than Bruneton.
- Simpler fallback: O'Neil, [GPU Gems 2 ch. 16](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-16-accurate-atmospheric-scattering). Accessible walkthroughs: [Maxime Heckel](https://blog.maximeheckel.com/posts/on-rendering-the-sky-sunsets-and-planets/), [Shadertoy production sky](https://www.shadertoy.com/view/slSXRW).

**Vulkan notes:**

- Compute passes: transmittance and multi-scattering on parameter change, sky-view and aerial perspective per view.
- Sample aerial perspective in the forward pass and in the particle and cloud shaders.
- The sun color entering the atmosphere is a parameter, so alien stars (red dwarf, blue giant) work physically.

### B3. Planetary rendering

- **Topology**: a **cube-sphere** with a quadtree per face, using a spherified-cube mapping to reduce distortion.
- **LOD**:
  - **CDLOD** (Strugar 2009): a distance-based quadtree of regular grid patches with vertex **geomorphing**, so there is no popping and no stitching ([paper](https://aggrobird.com/files/cdlod_latest.pdf), [code](https://github.com/fstrugar/CDLOD)).
  - Alternatives: Ulrich's [Chunked LOD](http://tulrich.com/geekstuff/chunklod.html) with skirts, and Bruneton's [Proland](https://proland.imag.fr/) (GPU tile producers for planets).
- **Height generation**: GPU compute noise (ridged fBm, erosion-approximating noise) plus authored data, generated **per tile on demand into an atlas cache**. It feeds collision via async readback, or via a parallel CPU generator on the server.
- **Hardware tessellation vs mesh shaders**: tessellation is legacy and slow on some hardware. Prefer CDLOD grids with compute-generated vertices, or mesh shaders emitting patch meshlets.
- **Surface texturing**: procedural material splatting baked into a **virtual texture** (B12), plus near-field detail textures and GPU-instanced scatter (rocks, vegetation) from compute.
- **Rings**: an annulus mesh with a radial density/color profile texture, strong forward-scattering phase, a planet-shadow term (ray–sphere test), and ring shadows cast on the planet (analytic ray–plane test into the profile). Particle clumps or impostor debris for close fly-through.
- **Gas giants**: latitude bands from a 1D palette, animated with flow maps or curl-noise advection, and storms as vortex decals. Near approach needs a volumetric layer (Nubis-style raymarch with the Hillaire atmosphere); Star Citizen has shown volumetric gas-giant tech publicly.
- **Precision**: per-tile double origin, camera-relative vertices, reverse-Z (B1).

### B4. Volumetric clouds

- **Nubis** (Horizon Zero Dawn, SIGGRAPH 2015 Advances) ([slides](https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf)):
  - Raymarch a spherical shell.
  - Density from a 128³ Perlin-Worley base, a 32³ Worley detail texture and 2D curl-noise distortion.
  - A **weather map** gives coverage, precipitation and type; a height gradient per cloud type shapes profiles.
  - Lighting uses Beer's law × a "powder" term and Henyey-Greenstein phase with a silver lining.
  - Quarter-resolution **temporal reprojection** updates 1 of 16 pixels per frame.
  - Budget: about 2 ms on PS4.
- **Nubis Evolved / Nubis³** (Horizon Forbidden West) moves to **voxel clouds with SDF-accelerated marching** so players can fly through them ([Guerrilla: Nubis, Evolved](https://www.guerrilla-games.com/read/nubis-evolved), [Schneider](https://andrewschneider.artstation.com/projects/ZeXyPZ)). This is the right target for ships that fly through cloud decks.
- **Hillaire 2016** (Frostbite) adds energy-conserving integration and a unified sky/cloud/fog pipeline ([PDF](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf)).
- **From orbit**: blend to a 2D cloud-map shell at distance with the same weather map, so clouds stay consistent from surface to space.

### B5. Nebulae and starfields

- **Skybox nebula (EVE look)**: an HDR cubemap per region, painted or procedural and baked. **Use it as the IBL source for ships**: prefiltered specular plus spherical-harmonic diffuse. This is what gives EVE ships their nebula-tinted rim light against a single hard sun. Procedural generation: layered fBm/Worley emission and absorption raymarched offline into the cubemap.
- **Fly-through nebulae**: a raymarched volume (sparse 3D texture or brick map, emission + absorption + single scattering from embedded stars) at half or quarter resolution with temporal accumulation, sharing the cloud raymarch code. Density mipmaps for empty-space skipping.
- **Starfield**: catalog-based (Hipparcos/Gaia-like) or procedural. Render as HDR point sprites with a point-spread-function kernel whose *integrated* energy follows magnitude, with a minimum size of about 1–1.5 px to avoid aliasing and TAA shimmer. Stars go through exposure, so they vanish in daylight and when looking toward the sun. Extinction by atmospheric transmittance on planets. Exclude them from TAA history, or mark them reactive for the upscaler.
- **Galactic backdrop**: a low-frequency Milky Way band layer and distant galaxies as sprites.

### B6. HDR, exposure, bloom, flares and tone mapping

- **Physical units**: sun illuminance in lux, emissives in nits, exposure in EV100. Space ranges from roughly 120 klux of direct sun to almost nothing in shadow.
- **Auto exposure**: a compute luminance histogram (e.g. 256 bins), percentile-trimmed average and eye-adaptation smoothing ([Opsenica](https://bruop.github.io/exposure/)). Add exposure bias zones for cockpits and interiors. Consider **local tone mapping** for sun-lit hulls next to deep shadows.
- **Bloom**: Jimenez's CoD: Advanced Warfare downsample (13-tap, Karis average on the first mip) plus tent upsample, blended at a small energy-conserving ratio ([Jimenez 2014](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare)).
- **Lens flares**: a sprite-based sun flare with GPU-computed occlusion (sample the depth buffer, no CPU queries), screen-space ghosts and halos, anamorphic streaks for the "cinematic sci-fi" look. The physically based reference is Hullin et al. 2011 ([MPI](https://resources.mpi-inf.mpg.de/lensflareRendering/)).
- **Tone mapping**: AgX or an ACES-like filmic curve, plus per-zone grading LUTs. **HDR10/scRGB output** via swapchain color spaces.

### B7. PBR for hard-surface sci-fi

- **BRDF**: GGX metal/roughness ([Karis 2013](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf), [Lagarde & de Rousiers 2014](https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf)). Add **clear coat** (painted hulls), **anisotropy** (brushed metal), and sheen (fabric, flight suits).
- **Trim sheets and tiling materials**: hull and interior modules UV'd onto shared trim atlases (Insomniac's "The Ultimate Trim", GDC 2015, is the standard reference). This keeps a 1 km capital ship's texture memory bounded.
- **Layered materials**: base plus up to N layers (paint, dirt, wear, burn) blended by masks, vertex color and curvature/AO. **Runtime liveries and damage** come from per-instance parameters. This is how Star Citizen and EVE do ship skins.
- **Decals**: mesh decals for greebles and panel lines; **clustered decals** (bindless, in the light cluster grid, as in DOOM 2016) for damage scorches and faction markings ([Sousa & Geffroy 2016](https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pdf)).
- **Emissives**: in physical units with an animated mask channel (running lights, window grids, power-state flicker driven by gameplay attributes such as `Ship.Power.Offline`).

### B8. Holograms and energy shields

- **Holograms**: unlit additive or alpha transparent, with:
  - fresnel rim
  - scanlines in world or screen space
  - noise flicker and glitch displacement
  - chromatic offset
  - depth-fade intersection lines
  
  Volumetric holograms can be point-cloud splats or a raymarched 3D texture. Render in the transparent pass with **weighted-blended OIT** ([McGuire & Bavoil 2013](https://jcgt.org/published/0002/02/09/)) or sorted per object.
- **Energy shields**: an inflated convex hull or ellipsoid around the ship with a fresnel rim, a hex/noise pattern and an intersection glow against the hull depth.
  - **Impact ripples**: gameplay damage events become a GPU ring buffer of `(localPos, time, strength, damageType)`. The shader sums expanding rings by chord distance with exponential decay. Shield strength (a gameplay attribute) drives overall opacity, and directional faces map to shield sectors.
  - Hits flow as a *GameplayCue* (A0), so shield VFX are fully data-driven.

### B9. Engine plumes, trails and warp tunnels

- **Plumes**: nested cone meshes with a raymarched or analytic core (shock diamonds / Mach disks), with length, color and expansion driven by throttle and ambient pressure (vacuum vs atmosphere). Add a heat-distortion refraction pass and an attached dynamic light in the clustered light list.
- **Trails**: GPU ribbon particles with camera-facing strips, per-segment age fade and velocity-based width. Contrails only inside atmosphere.
- **Warp / quantum / hyperspace**: layered screen and camera effects:
  - star sprites stretched along velocity (starlines)
  - camera-space tunnel mesh with scrolling noise
  - radial blur and chromatic aberration
  - FOV kick
  - a flash on entry and exit
  
  EVE and Star Citizen tunnels are art-directed, not physical. Drive every parameter from the warp state machine (A1).

### B10. Weapons VFX, explosions and GPU particles

- **Beams**: camera-aligned cylinder strips with an HDR core and bloom halo, noise UV scroll, and an impact point from a ray query or the server result. **Bolts and projectiles**: stretched billboards whose length comes from velocity × exposure time. **Impacts**: spark particles, clustered decal, short light.
  - Clients spawn projectile visuals locally from replicated fire events with deterministic seeds; the server stays authoritative on damage.
- **Explosions**: flipbooks baked from simulation tools (e.g. [EmberGen](https://jangafx.com/software/embergen/)) with **motion-vector frame blending**, shockwave distortion rings, debris via authored fracture chunks plus Jolt, and a flash light. High end: sparse volumetric explosions raymarched like clouds.
- **GPU particle system (compute)**:
  - emit → simulate (curl-noise, attractors, depth-buffer collision) → compact → sort (radix/bitonic for alpha) → indirect draw
  - ribbons and beams as particle types
  - lighting via the clustered light list
  - GPU-driven compute patterns per [Wihlidal, GDC 2016](https://www.slideshare.net/gwihlidal/optimizing-the-graphics-pipeline-with-compute-gdc-2016)
  
  Budget: hundreds of thousands of particles for large battles, with LOD by distance and screen coverage.

### B11. Lighting: clustered shading, shadows and volumetric fog

- **Clustered shading**: a froxel grid (e.g. 16×8×24 with exponential depth slices, as in DOOM 2016). A compute pass assigns lights, decals and probes, supporting thousands of lights such as running lights, muzzle flashes, plumes and holograms ([Olsson et al. 2012](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf), [Persson, Practical Clustered Shading](http://www.humus.name/Articles/PracticalClusteredShading.pdf)).
- **Shadows**:
  - Sun: cascaded shadow maps fitted per view. In space, fit cascades to the ship cluster rather than the view frustum.
  - **Per-object shadow maps for capital ships.**
  - Local lights: shadowed through an atlas.
  - Later: virtual shadow maps and RT shadows (ray query).
- **Volumetric fog**: a froxel-based unified volumetric pass (Wronski 2014; [Hillaire 2015](https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite)) for hangars, station interiors, planetary fog, and "dust" in combat zones or nebula edges. The same froxels receive the aerial perspective.

### B12. Virtual texturing

Use software **sparse virtual texturing**: a feedback pass, page table and physical tile cache, with pages decoded or generated asynchronously ([Barrett, SVT](https://silverspaceship.com/src/svt/)). Uses:

- **Procedural planetary surfaces**: bake composited terrain materials into VT pages on demand, the adaptive/procedural VT approach from Far Cry 4.
- Character and ship customization composites.
- Virtual shadow maps.

Prefer software indirection over Vulkan sparse residency, whose driver performance varies.

### B13. TAA, upscaling and temporal effects

- **TAA**: Halton jitter, reprojection with motion vectors, and YCoCg neighborhood clamping or clipping ([Karis 2014](https://de45xmedrsdbp.cloudfront.net/Resources/files/TemporalAA_small-59732822.pdf)).
- **Sci-fi pitfalls**: sub-pixel bright stars, thin lasers, HUD glyphs and particles ghost or vanish. Render stars and HUD after TAA, or feed a reactive mask. Every particle and transparent must write motion vectors or a reactive mask.
- **Upscalers**: FSR 2 has a **Vulkan backend** (MIT). Inputs: color, depth, motion vectors, exposure (optional), reactive mask and transparency/composition mask ([FidelityFX-FSR2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2)). The latest [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) ships FSR 3.1.x, the FSR 4 ML upscaler and frame generation, but its README currently lists "Vulkan is currently not supported in SDK" as a known issue.
  - **Requirement:** an `IUpscaler` abstraction (own TAA, FSR 2/3.1-Vulkan, DLSS via Streamline, XeSS), all sharing the same inputs.
- **Temporal amortization everywhere**: clouds (1/16 pixels per frame), volumetrics, SSR, GI and nebula raymarch.

### B14. Global illumination options

| Tier | Technique | Notes for sci-fi |
|---|---|---|
| Raster baseline | IBL from the nebula skybox + local reflection probes, GTAO/SSAO, SSR | Probes must be **ship-local** (they move with grids). Baked lightmaps do not work for modular or customizable ships. |
| Mid | **DDGI** irradiance probe volumes with visibility ([Majercik et al. 2019, JCGT](https://jcgt.org/published/0008/02/01/)) | Probe grids parented to ships and stations. Update with ray queries, or a compute/SDF tracer where RT is unavailable. |
| High | Lumen-style hybrid SDF/RT GI ([SIGGRAPH 2022 Advances](https://advances.realtimerendering.com/s2022/index.html)), ReSTIR direct and GI ([Bitterli et al.](https://benedikt-bitterli.me/restir/)) | Many emissive panels and muzzle flashes make ReSTIR direct lighting attractive. Requires `VK_KHR_acceleration_structure` + `ray_query`; TLAS rebuilds must handle many moving ships. |

Space exteriors are mostly sun + IBL; GI matters most in interiors, hangars and planetary surfaces.

### B15. Large ships and fleet LOD

- **Cluster-based continuous LOD** (Nanite-like DAG of meshlet clusters with an error metric) for kilometre-scale capital ships and stations ([Karis 2021, Nanite](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advanced_Graphics.pdf)). [meshoptimizer](https://github.com/zeux/meshoptimizer) provides meshlet building and cluster simplification. Fallback: authored LODs plus HLOD for modular assemblies.
- **Octahedral impostors** for distant ships and fleets: 8×8–16×16 views in an atlas, with depth for parallax and blend across three views ([Brucks](https://shaderbits.com/blog/octahedral-impostors)). Beyond that, **brackets/icons** as EVE does, where the tactical overlay replaces geometry.
- **Instancing and GPU culling** (frustum, occlusion via HiZ, small-feature) for battles with thousands of hulls. The GPU scene uses persistent instance buffers updated by deltas from the simulation.

### B16. Cockpit and HUD rendering

- **Diegetic MFDs**: UI rendered to texture (a retained-mode game UI, not ImGui), sampled on cockpit meshes with emissive and screen-door/scanline material. Update at reduced rate. Star Citizen's MFD/HUD framework ("Building Blocks") is the reference for data-driven diegetic UI.
- **Collimated HUD / helmet visor**: project HUD elements *at infinity* so there is no parallax (essential for VR and head-tracking), and draw after TAA/upscaling to keep glyphs crisp. Target brackets are projected from double-precision positions.
- **Cockpit depth range**: render the cockpit and first-person view models in a separate depth range or FOV, like FPS weapon models, to avoid near-plane precision problems and clipping into the canopy. Also: glass with dirt/scratch layer, reflections of the HUD, and G-force vignette as post.

### B17. Art styles and what they demand from the engine

- **Destiny ("mythic sci-fi")**: painterly skies, saturated color keys, strong silhouettes, stylized-PBR materials, heavy art-directed VFX.
- **SWTOR**: stylized proportions and textures, which kept performance and longevity on the HeroEngine base.
- **Star Citizen ("used future photoreal")**: dense greebling, layered wear materials, physically based lighting, 1:1 scale.
- **EVE**: high-contrast single-sun lighting, nebula-tinted reflections, ships that read by silhouette at tiny screen sizes.

**Engine implications:**

- A material graph that can do both photoreal and stylized (custom ramps, rim terms).
- Per-zone post stacks and grading LUTs.
- Art-directable atmosphere and sky parameters, not just physical ones.
- Silhouette-preserving LOD.

---

## Requirements

### Gameplay framework and backend

**P0 (needed for the first playable)**

1. **Gameplay kernel**: data-driven attributes, effects (instant, duration, infinite, periodic), ordered modifier ops, stacking rules and EVE-style stacking penalties, resistances, hierarchical tags and abilities. Server-authoritative with owner-client prediction. One shared C++ formula library for client, server and tools.
2. **64-bit positions + hierarchical reference frames** (system → body → grid) in ECS, physics and the network protocol. Atomic re-parenting.
3. **Pluggable movement models**: kinematic command flight (EVE), Newtonian rigid body + PID flight assist with thruster allocation (Elite/SC), `CharacterVirtual` ground movement, hover/wheeled vehicle. Per-zone tick-rate profiles.
4. **Combat framework**: a damage pipeline (types → resist → layered pools → subsystem routing via hit-zone tags) and delivery strategies: statistical (turret formula), lag-compensated hitscan/ballistic, guided missile entities, telegraph shape queries.
5. **Sensor/targeting model**: signatures (radius, EM/IR/CS), lock time and count, scan queries.
6. **Ship/vehicle component graph** with slots, fitting budgets (CPU/grid) and per-component health.
7. **Item service**: location-tree items, stacks vs uniques, transactional moves, versioned single-writer ownership, audit log. Shared loadout validation.
8. **Double-entry currency ledger** with reason codes, and a **regional order-book market** + contracts (Go/PostgreSQL). Faucet/sink telemetry via NATS from day one.
9. **AI framework**: BT + utility scorer + blackboard + perception (same sensor model), with AI LOD tiers and squad/fleet coordinators.
10. **Timers, standings and PvP eligibility** services (crimewatch-style timers, faction standings, flags).
11. **Configurable death pipeline**: loot drop rolls, wrecks, killmail events, respawn points, insurance hooks.
12. **Event/quest director** authored in the visual graph + LuaJIT, with persisted world-state flags.

**P1**

- Multi-crew stations and boarding: per-grid Jolt `PhysicsSystem`s, seat/station binding, doors and airlocks.
- Time dilation per zone.
- SWG-style resource spawns with quality stats, schematics, experimentation, offline industry jobs.
- Territory structures with state machines and owner-chosen vulnerability windows. Durable cross-server scheduler.
- Housing placement with terrain stamps, decoration, permissions, maintenance sinks.
- GW2-style scaling dynamic events with contribution tiers and cross-zone meta-events.
- Guild/corp RBAC shared across services; org wallets.
- Character customizer: morphs, bone scales, species definitions.
- Mining, salvage and scanning minigames. Procedural site spawner.

**P2**

- Galaxy-scale economy/ecosystem simulation (Quantum-like agents) driving NPC traffic and missions.
- Player-city governance (elections, zoning, taxes).
- Influence-map GCW-style planetary control.
- Physicalized cargo.
- Power/heat/coolant resource-network solver.
- Public killmail/market APIs for third-party tools.

### Renderer

**P0**

1. Vulkan 1.3 render graph (sync2, dynamic rendering, timeline semaphores), bindless descriptors, GPU-driven culling + indirect draws.
2. Camera-relative rendering from double positions. Reverse-Z D32F with infinite far plane. Scaled-space pass for distant bodies.
3. HDR pipeline in physical units: histogram auto-exposure, CoD-AW bloom, AgX/ACES tone mapping + grading LUTs, HDR10 output.
4. PBR metal/rough with clear coat, anisotropy, emissive (nits), layered materials with runtime livery/wear parameters, mesh and clustered decals.
5. Clustered forward+ lighting for thousands of lights. CSM + per-capital-ship shadows + local shadow atlas.
6. Transparency path: weighted-blended OIT for shields and holograms, soft particles. Data-driven shield-impact ring buffer fed by gameplay cues.
7. Compute GPU particles (sim, sort, indirect, ribbons, beams) lit by clusters, with motion vectors or reactive masks.
8. TAA plus an `IUpscaler` abstraction (FSR 2/3.1 Vulkan, DLSS/Streamline, XeSS). Stars and HUD composited post-upscale.
9. HDR nebula skybox as IBL. PSF starfield with exposure-aware brightness.
10. Hillaire 2020 atmosphere (per-frame LUTs, per-planet parameters, space view).

**P1**

- Cube-sphere CDLOD planet terrain with GPU tile generation and cache, plus procedural virtual texturing.
- Nubis-style volumetric clouds (weather map, temporal reprojection), orbit-to-surface consistent.
- Froxel volumetric fog.
- Planetary rings and animated gas giants.
- Mesh-shader/meshlet path with cluster LOD (meshoptimizer) for capital ships; octahedral impostors and bracket LOD for fleets.
- Engine plumes, heat distortion, warp/quantum tunnel stack; flipbook explosions with motion-vector blending.
- Lens flares with GPU occlusion.
- GTAO, SSR, ship-local reflection probes; DDGI probe volumes parented to grids.
- Diegetic MFD render-to-texture and a collimated HUD layer.

**P2**

- Hardware RT: ray-query shadows and reflections, ReSTIR direct/GI, a TLAS strategy for many moving ships.
- Voxel/SDF fly-through clouds (Nubis³) and fly-through volumetric nebulae.
- Sparse volumetric explosions.
- Frame generation.
- Fully virtualized geometry (Nanite-class).
- Local tone mapping.

---

## Sources

**Gameplay: EVE Online**
- EVE Academy, Flying Your Ship: https://www.eveonline.com/eve-academy/ships/flying-your-ship
- EVE Academy, Combat Mechanics: https://www.eveonline.com/eve-academy/ships/combat-mechanics
- EVE Support, System Travel – Warping: https://support.eveonline.com/hc/en-us/articles/115004925685-System-Travel-Warping
- EVE University Wiki, Acceleration: https://wiki.eveuniversity.org/Acceleration
- EVE University Wiki, Turret mechanics: https://wiki.eveuniversity.org/Turret_mechanics
- EVE University Wiki, Advanced piloting techniques: https://wiki.eveuniversity.org/Advanced_piloting_techniques
- EVE Developer Docs, Useful Formulae: https://developers.eveonline.com/docs/guides/useful-formulae/
- chruker EVE math (community formulas): http://games.chruker.dk/eve_online/eve_math.php
- High Scalability, EVE Online Architecture: https://highscalability.com/eve-online-architecture/
- CCP, "My node was equipped with the following...": https://www.eveonline.com/news/view/my-node-was-equipped-with-the-following...
- Talk Python #52, EVE Online powered by Python: https://talkpython.fm/episodes/show/52/eve-online-mmo-game-powered-by-python
- Pyfa (EVE fitting / Dogma reimplementation): https://github.com/pyfa-org/Pyfa

**Gameplay: Star Citizen, Elite Dangerous, SWG**
- Star Citizen Wiki, Master Modes: https://starcitizen.tools/Master_Modes
- Star Citizen Wiki, IFCS: https://starcitizen.tools/Intelligent_Flight_Control_System
- Master Modes Guide (comm-link): https://api.star-citizen.wiki/comm-links/20053
- Star Citizen Wiki, Replication layer: https://starcitizen.tools/Replication_layer
- Server Meshing & Persistent Streaming Q&A: https://star-citizen.wiki/Comm-Link:18397/en
- Hangarbase, CitizenCon 2025 server mesh: https://hangarbase.org/news/star-citizen-the-expanded-server-mesh-the-future-of-the-verse-revealed-at-citizencon-2025
- Elite Dangerous Wiki, Flight Assist: https://elite-dangerous.fandom.com/wiki/Flight_Assist
- Elite Dangerous Wiki, Flight Model: https://elite-dangerous.fandom.com/wiki/Flight_Model
- SWG Wiki, Jump to Lightspeed basics: https://swg.fandom.com/wiki/Jump_to_Lightspeed_basics
- SWG Legends Wiki, JTL Basics: https://swglegends.com/wiki/index.php?title=Jump_to_Lightspeed_Basics
- GameSpot, JTL Q&A: https://www.gamespot.com/articles/star-wars-galaxies-jump-to-lightspeed-updated-qanda/1100-6102072/
- SWGEmu Core3 (SWG server emulator): https://github.com/swgemu/Core3
- Core3 ResourceSpawnImplementation.cpp (resource attributes and shifts): https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/objects/resource/ResourceSpawnImplementation.cpp

**Gameplay: frameworks, AI and networking**
- GAS Documentation (tranek): https://github.com/tranek/GASDocumentation
- Jolt Physics: https://github.com/jrouwe/JoltPhysics
- Jolt Architecture doc (double precision, multiple PhysicsSystems, determinism, CharacterVirtual): https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md
- Guild Wars 2 Wiki, Event: https://wiki.guildwars2.com/wiki/Event
- Lewis & Mark, "Building a Better Centaur: AI at Massive Scale" (GDC 2015): https://www.gdcvault.com/play/1021848/Building-a-Better-Centaur-AI
- Orkin, Goal-Oriented Action Planning: https://alumni.media.mit.edu/~jorkin/goap.html
- Isla, "Handling Complexity in the Halo 2 AI" (GDC 2005): https://www.gamedeveloper.com/programming/gdc-2005-proceeding-handling-complexity-in-the-i-halo-2-i-ai
- Truman, "Shared World Shooter: Destiny's Networked Mission Architecture" (GDC 2015): https://www.gdcvault.com/play/1022246/Shared-World-Shooter-Destiny-s
- Valve, Lag Compensation: https://developer.valvesoftware.com/wiki/Lag_Compensation
- Bernier, Latency Compensating Methods: https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization

**Graphics: sky, atmosphere and clouds**
- Hillaire 2020, EGSR paper: https://sebh.github.io/publications/egsr2020.pdf
- Hillaire 2020, EG Digital Library: https://diglib.eg.org/items/8a3e5350-18b3-46bd-9274-3add5af88c75
- Hillaire 2020, reference code: https://github.com/sebh/UnrealEngineSkyAtmosphere
- Bruneton, Precomputed Atmospheric Scattering (new implementation): https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Bruneton, code: https://github.com/ebruneton/precomputed_atmospheric_scattering
- O'Neil, GPU Gems 2 ch. 16: https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-16-accurate-atmospheric-scattering
- Maxime Heckel, On Rendering the Sky, Sunsets, and Planets: https://blog.maximeheckel.com/posts/on-rendering-the-sky-sunsets-and-planets/
- Shadertoy, Production Sky Rendering: https://www.shadertoy.com/view/slSXRW
- Schneider 2015, Real-time Volumetric Cloudscapes of Horizon Zero Dawn: https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
- Guerrilla, Nubis, Evolved: https://www.guerrilla-games.com/read/nubis-evolved
- Schneider, Nubis Evolved (ArtStation): https://andrewschneider.artstation.com/projects/ZeXyPZ
- Hillaire 2016, Frostbite sky and clouds: https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
- Hillaire 2015, Unified Volumetric Rendering in Frostbite: https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite

**Graphics: terrain, precision, lighting and materials**
- Strugar, CDLOD paper: https://aggrobird.com/files/cdlod_latest.pdf
- Strugar, CDLOD code: https://github.com/fstrugar/CDLOD
- Ulrich, Chunked LOD: http://tulrich.com/geekstuff/chunklod.html
- Proland: https://proland.imag.fr/
- Cozzi & Ring, 3D Engine Design for Virtual Globes: https://www.virtualglobebook.com/
- Ohlarik, Precisions, Precisions: https://help.agi.com/AGIComponents/html/BlogPrecisionsPrecisions.htm
- NVIDIA, Depth Precision Visualized: https://developer.nvidia.com/content/depth-precision-visualized
- Olsson et al. 2012, Clustered Shading: https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf
- Persson, Practical Clustered Shading: http://www.humus.name/Articles/PracticalClusteredShading.pdf
- Sousa & Geffroy 2016, idTech 6 (DOOM): https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pdf
- Karis 2013, Real Shading in UE4: https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
- Lagarde & de Rousiers 2014, Moving Frostbite to PBR: https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
- McGuire & Bavoil 2013, Weighted Blended OIT: https://jcgt.org/published/0002/02/09/

**Graphics: post-processing, temporal and upscaling**
- Karis 2014, High Quality Temporal Supersampling: https://de45xmedrsdbp.cloudfront.net/Resources/files/TemporalAA_small-59732822.pdf
- Jimenez 2014, CoD: Advanced Warfare post-processing: https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
- Hullin et al. 2011, Physically-Based Lens Flare: https://resources.mpi-inf.mpg.de/lensflareRendering/
- Opsenica, Automatic Exposure: https://bruop.github.io/exposure/
- AMD FidelityFX FSR2: https://github.com/GPUOpen-Effects/FidelityFX-FSR2
- AMD FidelityFX SDK: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK

**Graphics: GI, geometry, texturing, VFX and Vulkan**
- Majercik et al. 2019, DDGI: https://jcgt.org/published/0008/02/01/
- ReSTIR (Bitterli et al.): https://benedikt-bitterli.me/restir/
- SIGGRAPH 2022 Advances (Lumen): https://advances.realtimerendering.com/s2022/index.html
- Karis 2021, Nanite: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advanced_Graphics.pdf
- meshoptimizer: https://github.com/zeux/meshoptimizer
- niagara (Vulkan GPU-driven renderer): https://github.com/zeux/niagara
- Brucks, Octahedral Impostors: https://shaderbits.com/blog/octahedral-impostors
- Barrett, Sparse Virtual Textures: https://silverspaceship.com/src/svt/
- Wihlidal 2016, Optimizing the Graphics Pipeline with Compute: https://www.slideshare.net/gwihlidal/optimizing-the-graphics-pipeline-with-compute-gdc-2016
- JangaFX EmberGen: https://jangafx.com/software/embergen/
- Khronos, VK_EXT_mesh_shader: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_mesh_shader.html

*Qualitative claims without a fetched source in this session:* EVE sovereignty (Equinox), EVE NPC AI generations, Star Citizen Quantum, and Destiny/SWTOR art-direction descriptions. They come from developer presentations and expansion notes and should be re-verified against primary sources when those domains are reachable.
