# 02 — Star Wars Galaxies (SOE) and Its Emulators

> Research note for the sci-fi MMO engine project. Scope: what SWG's original server cluster, client data formats, and procedural terrain system did; how the open-source emulators (SWGEmu Core3, Holocore, SWG:ANH) rebuilt it; which gameplay systems made SWG distinctive; and what our engine and backend should take from it.
>
> **IP note:** The leaked SOE source code was not downloaded, read, or quoted. Everything here comes from public writing: developer essays and talks, community wikis, emulator READMEs and docs, repository *structure*, and prose descriptions of AGPL emulator code. The emulator code was not copied. Descriptions of the original SOE processes come from public admin and community documentation and are marked as such. Some community servers (SWG Legends, SWG Restoration) run derivatives of the leaked code. We describe them only from their public announcements.
>
> **Research constraints:** Most non-GitHub domains (raphkoster.com, swgemu.com, deepwiki.com, fandom, patents sites) were blocked for direct fetch in this environment. Claims from those sources are based on search-engine summaries of those pages and are cited as such. GitHub-hosted material was read directly.

---

## 1. Overview

*Star Wars Galaxies* (SOE/LucasArts, launched June 2003, shut down December 15, 2011) is still the reference for a **sandbox sci-fi MMO**:
- ~30+ skill-tree professions with no classes (Pre-CU).
- A fully player-driven economy: nearly all good gear was crafted from **randomly spawning, stat-bearing resources**.
- Player-placed housing and **player cities** with mayors, taxes, and elections.
- **Social professions** (entertainers, image designers, doctors) that were mechanically necessary.
- Vehicles and mounts, and a Galactic Civil War PvP layer.
- From late 2004, **Jump to Lightspeed (JTL)** space combat with a component-based ship loadout.

Technically, SWG combined:
1. A **distributed server cluster per galaxy**. Each planet was spread across several game-server processes, and objects moved between them using an *authority + proxy* model.
2. A **rule-based procedural terrain system** (layers of boundaries, filters, and affectors, evaluated at runtime on both client and server). It let a small team ship ten 16 km × 16 km planets.
3. A **data-driven content pipeline**: IFF chunked binaries in TRE archives, inherited object templates split into client-visible "shared" and server-only parts, datatables, and string tables.

The game is equally known for its **Combat Upgrade (CU, April 2005)** and **New Game Enhancements (NGE, Nov 2005)**. These radical redesigns collapsed the sandbox into nine iconic classes and are the industry's standard warning about changing a live game too much.

The community has rebuilt SWG several times:
- **SWGEmu Core3**: C++ with the custom *Engine3* framework, targeting Pre-CU. AGPL.
- **ProjectSWG Holocore**: Kotlin/Java on the JVM, targeting CU. AGPL.
- **SWG:ANH mmoserver**: C++, a multi-process design similar to SOE's. GPL.
- Several popular servers (SWG Legends, SWG Restoration) run derivatives of the original server code.

For our engine, SWG is the best-documented example of **sandbox MMO systems at scale** and of a **procedural terrain authoring model**.

---

## 2. Original SOE Server Cluster Architecture (as publicly described)

### 2.1 Processes

