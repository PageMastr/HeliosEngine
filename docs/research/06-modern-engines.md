# 06 — Modern General-Purpose Engine Architecture: Lessons for Our Engine

**Scope:** Unreal Engine 5, Godot 4, Unity DOTS, O3DE, Bevy, Frostbite, id Tech, GPU-driven pipelines, Naughty Dog's fiber jobs. The goal is to find architecture patterns worth adopting in our C++20 / Vulkan 1.3 / Jolt / LuaJIT / Dear ImGui engine, editor, client and zone/cell servers.
**Date:** 2026-09-25 · **Status:** research input to architecture decisions

> **How this was researched.** The sandbox's network rules blocked direct fetches from dev.epicgames.com, docs.godotengine.org, docs.unity3d.com, bevyengine.org, gdcvault.com and advances.realtimerendering.com. Wherever possible I read the primary sources through their **GitHub mirrors**, so those claims are quoted from the source itself: the Godot docs source, the Godot website articles, Unity's Entities, Netcode and Burst package docs (needle-mirror), the O3DE docs source, the Bevy source and release notes, Jolt's `Architecture.md`, GASDocumentation and MassSample. Claims about Unreal come from search-engine excerpts of Epic's documentation. Claims about the GDC/SIGGRAPH talks come from well-established published content; those sources are marked **†** in *Sources* and should be spot-checked before anything depends on an exact number.

---

## 1. Overview

Every modern engine lands on a similar layered structure. What separates them is where each draws its boundaries:

1. **A low-level "server" layer with handle-based APIs sits under whatever high-level object model the engine exposes.** Examples: Godot's RenderingServer/PhysicsServer with RIDs, O3DE's RHI/RPI feature processors, Bevy's Render World, and Unreal's render-thread proxies. This split lets a headless server leave out rendering entirely. It also puts the data-oriented optimizations where they pay off.
2. **Unreal and Unity both bolted ECS onto an older object model and paid for it.** Unreal now has Actors, Mass and (in UEFN) Scene Graph. Unity has GameObjects plus Entities joined by *baking*. A new engine should choose one entity model. It should also separate *authoring* data from *runtime* data from the start.
3. **Reflection is the backbone of everything else.** Serialization, editor property panels, undo/redo, scripting bindings, networking and garbage collection all read from it: UHT in Unreal, ClassDB in Godot, the Serialize/Edit/Behavior contexts in O3DE. One schema should feed all of those consumers.
4. **Rendering has converged on the same toolkit:** a render graph (Frostbite FrameGraph, O3DE Frame Scheduler), bindless resources, GPU-driven culling and indirect submission (Ubisoft/RedLynx 2015, Nanite), and clustered lighting (Godot Forward+, id Tech). Nanite, Lumen and Virtual Shadow Maps are the top tier. They are useful targets to aim at, but should not be day-one requirements.
5. **Large worlds need double-precision positions on the CPU and camera-relative floats on the GPU.** Unreal's LWC takes the maximum world size from about 21 km to 88 million km. Godot uses emulated double precision in shaders. **Jolt** has `JPH_DOUBLE_PRECISION`, and its own documentation describes splitting a universe into multiple PhysicsSystems for space simulation. That guidance maps directly onto a Star Citizen-style game.
6. **At scale, networking means filtering, prioritization and serializing each object once.** Examples: Unreal's Replication Graph (100 players and about 50k replicated actors in Fortnite), Iris (a quantized copy of the state plus push-based dirty tracking), and Netcode for Entities (an importance priority queue per chunk). Prediction is expensive. Netcode's own docs say about 22 resimulated ticks per frame at 300 ms RTT.
7. **Asset pipelines are converging on content-addressed storage.** The chain is: source import → derived-data cache keyed by an input hash → platform cook → chunked containers (Unreal's Zen server and IoStore, O3DE's Asset Processor with hot-reload notifications). The same content addressing makes delta patching in the launcher straightforward.
8. **Visual scripting only works with high-level building blocks.** Godot removed VisualScript because only about 0.5% of users adopted it: its nodes were low-level and it lacked the game systems that make Blueprints useful. Unreal dropped Blueprint nativization in UE5, and Epic is moving to Verse plus Scene Graph.

---

## 2. Unreal Engine 5 (by subsystem)

