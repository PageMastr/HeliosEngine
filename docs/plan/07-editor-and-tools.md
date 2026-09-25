# 07 — Editor and Tool Suite

> **Status:** draft v1. **Conforms to:** ADR-001, -002, -004, -006, -009, -011, -012, -013, -014.
> **Satisfies capabilities (01 §2.6):** G02, G11, G15, G16 (tooling side) and the tool dependencies of
> all five reference games. **Citations:** `R08-T04` = research 08 tool T04; `R08-ED-P0-07` = its requirement
> list; `R03-P0-2` = research 03 §8; `R04-P0-5`, `R06-ENG-14`, `R10 §4` as in 01 §7.
> **Terminology (ADR-004):** designer content types are **record templates**, never "archetypes", so
> R08's "Data & Archetype Editor" is renamed.

## 0. Principles

1. **Platform first, tools second** (R08 §1). Thirty tools are affordable only because they share
   one schema, one transaction system, one graph framework, one viewport and one validation engine.
   A bespoke tool is built only when the generic record inspector is measurably too slow.
2. **UI-less core.** Every tool action is a ToolsFramework command that also runs headless, in CI
   and from Luau (R06 §6.10).
3. **Every mutation is a transaction** (R08-ED-P0-03). Undo, crash recovery, collaboration, GM audit
   and git export consume one property-path diff stream. Editor scripts get no side door (Godot
   `@tool` lesson, R08 §2.1).
4. **Text sources, binary products** (ADR-006, R08 F3): canonical JSONC, one entity or record per
   file, stable GUIDs.
5. **Same code as the game.** The viewport runs the real renderer. Planet evaluator, assembly and
   fitting validators, HXL, ASM and the loot roller are the runtime libraries. Authority lives in a
   real `helios-cell` process even in PIE (R08 §4.5).
6. **Iteration is measured** (AAA-ITR-*). Destiny had collaborative editing and still needed
   overnight loads for a half-second change (R05 §6).
7. **Windows-first ergonomics, Linux parity** (ADR-001): Explorer, VS/VS Code and DCC conventions on
   Windows; every tool builds and runs on Ubuntu 24.04 (AAA-PLT-3).

---

## 1. Editor architecture

### 1.1 Processes and modules

```
 helios-editor (EDITOR_ONLY)                            separate processes
 ┌────────────────────────────────────────────┐         ┌────────────────────────────────────────┐
 │ EditorUI: ImGui docking + multi-viewport,  │ pipe    │ helios-assetd (T24/T29): import, cook, │
 │ panels, widgets, viewports, themes         │◄───────►│ DDC, thumbnails, hot-reload broadcast  │
 ├────────────────────────────────────────────┤         ├────────────────────────────────────────┤
 │ ToolsFramework (UI-less): documents,       │   HTP   │ helios-cell (+ embedded gateway),      │
 │ selection, commands, transactions, journal,│◄───────►│ helios-client ×N, helios-bot ×M        │
 │ asset DB, graph compilers, validation,     │         ├────────────────────────────────────────┤
 │ source control, automation                 │         │ helios-backend (Go): auth, content,    │
 ├────────────────────────────────────────────┤ NATS +  │ collab (T30), GM, telemetry,           │
 │ Engine runtime (client configuration)      │◄───────►│ web tools (Connect RPC)                │
 └────────────────────────────────────────────┘         └────────────────────────────────────────┘
```

| Module | Contents | Linked by |
|---|---|---|
| `engine/toolsfw` (`helios::tf`) | Documents, selection, command bus, transactions, journal, asset-DB/source-control/collab clients, graph model and compilers, validation engine, Luau automation host | editor, `helios-tool`, `helios-assetd` |
| `engine/editorui` (`helios::edui`) | ImGui shell, docking, themes, widget library, viewport editor passes | editor |
| `engine/edtools/<tool>` | Per tool: headless `core` target + `ui` target | core: editor and `helios-tool`; ui: editor |
| `apps/editor`, `apps/tools/{helios-tool,helios-assetd}` | Executables | |

All are `EDITOR_ONLY` in `helios_module`; CI fails if the client or cell links one (CLAUDE.md
layering). ToolsFramework never includes ImGui or RHI headers.

### 1.2 ToolsFramework

**Documents.** An editable unit with a GUID, source files, a revision and a dirty flag: world (an
OFPA object container, R04-P0-5), record, graph, asset settings (`.meta`), sequence, planet.
External changes (VS Code, `git pull`) arrive via the platform file watcher (ReadDirectoryChangesW /
inotify) as an undoable "external edit" transaction, or a 3-way merge if the document is dirty.

**Selection.** Typed handles (`ObjRef{doc, guid}`) plus sub-selections (vertices, nodes, keys, grid
cells), a history (Alt+←/→) and named sets.

**Command bus.** Every action is registered as `{id: "world.duplicate", argsSchema, canExecute,
execute → Transaction}`. One registry drives menus, toolbars, shortcuts, the command palette, Luau,
the remote-control socket and tests.

**Transactions with property-path diffs.**

```
PropertyPath  ent:7f3a…c2/Transform/position
              rec:itm/kestrel_mk2/baseAttrs[Ship.MaxLinearSpeed]
              rec:loot/hollow_drone/entries[#b21c]/weight      // list elements keyed by GUID, never index
Op            Set{path, before, after} | Create{guid, snapshot} | Destroy{guid, snapshot}
              | Reparent{guid, from, to} | Insert/Remove/Move{list, elemGuid}
              | Blob{path, beforeHash, afterHash}              // terrain tiles, poly meshes (LFS)
Transaction   {id: (user, lamport), label, ops[], baseRev{doc → seq}, mergeKey, author, time}
```

Values use the schemac text codec, so diffs are readable. Continuous gestures coalesce by `mergeKey`
(Godot merge mode, R08 §4.3). Pre-commit hooks enforce schema ranges and reference integrity;
post-commit hooks run T28 rules asynchronously.

**Undo/redo.** Per-document and global histories (Godot `EditorUndoRedoManager`). Undo is a new
inverse transaction, so the journal and collaborators see it. In shared sessions a user undoes only
their own transactions; if a later foreign transaction touched the same path, undo reports a
conflict. History survives save, capped at 512 MB.

**Crash-recovery journal.** Each committed transaction is appended to
`%LOCALAPPDATA%\Helios\journal\<project>\<session>.hjl` (`$XDG_STATE_HOME/helios/journal` on Linux)
with group-commit flush ≤ 50 ms, so a crash loses at most one transaction (AAA-STB-2). After an
unclean exit the editor verifies source hashes and offers replay; sentry-native reports carry the
journal ID (Ph2).

**Automation API.**
- *Luau editor VM*, separate from game VMs and sandboxed (files only via the document API):
  `Editor.transaction(label, fn)`, `Editor.cmd(id, args)`, `Editor.find(query)`, `Record.get/set`,
  `Asset.import`, `Validate.run`. Project scripts in `editor/scripts/*.luau` add menu items,
  buttons and T28 rules, typed by the generated `.d.luau`.
- *Remote control:* JSON-RPC 2.0 over `\\.\pipe\helios-editor-<pid>` (Unix socket on Linux) exposes
  the command registry to DCC plug-ins, test harnesses and `helios://` links.

**Headless batch mode.** `helios-tool <verb>` links ToolsFramework without EditorUI: `validate`,
`fix`, `migrate`, `compile-graphs`, `asm-verify`, `loot-sim`, `bake-nav`, `export-loc`,
`merge-driver`, `fix-redirects`, `ide-setup`, `run <script.luau>`. GPU verbs (thumbnails, HLOD,
probes) take `--offscreen` and run on lavapipe in CI. The same binary backs CI (T29) and the
compile/validate workers behind the web tools (§1.9, R03 §8.1).

### 1.3 EditorUI

- **Shell.** Dear ImGui v1.92.9-docking. A Helios renderer backend on our RHI makes viewports and
  thumbnails bindless textures in the render graph; `imgui_impl_sdl3` provides windows, input and
  IME (ADR-011). Panels tear off into OS windows on any monitor.
- **High-DPI.** PerMonitorV2 manifest (ADR-011); ImGui 1.92 dynamic fonts re-rasterize per monitor
  scale, so a panel moved from a 100% to a 200% monitor stays sharp. Metrics are in DIPs; viewports
  render at physical resolution with an optional render scale.
- **Theming.** Dark (default), light and high-contrast token files. A **context accent** on frame
  and status bar: neutral = local, amber = shared edit instance, red + shard banner = live GM
  connection (T27), so live is never mistaken for local.
- **Role layouts.** Presets of dock layout, panels, toolbars, asset filters and keymap for **level
  designer, systems designer, artist, animator, writer, GM**, plus programmer, technical artist and
  audio. Team presets in git (`editor/layouts/*.json`), personal ones in `%APPDATA%\Helios\layouts`.
- **Windows integration.** Explorer drop onto the asset browser imports into that folder, onto the
  viewport imports and places at the cursor; drag-out via OLE `DoDragDrop` (Ph3, Windows only);
  "Show in Explorer"; "Open in VS Code / Visual Studio" (`code -g file:line`, `devenv /Edit`); SDL3
  native dialogs and taskbar progress; a per-user `helios://` handler (HKCU; `.desktop` on Linux)
  opens an entity, record or position from chat or the bug tracker.
