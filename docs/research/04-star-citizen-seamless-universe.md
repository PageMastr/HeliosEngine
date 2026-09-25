# 04 — Star Citizen / StarEngine and Other Seamless-Universe Space Games

> Research report for the sci-fi MMO engine project. Topic: how Cloud Imperium Games (CIG) built a seamless universe on StarEngine (Star Citizen, Squadron 42), plus lessons from Elite Dangerous, No Man's Sky, Dual Universe and Starfield.
>
> **Trust note.** WebFetch was blocked for every domain tried, and the WebSearch budget ran out partway through. `[n]` claims come from search-result extracts of the cited sources. **(bk)** claims come from background knowledge (to mid-2026) and were **not re-checked**; verify them before treating them as design-critical. † sources were not reached this session.

---

## 1. Overview

Star Citizen (SC) is the most ambitious public attempt at a *seamless*, *physicalized*, *persistent* space MMO: hangar on a rotating planet → multi-crew ship → orbit → quantum travel → boarding another ship, with no loading screen. The engine began as CryEngine 3, moved to Lumberyard in 2016 **(bk)**, and is now rebuilt enough to be called **StarEngine** [6]. The layers that matter to us:

| Layer | SC system | Purpose |
|---|---|---|
| Spatial | 64-bit world positions, camera-relative rendering, **Zone System** [7][6] | Precision at solar-system scale; one spatial partition for render, update and network |
| Physics | **Local physics grids** (ship interiors, stations, nested) [9] | Walking inside moving ships |
| Streaming | **Object Container Streaming** (client 3.3, server 3.8) [4][14][15] | No loading screens; bounded memory |
| Scheduling | **Entity-Component Update Scheduler**, Network Bind Culling [4][13] | Skip or throttle far-away components and network updates |
| Persistence | iCache → **EntityGraph** + **Persistent Entity Streaming** (3.18) [3][40] | Everything persists in a shard |
| Networking | **Replication Layer** ("Hybrid" = Replicant + Atlas + Scribe + Gateway) [1][2] | Split replication and persistence out of the game server |
| Scale-out | **Static Server Meshing** (4.0, Dec 2024), Dynamic Meshing (in progress) [17][18][19] | Many dedicated game servers (DGS) per shard |
| World gen | Planet Tech v2→v4→v5 / **Genesis** [23][24][25] | Procedural planets, biomes, ecosystems |
| Rendering | **Gen12** renderer + **Vulkan**, default as of 4.5 [29][31] | Multithreaded render backend |
| Gameplay sims | IFCS flight model, Master Modes, resource network (engineering), Maelstrom destruction, Quantum economy [35][37][38][32] | Physically grounded, systemic gameplay |

**Main lesson:** SC's seamlessness comes from a small number of cross-cutting architectural decisions:
1. Hierarchical coordinate frames.
2. Streamable containers as the one unit of content.
3. Entity authority that is independent of which process simulates the entity.
4. A replication and persistence service that sits between simulation servers and clients.

CIG added all four to an engine that was never designed for them, and that retrofit explains most of its roughly 10-year delay. We can design them in from day one.

---

## 2. Large World Coordinates & Physics Grids

### 2.1 64-bit positions, 32-bit rendering

- CIG made CryEngine store world positions as doubles (not a whole-engine conversion). Rendering stays 32-bit, and **all rendering code was made camera-relative**, so the 64-bit world costs nothing on the GPU [7][6][12][43].
- Arithmetic: at 4×10¹⁰ m (roughly an SC system) a float32 step is about 2.4 km. A double's step is about 2 mm at 2⁴³ m (59 AU) and 0.25 m at 2⁵⁰ m (0.12 ly). **Doubles cover one star system, not a galaxy**, so interstellar games need a per-system frame (or int64 sector) on top.
- Community analysis of CIG's spatial patents describes **spatial codes made by interleaving X/Y/Z bits** (Morton order), so objects near each other in space (including a ship's contents) are near each other in memory [11].

### 2.2 The Zone System (hierarchical frames)

- The Zone System **replaced CryEngine's octree** as the spatial partition. It suits "large, dynamic maps, huge amounts of entities and large moving ships" [6][7].
- Chris Roberts: most engines have separate partitions for rendering, entity updates and network traffic. The Zone System **combines rendering, visibility and occlusion, updating, and network relevance in one hierarchy** [8].
- Zones nest logically. Example: "orbit around a planet" holds the planet, its stations and the ships around it. Ships and stations hold their own interior zones [8][12]. A **zone host** can be a celestial body, a ship, a transit car or a space station [12].
- Each zone is a local frame, so a cup in a ship in orbit stays numerically stable however far it is from the system origin.
- **(bk)** SC planets *rotate* but do *not orbit*. That keeps navigation and streaming simple, but every surface object sits in a rotating frame. Decide early whether our bodies orbit: orbits turn every static location into a moving one.