### 2.1 Object model: UObject, UHT reflection, GC
- The **Unreal Header Tool (UHT)** pre-processes headers annotated with `UCLASS/USTRUCT/UPROPERTY/UFUNCTION` and generates reflection data. That data powers serialization, the editor Details panel, Blueprint exposure, replication (`Replicated`, `ReplicatedUsing`) and GC reference discovery ([Objects](https://dev.epicgames.com/documentation/unreal-engine/objects-in-unreal-engine), [Property System](https://www.unrealengine.com/blog/unreal-property-system-reflection)).
- **Tracing GC:** objects that can't be reached from the root set are collected. Only `UPROPERTY`-visible references count, which is a classic source of bugs. UE5 added **incremental reachability analysis**, which spreads marking across frames under a per-frame time limit. `TObjectPtr` write barriers mark objects reachable during an in-progress GC ([Incremental GC](https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine)).
- **Lesson:** reflection codegen is worth it. An engine-wide tracing GC over C++ objects costs years of hitch-reduction work. Use handles plus explicit ownership instead.

### 2.2 Actor/Component vs Mass Entity vs Scene Graph
- **Actors/Components** are deep-inheritance, heavyweight objects: each one is ticked, replicated and GC'd individually.
- **Mass** is an archetype ECS. Fragments are the data, Processors are stateless logic, and EntityQueries iterate chunk batches. Its archetype chunks resemble Unity's ([Mass overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine)). The AI team built it for crowds, and it bolts on LOD, a representation that can swap an entity for an ISM instance or a full Actor based on distance, replication through a separate NetID, and Signals and SmartObjects. It parallelizes per processor (a dependency graph) and per query (`ParallelForEachEntityChunk`, which creates a command buffer per job). Structural changes are deferred via `Defer()` ([MassSample](https://github.com/Megafunk/MassSample)). The MassSample README also notes that **Sequencer already used an internal ECS** for evaluation.
- **Scene Graph (UEFN, experimental)** has entities, components and prefabs, with Verse as the native language. Epic presents it as the replacement for Actors ([Scene Graph](https://dev.epicgames.com/documentation/en-us/fortnite/scene-graph-in-unreal-editor-for-fortnite)).
- **Lesson:** Epic now maintains three entity models plus bridges between them. We should ship one: an ECS with hierarchy/relationships and prefabs.

### 2.3 Gameplay Ability System (GAS)
- The parts: **Attributes** are floats (base and current values) grouped in AttributeSets that guard modification. **GameplayEffects** are data-only modifiers: instant, duration, infinite or periodic, with stacking and tags granted or required. **GameplayTags** are hierarchical interned names with containers and queries. **Abilities** have costs and cooldowns (both expressed as effects) and async tasks. **GameplayCues** are cosmetic only ([GAS overview](https://dev.epicgames.com/documentation/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system)).
- **Prediction keys:** the client generates a key when it activates an ability and attaches it to every effect it predicts. The server stamps the same key on its authoritative effects and echoes it back. When the client receives the echoed key, it removes all effects it predicted under that key; matching server effects persist. Anything the server did not confirm was a misprediction. Each key is valid only within an atomic "prediction window". New windows need synch points (`WaitNetSync`), and a malicious client can stall those. **GAS cannot predict effect removal or periodic effects**, so cooldowns cannot be fully predicted. The community documentation advises against predicting damage or death ([GASDocumentation §4.10](https://github.com/tranek/GASDocumentation), [FPredictionKey](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/GameplayAbilities/FPredictionKey)).
- **Lesson:** GAS's data model (attributes, effects, tags, cues) is proven and fits MMO combat and buffs well. Keep prediction narrow: costs, cosmetics, montages and movement. Damage and death results stay server-authoritative.

### 2.4 Networking: Replication Graph, Iris, networked physics
- **Replication Graph:** Fortnite runs 100 players and about 50,000 replicated actors per server. Per-actor, per-connection relevancy checks were CPU-bound, so actors are bucketed into graph nodes: AlwaysRelevant, AlwaysRelevantForConnection, and **GridSpatialization2D**, whose cells hold persistent lists of the actors visible from them. The engine docs explicitly suggest this for MMORPG-style worlds ([RepGraph docs](https://dev.epicgames.com/documentation/unreal-engine/replication-graph-in-unreal-engine), [tech blog](https://www.unrealengine.com/tech-blog/replication-graph-overview-and-proper-replication-methods)).
- **Iris** (opt-in as of the 5.8 docs) keeps a **full quantized copy of all replicated state**, which decouples gameplay objects from the replication system. It expects game code to *push* notifications of changes instead of having the system poll, and it is built for "higher player counts, and lower server costs" ([Intro to Iris](https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-iris-in-unreal-engine), [Components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine)). Its main parts: a ReplicationBridge to engine objects, per-type replication fragments/descriptors, filters (including a grid filter), prioritizers, and serialization that happens once and is shared across connections.
- **Networked physics (Chaos):** physics runs asynchronously at a fixed tick. There are two modes: *Predictive Interpolation* and full *Resimulation*, where the client runs about half an RTT ahead of the server. `NetworkPhysicsComponent` binds inputs to physics ticks ([Networked Physics](https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview)).

### 2.5 Large World Coordinates (LWC)
- UE5 moved `FVector` and transforms to double precision, raising the maximum world size from about 21 km to 88,000,000 km. On the GPU it uses camera-relative / tile-offset "translated world space" with float math, plus a double-float emulation type for the few operations that need it ([LWC](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-in-unreal-engine-5), [LWC rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5)).

### 2.6 World Partition, OFPA, Data Layers, HLOD, Level Instances, PCG
- **World Partition** replaces hand-made sublevels with a runtime **grid** that streams cells around *streaming sources* (players and other sources). Default Main grid settings are about 128 m cells with a 256 m loading range. The editor loads *regions* rather than the whole map ([World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)).
- **One File Per Actor (OFPA)** stores each actor in its own file, so many people can edit one map and merges or locks happen per actor.
- **Data Layers** group actors both in the editor and at runtime. At runtime they toggle between unloaded, loaded and activated to drive quests and events ([Data Layers](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---data-layers-in-unreal-engine)).
- **HLOD layers** bake proxy meshes and materials to represent unloaded cells and cut draw calls ([HLOD](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine)).
- **Level Instances / Packed Level Actors** provide reusable, prefab-like sub-levels.
- **PCG** is a node graph over spatial point data (transform, bounds, density, seed). It supports partitioned, hierarchical and **runtime** generation ([PCG](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-framework-in-unreal-engine), [PCG + WP](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-world-partition-in-unreal-engine)).

### 2.7 Rendering: Nanite, Lumen, Virtual Shadow Maps
- **Nanite:** meshes are pre-built into a hierarchy (DAG) of roughly 128-triangle clusters, with simplification that keeps group boundaries locked. At runtime a GPU persistent-thread traversal picks clusters by screen-space error. Culling uses frustum tests plus **two-pass HZB occlusion**. Small triangles go through a compute **software rasterizer** ("over 90%" of triangles in their example) and large ones through the hardware rasterizer. Both write a **visibility buffer** with 64-bit atomics, and materials are shaded afterwards in a deferred pass. Geometry streams in 128 KB pages, and the root page is always resident ([Nanite deep dive†](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)).
- **Lumen:** software ray tracing against mesh and global signed distance fields (hardware RT when available). Lighting is cached in a card-based **surface cache**, and the final gather uses screen-space probes backed by a world-space radiance cache ([Lumen blog](https://www.unrealengine.com/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen)).
- **Virtual Shadow Maps:** a 16k×16k virtual resolution split into 128×128 pages. Pages are allocated only where visible pixels need them and are cached across frames unless something invalidates them. The budget is the shared **physical page pool** ([VSM](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine), [VSM in Fortnite](https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4)).
- **Lesson:** VSM's page caching is achievable early. A Nanite-class continuous LOD system is a multi-year effort, but its prerequisites (a GPU scene, cluster culling, a visibility buffer) are worth building toward from the start.

### 2.8 Physics and animation
- **Chaos:** rigid bodies, destruction (geometry collections), cloth, vehicles, async fixed-tick physics (see 2.4).
- **Animation:** the AnimGraph runs inside Anim Blueprints with multithreaded update. **Control Rig** is procedural rigging on the RigVM. **Motion Matching** (PoseSearch) searches a pose database against the character's trajectory in place of hand-built locomotion state machines. The **IK Rig/IK Retargeter** retargets between different skeleton hierarchies, including auto-retargeting in 5.4 ([IK Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine)).
- **Lesson:** for an MMO with many body types and species, runtime retargeting plus motion matching reduces animation authoring cost.

### 2.9 Sequencer, Niagara, Material Editor
- **Sequencer** evaluates tracks with an internal ECS.
- **Niagara** is organized as a System, Emitters and Module stacks, with CPU or GPU simulation. *Simulation stages* and grid data interfaces make it a general GPU compute framework, and it has scalability overrides per quality level ([Niagara overview](https://www.strayspark.studio/blog/niagara-vfx-advanced-simulation-stages)).
- **Material editor:** node graphs compile to HLSL permutations, and Material Instances override parameters without recompiling. This is the source of Unreal's well-known PSO/permutation explosion (compare Godot's warning in §3).

### 2.10 Blueprints and Verse
- Blueprints run on a bytecode VM. **Nativization was deprecated in 4.27 and removed in 5.0**, so the performance path is now "profile, then rewrite hot graphs in C++" ([forum](https://forums.unrealengine.com/t/why-was-blueprint-nativization-removed-no-code-preaching/232490), [BP VM analysis](https://intaxwashere.github.io/blueprint-performance/)).
- **Verse** (UEFN) is a statically typed functional-logic language with failure contexts and transactional (rollback-on-failure) semantics. Epic has stated it will be the primary language for Scene Graph.
- **Lesson:** treat visual scripting as orchestration over native systems. Compiling graphs to a JIT-able target (Lua for us) avoids maintaining a separate VM or a nativizer.

### 2.11 Slate and editor architecture; Multi-User Editing
- The editor is built on **Slate**, a declarative C++ UI framework (UMG wraps it for games). Details panels are generated from reflection, with per-type customization hooks and metadata such as `ClampMin` and `Category`.
- **Transactions:** `FScopedTransaction` plus `UObject::Modify()` snapshots object state *by serialization* into the undo buffer. That makes undo generic for any reflected object.
- **Multi-User Editing** (a Concert server) records these transactions and rebroadcasts them to every client, and late joiners replay the history. The server stores only transactions and modified assets, not the whole project ([Multi-User overview](https://dev.epicgames.com/documentation/unreal-engine/multi-user-editing-overview-for-unreal-engine)).

### 2.12 Asset registry, DDC, cooking, Pak/IoStore, Zen, UGS/Horde
- **Asset Registry / Asset Manager:** an index of asset metadata and dependencies that can be queried without loading assets. It drives the content browser, cooking, *primary vs secondary assets*, soft references, asset bundles and **chunk assignment** (one .pak per chunk, used for DLC and patching) ([Asset Management](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine), [Cooking & chunks](https://dev.epicgames.com/documentation/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine)).
- **Derived Data Cache (DDC):** compiled shaders and cooked asset forms, keyed so they can be regenerated from source. Since 5.4 the local DDC is a **Zen Store** process. **Zen** also stores cooked output as references into a **content-addressable** layer and serves the editor over HTTP ([DDC](https://dev.epicgames.com/documentation/unreal-engine/using-derived-data-cache-in-unreal-engine), [Zen cooked output](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-zen-storage-server-as-cooked-output-store-for-unreal-engine)).
- **IoStore** (`.utoc/.ucas`) addresses data by **chunk ID** through the I/O dispatcher instead of by file path. The **Zen Loader** is built on top of it ([Zen Loader](https://dev.epicgames.com/documentation/unreal-engine/zen-loader-in-unreal-engine)).
- **Unreal Game Sync + Horde:** CI (BuildGraph on Horde agents) builds the editor for each changelist. Artists sync **precompiled binaries** instead of compiling. Horde also runs test automation and distributed compilation ([UGS PCBs](https://dev.epicgames.com/documentation/unreal-engine/using-precompiled-binaries-in-unreal-game-sync-for-unreal-engine), [Horde+UGS](https://dev.epicgames.com/documentation/en-us/unreal-engine/horde-unrealgamesync-tutorial-for-unreal-engine)).

---

## 3. Godot 4

- **Layering:** Core (Object/ClassDB, Variant, memory, I/O) → **Servers** ("singleton objects initialized at engine startup" that implement rendering, audio and physics) → **Scene** (the node tree) → Drivers/Platform ([architecture overview](https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/godot_architecture_diagram.rst)).
- **Servers and RIDs:** "the whole scene system is *optional*". Code can drive the RenderingServer and PhysicsServer directly through opaque **RIDs**, which are manually managed and not reference-counted. That path suits "tens of thousands of instances". The servers run asynchronously, and **querying them forces a sync**, so the API is designed to be write-mostly ([using servers](https://github.com/godotengine/godot-docs/blob/master/tutorials/performance/using_servers.rst)).
- **Why Godot isn't ECS:** "composition at a higher level". Nodes hold both data and logic, and the data-oriented optimization lives inside the servers. The article concedes that ECS is warranted for games with thousands of simultaneous objects ([article](https://github.com/godotengine/godot-website/blob/master/collections/_article/why-isnt-godot-ecs-based-game-engine.md)). An MMO zone server is exactly that case.
- **The editor is a Godot app:** "the Godot editor runs on the game engine. It uses the engine's own UI system, it can hot-reload code and scenes". `@tool` scripts extend the editor with the same code the game uses ([design philosophy](https://github.com/godotengine/godot-docs/blob/master/getting_started/introduction/godot_design_philosophy.rst)).
- **Scenes and resources** are text `.tscn`/`.tres` files, "human-readable and easy for version control". Each has a **string UID** (`uid://…`) so references survive renames ([TSCN format](https://github.com/godotengine/godot-docs/blob/master/engine_details/file_formats/tscn.rst)).
- **RenderingDevice + Forward+:** RenderingDevice is a WebGPU-level abstraction over Vulkan, D3D12 and Metal. **Forward+** assigns lights, decals and reflection probes to a 3D frustum grid in compute shaders, with up to 512 omni/spot lights per cluster. The Mobile renderer instead uses **subpasses** to stay in tile memory. Godot rejects deferred shading because clustered forward gives "a better tradeoff for performance versus flexibility" (including MSAA). It warns that one extra `#define` can *double* shader variants. Occlusion culling runs on the CPU with Embree. Other features: reverse-Z, a Hi-Z buffer ([internal rendering architecture](https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/internal_rendering_architecture.rst), [renderers](https://github.com/godotengine/godot-docs/blob/master/tutorials/rendering/renderers.rst)).
- **GDScript** is designed around engine types and a fast edit/reload loop, and it won over non-programmers. **GDExtension** is a stable **C ABI** (opaque pointers, proc-address loading) that lets C++, Rust and other languages add classes that look native in the editor, with optional hot reload (`reloadable = true`). Hot reload regressed across 4.2.x, which shows how fragile native hot reload is ([godot-cpp#1589](https://github.com/godotengine/godot-cpp/issues/1589)).
- **Jolt:** Godot 4.4 integrated Jolt as a built-in module, ported from the Godot Jolt extension with help from Jorrit Rouwe. Current docs state that new projects default to it ([using Jolt](https://github.com/godotengine/godot-docs/blob/master/tutorials/physics/using_jolt_physics.rst)).
- **Large worlds:** an opt-in `precision=double` build. Shaders stay in float and "emulate double precision for rendering using single-precision floats". Known holes include triplanar mapping, world-space GPU particles and `world_vertex_coords`. The GDExtension ABI also changes in double builds ([LWC](https://github.com/godotengine/godot-docs/blob/master/tutorials/physics/large_world_coordinates.rst)).
- **VisualScript removal:** only 0.5% of users used it as their main language. The reasons given: it lacked the high-level game features that Unreal, GameMaker and Construct package with their visual scripting; there were no docs examples; users preferred GDScript. The stated lesson is that Godot "must always be developed user-facing first" ([article](https://github.com/godotengine/godot-website/blob/master/collections/_article/godot-4-will-discontinue-visual-scripting.md)).

---

## 4. Unity DOTS, O3DE, Bevy, Frostbite, id Tech, GPU-driven pipelines, fibers

### 4.1 Unity DOTS
- **Entities:** an archetype is a unique combination of component types. Entities of one archetype live in **16 KiB chunks** made of parallel SoA arrays plus an entity-ID array. Adding or removing components moves the entity to a different archetype. **Structural changes** (create/destroy, add/remove, shared-component set) are expensive and synchronizing, so they are deferred through **Entity Command Buffers** or avoided with enableable components ([archetypes](https://github.com/needle-mirror/com.unity.entities/blob/master/Documentation~/concepts-archetypes.md), [structural changes](https://github.com/needle-mirror/com.unity.entities/blob/master/Documentation~/concepts-structural-changes.md)).
- **Baking** converts authoring GameObjects into runtime entity data. It happens only in the editor, "like asset importing". It runs incrementally while a subscene is open ("live baking") and asynchronously in the background when it is closed ([baking](https://github.com/needle-mirror/com.unity.entities/blob/master/Documentation~/baking-overview.md)). This is the cleanest statement of the authoring/runtime split.
- **Burst** compiles a restricted C# subset (HPC#) through LLVM. It is paired with a job system whose safety checks detect data races on native containers ([Burst](https://github.com/needle-mirror/com.unity.burst/blob/master/Documentation~/index.md)).
- **Netcode for Entities:** the client and server are separate **worlds**, which can live in one process, plus stripped-down **thin-client** worlds for load testing in the editor. **Ghosts** are server-owned. Each ghost prefab declares importance, a supported/default mode (interpolated, predicted or owner-predicted), static vs dynamic optimization, and a MaxSendRate. The server builds snapshots per chunk from an **importance priority queue** (base importance × ticks since last sent × user scaling such as distance tiles) until it hits the packet budget. Prediction rolls back to the last snapshot and resimulates in `PredictedSimulationSystemGroup`. At **300 ms that means about 22 resimulated ticks per frame**, so *prediction switching* limits full prediction to nearby or owned ghosts ([ghosts & snapshots](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/ghost-snapshots.md), [prediction](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/prediction-n4e.md), [optimize ghosts](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/optimization/optimize-ghosts.md), [thin clients](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/testing/thin-clients.md)).

### 4.2 O3DE
- **Modularity:** the engine is AzCore → AzFramework → AzGameFramework / AzToolsFramework, and **Gems** hold everything else, including Atom itself. Builds pull in only the Gems a project enables ([key concepts](https://github.com/o3de/o3de.org/blob/main/content/docs/welcome-guide/key-concepts.md)).
- **AZ reflection** has three contexts built with the builder pattern: **SerializeContext** (persistence), **EditContext** (editor UI) and **BehaviorContext** (Script Canvas and Lua bindings) ([reflection](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/programming/components/reflection/_index.md)). Serialization supports JSON (human-editable), XML and binary ([serialization](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/programming/serialization/_index.md)).
- **Atom:** the **RHI** abstracts DX12, Vulkan and Metal. Its **Frame Scheduler** "represents render passes as nodes in a graph" to get multithreaded command recording, transient memory reuse, async compute and on-chip render targets on mobile ([RHI](https://github.com/o3de/o3de.org/blob/main/content/docs/atom-guide/dev-guide/rhi/rhi.md)). The **pass system** authors passes as JSON data for iteration, as C++ for performance, or both. Attachments are declared Input, Output or InputOutput ([pass system](https://github.com/o3de/o3de.org/blob/main/content/docs/atom-guide/dev-guide/passes/pass-system.md)).
- **Prefabs** nest, and instances receive edits from the source file ([prefab basics](https://github.com/o3de/o3de.org/blob/main/content/docs/learning-guide/tutorials/entities-and-prefabs/entity-and-prefab-basics.md)). **EBus / AZ::Event** provide pub/sub messaging ([EBus](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/programming/messaging/ebus.md)).
- **Asset Processor** is a background daemon. It detects source changes, dispatches Create/Process jobs to **Asset Builders**, tracks source, job and product dependencies, writes to the Asset Cache, and **notifies the editor and runtimes to hot reload**. It can also serve assets over the network to a launcher running on a devkit ([Asset Processor](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/assets/asset-processor/_index.md)).
- **Multiplayer Gem:** server-authoritative, push-based network properties, RPCs and local prediction ([multiplayer](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/networking/multiplayer/_index.md)).

### 4.3 Bevy
- **ECS everywhere:** systems are plain functions whose parameter types declare their data access. The parallel executor uses those declarations to run non-conflicting systems concurrently, and explicit ordering goes through system sets. Components can be stored in **tables** (fast iteration, the default) or **sparse sets** (cheap add/remove). All component and resource mutations get **change detection** (`Changed<T>`) ([bevy_ecs README](https://github.com/bevyengine/bevy/blob/main/crates/bevy_ecs/README.md)). Bevy 0.16 added **relationships** (entity-to-entity links), GPU-driven rendering and occlusion culling ([0.16 notes](https://github.com/bevyengine/bevy-website/blob/main/content/news/2025-04-24-bevy-0.16/index.md)).
- **Render World pattern:** an `ExtractSchedule` copies what the renderer needs from the main world into a separate render world. The render schedule then runs Prepare → Queue → Sort → Render → Cleanup. **Pipelined rendering** renders frame N on another thread while frame N+1 simulates ([bevy_render](https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/lib.rs), [pipelined_rendering](https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/pipelined_rendering.rs)). On current `main`, the **render graph is itself an ECS schedule**: `RenderGraph` has `Core3d`/`Core2d` sub-schedules, so passes use the same scheduler as gameplay ([renderer/mod.rs](https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/renderer/mod.rs)).

### 4.4 Frostbite FrameGraph (GDC 2017)
Each frame is built in three phases. **Setup:** passes declare the resources they create, read and write, as code lambdas. **Compile:** the graph culls passes nobody reads, computes resource lifetimes, **aliases transient memory**, and places barriers and async-compute work. **Execute:** the graph runs the passes' recording callbacks. Resources are either *transient* (owned by the graph) or *imported*, and a "blackboard" shares handles between passes ([FrameGraph talk†](https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in), [reference implementation](https://github.com/skaarj1989/FrameGraph)). This is now the industry-standard shape, and O3DE's Frame Scheduler and Unreal's RDG follow it.

### 4.5 id Tech 6/7
id Tech 6 (DOOM 2016) introduced a **clustered forward** renderer: lights, decals and probes binned into a frustum grid, with async compute on consoles and a Vulkan backend ([idTech 666†](https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pdf)). id Tech 7 (DOOM Eternal) is Vulkan-only on PC. It dropped MegaTexture, moved to compute-heavy, GPU-side binning of lights and decals (plus geometry decals), and deliberately keeps its **pipeline-state count small** (hundreds, not tens of thousands), which avoids shader-compile stutter ([Rendering the Hellscape of DOOM Eternal†](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf)). The lesson is that a disciplined uber-shader and material model beats unbounded material-graph permutations.

### 4.6 GPU-driven rendering (Ubisoft/RedLynx 2015; Frostbite 2016)
In Assassin's Creed Unity and Trials, meshes are split into fixed-size **clusters** (64-vertex strips in ACU). The GPU then does all the culling. It culls instances, expands them into cluster chunks, and culls clusters (frustum, backface cone, and occlusion against a depth pyramid from reprojected previous-frame depth plus a current-frame second pass). It compacts the surviving indices and issues **multi-draw indirect**, so the CPU submits a near-constant number of draws. The result was an order of magnitude more objects ([GPU-Driven Rendering Pipelines†](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)). Frostbite's follow-up added per-triangle compute culling ([Optimizing the Graphics Pipeline with Compute†](https://www.gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With)). Nanite is the continuation of this line.

### 4.7 Naughty Dog fiber job system (GDC 2015)
Worker threads are locked one per core (6 on PS4), and jobs run on a pool of **fibers** (160 per the talk: many small-stack plus a few large-stack). There are three priority queues. **Atomic counters** replace locks for dependencies: a job that waits on a counter *yields its fiber* instead of blocking the thread. The frame is **pipelined** across stages (game logic for frame N, render prep for N-1, GPU for N-2). A **tagged-heap** allocator hands out 2 MiB blocks tagged by frame stage and frees each tag in bulk ([Parallelizing the Naughty Dog Engine†](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine)). Off-the-shelf equivalents exist, for example Google's Marl hybrid thread/fiber scheduler ([marl](https://github.com/google/marl)).

---

## 5. Comparison table

| | Object/entity model | Reflection | Scripting | Render architecture | GPU-driven | Large worlds | Net model | Asset pipeline | Editor tech |
|---|---|---|---|---|---|---|---|---|---|
| **UE5** | Actor/Component + Mass (ECS) + Scene Graph (UEFN) | UHT codegen; tracing GC | Blueprints (VM), Verse (UEFN), C++ | Deferred; RDG render graph; Nanite/Lumen/VSM | Yes (Nanite visibility buffer, SW raster) | Doubles on CPU; tile-offset floats on GPU | Legacy + RepGraph → Iris; GAS prediction keys; Chaos resim | Asset Registry, DDC, Zen CAS, IoStore chunks | Slate (custom C++ UI); transaction-based undo; Multi-User |
| **Godot 4** | Node tree + Resources; servers with RIDs | ClassDB + `_bind_methods` | GDScript, C#, GDExtension (C ABI) | Forward+ clustered, Mobile subpasses, GL compat | Limited (CPU occlusion via Embree) | `precision=double` build; emulated doubles in shaders | High-level multiplayer API | Import to `.godot/`; text `.tscn` + UIDs | Editor runs on the engine's own UI |
| **Unity DOTS** | Archetype ECS (16 KiB chunks) + GameObject authoring via baking | Source generators + C# reflection | C# (Burst HPC#) | SRP (URP/HDRP), GPU Resident Drawer | Partial | Float; origin shifting | Netcode for Entities ghosts, importance queue, rollback | Importer + baking, subscenes | IMGUI/UI Toolkit |
| **O3DE** | Component entities + nested prefabs | AZ Serialize/Edit/Behavior contexts | Lua, Script Canvas | Atom: RHI + Frame Scheduler + data-driven passes | Partial | Float | Multiplayer Gem (push-based, prediction) | Asset Processor daemon, builders, hot reload | Qt |
| **Bevy** | Pure ECS (tables/sparse sets, relationships) | `bevy_reflect` derive | Rust | Extract → Render World; render graph as schedules | Yes (0.16+) | Float | Third-party | Asset server with processors | Editor in progress |
| **Frostbite** | Proprietary entity system | Proprietary | Proprietary | FrameGraph (the origin of the pattern) | Yes (compute culling) | n/a | Proprietary | Proprietary | Proprietary |
| **id Tech 7** | Proprietary | Proprietary | Proprietary | Clustered forward, Vulkan-only (PC), few PSOs | GPU binning/compute-heavy | n/a | n/a | Proprietary | Proprietary |

---

## 6. Recommended reference architecture for our engine

### 6.1 Principles
1. **One entity model, data-oriented at the core, with a friendly authoring surface on top.** Godot's ergonomics come from its scenes, prefabs and inspector, not from inheritance.
2. **Headless first:** the zone/cell server is the same engine binary without the Render, Audio and UI servers. Anything the server needs must not depend on rendering.
3. **One reflection schema, many consumers:** serialization, editor, undo, scripting, replication and diffing.
4. **Handles, not pointers,** across all subsystem boundaries (RIDs, generational IDs), with no engine-wide tracing GC.
5. **Everything derived is content-addressed and reproducible:** DDC, cooked output, containers and patches.
6. **Budgets are explicit:** frame time per job graph, memory per tag, and bandwidth per connection.

### 6.2 Module layering (a strict DAG, enforced in CMake)
```
L5 Apps        Client | ZoneServer (headless) | Editor | AssetTools CLI | Launcher/Patcher | Bots/ThinClients
L4 Framework   Gameplay (Abilities/Attributes/Effects/Tags) · Scripting host (LuaJIT + graph compiler)
               World (ECS world, hierarchy, prefabs, streaming grid, PCG runtime) · Replication · UI runtime
L3 Servers     RenderServer (RHI→RenderGraph→GPU scene) · PhysicsServer (Jolt) · AudioServer
               AnimServer · NavServer · StreamingServer (IO/decompress) · NetTransport
               (servers expose command-style, handle-based APIs; queries are async or deferred)
L2 Foundation  Reflection registry + codegen output · Serialization (text/binary/net-quantized)
               Asset handles/registry client · VFS/container mounts · Config/CVars · Events/messages
L1 Core        Platform · Memory (allocators, tags) · Containers · Math (f32 + f64 world types)
               Job system (fibers, counters) · Strings/Names · Hashing · Logging · Profiling (Tracy hooks)
EditorOnly     ToolsFramework (transactions, selection, commands, asset DB) → EditorUI (ImGui)
```
Rules: editor-only modules may never be linked into Client or ZoneServer. Server targets compile with `ENGINE_NO_RENDER`, and CI builds that configuration on every commit. Plugins ("Gems") follow O3DE's model: a module plus manifest plus assets, opted into per project.

### 6.3 Threading, job system and frame pipeline
- **Job system:** fiber-based, following Naughty Dog and Marl. One worker per core minus reserved cores, a fiber pool, three priorities, and counter-based waits that yield the fiber. Rules: no TLS across waits, and no OS mutex held across a wait. **Separate long-running pools** handle I/O, decompression, shader and pipeline compiles, and pathfinding so they never starve frame jobs.
- **ECS scheduler** (Bevy/Mass/Unity pattern): systems declare read/write component access, and the scheduler builds a job DAG, running non-conflicting systems in parallel and splitting chunks inside a system. Structural changes go through **command buffers** applied at sync points.
- **Jolt:** implement `JPH::JobSystemWithBarrier` on top of our scheduler so physics shares the worker threads ([Jolt JobSystem.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Core/JobSystem.h)).
- **Client frame pipeline:** simulate N (a fixed-tick networked sim plus variable-rate presentation) → **extract** render-relevant changes into the RenderServer's world (Bevy extract / Godot server commands) → the render thread builds the graph and records command buffers in parallel jobs for N-1 → the GPU executes N-2.
- **ZoneServer:** fixed tick (configurable, e.g. 20–30 Hz). The per-tick job graph runs input → sim → physics → gameplay → replication gather → send. Nothing touches rendering.

### 6.4 Memory
- **Tagged frame heaps** in 2 MiB blocks (Naughty Dog), with a tag per pipeline stage, freed after the matching GPU fence or tick.
- **ECS chunk allocator** with 16–64 KiB chunks.
- **Pools with generational handles** for server objects.
- **GPU:** VMA sub-allocation. The render graph aliases transient memory. Bindless descriptor slots come from free-list allocators. GPU-scene buffers are persistent and updated with scatter uploads.
- **Tracking:** every allocation carries a subsystem tag, with budgets and dashboards in the profiler. Server builds must have deterministic memory ceilings per zone.
- **Scripting memory:** Lua heaps are separate per VM (one per zone on the server) and GC'd incrementally under a per-frame time budget. The C++ side holds Lua objects only by registry handle.

### 6.5 Reflection and serialization
- **Now:** annotate C++ types (`REFLECT`, `PROPERTY(meta…)`). A **clang-based header tool** runs during the build (UHT-style) and generates:
  - type info: fields, offsets, attributes and stable field IDs;
  - binary and text serializers;
  - editor metadata;
  - Lua bindings (O3DE's BehaviorContext equivalent);
  - **replication descriptors** (quantization, conditions, owner-only flags; the Iris-style descriptor).
- **Later:** keep the generated API stable so the backend can move to **C++26 static reflection** (P2996) once compilers support it. This is the only argument for moving past C++20, and it isn't urgent.
- **Formats:**
  1. **Source text:** deterministic key order, one entity per file (OFPA), GUID-based references (Godot UIDs), and property-path diffs for prefab overrides.
  2. **Cooked binary:** flat, zero-copy/relocatable, and versioned by schema hash.
  3. **Network:** bit-packed and quantized, generated from the same descriptors.
- **Versioning:** field renames go through redirect tables. Types can supply upgrade functions keyed by schema version.

### 6.6 Entity model: ECS plus hierarchy plus prefabs (not a scene graph)
- Use an **archetype ECS** as the single runtime model for client and server: dense SoA chunks, tags, enableable components, change detection, and **relationships** for parent/child, attachments, ownership and docking (Bevy 0.16, flecs). Evaluate **flecs** (archetype storage with built-in relationships and hierarchies) against an in-house ECS before committing. EnTT's sparse-set design is weaker for chunk-based replication and GPU upload.
- The **scene tree is a view**, not a storage model. The editor outliner shows hierarchy and prefab instances (Godot/O3DE UX) and edits ECS data through reflection.
- **Prefabs** nest, and their overrides are stored as property-path patches. Authoring components may differ from runtime components. A **bake step** (Unity) strips editor-only data during cook.
- **Why not Actors or a scene graph:** an MMO zone server holds tens of thousands of NPCs, projectiles, ships and loot entities. Unreal needed RepGraph, Mass and Iris to scale Actors. Godot itself concedes that ECS fits this case.

### 6.7 Large-world coordinates and reference frames
- **World positions** use `f64` (`WorldVec3`) on the CPU. Everything else (rotations, velocities, local offsets) stays `f32`.
- **Rendering** is camera-relative: CPU doubles are converted to floats relative to the camera, and a per-view high-precision origin is uploaded. There is no double math in shaders (Unreal LWC, Godot).
- **Physics:** build Jolt with `JPH_DOUBLE_PRECISION` (5–10% slower). Following Jolt's space-sim guidance, use **multiple PhysicsSystems** for moving reference frames: a ship interior, a planet surface grid, open space. Keep velocities low inside each system and transfer bodies between systems at boundaries ([Jolt Big Worlds/Space](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)). Jolt's broadphase stays float, so resolution degrades to about 1 m at 10,000 km. Keep each system's origin local.
- **Hierarchical frames:** galaxy → system → body → grid → interior. These are first-class ECS relationships, and replication quantizes relative to the frame or cell origin.

### 6.8 Rendering: RHI, render graph, bindless, GPU-driven
- **RHI:** a thin abstraction at roughly Godot RenderingDevice or O3DE RHI level. Implement Vulkan 1.3 first, and keep the interface portable to D3D12, Metal and consoles. Baseline Vulkan features: dynamic rendering, synchronization2, timeline semaphores, descriptor indexing, and buffer device address. Optional: mesh shaders and descriptor buffers.
- **Render graph** (FrameGraph shape): setup/compile/execute, pass culling, automatic barriers and layout transitions, transient aliasing, async compute queues, parallel recording, and a graph visualizer. Passes are C++ lambdas, with an optional data-driven layer for pipeline composition (Atom).
- **Bindless:** one global descriptor heap holding texture, sampler and buffer arrays. Materials become parameter blocks in GPU buffers indexed by material ID. Use a **small uber-shader family with specialization constants** and a hard permutation budget, following id Tech and Godot's `#define` warning. Maintain a pipeline cache and precompile PSOs from recorded usage.
- **GPU-driven:** a persistent **GPU scene** (instances, transforms, bounds, material IDs). Compute culling runs on instances, then on **meshlets/clusters**, with frustum, cone and **two-phase HZB occlusion**. Survivors become compacted `DrawIndexedIndirectCount` calls, or mesh-shader dispatches where supported.
- **Shading:** clustered forward+ (Godot/id) as the baseline. Add a **visibility buffer** for dense geometry later. Later still: cached **virtual shadow map pages** for the sun, then continuous cluster LOD (Nanite-lite) and dynamic GI (probe-based or RT).

### 6.9 Asset pipeline
```
Source (glTF/FBX, PNG/EXR, WAV, .entity text, .graph)  ──Importer──▶  Intermediate (engine-native, platform-neutral)
      │  AssetID = stable GUID; importer version + settings in sidecar           │
      ▼                                                                           ▼
Asset Processor daemon (watch, job DAG, dependency tracking)  ──Cooker(platform)──▶  Cooked blobs
      │   DDC key = H(source bytes, settings, builder version, platform, deps)     │  stored in local + shared CAS (Zen-like HTTP)
      ▼                                                                           ▼
Asset Registry (metadata, tags, deps; queryable unloaded)   ──Packager──▶  Containers: TOC(assetID→chunk hashes) + chunk data
                                                                                  (zstd/LZ4 per chunk, signed; optional encryption)
Launcher/Patcher: diff TOCs, download only missing chunk hashes from CDN; streaming-install by chunk group
```
- **Hot reload:** Asset Processor → editor, client and zone servers over a local socket (O3DE) → swap by handle. Covers textures, meshes, materials/shaders, Lua and entity data. Devkits can mount assets remotely over the network.
- **Chunk assignment** follows Unreal's primary-asset rules: zones, planets and ship packs map to install and stream groups.
- **Server cooks** strip rendering data (textures, LODs) and keep collision, navigation and gameplay data.

### 6.10 Editor architecture
- **Split into two modules.** `ToolsFramework` holds no UI: documents, selection, commands, transactions, asset DB client, and scripting and automation. `EditorUI` renders it with ImGui (docking + multi-viewport). The split makes headless editor automation, tests and batch tools possible.
- **Undo/redo:** scoped transactions record **property-path diffs via reflection** (before/after serialized values keyed by entity GUID), plus create/delete operations. This is generic (Unreal's snapshot approach) and compact. The same stream feeds a crash-recovery journal and, later, a Concert-style **multi-user** server.
- **Inspector:** generated from reflection metadata (ranges, units, enums, categories, asset pickers). Per-type customizers override it, like Unreal's IDetailCustomization.
- **Viewports** use the real RenderServer. Editor-only gizmos live in their own render-graph passes.
- **Play-in-editor:** spawn a **local ZoneServer**, in-process as a separate ECS world or out of process (the default, for fidelity), plus N clients. Include **thin/bot clients** for load (Netcode for Entities) and simulated latency and loss.
- **Large-world editing:** region loading (World Partition), data layers, OFPA files, CI-built HLOD and navigation, and a PCG graph editor with deterministic seeds.
- **Shared widgets** built once on ImGui: node-graph editor (visual scripting, materials, PCG, abilities), curve and timeline editor (sequencer, animation), asset browser.

### 6.11 Gameplay framework and scripting
- Build a **GAS-equivalent** natively on the ECS: attributes as components, effects as data assets (instant, duration, infinite, periodic; stacking; tag requirements), hierarchical **tags** interned to bitsets, and abilities with prediction keys limited to costs, cosmetics and movement. Cues run only on the client.
- **LuaJIT** is for gameplay scripts and mods. The **visual graph compiles to Lua source**, so it gets the JIT for free and can be debugged as text, and there is no second VM. Nodes should be **domain-level** (abilities, quests, dialogue, missions, spawners, UI flows), which is the lesson of Godot's VisualScript. Native C++ systems do the heavy per-entity work (the Blueprint lesson).

### 6.12 Engine-side replication hooks (for the zone servers)
The pipeline for each tick:
1. **Descriptors** come from reflection.
2. A **quantized shadow state** is kept per replicated entity (Iris).
3. **Dirty bits** come from ECS change detection (push model).
4. **Filtering** uses an interest grid shared with the streaming and cell grid (RepGraph GridSpatialization).
5. **Prioritization** is importance × staleness × distance under a per-connection bandwidth budget (Netcode for Entities).
6. **Serialize once per entity per tick** and share the result across connections, with delta compression against acked baselines.

Prediction: owner-predicted player and vehicle state with rollback. Everyone else is interpolated. Physics resimulation only for the controlled vehicle.

### 6.13 World streaming and procedural content
- **Streaming:** a World Partition-style grid, with streaming sources on the client (camera, players) and interest-driven cells on the server.
- **Per cell:** OFPA entity files, data layers for events and phasing, and HLOD proxies.
- **PCG:** a deterministic, seedable graph library that runs in three places: the editor (baked), the client at runtime, and the server at runtime (collision and gameplay-relevant outputs only). This matters most for planets and asteroid fields.

### 6.14 Build and CI
- **Build:** CMake presets and Ninja. Clang on Linux (servers, CI) and MSVC or clang-cl on Windows. Unity builds, sccache/ccache, and distributed compilation (FASTBuild/Incredibuild class; Unreal's Horde and UBA show the payoff).
- **Precompiled editor binaries** for content creators, UGS-style: CI publishes editor builds per commit, and a sync tool (which can share code with the launcher) pulls them.
- **CI graph** (Horde/BuildGraph-like):
  - **Per commit:** compile every target and configuration (including `NO_RENDER`), unit tests, reflection-codegen validation, headless server + bot smoke test, incremental cook of a test map, shader/PSO compile.
  - **Nightly:** full cook, packaging, perf captures against baselines, memory reports, and a soak test with 1k thin clients.
- **Infrastructure:** a shared DDC/CAS cache server, a symbol server, crash reporting, and Tracy captures stored as artifacts.
- **Source control:** decide early. OFPA and text entity files reduce conflicts. Binary content needs locking (Perforce, or Git+LFS with locks).

### 6.15 Flags on the tentative stack
- **C++20:** fine. Plan for C++26 reflection later; don't block on it.
- **Vulkan 1.3:** fine for PC and Linux servers, which don't render. Keep an RHI seam if consoles are ever on the table.
- **Jolt:** strongly supported. Double-precision mode and multi-system reference frames fit space games, and cross-platform determinism is an option at about 8% cost with caveats: broadphase queries and callback order are nondeterministic.
- **LuaJIT:** the JIT is unavailable on platforms that forbid executable memory (consoles, iOS), where it falls back to the interpreter, and upstream releases are infrequent. Consider **Luau** (typed, sandboxed, fast interpreter) as an alternative worth a spike, especially for server-side and mod sandboxing.
- **Dear ImGui:** acceptable (Godot shows that self-hosted UI works), but expect to build a substantial widget layer: graphs, timelines, property grids, IME and high-DPI. O3DE chose Qt and Unreal built Slate. Budget accordingly.

---

## 7. Requirements

| ID | Pri | Requirement | Rationale / source |
|---|---|---|---|
| ENG-01 | P0 | Strict module DAG (Core → Foundation → Servers → Framework → Apps), enforced by the build. Editor code never links into Client or Server. | Godot layers, O3DE Az* modules |
| ENG-02 | P0 | Headless `NO_RENDER` ZoneServer target built from the same engine, on CI every commit | Godot "servers optional", Netcode server world |
| ENG-03 | P0 | Fiber job system: counters, priorities, separate I/O/compile pools, Tracy instrumentation | Naughty Dog GDC 2015, Marl |
| ENG-04 | P0 | ECS scheduler derives parallelism from declared component access; deferred structural changes via command buffers | Bevy, Mass, Unity ECB |
| ENG-05 | P0 | Clang-based reflection codegen generating serializers, editor metadata, Lua bindings and replication descriptors from one annotation set | UHT, O3DE 3 contexts |
| ENG-06 | P0 | Archetype ECS with tags, enableable components, change detection and relationships as the single runtime entity model | Mass, Unity, Bevy 0.16 |
| ENG-07 | P0 | f64 world positions on CPU; camera-relative f32 on GPU; Jolt `JPH_DOUBLE_PRECISION` | UE LWC, Godot LWC, Jolt |
| ENG-08 | P0 | RHI over Vulkan 1.3 plus FrameGraph-style render graph (culling, auto barriers, transient aliasing, async compute) | Frostbite, O3DE Frame Scheduler |
| ENG-09 | P0 | Bindless descriptor heap; materials as GPU data; capped uber-shader permutations; PSO cache and precompile | id Tech 7, Godot `#define` warning |
| ENG-10 | P0 | GPU scene plus compute instance culling (frustum + HZB) plus indirect draws; clustered forward+ lighting | GPU-Driven 2015, Godot Forward+ |
| ENG-11 | P0 | Stable asset GUIDs; importer → DDC (input-hash keyed) → cooker; Asset Processor daemon with dependency tracking and hot-reload notifications | UE DDC, O3DE AP, Godot UIDs |
| ENG-12 | P0 | Content-addressed cooked store and chunked containers (TOC → chunk hashes) enabling delta patching | Zen, IoStore |
| ENG-13 | P0 | Asset registry (metadata and dependencies, queryable without loading) | UE Asset Registry |
| ENG-14 | P0 | Editor: ToolsFramework/EditorUI split; reflection-diff transactions (undo/redo); reflection-generated inspector | UE transactions/Details |
| ENG-15 | P0 | PIE launches a local ZoneServer plus N clients, with latency and loss simulation | Netcode worlds, UE PIE |
| ENG-16 | P0 | Tagged frame heaps, pooled handles, per-subsystem memory tags and budgets; no engine-wide tracing GC | Naughty Dog tagged heap, UE GC lessons |
| ENG-17 | P0 | CI: all targets per commit, unit + headless bot smoke tests, shared DDC, precompiled editor binaries for artists | Horde/UGS |
| ENG-18 | P1 | Replication layer: quantized shadow state, push dirty bits, spatial interest grid, importance × staleness prioritizer, serialize-once | Iris, RepGraph, Netcode |
| ENG-19 | P1 | GAS-equivalent (attributes, effects, hierarchical tags, abilities, cues, prediction keys) native to the ECS | UE GAS |
| ENG-20 | P1 | Grid world streaming with streaming sources, one file per entity, data layers, CI-built HLOD | World Partition |
| ENG-21 | P1 | Nested prefabs with property-path overrides; authoring → runtime bake step | O3DE prefabs, Unity baking |
| ENG-22 | P1 | Meshlet/cluster culling with two-phase HZB occlusion; mesh-shader path where supported | GPU-Driven 2015, Nanite |
| ENG-23 | P1 | Multiple Jolt PhysicsSystems for moving reference frames (ships, planets) with body transfer | Jolt space-sim guidance |
| ENG-24 | P1 | Visual scripting graph compiled to Lua, with domain-level node libraries | Godot VisualScript lesson, BP nativization removal |
| ENG-25 | P1 | Deterministic, seedable PCG graph runtime shared by editor, client and server | UE PCG |
| ENG-26 | P1 | Cached virtual shadow map pages for directional lights | UE VSM |
| ENG-27 | P1 | Thin/bot clients for load testing in editor and CI | Netcode thin clients |
| ENG-28 | P1 | Animation: runtime retargeting (IK retarget) and motion-matching-ready pose database | UE IK Retargeter, Motion Matching |
| ENG-29 | P2 | Visibility buffer plus continuous cluster LOD (Nanite-lite) with streamed geometry pages | Nanite |
| ENG-30 | P2 | Dynamic GI (probe or surface-cache based; HW RT optional) | Lumen |
| ENG-31 | P2 | Multi-user editing via a transaction broadcast server | UE Multi-User |
| ENG-32 | P2 | Stable C ABI plugin interface (GDExtension-style) for native mods and tools | GDExtension |
| ENG-33 | P2 | Data-driven render pipeline composition (JSON passes) for tech-art iteration | O3DE Atom passes |
| ENG-34 | P2 | GPU compute VFX framework with simulation stages (Niagara-class) | Niagara |

---

## 8. Sources

**Unreal Engine** (dev.epicgames.com content via search excerpts)
- Iris: https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-iris-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine
- Replication Graph: https://dev.epicgames.com/documentation/unreal-engine/replication-graph-in-unreal-engine · https://www.unrealengine.com/tech-blog/replication-graph-overview-and-proper-replication-methods
- Networked physics: https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview
- LWC: https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-in-unreal-engine-5 · https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5
- World Partition, Data Layers, HLOD, PCG: https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---data-layers-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-framework-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-world-partition-in-unreal-engine
- Mass: https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine · https://github.com/Megafunk/MassSample
- Scene Graph: https://dev.epicgames.com/documentation/en-us/fortnite/scene-graph-in-unreal-editor-for-fortnite
- GAS: https://dev.epicgames.com/documentation/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system · https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/GameplayAbilities/FPredictionKey · https://github.com/tranek/GASDocumentation
- Objects, GC, reflection: https://dev.epicgames.com/documentation/unreal-engine/objects-in-unreal-engine · https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine · https://www.unrealengine.com/blog/unreal-property-system-reflection
- Nanite†: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- Lumen: https://www.unrealengine.com/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen
- VSM: https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine · https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4
- Animation: https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine
- Niagara (third-party overview): https://www.strayspark.studio/blog/niagara-vfx-advanced-simulation-stages
- Blueprints: https://forums.unrealengine.com/t/why-was-blueprint-nativization-removed-no-code-preaching/232490 · https://intaxwashere.github.io/blueprint-performance/
- Multi-User: https://dev.epicgames.com/documentation/unreal-engine/multi-user-editing-overview-for-unreal-engine
- Assets, DDC, Zen, cooking: https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine · https://dev.epicgames.com/documentation/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine · https://dev.epicgames.com/documentation/unreal-engine/using-derived-data-cache-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/using-zen-storage-server-as-cooked-output-store-for-unreal-engine · https://dev.epicgames.com/documentation/unreal-engine/zen-loader-in-unreal-engine
- UGS/Horde: https://dev.epicgames.com/documentation/unreal-engine/using-precompiled-binaries-in-unreal-game-sync-for-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/horde-unrealgamesync-tutorial-for-unreal-engine

**Godot** (read from godot-docs / godot-website sources on GitHub)
- https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/godot_architecture_diagram.rst
- https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/internal_rendering_architecture.rst
- https://github.com/godotengine/godot-docs/blob/master/tutorials/rendering/renderers.rst
- https://github.com/godotengine/godot-docs/blob/master/tutorials/performance/using_servers.rst
- https://github.com/godotengine/godot-docs/blob/master/getting_started/introduction/godot_design_philosophy.rst
- https://github.com/godotengine/godot-docs/blob/master/engine_details/file_formats/tscn.rst
- https://github.com/godotengine/godot-docs/blob/master/engine_details/architecture/object_class.rst
- https://github.com/godotengine/godot-docs/blob/master/tutorials/physics/large_world_coordinates.rst
- https://github.com/godotengine/godot-docs/blob/master/tutorials/physics/using_jolt_physics.rst
- https://godotengine.org/article/why-isnt-godot-ecs-based-game-engine/ (source: https://github.com/godotengine/godot-website/blob/master/collections/_article/why-isnt-godot-ecs-based-game-engine.md)
- https://godotengine.org/article/godot-4-will-discontinue-visual-scripting/ (source: https://github.com/godotengine/godot-website/blob/master/collections/_article/godot-4-will-discontinue-visual-scripting.md)
- https://github.com/godotengine/godot-cpp/issues/1589 · https://gamefromscratch.com/godot-4-4-gets-native-jolt-physics-support/

**Unity DOTS** (package docs via the needle-mirror GitHub copies)
- https://github.com/needle-mirror/com.unity.entities/blob/master/Documentation~/concepts-archetypes.md · …/concepts-structural-changes.md · …/baking-overview.md
- https://github.com/needle-mirror/com.unity.burst/blob/master/Documentation~/index.md
- https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/ghost-snapshots.md · …/prediction-n4e.md · …/optimization/optimize-ghosts.md · …/client-server-worlds.md · …/testing/thin-clients.md

**O3DE** (docs source: https://github.com/o3de/o3de.org, rendered at docs.o3de.org)
- …/content/docs/welcome-guide/key-concepts.md · …/atom-guide/dev-guide/rhi/rhi.md · …/atom-guide/dev-guide/passes/pass-system.md · …/user-guide/programming/components/reflection/_index.md · …/user-guide/programming/serialization/_index.md · …/user-guide/assets/asset-processor/_index.md · …/learning-guide/tutorials/entities-and-prefabs/entity-and-prefab-basics.md · …/user-guide/programming/messaging/ebus.md · …/user-guide/networking/multiplayer/_index.md

**Bevy**
- https://github.com/bevyengine/bevy/blob/main/crates/bevy_ecs/README.md · https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/lib.rs · https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/pipelined_rendering.rs · https://github.com/bevyengine/bevy/blob/main/crates/bevy_render/src/renderer/mod.rs · https://bevy.org/news/bevy-0-16/ (source: https://github.com/bevyengine/bevy-website/blob/main/content/news/2025-04-24-bevy-0.16/index.md)

**Frostbite, id Tech, GPU-driven, jobs, physics**
- † O'Donnell, *FrameGraph: Extensible Rendering Architecture in Frostbite*, GDC 2017: https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in · reference implementation: https://github.com/skaarj1989/FrameGraph
- † Haar & Aaltonen, *GPU-Driven Rendering Pipelines*, SIGGRAPH 2015: https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- † Wihlidal, *Optimizing the Graphics Pipeline with Compute*, GDC 2016: https://www.gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With
- † Sousa & Geffroy, *The Devil is in the Details: idTech 666*, SIGGRAPH 2016: https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pdf
- † Geffroy, Gneiting & Wang, *Rendering the Hellscape of DOOM Eternal*, SIGGRAPH 2020: https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf
- † Gyrling, *Parallelizing the Naughty Dog Engine Using Fibers*, GDC 2015: https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine
- Marl: https://github.com/google/marl
- Jolt Physics architecture (big worlds, space sims, determinism, job system): https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md · https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Core/JobSystem.h

† = primary source could not be fetched from this environment (network egress blocked). The content is summarized from well-established published material and search excerpts; verify exact figures before depending on them.