- **Text.** Apache-2.0 fonts (Roboto 2.x, Roboto Mono, Droid Sans Fallback for CJK) and icon font.
  ImGui cannot shape or lay out RTL text (R08 F1), so localization and UI previews render through
  the runtime RmlUi/HarfBuzz stack into a texture.

### 1.4 Shared widget library

| Widget | Basis and features | Users |
|---|---|---|
| **Property grid** | From schemac editor metadata: ranges, units, enums, categories, doc tooltips, `client`/`server_only` badges. Per-type/per-field **customizers** (`@editor(customizer=…)`); multi-edit with mixed values; **override markers** with revert for prefab and record-template inheritance; "copy path", "where used" | all |
| **Typed pickers** | AssetRef, RecordRef (fuzzy search, `TagQuery` filter), TagSet tree, TagQuery builder, LocString with preview, inline HxlExpr with plot | T08, T09, T12, T13, T23 |
| **Node-graph editor** | **imgui-node-editor** (MIT, R08 §2.4; to vendor, §5.5); vendored ImGuizmo `GraphEditor` as fallback. Typed pins, reroutes, comments, groups, subgraphs, search palette, minimap, execution highlight, diff overlay | T04, T09, T11–T13, T15–T17, T23 |
| **Timeline & curves** | Vendored ImSequencer + ImCurveEdit forked into a Helios timeline: tracks, nesting, dopesheet, Bézier tangents, frame snap, scrubbing | T09, T14, T15, T17, T20 |
| **Gizmos** | ImGuizmo transform/bounds + view cube; math in camera-relative f32, deltas accumulated into f64 `WorldPos` | T01–T07, T21 |
| **Colour, gradient, charts** | HDR colour (EV, Kelvin), palettes, ImGradient; ImPlot histograms, CDFs, heatmaps | T09, T16–T18, T22, T26 |
| **Data grid** | Virtualized 100k rows: frozen columns, filter, sort, fill-down, HXL column formulas | T08, T12, T25 |
| **Code pane** | Text editor with LSP and DAP client (Ph3) | T10, T19 |

### 1.5 Viewport

- **Rendering.** The real renderer and render graph (ADR-003) plus editor passes: u64 entity-ID
  buffer with readback for picking, selection outlines, an adaptive grid in the active frame's
  tangent plane, gizmos, debug draw, icons for non-visual entities. Editor passes ≤ 1 ms GPU at
  1440p.
- **View modes.** Lit, unlit, wireframe, lighting only, material channels, overdraw, LOD, texel
  density, collision, navmesh, streaming cells, data layers, phase masks, replication relevancy.
- **Cameras.** Fly (RMB + WASD, wheel = speed), orbit/pan/dolly (Alt + mouse, Maya convention),
  focus (F), orthographic, pilot selected entity, game-camera preview, bookmarks (Ctrl+1…9).
- **Large-world navigation (ADR-005).** The camera holds a reference frame and an f64 frame-local
  position. Fly speed scales logarithmically with distance to the nearest surface, so one gesture
  crosses 10¹¹ m or 10 cm; "Go to" a body interpolates in log space. Planet mode aligns up to local
  gravity and shows latitude/longitude/altitude. The outliner scopes to a frame (system → body →
  grid → interior); in PIE the camera can lock to a moving ship's grid.
- **Snapping and measuring.** Grid, angle, scale; surface (physics ray, else depth); vertex (V);
  socket/port (T21, kits); pivot edit. Ruler, angle and area tools in m/km/AU with f64 precision,
  frame-local and absolute.
- **"View as" debugger (06 §6, R03-P1-3).** Pick a viewer profile (quest stages, facts, standings,
  species, party); the viewport evaluates every `PhaseFilter` with the interest-management evaluator
  and hides or ghosts entities; T13 shows the options that profile sees. Two to four profiles preview
  party phase sharing. The same control drives PIE clients.

### 1.6 Play-in-Editor

| Mode | What runs | Use |
|---|---|---|
| **Play** (F5) | `helios-cell --embedded-gateway` out of process (authority); client 1 in-process in the viewport; clients 2…N as in-process worlds in own windows (cheap) or `helios-client` processes (fidelity, crash isolation); M `helios-bot`s with Luau behaviours (04 §10); dev `helios-backend`, auto-started | Playtest |
| **Simulate** (Alt+F5) | Cell only, editor camera; live entities selectable with read-only live state; "Keep simulation changes" turns chosen live values into a transaction | AI, physics, spawns |
| **Possess/eject** (F8) | Switch between client 1 and the free camera without stopping | Debugging |
| **Join shard** | Client joins a dev/staging shard at its pinned content version | Multi-machine |

- **Content path.** The cell loads cooked base content from the assetd DDC plus an **editor
  overlay**: dirty documents serialized as a delta package over the tools channel. PIE never
  requires saving, and the overlay is the edit-instance transaction format (§1.8).
- **Hot swap.** Placement and record edits apply next tick (AAA-ITR-3); Luau (`__reload(old)`, 06
  §11) and records ≤ 2 s (AAA-ITR-1, Ph1); shaders and assets ≤ 2 s from Ph2. Gameplay C++ modules
  build as DLL/.so in dev; "Rebuild & restart PIE" relinks them in ≤ 30 s on MSVC (AAA-ITR-5) while
  documents stay open.
- **Network emulation.** Per-client NetSim profiles (`lan`, `good`, `mobile`, `awful`, custom; 04
  §10), "drop client", "kill cell" (AAA-SRV-12) and a TiDi override.
- **Debugging.** Every VM announces a DAP port (T10); "Launch cell under debugger" uses
  `devenv /DebugExe` (Windows) or a generated VS Code `launch.json` (Linux); Tracy attaches to all.
- **Start budget.** Cell + backend + 2 clients cold ≤ 15 s (AAA-ITR-3). The cell stays warm and
  reloads its world in ≤ 3 s, so the next PIE starts in ≤ 5 s.

### 1.7 Content versioning and source control

- **Layout.** Git is the source of truth (ADR-006, 05 §1.14). Text (JSONC, graphs, Luau, `.meta`) is
  plain git; binary sources (PNG/EXR/PSD, glTF/FBX/.blend, WAV/FLAC, terrain tiles) are Git LFS
  `lockable`. `.gitattributes` forces `eol=lf`; the canonical JSONC writer (schema key order, one
  property per line) makes equal data produce equal bytes.
- **Provider.** `ISourceControl` (status, lock, commit, history, diff, blame) shells out to the **git
  CLI** (Git for Windows bundles git-lfs). libgit2 is avoided: GPL-2.0-with-linking-exception is not
  on the allow-list (01 §5.2). An optional Perforce adapter over `p4` arrives in Ph4 (R08 §4.8).
- **Changelists.** Perforce-style named changelists over git, committed one at a time; shelves are
  patches in `.helios/shelves`. Editing a lockable binary prompts for an LFS lock; owners show in the
  asset browser and are mirrored to the collab service.
- **Merging.** A `merge=helios` driver (`helios-tool merge-driver`) merges JSONC per property path
  with keyed lists; only same-path conflicts reach the visual record/entity/graph merge UI.
- **Hooks and branches.** Pre-commit runs T28 on changed files (≤ 10 s); CI validates and cooks
  (T29). Branches: `main`, `release/*`, `hotfix/*` (R03-P0-4). Large projects use sparse checkout
  and on-demand LFS fetch.
- **Content versions** are immutable `(git commit, schema hash, record-DB hash, cook-manifest hash)`
  tuples registered with the content service (05 §1.14).

### 1.8 Live collaborative editing (T30 protocol)

Tier A = files and locks (Ph1–2); Tier B = presence and soft locks (Ph2); Tier C = HeroEngine-style
co-editing in an **edit instance** (Ph3; R08 §4.7, R03-P0-2/3).

- **Edit instance.** The orchestrator runs instance #0 of a zone with `kind=edit` in a dev shard.
  It simulates normally (designers can play in it) but persists nothing through checkpoints:
  authored changes live only in the collab journal. Play instances pin published content versions
  and never accept edit transactions.
- **Sequencer.** The Go **collab service** (content domain, 05 §1.14) is the only writer of the
  JetStream stream `COLLAB_<session>`. Editors link nats.c (allowed by ADR-013).

  | Channel | Contents |
  |---|---|
  | `collab.<session>.tx.<docId>` | Accepted transactions (stored) |
  | `collab.<session>.presence` | Core NATS, 5 Hz, not stored: user, frame, camera, selection, tool |
  | KV `LOCKS_<session>` | Lock leases, 90 s TTL, renewed every 30 s |
  | KV `NOTES_<session>` | In-world notes linked to issues (R03-P1-5) |

- **Submit.** The editor applies a transaction optimistically (pending) and sends
  `collab.submit {tx, baseRev}`. The service (1) checks RBAC and area permissions, (2) rejects on a
  foreign hard lock, (3) validates with schemac-generated Go validators, (4) publishes with
  `Nats-Expected-Last-Subject-Sequence = baseRev`. On mismatch it auto-rebases disjoint paths or
  returns `conflict{theirs}`. Accepted transactions fan out to editors and to edit-instance cells,
  which apply them at a tick boundary.