### 2.3 Local physics grids

- The August 2014 report introduced the **localized physics grid**: "the global grid tracks each smaller grid as an individual and that smaller grid tracks all objects within it relative to whatever it is attached to", so a player in a Constellation moves "in perfect unison" with it [9].
- Grids nest (a vehicle in a cargo bay in a carrier in a station hangar). Moving between grids re-parents the object and converts its position and velocity. Grid-transition bugs are among SC's longest-running problems **(bk)**: falling through moving ships, ships "exploding" on pads, elevator deaths. They occur at frame boundaries, under latency, with authority split between client and server.

**Implication for us (Jolt).** Jolt supports a double-precision build (`JPH_DOUBLE_PRECISION`) **(bk)**, which handles large frames (planet surfaces). It has no concept of moving reference frames. We should build **physics grids on top of Jolt as separate `PhysicsSystem` instances per interior grid** (ship interiors, stations, planets):
- Each grid simulates in its own local (non-inertial) frame. Artificial gravity is a grid property, and the host's acceleration is either ignored or applied as a fictitious force.
- The ship's *hull* is one rigid body in the parent grid.
- Grid transfer is an explicit engine operation. It is triggered by authored *grid volumes* with hysteresis, and it moves the body between systems while converting its transform and velocity.

---

## 3. Streaming (OCS/PES) & Persistence

### 3.1 Object Container Streaming (OCS)

- OCS is "the umbrella term for all the technology in Star Engine that makes a vast seamless universe possible" [4]. Content is split into **object containers** (nested, prefab-like units: station, landing zone, interior, ship) that stream asynchronously as players approach [4][15].
- **Client OCS (Alpha 3.3, Dec 2018):** far entities are tracked only by the server, not by every client, which gave large FPS gains [4][15].
- **Network Bind Culling** decides "what entities to load and unload on any client" [4][13].
- **Entity-Component Update Scheduler:** updates components "based on how they are spatially placed relative to the player" and skips far ones, with more **update policies** being added [4]. Network tick rates also drop with distance ("the further away an entity is, the more ticks are skipped") [13].
- **Server OCS (SOCS, Alpha 3.8, Dec 2019):** with no player nearby, an object is frozen and its **state serialized to a database** [14][16]. This enabled microTech and **cut designer iteration from 5–7 minutes to seconds** [14].

### 3.2 Persistence: iCache → EntityGraph → PES

- **(bk)** **iCache** (around Alpha 3.15, 2021) persisted physicalized items for "global persistence". EntityGraph grew out of it.
- **EntityGraph**: "a highly scalable service built on top of a highly scalable database" storing **every network-replicated entity** as a graph (hierarchy, item ports, inventories) [3][1].
- **PES (Alpha 3.18, early 2023):** bodies, items, ships and debris stay in the shard after players leave or log out. The Replication Layer persists entity state into the graph database [1][3][40]. Streaming decides what is *loaded*; persistence means unloaded things still *exist*.
- **(bk) Post-3.18 pitfalls:** severe database and service instability at launch; **entity bloat** (abandoned ships, loot, corpses) hurting server FPS and database load until cleanup and decay rules arrived; and a move to long-lived *shards* with players tied to a home shard.
- Lesson: **persistence and cleanup policy are one feature.** Every persistent entity class needs a data-driven, observable lifetime policy (despawn timer, ownership retention, decay, pruning).

---

## 4. Replication Layer & Server Meshing

This is the core of SC's scalability. Below is the public description, followed by a concrete handoff protocol we could build.

### 4.1 Architecture (as described by CIG)

1. **The replication layer is its own service.** "The streaming and replication logic was moved out of the dedicated server into the 'Replication' layer, which now hosts the network replication and entity-streaming code." Clients and game servers both connect to the replication layer, not to each other [1][2].
2. **It is a set of microservices** with names like **Replicant** (holds replicated entity state and streams it), **Atlas** (the coordinating or assignment service; **(bk)** understood to decide which server node gets which territory and authority), **Scribe** (**(bk)** writes entity state to persistence), and **Gateway** [1][2]. "The Hybrid service is a hybrid of the Replicant, Atlas, Scribe, and Gateway services (but not EntityGraph)" [1][2]. Hybrid was the single-process first version that shipped with PES in 3.18; the parts can be split out to scale later.
3. **Gateway.** "Clients don't connect directly with replicants, but instead connect to a Gateway node... the Gateway service merely relays packets between clients and the various replicants they communicate with." It holds no game state, so it recovers in seconds [1].
4. **Server nodes are clients with extra duties.** DGS nodes get **streaming bubbles**. These make the replication layer send them the entities inside the bubbles, "and unlike a player's client, the server node has the additional responsibility to execute server-side authoritative code for those entities." Replication to server nodes "is controlled by the network bind culling algorithm driven by streaming bubbles," the same way it works for clients [1][4].
5. **Entity authority.** "Any given entity is no longer owned by a single dedicated game server, but instead there are multiple server nodes in the mesh — one server node that controls the entity, and multiple other server nodes that have a client view." Streaming bubbles may overlap, so "to avoid two server nodes trying to simulate the same entity **only one server node can have authority** over any given entity, and **only that server is allowed to write entity state back to the replication layer**" [1].
6. **Authority transfer.** "If an entity leaves the streaming bubble of the current authoritative server it is then transferred to the next server node." The replication layer gives the entity to both servers, one simulating and one observing, and when the entity crosses the border the authority flips [1][2]. Much of H1 2020 went into entity authority and authority transfer, because "a lot of game-code had to be changed to work with the new entity-authority concept" [2].
7. **Crash recovery.** If a replicant crashes, the replication layer starts a new one and **rebuilds the lost state from EntityGraph**. Gateways and DGS nodes reconnect, and clients see the game "unfreeze", with a target of under a minute [1]. **(bk)** A DGS crash no longer has to disconnect players (the infamous "30k" error): a replacement DGS gets the territory and loads its state from the replication layer.

