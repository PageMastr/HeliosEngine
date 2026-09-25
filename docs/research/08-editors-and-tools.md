# 08 — Game Editors & Content-Creation Tooling for a Sci-Fi MMO

**Status:** Research report (draft 1) · **Date:** 2026-09-25 · **Scope:** Editor + content tool suite for our C++20/Vulkan engine, ImGui editor, LuaJIT scripting, and Go/PostgreSQL/Redis/NATS backend.

---

## 1. Overview

To ship an MMO you have to author and operate a large amount of content: worlds, planets, star systems, thousands of items, NPCs and quests, and the ongoing live-ops that follow. The tools decide both whether that is possible and what it costs. Our reference games show this clearly. The open-sourced Star Wars Galaxies (SWG) client-tools tree has **30+ standalone editors**, including TerrainEditor, TemplateEditor, QuestEditor, NpcEditor, ParticleEditor, AnimationEditor, ShipComponentEditor, SoundEditor, UiBuilder, ShaderBuilder, SwooshEditor, LightningEditor and a WorldSnapshotViewer. It also has a separate **God Client** for editing live game worlds and a **CS tool** for customer service ([SWG client-tools engine apps](https://github.com/SWG-Source/client-tools/tree/master/src/engine/client/application), [game apps](https://github.com/SWG-Source/client-tools/tree/master/src/game/client/application)). HeroEngine, which BioWare licensed for SWTOR, was built on the idea that *developers work in the world together, live, like players do* ([HeroBlade](http://hewiki.heroengine.com/wiki/HeroBlade), [Triad HeroBlade](http://www.triadmain.com/heroengine/about-heroengine/heroblade)). EVE Online moved its static game data from a database into version-controlled YAML files (FSD) so it could be branched, merged and built in parallel ([FSD](https://medium.com/@offbyone/file-static-data-e2af8f8e8c0a), [Fuzzwork SDE](https://www.fuzzwork.co.uk/2021/07/17/understanding-the-eve-online-sde-1/)).

**Core thesis.** The editor is not one app. It is a *platform* made of:

1. a **reflection- and schema-driven object model**, the single source of truth that drives C++, Go and Lua code, inspectors, validation and cooking;
2. a **transaction system**, which provides undo/redo, multi-user sync, crash journaling and GM hotfix audit trails;
3. **mergeable, text-based source formats** with one file per object;
4. an **asset daemon/cook pipeline** that produces separate client, zone-server and backend-service outputs;
5. **Play-In-Editor (PIE)** that runs real zone-server processes and a local backend stack;
6. a set of **domain-specific editors** on top: world, planet, galaxy, data, graphs, animation, VFX, UI, ships, quests.

### Stack flags (where research argues for changes or additions)

| # | Flag | Recommendation |
|---|---|---|
| F1 | Dear ImGui is designed for "content creation tools and visualization / debug tools" and does **not** support "full internationalization (right-to-left text, bidirectional text, text shaping etc.) and accessibility" ([Dear ImGui](https://github.com/ocornut/imgui)). | Keep ImGui (use the **docking** branch) for the editor. Use a **separate retained-mode runtime UI** for players. RmlUi is the leading candidate: HTML/CSS, data bindings, localization, hot reload, MIT ([RmlUi](https://github.com/mikke89/RmlUi)). |
| F2 | General-purpose visual scripting fails or turns into spaghetti. Godot removed VisualScript for low adoption and a flawed approach, but kept visual *shaders*, which "are working well" ([Godot blog](https://godotengine.org/article/godot-4-will-discontinue-visual-scripting/)). Hazelight: Blueprints "can easily lead to unmaintainable spaghetti" ([Hazelight Angelscript](https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/master/content/_index.md)). | Do **not** build a general-purpose Blueprint clone first. Build one graph framework that powers *domain-specific* graphs (material, VFX, anim state machine, AI, quest, dialogue, level logic) which **compile to Lua**. Systemic gameplay goes in C++ and text Lua. |
| F3 | Binary asset formats block merging. Unreal Blueprints are binary `.uasset` files and need special diff tools plus exclusive locks ([Diffing Unreal assets](https://www.unrealengine.com/en-US/blog/diffing-unreal-assets), [MergeAssist](https://github.com/KennethBuijssen/MergeAssist)). | **All authored source data is text** (Godot TSCN precedent: "human-readable and easy for version control" — [TSCN](https://github.com/godotengine/godot-docs/blob/master/engine_details/file_formats/tscn.rst)). Binary appears only in cooked output. |
| F4 | Go services (market, crafting, character) need the *same* static data as C++ zone servers. | Add a **schema IDL + code generator** that emits C++ reflection types, Go structs, Lua annotations, editor inspectors and validators from one definition. |
| F5 | Audio authoring is a mature middleware market. | License **Wwise or FMOD** and build integration tooling, not an audio authoring app. Use Steam Audio (Apache-2.0) for spatialization and occlusion ([Steam Audio](https://github.com/ValveSoftware/steam-audio)). |
| F6 | The FBX SDK is a proprietary dependency. | Import with **ufbx** (MIT/public domain, fuzzed) and treat glTF as primary ([ufbx](https://github.com/ufbx/ufbx)). |
| F7 | Animation runtime and compression are solved problems. | Use **ozz-animation** (MIT: sampling, blending, two-bone/aim IK; *no* editor or state machine) and **ACL** (MIT, ships as an Unreal plugin) ([ozz](https://github.com/guillaumeblanc/ozz-animation), [ACL](https://github.com/nfrechette/acl)). |
| F8 | LuaJIT debugging is feasible without custom work. | Expose the **Debug Adapter Protocol**. The pure-Lua local-lua-debugger already supports LuaJIT breakpoints, stepping and conditional breakpoints ([DAP](https://github.com/microsoft/debug-adapter-protocol), [local-lua-debugger](https://github.com/tomblind/local-lua-debugger-vscode)). |

---

## 2. Survey by Tool Category

### 2.1 Level / world editors

- **Unreal Editor.** This is the UX baseline: viewport gizmos, snapping, World Outliner, Details panel, PIE. For large worlds, **World Partition** puts the world on a grid and streams it. **One File Per Actor (OFPA)** "reduces overlap between users by saving data for instances of Actors in external files", and it is on by default with World Partition. **Data Layers** load and unload groups of actors in the editor and at runtime. Renaming a data layer changes only its label, and deleting one touches only the WorldDataLayer file ([World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine), [OFPA](https://dev.epicgames.com/documentation/unreal-engine/one-file-per-actor-in-unreal-engine?lang=en-US), [Data Layers](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---data-layers-in-unreal-engine)).
- **Unreal Multi-User Editing (Concert).** Every change to a level, and every asset save, becomes a transaction forwarded to a server. The server "keeps track of all these change records" and rebroadcasts them. It "only records transactions and modified Assets" and does not need the full project ([Multi-User overview](https://dev.epicgames.com/documentation/unreal-engine/multi-user-editing-overview-for-unreal-engine?lang=en-US), [reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-reference-for-unreal-engine)).
- **HeroEngine HeroBlade.** Live collaboration is the default. "One developer can be creating a house and the entities inside, while another works on the landscaping and terrain around it. Each sees the other's work in real time." Developers can also leave notes directly in the level for others ([HeroBlade](http://hewiki.heroengine.com/wiki/HeroBlade), [Building Areas](http://hewiki.heroengine.com/wiki/Building_Areas_Tutorial), [Incredibuild glossary](https://www.incredibuild.com/glossary/heroengine)). This is the MMO-native model. Its cost is that you need a server-authoritative content store.
- **Guerrilla's Decima tools.** For Horizon Zero Dawn, Guerrilla rebuilt its tools pipeline from scratch around a text object format (CoreText) and in-house editors, including a State Machine Editor ([GDC 2017](https://gdcvault.com/play/1024685/Creating-a-Tools-Pipeline-for), [Guerrilla write-up](https://www.guerrilla-games.com/read/creating-a-tools-pipeline-for-horizon-zero-dawn)). Naughty Dog built its level and shader editors on Sony's Authoring Tools Framework ([ATF GDC](https://www.gdcvault.com/play/1020154/Authoring-Tools-Framework-Open-Source)).
- **O3DE.** A useful open reference implementation. **Layers** are saved as separate `.layer` files so that "different team members [can] work asynchronously", and they are stripped on export ([O3DE layers](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/editor/layers.md)). **Prefabs** nest. Instance overrides "are stored in the prefab being edited that contains the instances", show a visual marker, and can be reverted per property, per component or per entity ([overrides](https://github.com/o3de/o3de.org/blob/main/content/docs/learning-guide/tutorials/entities-and-prefabs/override-a-prefab.md), [prefab basics](https://github.com/o3de/o3de.org/blob/main/content/docs/learning-guide/tutorials/entities-and-prefabs/entity-and-prefab-basics.md)).
- **Godot.** Scenes are prefabs, and inherited scenes act as variants. The text scene format stores only properties that differ from defaults ("properties equal to the default value are not stored") and uses string UIDs so files can move without breaking references ([TSCN](https://github.com/godotengine/godot-docs/blob/master/engine_details/file_formats/tscn.rst)). `@tool` scripts run game code inside the editor, but "modifications in the editor are permanent, with no undo/redo possible" ([running code in editor](https://github.com/godotengine/godot-docs/blob/master/tutorials/plugins/running_code_in_the_editor.rst)). The lesson: editor-side scripts must go through the transaction system.
- **CryEngine Sandbox.** Mature in-editor terrain, vegetation and **Flow Graph** level scripting. **Schematyc** is designed for "more finite control of Objects", with logic driven by state and context, built from programmer-provided building blocks ([Schematyc](https://docs.cryengine.com/display/CEMANUAL/Schematyc), [Flow Graph](https://docs.cryengine.com/display/CEMANUAL/Flow+Graph), [Entity Components](https://docs.cryengine.com/display/CEMANUAL/Entity+Components)).
- **Bethesda Creation Kit.** Everything is a data record ("form") in plugin files. It has a dedicated **navmesh mode** with several auto-generation methods (Recast-based is recommended for interiors) plus hand editing. **Dialogue** is organized into quests and topics, and "the first Info whose conditions are satisfied will be used" ([CK Dialogue](https://ck.uesp.net/wiki/Category:Dialogue), [CK Conversations](https://ck.uesp.net/wiki/Bethesda_Tutorial_Conversations), [CK Navmesh](https://ck.uesp.net/wiki/Bethesda_Tutorial_Navmesh/fr)). Its shared **conditions system**, used by quests, dialogue, AI and spawns, is worth copying.
- **Hammer 2 (Source 2).** Replaced BSP brushes with free-form polygon meshes ("brush restrictions are removed"). It supports vertex, edge and face editing, extrusion and bridging, texture tools, and knowledge that "can also apply to... Modo or Maya" ([Mesh Editing 1](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Basic_Construction/Mesh_Editing_1), [Mesh Editing 3](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Basic_Construction/Mesh_Editing_3)). **TrenchBroom** adds CSG, texture lock, smart entity property editors and an **issue browser with automatic quick fixes** ([TrenchBroom](https://github.com/TrenchBroom/TrenchBroom)). That issue browser is the model for our validation UI.

### 2.2 Planet, terrain and space tools

- **SWG procedural terrain.** "Photoshop layers married with procedural generation". Designers say "run this rule when you're inside this circle, and this other rule when you're outside of it" ([Koster](https://www.raphkoster.com/2015/04/20/swgs-dynamic-world/), [PCG wiki](http://pcg.wikidot.com/pcg-games:star-wars-galaxies), [Shaping of Corellia](https://www.swgemu.com/archive/scrapbookv51/data/20070128120651/)). The source tree confirms the vocabulary:
  - **Boundaries:** Circle, Polygon, Polyline, Rectangle.
  - **Filters:** shared filter base plus fractal groups.
  - **Affectors:** Height, Color, Shader, FloraStatic, FloraDynamic, Environment, River, Road, Ribbon, Passable, Exclude.
  - **Groups:** Fractal, Bitmap, Shader, Flora, Radial, Environment.

  It also includes a TerrainModificationHelper for building footprints ([generator sources](https://github.com/SWG-Source/client-tools/tree/master/src/engine/shared/library/sharedTerrain/src/shared/generator), [TerrainEditor UI](https://github.com/SWG-Source/client-tools/tree/master/src/engine/client/application/TerrainEditor/src/win32)). The generator is seeded and deterministic, so a third-party web viewer reproduces the ground "to the centimetre" from the `.trn` file ([swg3js](https://github.com/erik-t-irgens/swg3js)). **Deterministic layered rules make planet-sized worlds small to store and identical on client and server.**
- **Star Citizen Planet Tech v4/v5 ("Genesis").** Planets are driven by ecosystem data such as temperature, humidity, geology, soil type and depth. That data controls "the assignment and unrepetitively tiled blending of terrain textures, the distribution of each individual type of Flora based on competition rules and override variables, and the placement of rocks and debris derived from erosion simulation" ([Planet Tech v4](https://starcitizen.tools/Planet_Tech_v4), [v5](https://starcitizen.tools/Planet_Tech_v5), [CitizenCon 2019](https://starcitizen.tools/CitizenCon_2019_-_Terra_Firmer)).
- **Guerrilla GPU procedural placement.** Artists define placement rules in a **graph editor** that reads world data (density maps). The GPU then assembles "fully-fledged environments... complete with sounds, effects, wildlife and game-play elements" around the player. Everything stays artist-editable, and only 3 people made the nature assets for the whole game ([GDC Vault](https://gdcvault.com/play/1024700/GPU-Based-Run-Time-Procedural), [Guerrilla](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)).
- **Elite Dangerous Stellar Forge.** Generates about 400 billion systems in a 1:1 Milky Way from first-principles physical simulation, with real star catalogues layered in ([Stellar Forge](https://elite-dangerous.fandom.com/wiki/Stellar_Forge), [Frontier forum](https://forums.frontier.co.uk/threads/myth-busting-on-stellar-forge-and-the-generation-of-everything-from-stars-to-rocks.517029/)). EVE takes the opposite approach: thousands of hand-curated systems kept as static data ([EVE static data](https://developers.eveonline.com/docs/services/static-data/)). **Our galaxy editor must support both: procedural generation from seeds, plus pinned hand-authored overrides.**
- **Offline generators** (World Machine, Gaea, Houdini). These are node-based erosion and terrain tools that export heightmaps and masks. Houdini Engine can embed Houdini Digital Assets (HDAs) in the editor with parameters artists edit live, and it can take engine assets as inputs ([Houdini Engine for Unreal](https://github.com/sideeffects/HoudiniEngineForUnreal)). Our planet tool should *import* their outputs as layer inputs, not compete with them.

### 2.3 Data-driven object design

- **SWG object templates.** Plain-text TPF files use `@base` inheritance ("Anything that backpack_s01.iff doesn't explicitly define is inherited from base_backpack.iff"). There is a **shared** template for client-visible data (name, appearance) and a separate **server** template that references it. Templates compile to IFF, and data tables (`.tab`) are compiled too. All generated templates are registered in a CRC ID table ([SWG adding objects](https://github.com/SWG-Source/swg-main/wiki/Adding-New-Objects-To-The-SWG-Server)). **The shared/server split is essential for an MMO: client builds must never contain loot tables, spawn logic or server formulas.**
- **EVE FSD.** CCP moved from database-backed static data (BSD) to files, largely YAML, with a parallel map-reduce build for about 1.2 GB of data spread over tens of thousands of files ([FSD](https://medium.com/@offbyone/file-static-data-e2af8f8e8c0a), [Fuzzwork](https://www.fuzzwork.co.uk/2021/07/17/understanding-the-eve-online-sde-1/)).
- **Unreal GAS.** This is the reference vocabulary for ability and stat editors:
  - **Attributes** held in AttributeSets.
  - **Gameplay Effects:** Instant, Duration or Infinite; modifiers Add, Multiply, Divide or Override; stacking by source or by target; custom Execution Calculations and Modifier Magnitude Calculations (MMC).
  - **Hierarchical Gameplay Tags.**
  - **Abilities** with costs and cooldowns.
  - **Gameplay Cues** for VFX and SFX.
  - **Client prediction.**

  ([GASDocumentation](https://github.com/tranek/GASDocumentation)). UE DataAssets and DataTables provide typed records and CSV-like tables.
- **Godot Resources.** Custom resource types get "auto-serialization" to text `.tres`, inspector editing out of the box, and sub-resources. The docs argue these beat JSON and CSV because they carry types, validation and methods ([Resources](https://github.com/godotengine/godot-docs/blob/master/tutorials/scripting/resources.rst)).

**Synthesis.** Use a schema IDL, archetype inheritance with sparse overrides, shared/server field partitioning, one text file per record, stable IDs, a spreadsheet view plus inspector view over the same records, and binary cooking for each consumer.

### 2.4 Scripting and visual scripting

- **Blueprints.** Very accessible and tightly integrated with the editor. However, they compile to VM bytecode, are stored as binary assets, and merge poorly (Epic added dedicated text export and a Blueprint diff tool). Large graphs become unmaintainable, which is why Hazelight wrote most of *It Takes Two* and *Split Fiction* gameplay in Angelscript. Angelscript also hot-reloads "non-structural changes... without having to exit the play session" ([Hazelight docs](https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/master/content/_index.md), [angelscript.hazelight.se](https://angelscript.hazelight.se/), [Blueprints doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprints-visual-scripting-in-unreal-engine)). *Note: we could not find a GDC talk titled "Blueprints: The Good, the Bad…"; the points above come from the primary sources cited.*
- **Unity Visual Scripting (formerly Bolt).** The community reports poor performance compared with Blueprints and C# ([Unity discussion](https://discussions.unity.com/t/performance-troubles-unity-visual-scripting-compared-with-unreal-blueprints-and-direct-c/883750)).
- **Godot VisualScript removal.** Reasons given: low adoption, no high-level packaged nodes, documentation never covered it, and "the approach we took was not the right one". Visual *shaders* are kept ([Godot blog](https://godotengine.org/article/godot-4-will-discontinue-visual-scripting/), [module repo](https://github.com/godotengine/godot-visual-script)). **Lesson: graphs succeed when their nodes are high-level and domain-specific.**
- **CryEngine.** Flow Graph for level scripting, Schematyc for object behavior built from programmer-provided blocks.
- **O3DE Script Canvas.** Best practices: event-driven graphs, and programmers should build "custom nodes" for common complicated patterns. Its debugger logs node execution in order and can attach to a running game ([best practices](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/scripting/script-canvas/best-practices.md), [debugging](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/scripting/script-canvas/debugging.md)).
- **Node-editor libraries.**
  - **imgui-node-editor** (MIT): Blueprint-style theme, selection, zoom and pan, groups, copy and paste, JSON state saving. It proved itself in the Spark CE Blueprint editor ([repo](https://github.com/thedmd/imgui-node-editor)).
  - **imnodes** (MIT): lighter weight, with a minimap ([repo](https://github.com/Nelarius/imnodes)).
  - **ImGuizmo** (MIT): transform gizmos, a view cube, a sequencer widget and a graph editor ([repo](https://github.com/CedricGuillemet/ImGuizmo)).

  **Pick imgui-node-editor** for the graph framework.

### 2.5 Narrative

- **Yarn Spinner.** Screenplay-like nodes, lines, options and commands. Line IDs and string tables support localization. It has a VS Code extension and is MIT licensed ([YarnSpinner](https://github.com/YarnSpinnerTool/YarnSpinner)).
- **ink.** Knots, stitches, choices, variables and external functions. It compiles to JSON and has the Inky live-preview editor. MIT licensed ([ink](https://github.com/inkle/ink)).
- **articy:draft.** Flow-based narrative database with templated entities, global variables, a simulation mode and engine exporters.
- **Creation Kit.** Quests own their dialogue, and conditions pick responses.
- **BioWare (SWTOR).** Auto-staged cinematic conversations, including multiplayer group dialogue rolls. This is the MMO benchmark for conversation presentation.
- **Cinematics.** UE Sequencer: level sequences with tracks for camera, transform, animation, audio and events, plus a curve editor and shot/take structure.

**Recommendation.** Build quest and dialogue editors on our graph framework, with text storage and Yarn/ink import. Share one conditions/facts system across both. Every line gets a stable ID for localization and VO.

### 2.6 Animation

- **Runtime.** ozz-animation handles loading, sampling, blending, two-bone and aim IK, with SoA data-oriented layouts. It imports from glTF and FBX, and deliberately ships **no editor or state machine** ([ozz](https://github.com/guillaumeblanc/ozz-animation)). ACL (MIT) prioritizes minimal compression artifacts, then fast decompression, then small memory, and ships an Unreal plugin ([ACL](https://github.com/nfrechette/acl)).
- **Motion matching.** It searches a feature database each frame to pick the best pose. **Learned Motion Matching** (Ubisoft La Forge) replaces the database with decompressor, stepper and projector networks, which decouples memory from the amount of mocap ([Motion-Matching reference implementation](https://github.com/orangeduck/Motion-Matching), [LMM article](https://www.theorangeduck.com/page/learned-motion-matching)). UE5 ships a motion matching (Pose Search) system.
- **Editor needs.** Clip import and compression settings with error visualization, notifies/events, root motion, state machines, blend trees and blend spaces, IK (two-bone, then FABRIK or full-body), retargeting across humanoid and alien skeletons, and facial blend-shape and FACS curves. SWG's standalone AnimationEditor was a core tool ([SWG apps](https://github.com/SWG-Source/client-tools/tree/master/src/engine/client/application)).

### 2.7 Materials, VFX, UI, audio

- **Materials.** Node graphs (UE Material Editor, Godot visual shaders) plus **material instances** so artists can tweak parameters without recompiling.
- **VFX.** Niagara (systems, emitters, module stacks, GPU sims, data interfaces), Unity VFX Graph and PopcornFX set the AAA bar. **Effekseer** is an MIT-licensed node-based particle editor with a Vulkan runtime and could bootstrap our MVP ([Effekseer](https://github.com/effekseer/Effekseer)). SWG kept separate Particle, Swoosh (trails) and Lightning (beams) editors.
- **Player UI.** UMG-style designer with data binding. See flag F1: RmlUi offers MVC data bindings, CSS animation and hot reload.
- **Audio.** Wwise and FMOD use events, parameters (RTPCs), switches and states, soundbanks, and 3D attenuation curves. MetaSounds adds procedural DSP graphs. Steam Audio adds HRTF, occlusion and baked propagation, with plugins for FMOD and Wwise.

### 2.8 Modular ships, vehicles, buildings and characters

- **Space Engineers:** large and small **block grids** defined in data files.
- **Starfield:** free-form module snapping with validation that required modules are present, plus stat budgets.
- **Star Citizen:** hierarchical **item ports** (size- and type-constrained hardpoints) holding components. Ships are item trees.
- **Kerbal:** part tree with attach nodes, surface attach and symmetry.
- **SWG:** had a dedicated **ShipComponentEditor**.

**Takeaway.** Modular assembly is a *data model*: parts, ports, constraints and derived stats. The same validation code must run in the editor, in the player builder on the client, and authoritatively on the server.

Character customization uses morph targets and bone scales, palettes and texture layers, and fits clothing to body variants (SWG CharacterInfoTool and NpcEditor).

---

## 3. Editor Tool Suite Specification

**Priority.** **P0** = needed for the first playable vertical slice. **P1** = needed to produce content at MMO scale for launch. **P2** = AAA differentiator or post-launch.

| ID | Tool | Purpose | Source data format | MVP prio | AAA prio |
|---|---|---|---|---|---|
| T01 | World Editor (shell, viewport, outliner, details, layers) | Place/edit entities in zones and streamed worlds | Per-entity text files + cell index | P0 | P1 |
| T02 | Prefab & Variant system | Reusable nested compositions with overrides | Prefab text + property-path override patches | P0 | P1 |
| T03 | Blockout / Mesh Editing | Hammer-2-style greyboxing of stations and interiors | Editable poly mesh text/binary sidecar → cooked mesh | P1 | P2 |
| T04 | Terrain & Planet Editor | Heightfield sculpt/paint + SWG-style procedural layer stack + planet ecosystems | `.planet` layer/graph text + masks (PNG16/EXR) + sparse sculpt tiles | P0 | P1 |
| T05 | Scatter & Procedural Placement | Foliage, rocks, asteroids, gameplay nodes by rules | Placement-graph text → cooked instance caches | P1 | P2 |
| T06 | Spline Tools | Roads, rails, rivers, cables, lanes | Spline entity (points/tangents) | P1 | P2 |
| T07 | Star System & Galaxy Editor | Galaxy map, systems, orbits, gates, regions | FSD-like YAML per system + galaxy index | P1 | P1 |
| T08 | Data & Archetype Editor (schema-driven) | All object types and tables: items, NPCs, ships, factions | Schema IDL + one text record per object | P0 | P0 |
| T09 | Gameplay Systems Editors | Abilities/effects/stats/formulas, loot, crafting, economy seeds | T08 records + expression language | P0 (abilities/loot) | P1 |
| T10 | Script IDE & Lua Debugger | Author/debug LuaJIT on client and zone server | `.lua` + generated type stubs | P0 | P1 |
| T11 | Visual Graph Framework + Logic Graph | Shared node-graph infra; level logic graphs → Lua | Graph text with stable node GUIDs | P1 | P1 |
| T12 | Quest Editor | Stages, objectives, conditions, rewards, phasing | Graph text → server state machine | P1 | P1 |
| T13 | Dialogue Editor | Branching conversation, barks, VO | Yarn-like text / graph + string tables | P1 | P2 |
| T14 | Cinematic Sequencer | Cutscenes, scripted world events, conversation staging | Sequence text (tracks/keys) | P2 | P1 |
| T15 | Animation Suite | Import/compress, notifies, state machines, blend spaces, IK, retarget, MM | Clip sidecars + anim-graph text | P0 (import/preview) | P1 |
| T16 | Material Graph Editor | Shader graphs + material instances | Graph text → SPIR-V cache | P0 (instances) | P1 |
| T17 | VFX Editor | Particles, trails, beams, mesh FX | Effect text; runtime binary | P1 | P1 |
| T18 | Environment / Post / Decals | PP volumes, atmosphere, fog, decals, probes | Volume components + profile records | P1 | P2 |
| T19 | UI Designer (runtime) | Player HUD/menus with data binding | RML/CSS-like markup + view-model schema | P1 | P1 |
| T20 | Audio Integration Tools | Middleware events, emitters, attenuation, zones | Middleware project + engine components | P1 | P2 |
| T21 | Modular Assembly Editor | Ships/vehicles/stations/housing parts, ports, rules | Assembly text (part tree) + part archetypes | P1 | P0-for-design |
| T22 | Character Customization | Species sliders, palettes, clothing fit | Customization schema records | P2 | P1 |
| T23 | AI, Navigation & Spawn Editor | Navmesh, behavior graphs, spawn/population | Navmesh bake cache; BT graph text; spawner records | P1 | P1 |
| T24 | Asset Browser & Import Pipeline | Browse, import, thumbnail, dependency, redirect | Source files + `.meta` sidecars (GUID, settings) | P0 | P0 |
| T25 | Localization Tool | String tables, plural/gender, VO, pseudo-loc | Key → MessageFormat string tables | P1 | P1 |
| T26 | Profiling & Debug Viz | Tracy, stat overlays, network/content budgets | Traces (.tracy) | P0 | P1 |
| T27 | Live-Ops / GM Client & Admin | Live world inspect/edit, events, hotfix, CS | Audit log, hotfix bundles | P0 (GM cmds) | P1 |
| T28 | Validation & Content QA | Rules, issue browser, quick fixes, CI gates | Rule registry; issue reports | P0 | P1 |
| T29 | Build / Cook / Deploy | Asset daemon, client/server/service cooks, patches | Cook manifests, content hashes | P0 | P1 |
| T30 | Collaboration Service | Presence, locks, live transaction sync | Transaction stream → changelists | P1 | P2 |

### 3.1 Per-tool detail

Data formats are listed in the table above. Sources for each precedent are in §2.

**T01 World Editor.** The hub that hosts every mode tool.
- *Features:* ImGui docking with saved workspaces; ImGuizmo translate/rotate/scale (local/world, pivot edit); grid, angle, surface and vertex snapping; marquee multi-select; align/distribute; camera bookmarks; outliner with search, filters, visibility and lock; reflection-generated details panel with multi-object edit and override markers. **Editor layers** (organization only) are kept separate from **data layers** (runtime-conditional content such as events, phases or faction control).
- *MVP:* single-zone editing, gizmos, snapping, outliner, details, undo, one file per entity.
- *AAA:* streamed partition editing at planet-scale coordinates (double-precision positions, origin rebasing); HLOD; multi-user presence and in-world notes; bulk find/replace; Lua automation.

**T02 Prefabs & Variants.**
- *Features:* nested prefabs, edit-in-context, overrides stored as property-path patches, revert at property/component/entity level, variants inheriting a base, "where used", propagation-conflict detection.
- *MVP:* nesting, overrides, revert.
- *AAA:* procedural prefabs (script or HDA), conflict-resolution UI, impact analysis before saving a heavily used prefab.

**T03 Blockout / Mesh Editing.** Greyboxing of station and ship interiors, where scale matters.
- *Features:* primitives (box, stairs, arch); vertex/edge/face editing; extrude, bevel, bridge, clip; texture lock and UV align; modular-kit snapping; collision generation; glTF/OBJ export for the DCC round trip.
- *MVP:* primitives, extrude, materials, collision.
- *AAA:* full poly modeling, booleans, trim-sheet UV tools.

**T04 Terrain & Planet Editor.**
- *Features:*
  - SWG-style **layer stack**: boundaries (circle, rectangle, polygon, polyline, with feathering); filters (height, slope, fractal, shader, mask); affectors (height, shader, color, static/dynamic flora, environment, river, road, ribbon, passable, exclude).
  - Sculpt and paint brushes, stored as sparse delta tiles.
  - Ecosystem maps (temperature, humidity, geology) that drive materials and flora.
  - Import of World Machine / Gaea / Houdini masks; POI footprint flattening.
  - **One deterministic C++ evaluator shared by client, editor and zone server**; cube-sphere quadtree LOD.
- *MVP:* zone heightfield with sculpt/paint plus the layer stack (4 boundaries; height/shader/flora affectors).
- *AAA:* spherical planets, erosion, ecosystem competition, whole-planet GPU preview, per-planet budget analyzer.

**T05 Scatter & Procedural Placement.**
- *Features:* rule graph over density inputs (height, slope, climate, masks, distance-to-spline); footprints and exclusions; seeded determinism; paint/erase brush; non-visual entities (sounds, resource nodes, lairs); per-platform density; "bake to instances" for hand touch-up; volumetric asteroid and debris fields.
- *MVP:* painting plus rule scatter bound to terrain layers.
- *AAA:* runtime GPU generation around the player (Guerrilla), ecosystem competition.

**T06 Spline Tools.**
- *Features:* spline editing, spline-mesh deformation, terrain carve/flatten, entity placement along splines.
- *MVP:* spline, mesh deform, flatten.
- *AAA:* road networks with junction generation, rail gameplay (trams, lifts), procedural splines.

**T07 Star System & Galaxy Editor.**
- *Features:*
  - Galaxy view (region → constellation → system) with jump-gate and lane graph editing, plus connectivity and security validation.
  - System view with Keplerian on-rails orbits and time scrub; bodies linked to T04 recipes; stations, belts, anomaly spawners.
  - **Seeded generation plus pinned hand overrides.**
  - Export to Go services (market regions, routing) and to zone orchestration (system → server mapping).
- *MVP:* one-system editor plus a galaxy graph of a few dozen systems.
- *AAA:* 10⁵+ procedural systems with curation, live telemetry overlays.

**T08 Data & Archetype Editor.** The backbone of MMO content.
- *Features:*
  - IDL with types, ranges, enums, typed references, units, `server_only`/`client` visibility, localization keys and editor hints.
  - **Sparse-override inheritance.**
  - **Spreadsheet grid** (filter, bulk edit, column formulas, CSV/XLSX round trip) alongside a generated inspector.
  - "Where used", record diff/merge, stable IDs, schema migrations, per-consumer cooking (client, zone, Go).
- *MVP:* IDL → inspector, grid, inheritance, validation, C++/Go/Lua codegen.
- *AAA:* live tuning through the hotfix path (T27), change approval, balance diff reports.

**T09 Gameplay Systems Editors.**
- *Features:* GAS-style attributes, effects, tags, abilities and cues; sandboxed formula language with level-curve plots; **loot tables** (nested weighted tables, conditions, guaranteed drops, Monte-Carlo EV simulator); crafting, recipe and resource editors; vendor and market seeding.
- *MVP:* ability, effect, stat and loot editors with PIE preview.
- *AAA:* combat sandbox (DPS and time-to-kill), economy sim feeding the Go market-service test harness.

**T10 Script IDE & Lua Debugger.**
- *Features:* DAP server inside client and zone-server VMs (breakpoints, stepping, watch, conditional breakpoints); LSP type stubs generated from bindings and schema; **hot reload during PIE**; error console linking to source; Tracy Lua zones.
- *MVP:* VS Code via DAP, hot reload, error linking.
- *AAA:* embedded editor pane, RBAC-gated remote attach to staging shards, record/replay, per-zone script CPU budgets.

**T11 Visual Graph Framework + Logic Graph.**
- *Features:*
  - imgui-node-editor base: typed pins, reroutes, comments and groups, subgraphs, context-sensitive search palette, minimap.
  - **Text serialization with stable node GUIDs** so graphs merge.
  - **Compile to Lua with source maps**, giving node breakpoints and live execution highlighting.
  - Size/complexity lint; programmer-authored custom nodes.
  - The **Logic Graph** is event-driven level scripting (triggers, doors, spawns, objectives), deliberately not general-purpose.
- *MVP:* framework, Logic Graph, compile to Lua, execution highlighting.
- *AAA:* visual 3-way merge, server-side node breakpoints, per-node perf heatmaps.

**T12 Quest Editor.**
- *Features:* stage graph; objectives (kill, collect, goto, interact, escort, scan, craft); a conditions/facts system shared with dialogue, AI and spawns; rewards from T09; world markers placed in T01; **phasing via data layers**; group and raid credit rules; repeatable, daily and public-event types; reachability validator; GM stage commands; funnel analytics hooks. Compiles to a **server-authoritative** state machine, and clients receive only display data.
- *MVP:* branching quests, objectives, rewards, debug commands, validator.
- *AAA:* template-driven dynamic quests, world-event scheduler, completion analytics in-editor.

**T13 Dialogue Editor.**
- *Features:* nodes for line, choice, condition, action, jump and random; shared facts with T12; **stable line IDs** for localization and VO; VO script export and recording status; playthrough simulator with a variable inspector; Yarn, ink and articy JSON import.
- *MVP:* branching, conditions/actions, simulator, line IDs.
- *AAA:* SWTOR-style auto-staged cinematic conversations (camera and gesture templates, lipsync), multiplayer group dialogue.

**T14 Cinematic Sequencer.**
- *Features:* tracks for camera, transform, animation, VFX, audio, event, fade and subtitle; curve editor (ImSequencer as the starting point); shots and takes; playback either client-local or server-triggered (e.g. capital ships warping in).
- *MVP:* camera, animation and event tracks.
- *AAA:* nested sequences, conversation templates, offline movie render, virtual camera / mocap link.

**T15 Animation Suite.**
- *Features:* skeleton sockets and retarget pose; glTF/ufbx import with **ACL settings and error visualization**; root motion; notifies (footsteps, hit windows, VFX, audio); additive clips; ability montages; state machines and blend trees (on T11); 1D/2D blend spaces; two-bone and aim IK (ozz); retargeting across species; cloth and physics preview.
- *MVP:* import, compress, preview, notifies, state machine, blend spaces, two-bone IK, root motion.
- *AAA:* motion matching database editor (with an LMM memory option), full-body IK, FACS/blend-shape facial animation with VO lipsync, crowd LOD.

**T16 Material Graph Editor.**
- *Features:* graph → HLSL/GLSL → SPIR-V; material functions; **parameterized instances**; preview meshes; instruction, sampler and permutation stats; layered materials (terrain, ship paint, wear, decals); platform-limit validation.
- *MVP:* instances plus surface-shader graph.
- *AAA:* runtime-customizable layered materials (player liveries), permutation analytics.

**T17 VFX Editor.**
- *Features:* system/emitter/module stacks; CPU and GPU sprites, ribbons, beams, meshes and lights; curves and gradients; events; depth collision; **scalability LOD and per-effect budgets** for battles with 100+ players.
- *MVP:* evaluate Effekseer integration against a minimal in-house stack.
- *AAA:* simulation stages, data interfaces (skeleton, mesh, terrain), culling by crowd density.

**T18 Environment / Post / Decals.**
- *Features:* PP volumes with priority and blend (exposure, LUT grading, bloom); per-planet atmosphere and sky profiles; fog; projected decals with a paint tool; reflection probes.
- *MVP:* PP volumes, decals, atmosphere.
- *AAA:* per-biome time-of-day and weather, volumetric clouds, nebula volumetrics.

**T19 UI Designer (runtime).**
- *Features:* RmlUi-class markup and stylesheets; **data binding to schema-generated view-models** (inventory, market, chat); hot reload; anchors and themes; pseudo-localization and overflow checks; gamepad navigation; world-space nameplates; a **sandboxed addon API**, since MMO players expect UI mods.
- *MVP:* markup/CSS, hot reload, bindings, preview.
- *AAA:* WYSIWYG designer, addon sandbox, accessibility.

**T20 Audio Integration.**
- *Features:* middleware event browser; emitter components; **attenuation and cone visualizers**; reverb and occlusion zones; Steam Audio geometry tagging; notify → event mapping; bank and streaming budgets per zone; VO banks per language.
- *MVP:* integration, emitters, attenuation visualization.
- *AAA:* dynamic music, ship-interior acoustics.

**T21 Modular Assembly Editor.**
- *Features:*
  - Part archetypes with **ports** (size/type tags, orientation, symmetry).
  - Grid/block mode and free-snap mode.
  - **Rule validation**: required modules; power, heat, mass, thrust and CPU budgets; center of mass vs. center of thrust; clearance; docking size.
  - Live stat preview; interior volume detection (atmosphere, gravity); damage sections; fleet mesh merge/HLOD.
  - **One validation library for the editor, the client-side player builder and the authoritative server.**
- *MVP:* parts, ports, snapping, validation, stats.
- *AAA:* player builder with shareable blueprints, per-module destruction, auto-generated interior navmesh.

**T22 Character Customization.**
- *Features:* species slider schemas (morphs, bone scales), palettes and texture layers, clothing layering and fit morphs, presets, runtime bake to one mesh and atlas for crowds.
- *MVP:* one species.
- *AAA:* many species, clothing-fit tooling, facial-rig-safe face sliders.

**T23 AI, Navigation & Spawn Editor.**
- *Features:* **Recast/Detour tiled navmesh** (streaming, tile cache, crowds; zlib) with manual edit mode and off-mesh links ([Recast](https://github.com/recastnavigation/recastnavigation)); 3D flight volumes; behavior tree and utility graphs; **spawners** (regions, population tables, respawn, lairs); patrol splines; server debug draw streamed into the editor.
- *MVP:* navmesh bake, BT editor, spawn regions.
- *AAA:* 3D space navigation, population director driven by live metrics.

**T24 Asset Browser & Import.**
- *Features:* thumbnails, tags, collections, drag into viewport; **dependency graph and "where used"**; GUID-stable move/rename; glTF 2.0 primary, FBX via ufbx, textures → KTX2 (BC/ASTC/Basis) ([KTX](https://github.com/KhronosGroup/KTX-Software)), LOD and meshlets via meshoptimizer ([meshoptimizer](https://github.com/zeux/meshoptimizer)); settings in `.meta` sidecars; automatic reimport.
- *MVP:* glTF, FBX, PNG, EXR; thumbnails; GUIDs.
- *AAA:* Blender/Maya live links, Houdini HDAs, USD interchange, provenance metadata.

**T25 Localization.**
- *Features:* keyed tables with **Unicode MessageFormat 2** semantics (plurals, gender, selectors; now stable in CLDR — [MF2](https://github.com/unicode-org/message-format-wg)); translator context and screenshots; length limits; pseudo-localization; XLIFF/CSV exchange; VO tracking; glyph coverage checks.
- *MVP:* tables, pseudo-localization, export.
- *AAA:* TMS integration, in-game review, string hotfix.

**T26 Profiling & Debug Viz.**
- *Features:* **Tracy** in editor, client and zone servers (nanosecond resolution, remote capture, Vulkan and Lua zones, memory, locks — [Tracy](https://github.com/wolfpld/tracy)); stat overlays; memory by asset type; network profiler (bytes per entity type); content budget views.
- *MVP:* Tracy plus overlays.
- *AAA:* unified Insights-style timeline, automated captures from bot soak tests.

**T27 Live-Ops / GM Client & Admin.** MMOs are operated, not just shipped: SWG had a God Client and a CS tool.
- *Features:* GM mode connected to a live shard with **RBAC and a full audit log** (inspect and modify entities, spawn, teleport, observe); player lookup and restores; event scheduler that toggles data layers; **hotfix pipeline** for data and Lua (versioned, staged, canaried, rollback); bug reporter capturing position and state; Go web admin for accounts, economy and chat moderation.
- *MVP:* GM console, audit log, basic web admin.
- *AAA:* staged live-world editing with approval, A/B configs, telemetry heatmaps in the viewport.

**T28 Validation & Content QA.**
- *Features:* per-type rules run on save, pre-commit and in CI; **issue browser with quick fixes**; budgets (triangles, texture memory, draw calls per cell, script CPU); reference integrity; missing localization keys; quest reachability; navmesh coverage; **server-data leakage checks on client cooks**.
- *MVP:* framework, about 20 rules, CI gate.
- *AAA:* auto-fix bots, content-health dashboards.

**T29 Build / Cook / Deploy.**
- *Features:* an **asset daemon** modeled on O3DE's Asset Processor (watches sources, builds per-platform products, tracks dependencies, sends hot-reload notifications, serves assets remotely — [O3DE AP](https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/assets/asset-processor/_index.md)); content-hash shared cache; **separate client, zone and Go-service cooks**; launcher delta-patch chunking; versioned manifests.
- *MVP:* local daemon, CI cook, client/server split.
- *AAA:* distributed cooking, patch-size optimization, streaming install.

**T30 Collaboration Service.**
- *Features:* transaction rebroadcast (Concert model); presence (cursors, selections); **soft locks** per entity plus hard locks for binaries (Git LFS verifies "theirs" locks before push — [LFS locking](https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md)); session → changelist persistence; in-world notes.
- *MVP:* locks and presence.
- *AAA:* HeroEngine-style live co-editing, including on running playtest shards.

---

## 4. Cross-Cutting Editor Infrastructure

1. **Reflection and schema core.** One IDL generates C++ reflected types (for inspectors, serialization and the network replication description), Go structs, Lua type stubs, validators and default editors. Custom property drawers can be registered per type or attribute. This is what lets 30 tools be built by a small team.
2. **Object model and serialization.**
   - Stable 128-bit GUIDs.
   - Text source formats with one file per entity or record (OFPA, O3DE layers, EVE FSD).
   - Archetype/prefab inheritance stored as **sparse property-path patches** (Godot stores only non-defaults; O3DE stores overrides in the parent).
   - Deterministic key ordering so diffs stay minimal.
   - Cooked output is binary and version-tagged.
3. **Transactions and undo.** Every mutation is a command that produces a property-level diff. Godot's model is the baseline: per-document histories plus a global one, and merge modes for continuous drags ([EditorUndoRedoManager](https://github.com/godotengine/godot-docs/blob/master/classes/class_editorundoredomanager.rst)). The same diff stream feeds (a) undo/redo, (b) the multi-user broadcast (T30), (c) a crash-recovery journal, and (d) GM/hotfix audit logs. Editor-side scripts must write through transactions, because Godot's `@tool` scripts cannot be undone.
4. **Asset database and dependency graph.** Source → product mapping, reverse dependencies, thumbnail cache, redirects on move, and a license/provenance field.
5. **Editor ↔ runtime architecture.** The editor links the client engine in-process for the viewport, rendering the same way the game does. **Authority always lives in a separate zone-server process**, even in PIE. This keeps client/server separation honest and matches SWG's split between shared and server data.
6. **PIE (Play-In-Editor).**
   - One click starts 1–N zone servers and a local backend: Go services, PostgreSQL, Redis and NATS via a compose profile or in-memory fakes.
   - It then spawns N client viewports (in the editor or as separate windows).
   - Latency and packet-loss simulation; "possess" and "eject" camera; attach Lua and C++ debuggers.
   - Hot reload of Lua, data records and assets **into the running session**.
   - Also offer "simulate" (no player) and "join staging shard" modes.
7. **Collaboration tiers.**
   - Tier A (P0): asynchronous work through per-object files, locks and mergeable text.
   - Tier B (P1): live session with presence and soft locks.
   - Tier C (P2): live-world editing against running shards with staged publishing (HeroEngine / SWG God Client).
8. **Source control.** An abstraction layer (status, checkout/lock, submit, history, diff) supporting **Git + LFS** (text data and code; LFS locks for binaries) and **Perforce** (large art depots, exclusive checkout), with visual diff for records and graphs.
9. **Extensibility.** A C++ plugin API for editor modes, panels, importers, validators and graph node libraries. **Lua editor-automation** (batch operations, custom tools) runs under transactions. Python bridges for DCC pipelines are optional.
10. **Quality gates.** Validation (T28) and cook (T29) run in CI on every commit. Nightly bot soak tests produce Tracy captures and budget reports.

---

## 5. Requirements (prioritized)

### P0 — required for vertical slice

- **ED-P0-01** A schema IDL generates C++ reflection, Go structs, Lua stubs, inspectors and validators. It supports `server_only` field partitioning, and client cooks exclude server-only data. A CI check verifies this.
- **ED-P0-02** All authored source content is text, one file per entity or record, with GUID references and deterministic serialization. Binary exists only in cooked outputs and imported DCC sources.
- **ED-P0-03** A transaction and command system with property-level diffs, per-document and global undo histories, and a crash-recovery journal. Every tool and every editor script writes through it.
- **ED-P0-04** World Editor MVP: docked ImGui shell, ImGuizmo gizmos, grid/angle/surface snapping, multi-select, outliner, reflection-driven details panel with multi-edit, editor layers.
- **ED-P0-05** Nested prefabs with overrides stored as sparse patches, override markers and revert.
- **ED-P0-06** Data & Archetype Editor with inheritance, grid (spreadsheet) view, CSV round trip, reference pickers and "where used".
- **ED-P0-07** Terrain MVP: sculpt/paint plus SWG-style layer stack (4 boundary types; height, shader and flora affectors) evaluated by one **deterministic C++ library shared by client, editor and zone server**.
- **ED-P0-08** Asset import (glTF, FBX via ufbx, textures → KTX2) with `.meta` sidecars, thumbnails, and an asset daemon providing dependency tracking and hot reload.
- **ED-P0-09** PIE launches real zone-server process(es) plus the local backend stack and N clients, with network condition simulation.
- **ED-P0-10** Lua: DAP debugging (breakpoints, stepping, watch) in client and server VMs, and hot reload during PIE.
- **ED-P0-11** Animation MVP: import, ACL compression, ozz runtime preview, notifies, root motion.
- **ED-P0-12** Material instances (parameter editing without recompiling) and a basic surface-shader graph.
- **ED-P0-13** Ability/effect/stat and loot table editors (GAS-style model) with a loot EV simulator.
- **ED-P0-14** Validation framework with an issue browser and quick fixes, run on save and in CI.
- **ED-P0-15** Tracy integrated in editor, client and zone server; GM command console with an audit log.
- **ED-P0-16** Separate cooks for client, zone server and Go services, with versioned manifests.

### P1 — required for launch-scale production

- **ED-P1-01** Visual Graph Framework (imgui-node-editor) with text/GUID serialization, compile-to-Lua with source maps, node breakpoints, execution highlighting and complexity lint.
- **ED-P1-02** Quest editor (server-authoritative state machines, shared conditions/facts, phasing via data layers, validator, GM stage commands).
- **ED-P1-03** Dialogue editor with stable line IDs, VO tracking, simulator, and Yarn/ink import.
- **ED-P1-04** Star System & Galaxy editor with seeded generation plus pinned overrides, jump-graph validation, and export to Go services.
- **ED-P1-05** Modular Assembly editor with ports, rules and stat preview. The validation library is shared with the client builder and the authoritative server.
- **ED-P1-06** World partition and data-layer editing for streamed worlds; planet-scale coordinates.
- **ED-P1-07** Procedural scatter and placement graphs, plus spline tools with terrain carving.
- **ED-P1-08** Animation state machines, blend spaces, two-bone/aim IK and retargeting.
- **ED-P1-09** VFX editor (or Effekseer integration) with per-effect budgets and scalability.
- **ED-P1-10** Runtime UI framework (not ImGui) with data binding, hot reload, localization preview.
- **ED-P1-11** Audio middleware integration with attenuation visualizers and bank budgets.
- **ED-P1-12** Recast tiled navmesh, BT editor and spawner/population editor.
- **ED-P1-13** Localization tool (MessageFormat 2, pseudo-localization, XLIFF).
- **ED-P1-14** Collaboration service: presence, soft/hard locks integrated with Git LFS or Perforce.
- **ED-P1-15** Live-ops: hotfix pipeline for data and Lua (staged, canaried, rollback), event scheduler, web admin.

### P2 — AAA differentiators / post-launch

- **ED-P2-01** HeroEngine-style live co-editing, including editing on running staging shards.
- **ED-P2-02** Spherical planets with ecosystems and erosion, plus runtime GPU placement.
- **ED-P2-03** Motion matching (with an LMM memory option), full-body IK and facial animation.
- **ED-P2-04** Cinematic sequencer with auto-staged conversations (SWTOR-class).
- **ED-P2-05** Hammer-2-class mesh editing and modular-kit tools.
- **ED-P2-06** Character customization suite across species.
- **ED-P2-07** Houdini Engine HDA support, DCC live links, USD interchange.
- **ED-P2-08** Player-facing ship/base builder and UI addon sandbox built from the editor tools.
- **ED-P2-09** Distributed cooking and patch-size optimization.
- **ED-P2-10** Telemetry overlays in the editor (heatmaps, economy, population).

---

## 6. Sources

**Research limitations.** The web-search budget ran out partway through, and many vendor sites were blocked by the egress proxy (dev.epicgames.com, godotengine.org, gdcvault.com, guerrilla-games.com, cryengine.com, valvesoftware.com, articy.com, and others). Claims about those sources are taken from search-result excerpts gathered this session. Primary sources on GitHub were fetched and read directly. Statements about UE Sequencer, Niagara, UMG and MetaSounds, Wwise/FMOD, Starfield, SWTOR and Kerbal come from general domain knowledge and were not re-verified this session.

- SWG client tools (app list): https://github.com/SWG-Source/client-tools/tree/master/src/engine/client/application · https://github.com/SWG-Source/client-tools/tree/master/src/game/client/application
- SWG terrain generator sources: https://github.com/SWG-Source/client-tools/tree/master/src/engine/shared/library/sharedTerrain/src/shared/generator · TerrainEditor UI: https://github.com/SWG-Source/client-tools/tree/master/src/engine/client/application/TerrainEditor/src/win32
- SWG object templates: https://github.com/SWG-Source/swg-main/wiki/Adding-New-Objects-To-The-SWG-Server
- SWG terrain design: https://www.raphkoster.com/2015/04/20/swgs-dynamic-world/ · http://pcg.wikidot.com/pcg-games:star-wars-galaxies · https://www.swgemu.com/archive/scrapbookv51/data/20070128120651/ · https://github.com/erik-t-irgens/swg3js
- HeroEngine: http://hewiki.heroengine.com/wiki/HeroBlade · http://hewiki.heroengine.com/wiki/Building_Areas_Tutorial · http://www.triadmain.com/heroengine/about-heroengine/heroblade · https://www.incredibuild.com/glossary/heroengine
- Unreal: https://dev.epicgames.com/documentation/unreal-engine/multi-user-editing-overview-for-unreal-engine?lang=en-US · https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-reference-for-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine · https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---data-layers-in-unreal-engine · https://dev.epicgames.com/documentation/unreal-engine/one-file-per-actor-in-unreal-engine?lang=en-US · https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprints-visual-scripting-in-unreal-engine · https://www.unrealengine.com/en-US/blog/diffing-unreal-assets · https://github.com/KennethBuijssen/MergeAssist
- GAS: https://github.com/tranek/GASDocumentation
- Hazelight Angelscript: https://angelscript.hazelight.se/ · https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/blob/master/content/_index.md
- Godot: https://godotengine.org/article/godot-4-will-discontinue-visual-scripting/ · https://github.com/godotengine/godot-visual-script · https://github.com/godotengine/godot-docs/blob/master/engine_details/file_formats/tscn.rst · https://github.com/godotengine/godot-docs/blob/master/tutorials/scripting/resources.rst · https://github.com/godotengine/godot-docs/blob/master/tutorials/plugins/running_code_in_the_editor.rst · https://github.com/godotengine/godot-docs/blob/master/classes/class_editorundoredomanager.rst
- Unity VS performance: https://discussions.unity.com/t/performance-troubles-unity-visual-scripting-compared-with-unreal-blueprints-and-direct-c/883750
- O3DE: https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/assets/asset-processor/_index.md · https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/editor/layers.md · https://github.com/o3de/o3de.org/blob/main/content/docs/learning-guide/tutorials/entities-and-prefabs/override-a-prefab.md · https://github.com/o3de/o3de.org/blob/main/content/docs/learning-guide/tutorials/entities-and-prefabs/entity-and-prefab-basics.md · https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/scripting/script-canvas/debugging.md · https://github.com/o3de/o3de.org/blob/main/content/docs/user-guide/scripting/script-canvas/best-practices.md
- CryEngine: https://docs.cryengine.com/display/CEMANUAL/Schematyc · https://docs.cryengine.com/display/CEMANUAL/Flow+Graph · https://docs.cryengine.com/display/CEMANUAL/Entity+Components
- Creation Kit: https://ck.uesp.net/wiki/Category:Dialogue · https://ck.uesp.net/wiki/Bethesda_Tutorial_Conversations · https://ck.uesp.net/wiki/Bethesda_Tutorial_Navmesh/fr
- Hammer 2: https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Basic_Construction/Mesh_Editing_1 · https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Basic_Construction/Mesh_Editing_3
- TrenchBroom: https://github.com/TrenchBroom/TrenchBroom
- Guerrilla: https://gdcvault.com/play/1024700/GPU-Based-Run-Time-Procedural · https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn · https://gdcvault.com/play/1024685/Creating-a-Tools-Pipeline-for · https://www.guerrilla-games.com/read/creating-a-tools-pipeline-for-horizon-zero-dawn
- Naughty Dog / Sony ATF: https://www.gdcvault.com/play/1020154/Authoring-Tools-Framework-Open-Source
- EVE static data: https://medium.com/@offbyone/file-static-data-e2af8f8e8c0a · https://www.fuzzwork.co.uk/2021/07/17/understanding-the-eve-online-sde-1/ · https://developers.eveonline.com/docs/services/static-data/
- Star Citizen: https://starcitizen.tools/Planet_Tech_v4 · https://starcitizen.tools/Planet_Tech_v5 · https://starcitizen.tools/CitizenCon_2019_-_Terra_Firmer
- Elite Dangerous: https://elite-dangerous.fandom.com/wiki/Stellar_Forge · https://forums.frontier.co.uk/threads/myth-busting-on-stellar-forge-and-the-generation-of-everything-from-stars-to-rocks.517029/
- Houdini Engine: https://github.com/sideeffects/HoudiniEngineForUnreal
- Node editors / gizmos / ImGui: https://github.com/thedmd/imgui-node-editor · https://github.com/Nelarius/imnodes · https://github.com/CedricGuillemet/ImGuizmo · https://github.com/ocornut/imgui
- Narrative: https://github.com/YarnSpinnerTool/YarnSpinner · https://github.com/inkle/ink
- Animation: https://github.com/nfrechette/acl · https://github.com/guillaumeblanc/ozz-animation · https://github.com/orangeduck/Motion-Matching · https://www.theorangeduck.com/page/learned-motion-matching
- VFX / UI / audio: https://github.com/effekseer/Effekseer · https://github.com/mikke89/RmlUi · https://github.com/ValveSoftware/steam-audio
- Pipeline: https://github.com/ufbx/ufbx · https://github.com/zeux/meshoptimizer · https://github.com/KhronosGroup/KTX-Software · https://github.com/recastnavigation/recastnavigation · https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md · https://github.com/wolfpld/tracy · https://github.com/microsoft/debug-adapter-protocol · https://github.com/tomblind/local-lua-debugger-vscode · https://github.com/unicode-org/message-format-wg