- **Conflict rules.** Disjoint paths commute. Same path: first writer wins; the other user gets
  "take theirs / reapply mine". Delete beats concurrent edits, which stay recoverable for 24 h.
  Keyed list inserts commute. Terrain sculpt deltas are additive and commute; terrain paint and
  scatter need a region lock. Graph edits are node/pin operations keyed by GUID.
- **Locks.** *Soft* per entity, record or graph (advisory; "ask / steal", steals logged); *region*
  (planet polygon or container bounds) for terrain and scatter; *hard* = Git LFS locks on binaries.
- **Late join.** Load the base commit, replay from the newest snapshot (written every 1,000
  transactions): ≤ 10 s for a 50k-transaction session.
- **Budget.** Visible in every editor ≤ 1 s p95 (AAA-ITR-7), typically ≤ 150 ms on a LAN.
- **Publish.** *Submit session* exports each user's transactions as JSONC commits on
  `collab/<session>/<user>` (author kept) and opens a PR; CI validates and cooks; the merged build
  reaches dev play instances in ≤ 5 min (AAA-ITR-7). *Publish preview* registers an overlay content
  version (base build + journal range) that dev play instances pin within seconds. Production
  channels accept only CI builds (R03-P0-3).
- **Live shards are never edited directly.** Staged live-world edits (T27, Ph4) use the same format
  but target an approved hotfix changeset shipped as a hotfix build (AAA-ITR-8).

### 1.9 Web tools for narrative and localization (R03 §8.1)

Writers, localizers and VO coordinators should never need the 3D editor. From Ph3 the collab service
hosts **Writers' Room** and **Loc Review**, on 05's admin-console stack (Go templates + htmx, no Node
toolchain) plus a vendored prebuilt CodeMirror 6 bundle (MIT):
- a **screenplay view** of dialogue that round-trips with the T13 graph;
- **VO status boards**, recording-script export, per-line comments;
- **translation** with context, screenshots, length limits and MessageFormat 2 preview (T25).

Edits are ordinary `collab.submit` transactions; compile and validation run on headless
`helios-tool` workers; graphs render as read-only SVG. Structural graph editing, staging and
cinematics stay native.

---

## 2. The tool suite T01–T30

**Common contract.** Unless a tool says otherwise:
- it writes only through transactions and exposes its actions as commands (AAA-TOOL-6);
- save → assetd → PIE hot reload ≤ 2 s (AAA-ITR-1);
- T28 rules run on save and in CI;
- T30 presence and locks apply;
- every record type is editable in the generic T08 inspector as soon as its schema exists, so
  bespoke tools add speed, not capability.

**MVP/AAA.** A tool is **AAA-complete** when every AAA item whose runtime feature has shipped is
done; items waiting on a later runtime feature (volumetric clouds, AAA-REN-6 Ph4) are tracked with
that feature. "Ph5+" is beyond the AAA bar.

**Layouts** (every tool docks inside the T01 shell; bottom row is shared Asset Browser / Output /
Issues unless listed):

| Tool | Left | Centre | Right | Bottom |
|---|---|---|---|---|
| T01 World | Outliner, Layers | Viewports | Details, Mode options | shared |
| T02 Prefab | Prefab hierarchy | Viewport + breadcrumb | Details (override markers) | shared |
| T03 Blockout | Primitives, Kits | Viewport | Mesh ops, Details | UV view |
| T04 Terrain | Layer stack / Graph | Viewport + 2D map | Layer Details, Brush | Budget, Seeds |
| T05 Scatter | Rule graph | Viewport | Rule Details, Brush | Density preview |
| T06 Splines | Spline list | Viewport | Point Details | Profile |
| T07 System | Frame tree | Log-scale system / galaxy canvas | Body/orbit Details | Time scrub, Reports |
| T08 Data | Tables, Template tree | Grid ⇄ Inspector | Where used, History | Issues |
| T09 Gameplay | Kernel records | Graph / formula / sim view | Details | Sim charts, Timeline |
| T10 Script | Files, Call stack | Code pane / VS Code | Locals, Watch | Console |
| T11 Graphs | Node library | Graph | Node Details | Compile output |
| T12 Quest | Quests, Objectives | Stage graph | Details, View-as | Validator, Funnel |
| T13 Dialogue | Conversations, Roles | Graph ⇄ Screenplay | Line Details, VO | Simulator |
| T14 Sequencer | Bindings | Cinematic viewport | Track Details | Timeline, Curves |
| T15 Animation | Skeleton, Assets | Preview viewport | Details, State graph | Timeline, Notifies |
| T16 Material | Functions | Graph | Preview, Parameters | Stats |
| T17 VFX | Emitter stack | Preview viewport | Module Details | Curves, Budget |
| T18 Environment | Volumes, Profiles | Viewport | Profile Details | |
| T19 UI | Documents, View models | Live RmlUi preview | Bindings, Styles | Checks |
| T20 Audio | Event tree | Viewport (attenuation) | Event Details | Mixer, Meters |
| T21 Assembly | Parts palette, Port tree | Viewport | Stats, Rules | shared |
| T22 Character | Species, Params | Preview viewport | Sliders, Palettes | Stress grid |
| T23 AI/Nav | Spawners, Behaviours | Viewport / BT graph | Details, Blackboard | Live debug |
| T24 Assets | Folders, Collections | Tile/list view | Metadata, Preview | Import log |
| T25 Localization | Tables, Languages | String grid | Context, Preview | Issues |
| T26 Profiling | Processes | Tracy / inspector / overlays | Details | Timeline |
| T27 GM | Players, Shard map | Live viewport | Entity inspector, Commands | Audit log |
| T28 Validation | Rule list | Issues table | Fix preview | |
| T29 Build | Profiles, Jobs | Job graph | Job log | Progress |
| T30 Collab | Participants | (viewport overlays) | Locks, Notes | Session lag |

### 2.1 World building

**T01 World Editor** (R08-T01, ED-P0-04, ED-P1-06) — the hub hosting every mode.
- *Features:* outliner over flecs `ChildOf`/`InFrame` (filters: component, tag, editor/data layer,
  lock owner); Details, gizmos, snapping (§1.5); align/distribute, replace-with, group; **editor
  layers** (organization) separate from **data layers** (runtime-conditional: events, phases,
  faction control); region loading of streamed containers; in-world notes (R03-P1-5); bulk
  find/replace.
- *Data → consumer:* object-container folder = `.hcont` manifest (frame, bounds, data layers) + one
  `.hent` JSONC per entity (OFPA; format owned by 02) → container blobs split client/cell, streamed
  by both. *Integrations:* placement applies live in PIE; per-cell budget rules.
- **MVP (Ph1):** one zone with its frame hierarchy (Tallis → Harrow → Saltmarch), gizmos, snapping,
  outliner, multi-edit, undo, OFPA save, editor layers, bookmarks.
- **AAA (Ph3):** planet-scale region editing, data layers with "view as" preview, HLOD builds, notes
  ↔ issue tracker, per-cell budget heatmap, bulk operations across containers.

**T02 Prefabs & Variants** (R08-T02, ED-P0-05).
- *Features:* nested prefabs as flecs `IsA` hierarchies; edit-in-context; overrides as sparse
  property-path patches; revert per property, component or entity; variants; "where used";
  propagation-conflict detection.
- *Data → consumer:* `.hprefab` JSONC (base + patches) → baked into containers; runtime `IsA`.
- **MVP (Ph1):** nesting, overrides, revert, variants.
- **AAA (Ph3):** conflict-resolution UI, impact analysis before saving a prefab used N times, seeded
  Luau procedural prefabs, apply-overrides-to-base.

**T03 Blockout / Mesh Editing** (R08-T03, ED-P2-05) — greyboxing interiors, where scale is gameplay.
- *Features:* primitives (box, stairs, ramp, arch, cylinder, door kit); vertex/edge/face editing;
  extrude, bevel, bridge, clip, booleans; texture lock, UV align, trim sheets; metric kit snapping;
  collision generation; glTF export for the DCC round trip, keeping the blockout as collision proxy
  when art replaces it.
- *Data → consumer:* `.hmesh` editable-poly sidecar (LFS) → assetd cooks mesh and collision.
- **MVP (Ph2):** primitives, extrude, materials, collision, glTF export.
- **AAA (Ph3):** full poly editing, booleans, trim-sheet tools, kit snapping.

**T04 Terrain & Planet Editor** (R08-T04, ED-P0-07, ED-P2-02; W04, R02; R02-P0-5).
- *Features:*
  - **SWG layer stack generalized to the cube-sphere.** Boundaries: geodesic circles, spherical
    polygons, polylines with width, tangent-frame rectangles, feathered in metres. Filters: height,
    slope, aspect, fractal (3D noise on the sphere: no seams or pole pinching), shader, mask,
    ecosystem. Affectors: height, material, colour, flora (T05), environment (T18), river, road
    (T06), passable, exclude.
  - Per ADR-006's legal note the stored model is a **generic node DAG**; layer stack and graph view
    are two presentations of it.
  - Sculpt/paint as **sparse delta tiles** keyed by (cube face, level, x, y); ecosystem maps
    (temperature, humidity, geology); World Machine/Gaea/Houdini mask import; footprints as runtime
    stamps (06 §9).
  - Live 2D map and 3D preview re-evaluating only dirty tiles; **budget analyzer** (µs per sample,
    layers, flora density, memory).