### 4.2 Static → Dynamic meshing (2024–2026)

- **Static Server Meshing** shipped with **Alpha 4.0 (Pyro + Stanton–Pyro jump point), December 2024**: servers are pre-assigned areas such as planets and stations, with shards of about **500 players** [17][18]. **Nyx** followed in **Alpha 4.4 (Nov 2025)**, and 2025 refocused on "playability" (stability, performance, tech debt) [18][20].
- **Early-2026 tech talk:** CTO Benoit Beauséjour called a year of meshing "surprisingly solid". Next is **dynamic meshing**, which "will dynamically adapt to where players are, provisioning compute power in real-time" [19]. "Dynamic Mesh 2.0" targets cross-system travel with no instance boundaries [18].
- **SpatialOS warning (bk):** Improbable's similar design (workers, per-component authority, interest queries; see its view-replication patent [42]) failed commercially for several games because of cost, boundary latency, and how hard it is to write gameplay that is correct under split authority. **Gameplay code must be written for split authority from the start.**

### 4.3 Concrete authority handoff design (our proposal, based on the above)

Terms:
- **Authority group (AG):** a root entity and its attached hierarchy (a ship with everything in its interior grid). Authority transfers at AG granularity, never per component. **(bk)** This matches SC's container and hierarchy model.
- **Territory:** a region of zone space assigned to one DGS by the orchestrator (Atlas equivalent). Static at first, split and merged later.
- **Interest bubble:** every DGS subscribes to its territory plus a margin M. This guarantees the next owner already has a warm client view before handoff. The margin must be at least the maximum distance an AG can travel during a handoff round-trip.
- **Epoch (fencing token):** a 64-bit counter per AG that goes up on every authority change. The replication layer rejects state writes and RPC results carrying a stale epoch.

```mermaid
sequenceDiagram
  participant A as DGS A (current authority)
  participant R as Replicant (entity state owner)
  participant O as Orchestrator (Atlas role)
  participant B as DGS B (warm client view)
  participant C as Clients (via Gateway)
  A->>O: AG x left territory A beyond hysteresis H (pos, vel)
  O->>R: TransferAuthority(x, from=A, to=B, epoch e -> e+1)
  R->>A: RevokeAuthority(x, e) - A stops simulating x after tick T, flushes final delta @T
  A-->>R: FinalState(x, e, tick T) + queued outbound RPCs
  R->>B: GrantAuthority(x, e+1, state@T, pending inbound RPCs)
  B->>B: Promote proxy to authoritative; resume sim from T (extrapolate to now)
  R->>C: Authority epoch change (clients keep the same connection; input routing updated)
  Note over R: Writes from A with epoch e now rejected (fenced)
```

Rules:
1. **Hysteresis.** Transfer only once the AG is H metres past the boundary. This stops ping-ponging of entities that sit on a border.
2. **Atomic AG.** The children of an AG (players inside a ship) move with it. A player who leaves the ship becomes their own AG and may transfer on their own.
3. **Single writer.** Only the epoch holder writes state. Non-owners send **RPCs to the authority** through the replication layer. Example: DGS A detects a hit on a ship owned by B and sends `ApplyDamage` to B. The authority checks and applies it.
4. **Client transparency.** Clients never reconnect. The Gateway and Replicant update which DGS gets each client's input for the AGs that client controls.
5. **Stall rather than duplicate.** While a transfer is in flight (target under 2 ticks), the AG is extrapolated on clients. If B does not acknowledge in time, O returns authority to A with epoch e+2.
6. **Crash case.** When a DGS dies, O reassigns its territory. The new owner gets every AG at epoch+1 from the Replicant's last committed state. At most the ticks since the last flush are lost.
7. **Cross-boundary interaction** (two ships dogfighting across a border) is the hard case. Either extend the territory dynamically so both AGs share an owner (co-location), or accept RPC latency for hits and damage. Dynamic meshing's main value is keeping interacting AGs on the same server.