Public admin documentation from the community server project lists the per-galaxy process set as: LoginServer, CentralServer, ConnectionServer, PlanetServer, GameServer, ChatServer, DatabaseServer, CommoditiesServer, TaskManager, LogServer, plus supporting tools ([SWG-Source admin guide](https://github.com/SWG-Source/swg-main/wiki/StellaBellum-NGE-Server-Administration-Guide)). The original persistence backend was **Oracle** (community reports about running the original server). The roles below are inferred from that documentation, emulator reverse-engineering, and public developer commentary:

| Process | Cardinality | Role (public description) |
|---|---|---|
| **LoginServer** | Shared across galaxies | Account authentication, galaxy list and status, character list, hand-off into a galaxy. |
| **CentralServer** | 1 per galaxy | Cluster coordinator. Knows every process in the cluster, brokers the login hand-off to a ConnectionServer, sequences cluster startup (planets "preloaded" before the galaxy opens) and shutdown, and relays galaxy-wide messages. |
| **ConnectionServer** | Several per galaxy | **The only process the client talks to.** Terminates the SOE reliable-UDP session and routes client messages to whichever GameServer owns the player's character. When authority moves, it re-routes, and the client never sees server topology. |
| **PlanetServer** | 1 per scene (planet or space zone) | Spatial coordinator for one scene. Tracks which GameServer is authoritative for which area, places new objects and logging-in characters on the right GameServer, and tells GameServers which remote objects they need **proxies** of. |
| **GameServer** | Many per scene | The simulation: movement validation, combat, AI, scripts, crafting. Each object is **authoritative on exactly one GameServer**. Nearby servers hold read-only **proxies**, and changes are requested by messaging the authoritative copy. |
| **DatabaseServer** | 1 per galaxy | Persistence gateway. Collects object changes from GameServers, batches them into Oracle, and loads objects on demand (characters on login, building contents, etc.). |
| **ChatServer** | 1 per galaxy | Chat rooms (planet, guild, group, custom), tells, and presence. |
| **CommoditiesServer** | 1 per galaxy | Bazaar and vendor listings (the market), searchable across regions and planets. |
| **TaskManager** | 1 per host | Starts, monitors, and restarts processes on its machine on CentralServer's instruction. |
| **Log/Metrics** | — | Centralized logging and metrics. |

### 2.2 Partitioning planets across game servers

- **Static or preload-based distribution.** The admin documentation shows two modes. One loads a whole planet on one GameServer. The other spreads a planet across several GameServers using *preload* definitions (separate "heavy" and "light" preload lists), and administrators choose a load level per deployment ([admin guide](https://github.com/SWG-Source/swg-main/wiki/StellaBellum-NGE-Server-Administration-Guide), [load-manager page](https://github.com/SWG-Source/swg-main/wiki/Configuring-The-Server-Load-Manager-Inside-The-VM)). Loading fewer scenes directly reduces RAM and CPU ([zones page](https://github.com/SWG-Source/swg-main/wiki/How-To-Enable-Or-Disable-Ground-and-Space-Zones)).
- **Authority regions within a planet.** SWG Legends' January 2020 update described splitting a planet into several **game zones**. For example, the zone with authority over Bestine is separate from the one around Mos Espa, so a Bestine invasion doesn't lag Mos Espa. They also noted the trade-off: dividing a planet further costs more CPU in total. A galaxy needed roughly 3–7 physical servers (search summary of [SWG Legends dev update, Jan 2020](https://swglegends.com/wiki/index.php?title=Development_Update_-_January_2020_in_Review); [Massively OP coverage](https://massivelyop.com/2020/02/05/swg-legends-announces-new-plan-to-combat-latency-caused-by-its-own-dang-popularity/)).
- **Proxies at boundaries.** An object near a region edge must be visible and interactable from the neighboring server. The neighbor gets a proxy, and interactions become messages to the authoritative server. When the object crosses the boundary, authority transfers and the ConnectionServer re-routes the player.

### 2.3 Replication model (as reverse-engineered by emulators)

The emulators show the client protocol shape:
- The SOE reliable-UDP session layer carries game messages.
- Objects are created with **SceneCreateObject** followed by **Baselines**: full state, split into numbered *packages* by audience. Some packages go to every observer, some only to the owner, some only to other servers.
- After that, **Deltas** carry incremental changes to individual fields.
- Movement uses **UpdateTransform** in world space and **UpdateTransformWithParent** when inside a building cell. **UpdateContainment** handles moves into and out of containers.

Core3's packet directory lists exactly these messages: `BaseLineMessage`, `DeltaMessage`, `SceneObjectCreateMessage`, `UpdateTransformWithParentMessage`, `UpdateContainmentMessage`, `IsFlattenedTheaterMessage`, and others ([Core3 packets](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/packets), [scene packets](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/packets/scene)).

**Takeaway:** replication was schema-driven, audience-segmented, and delta-based. That is the right pattern for us.

---

## 3. Client Engine & Data Formats

### 3.1 Client engine (high level)

The 2003 client was a C++ Direct3D 8/9-era engine. Its effect files describe multi-pass texture-stage setups, and later shader paths were added ([swg3js ASSETS.md](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md)). Its main features:
- Procedural terrain streamed around the player.
- **Portal-based interiors** (`.pob`): cells connected by portals, so players walk into buildings with no loading screen.
- **Skinned characters** built from mesh generators with blend targets. These drive SWG's extensive body and face customization and palette-based coloring.
- Compressed keyframe animation.
- Client effect and particle files.
- Shared static world objects loaded from snapshots.

### 3.2 Container and archive formats

- **IFF (EA Interchange File Format), extended.** Everything is nested `FORM`s and chunks with 4-character tags and big-endian lengths. Payloads are little-endian. Forms are **versioned by a child form named `0000`, `0001`, …**, so loaders can support several revisions ([swg3js ASSETS.md](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md); [IFF background](http://fileformats.archiveteam.org/wiki/IFF)).
- **TRE archives.** Version `5000` has a 36-byte header (`EERT` magic, version, file count, TOC offset, compressor info) and 24-byte records (CRC, length, offset, compression type, name offset), with zlib compression. From Publish 18, version `6000` archives are indexed by separate `.toc` files ([swg3js ASSETS.md](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md)). The file system is a **layered overlay**: Core3's config lists 45+ TRE files, and newer patches load first and override older content ([Core3 config.lua](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/conf/config.lua)).

### 3.3 Content file types (community-documented)

| Kind | Extension / root form | Notes |
|---|---|---|
| Static mesh | `.msh` (`FORM MESH`) | Shader groups plus indexed primitives. Vertex-format flags. Left-handed Y-up. |
| Skinned mesh | `.mgn` (`SKMG`) | Joint weights and **blend targets** (customization morphs). |
| Skeleton / skeletal appearance | `.skt` (`SKTM`), `.sat` | `.sat` binds mesh generators, skeletons, and attachment joints. |
| Animation | `.ans` (`KFAT` raw / `CKAT` compressed), `.lat` | Per-frame events via `MSGS`. |
| Buildings | `.pob` (`PRTO > CELS > CELL`) | Cell 0 is the exterior. Objects inside are positioned relative to their cell. |
| Shaders | `.sht` (`SSHT`), `.cshd`, `.eff` | Texture maps, texcoord sets, factors, palettes. |
| Terrain | `.trn` (`PTAT > DATA + TGEN + BAKE`) | See §3.5. |
| World snapshot | `.ws` (`WSNP > NODS + OTNL`) | Static placed objects (§3.6). |
| String table | `.stf` | UTF-16LE strings keyed by name and ID. Referenced as `@file:key`. |
| Datatable | `.iff` (`DTII > COLS, TYPE, ROWS`) | Typed columnar tables (buildout areas, weapon effects, …). |

(Formats from [swg3js ASSETS.md](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md); tools: [Swg.Explorer](https://github.com/wverkley/Swg.Explorer).)

### 3.4 Object templates: "shared" vs "server"

Every game object is instantiated from a **template path**. Templates form an **inheritance chain**: a `DERV` chunk points at the parent, and only overridden fields are stored.
- **Shared templates** (`object/.../shared_*.iff`) ship to the client and hold what both sides need: name and description string IDs, appearance file, portal layout, container type and volume, **slot descriptor** and **arrangement descriptor** (which equipment slots an item can occupy), client data file, collision flags, game object type, scale range, `clearFloraRadius`, `noBuildRadius`, surface type.
- **Server templates** hold server-only data and logic hooks.

Core3 parses the shared IFF chain and then **merges a Lua "server template" of the same name minus the `shared_` prefix**. This gives one runtime template from a client-visible part and a server-only part ([Core3 SharedObjectTemplate description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/templates/SharedObjectTemplate.cpp); [templates dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/templates)). Templates are identified at runtime by a **CRC of the path**, and the object factory switches on game object type ([ObjectManager description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/managers/object/ObjectManager.cpp)).

**Why this matters to us:** the client/server template split limits data leakage and client patch size. Inheritance keeps thousands of item variants small, and path-CRC IDs make network references compact.

### 3.5 The `.trn` procedural terrain system (in detail)

This is the most reusable idea in SWG. Raph Koster described the core as **"marrying Photoshop layers with procedural generation."** The motivation was memory: storing ~3 bytes per m² for 16 km × 16 km × 8 planets would need about 4 GB, while rules plus fractals take the same space whatever the world size (search summaries of [Koster's "SWG's Dynamic World"](https://www.raphkoster.com/2015/04/20/swgs-dynamic-world/) and the [PCG wiki entry](http://pcg.wikidot.com/pcg-games:star-wars-galaxies)). Planets are **flat squares** (typically 16 km on a side), not spheres.

**File structure.** `FORM PTAT` (versions 0013–0015) contains:
- A `DATA` header:
  - Map size, chunk width, tiles per chunk.
  - Global water table (on/off, height, shader) and a time-cycle parameter.
  - Separate distance, tile-size, border, and seed settings for **collidable flora**, **non-collidable flora**, and **near/far radial flora** (grass).
- `TGEN`, the generator. It holds named groups plus the layer tree:
  - **ShaderGroup**: terrain surface families with weighted children and surface properties.
  - **FloraGroup**: tree and rock families.
  - **RadialGroup**: grass and small-plant families.
  - **EnvironmentGroup**: per-area lighting, sky, and weather settings.
  - **FractalGroup**: a library of named, reusable fractals.
  - **BitmapGroup**: painted Targa masks.
  - **LayersGroup**: the layer tree itself.
- `BAKE`: precomputed data, including baked maps for collidable flora.

(Sources: [Core3 ProceduralTerrainAppearance description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/terrain/ProceduralTerrainAppearance.cpp), [Core3 terrain dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain), [swg3js ASSETS.md](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md).)

**Layers.** A layer (`LAYR`) contains boundaries, filters, affectors, and child layers, plus header flags (enabled, inverted, boundary and filter flags) ([Core3 Layer description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/terrain/layer/Layer.cpp)).

- **Boundaries** say *where* a layer applies:
  - Circle `BCIR`, rectangle `BREC`, polygon `BPOL`, polyline `BPLN` (a polyline with width, used for roads and rivers).
  - Each has a **feather distance** and a **feather function**: linear, squared, square-root, or smoothstep.
  - Boundaries can be inverted. ([boundaries dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain/layer/boundaries))
- **Filters** say *under what conditions* it applies:
  - Height `FHGT`, slope `FSLP`, direction/aspect `FDIR`, fractal `FFRA`, shader (what the ground already is) `FSHD`, bitmap `FBIT`.
  - Each has its own feathering and can be inverted. ([filters dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain/layer/filters))
- **Affectors** say *what happens* ([affectors dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain/layer/affectors)):
  - *Height:* constant, fractal, terrace. Each applies an operation (add, replace, multiply, …).
  - *Color:* constant, ramp-by-height, ramp-by-fractal.
  - *Shader:* constant, replace.
  - *Flora:* collidable constant, non-collidable constant.
  - *Radial:* near constant, far constant.
  - *Other:* environment, exclude (suppress flora), passable (mark impassable), **road**, **river**.

**Evaluation algorithm** (prose from Core3's reverse-engineered implementation, [ProceduralTerrainAppearance](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/terrain/ProceduralTerrainAppearance.cpp)):
1. Walk the root layers in order. For each enabled layer, compute an **amount** in [0,1] for the query position:
   - Boundary amount is the **max** (union) over the enabled boundaries, each feathered. A layer with no boundaries gets 1.0.
   - Each filter then feathers its own result, and the layer takes the **min** (intersection) of these.
   - Inversion flips x → 1−x.
2. If the amount is greater than 0, run each affector of the requested kind (height pass, environment pass, …), scaled by the amount. Affectors accumulate into the running height or other value.
3. Recurse into child layers, **multiplying their amount by the parent's**. A child only matters where the parent applies.
4. Append **runtime terrain modifications**. Placed structures carry footprint layers that flatten the ground under them without recompiling the planet. The height query is guarded by a reader-writer lock.

**Fractals** (`MFRC`) take the following parameters. The octave-combination modes are add, multiply, crest, turbulence, and clamped variants ([MapFractal description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/terrain/MapFractal.cpp)).

| Parameter | Role |
|---|---|
| Seed | Makes the pattern reproducible |
| Bias, gain | Reshape the output curve (gain gives an S-curve) |
| Octaves, octave frequency multiplier, amplitude falloff | Control how noise layers stack |
| Frequency X/Z, offset X/Z | Scale and shift the noise pattern |

**Roads** turn a spline of control points into segments. Heights inside the road are flattened toward the nearest road height, and the road also paints a shader with its own feathering ([AffectorRoad description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/terrain/layer/affectors/AffectorRoad.cpp)).

**Runtime.** Terrain is generated **deterministically on both sides**:
- **Client** meshes: a chunk is a grid of height samples every half tile, padded by two samples on each side for seamless normals.
- **Flora** placement is seeded by coordinates, so every client sees the same trees. Collidable flora uses baked maps so the server agrees on what blocks movement and line of sight.
- **Server** needs heights for movement validation, spawning, and structure placement. Core3 has a `TerrainManager` with a `TerrainCache`, and SWGEmu ships `SWGHeightCompiler` and `SWGHeightDump` tools ([terrain/manager](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain/manager), [SWGEmu Tools](https://github.com/swgemu/Tools)).
- A browser re-implementation (swg3js) generates chunks in a Web Worker from `.trn` files and reports that **"the ground matches the original game to the centimetre"**. That confirms the rules are fully deterministic ([swg3js](https://github.com/erik-t-irgens/swg3js)).

**Patents — must check before copying the model.** Three US patents describe the same approach:
- **US 8,115,765**, *Rule-based procedural terrain generation*: layers, boundaries (circle/rectangle/polygon), filters (slope, height, shader, direction, fractal), and affectors (height, color, shader, flora, radial flora).
- **US 8,207,966**, *Terrain editor tool for rule-based procedural terrain generation*.
- **US 8,368,686**, *Resource management for rule-based procedural terrain generation*.

These appear to be SOE's (search results for USPTO documents; the PDFs could not be fetched here). **Have counsel check the assignee, claims, and expiry.** Mid-2000s filings may be expired or close to it.

### 3.6 Snapshots, buildouts, and theaters

- **World snapshots (`.ws`)** list static world objects per planet. Each node records network ID, container (parent/cell) ID, template index, orientation quaternion, position, radius, and portal-layout CRC. An `OTNL` table stores each template name once ([Core3 WorldSnapshotIff description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/templates/snapshot/WorldSnapshotIff.cpp), [swg3js](https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md)). The client already knows these objects, so the server never has to stream "create" messages for them.
- **Buildouts** are datatables of server objects per *buildout area*, with pre-assigned object IDs: cities, terminals, NPC spawners. Holocore has a dedicated `StaticService` for buildouts ([Holocore buildouts](https://github.com/ProjectSWGCore/Holocore/tree/master/src/main/java/com/projectswg/holocore/services/support/objects/buildouts)).
- **Theaters** are groups of objects spawned at runtime (points of interest, lairs) that flatten terrain. The client is told via `IsFlattenedTheaterMessage`.

---

## 4. SWGEmu Core3 Architecture

**Scope and stack.** Core3 targets Pre-CU. It is C++ built with CMake, Clang, and Ninja, and depends on BerkeleyDB 5.3, MariaDB (accounts), Lua 5.3, OpenSSL, and Boost. It is AGPL-3.0 ([Core3 README](https://github.com/swgemu/Core3)).

### 4.1 Engine3 ("MMOEngine")

Engine3 is the low-level framework ([engine3](https://github.com/swgemu/engine3), [src/engine](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine)). Its modules are `core`, `db`, `dbo`, `log`, `lua`, `orb`, `service`, `stm`, and `util`.

- **ManagedObject / IDL / ORB.** Game classes are declared in an **IDL** (`.idl`), for example `class Zone extends SceneObject`. The engine's `idlc` compiler generates four parts per class:
  - a *stub*: the reference type everyone uses;
  - an *Implementation*: hand-written logic in `FooImplementation.cpp`;
  - an *adapter*: remote invocation dispatch;
  - a *helper*: class factory.

  The generated code also includes serialization. IDL annotations state locking contracts: `@preLocked` means the caller already holds the lock, `@read` takes a read lock, and there are markers for unlocked or "dirty" access. Other annotations cover `@local` (no remote calls) and embedded fields, and `transient` excludes a field from persistence ([Zone.idl description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/Zone.idl)). A **DistributedObjectBroker** with a naming directory lets objects be looked up and called across processes ([orb](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine/orb)); config exposes an ORB port ([config.lua](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/conf/config.lua)). References are `ManagedReference<T>` and `ManagedWeakReference<T>`.
- **Tasks.** A `TaskManager` runs worker-thread pools, priority queues, and timed queues ([core](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine/core)). Game logic is written as small tasks that lock the objects they touch (`Locker`, cross-lockers). Lock ordering is a constant source of bugs: search results surfaced Core3 docs about "herd lock" ordering invariants.
- **Software transactional memory.** An experimental `stm` module provides `Transaction`, `TransactionalMemoryManager`, and transactional references ([stm](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine/stm)).
- **Persistence.** `db/berkeley` and `db/mysql` hold the ObjectDatabase, IndexDatabase, and ObjectDatabaseManager, with optional compression ([db](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine/db)).
- **Networking.** Datagram and stream services, message queues, and service handler threads ([service](https://github.com/swgemu/engine3/tree/master/MMOEngine/src/engine/service)).

### 4.2 Core3 server

**Processes and ports.** One binary hosts LoginServer, ZoneServer, StatusServer, PingServer, and an optional WebServer. Config enables 12 ground zones (including a tutorial) and 10 space zones. It uses **10 zone-processing threads**, 1 login thread, and up to 30,000 zone connections ([config.lua](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/conf/config.lua)).
- **Unlike SOE, all planets run in one process** and scale by threads, not by processes. The ORB could distribute objects, but the planet-partitioning cluster was never rebuilt (our reading of the structure).

**Zones.** `ZoneServer` owns `Zone` objects. `GroundZone` uses a **QuadTree**, and `SpaceZone` uses an **Octree**. Both have separate active-area trees for regions and triggers ([zone dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone)).
- Each object keeps a **CloseObjectsVector**: its in-range set, used for visibility and update fan-out.
- Zones provide `getInRangeObjects` and `getInRangePlayers` queries under read locks ([ZoneImplementation description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/ZoneImplementation.cpp)).
- A `ZoneProcessServer` holds the shared processing managers.

**Managers.** Each system has its own manager:

| Area | Managers |
|---|---|
| Economy & crafting | auction, crafting, resource, vendor, credit |
| Social & organizations | city, guild, group, sui (server UI), conversation |
| Combat & factions | combat, faction, gcw, frs (Jedi ranks) |
| Characters | player, skill, jedi |
| World & NPCs | creature, planet, weather, mission, loot |
| Space | ship, space, spacecombat, spacecollision |
| Technical | collision, visibility, statistics |

([managers](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/managers))

**Components.** Scene objects are made up of pluggable C++ or Lua components, chosen per template ([components](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/objects/scene/components)):
- `ContainerComponent` and `ObjectMenuComponent` (radial menus), each with Lua variants;
- `AttributeListComponent` and `DataObjectComponent`;
- `GroundZoneComponent` and `SpaceZoneComponent`.

**Persistence** ([ObjectManager description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/managers/object/ObjectManager.cpp)):
- There are about 25 named **Berkeley DB object databases**, including `sceneobjects`, `playerstructures`, `mail`, `cityregions`, `guilds`, `resourcespawns`, `spawnareas`, and `buffs`.
- **64-bit object IDs**: the high 16 bits are the database or table ID and the low 48 bits a sequence number.
- Objects are either transient or explicitly `persistObject()`-ed with a *persistence level*.
- **Dirty objects are saved periodically by background tasks**, and there is a batched character-commit phase.
- Objects are rebuilt from blobs by IDL-generated `readObject`, with a CRC check to detect corruption.

**Terrain and navigation.** Core3 has its own TRN evaluator (§3.5). It builds **Recast navmeshes** by tile, per region and building ([pathfinding](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/pathfinding)).

**Scripting.** Lua is used heavily ([bin/scripts](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/bin/scripts)):
- **Object templates:** thousands of Lua files, including every draft schematic.
- **Mobiles, loot, commands, skills.**
- **Manager configs:** `city_manager.lua`, `resource_manager.lua`, `gcw_manager.lua`, …
- **Screenplays:** content and quests. A `DirectorManager` bridges C++ and Lua. Screenplays inherit from a base `ScreenPlay`, register with the director, have `start` methods, and use **observers** (event subscriptions) and timed events (search summary of [DeepWiki: Scripting System](https://deepwiki.com/swgemu/Core3/9-scripting-system)).

**AI.** AiAgent uses **behavior trees**. The trees are defined in Lua (`ai/default.lua`, `herd.lua`, `escort.lua`, `cityPatrol.lua`, `villageRaider.lua`, …) and authored in **Behavior Studio**, an XML behavior-tree editor that exports Lua ([ai dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/objects/creature/ai), [scripts/ai](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/bin/scripts/ai), [behavior-studio](https://github.com/swgemu/Tools/tree/master/behavior-studio)). Lairs, spawn areas, and mission generation are manager-driven (search summary of [DeepWiki: Lair System](https://deepwiki.com/swgemu/Core3/8.2-lair-system)).

**Assessment.** The IDL codegen gave Core3 serialization, remote references, and a consistent way to state locking, which is valuable. Its costs are:
- a heavy, custom toolchain;
- lock-ordering deadlocks;
- opaque blob persistence (hard to query or migrate);
- single-process scaling.

For us, the lesson is **codegen from a schema: yes; fine-grained object locking across many threads: avoid.** Prefer a single writer per region or cell with message passing.

---

## 5. Other Emulators

- **ProjectSWG Holocore** (Kotlin/Java, AGPL, CU era; [repo](https://github.com/ProjectSWGCore/Holocore)).
  - **Stack:** Gradle build, **MongoDB** (docker-compose ships mongo and mongo-express), Docker images.
  - **Boot sequence:** connect to Mongo, load a `serverdata` directory, then start an **IntentManager**. This is a typed publish/subscribe event bus on a thread pool sized to cores + 8.
  - **Structure:** two service trees, `GameplayManager` and `SupportManager`, with a **50 ms main tick** and coroutines ([ProjectSWG.kt description](https://github.com/ProjectSWGCore/Holocore/blob/master/src/main/java/com/projectswg/holocore/ProjectSWG.kt)). Gameplay services cover combat, commodities, crafting, entertainment, faction, jedi, missions, structures, trade, training, and world.
  - **Awareness:** an `AwarenessService` sends updates only to observers ("if nobody observes, send nothing"), uses chunk-based spatial partitioning, and handles world-versus-container movement ([AwarenessService description](https://github.com/ProjectSWGCore/Holocore/blob/master/src/main/java/com/projectswg/holocore/services/support/objects/awareness/AwarenessService.java)).
  - **Content:** tab-separated **`.sdb`** tables, for example dynamic spawns that reference lairs and NPC tiers ([dynamic spawns guide](https://github.com/ProjectSWGCore/Holocore/blob/master/contribution_dynamic_spawns.txt), [serverdata](https://github.com/ProjectSWGCore/Holocore/tree/master/serverdata)).
  - **Tools:** MobileScriptBuilder, ConversationScriptCreator (a visual conversation tool), SchematicDump, StfSearcher, and a Kotlin launcher ([org](https://github.com/ProjectSWGCore)). Its predecessor, NGECore2, was Java with Python scripting.
- **SWG:ANH mmoserver** (C++, GPL, Pre-CU). Separate LoginServer, ConnectionServer, ChatServer, ZoneServer, and PingServer processes over MySQL. It deliberately followed SOE's multi-process topology ([src](https://github.com/swganh/mmoserver/tree/master/src)).
- **SWG Legends and SWG Restoration.** The largest current communities run derivatives of the original server code, so they inherit the Central/Connection/Planet/Game cluster (see §2.2 for Legends' zone splitting). We treat them only as a source of public operational lessons, not as code references.

---

## 6. Gameplay Systems and What the Engine/Backend Must Provide

### 6.1 Professions and skills (Pre-CU)
**Design:** six starter professions (Artisan, Brawler, Entertainer, Marksman, Medic, Scout) branch into ~30+ elite professions. Each is a small tree (novice box, four branches of four boxes, master box) under a global **250 skill-point cap**, so players mix "templates" and retrain freely. XP is typed per activity (weapon type, crafting, dance, music…), and boxes grant commands, skill mods, schematics, titles, and certifications.
**Engine needs:** a data-driven, hot-reloadable, versioned skill graph (prerequisites, costs, grants), typed XP, and **skill mods** as a generic stat-modifier system, so balance changes are data, not code.

### 6.2 Resources and crafting
**Design:**
- **Resources are generated types, not fixed items.** Each spawn has a random name, a class in a deep tree (e.g., Inorganic → Metal → Ferrous → Iron → *Doonium*), and class-specific attributes: OQ, CD, DR, FL, HR, MA, PE, SR, UT, CR, ER (flora has flavor, metals have conductivity). Spawns shift irregularly, living about 6–21 days ([SWG Legends: Resource](https://swglegends.com/wiki/index.php?title=Resource), [SWGR](https://swgr.org/wiki/crafting_resources/)), with Perlin-noise concentration across the planet (search summary of [PCG wiki](http://pcg.wikidot.com/pcg-games:star-wars-galaxies)).
- **Core3's spawner** checks shifts every 2 hours, gives class-specific lifetimes, fills **minimum, random, fixed, and native (per-planet organics) pools**, and throttles quality so ~90% of stats stay below a cap ([resource_manager.lua](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/managers/resource_manager.lua), [resource dir](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/managers/resource)).
- **Crafting chain:** survey → sample or place a **harvester** (powered installation with maintenance) → **draft schematic** → assembly → experimentation → customization → prototype → **manufacturing schematic** → **factory** batch runs. Schematics define complexity, XP, skills, and ingredient slots (resource class or component, quantity, contribution) ([carbine schematic](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/object/draft_schematic/weapon/carbine_blaster_cdef.lua)). Experimental properties are **weighted sums of resource attributes**; that quality sets the **maximum experimentation percentage**, and skill-modified assembly and experimentation rolls move each property within min–max ([ResourceLabratory description](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/managers/crafting/labratories/ResourceLabratory.cpp)). Separate labs handle genetics (Bio-Engineer) and droids.

**Engine needs:** a galaxy-wide **resource-type service** (spawn catalogue, concentration fields, history); a server-authoritative **crafting session state machine**; per-instance item stats computed by formulas; crafter name and serial on items; installations evaluated lazily on access rather than ticked.

### 6.3 Economy
**Design:** almost everything worth having was player-made, and **item decay** kept demand steady. **Bazaar terminals** carried region-scoped listings; **player vendors** (NPCs in houses and cities) created "vendor malls". Secure trade windows handled swaps; maintenance, taxes, and fees were sinks.
**Engine needs:** a transactional **commodities service** (Go + Postgres: listings, bids, escrow via mail, expiry, attribute/region search); in-world vendors whose stock lives in that service; atomic multi-party trades with **full audit logs** (dupes are existential); faucet/sink telemetry.

### 6.4 Housing and player cities
**Design:** houses, guild halls, harvesters, and factories can be **placed almost anywhere on open terrain**, subject to no-build radii, slope and footprint checks, and a **per-character lot budget**. Structures pay maintenance or are condemned; owners manage admin/entry/ban lists and decorate freely. **Player cities** (founded by Politicians) grow Outpost → Village → Township → City → Metropolis with citizen count, radius ~150 m → 450 m, and run a weekly update cycle, elections, five taxes (income, property, sales, travel, garage), rank-gated civic structures, and specializations; some planets forbid cities ([city_manager.lua](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/managers/city_manager.lua); placement per search summary of [DeepWiki](https://deepwiki.com/swgemu/Core3/13.1-structure-placement-and-management)).
**Engine needs:** replicated, persisted **runtime terrain modification layers**; placement validation against procedural terrain and region rules; region permissions; load-on-demand interiors and per-structure item caps; cities as persistent entities with governance state and scheduled jobs.

### 6.5 Social professions
**Design:** entertainers (dancers and musicians, with synchronized bands) healed **battle fatigue and mind wounds** and granted long **buffs** in cantinas; doctors buffed stats; image designers changed appearance. Combat players *needed* social players, which made cantinas hubs.
**Engine needs:** buffs/wounds with sources and durations; performance sync (shared music timeline and animation across a band); spectator-driven XP; crowd LOD for packed cantinas.

### 6.6 Mounts and vehicles
**Design:** speeders (crafted) and creature mounts (tamed or bio-engineered) live as intangible **control devices** in the datapad; calling one spawns it and storing it despawns it. Pets work the same way.
**Engine needs:** a generic "control device ↔ world object" pattern, server-validated fast movement on heightfield terrain, and mounted animation rigs.

### 6.7 Galactic Civil War and PvP
**Design:** faction standing, covert/overt flagging, faction points and ranks, attackable player-placed **faction bases**, planetary control and invasion events, bounty hunters versus Jedi.
**Engine needs:** a flagging/permission matrix checked on every hostile action, and **load-surge handling**: GCW hotspots are exactly what overloaded fixed zone partitions (§2.2).

### 6.8 Jump to Lightspeed (space)
**Design:** space zones are separate scenes (Core3 enables 10, using an Octree `SpaceZone` — [config](https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/conf/config.lua)). Pilot professions exist for three factions, plus a Shipwright crafting profession. A ship is a **chassis with a mass limit** carrying components (reactor, engine, shield, armor, capacitor, booster, droid interface, weapons) with mass and energy draw; loot components can be reverse-engineered. Later expansions added **multi-crew ships with walkable interiors** (portal cells inside a moving object). Core3 recently added `ship`, `spacecombat`, and `spacecollision` managers and space behavior trees ([managers](https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone/managers)).
**Engine needs:** 3D partitioning; server-authoritative flight with client prediction; loadouts with mass/energy budgets as data; **cells that move with a parent object**, which needs parent-relative transforms throughout replication, physics, and navigation. SWG put space and ground behind loading screens; a Star Citizen-class target needs more.

---

## 7. Tools and Editors

- **SOE internal (as publicly discussed).** A **terrain editor**: the layer-tree authoring tool described in patent US 8,207,966. Sony's technical article describes artist rules like *"within this circle I want mountains with these kinds of pine trees on it"* (search summary of [PCG wiki](http://pcg.wikidot.com/pcg-games:star-wars-galaxies)). There was also a **God Client**, an in-game world-editing and GM client listed among community tools ([SWG-Source wiki](https://github.com/SWG-Source/swg-main/wiki)), plus datatable and string-table tooling compiling into IFF.
- **Community.**
  - **TRE Explorer / Swg.Explorer**: multi-archive browsing, IFF and STF viewing, Collada mesh export, STF-to-CSV ([Swg.Explorer](https://github.com/wverkley/Swg.Explorer)).
  - **SIE (IFF editor)**: generic IFF, datatable, and string editing; listed as the "IFF Editor" on the [SWG-Source wiki](https://github.com/SWG-Source/swg-main/wiki).
  - **SWGEmu Tools** ([repo](https://github.com/swgemu/Tools)): CRCCalculator, DBExplorer (object DB inspection), **SWGHeightCompiler and SWGHeightDump**, **WorldSpawnerTool** (C++, Lua, and Python variants), and **Behavior Studio**.
  - **ProjectSWG**: MobileScriptBuilder, ConversationScriptCreator, SchematicDump, ScriptEditor ([org](https://github.com/ProjectSWGCore)).
  - **swg3js**: converts TRE to glTF and re-implements the terrain generator in the browser ([repo](https://github.com/erik-t-irgens/swg3js)).

**Lessons for our editor:**
1. The terrain editor must be **rule- and layer-based with live preview**, not heightmap painting only.
2. The **in-game god client is essential** for placing buildouts in context.
3. The community rebuilt a behavior-tree editor, a conversation editor, and a spawn tool, so these must be first-class in our editor from day one.
4. Plain inspectable data (datatables, `.sdb`/TSV, Lua) makes content accessible to non-engineers.

---

## 8. Lessons and Pitfalls

1. **Don't ship a radical redesign to a live sandbox.** The CU (April 2005) and NGE (Nov 2005) cut ~30+ professions to 9 iconic classes and changed combat to twitch-style. Much of the community left. *Engine implication:* keep game rules data-driven so tuning is cheap, but the real lesson is about product management.
2. **Interdependence works, and it is fragile.** Social professions and a crafted-only economy created lasting communities. Removing that need (NGE-era buff and loot changes) eroded it.
3. **Iconic power fantasies break sandboxes.** Everyone wanted to be a Jedi. The hidden unlock and the later "holocron grind" distorted player behavior.
4. **The economy needs instruments.** Mission-terminal and loot faucets caused inflation. Koster has said that wealth concentration in SWG shaped his later economic designs. Item decay was a healthy sink. *Engine implication:* build economy telemetry and tunable faucets and sinks.
5. **Houses everywhere cause clutter and cost server load.** Free placement created sprawl and ghost towns of abandoned structures, and houses full of items and vendors inflate object counts and load times. SOE responded with maintenance-driven condemnation and storage limits. *Implication:* lazy-load interiors, cap items per structure, reclaim inactive property automatically, and consider zoned "suburbs".
6. **Static partitions fail under hotspots.** Invasions, GCW battles, and city events pile onto one server. Legends' remedy (more zones per planet) trades hotspot isolation for more total CPU. *Implication:* dynamic, load-driven repartitioning with hysteresis, splitting hot cities or cells on demand, and transparent authority migration.
7. **Procedural worlds are large but can feel empty.** 16 km planets were cheap to author but needed POIs, lairs, and theaters to feel alive. The ambitious "dynamic world" and "living society" ecology designs were cut for complexity (search summaries of Koster's [dynamic world](https://www.raphkoster.com/2015/04/20/swgs-dynamic-world/) and [living society](https://www.raphkoster.com/2015/04/21/designing-a-living-society-in-swg-part-one/) essays). *Implication:* tools for density and dressing, and simulation budgets designed for scale.
8. **Launch readiness.** SWG launched with missing and buggy systems: space and mounts came later, and Jedi was contentious. Emulators show which subsystems are really core.
9. **Emulator architecture lessons.** Core3 shows that fine-grained locking over a shared object graph produces deadlocks and needs lock-ordering rules. Blob persistence is hard to migrate or query, and one process caps scale. Holocore shows that an event bus plus services is easy to contribute to but also single-process. None of them rebuilt SOE's multi-server planet partitioning, which is the hard part and the part we most need.

---

## 9. Requirements for Our Engine

### P0 — foundational (architecture-shaping)

1. **Authority + proxy spatial partitioning.** Each world object is authoritative on exactly one simulation ("cell") server. Neighbors hold read-only proxies inside an interest margin, and mutations go to the authority as messages. Authority transfers must be seamless.
2. **A scene coordinator ("PlanetServer") per planet or space zone.** It maintains the region→server map, places spawns and logins, requests proxies, and **re-partitions dynamically by load**: split hot regions, merge cold ones, with hysteresis. This goes beyond SOE's static preload lists.
3. **Gateway (connection) servers** terminate client sessions (reliable/unreliable UDP), hide topology, and re-route on authority change without client-visible handoffs.
4. **Schema-driven replication.** Replicated fields are declared once in an IDL-like schema. Codegen produces C++ (sim and client) and Go (services) serializers. Fields are grouped into **audience packages** (all observers / owner only / server-to-server only). Sends are a baseline plus delta-compressed field changes, with parent-relative transforms for cells and ships.
5. **Deterministic rule-based procedural terrain**, bit-identical on client and server:
   - a layer tree;
   - boundaries (circle, rect, polygon, polyline-with-width) with feather functions;
   - filters (height, slope, direction, fractal, shader, bitmap) combined as max for boundaries and min for filters;
   - affectors (height const/fractal/terrace, color, shader, flora, radial, environment, exclude, passable, road, river);
   - a named fractal library, seeded flora with baked collidable-flora data, chunk caching, and **runtime modification layers** for structure footprints.

   Adapt it to spherical or cube-sphere planets for seamless space-to-ground. **Get a legal patent review first** (US 8,115,765 / 8,207,966 / 8,368,686).
6. **Template system with inheritance and a client/server split:**
   - shared templates ship to the client, and server templates stay server-only, merged at load;
   - path-CRC IDs;
   - slot and arrangement descriptors for equipment and containers;
   - datatables and localized string tables;
   - a layered virtual file system (archives plus patch overlays, versioned chunk formats).
7. **Persistence:**
   - 64-bit namespaced object IDs;
   - dirty tracking with **write-behind batched saves** from sim servers through a persistence service to Postgres;
   - load-on-demand containers;
   - crash-consistent save epochs;
   - **transactional cross-object operations** (trade, crafting consumption, vendor sale) with audit logs.

   Redis is a cache and presence store, never the source of truth.
8. **A generic container/cell model:** inventories, equipment, buildings with portal cells, vendors, and ships with interiors all use the same parent-child containment with parent-relative transforms, per-cell navmesh (Recast/Detour), and interior culling.

### P1 — core sandbox systems

9. **Resource-type service:** galaxy-wide procedural resource spawns (class tree, per-class attribute ranges, lifetimes, per-planet noise concentration maps, quality throttling, history), plus survey, sample, and harvester installations evaluated lazily.
10. **Crafting framework:** schematics with ingredient slots, weighted attribute formulas, server-authoritative assembly and experimentation rolls, per-instance item stats, crafter and serial tagging, manufacturing runs, and component sub-assemblies. All data-driven.
11. **Commodities/market service (Go + Postgres):** region-scoped bazaar plus in-world vendors backed by the service, escrow via mail, search, expiry, taxes, and economy telemetry (faucets and sinks dashboards).
12. **Structures and cities:** placement validation (slope, footprint, no-build radius, lots), maintenance and condemnation, permission lists, per-structure item caps, and player cities as persistent entities (rank, radius, taxes, elections, civic structures, scheduled weekly jobs).
13. **Data-driven skill graph:** boxes, prerequisites, typed XP, skill mods as generic modifiers, and grants (commands, schematics). Hot-reloadable and versioned.
14. **Content scripting and AI:** Lua screenplays with an observer/event API and timers, a visual behavior-tree editor that exports to runtime data, lair and dynamic-spawn tables, and a procedural mission generator.
15. **Space scenes:** octree partitioning, authoritative flight with prediction, ship loadouts with mass and energy budgets, and **moving multi-crew interiors** (cells parented to a ship).

### P2 — breadth and tooling

16. **Social systems:** a chat service (planet, city, guild, group, custom rooms, tells), persistent mail, buffs and wounds, synchronized entertainer performances, and crowd LOD.
17. **Mounts and vehicles** via "control device ↔ world object" and vehicle movement validation. **GCW** flagging matrix, faction bases, and invasion events that feed the load balancer.
18. **Editor and tools:**
    - a layer-tree terrain editor with live 2D and 3D preview;
    - an **in-game god client** for buildout and snapshot placement;
    - a conversation editor, spawn/lair tool, datatable editor, and object-DB inspector;
    - hot reload of scripts and data on live servers.
19. **Ops:** a per-host process supervisor (TaskManager analogue) in the Go orchestration layer, per-scene load levels (light/medium/heavy), and per-region CPU and population metrics.

**Stack flags:**
- **Use NATS for control-plane messaging only.** It is fine for chat, market, and cluster control. Sim-to-sim proxy replication and authority hand-off need a low-latency direct transport (custom UDP/TCP mesh), not a general message bus.
- **Add a dedicated persistence gateway.** Persistence should go through its own service (the DatabaseServer analogue) that batches writes, rather than every sim server writing to Postgres directly.
- **Choose threading deliberately.** Prefer single-writer-per-region message passing over Core3-style fine-grained object locks.
- **LuaJIT is appropriate.** Core3 and SWG:ANH show Lua scales for screenplays, templates, and AI data.
- **Add Recast/Detour.** Navmesh is a missing piece in the current stack list.

---

## 10. Sources

**Read directly (GitHub):**
- SWGEmu Core3 README and structure: https://github.com/swgemu/Core3
- Core3 config: https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/conf/config.lua
- Core3 zone, managers, packets, components, AI, pathfinding, terrain, templates dirs: https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src
- Core3 terrain sources (described, not copied): ProceduralTerrainAppearance.cpp, layer/Layer.cpp, MapFractal.cpp, affectors/AffectorRoad.cpp, boundaries/, filters/, affectors/ under https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/terrain
- Core3 templates: SharedObjectTemplate.cpp, snapshot/WorldSnapshotIff.cpp: https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/templates
- Core3 ObjectManager: https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/managers/object/ObjectManager.cpp
- Core3 Zone.idl and ZoneImplementation.cpp: https://github.com/swgemu/Core3/tree/unstable/MMOCoreORB/src/server/zone
- Core3 crafting: https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/src/server/zone/managers/crafting/labratories/ResourceLabratory.cpp ; schematic example https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/object/draft_schematic/weapon/carbine_blaster_cdef.lua
- Core3 manager configs: https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/managers/city_manager.lua , https://github.com/swgemu/Core3/blob/unstable/MMOCoreORB/bin/scripts/managers/resource_manager.lua
- Engine3: https://github.com/swgemu/engine3 (core, orb, stm, db, service dirs)
- SWGEmu Tools: https://github.com/swgemu/Tools
- Holocore: https://github.com/ProjectSWGCore/Holocore ; ProjectSWG.kt, AwarenessService.java, docker-compose.yml, contribution_dynamic_spawns.txt, serverdata/
- ProjectSWG org: https://github.com/ProjectSWGCore
- SWG:ANH: https://github.com/swganh/mmoserver/tree/master/src , https://github.com/swganh/documentation
- SWG-Source public wiki (admin docs only): https://github.com/SWG-Source/swg-main/wiki/StellaBellum-NGE-Server-Administration-Guide , https://github.com/SWG-Source/swg-main/wiki/Configuring-The-Server-Load-Manager-Inside-The-VM , https://github.com/SWG-Source/swg-main/wiki/How-To-Enable-Or-Disable-Ground-and-Space-Zones
- swg3js (formats and terrain port): https://github.com/erik-t-irgens/swg3js , https://github.com/erik-t-irgens/swg3js/blob/main/docs/ASSETS.md
- Swg.Explorer: https://github.com/wverkley/Swg.Explorer

**Via search-engine summaries (direct fetch blocked here):**
- Raph Koster, "SWG's Dynamic World": https://www.raphkoster.com/2015/04/20/swgs-dynamic-world/
- Raph Koster, "Designing a Living Society in SWG, part one": https://www.raphkoster.com/2015/04/21/designing-a-living-society-in-swg-part-one/
- Classic Game Postmortem: SWG (Koster/Vogel): https://www.raphkoster.com/games/presentations/classic-game-postmortem-star-wars-galaxies/ ; https://www.gdcvault.com/play/1027153/Classic-Game-Postmortem-Star-Wars
- GDC 2004 Koster, "SciFi MMPs: Lessons from SWG and Earth & Beyond": https://archive.org/details/GDC2004Koster
- PCG Wiki, Star Wars Galaxies: http://pcg.wikidot.com/pcg-games:star-wars-galaxies
- SWG Legends Dev Update Jan 2020: https://swglegends.com/wiki/index.php?title=Development_Update_-_January_2020_in_Review ; https://massivelyop.com/2020/02/05/swg-legends-announces-new-plan-to-combat-latency-caused-by-its-own-dang-popularity/
- Resources: https://swglegends.com/wiki/index.php?title=Resource ; https://swgr.org/wiki/crafting_resources/
- DeepWiki Core3 / engine3: https://deepwiki.com/swgemu/Core3 , https://deepwiki.com/swgemu/Core3/9-scripting-system , https://deepwiki.com/swgemu/Core3/8.2-lair-system , https://deepwiki.com/swgemu/Core3/13.1-structure-placement-and-management , https://deepwiki.com/swgemu/engine3
- Terrain patents (USPTO): US 8,115,765; US 8,207,966; US 8,368,686 (https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8115765 , .../8207966 , .../8368686)
- IFF background: http://fileformats.archiveteam.org/wiki/IFF