- *Data → consumer:* `.hplanet` JSONC (DAG, fractal library, seeds) + LFS tiles (EXR half heights,
  PNG16 weights) → `engine/pcg`, one deterministic HEADLESS library (fixed-point `hnoise`, `hmath`,
  strict FP) in client, cell and editor (02 §5.8). The CPU VM is authoritative; the GPU twin generates
  visual LODs only (03 §5.5). *Integrations:*
  cross-compiler hash test on sample points (AAA-PLT-4).
- **MVP (Ph1):** cube-sphere planets up to Harrow's 1,500 km, four boundary types,
  height/material/flora affectors, sculpt/paint tiles, 2D and 3D preview, footprint stamps.
- **AAA (Ph3):** Earth-size planets (6,400 km, AAA-CNT-1), offline erosion filter, ecosystem
  competition, whole-planet GPU preview, budget analyzer, graph view.

**T05 Scatter & Procedural Placement** (R08-T05, ED-P1-07).
- *Features:* rule graph over density inputs (height, slope, climate, masks, distance to spline or
  POI); footprints and exclusions; seeded determinism; paint/erase over rules; non-visual entities
  (sound emitters, resource nodes, lairs); per-quality density; bake-to-instances for touch-up;
  volumetric asteroid and debris fields (the Scree belt).
- *Data → consumer:* `.hscatter` graph → instance caches near authored content; elsewhere runtime
  generation, client for visuals, cell for collidable and gameplay instances only.
- **MVP (Ph2):** painting, rule scatter bound to T04 layers, asteroid fields.
- **AAA (Ph3):** GPU runtime-generation preview around the camera (Guerrilla), ecosystem
  competition, per-platform densities.

**T06 Spline Tools** (R08-T06).
- *Features:* splines (points, tangents, roll, width); spline-mesh deformation; terrain carve/flatten
  (feeds T04 road and river affectors); placement along splines; patrol and rail paths (T23, trams,
  lifts); junction generation.
- *Data → consumer:* spline component in `.hent` → client mesh deform; cell movement and paths.
- **MVP (Ph2):** splines, mesh deform, flatten. **AAA (Ph3):** road networks with junctions, rail
  gameplay, procedural splines.

**T07 Star System & Galaxy Editor** (R08-T07, ED-P1-04; W06).
- *Features:*
  - **System view** on a log-scale canvas (10¹¹ m): Keplerian orbits (a, e, i, Ω, ω, M₀, epoch),
    rotating bodies, spheres of influence, time scrub; bodies linked to T04 recipes; stations, belts,
    anomaly spawners, Lattice relays.
  - **Galaxy view** (region → constellation → system): jump-graph editing, security levels,
    connectivity and chokepoint reports.
  - **Seeded generation with pinned overrides:** hand edits are sparse patches over generated
    output, so regeneration keeps them (R08 §2.2).
  - Zone assignment per body and orbit, with tick profile.