---

## 5. Planet Tech & Procedural Generation

- **Evolution.**
  - **v2 (Alpha 3.0, 2017):** full-size procedural moons, plus *edge blending* of placed meshes into terrain [7].
  - **v3:** texturing and terrain improvements **(bk)**.
  - **v4 (CitizenCon 2019 "Terra Firmer", microTech):** **biome transitions driven by temperature and humidity maps**, the start of Genesis's shared data pools [23][28].
  - **v5 / Genesis (CitizenCon 2023/2954, staged through 4.x):** "physically-based rules with Genesis data pools." It adds **geology, soil type and depth, and nutrients**, plus derived **sunlight exposure and slope aspect**. These drive non-repeating texture blending, **flora distribution by competition rules**, and **rocks placed from erosion simulation** [24][25][26][27].
- **Performance:** GPU spawning and scattering, and **virtual terrain texturing**. Rivers are planned, not shipped [27].
- **Weather:** a **rotating planet-scale texture** drives snow, wind, dust and sandstorms, which feed actor status and atmospheric flight. Seasons and **Starchitect** (procedural buildings) are planned [23][25][44].
- **Clouds and atmosphere:** planet-scale volumetric clouds arrived around 3.17 (2022) **(bk)**. For us: Hillaire-style LUT atmosphere (EGSR 2020)† plus raymarched clouds with temporal reprojection.
- **Gen12 + Vulkan:** a new backend for render-thread performance and multi-core submission [29][30]. It took about 4–5 years in stages; **Vulkan became the default in Alpha 4.5**, with **precompiled shaders** reducing stutter, and DX11 remains a fallback [29][31]. **This supports our Vulkan-1.3-from-day-one plan.**

---

## 6. Flight Model, Multi-crew & Ship Systems

### 6.1 IFCS and Newtonian flight

- A **6-DoF rigid-body Newtonian model**: each thruster applies force at its mount point, and the summed forces and torques give the acceleration [34][35].
- **IFCS** turns pilot velocity commands into thruster allocation, so flight responds to "damage states, changing mass distribution, power allocation, thruster placement". It "is an emergent system, and therefore may be imperfect at times". In atmosphere it can use aerodynamic control surfaces [34][35].
- **Coupled** holds a commanded velocity; **decoupled** keeps momentum [37].
- **Master Modes (Alpha 3.23, 2024):** **SCM** (combat, speed-capped) and **NAV** (travel, about 1 km/s, weapons locked, shields ineffective) [37]. This is a *design* fix for a *physics* problem: pure Newtonian flight at high speeds made combat jousting. Build **data-driven speed and agility envelopes** into the flight controller from the start.

### 6.2 Multi-crew and FPS-to-cockpit

- Players on a ship are children of the ship's physics grid (§2.3). Entering a seat is an animated interaction that hands flight or turret **input channels** to that player, while the camera stays first-person throughout **(bk)**. Seats, turrets, components and weapons attach through **item ports** (the item/port hierarchy that also drives loadouts and persistence) **(bk)**.
- **Engineering / Resource Network (Alpha 4.5):** "all ship components become physical objects that talk to each other." Power plants, coolers, shields and quantum drives each have health, temperature and power draw. **Relays and fuses** route resources, and components can be physically removed and replaced in flight [38][39]. This creates real jobs for multi-crew (engineer, repair) and is simulated server-side per ship.

### 6.3 Quantum travel, cargo, economy, destruction

- **Quantum travel (bk):** spool → align → jump along a line to a marker at a large fraction of light speed, with obstruction checks and interdiction. It requires **container prefetch along the path**, uses tunnel VFX to hide LOD and streaming, and crosses mesh territories, triggering authority transfer. We should make QT a deterministic, server-computed trajectory.
- **Physicalized cargo and inventory (bk):** physical SCU boxes on cargo grids, moved by tractor beams and freight elevators (3.23 era). Immersive, but it **multiplies entity counts by orders of magnitude**, which is part of the post-PES performance problem.
- **Quantum economy sim (bk, CitizenCon 2951):** a backend simulation of production, consumption, prices and aggregated NPC agents ("quanta"). It publishes probabilities that servers turn into spawns and missions. **The economy runs as a backend service at its own tick rate, not in zone servers.**
- **Maelstrom:** destruction that "models damage based on material properties", and computes **part mass from materials and thicknesses** [32][33]. It was shown in the SQ42 reveal and is planned for ships and Genesis structures [32][45][41].

---

## 7. Tools