- *Data → consumer:* `.hsystem` JSONC per system + `galaxy.hgalaxy` index (JSONC per ADR-013, not
  R08's YAML) → record DB (orbits, frames), orchestrator (system → zones), Go market regions and
  routing.
- **MVP (Ph1):** the Tallis system (star, Harrow, Harrow High, Scree); a small galaxy graph with Osk
  in Ph2.
- **AAA (Ph3):** 10⁵ seeded systems with curation, validation reports, export to Go services.
  **Ph5+:** telemetry overlays (traffic, kills, prices) on the galaxy map.

### 2.2 Data and gameplay

**T08 Data & Record-Template Editor** (R08-T08, ED-P0-01/06; G02; R04-P0-9) — the backbone.
- *Features:* read-only schema browser (schema changes go through `.hschema` + schemac, HeroEngine's
  DOM discipline, R03 §2.4); record inspector with record-template inheritance tree and override
  markers; **spreadsheet grid** (filter, sort, fill-down, bulk edit, HXL column formulas such as
  `damage = base * 1.1`, CSV/TSV round trip); where used; record diff/merge; per-field visibility
  badges (`client`, `server_only`, service); `.htags` editor with hot-set meter (≤ 1,024, 06 §1.1);
  schema-migration runner with preview diff.
- *Stable IDs:* record hash IDs are minted once at creation and stored in the file, so renames never
  change them and ledger item instances never dangle; a T28 rule rejects reuse and collisions.
- *Data → consumer:* one `.hrec` JSONC per record → record DBs per consumer (client, cell, Go);
  client cooks strip `server_only` (AAA-SEC-4).
- **MVP (Ph1):** inspector, grid, inheritance, validation, where used, CSV.
- **AAA (Ph3):** 100k-record tables at 60 fps, balance diffs between builds, approval workflow, XLSX
  exchange; live tuning through T27 hotfixes in Ph4.

**T09 Gameplay Systems Editors** (R08-T09, ED-P0-13; G01, G04–G06, M04, M06, M08).
- *Kernel:* attribute sets; effects with a stacking/timing timeline preview; cues with viewport
  preview; telegraph shapes (R03-P1-10).
- *HXL formula editor:* highlighting, evaluation against sample contexts, plots over level or any
  variable, golden test cases stored with the formula; compiled by the schemac library (06 §1.2).
- *Abilities:* gating fields plus an **ability graph** (T11 family); predicted → ASM, server-only →
  Luau. **Rollback-safety checks** fail compilation of a predicted graph on:
  - 06's rules: any Luau call, ledger operation, unpredicted spawn or wall-clock read;
  - more than 64 states or 32 i32 variables (`AsmInstance` ≤ 192 B); a cycle without a `Wait`;
  - math outside `hmath` nodes; RNG not keyed by (entity, predictionKey, frame); unsorted physics
    queries;
  - reading an attribute or tag not replicated to the owner (the client would mispredict), or
    applying a non-`predictable` self-effect.

  `helios-tool asm-verify` then fuzzes each program under random rollback/resimulation schedules in
  CI and asserts bit-identical `AsmInstance` and overlay state.
- *Loot and plug sets* (nested weights, guaranteed drops, pity, lockouts) with a **drop-rate
  simulator** on the runtime roller (SipHash-seeded PCG64, simulation salt): 10⁶ rolls ≤ 2 s,
  histograms, EV per item, "kills to first drop" p50/p90/p99, CI's chi-square and perk-pair tests
  (06 §12.2 #6), value injected per hour.
- *Crafting:* resource-class tree; schematic quality simulator plotting output distributions.
- *Progression graphs* with a "what changes for existing characters" diff and
  `ProgressionMigrationDef` authoring (NGE guard, 06 §5).
- *Economy seeds:* vendors, regional NPC orders, tax policies, capped reason codes, faction matrix.
- *Fitting sandbox:* `helios-fitsim` UI (DPS, EHP, time-to-kill, stacking penalties).
- *Data → consumer:* T08 records + `.hgraph` → ASM, Luau, HXL bytecode, record DBs → cell, owner
  client, Go HXL.
- **MVP:** Ph1 text ability graphs compiled by `helios-tool compile-graphs` with every check, attributes, effects,
  HXL editor; Ph2 visual ability graph, loot simulator, crafting, economy seeds, progression.
- **AAA (Ph3):** combat sandbox, fitsim at EVE fit scale, economy-sim harness for the Go market test
  rig; the `EconomyModelDef` editor follows 05 (Ph4–5).

**T10 Script IDE & Luau Debugger** (R08-T10, ED-P0-10; R10 §4).
- *Features:* **VS Code first**: `helios-tool ide-setup` writes `.vscode/settings.json` (luau-lsp
  1.70 + generated `.d.luau` definitions) and `launch.json` (DAP attach). The script host's DAP
  adapter (~2 kLOC over `lua_breakpoint`/`lua_singlestep`, R10 §4) serves breakpoints (incl.
  conditional), stepping, watches and coroutine lists over TCP from editor, client and cell VMs.
  Visual Studio stays the C++ IDE (CMake presets, natvis). Hot reload: save → assetd type-checks
  (`Luau.Analysis`) and compiles → PIE VMs run `__reload(old)`, ≤ 2 s. Clickable file:line errors;
  capability-manifest checks at publish; per-module CPU/memory via interrupt sampling and
  `lua_setmemcat`.
- **MVP (Ph1):** DAP in every VM, LSP definitions, hot reload, error linking.
- **AAA (Ph3):** embedded code pane with LSP/DAP clients for designers; RBAC-gated, audited remote
  attach to staging cells; replay debugging (DAP on `helios-cell --replay` at a tick, 04 §10);
  per-zone script budgets in T26.

**T11 Visual Graph Framework + Logic Graph** (R08-T11, ED-P1-01; ADR-002; R06-ENG-24).
- *Framework:* UI-independent model in ToolsFramework: GUID nodes, typed pins, subgraphs, per-family
  node libraries (C++ or Luau), type checker, complexity lint (nodes, fan-out, depth). Node positions
  live in a trailing `layout` block, so moving nodes never conflicts with logic edits.
- *Compile targets (06 §11):*

| Family | Compiles to | Live debugging |
|---|---|---|
| Logic, minigame | Luau with source maps | Node breakpoints (DAP), execution highlight |
| Ability (predicted / server) | ASM / Luau | Active state from `AsmInstance` |
| Formula | HXL bytecode | Inline evaluation |
| Quest, event, mission | Data state machine + Luau actions | Stage/objective overlay |
| Dialogue | Records | Simulator |
| BT / utility | Flat node arrays + curves | Running-node highlight |
| Material / VFX / anim state machine | Slang module / module stack / ozz blend tree | Previews |

- *Logic Graph:* event-driven level scripting (triggers, doors, spawns, objectives, cinematics),
  deliberately not general purpose (R08 F2).
- **MVP (Ph2):** framework, Logic Graph, Luau compile with execution highlight, text diff.
- **AAA (Ph3):** visual 3-way merge, server-side node breakpoints, per-node CPU heatmaps from Tracy.

**T12 Quest Editor** (R08-T12, ED-P1-02; G10, W07).
- *Features:* stage graph; objectives (Kill, Collect, Goto, Interact, Escort, Scan, Craft, Deliver,
  Survive, Custom); shared Conditions/Facts picker; rewards from T09 as ledger grants keyed by
  stage; world markers placed in T01; phase actions and data layers; group, raid and public-event
  credit; mission and site templates; `EventDef` graphs with contribution scoring; reachability
  validator; GM stage commands; **"view as state"** with party phase-sharing preview (§1.5,
  R03-P1-3).
- *Data → consumer:* `.hgraph` (quest family) → server-only `QuestDef`/`EventDef` records (06 §6);
  clients get display data.
- **MVP (Ph2):** branching quests, objectives, rewards, validator, debug commands, view-as.
- **AAA (Ph3):** template-driven dynamic missions (R03-P2-2), world-event scheduler preview,
  per-stage funnels (§3.7), coverage bots.

**T13 Dialogue Editor** (R08-T13, ED-P1-03; G11; R03-P0-11, R03-P1-1/2/4).
- *Features:* Line, Choice, Branch, Action and Cinematic nodes; speaker roles incl. `PlayerSpeaker`;
  group settings (resolver Roll/Leader/Vote/Owner, choice window); stable line IDs; **VO workflow**
  (written → locked → recorded → implemented; recording scripts per actor and language; placeholder
  VO via Windows speech synthesis, an external TTS command on Linux); a **simulator** with variable
  inspector and **N simulated players** for group rolls; screenplay view (§1.9); Yarn/ink/articy
  import; **coverage** (unreachable nodes, never-true conditions, missing VO or translations per
  species × gender × language) and playthrough bots (R03-P1-4).
- *Data → consumer:* `.hgraph` (dialogue family) → `DialogueDef` records for the cell's
  `ConversationSession`; line IDs → T25 tables and T20 VO banks.
- **MVP (Ph2):** branching, conditions/actions, simulator, line IDs, VO status. **Complete (Ph3):**
  group simulator, coverage, web screenplay.
- **AAA (Ph4):** **auto-staged cinematic conversations** (R03-P1-1): line metadata (speaker,
  listener, emotion, intent) generates shots, blocking, look-ats, gestures and lip-sync, refined by
  T14 overrides.

### 2.3 Presentation

**T14 Cinematic Sequencer** (R08-T14, ED-P2-04).
- *Features:* tracks for camera, transform, animation/montage, VFX, audio, dialogue line, event
  (gameplay or Luau), fade, subtitle, light, material parameter, visibility; role bindings (player
  slot, NPCs by tag); sequences authored in a reference frame (a capital ship warps in relative to
  its system); shots and takes; client-local or server-triggered playback synced to server time
  (`Cinematic.Play`, 06 §6).
- *Data → consumer:* `.hseq` JSONC → client sequence player; cell for triggers and state tags.
- **MVP (Ph3):** camera, transform, animation, event, audio and subtitle tracks; server-triggered
  world events.
- **AAA (Ph4):** nested sequences, conversation templates driving T13 auto-staging, offline movie
  render, virtual camera / mocap link over UDP, photo and replay capture (R03-P2-5).

**T15 Animation Suite** (R08-T15, ED-P0-11, ED-P1-08, ED-P2-03; R04, R05).
- *Features:* skeletons, sockets, retarget poses; glTF/FBX (ufbx) import through ozz offline
  builders; compression with error visualization (ozz first; ACL deferred, R10 §7); root motion;
  notifies (footstep, hit window, VFX, audio, ability event); additive clips, montages; **state
  machines and blend trees** (T11 family); 1D/2D blend spaces; two-bone and aim IK (ozz);
  **retargeting across species** (human ↔ digitigrade Keth); motion-matching database editor;
  full-body IK; crowd animation LOD.
- *Data → consumer:* `.meta` import settings + `.hanimgraph` → ozz archives and blend-tree data for
  client and cell (the cell samples hitboxes and root motion).
- **MVP:** Ph1 import and preview; Ph2 compression, notifies, root motion, state machine, blend
  spaces, two-bone IK.
- **AAA (Ph3):** retargeting, full-body IK, crowd LOD. Motion matching, FACS facial curves and VO
  lip-sync land in Ph4 with their runtime (02 §7.2) and T13 auto-staging (capability R04). **Ph5+:**
  learned motion matching.

**T16 Material Graph Editor** (R08-T16, ED-P0-12; ADR-003).
- *Features:* graph → **Slang** module implementing a material `interface`, avoiding `#define`
  permutation explosion (R10 §3); material functions; **parameterized instances**; preview meshes and
  lighting rigs; instruction, sampler and permutation stats against a hard PSO budget; layered
  materials (terrain, ship paint, wear, decals); runtime liveries.
- *Data → consumer:* `.hmat` graph / `.hmati` instance → `helios-shaderc` in assetd → SPIR-V,
  reflection blob, recorded PSO lists (client). The editor loads Slang at runtime for hot reload.
- **MVP:** Ph1 instances with live parameters; Ph2 surface-shader graph.
- **AAA (Ph3):** layered and livery materials, permutation analytics, per-platform limit checks.

**T17 VFX Editor** (R08-T17, ED-P1-09; R01).
- *Decision:* in-house module-stack editor for 03's GPU particle runtime. Effekseer (MIT) is not
  integrated: a second runtime would split budgets and shading; an asset converter may come later.
- *Features:* system → emitter → module stacks; CPU/GPU sprites, ribbons, beams, meshes, lights;
  curves and gradients; events (spawn on death or collision); depth collision; **scalability LOD and
  per-effect budgets** (particles, overdraw, GPU µs) for 1,000-ship battles (BENCH-3); shield, plume
  and hologram presets.
- *Data → consumer:* `.hvfx` → client-only effect binary (cue payloads, 06 §1.5).
- **MVP (Ph2):** module stacks, GPU sprites and ribbons, budgets.
- **AAA (Ph3):** simulation stages, data interfaces (skeleton, mesh, terrain), crowd-density
  culling, budget heatmap under BENCH-3 load.

**T18 Environment / Post / Decals** (R08-T18).
- *Features:* post-process volumes with priority and blend (exposure, LUT grading, bloom);
  per-planet atmosphere (Hillaire parameters) and sky; froxel fog; projected decals with a paint
  tool; reflection probes; per-biome time of day and weather; volumetric clouds and nebulae as 03
  ships them.
- *Data → consumer:* volume components + profile records → client renderer.
- **MVP:** Ph1 atmosphere and PP profiles in T08 with live preview; Ph2 PP volumes, decals, fog.
- **AAA (Ph3):** biome time of day and weather, probes, decal painting; cloud and nebula authoring
  with 03's Ph4 features.

**T19 UI Designer** (R08-T19, ED-P1-10; R06, ADR-010).
- *Features:* RmlUi RML/RCSS with hot reload; **view models generated from `.hschema`** with
  mock-data fixtures and live PIE binding; binding inspector; diegetic preview on 3D surfaces;
  gamepad-navigation overlay; pseudo-loc, overflow and RTL checks; resolution/DPI matrix preview;
  **accessibility checks** (contrast, minimum font size, colour-blindness simulation); WYSIWYG
  layout that writes RML.
- *Data → consumer:* `.rml`/`.rcss` + view-model schema → client game UI (08).
- **MVP (Ph2):** mock-data preview, binding inspector, pseudo-loc (Ph1 = text + 08's runtime hot
  reload).
- **AAA (Ph3):** WYSIWYG, diegetic preview, accessibility checks. **Ph5+:** sandboxed player addon
  API (UGC is a non-goal before Ph5, 01 §5.1).

**T20 Audio Tools** (R08-T20, ED-P1-11; ADR-013). ADR-013 makes the Helios audio event system
primary, so T20 authors it; Wwise/FMOD are optional `IAudioBackend` plug-ins with their own browser.
- *Features:* event records (random, sequence, switch, blend containers; parameters; states); buses,
  snapshots, mixer with live PIE metering; attenuation and cone curves with viewport visualizers;
  occlusion and reverb zones; notify → event mapping (T15); VO banks per language; bank and streaming
  budgets per zone; dynamic music (layers, stingers, beat-synced transitions); ship-interior
  acoustics.
- *Data → consumer:* `.hsnd` records + LFS WAV/FLAC → client banks.
- **MVP (Ph2):** events, emitters, attenuation visualization, buses.
- **AAA (Ph3):** music system, interior acoustics, budgets, optional Steam Audio geometry tagging.

**T21 Modular Assembly Editor** (R08-T21, ED-P1-05; G16, M09, G07; R04-P2-23) — ships, vehicles,
stations and housing parts.
- *Features:* part records with **ports** (size/type tags, transform, symmetry); grid/block and
  free-snap modes; **rules** from one HEADLESS `engine/assembly` library shared with the client
  builder and the cell (required modules; power, heat, mass, thrust, CPU budgets; centre of mass vs
  thrust; clearance; docking size); live stats via 06's `FittingValidator`; thrust-envelope preview
  (06 §8.2); interior volume detection; seats, stations, damage sections, liveries, fleet HLOD.
- *Data → consumer:* `.hassembly` part tree → prefab + ship record-template variant; port hierarchies
  become ledger locations on the cell (06 §8.4).
- **MVP (Ph2):** parts, ports, snapping, validation, stats (Kestrel variants, a housing kit).
- **AAA (Ph3):** multi-crew Mule, interior volumes, damage sections, shared player-builder library.
  **Ph5+:** shareable player blueprints (ED-P2-08).

**T22 Character Customization** (R08-T22, ED-P2-06; G15).
- *Features:* `SpeciesDef`/`CustomizationParamDef` editing (morphs, bone scales, palettes, texture
  layers, decals, mesh options, DNA blend); **bit-budget meter** for `CharacterAppearance` (≤ 600 B,
  06 §10); clothing layers and fit morphs per body type (digitigrade Keth); presets; an extreme-value
  stress grid rendering slider corners to catch clipping; facial-rig-safe ranges; crowd-bake
  settings.
- *Data → consumer:* records → client creator UI and crowd bake; cell range validation.
- **MVP (Ph2):** one species, sliders, palettes, presets (Ph1 basic morphs are T08 records previewed
  in T15). **AAA (Ph4):** multiple species, fit tooling, stress grid, crowd bake.

### 2.4 AI

**T23 AI, Navigation & Spawn Editor** (R08-T23, ED-P1-12; G13; R02-P1-14).
- *Features:* Recast/Detour tiled navmesh **per physics grid** (agent profiles, area paint,
  blockers, off-mesh links; `GridLink`s for airlocks, ramps and elevators trigger `Reparent()`; ship
  interiors baked in ship-local space, 06 §7); 3D flight volumes for space; BT and utility graphs
  (T11 family) with response-curve editors; blackboard schema per record template; perception
  preview; spawners, lairs with waves, population density painted on planets, patrol splines (T06),
  AI LOD tier preview; **server debug draw streamed into the viewport** (running BT node, threat
  table, path, perception).
- *Data → consumer:* nav tiles in containers (cell only); `.hgraph` → flat BT arrays;
  `SpawnerDef`/`PopulationDef` → cell.
- **MVP:** Ph1 bake command, spawn regions as T01 entities, text BTs (Hollow drones); Ph2 BT
  editor, spawner/population editor, live debug draw.
- **AAA (Ph3):** space navigation, squad/fleet coordinator tuning, ship-interior navmesh on moving
  grids, population-director preview from live metrics (§3.7).

### 2.5 Pipeline and operations

**T24 Asset Browser & Import** (R08-T24, ED-P0-08).
- *Features:* folders, collections, saved searches, thumbnails, tags; Explorer drag-drop (§1.3);
  **dependency graph and "where used"**; GUID-stable move/rename; import of glTF (primary), FBX
  (ufbx), PNG/TGA/EXR (tinyexr) → BCn (bc7enc_rdo) or basis/KTX2 (R10 §12); meshoptimizer LODs and
  meshlets; `.meta` sidecars (GUID, importer version, settings, source-DCC path, **provenance and
  licence**, 01 §5.2); reimport on change.
- *DCC live links:* "Edit in Blender/Maya" opens the recorded source; a DCC save writes glTF/FBX to
  the watched folder, visible in the editor ≤ 5 s. The Blender add-on is a separate GPL script with
  no engine code, talking only to the remote-control socket (legal sign-off, §5.3); the Maya plug-in
  is likewise separate.
- **MVP (Ph1):** glTF, FBX, PNG, EXR, thumbnails, GUIDs, reimport.
- **AAA (Ph3):** live links, drag-out, provenance reports, collections. **Ph5+:** Houdini Engine
  HDAs and USD (ED-P2-07) as optional proprietary-SDK plug-ins.

**T25 Localization Tool** (R08-T25, ED-P1-13; R03-P1-2, AAA-CNT-6).
- *Features:* keyed tables with **MessageFormat 2** (plural, gender, selectors; ICU in tools only,
  R10 §12); translator context (PIE screenshots, speaker, max length, line ID); pseudo-localization
  (accents, +40% expansion, bidi markers); length and glyph-coverage checks against game fonts;
  XLIFF 2 / CSV exchange; VO tracking (T13); previews through the runtime text stack.
- *Data → consumer:* `loc/<lang>/<table>.hloc` JSONC → client string tables, T20 VO banks.
- **MVP (Ph2):** tables, pseudo-loc, XLIFF round trip, missing-key rules.
- **AAA (Ph3):** web Loc Review (§1.9), TMS integration via XLIFF APIs, in-game review mode, 500k
  strings with search ≤ 300 ms; string hotfixes via T27 in Ph4.

**T26 Profiling & Debug Viz** (R08-T26, ED-P0-15).
- *Features:* Tracy 0.14.1 in every process (version-matched viewer launched by the editor); stat
  overlays (frame, tick, memory tags, draws, streaming, dilation); **packet and replication
  inspector** (04 §10: bandwidth per channel/class/component, priorities, relevancy set in the
  viewport, `.hnetcap`); authority view (cell boundaries, interest margins, ghosts, epochs);
  **content budget views** (per-cell triangles, draws, texture MB, entities, script CPU vs declared
  budgets, 01 pillar 6); memory by asset type; Luau profiler.
- **MVP (Ph1):** Tracy, overlays, packet inspector.
- **AAA (Ph3):** unified cross-process timeline, automated nightly-soak captures with regression
  diffs, budget heatmaps.

**T27 Live-Ops / GM Client & Admin** (R08-T27, ED-P1-15; G17; 05 §1.17; R02-P2-18 god client).
- *Split with 05:* the **web admin console** (05 §5, Go + htmx) owns accounts, ledger explorer,
  market, chat moderation, config, kill switches, content channels. The **native GM client** (editor
  in GM mode, or a client build with the GM role) owns in-world operations.
- *GM client:* MFA and GM connect token via the gateway; read-only entity inspection by default;
  audited GM commands with a reason (spawn, teleport, kick, modify runtime state; 05 §1.17);
  invisible observer; bug reporter filing position, entity states, log and screenshot with a
  `helios://` link; red live chrome (§1.3).
- *Hotfix pipeline* (R03-P1-11, AAA-ITR-8): data/Luau change → hotfix build → canary zone → staged →
  live, with rollback. The event scheduler toggles data layers from the calendar.
- **MVP (Ph1):** GM console commands in client and PIE with audit log. **Ph3:** GM client on shards,
  observer, bug reporter.
- **AAA (Ph4):** staged live-world edits with two-person approval, A/B configs, hotfix ≤ 15 min,
  telemetry heatmaps in the GM viewport, complete web admin.

**T28 Validation & Content QA** (R08-T28, ED-P0-14).
- *Features:* the rules engine (§3.6) on save, pre-commit, CI and nightly; **issue browser with quick
  fixes** (TrenchBroom model; double-click navigates to entity, record, node or line); budgets;
  reference integrity; missing localization; quest reachability, dialogue and navmesh coverage;
  **server-data leakage check on client cooks** (AAA-SEC-4); **Windows/Linux portability rules**
  (reference case must match disk, no case-only path collisions, paths ≤ 180 characters, no
  reserved names like `CON`/`AUX`).
- **MVP (Ph1):** framework, ≥ 20 rules, CI gate, issue browser.
- **AAA (Ph3):** ≥ 150 rules covering every asset class (required from Ph2, AAA-TOOL-6), auto-fix bot
  PRs, content-health dashboards.

**T29 Build / Cook / Deploy** (R08-T29, ED-P0-16).
- *Features:* `helios-assetd` (§3.1); **cook profiles** for client (win64, linux64), cell
  (collision, navmesh, gameplay data, no textures) and Go services (record tables); packaging into
  content-defined-chunked containers with signed manifests (05 §7, 08); recorded PSO lists
  (AAA-REN-4); "cook & launch standalone"; `helios-patch publish` to dev/staging channels; build
  dashboard.
- **MVP (Ph1):** local daemon, CI cook, client/cell/service split, manifests.
- **AAA (Ph3):** single-zone incremental cook ≤ 60 s (AAA-ITR-4, from Ph2), distributed cook workers
  on a NATS work queue, nightly full cook ≤ 4 h, patch-size reports (AAA-CNT-7).

**T30 Collaboration Service** (R08-T30, ED-P1-14, ED-P2-01; R06-ENG-31) — protocol in §1.8.
- *Editor UI:* presence avatars in viewport and outliner (who, where, selecting what); lock badges;
  session panel (participants, stream lag, conflicts); in-world notes; follow-user camera.
- **MVP (Ph2):** LFS lock mirroring, who-has-what-open presence, soft locks.
- **AAA (Ph3):** live edit instances, transaction stream, conflict rules, publish preview, ≤ 1 s
  propagation; staged live-shard editing ships through T27 (Ph4).

---

## 3. Supporting infrastructure

### 3.1 Asset processor daemon (`helios-assetd`)
One per workstation per project, started by the editor or `helios-tool`, after O3DE's Asset
Processor (R06 §4.2, R06-ENG-11):
- watches sources, runs importer/builder jobs on the IO and compile pools, tracks source → product
  dependencies, writes products to the DDC;
- **priority:** assets requested by the viewport or PIE jump the queue;
- IPC over `\\.\pipe\helios-assetd-<project>` (Unix socket on Linux); broadcasts **hot-reload
  notifications** to editor, PIE cell and clients, which swap by handle;
- Windows: retries with backoff on sharing violations while a DCC holds a file; warns at startup if
  the repo or DDC is neither on a Dev Drive nor Defender-excluded (as `helios-backend doctor` does,
  05 §5).

### 3.2 Derived-data cache
- Key = `H(source bytes, settings, builder version, platform, dependency keys)` (ADR-006).
- Local tier `%LOCALAPPDATA%\Helios\DDC`, then a shared studio tier (`helios-ddc`, content-addressed
  HTTP store on MinIO/S3, filled by CI). A prebuilt editor plus warm shared DDC gets an artist
  in-game in ≤ 10 min (AAA-ITR-6). LRU eviction, 200 GB default cap.

### 3.3 Thumbnails
assetd renders 256² thumbnails offscreen with the real renderer (meshes, materials, prefabs, VFX;
hover turntables at AAA), stores them in the DDC and regenerates them when dependency keys change;
CI uses lavapipe.

### 3.4 Asset registry, dependency graph, redirects
- `.helios/registry.db` (SQLite, allowed for tools by ADR-014) indexes GUID ↔ path, type, tags,
  dependencies and reverse dependencies, queryable without loading (R06-ENG-13); rebuildable from
  sources in ≤ 60 s for the sample project.
- References are GUIDs, so moves need no redirect. Names and paths used in Luau or config get a
  `redirects.jsonc` entry until `helios-tool fix-redirects` rewrites every reference and deletes it
  (explicit, unlike Unreal's hidden redirectors).

### 3.5 Search
- **Ctrl+P** "go to anything": assets, records, entities, graph nodes, commands.
- **Ctrl+Shift+F** full text over names, tags, record string/enum fields, localized text, Luau and
  node titles (SQLite FTS5).
- Typed, savable queries: `type:ItemDef tag:Item.Weapon.* attr:Damage>100 where-used:itm/kestrel_mk2`.
- Budget ≤ 200 ms over 1M indexed items.

### 3.6 Validation rules engine
- Rules are native C++ or sandboxed Luau: `{id, severity, assetClasses, triggers: save | commit | ci
  | nightly, check(doc, ctx) → issues, quickFixes[]}`.
- Incremental: only changed documents and their reverse dependencies re-run; on-save ≤ 200 ms per
  document.
- Suppressions need a written justification and are reported. CI emits **SARIF**, so GitHub
  annotates PRs.

### 3.7 Telemetry-informed design tools
Aggregated read-only queries via the telemetry service (05 §1.16) under RBAC; no personal data leaves
the service.
- **Viewport heatmaps** (T01, T04, T07): deaths, kills, traffic, loot, density, client frame time by
  location.
- **Economy dashboards** in ImPlot: faucets and sinks per reason code, price indices, money supply;
  links to Grafana (ED-P2-10).
- **Design feedback:** quest funnels, dialogue choice distributions, spawner kill rates, observed
  time-to-kill vs the T09 simulator's prediction.

---

## 4. Budgets, UX standards, accessibility

### 4.1 Editor performance and iteration budgets
DEV hardware, sample project, measured in CI or nightly.

| Metric | Budget | Criterion |
|---|---|---|
| Cold open to interactive viewport, warm DDC | ≤ 10 s | AAA-ITR-2 (Ph2) |
| Open a 50k-entity container | ≤ 5 s | |
| Hot reload p95: Luau and records (Ph1), shaders and assets (Ph2) | ≤ 2 s | AAA-ITR-1 |
| PIE cold start (cell + backend + 2 clients) / warm restart / stop | ≤ 15 / 5 / 1 s | AAA-ITR-3 |
| Gameplay `.cpp` edit → relinked and reloaded (MSVC) | ≤ 30 s | AAA-ITR-5 |
| Editor frame p95 with BENCH-1 loaded | ≤ 16.7 ms (editor CPU ≤ 2 ms, GPU passes ≤ 1 ms) | |
| Select / transform / undo 10k entities | ≤ 50 / 100 / 250 ms | |
| Save 1,000 dirty entity files | ≤ 2 s | |
| Edit visible in all editors p95; content publish | ≤ 1 s; ≤ 5 min | AAA-ITR-7 |
| Incremental single-zone cook | ≤ 60 s | AAA-ITR-4 |
| 100k-record compile / grid scroll | ≤ 60 s / 60 fps | AAA-CNT-5 |
| Journal loss; editor crash rate | ≤ 1 tx; ≤ 1 per 40 h (Ph3), 100 h (Ph4) | AAA-STB-2 |
| Editor RSS, sample project | ≤ 16 GB | |

### 4.2 UX standards
- **Command palette** (Ctrl+Shift+P) lists every command with its shortcut; Ctrl+P goes to anything.
- **Shortcuts** follow Windows and industry conventions, rebindable, with keymap presets (Helios,
  Unreal-like, Maya/Blender navigation):

| Action | Keys | Action | Keys |
|---|---|---|---|
| Save / Save all | Ctrl+S / Ctrl+Shift+S | Undo / Redo | Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z |
| Duplicate / Delete / Rename | Ctrl+D / Del / F2 | Move / Rotate / Scale | W / E / R |
| Focus / Frame all | F / Shift+F | Play / Simulate / Stop | F5 / Alt+F5 / Shift+F5 |
| Possess or eject | F8 | Breakpoint / Step over / Step in | F9 / F10 / F11 |
| Find in files | Ctrl+Shift+F | Set / recall bookmark | Ctrl+Shift+1…9 / Ctrl+1…9 |

- **Consistent undo:** graphs, curves, terrain and grids all use transactions (AAA-TOOL-6); the
  History panel shows labels; undo never silently crosses into another user's work.
- Non-modal by default; long operations show progress and can be cancelled; errors link to source;
  tooltips come from schema doc comments.

### 4.3 Accessibility
Dear ImGui has no screen-reader support (R08 F1). Mitigations: UI scale 75–250%; high-contrast and
colour-blind-safe themes (pins and severities use shape as well as colour); full keyboard navigation
and rebindable keys; a reduced-motion option; the web tools used by text-heavy roles (§1.9) target
WCAG 2.2 AA with screen readers; T19 applies the same checks to player UI.

---

## 5. Delivery, acceptance, risks, traceability

### 5.1 Phase-by-phase delivery
**M** = MVP, **A** = AAA-complete, **lite** = generic-inspector or text authoring. Gates from 01 §3
(AAA- prefix omitted).

| Tool | Ph0 | Ph1 | Ph2 | Ph3 | Ph4 | Ph5 |
|---|---|---|---|---|---|---|
| Framework / UI | Transactions, journal, property grid, shell, `helios-tool` | Viewport, PIE, automation | Crash reports, role layouts, source-control UI | Web tools, remote control | Accessibility pass | |
| T01 World | | M | Data layers | A | | |
| T02 Prefabs | | M | | A | | |
| T03 Blockout | | | M | A | | |
| T04 Terrain/Planet | `pcg` lib | M (cube-sphere) | Ecosystems | A (Earth-size) | | |
| T05 Scatter | | | M | A | | |
| T06 Splines | | | M | A | | |
| T07 System/Galaxy | | M (Tallis) | Galaxy (Osk) | A | | Telemetry overlays |
| T08 Data | Headless inspector | M | Migrations | A | Live tuning | |
| T09 Gameplay | HXL test runner | lite (ASM compile, HXL) | M (loot, crafting, economy) | A | Economy model | |
| T10 Script | | M | | A | | |
| T11 Graphs | Graph model | | M | A | | |
| T12 Quest | | | M | A | | UGC missions |
| T13 Dialogue | | | M | Complete | A (auto-staging) | |
| T14 Sequencer | | | | M | A | |
| T15 Animation | | M (import) | M | A | Motion matching, facial, lip-sync | Learned MM |
| T16 Material | | M (instances) | Graph | A | | |
| T17 VFX | | | M | A | | |
| T18 Environment | | lite | M | A | Clouds (03) | |
| T19 UI Designer | | lite | M | A | | Addon sandbox |
| T20 Audio | | | M | A | | |
| T21 Assembly | | | M | A | | Player blueprints |
| T22 Character | | lite | M | | A | |
| T23 AI/Nav | | lite | M | A | | |
| T24 Assets | | M | | A | | HDA, USD |
| T25 Localization | | | M | A | String hotfix | |
| T26 Profiling | Tracy | M | | A | | |
| T27 GM/Live-ops | | M (GM cmds) | | GM client | A | |
| T28 Validation | Framework | M (≥ 20 rules) | All asset classes | A (≥ 150) | | |
| T29 Build/Cook | assetd skeleton | M | Incremental ≤ 60 s | A | | |
| T30 Collab | | | M (locks, presence) | A (edit instance) | Live shards via T27 | UGC publish |
| **Gate** | PLT-1 | TOOL-1 | TOOL-2, TOOL-6 | TOOL-3 (26/30), TOOL-5, ITR-7 | TOOL-4, ITR-8 | |

At the end of Ph3, 26 tools are AAA-complete; the four finishing in Ph4 (T13, T14, T22, T27) are
exactly those AAA-TOOL-4 names.

### 5.2 Acceptance criteria
Automated tests or QA-timed scripted designer tasks, on Windows and Linux.

| ID | Criterion | Ph | Refs |
|---|---|---|---|
| ED-1 | Headless ToolsFramework round-trips 10k random transactions (apply, undo, redo) to byte-identical JSONC on MSVC, GCC and Clang; `kill -9` mid-burst loses ≤ 1 transaction | 0 | PLT-1, STB-2 |
| ED-2 | **Saltmarch slice, editor only:** a designer places the Saltmarch outpost on Harrow (terrain stamp, ≥ 40 entities, landing pad), a Hollow-drone spawn region with navmesh and a Kestrel variant record, then flies the BENCH-2 descent in PIE with 2 clients and 4 bots; no text editor, no engineer | 1 | TOOL-1, ITR-3 |
| ED-3 | 100 scripted Luau/record edits: save → visible in running PIE cell and client ≤ 2 s p95; placement edits apply without reload; PIE cold start ≤ 15 s | 1 | ITR-1, ITR-3 |
| ED-4 | Predicted-ability compiler rejects 100% of a ≥ 50-graph unsafe corpus (every T09 rule); `asm-verify` passes all Foundation abilities under 10k random rollback schedules | 1 | M06, 06 §12.2 #2 |
| ED-5 | Zero `server_only` fields in client cooks; editor smoke on Windows 10/11 and Ubuntu 24.04 | 1 | SEC-4, PLT-3 |
| ED-6 | Automated tests show R08-ED-P0-01…16; every record type opens in T08; every asset class has ≥ 1 T28 rule; a direct-write detector finds zero mutations outside transactions | 2 | TOOL-2, TOOL-6 |
| ED-7 | **Osk loop, editor only:** resource class, schematic, vendor, loot table and a two-stage quest with dialogue; the loot table passes chi-square at 10⁶ rolls in simulator and CI | 2 | TOOL-2, G04–G06 |
| ED-8 | Cold open ≤ 10 s (warm DDC); single-zone incremental cook ≤ 60 s; artist prebuilt editor → in-game ≤ 10 min | 2 | ITR-2/4/6 |
| ED-9 | **Designer day:** a newcomer adds a hull variant (T21), a weapon with rolled perks (T09), a 3-step quest with dialogue (T12/T13) and a vendor in ≤ 1 day, with no engineer and no restart | 3 | TOOL-5 |
| ED-10 | **Co-edited BENCH-5:** three designers in one Saltmarch edit instance build ≥ 300 structures and 3,000 decor items in 2 h; propagation ≤ 1 s p95; journal vs git export diff = 0; publish to a play instance ≤ 5 min | 3 | ITR-7, TOOL-3 |
| ED-11 | BENCH-4 Hollow Vault encounter authored with T01, T11, T12, T23 only; coverage shows 0 unreachable nodes; 26/30 tools AAA-complete (§5.1) | 3 | TOOL-3 |
| ED-12 | Editor crashes ≤ 1 per 40 user-hours (Ph3), ≤ 1 per 100 (Ph4), measured by sentry | 3–4 | STB-2 |
| ED-13 | ~300 voiced lines in 2 languages auto-staged, ≥ 80% of conversations with no manual shot edits; a data/Luau hotfix goes live via T27 in ≤ 15 min with canary and rollback; 30/30 tools AAA-complete | 4 | TOOL-4, ITR-8, CNT-6 |

### 5.3 Risks and mitigations

| Risk | Mitigation |
|---|---|
| ImGui widget work (property grid, graphs, timeline, code pane, data grid) is the largest hidden cost (R06 §6.15) | Widget library first (Ph0–1) under one owner; reuse vendored ImGuizmo, ImPlot, imgui-node-editor; review gate requiring shared widgets; ~25% of tools effort reserved |
| 30 tools is too much scope | Generic T08 inspector makes every type editable on day one; bespoke tools only for measured workflows; cut Ph5+ items, then AAA polish, never MVP items |
| Co-editing corrupts or loses edits | Property-level ops, keyed lists, one sequencer; CI check journal replay = git export; soaks with scripted "bot editors" via the automation API |
| Editor previews drift from the game | Shared libraries (`pcg`, `assembly`, HXL, ASM, roller); golden-hash tests |
| Iteration regresses as content grows (Destiny) | §4.1 budgets nightly with a ±10% gate; warm cell; overlays instead of recooks |
| ImGui lacks accessibility and complex text | Web tools for text-heavy roles; runtime text previews; scaling, themes |
| Licences: Blender add-on must be GPL; Maya/Houdini/Wwise/FMOD SDKs are proprietary; libgit2 is GPL-with-exception | Separate optional plug-ins or processes; git CLI; legal sign-off before distribution |
| SWG terrain patents (ADR-006) | Node-DAG formulation; legal review before Ph4 (01 §5.2) |
| Web tools add a stack | Go + htmx as in 05, no Node toolchain; one validation path (`helios-tool`) |

### 5.4 Traceability
- **R08:** §3 T01–T30 → §2; §4.1–4.10 → §1, §3; ED-P0-01…16 → ED-6; ED-P1-01…15 → §2; ED-P2-01
  (§1.8), -02 (T04, T05), -03 (T15), -04 (T13, T14), -05 (T03), -06 (T22), -07 (T24), -08 (T19,
  T21), -09 (T29), -10 (§3.7).
- **R03:** P0-1 (T08), P0-2/3 (§1.8), P0-4 (§1.7), P0-5 (§1.6), P0-11 (T13); P1-1/2/4 (T13, T14,
  T25), P1-3 (§1.5, T12), P1-5 (T01, T30), P1-10 (T09), P1-11 (T27); P2-2 (T12), P2-5 (T14); §8.1
  web tools (§1.9).
- **R04:** P0-5 (T01), P0-9 (T08), P1-17 (tools), P2-23 (T21). **R02:** P0-5 (T04), P1-14 (T23),
  P2-18 (T04, T13, T23, T27).
- **R06:** ENG-11/13 (§3), ENG-14 (§1.2), ENG-15/27 (§1.6), ENG-24 (T11), ENG-25 (T04, T05), ENG-31
  (§1.8). **R10:** §3 (T16), §4 (T10), §8 (§1.3), §12 (T24, T25). **R05** §6 targets (§4.1).
- **06 asks:** graph compilers with rollback checks and loot/EV simulators (T09, T11); "view as
  state" (§1.5, T12); AI and spawn tool (T23).

### 5.5 Cross-section dependencies
- **02 Engine runtime:** `EDITOR_ONLY` in `helios_module`; `.hcont/.hent/.hprefab/.hrec` formats;
  **keyed list elements** (`@keyed` GUIDs) in `.hschema`; **record hash IDs minted once and stored
  in source**, never recomputed from paths; schemac editor metadata (`@editor`, units, doc comments)
  and Go validators; DAP adapter in the script host; `engine/pcg` determinism; hot swap by handle.
- **03 Rendering:** editor passes (ID buffer, outlines, grid, debug draw); offscreen thumbnails; an
  ImGui backend on the RHI; the Slang material `interface` contract; the GPU-particle module
  contract; cloud and nebula preview paths.
- **04 Networking:** tools channel for PIE overlays; GM connect tokens and RBAC; NetSim toolbar
  hooks; server debug-draw stream; DAP hooks in `helios-cell --replay`.
- **05 Backend:** the **collab service** (sessions, JetStream streams, submit/validate, locks,
  notes, snapshots, git export); overlay content versions; telemetry query API; web-tools hosting.
- **08 Client:** RmlUi view-model codegen and hot reload; runtime text rendering for editor
  previews.
- **09 Roadmap:** gates ED-1…13; a dedicated widget-library owner; nightly editor-performance CI;
  bot-editor soaks.
- **R10 manifest:** vendor **imgui-node-editor** (MIT, WP-2.10); **nats.c** in the editor (now in ADR-013);
  vendor the CodeMirror bundle and Apache-2.0 fonts.