- **StarEngine editor** (Sandbox-derived) **(bk)**: levels are built from **object containers**, so **the unit of authoring is also the unit of streaming, persistence and authority**. This is the key tools lesson. SOCS made containers hot-loadable on servers, cutting iteration from minutes to seconds [14].
- **Planet Editor (bk):** configures ecosystems (asset sets, ground textures, distribution rules). It uses climate maps from v4 onward and rules rather than painting under Genesis [23][24][25].
- **DataForge (bk):** a typed record database (ships, items, loadouts, missions, loot, AI parameters) with schemas and references, compiled to a binary database loaded by client and server. Community dataminers read it.
- **Building Blocks (bk):** the in-house UI system that replaced Scaleform-era UI. Component layout with data binding, used for MFDs, mobiGlas, kiosks and render-to-texture screens. **Editor UI (ImGui) and diegetic UI are different problems.**
- **Subsumption (bk):** visual AI and mission logic, the counterpart to our visual scripting graph. **Starchitect** [25]: procedural buildings and interiors.

---

## 8. Elite Dangerous, No Man's Sky, Dual Universe, Starfield — Lessons

**Elite Dangerous (Frontier, Cobra engine) (bk)**
- **Stellar Forge** builds a 1:1 Milky Way of about **400 billion systems** from a galactic mass-distribution model plus real catalogue stars, with system bodies from accretion-style rules.
- Procedural names like `Eol Prou RS-T d3-94` encode sector, octree cell and mass code (a–h). **A name is an address**: any system can be generated from its ID, and only deltas are stored.
- **Networking:** a **P2P** instance mesh (about 32 players) plus servers for transactions, persistence, matchmaking and the daily-tick **Background Simulation (BGS)** of faction influence. P2P was cheap but brought weak authority, cheating, combat logging, NAT failures and the Open/Solo/Private split. **Do not use P2P for an authoritative MMO; do copy the BGS tick.**
- Odyssey's on-foot launch (2021) had severe performance problems: adding a new gameplay scale late is costly.

**No Man's Sky (Hello Games) (bk)**
- Everything derives from **64-bit seeds** (about 1.8×10¹⁹ planets). Layered noise produces **voxel terrain**, polygonized in background jobs (GDC 2017: McKendrick, *Continuous World Generation in No Man's Sky*; Murray, *Building Worlds Using Math(s)*)†. Only player changes are stored, and terrain edits are bounded.
- Lessons: generation must be **cross-platform deterministic** wherever it feeds collision (watch transcendental functions and FMA); budget delta storage; variety comes from art-directed parameter spaces, which is why the "Worlds" updates reworked generation.

**Dual Universe (Novaquark) (bk)**
- **CSSO:** a single-shard world split by a dynamic **octree of cells** that split when crowded and merge when empty. A **Visibility Server** separates *interest management* from *simulation*. Players build voxel constructs scripted in **Lua**.
- Lessons: large player constructs and fights overwhelmed the simulation, forcing complexity caps, so **player-built content needs engine-enforced budgets**. Sandboxed Lua player scripting works but needs CPU and memory quotas. A single shard does not make up for thin content.

**Starfield (Bethesda, Creation Engine 2) (bk)**
- The **ship builder** snaps modules at attach points with validation rules (cockpit, reactor, grav drive, landing gear, docker), and **interiors are generated from hab modules**. This is a good pattern for our ship and base building.
- About 1,000 procedural planets with handcrafted POIs, **not seamless** (loading screens and cutscenes for landing and travel, bounded walkable areas). Players criticized both the seams and the procedural emptiness. It also shows that **a non-seamless fallback can ship.**

---

## 9. Lessons & Pitfalls (scope, performance, tech debt)

1. **Retrofitting is the killer.** SC retrofitted 64-bit positions, zones, OCS, entity authority, PES, the replication layer, Gen12 and Vulkan onto CryEngine, and each touched all game code ("a lot of game-code had to be changed" for entity authority [2]). **Build these seams in on day one, even as trivial versions**: one DGS behind the replication layer, one zone that is still a frame.
2. **Server tick rate is the real metric.** SC servers often ran at single-digit FPS under load **(bk)**, and physicalization plus persistence multiply entity counts. We need **per-territory entity budgets, an update scheduler and cleanup policies** from the start, plus a live server-FPS dashboard.
3. **Persistence without garbage collection is a leak** [3].
4. **Split authority is a programming model.** Gameplay must assume an interaction's target may live on another server: RPC to authority, idempotent operations, no raw pointers across AGs. Enforce this through APIs, not code review.
5. **Bugs live at seams:** grid transitions, territory handoffs, container stream-in, rotating frames. Build **deterministic harnesses and bots** that cross every kind of boundary.
6. **Crash isolation pays off:** with state outside the DGS, crashes become stalls instead of disconnects [1].
7. **Newtonian flight needs design guardrails** (Master Modes) [37].
8. **Renderer modernization takes years** (Gen12/Vulkan took about 5 [29][31]). Our Vulkan plan avoids this if PSO caching and multithreaded recording exist from the start.
9. **Scope:** over US$800M raised and in development since 2012 **(bk)**, with SQ42 re-targeted for 2026 [20] **(bk)**, delivered as many long-lived partial systems. **Make each milestone shippable**: static mesh before dynamic; physicalized cargo only after entity budgets.

**Where this research questions the tentative stack:**
- **Replication layer and Gateway in Go: reconsider.** This is the hottest path (every entity delta to every client and server). It should share serialization with the C++ engine and must avoid GC tail latency. **Write the Replicant and Gateway in C++ (or Rust)**, and keep Go for the control plane (auth, character, chat, market, guilds, orchestrator/Atlas, economy).
- **PostgreSQL for entity persistence: conditionally OK.** It is fine for relational data. World entities (millions, hierarchical, write-heavy) should go through a **write-behind Scribe service** that batches container blobs into partitioned Postgres behind an abstract interface. Benchmark ScyllaDB or FoundationDB before launch-scale tests.
- **NATS:** control-plane events only, not per-tick replication.
- **Jolt: OK**, but spike the double-precision build and one `PhysicsSystem` per grid early.
- **Dear ImGui: editor only.** Diegetic UI needs a retained-mode system.

---

## 10. Requirements for Our Engine

### P0 — architectural, must exist in v0 (even if trivial)

1. **Hierarchical reference frames ("zones").** Every entity transform is relative to a parent frame. Frames nest (galaxy/sector → star system → body (rotating) → ship/station → interior). Absolute positions are **system-local doubles** plus a **system/sector ID**. Local transforms are floats. There is a first-class `Reparent(entity, newFrame)` that preserves world position and velocity.
2. **Camera-relative rendering.** CPU culling in double. All GPU data is rebased to the camera each frame (float offsets). Use reverse-Z with an infinite far plane, and LOD, impostors and scaled-space rendering for astronomical-distance bodies. Include a unit test that renders at 10¹³ m from the origin with no jitter.
3. **Physics grids on Jolt.** A double-precision Jolt build, one `PhysicsSystem` per grid (planet/zone, ship interior, station). Grid volumes with hysteresis, grid transfer that converts transform and velocity, artificial gravity per grid, and the ship hull as one body in the parent grid.
4. **Unified entity model and IDs.** Stable 64-bit entity IDs, entity hierarchies, and **authority groups** as the unit of streaming, persistence and authority. The same entity and component schema in client, DGS, editor and tools, with code generation for the C++ engine and Go services.
5. **Object containers.** Authored, nested, streamable content units. The same unit is edited in the editor, streamed by client and server, persisted, and hot-reloaded. Async loading with dependency ordering, and streaming bubbles that drive loading.
6. **Relevance and update scheduling.** Distance, visibility and relevance-driven **component update policies** (every N ticks, frozen, off) and **network bind culling** per client and per server, driven by one spatial structure (the zone tree plus Morton-coded cells).
7. **Replication layer as a separate process from day one.** Clients connect to a Gateway, never directly to a DGS. A DGS connects to a Replicant as a privileged client. Single-writer authority per AG with **epoch fencing**. RPCs are routed to the authority. v0 can run a single DGS behind the replication layer.
8. **Write-behind persistence (Scribe/EntityGraph role).** Dirty tracking, container-granular snapshots, schema versioning and migration, crash recovery from the last snapshot, and **mandatory lifecycle and cleanup policy per persistent archetype**.
9. **Data-driven typed records (DataForge equivalent).** Schema'd records with references, a diffable text source format, compilation to a binary database, hot reload, and validation in CI. Records are shared by C++ and Go.
10. **Vulkan 1.3 renderer designed for multicore.** Multithreaded command recording, bindless resources, GPU-driven culling and instancing, PSO precompilation and caching (no runtime shader stutter).

### P1 — needed for first playable MMO slice

11. **Static server mesh.** An orchestrator (Atlas role, in Go) assigns territories to DGS nodes. Authority handoff as in §4.3 with a hysteresis margin, interest margin ≥ handoff travel distance, and transfer target under 2 ticks. **DGS crash recovery** without client disconnect.
12. **Deterministic procedural planets.** Cube-sphere quadtree terrain generated on the GPU, with a deterministic CPU path for server collision. Climate and geology data maps drive biome texturing and GPU scattering (the Genesis model). Planet-scale atmosphere (LUT-based scattering) and volumetric clouds.
13. **Flight model.** 6-DoF rigid body with per-thruster force and torque. A flight-control layer (thruster allocation plus PID) with coupled/decoupled modes and **data-driven speed and agility envelopes** (SCM/NAV-style master modes). Thruster damage and mass changes affect handling.
14. **Multi-crew seats and item ports.** Seats and stations grant input channels (pilot, turret, engineering). Item-port hierarchy for components and loadouts. Seamless FPS↔seat transitions with one continuous camera.
15. **Long-range travel (quantum-travel equivalent).** Server-computed deterministic trajectories, streaming prefetch along the path, interdiction hooks, and authority transfers mid-travel.
16. **Engine-enforced entity budgets.** Per territory, per player and per construct budgets with telemetry (server tick, entity count, bound entities per client) exported to our observability stack.
17. **Tools.** Container editor, planet and biome rule editor, record editor, visual scripting graph for missions and AI (Subsumption-like), all hot-reloadable against a running local DGS plus replication layer.
18. **In-world UI system.** Retained-mode layout with data binding, rendered to texture for diegetic screens. ImGui stays editor-only.

### P2 — scale and differentiation

19. **Dynamic server meshing.** Split and merge territories by load, with co-location of interacting AGs. Consider EVE-style time dilation as an overload fallback.
20. **Backend economy and world simulation service** (Go, Quantum/BGS-style ticks) that publishes spawn and mission probabilities to DGS nodes.
21. **Physicalized cargo and inventory** at scale (cargo grids, tractor beams), gated behind P1 budgets.
22. **Material-based destruction** (Maelstrom-like) and physicalized component resource networks (power, heat, relays).
23. **Procedural buildings and interiors** (Starchitect-like). Modular, validated ship and base builder (the Starfield pattern). Sandboxed LuaJIT player scripting with CPU and memory quotas (the Dual Universe pattern).
24. **Cross-system seamless travel** and a galaxy addressed by deterministic IDs (the Elite boxel pattern).

---

## 11. Sources

Numbered sources were reached through WebSearch result extracts in this session. † means not reached this session (cited from background knowledge; check before relying).

1. RSI Comm-Link 18397, *Server Meshing and Persistent Streaming Q&A* — https://robertsspaceindustries.com/en/comm-link/transmission/18397-Server-Meshing-And-Persistent-Streaming-Q-A (mirror: https://star-citizen.wiki/Comm-Link:18397/en)
2. Star Citizen Wiki, *Replication layer* — https://starcitizen.tools/Replication_layer
3. Star Citizen Wiki, *Persistent Entity Streaming* — https://starcitizen.tools/Persistent_Entity_Streaming
4. Star Citizen Wiki, *Object Container Streaming* — https://starcitizen.tools/Object_Container_Streaming
5. Star Citizen Wiki, *Server meshing* — https://starcitizen.tools/Server_meshing
6. Star Citizen Wiki, *Star Engine* — https://starcitizen.tools/Star_Engine
7. GamersNexus, *Sean Tracy on 64-bit Engine Tech & Procedural Edge Blending* — https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending
8. GamersNexus, *Chris Roberts on Instancing, Zoning, Player Counts* — https://gamersnexus.net/gg/1854-chris-roberts-pax-east-instancing-and-zoning
9. RSI, *Monthly Report: August 2014* (localized physics grid) — https://robertsspaceindustries.com/en/comm-link/transmission/14126-Monthly-Report-August-2014
10. RSI, *Monthly Studio Report: May 2015* — https://robertsspaceindustries.com/en/comm-link/transmission/14758-Monthly-Studio-Report-May-2015
11. SCFocus, *64-Bit Spatial Management of Objects for Star Citizen* — https://scfocus.org/64-bit-spatial-management-of-objects-for-star-citizen/
12. starcitizen.gr, *Understanding 64-bit coordinate system and possible integration with server meshing* — https://www.starcitizen.gr/3466733-2/
13. starcitizen.gr, *Client Side OCS – Reduced Tick Rates and Entity States* — https://www.starcitizen.gr/3372301-2/
14. Newsweek, *Star Citizen Devs Explain How SOCS Makes Space for Space in Alpha 3.8* — https://www.newsweek.com/star-citizen-devs-explain-socs-space-alpha-3-8-1477149
15. Neowin, *Star Citizen Alpha 3.3.0 rolls out Object Container Streaming* — https://www.neowin.net/news/star-citizen-alpha-330-now-live-rolls-out-object-container-streaming-for-major-fps-gains/
16. Massively Overpowered, *Alpha 3.8 on the PTU, SOCS interview* — https://massivelyop.com/2019/12/19/star-citizen-alpha-3-8-on-the-ptu-socs-interview-pillar-talk-and-the-bbc-click-investigation/
17. MMORPG.com, *Server Meshing Tech and Pyro System Go Live in 4.0 Preview* — https://www.mmorpg.com/news/star-citizens-server-meshing-tech-and-pyro-system-go-live-in-first-ever-parallel-40-preview-build-2000133737
18. Hangarbase, *The Expanded Server Mesh – CitizenCon 2025* — https://hangarbase.org/news/star-citizen-the-expanded-server-mesh-the-future-of-the-verse-revealed-at-citizencon-2025
19. Starship Dealers, *SC Live Tech Talk: Server Meshing 1 Year (2026)* — https://www.starshipdealers.com/blog/sc-live-tech-talk-server-meshing-2026/
20. Scopique, *CitizenCon Direct 2025* — https://scopique.com/2025/10/13/citizencon-direct-2025/
21. *Unofficial Road to Dynamic Server Meshing* (community explainer) — https://sc-server-meshing.info/ and https://github.com/un0btanium/unofficial-road-to-dynamic-server-meshing
22. CitizenCon 2953, *Server Meshing, PES & Replication Layer* highlight — https://www.youtube.com/watch?v=fAbcr35_Teg
23. Star Citizen Wiki, *Planet Tech v4* — https://starcitizen.tools/Planet_Tech_v4
24. Star Citizen Wiki, *Planet Tech v5* — https://starcitizen.tools/Planet_Tech_v5
25. Star Citizen Wiki, *Genesis (Star Engine)* — https://starcitizen.tools/Genesis_(Star_Engine)
26. CitizenCon 2954, *Planet Tech V5 – Genesis* — https://www.youtube.com/watch?v=2i_6wtsbJuc
27. MMOPIXEL, *Planet Tech V5 and Genesis Update Explained* — https://www.mmopixel.com/news/star-citizen-planet-tech-v5-and-genesis-update
28. Star Citizen Wiki, *CitizenCon 2019 – Terra Firmer* — https://starcitizen.tools/CitizenCon_2019_-_Terra_Firmer
29. Star Citizen Wiki, *Gen12* — https://starcitizen.tools/Gen12 ; *Vulkan* — https://starcitizen.tools/Vulkan
30. CitizenCon 2951, *Gen 12 & The Multicore of Vulkan* — https://www.youtube.com/watch?v=SV9_chUpDgc
31. MMOPIXEL, *Vulkan Renderer in Star Citizen 4.5* — https://www.mmopixel.com/news/vulkan-renderer-in-star-citizen-4-5-what-s-new-and-how-it-changes-performance
32. Star Citizen Wiki, *Maelstrom (destruction system)* — https://starcitizen.tools/Maelstrom_(destruction_system)
33. Wild Knight Squadron, *What we know: Maelstrom* — https://wildknightsquadron.com/what-we-know-maelstrom/
34. RSI Comm-Link, *Flight Model and Input Controls* — https://robertsspaceindustries.com/en/comm-link/engineering/13951-Flight-Model-And-Input-Controls
35. Star Citizen Wiki, *Intelligent Flight Control System* — https://starcitizen.tools/Intelligent_Flight_Control_System
36. RSI, *Design Notes: Flight Model Changes in Alpha 2.0* — https://robertsspaceindustries.com/en/comm-link/transmission/15031-Star-Citizen-Alpha-20-Flight-Model-Changese
37. Citizen History, *Star Citizen Master Modes Explained* — https://citizen-history.com/article/starcitizen-mastermodes-explained
38. Star Citizen Wiki, *Alpha 4.5.0* (engineering / resource network) — https://starcitizen.tools/Update:Star_Citizen_Alpha_4.5.0
39. PCGamesN, *Star Citizen engineering update details* — https://www.pcgamesn.com/star-citizen/engineering-update-details
40. Star Citizen Wiki, *Alpha 3.18.0* (PES) — https://starcitizen.tools/Update:Star_Citizen_Alpha_3.18.0
41. RSI, *CitizenCon 2954 FAQ* — https://robertsspaceindustries.com/en/comm-link/transmission/19896-CitizenCon-2954-FAQ
42. Improbable Worlds patent US11792306B2, *Network protocol for view replication over unreliable networks* (Improbable, **not** CIG; related prior art) — https://patents.google.com/patent/US11792306
43. Hacker News discussion of SC's 64-bit CryEngine changes — https://news.ycombinator.com/item?id=15920871
44. MMOPIXEL, *Star Citizen Genesis Explained* — https://www.mmopixel.com/news/star-citizen-genesis-explained
45. Star Citizen Wiki, *Squadron 42 Gameplay Reveal* — https://starcitizen.tools/Squadron_42_Gameplay_Reveal

Background references (†, not reached this session):
- † Jolt Physics (double-precision "big worlds" build) — https://github.com/jrouwe/JoltPhysics
- † S. Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*, EGSR 2020 — https://sebh.github.io/publications/egsr2020.pdf
- † N. Reed, *Depth Precision Visualized* (reverse-Z) — https://developer.nvidia.com/content/depth-precision-visualized
- † GDC 2017: I. McKendrick, *Continuous World Generation in No Man's Sky*; S. Murray, *Building Worlds Using Math(s)* — GDC Vault, https://www.gdcvault.com/
- † Frontier Developments, Elite Dangerous / Stellar Forge material — https://www.elitedangerous.com/
- † Novaquark, Dual Universe CSSO / single-shard technology — https://www.dualuniverse.game/
