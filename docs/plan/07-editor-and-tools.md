# 07 — Editor and Tool Suite

> **Status:** draft v5. Round-1 review fixes: physics and volume authoring §2.6, multi-cell PIE §1.6,
> UI test harness §4.4, collab publish and rebase §1.8, source-control scale §1.7. Round-2 review fixes:
> the editor extension SDK §1.10 (ED-19), collab durability and scale §1.8.1 (ED-20), and derived-data
> rebuild budgets §4.1.1 (ED-3, ED-21). Round-3 review fixes: collab session scope for documents with no
> zone, with a project-wide data session and one home session per document §1.8.2 (ED-20); from the other
> sections' round-3 fixes, T08's governed schema editor for dynamic project types (ED-22), T27's case queue,
> T21's ground-vehicle authoring and T15's mount and rider sets (with two T28 vehicle rules), T04's
> collision-tile checks and T22's per-tier vertex lint. Round-4 review fixes: world scripts (05 §1.23) in the
> editor §1.6.2: one WSH in every PIE mode with per-partition DAP, T08's `worldscript` block editor with the
> `ws.*` rules and a migration preview, the World scripts panel (T26/T27), world-script hot-reload budgets in
> §4.1, and ED-23.
> **Conforms to:** ADR-001, -002, -003, -004, -005, -006, -007, -009, -010, -011, -012, -013, -014, -016.
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
8. **Seams are reproducible at a desk** (06 §0 rule 4, R04 §9.4). Split-authority bugs live at cell
   boundaries, so PIE can run N cells with forced handoffs, per-cell kills and per-cell debuggers
   (§1.6). CI torture bots are a safety net, not the only way to see a seam.
9. **Tested at the UI, not only the command layer.** Tools are built by agents in headless Linux
   containers, and the user has little time for manual sessions (09 K31). Every tool's layout, DPI and
   themes, and the ED designer flows, are therefore driven through injected UI input every night (§4.4).
10. **Extensible without forking** (01 §1.1, R08 §4 item 9). Every AAA studio builds its own tools. A
    studio adds panels, documents, importers, cook steps, viewport modes, rules and node libraries through
    a public, versioned, documented editor API, never by editing engine source (§1.10). Built-in tools
    register through the same API, and three of them are built against its public headers only.
11. **Edits keep derived data live.** Navmesh, HLOD, impostors, probes and scatter caches that an edit
    invalidates are rebuilt incrementally within stated budgets. Until then a stale product stays visible,
    marked as stale, and is never replaced by a hole (§4.1.1).

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
| `include/helios/editor/` (`helios::editor`, `HELIOS_EDITOR_API`) | The public editor extension API (§1.10): registration contexts, `TxBuilder`, `DocView`, the `Ui` façade and `Canvas`, and the interfaces for viewport modes, importers, builders and node libraries. It contains no ImGui or other third-party types | Engine tools and project gems' `editor-core` and `editor-ui` modules |
| `gems/<gem>/editor/` | A project's or gem's editor modules and Luau editor scripts (§1.10) | Loaded at runtime by the editor, `helios-tool` and `helios-assetd` |
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
Transaction   {id: (user, lamport), label, ops[], baseRev{doc → seq}, mergeKey, author, origin, time}
```

`origin` is stamped by ToolsFramework from the input path that produced the command, never by the caller:
`ui` (a human's input in the editor or a web tool), `ui-scripted` (`helios-uitest`), `luau` (editor automation),
`rpc` (remote control, DCC plug-ins), `cli` (`helios-tool`), `import` (asset import, live links) or `collab`
(rebase and merge). It is journaled with the transaction and feeds the nightly `tool-provenance` report of
01 AAA-TOOL-10 (Ph3+).

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
  `Asset.import`, `Validate.run`. Project scripts in `editor/scripts/*.luau` add commands, menu items,
  buttons and T28 rules, typed by the generated `.d.luau`. From Ph2 they also add dock panels and
  customizers through the immediate-mode `EditorUI` binding, text-format importers and node libraries
  (§1.10).
- *Remote control:* JSON-RPC 2.0 over `\\.\pipe\helios-editor-<pid>` (Unix socket on Linux) exposes
  the command registry to DCC plug-ins, test harnesses and `helios://` links.

**Headless batch mode.** `helios-tool <verb>` links ToolsFramework without EditorUI: `validate`,
`fix`, `migrate`, `compile-graphs`, `asm-verify`, `loot-sim`, `bake-nav`, `export-loc`,
`merge-driver`, `fix-redirects`, `ide-setup`, `fit-hitboxes`, `pie` (headless PIE, including
`--cells N`), `gen-scale-repo`, `collab-bot` (a scripted co-editor for soaks), `collab restore` (§1.8.1),
`doctor`, `run <script.luau>`. Enabled gems' `editor-core` modules add their commands and verbs (§1.10). `helios-uitest` drives a real editor
process through the remote-control socket (§4.4). GPU verbs (thumbnails, HLOD,
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
| **Property grid** | From schemac editor metadata: ranges, units, enums, categories, doc tooltips, `client`/`server_only` badges. Per-type/per-field **customizers** (`@editor(customizer=…)`, registered in C++ or Luau through §1.10); multi-edit with mixed values; **override markers** with revert for prefab and record-template inheritance; "copy path", "where used" | all |
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
| **Play** (F5) | `helios-cell --embedded-gateway` out of process (authority); client 1 in-process in the viewport; clients 2…N as in-process worlds in own windows (cheap) or `helios-client` processes (fidelity, crash isolation); M `helios-bot`s with Luau behaviours (04 §10); dev `helios-backend`, auto-started; from Ph3, one world-script host (`helios-cell --role world-script --dev`) when the project declares a `worldscript` block (§1.6.2) | Playtest |
| **Simulate** (Alt+F5) | Cell only, editor camera; live entities selectable with read-only live state; "Keep simulation changes" turns chosen live values into a transaction | AI, physics, spawns |
| **Possess/eject** (F8) | Switch between client 1 and the free camera without stopping | Debugging |
| **Multi-cell** (Play or Simulate with **Cells: N**, 1–8; Ph3) | `helios-gateway` + N `helios-cell` processes + the dev backend's orchestrator (`LocalProcessPlacer`, zone leader, handle blocks, fence), partitioned by a debug partition or the zone's `ZonePartitionDef` (§2.6); clients, bots and the world-script host as in Play | Handoff, ghost and effect-ordering bugs |
| **Two zones** (Ph2) | A second cell hosts an adjacent zone, so v0 zone transitions exercise the handoff seam (04 §6.5). In a project with one zone, the second cell hosts a second instance of it. From Ph3 both cells share one world-script host (§1.6.2) | Transition bugs before v1; cross-zone world scripts |
| **Join shard** | Client joins a dev/staging shard at its pinned content version | Multi-machine |

- **Content path.** The cell loads cooked base content from the assetd DDC plus an **editor
  overlay**: dirty documents serialized as a delta package over the tools channel. PIE never
  requires saving, and the overlay is the edit-instance transaction format (§1.8).
- **Hot swap.** Placement and record edits apply next tick (AAA-ITR-3); Luau (`__reload(old)`, 06
  §11) and records ≤ 2 s (AAA-ITR-1, Ph1); shaders and assets ≤ 2 s from Ph2; world-script handlers ≤ 2 s
  and additive `worldscript` block edits ≤ 5 s from Ph3 (§1.6.2). Gameplay C++ modules
  build as DLL/.so in dev (ADR-016) and live-reload in the editor and PIE processes in ≤ 30 s on MSVC
  (AAA-ITR-5, 02 §1.4) while documents stay open. "Rebuild & restart PIE" is the fallback when engine
  headers change.
- **Network emulation.** Per-client NetSim profiles (`lan`, `good`, `mobile`, `awful`, custom; 04
  §10), "drop client", "kill cell" (AAA-SRV-12) and a TiDi override.
- **Debugging.** Every VM announces a DAP port (T10), including each world-script partition VM (§1.6.2);
  "Launch cell under debugger" uses
  `devenv /DebugExe` (Windows) or a generated VS Code `launch.json` (Linux); Tracy attaches to all.
- **Start budget.** Cell + backend + 2 clients cold ≤ 15 s (AAA-ITR-3). The cell stays warm and
  reloads its world in ≤ 3 s, so the next PIE starts in ≤ 5 s.

#### 1.6.1 Multi-cell PIE (Ph3, with 04's v1 authority; 06 §0 rule 4)

Scripters and gameplay programmers must be able to reproduce a handoff, ghost or effect-ordering bug on
one Windows or Linux desktop and step through it. NS-3.2 and NS-3.6 torture bots find such bugs in CI;
multi-cell PIE is where people fix them.

- **Partitions.** Three sources, chosen on the PIE toolbar:
  - *Split here* (Ctrl+Alt+S): a vertical plane through the camera focus, facing the camera. On a
    planet the plane passes through the body centre, so the cut is a great circle.
  - *Debug grid*: k × 1 or k × k regions over the zone's bounds, with k ≤ 8 in total.
  - *Authored*: the zone's `ZonePartitionDef` from T01's Partition tool (§2.6.5).

  Debug partitions exist only in the PIE session. *Save as partition* turns one into a
  `ZonePartitionDef` draft transaction.
- **Controls** (PIE toolbar, T26 authority panel, `Editor.cmd` for scripts):

  | Control | Effect |
  |---|---|
  | Kill cell k | Hard-kills the process. The dev orchestrator promotes a pre-warmed standby, so 04 §6.4's recovery runs end to end; from Ph4 the standby restores from the zone's replicant (`helios-cell --replicant`, started by PIE), and *Kill replicant* exercises its resync |
  | Freeze cell k (n s) | Stops its ticks, which exercises the suspect path, gateway freeze and neighbours' late-message handling |
  | **Force handoff** (Ctrl+Alt+H) | Hands the selected AG root to the named or nearest neighbour now. It sends a dev-only `DevForceHandoff{root, toCell}` that bypasses only the H-past-boundary trigger; the protocol itself is 04 §6.3 unchanged |
  | Ping-pong | Force-hands the selected AG back and forth every n ticks (default 10) until stopped |
  | Trunk NetSim | Latency, jitter, loss and reordering on cell ↔ cell trunks, separate from client NetSim |

- **Seeing authority.** T26's authority view draws in the viewport: region tint and boundaries, the
  handoff band H and the ghost margin M, entities tinted by owner cell, hatched ghosts, epoch labels
  and in-flight handoffs as arrows. An **effect log** lists `send_effect` traffic per (src AG, dst AG)
  with per-pair sequence numbers, grace-window forwards and de-duplication hits. An idempotency ID
  applied twice, a lost input sequence or a Luau `StaleHandle` after a handoff is a red **seam
  error**. With *Break on seam error* set, PIE pauses there.
- **Debugging.**
  - Every cell's Luau VM announces its own DAP port, and `helios-tool ide-setup` writes a VS Code
    compound configuration that attaches to all of them. Breakpoints apply to every cell by default.
  - When any cell stops at a breakpoint, the PIE supervisor asks the zone leader for a **debug hold**:
    a `ZoneSchedule` hold from `now + 2`, so the other cells stop within 2 ticks. While a cell reports
    `DebugPaused`, gateways and the dev orchestrator suspend suspect and failure detection for it, and
    handoff timers restart on resume. These hooks exist only in processes started with `--dev` and
    are compiled out of shipping builds (ADR-016; 04 §10.3).
  - "Launch cell k under debugger" works per cell (`devenv /DebugExe` or `launch.json`).
- **Repro bundles.** Every PIE cell records its replay log (04 §10.2). Trunk traffic is logged at each
  cell's `SimInbox`, so each log replays on its own. *Save seam repro* bundles the N logs, the
  partition and the editor overlay into a `.hrepro` that any teammate replays under the debugger with
  `helios-cell --replay --dap`, one cell at a time or all together.
- **Headless.** `helios-tool pie --cells 2 --partition plane --script <scenario.luau>` runs the same
  set-up without UI. ED-14 runs nightly this way on Windows and Linux.
- **Budgets.** Four cells + gateway + backend + 2 clients: cold start ≤ 25 s, warm restart ≤ 8 s;
  ≤ 4 GB RSS per cell on the sample project; a debug hold reaches every cell in ≤ 2 ticks.

#### 1.6.2 World scripts in PIE and the editor (Ph3, with 05 §1.23's API 1.0)

World scripts are the studio's backend extension surface (05 §1.23): shard-scope Luau with declared tables,
RPCs, event subscriptions, timers and escrow-only value. A designer builds a cross-zone system with them, such as
a bounty board, a galactic senate or a shard lottery. The editor gives world scripts the same loop as zone
scripts: declare the block in T08, write handlers in T10, play in PIE, step through a handler, then inspect
the rows. `helios-admin ws rows` stays available for scripts and CI, but a designer never needs it.

**The host in PIE.**
- Every PIE mode that runs cells (Play, Simulate, Two zones and Multi-cell) also launches **one world-script
  host** (WSH), `helios-cell --role world-script --dev`, when the project declares at least one `worldscript`
  block. PIE's supervisor launches it beside the cells, as `helios-dev up`'s `--spawn ws` does (05 §5), so the
  toolbar can kill, restart and debug it like a cell. *Join shard* uses the shard's own WSHs, which the World
  scripts panel shows through T27 (below).
- The WSH hosts every partition of every script, as in 05's Phase 3 placement. Each partition is a `ws` region
  that the dev orchestrator leases to the WSH, so the `lease_gen` fence, the `ws_inbox` dedup and the
  `worldscript.Commit` path are the production code. In two-zone and multi-cell PIE every cell calls the same
  WSH, and that is what makes a PIE session cross-zone.
- **Store.** Rows live in the dev backend's embedded PG, in the PIE data directory
  (`.helios/pie/<user>/helios-data`, 05 §5), and persist across PIE sessions. The panel's *Reset tables* clears
  one script or all of them. *Load fixture* inserts rows from `scripts/world/<name>/fixtures/*.jsonc`, validated
  against the block, and funds the script's escrow wallet from the dev seed with the backed sum, so the backing
  check still holds. *Snapshot* and *Restore* save and reload the project's rows with their escrow balances, so
  a scenario can be rerun from the same data.
- **Content.** The WSH loads the scripts' server part from the DDC plus the PIE editor overlay (§1.6).
  Unsaved handler and block edits therefore reach it exactly as cell content does.
- **Failover.** *WSH standby* (a toolbar option, off by default) launches a warm standby. *Kill WSH* then runs
  05's takeover end to end: partitions serve again ≤ 10 s after confirmation. With the option off, *Kill WSH*
  restarts the process, and partitions resume from committed rows.
- **Start budget.** The WSH starts in parallel with the cells and serves every partition ≤ 3 s after the
  backend is ready, within PIE's ≤ 15 s cold start. It stays warm between PIE runs, as the cell does.

**Debugging.**
- **DAP ports.** Each partition VM announces its own DAP port to the PIE supervisor, as each cell's VM does
  (§1.6.1), and each port is labelled `<script>/p<k>`. T10's code pane and the compound configuration from
  `helios-tool ide-setup` attach to all of them, so a breakpoint in `scripts/world/<name>/*.luau` applies to
  every partition.
- **A stop pauses one partition.** Partitions are independent actors (05 §1.23 item 3), so a stop pauses only
  its own partition. That partition's queued invocations wait, and the other partitions keep serving. The
  following hooks exist only in `--dev` processes and are compiled out of shipping builds, like §1.6.1's hooks:
  - The WSH reports the partition as `DebugPaused`. The dev orchestrator then suspends H, T, P and R detection
    for that region, so no takeover happens.
  - A cell's `World.call` to the paused partition waits with no deadline. It never blocks a tick
    (05 §1.23 item 7).
  - The WSH sends in-progress acks every 10 s for JetStream deliveries that the paused partition holds, so they
    are not redelivered. A redelivery would still be deduplicated in `ws_inbox`.
  - Timers that fall due during the pause fire on resume, in due order.
  - Fuel counts instructions, not wall time, so a handler held at a breakpoint is never fuel-killed.
- **Break options.** *Hold zones on world-script break* is off by default. When it is on, a stop also takes
  §1.6.1's zone-leader debug hold, so the world stops with the handler. *Break on world-script failure* pauses
  PIE at a handler error, at a fuel kill and when an input moves to `ws_dead`.
- **Re-run under debugger.** In `--dev` mode the WSH keeps a record of its last 10k invocations (≤ 256 MB):
  each input, `ctx.now`, the RNG seed and the before-images of the rows the handler read. *Re-run* executes the
  handler against that record in a scratch VM with DAP attached. It commits nothing, and it diffs its buffered
  effects with the recorded ones. A difference flags non-determinism, such as state kept outside a declared
  table (05's "no hidden state" rule).
- **Native debugging.** *Launch WSH under debugger* works as it does for a cell (`devenv /DebugExe` on Windows,
  `launch.json` on Linux).

**Hot reload in PIE** (budgets in §4.1):

| Edit | Path | Save → live |
|---|---|---|
| Handler Luau that leaves table shapes alone | assetd type-checks the handler against the `world` realm's `.d.luau` and compiles it. Each partition swaps at an invocation boundary, a pause of ≤ 1 s (05 §1.23 item 9). PIE has no canary stages | ≤ 2 s p95 in every partition |
| Additive block edit: an optional or defaulted field, a `@was` rename, a widened number, an appended enum value, a new index, RPC, event subscription, timer or reason | schemac regenerates the server part and the `.d.luau`, then the WSH swaps. A new index backfills over the dev rows as batch invocations and becomes queryable when complete | ≤ 5 s p95 (as for T08's other types); index backfill ≤ 5 s per 100k dev rows beyond that |
| Non-additive table change: a removed field or enum value, a narrowed or retyped field, a changed `@escrowBacked` field or condition | Not applied hot. `ws.compat` blocks the save until a `migrate` handler exists for the version step. The handler then runs on dev data from the panel | Dry run or apply ≤ 60 s per 100k dev rows, with RPCs served throughout |
| Non-additive RPC record change within the compat epoch; a `@key`, `@partitionKey` or `@partitions` change | Refused. 05 §1.23 item 9 refuses the RPC change within an epoch, and it has no in-place path to re-key or re-partition a table. The preview suggests declaring a new table or RPC instead | — |

**World scripts panel.** This dockable panel is hosted by T26 for PIE and dev backends and by T27 for shards,
where it shows the red live chrome (§1.3). It reads and writes through the `worldscript` service's GM API,
the same API that `helios-admin ws call | rows | edit` uses (05 §1.23 item 7). In PIE it also reads the WSH's
`--dev` trace stream.

| Tab | Shows | Actions |
|---|---|---|
| **Scripts** | Each script and its partitions: holder process, `lease_gen`, state (serving, paused, `DebugPaused`, migrating, kill-switched), invocations/s against the 2k/s bucket, commit lag, cache MB and fuel kills | Toggle the `ws.<script>` kill switch (RBAC on shards). PIE only: *Fire timer now*, and *Emit test event* from a fixture, such as a `Killmail` |
| **Tables** | A virtualized data grid (§1.4) over one table, read through an index. Prefix and range filters compile to `query(index, from, to, limit ≤ 500, cursor)` and `count(index prefix)`, the calls handlers make, so the panel never scans a table. Row detail decodes every field and shows `schema_ver`, `version`, partition and `expires_at`. `EncryptedText` shows as `[encrypted, n B]`. PIE decrypts it with the dev KEK; on shards this panel never decrypts it | *Edit row* (below); *Call RPC*, with a form built from the request record, as a GM caller; *Check escrow backing now* (05 §1.23 item 6's audit, for this script) |
| **Invocations** | One line per invocation: ID, kind (RPC, event, timer, migration or GM), handler, partition, caller (cell region and character, or GM), start, wall time, **fuel used against the 5 ms limit**, heap peak, row reads and writes against the 256 cap, ledger intents, timers, event bytes, outcome (committed, deduplicated, fuel-killed, error, rate-limited or dead) and commit latency. Selecting one shows its row diffs and outbox entries. For an error it shows the Luau stack with file:line links | *Re-run under debugger* (PIE) |
| **Dead letters** | `ws_dead` entries: the decoded input, the error and the attempts | *Open handler at the error line*; *Re-run under debugger* (PIE); *Replay*, which resubmits the input as a new invocation after a fix (05 §6.8's runbook); *Discard*, with a reason |
| **Audit** | Every GM invocation and row edit: who, when, the reason, and the row before and after | — |

- **Source of the invocation log.** In PIE the Invocations tab lists every invocation, from the WSH's `--dev`
  trace, kept in a ring of 100k entries per session. On shards it lists failures, fuel kills, `ws_dead`
  entries and GM invocations from the service, with 05 §6.2's per-partition metrics. Shards have no
  per-invocation trace.
- **Row edits** are GM invocations (`helios-admin ws edit`), so they are deduplicated, committed like any other
  invocation and audited under 05 §1.17. The form requires a reason and shows the diff before it submits.
  It refuses a change to a `@currency` field or to a field that the table's `@escrowBacked` condition reads,
  and the `worldscript` service refuses one too, so an edit can never unbalance an escrow. A value correction
  goes through a GM-callable RPC whose handler issues ledger intents. In PIE, edits need no MFA but are still
  audited. On shards they need MFA and the GM role `ws.edit`, and from Ph4 they need T27's two-person approval.
- **Migrations.** For a pending non-additive change, the panel shows T08's migration preview. *Dry run* runs
  the `migrate` handler over a copy of the dev rows in a scratch schema. It reports 20 sample before-and-after
  row diffs, the failures and the time taken. *Run on dev data* runs 05's resumable 1,000-row batch invocations
  through the WSH, with a progress bar, while RPCs are still served. For an `@escrowBacked` table it then runs
  the backing check. `helios-tool upgrade-project` runs the same handler path (09 §2.7.3). On shards,
  migrations roll out through live ops (05 §1.23 item 9), and T27 shows their progress read-only.
- **Budgets.** A 500-row table page loads in ≤ 200 ms p95 over 1 M dev rows. An invocation appears in the log
  ≤ 1 s after its commit. The panel draws in ≤ 1 ms p95.

**Rules** (T28, on save and in CI; a `worldscript` error blocks T08's save, as the other schema lints do):

| Rule | Check | Severity | Ph |
|---|---|---|---|
| `ws.quota` | 05's quotas: ≤ 64 tables per project, ≤ 4 indexes per table, `@maxRows` ≤ 10 M, worst-case encoded row ≤ 16 KiB (lists and maps need `@maxLen`; `EncryptedText(n)` counts 4n + 48 B), and `@partitions` ≤ 4 (Ph3) or ≤ 16 (Ph4) | Error | 3 |
| `ws.escrow.backing` | Every `@currency(E)` field names an escrow `E` declared in the block and is the field of an `@escrowBacked(field, when: …)` declaration on its table, so no stored amount goes unbacked. Every such declaration names an `i64 @currency` field, and its condition reads only the row's own scalar and enum fields | Error | 3 |
| `ws.escrow.flow` | Each `release` and `sink` in the handlers names a reason of the matching kind (`EscrowRelease` or `Sink`), and each `grant` names a `ReasonCodeDef` with a `faucet {dailyCap}`. The generated `.d.luau` types make a violation a type error in the code pane too | Error | 3 |
| `ws.partition` | `@partitionKey` is the key or the leading field of an index. Every mutating RPC and every event subscription declares `@partitionBy` over a request or event field of the partition key's type | Error | 3 |
| `ws.index` | Every `query` and `count` call in the handlers names a declared index whose prefix it matches. schemac enforces this; T28 reports it at the call site | Error | 3 |
| `ws.privacy` | No `@pii` field (CONF-07). Free text is `EncryptedText`, only on a row that has a `@subject` field | Error | 3 |
| `ws.compat` | A non-additive table change with no `migrate` handler for its version step; a non-additive RPC record change within the compat epoch; a `@key`, `@partitionKey` or `@partitions` change | Error | 3 |
| `ws.globals` | A handler module that writes a module global after load. Globals are frozen at run time, so the write would fail | Warning | 3 |
| `ws.budget` | From PIE traces, a handler whose p99 fuel over the session exceeds 50 % of its 5 ms, or whose row operations exceed 128 of the 256 allowed | Warning | 3 |

### 1.7 Content versioning and source control

- **Layout.** Git is the source of truth (ADR-006, 05 §1.14). Text (JSONC, graphs, Luau, `.meta`) is
  plain git; binary sources (PNG/EXR/PSD, glTF/FBX/.blend, WAV/FLAC, terrain tiles) are Git LFS
  `lockable`. `.gitattributes` forces `eol=lf`; the canonical JSONC writer (schema key order, one
  property per line) makes equal data produce equal bytes.
- **Provider.** `ISourceControl` (status, lock, commit, history, diff, blame) shells out to the **git
  CLI** (Git for Windows bundles git-lfs). libgit2 is avoided: GPL-2.0-with-linking-exception is not
  on the allow-list (01 §5.2).
- **Helios projects are git-first.** Git + LFS is the only source control that the collab export,
  the merge driver, CI and the content-version registry are specified and gated against, and it must
  meet the scale budgets in §1.7.1. Perforce is an optional Ph4 adapter whose parity scope is fixed in
  §1.7.2 (R08 §4.8); no gate depends on it.
- **Changelists.** Perforce-style named changelists over git, committed one at a time; shelves are
  patches in `.helios/shelves`. Editing a lockable binary prompts for an LFS lock; owners show in the
  asset browser and are mirrored to the collab service.
- **Merging.** A `merge=helios` driver (`helios-tool merge-driver`) merges JSONC per property path
  with keyed lists; only same-path conflicts reach the visual record/entity/graph merge UI.
- **Hooks and branches.** Pre-commit runs T28 on changed files (≤ 10 s); CI validates and cooks
  (T29). Branches: `main`, `release/*`, `hotfix/*` (R03-P0-4).
- **Content versions** are immutable `(git commit, schema hash, record-DB hash, cook-manifest hash)`
  tuples registered with the content service (05 §1.14).

#### 1.7.1 Repository scale (git + LFS)

AAA art repositories run to hundreds of gigabytes, and one entity per file (OFPA) means millions of
small text files. Helios keeps git fast at that size with these settings, and proves it with a synthetic
reference repository.

- **Set-up.** `helios-tool new-project` writes the settings below and `helios-tool doctor` checks them:
  - `feature.manyFiles` (index v4, untracked cache) and `core.fsmonitor`: git's built-in daemon on
    Windows, a Watchman hook on Linux;
  - **cone-mode sparse checkout with a sparse index**, one cone set per role layout (§1.3): for
    example `content/zones/saltmarch/`, shared records and `editor/` for a level designer;
  - `git maintenance` (commit-graph, multi-pack-index, hourly prefetch);
  - LFS sources are not smudged on checkout (`git lfs install --skip-smudge`, plus a per-role
    `lfs.fetchinclude` for sources that role always edits). assetd fetches any other source on demand when a document that needs it opens. Most users consume cooked
    products from the shared DDC (§3.2) and never fetch the source;
  - `lfs.concurrenttransfers=16`. The LFS server must implement the Git LFS batch and locking APIs
    (GitHub, GitLab or self-hosted Gitea, MIT). The Compose kit runs Gitea with its LFS store on the
    same MinIO as the shared DDC.
- **Reference repository.** `helios-tool gen-scale-repo` synthesizes a **500 GB** repository:
  400,000 LFS objects (median 400 KB, 1 % above 100 MB, largest 4 GB), 1.5 M text files, 200,000
  commits and 20,000 active locks. Payloads are random bytes, so compression and deduplication cannot
  flatter the numbers.

| Metric (DEV workstation, 1 Gbit LAN to the LFS server, Dev Drive on Windows) | Budget |
|---|---|
| Role clone: sparse cone, full text history, LFS on demand | ≤ 20 min; ≤ 40 GB on disk |
| `git status` / editor source-control refresh, warm fsmonitor, 1.5 M tracked files | ≤ 1 s / ≤ 200 ms p95 |
| Commit 1,000 changed entity files, including the pre-commit T28 run | ≤ 15 s |
| LFS lock acquire or release (the UI shows a pending badge and never blocks) | ≤ 500 ms p95 |
| Verify 20,000 locks (`git lfs locks --verify`) / lock badges from the collab KV mirror | ≤ 3 s / ≤ 100 ms |
| On-demand fetch of one 200 MB source | ≤ 5 s |
| Pull one day of team changes (5 GB LFS, 20,000 text files) | ≤ 3 min |

The full repository runs weekly on lab hardware (W-class) and a 50 GB cut runs nightly (ED-18). If a
budget is missed by more than 2× in two consecutive weekly runs, the Perforce adapter (§1.7.2) moves
from optional to planned, and 09 re-plans it (risk in §5.3).

#### 1.7.2 Perforce adapter: parity scope (Ph4, optional)

`ISourceControl` over the `p4` CLI, shelled out like git with no SDK linked (the CLI is proprietary but
free to download):

| Git feature | Perforce equivalent in the adapter |
|---|---|
| LFS `lockable` binaries | Typemap `binary+l` (exclusive open); text sources are `text`, already LF-normalized by the canonical writer |
| Changelists and shelves emulated over git | Native pending changelists and `p4 shelve` |
| `merge=helios` driver | `p4 resolve` calls `helios-tool merge-driver` through `P4MERGE`, so the per-property merge and the visual merge UI are unchanged |
| Collab publish: one linearized branch and one PR (§1.8) | One shelved changelist per author run, in sequencer order, submitted by the collab service user. With `admin` permission the service sets each changelist's User field to its author (`p4 change -f`); otherwise the author goes in a `Helios-Author:` trailer |
| PR checks and SARIF annotations | Shelves are the pre-submit review unit. A `change-submit` trigger rejects a submit whose shelf has not passed T28 and cook; issues are posted to the review |
| Content version `(git commit, …)` | `(changelist number, …)`, registered by a `change-commit` trigger |
| `main`, `release/*`, `hotfix/*` | Streams with the same names |

Not in scope: GitHub-hosted workflows and per-commit git authorship. Before a project may enable the
adapter it must pass ED-3, ED-6's source-control tests and the ED-10 publish flow against a test depot.

### 1.8 Live collaborative editing (T30 protocol)

Tier A = files and locks (Ph1–2); Tier B = presence and soft locks, project-wide in the data session
(Ph2); Tier C = HeroEngine-style co-editing in an **edit instance** (Ph3; R08 §4.7, R03-P0-2/3).

- **Edit instance.** The orchestrator runs instance #0 of a zone with `kind=edit` in a dev shard.
  It simulates normally (designers can play in it) but persists nothing through checkpoints:
  authored changes live in the collab journal until they are published. The journal is mirrored to
  object storage within 30 s and to a WIP git ref every 10 minutes (§1.8.1). Play instances pin
  published content versions and never accept edit transactions.
- **Sessions and homes.** Each zone's edit instance has a zone session (`zone-<zoneId>`). Documents
  with no spatial home (records, graphs, quests, dialogue, string tables, prefabs) belong to the
  project-wide **data session** (`data`) unless a zone session has checked them out. Every document has
  exactly one home session at a time; editors route each transaction to it, and locks and presence for
  global documents are project-wide (§1.8.2). Everything below applies to both kinds of session.
- **Sequencer.** The Go **collab service** (content domain, 05 §1.14) is the only writer of the
  JetStream stream `COLLAB_<session>`. Editors link nats.c (allowed by ADR-013).

  | Channel | Contents |
  |---|---|
  | `collab.<session>.tx` | Accepted transactions, **one message each**, including multi-document ones (stored). A `Helios-Docs` header lists the touched documents, so editors filter to what they have loaded |
  | `collab.<session>.ctl` | Session events: group commit and abort, rebase, publish (stored) |
  | `collab.<session>.presence` | Core NATS, 5 Hz, not stored: user, frame, camera, selection, tool |
  | `collab.<session>.preview` | Core NATS, ≤ 10 Hz per user, not stored: the coalesced ops of a gesture in progress (a drag, a sculpt stroke), so others see it move. Only the gesture's final transaction goes through `submit` and the stream (§1.8.1) |
  | KV `LOCKS_<session>` | Lock leases, 90 s TTL, renewed every 30 s. Soft locks on global documents are all in `LOCKS_data`, whichever session homes the document (§1.8.2) |
  | KV `NOTES_<session>` | In-world notes linked to issues (R03-P1-5) |
  | KV `HOMES_data` | Read-only mirror of each global document's home session, epoch and transit state, for routing and badges (§1.8.2) |

- **Submit.** The editor applies a transaction optimistically (pending) and sends
  `collab.submit {tx, baseRev{doc → rev}, deps{session → seq}}` to the session that homes its
  documents (§1.8.2). A transaction may touch any number of documents. The service runs **one
  single-writer actor per session**, which is the session lock. For each submit it:
  1. checks that this session homes every touched document (otherwise `moved{doc, session}` or
     `inTransit{doc}`, and the editor re-routes), then RBAC and area permissions for every touched
     document;
  2. rejects on a foreign hard lock;
  3. compares every `baseRev[doc]` with its in-memory revision index. Disjoint paths auto-rebase; if
     any path overlaps, the whole transaction returns `conflict{doc, theirs}` (all or nothing);
  4. validates with the schemac-generated Go validators, including cross-document reference
     integrity;
  5. publishes the transaction as one JetStream message with `Nats-Msg-Id` set to the transaction ID and
     `Nats-Expected-Last-Sequence` set to the stream's last sequence, which fences a superseded service
     replica after a failover. Publishes are pipelined (§1.8.1);
  6. advances each touched document's revision.

  One message is one atomic apply: editors apply it, and edit-instance cells apply it at a tick
  boundary.
- **Multi-document operations.** Cross-container reparent, prefab apply-to-base, bulk find/replace
  and bulk operations across containers are ordinary multi-document transactions. They take soft
  locks on every touched entity as a set, in GUID order, all or none. A transaction that spans a
  zone's spatial documents and global documents first checks the global ones out to the zone session
  (§1.8.2).
  - **Transaction groups.** A transaction larger than 1 MB (the NATS payload limit), such as
    apply-to-base across 5,000 prefab instances, is split by the service into parts
    `{groupId, k of n}` followed by `commit{groupId}` on `.ctl`. Consumers buffer parts and apply
    nothing until the commit. A service that restarts with an open group publishes `abort{groupId}`.
  - Blob payloads (terrain tiles, poly meshes) never ride NATS: `Blob` ops carry hashes, and the
    bytes go to the LFS or DDC store before the transaction is submitted.
- **Conflict rules.** Disjoint paths commute. Same path: first writer wins; the other user gets
  "take theirs / reapply mine". Delete beats concurrent edits, which stay recoverable for 24 h.
  Keyed list inserts commute. Terrain sculpt deltas are additive and commute; terrain paint and
  scatter need a region lock. Graph edits are node/pin operations keyed by GUID.
- **Locks.** *Soft* per entity, record or graph (advisory; "ask / steal", steals logged), project-wide
  for global documents (§1.8.2); *region* (planet polygon or container bounds) for terrain and
  scatter; *hard* = Git LFS locks on binaries.
- **Late join.** Load the base commit and the newest snapshot (§1.8.1) of each joined session (the data
  session and the zone), then replay each stream from its snapshot's sequence, in parallel: ≤ 10 s for
  any session length with up to 20,000 changed documents per session.
- **Budget.** Visible in every editor ≤ 1 s p95 (AAA-ITR-7), typically ≤ 150 ms on a LAN.
- **When `main` moves (rebase).** A session is based on commit B. When `main` advances to M:
  - the service lists the files changed in B..M. Documents the journal never touched adopt M as they
    are;
  - documents touched by both sides are 3-way merged per property path by the `merge=helios` driver
    (base = B, theirs = M, ours = B + journal). A document the session adopted from another session
    uses its adopt content as the base instead of B, and a document it released with all its writes
    published takes M (§1.8.2), so two sessions never merge against each other. Clean results form a
    new session snapshot on M. Conflicts go to the authors of the conflicting transactions in the
    visual merge UI, and the rebase commits only when none remain;
  - if B..M changes a schema, `helios-tool migrate` first runs over the session snapshot. If no
    migration exists, the rebase is blocked and the session must publish on B first;
  - the rebase is one `rebase{from: B, to: M, snapshotSeq}` event on `.ctl`. Edit-instance cells
    hot-swap to the overlay (M + snapshot), and late joiners start from that snapshot.

  The service rebases automatically at most once an hour while `main` moves, and always before a
  publish. The session panel shows how far the session is behind `main`.
- **Publish.** *Submit session* rebases onto the current `main` head, then exports **one linearized
  branch** `collab/<session>` in sequencer order and opens **one PR**.
  - Consecutive transactions by the same author become one commit (split at 200 transactions or a
    10-minute gap). The author is the git author, the collab service is the committer, and a
    `Helios-Tx: <first>..<last>` trailer links the journal range.
  - Because the order is the sequencer's, every intermediate commit is a state the edit instance
    really had. An entity that one user created and another user referenced appears in order, and
    CI's incremental T28 run passes on each commit.
  - *Partial publish* selects containers or records. The service computes the dependency closure
    over the journal (a transaction depends on the earlier ones that created, or last wrote, any path
    it reads or references) and refuses a subset that is not closed, naming the missing transactions.
  - Dependencies on another session's unpublished journal (a document adopted from it, or a `deps`
    entry) are published first as that session's **carry** publishes, queued ahead of this PR (§1.8.2).
  - CI validates and cooks. The PR merges with a merge commit or rebase-merge, never a squash, so
    per-commit authorship survives. The merged build reaches dev play instances in ≤ 5 min
    (AAA-ITR-7). ED-10's "journal vs git export diff = 0" is checked on the branch head.
  - *Publish preview* registers an overlay content version (base build + the zone's and the data
    session's journal ranges) that dev play instances pin within seconds. Production channels accept
    only CI builds (R03-P0-3).
- **Live shards are never edited directly.** Staged live-world edits (T27, Ph4) use the same format
  but target an approved hotfix changeset shipped as a hotfix build (AAA-ITR-8).

#### 1.8.1 Durability, recovery and scale (Ph3, ED-20)

Unpublished edit-instance work can be days of a team's output, and before publish it exists nowhere else.
The journal therefore gets the same treatment as player data: replicated, mirrored off the bus, restorable
to any point, and drilled.

- **Where a session runs.** Tier C sessions run on the studio's collab deployment: 05 §5's Compose kit
  with the `ha` profile (3 NATS nodes, MinIO) or the studio's Kubernetes backend (05 §6.1). There the
  `COLLAB_<session>` stream and the `LOCKS_`/`NOTES_`/`HOMES_` buckets are **R3**, for the data session
  (§1.8.2) as for zone sessions. The all-in-one `helios-backend`
  (embedded NATS, R1) may host a session only with `--collab-archive <path|s3-url>` set, and every
  participant's editor then shows an amber "not replicated (R1)" banner. PIE's auto-started backend
  hosts no Tier C sessions.
- **Three copies of the journal:**

  | Copy | Contents | Written | Store |
  |---|---|---|---|
  | JetStream stream (R3) | Every `.tx` and `.ctl` message | On accept (the source of truth) | NATS file store |
  | **Journal segments** | The stream packed into `<firstSeq>-<lastSeq>.hjs.zst` segments. Each segment carries the BLAKE2b-256 of its predecessor, so a gap or edit breaks the chain | Every 30 s or 4 MB, whichever comes first, by an ordered consumer in the session's owning replica | Object storage: `collab/<project>/<session>/journal/`, a versioned bucket with 30-day object lock |
  | **Snapshots** | A manifest `{seq, baseCommit, streamSeq, doc → (rev, hash), tx-ID index hash}` plus content-addressed canonical JSONC for each document changed since the previous snapshot | Every 1,000 transactions or 5 min, whichever comes first, and at every rebase and publish. A copy-on-write revision index means the actor pauses ≤ 5 ms | Object storage: `collab/<project>/<session>/snap/` and `…/docs/<hash>.jsonc.zst` |
  | **WIP git ref** | The snapshot as one commit on base B (`Helios-Tx: 1..<seq>` trailer). LFS pointers resolve, because `Blob` bytes are uploaded before submit (§1.8) | Every 10 min while the session changes, and at pause and end | `refs/helios/collab/<session>/wip` on the studio git server. It is a hidden ref, deleted 30 days after the publish merges |

- **Idempotent submits.** The session's tx-ID → sequence index is part of every snapshot. A resubmitted
  transaction ID returns its original result, and `Nats-Msg-Id` deduplicates at the stream as well
  (10-minute window). Editors keep unacknowledged submits in their crash journal (§1.2) with state
  `pending` and resubmit them after a reconnect, a failover or an editor restart. They also keep their
  own accepted transactions there until an `archived{seq}` event on `.ctl` shows them in object storage,
  which is what makes tail recovery possible.
- **Failover.** Session actors are placed on collab replicas by a PG lease row,
  `collab_owner(session, term, holder, expires_at)`, with 05 §1.4.1's term-fenced pattern: a 6 s expiry,
  renewal every 1 s, and no action more than 4 s after the last renewal. A new owner loads the newest
  snapshot's index and replays the stream tail of ≤ 1,000 transactions, so a takeover completes in ≤ 8 s.
  Editors queue submits meanwhile, and nothing is lost. `Nats-Expected-Last-Sequence` fences a deposed
  owner that is still publishing.
- **Recovery objectives:**

  | Failure | Accepted transactions lost (RPO) | Recovery (RTO) | Mechanism |
  |---|---|---|---|
  | An editor crashes | 0 | Restart plus late join ≤ 10 s | Crash journal resubmits `pending` transactions |
  | A collab replica crashes or is partitioned | 0 | ≤ 8 s | PG lease takeover; the stream is the truth; idempotent resubmit |
  | One NATS node is lost | 0 | ≤ 5 s (stream leader election) | R3 |
  | The whole NATS cluster is lost, or the stream is deleted | 0 for every transaction whose author's editor is still running or restarts with its journal; otherwise ≤ 30 s | ≤ 10 min for any session length (only the ≤ 1,000 transactions after the newest snapshot are republished) | Snapshot plus segments, then **tail recovery** from participants' journals |
  | Object storage is lost as well | As above, but ≤ 10 min for transactions no journal holds | ≤ 30 min | WIP git ref, then tail recovery |
  | A destructive edit (a bad bulk replace) | None: a point-in-time restore | ≤ 10 min | `restore --to-seq` |

- **Restore.** `helios-tool collab restore <session> [--to-seq N] [--from s3|git]`:
  1. Pause the session: editors go read-only and show "restoring".
  2. Pick the newest snapshot at or before the target and verify the segment hash chain from it.
  3. Recreate `COLLAB_<session>` with JetStream's `FirstSeq` = snapshot sequence + 1, so stream and session
     sequences stay equal. Republish the segments' messages in order, up to the target.
  4. Without `--to-seq`, ask every connected editor for journal entries above the restored head, and accept
     them in their original sequence order through the normal submit path. Each editor then lists any
     transaction of its own that no longer applies.
  5. Publish `restored{seq, snapshot}` on `.ctl`. Edit-instance cells hot-swap to the restored overlay and
     editors late-join.
- **Restore drill.** A nightly CI test restores a 2,000-transaction session after deleting its stream and
  compares document hashes (part of ED-20). Each studio deployment also runs a quarterly drill from the
  runbook `deploy/runbooks/collab-restore.md` against a copy of a real session. The drill restores from
  object storage alone and from the WIP ref alone, and its report goes to the next phase exit.
- **Throughput.** One actor serves one session: a zone's, or the project's data session (§1.8.2). It is
  sized for **≥ 500 transactions per second
  sustained** (median transaction ≤ 20 ops and ≤ 8 KB) on one core of a DEV-class host:
  - schema-range checks run on a worker pool ahead of the actor, so the actor does only revision,
    lock and cross-document reference checks against its in-memory index;
  - JetStream publishes are pipelined up to 64 in flight. Each message's expected last sequence is known
    in advance, and an editor is acknowledged only after the stream acknowledges its message. A failed
    publish fails its successors too; the actor then reverts its speculative index to the last acknowledged
    sequence and re-runs the queued submits;
  - gesture previews bypass the stream (`.preview`), so a 60 Hz drag stores one transaction, not 60.

  Targets: submit → acknowledgement ≤ 50 ms p99 at 50 transactions/s and ≤ 250 ms p99 at 500/s;
  propagation ≤ 1 s p95 (AAA-ITR-7) with 25 editors in one session. Above 40 editors writing in the last
  10 minutes a session shows a warning, and the next scale step is more zones, not more editors per zone.
  Global documents do not count against a zone, because they are homed in the data session unless checked
  out (§1.8.2).
- **Stream bounds.** `max_bytes` is 20 GB per session and `max_msg_size` 1 MB (larger transactions become
  groups, §1.8). After each rebase or publish, messages older than 24 h that precede the newest snapshot are
  purged from the stream. Their segments stay in object storage for 90 days, which also covers the 24 h
  "delete beats edit" recovery.

#### 1.8.2 Session scope: zone sessions, the data session and document homes (Ph2 locks, Ph3 transactions; ED-20)

A zone's edit instance is the natural session for placed content, but most of an MMO's data has no spatial
home: items, abilities, loot tables, NPC and faction records, quests, dialogue, graphs and string tables. If
such a document belonged to whichever zone session touched it, two zone sessions could each accept a
conflicting edit to one loot table within a second, and soft locks in two `LOCKS_<zone>` buckets would not
see each other. The clash would surface only later, as a 3-way merge at rebase or publish. Helios therefore
gives every document **exactly one home session at a time**, and makes locks and presence for global
documents project-wide.

**Sessions.** A project has one data session and one session per zone edit instance, per target branch
(`main`; a `release/*` branch has sessions only while a hotfix session is open, AAA-ITR-8):

| Session | ID | Homes | Participants | Phase |
|---|---|---|---|---|
| **Data session** (one per project) | `data` | Every global document that is not checked out; pinned documents always | Every editor of the project, joined automatically at project open (routing, presence and locks for global documents); `helios-tool` and the web tools (§1.9) | Ph2: project-wide soft locks and presence (Tier B). Ph3: transaction stream |
| **Zone session** (one per edit instance) | `zone-<zoneId>` | The zone's spatial documents, always; global documents checked out to it | Editors that open the zone | Ph3 (Tier C) |

Both kinds are ordinary sessions for §1.8 and §1.8.1. Each has one actor, its own `COLLAB_<session>` stream,
snapshots, journal segments, WIP ref and `collab_owner` lease, and the same recovery objectives. An editor's
crash journal keeps one file per joined session (§1.2).

**Document classes.** Each document type declares its class (`DocumentTypeDesc.scope`, §1.10; `global` by
default). T28's `collab.scope` rule (Error) checks that every spatial document sits under its zone's
`content/zones/<zone>/` folder and that no other document does:

| Class | Documents | Home |
|---|---|---|
| **Spatial** | Everything under `content/zones/<zone>/`: object containers and entities, terrain tiles and layer stacks, scatter overrides, `ZonePartitionDef` and portal graphs | Always `zone-<zoneId>`. Never moves |
| **Global** | Every other document under `content/`: records (items, abilities, loot and plug tables, NPC, faction, vehicle and schematic records), graphs, quests, dialogue, string tables, prefabs, materials, sequences, UI documents and `.meta` settings | `data` by default, or the one zone session that has checked it out |
| **Global, pinned** | Project schema packages (`.hschema`, 02 §3.8), `.htags`, `PhysicsLayersDef` and project settings | Always `data`. They change validation in every session, so they never move |

**Routing.** Editors mirror the home map (below) and send each transaction to the one session that homes
every document it touches:
- **One home that the user has joined** (the data session, or a zone the user has open). The transaction goes
  there, with no checkout. A level designer in Saltmarch who tunes a loot
  weight submits to `data`, exactly as a systems designer in T08 or a writer in the Writers' Room does.
- **Zone Z's spatial documents plus global documents homed in `data`.** The editor asks Z's actor to **check
  out** those global documents, all or none, and then submits to Z. Typical cases: placing a spawner together
  with its new NPC record, wiring a quest step to a trigger volume, and prefab apply-to-base (the prefab moves
  to Z; instances in other zones inherit the change and need no edit).
- **Spatial documents of two zones.** Rejected with `crossZone`. A move between zones is a paste in one
  session and a delete in the other.
- **A global document held by a zone session the user has not joined.** It is read-only for that user, as in
  the web tools (§1.9), and the editor does not send the transaction. The UI shows the holder and offers *Ask to return* (a notification in the holder's session) or, for leads, *Force return*,
  which is logged like a soft-lock steal.
- **At the actor.** A transaction that touches any document the receiving session does not home is rejected
  with `moved{doc, session}` or `inTransit{doc}`, never accepted. The editor re-routes and resubmits it.

A new global document created in a zone session (the NPC record above) first reserves its path and GUID with
the data actor (≤ 20 ms), so two sessions can never create the same path. It is homed in the creating
session until it is checked in.

**Handoff protocol.** Every home change goes through the data actor, so `COLLAB_data` gives all of them one
global order. In each direction the releasing session's event is the commit point, and the adopting session
adopts idempotently by event ID:

| Step | Checkout (`data` → Z) | Check-in (Z → `data`) |
|---|---|---|
| 1. Request | Z's actor asks the data actor for document set D | Explicit, automatic (below) or *Force return* |
| 2. Checks, all or none | Every d ∈ D is homed in `data` and not in transit; no other user holds a soft lock on it; no open transaction group touches it | No open transaction group in Z touches D |
| 3. Commit point | The data actor publishes `handoff{id, to: Z, docs[{d, rev, hash}]}` on `collab.data.ctl` and rejects writes to D from then on | Z's actor publishes `release{id, docs[{d, rev, hash, content}]}` on its `.ctl` and rejects writes to D from then on |
| 4. Adopt | Z's actor publishes `adopt{handoffId, dataSeq, docs[{d, rev, hash, content}]}` and accepts writes to D from then on | The data actor publishes `adopt{releaseId, zoneSeq, docs[…]}` and accepts writes to D from then on |
| Recovery | A new owner of Z scans `collab.data.ctl` from its snapshot's `dataSeq` and adopts every handoff to Z that has no adopt | A new owner of `data` scans each zone's `.ctl` from its snapshot and adopts every release that has no adopt |

- An adopt carries each document's canonical content, so every session's journal is self-contained for
  restore and export. Adopts over 1 MB become transaction groups (§1.8).
- Between commit point and adopt a document is **in transit**: submits get `inTransit`, and editors retry.
  Request → adopt is ≤ 500 ms p95 and ≤ 2 s p99, and ≤ 10 s when an actor takeover (§1.8.1) lands in between.
- Zone to zone is a check-in followed by a checkout in one data-actor step.
- **Automatic check-in.** Checked-out documents return to `data` when Z publishes them, after 30 min with no
  write from Z and no lock in Z covering them, when *Force return* runs, and before Z's session ends. A session
  cannot end while it holds global documents.
- **Authority.** Each actor decides from its own stream, whose single writer is fenced by `collab_owner` and
  `Nats-Expected-Last-Sequence` (§1.8.1). The KV bucket `HOMES_data` (document → `{session, epoch, state}`) is
  a read-only mirror that the data actor writes for routing and badges. It decides nothing, so no ownership
  state lives in NATS KV (05 §0 rule 8).
- **Point-in-time restore.** `collab restore --to-seq` (§1.8.1) refuses a target earlier than the session's
  last handoff, release or adopt event, and names those events. To go back further, the documents are first
  checked back in. A full restore replays the events unchanged.

**Locks and presence.** Soft locks on global documents are **project-wide**. They live in `LOCKS_data`, the
data actor grants them whichever session homes the document, and every editor watches that bucket. The home
actor honours them: another user's submit gets `locked{user}`. A checkout never moves a document that another
user has locked. Region locks and soft locks on spatial documents stay in `LOCKS_zone-<zoneId>`. Presence on a
global document (open, selected, field being edited) goes on `collab.data.presence` at 2 Hz, whatever session
the user is in; spatial presence (camera, viewport selection) stays on the zone's channel at 5 Hz. The T30
panel lists the holders of global documents across all sessions.

**Causal order across streams.**
- A transaction that reads or references a document homed in another session carries
  `deps{session → seq}`: the other streams' sequences its author had applied. The home actor validates
  cross-session references against its read replica of those streams at or after `deps`. It waits ≤ 1 s for
  the replica to catch up, otherwise it answers `retry`.
- Editors and edit-instance cells hold a transaction until its `deps` are applied, and apply each stream at
  tick boundaries. A schema transaction in `data` is a barrier: a zone actor validates submits whose
  `deps.data` is at or after it with the new types.
- Because each document has one home at a time, the overlays compose without merging. An edit instance runs
  base + the data overlay + its zone overlay. A late joiner loads and replays both streams in parallel within
  §1.8's ≤ 10 s, and a publish preview pins both journal ranges.
- Deleting a global document that another session's overlay references is refused, with the referrers listed
  from the project reference index (§3.4), unless the user confirms *delete anyway*. T28's `ref.dangling` then
  flags the referrers in ≤ 2 s and blocks their publish.

**Publishing across sessions.** Each session still publishes its own linearized branch and PR (§1.8). Two
rules make sure a document's history is never merged against itself:
1. **Carry publishes.** Suppose a published document was adopted from session P, and P's writes to it are not
   yet on `main`. Or suppose a published transaction's `deps` point at unpublished transactions of P. Then the
   service first opens a **carry**: P's dependency-closed partial publish (§1.8), ending at the handoff or at
   the depended-on sequence, on `collab/<P>-carry-<n>`. Carries go ahead of the dependent PR in the content
   queue (09 §5.2a), and they recurse. They terminate, because each step moves strictly earlier in
   `COLLAB_data`'s order of home changes or in P's own sequence.
2. **Per-document merge base.** At a rebase (§1.8), the merge base of a document the session adopted is its
   adopt content, not the session's base commit B. Once the carries have landed, `main` holds exactly that
   content, so the session's writes apply with no merge. A document that the session released, and whose
   writes in this session are all published, takes `main` as it is.

A 3-way merge on a global document therefore happens only when a commit reached `main` outside collab (a
text-editor edit), never between two sessions. §1.8's promise holds per session: in each exported commit, the
session's own documents are exactly as its edit instance had them, and every document from another session
that they depend on is already on `main` through a carry, so CI's per-commit T28 run passes.

**Scale.** The data actor is the busiest session. It carries every systems designer, writer and localizer,
plus zone users' record edits, within §1.8.1's ≥ 500 transactions/s. ED-20 runs it at 30 transactions/s
beside two zone sessions. 200 participants at 2 Hz add 400 presence messages/s on core NATS. §1.8.1's
40-editor warning counts writers active in the last 10 minutes, not participants.

### 1.9 Web tools for narrative and localization (R03 §8.1)

Writers, localizers and VO coordinators should never need the 3D editor. From Ph3 the collab service
hosts **Writers' Room** and **Loc Review**, on 05's admin-console stack (Go templates + htmx, no Node
toolchain) plus a vendored prebuilt CodeMirror 6 bundle (MIT):
- a **screenplay view** of dialogue that round-trips with the T13 graph;
- **VO status boards**, recording-script export, per-line comments;
- **translation** with context, screenshots, length limits and MessageFormat 2 preview (T25).

Edits are ordinary `collab.submit` transactions, and the web tools submit them to the project's **data
session** (§1.8.2): dialogue, quests and string tables are global documents, so they have no zone. A document
that a zone session has checked out opens read-only, showing its holder, with *Ask to return*. Presence and
soft locks are the project-wide ones in `LOCKS_data` and `collab.data.presence`. A writer in the screenplay
view and a level designer staging the same conversation in a zone therefore see each other, and neither can
overwrite the other. Compile and validation run on headless `helios-tool` workers; graphs render as
read-only SVG. Structural graph editing, staging and cinematics stay native.

### 1.10 Editor extension SDK (R08 §4 item 9; Ph2 preview, Ph3 stable)

Every AAA studio builds tools the engine did not foresee: a faction-territory painter, a proprietary
mocap importer, a mission-balance dashboard, a cook step that signs its own data. The north star (01 §1.1)
says a studio never edits engine source, so the editor's extension points are a **public API** with the same
documentation, deprecation and upgrade guarantees as the Luau and game-module APIs (09 §2.7.2).

**Packaging.** An editor extension is part of a gem (02 §1.2). `gem.jsonc` declares its editor modules and
scripts:

```jsonc
"modules": [
  { "name": "survey_edcore", "kind": "editor-core", "reloadable": true },  // UI-less
  { "name": "survey_edui",   "kind": "editor-ui",   "reloadable": true }   // panels, modes, customizers
],
"editor": {
  "api": "^1.2",                               // HELIOS_EDITOR_API_VERSION range
  "scripts": "editor/**/*.luau",               // Luau extensions (sandboxed editor VM, §1.2)
  "capabilities": ["assets.import", "pie.control"]   // shown when the gem is enabled
}
```

- **`editor-core` modules** hold commands, document types, importers, builders and cook steps, T28 rules,
  node libraries, PIE hooks, settings, search providers and remote-control verbs. The editor, `helios-tool` and `helios-assetd` all load them, so a
  studio's importers and rules run the same way in CI (T29) as on a desk. They may not include
  `helios/editor/ui/` headers; an include lint enforces this.
- **`editor-ui` modules** hold panels, viewport modes, gizmos and customizers, and only the editor loads them.
- Both kinds are `EDITOR_ONLY` (02 §1.1): configure fails if a client, cell or shipping target links one.
  They build as `game_<gem>_edcore` and `game_<gem>_edui` DLLs or `.so`s against the dev link groups
  (ADR-016). The SDK's `helios-editor`, `helios-tool`, `helios-assetd` and `helios-cook` are dev-flavour
  builds for this reason, so they load a project's editor modules without engine source (09 §2.7.2).

**Extension points.** All are registered through the module's `EditorModuleContext`. Every registration
lands in the module's `ModuleRegistrationScope` (02 §1.4), so an unload removes it completely.

| Extension point | C++ (`include/helios/editor/`) | Luau | Loaded in | What it gets for free |
|---|---|---|---|---|
| **Commands** | `ctx.commands.add(CommandDesc{id, argsSchema, canExecute, execute → Transaction, placements})`. Placements are menu paths, toolbars, context menus per asset or record type, and a default shortcut | `Editor.registerCommand` | Editor, `helios-tool` | Palette, rebindable keys, remote control, `helios-tool run`, UI lint coverage (§4.4) |
| **Document types** | `ctx.documents.add(DocumentTypeDesc{extension, schema or codec, icon, openWith, merge, scope})`. A schema-backed JSONC document uses the canonical writer and property-path merge; a custom binary document is stored as `Blob` ops in LFS. `scope` is `spatial`, `global` (the default) or `pinned`, and picks the document's collab home (§1.8.2) | — | Editor, `helios-tool` | Undo, crash journal, collab, `merge=helios`, T28, search, dependency graph |
| **Dock panels and whole tools** | `ctx.ui.addPanel(PanelDesc{id, title, defaultDock, roles, draw(Ui&, PanelState&)})`; `ctx.ui.addTool(ToolDesc{id, layout, panels, modes})` with a layout row like §2's table | `EditorUI.panel{…}` | Editor | Docking, tear-off, role layouts, themes, F1 help, goldens and lints |
| **Viewport modes, overlays and gizmos** | `IViewportMode{activate, deactivate, onInput(ViewportInput&), drawOverlay(OverlayDraw&), toolbar(Ui&)}`; handles (point, axis, plane, radius, rotation) that report drags as f64 `WorldPos` deltas (§1.5); view modes as a debug-draw overlay or a Slang post pass over the ID and G-buffers (`ViewModeDesc{slangModule}`) | Overlays and handles | Editor | Snapping, camera-relative precision, picking, undo of drags |
| **Inspectors and customizers** | `ctx.inspector.addCustomizer(typeOrField, ICustomizer{draw(Ui&, PropertyView, TxBuilder&)})`, bound by `@editor(customizer="<gem>.<id>")` in `.hschema`; extra Details sections per component | `EditorUI.customizer` | Editor | Multi-edit, override markers, mixed values |
| **Asset importers** | `IImporter{extensions, settingsSchema, version, import(ImportContext&) → products, dependencies}` | Text formats only (CSV, JSON, XML → records) | assetd, `helios-tool` | `.meta` sidecars, GUIDs, provenance, reimport on change, DCC live links |
| **Builders, bakers and cook steps** | `IBuilder{inputs, outputs, platforms, version, build(BuildContext&)}`, whose declared inputs feed the DDC key (§3.2); `ICookStep{stage: preCook, postCook or package; run(CookContext&)}` | — | assetd, `helios-cook`, `helios-tool` | DDC caching, shared cache, distributed cook workers |
| **T28 rules and quick fixes** | `RuleDesc` (§3.6) | Yes | All three | Issue browser, SARIF, suppressions |
| **Graph node libraries and families** | `NodeLibraryDesc{family, nodes[{guid, pins, typeRules, emit(CompileContext&)}]}` for any §2 T11 family; `GraphFamilyDesc{family, validator, emitter}` for a new family that compiles to Luau or records | Nodes that emit Luau calls | Editor, `helios-tool` | Graph editor, type checker, diff, merge, breakpoints |
| **PIE hooks** | `PieHooks{configure(PieConfig&), onStarted, onClientSpawned, onSeamError, onStop}` | Yes | Editor, `helios-tool pie` | The same hooks in headless PIE |
| **Remote control, search, settings, asset browser** | `ctx.rpc.add("<gem>.<verb>", paramsSchema, handler)` for read-only queries (mutations must be commands); `ctx.search.addProvider` for Ctrl+P; `ctx.settings.add<T>()` for a project or user settings page from a schema struct; thumbnail renderers and preview panels per asset type | Verbs and search providers | Editor | JSON-RPC, DCC plug-ins, `helios://` links |

**Rules.**
- **Transactions only.** Extension code reads documents through `DocView`s, and `TxBuilder` is its only way
  to write. ED-6's direct-write detector covers extension modules. Each transaction records the gem that
  registered the command (`via: <gem>`) next to its `origin`, so the provenance report (01 AAA-TOOL-10) and
  crash triage can attribute it.
- **No third-party types.** `Ui` is a Helios immediate-mode façade over ImGui, not ImGui itself (02 §1.1's
  public-header rule), so ImGui's API breaks, such as 1.92's font rework, never break a studio's plug-in. It
  covers layout (rows, columns, tables, splitters, tabs, trees), the basic widgets, every §1.4 shared widget
  (property grid, typed pickers, node-graph host, timeline and curves, colour, charts, data grid, code pane)
  and a `Canvas`. The `Canvas` has a Helios draw list (lines, polylines, rectangles, circles, text, images
  from an `AssetRef` or a render target, clipping, hit tests) for custom widgets. Every interactive call
  takes a label, so §4.4's item table, test paths and lints work on extension panels unchanged.
- **Threading.** UI callbacks and `execute` run on the UI thread. Long work goes through `ctx.jobs` with a
  progress and cancel token, following the non-modal rule (§4.2). Importers and builders run on assetd's
  pools and must be pure functions of their declared inputs. A nightly `helios-assetd --verify-determinism`
  rebuilds 1 % of each builder's products and compares hashes.
- **Budgets.** A panel's `draw` ≤ 1 ms p95, shown per panel in T26; the nightly UI run raises T28's
  `ext.panel.slow` above it. Loading an `editor-core` module ≤ 200 ms.
- **Fault containment.** C++ modules are trusted in-process code. The crash handler attributes a fault to
  the module whose image holds the faulting address, and Sentry tags `gem`, `module` and version. After two
  crashes from one module in 24 h, the next launch offers "Start with `<gem>`'s editor modules disabled".
  Importers and builders run in assetd, a separate process: a crash there fails that job with the module
  named, assetd restarts in ≤ 2 s, and the job stays quarantined until the module's version changes.
- **Luau sandbox.** Luau extensions run in the editor VM (§1.2): no file or network access beyond the
  document API and the declared capabilities. Each Luau panel gets 2 ms of fuel per frame and all of them
  4 ms together. A panel over budget for 30 consecutive frames is suspended with a *Resume* button and a
  T26 entry.

**Luau panels.** The `EditorUI` binding exposes the same `Ui` façade to the editor VM, typed by the
generated `.d.luau` (realm `editor`), and reloads in ≤ 2 s:

```lua
EditorUI.panel({ id = "survey.heatmap", title = "Survey heatmap", dock = "right", roles = { "level" } },
  function(ui, state)
    local zone = Editor.activeZone()
    if ui.button("Rebuild##heat") then Editor.cmd("survey.rebuildHeatmap", { zone = zone }) end
    ui.plotHeatmap("density", Survey.sample(zone, 64))
    for _, node in ui.table("nodes", Editor.find("type:ResourceNodeDef zone:" .. zone)) do
      ui.property(node, "yield")          -- an edit here is one transaction, merged while dragging
    end
  end)
```

**Hot reload.** C++ editor modules reload through ADR-016's protocol (02 §1.4) in the editor and in assetd.
Open documents and undo history belong to ToolsFramework and are untouched. Open panels keep their
`PanelState` through `HELIOS_RELOAD_STATE`, and a viewport mode that was active is reactivated. A
one-`.cpp` edit reaches a reloaded panel in ≤ 30 s p95 (the AAA-ITR-5 path). A changed importer or builder
`version` invalidates only that builder's DDC products. If `engineBuildId` or the editor API's major version changed, the module is
refused and the editor asks for a restart.

**Versioning, documentation and upgrades.**
- **Version.** `HELIOS_EDITOR_API_VERSION` is a major.minor pair of its own: `0.x` (experimental, Ph2) and
  `1.0` at the Ph3 exit. The loader checks `gem.jsonc`'s `editor.api` range and refuses a mismatch with a
  message naming the SDK the gem needs.
- **Public surface.** It is part of 09 §2.7.2's public API: `include/helios/editor/`, the Luau `editor`
  realm (`Editor`, `EditorUI`, `Validate`, `UiTest`), the `gem.jsonc` editor block and the command-ID
  namespace. From `1.0` the deprecation policy applies unchanged: `[[deprecated]]` shims in C++,
  `@deprecated` in `.d.luau`, removal no earlier than two minor releases later, and a migration step. The
  Luau codemods of `upgrade-project` cover the `editor` realm.
- **Docs.** `helios-docs cpp` covers `include/helios/editor/`, and the PR-tier coverage gate includes it from
  Ph3 (AAA-TOOL-7). The manual gains "Extending the editor", one page per extension point, and two
  executable tutorials: *A Luau panel and command* (Ph2) and *A C++ importer, viewport mode and node library*
  (Ph3).
- **Upgrades.** The `sample-editor-ext` gem joins `tests/upgrade/<N>/` from Ph3, so every release candidate's
  `upgrade-test` moves it N → N+1 with no manual edit (09 §2.7.3), and N−2 → N from Ph4.
- **Templates.** Starter templates stay free of C++ but may ship Luau editor extensions. `starter-sandbox`
  ships a *Resource survey* panel and command, and its `template-proof` job opens the panel through
  `helios-uitest` (09 §2.7.4), so a template proof exercises the extension API from a clean New Project.
- **Dogfooding.** The `edtools` targets of T06 Splines, T20 Audio and T25 Localization may include only
  `include/helios/editor/` and register only through `EditorModuleContext` (the `edtools-public-only`
  include lint). The API is therefore proven on three real tools, not only on the sample.

**Phasing.** Ph1: Luau commands, menu items and T28 rules. Ph2: `EditorUI` Luau panels and customizers, Luau
importers and node libraries, and the C++ API as `0.x` under `include/helios/editor/experimental/`
(documented, outside the deprecation policy); the Luau half of ED-19. Ph3: API `1.0`, the three dogfooded
tools and ED-19. Ph4: N−2 → N upgrades of the sample gem.

---

## 2. The tool suite T01–T30

**Common contract.** Unless a tool says otherwise:
- it writes only through transactions and exposes its actions as commands (AAA-TOOL-6);
- save → assetd → PIE hot reload ≤ 2 s (AAA-ITR-1);
- derived data that its edits invalidate (nav tiles, HLOD, impostors, probes, scatter caches) is rebuilt
  within §4.1.1's budgets, with a marked stale product shown meanwhile;
- T28 rules run on save and in CI;
- T30 presence and locks apply, project-wide for global documents such as records, graphs, quests and
  dialogue, which live in the data session (§1.8.2);
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
| T12 Quest | Quests, Objectives, Activities | Stage graph / encounter graph | Details, View-as | Validator, Funnel, Queue sim |
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
- **Volumes mode** (§2.6.3–2.6.5): portal cells and portals auto-generated from collision (T03
  blockout or imported kits) and then edited; pressurized, roofed, airlock and window flags; door
  binding; a `GridVolume` editor with a hysteresis visualizer and transfer probe; the **Partition
  tool** that writes each zone's `ZonePartitionDef`; and one list of every other volume kind
  (trigger, post, reverb, weather shelter, nav blocker).
- *Data → consumer:* object-container folder = `.hcont` manifest (frame, bounds, data layers) + one
  `.hent` JSONC per entity (OFPA; format owned by 02) → container blobs split client/cell, streamed
  by both. `PortalCell`/`Portal` entities cook into the container's `PortalGraph` (02 §5.1);
  `ZonePartitionDef` records go to the orchestrator and cells (02 §5.5). *Integrations:* placement
  applies live in PIE; per-cell budget rules.
- **MVP (Ph1):** one zone with its frame hierarchy (Tallis → Harrow → Saltmarch), gizmos, snapping,
  outliner, multi-edit, undo, OFPA save, editor layers, bookmarks; Volumes mode for cells, portals
  and grid volumes, and `Whole` partitions (ED-16, Harrow High).
- **AAA (Ph3):** planet-scale region editing, data layers with "view as" preview, HLOD builds, notes
  ↔ issue tracker, per-cell budget heatmap, bulk operations across containers; multi-cell zone
  partitions with load preview (Harrow orbit, M3).

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
  collision generation with a `CollisionProfileDef` per mesh and a `PhysicalMaterialDef` per face
  (§2.6); glTF export for the DCC round trip, keeping the blockout as collision proxy when art
  replaces it. A blockout interior feeds T01's portal-cell generation directly (§2.6.3).
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
    layers, flora density, memory, and the collision checks of 02 §5.8 and §5.8a: the interpolation bound
    at the collision spacing and the graph's `slabMargin`).
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
- *Features:* schema browser, read-only for engine and Foundation packages; a **governed schema editor**
  for project packages (HeroEngine's DOM Editor model, R03 §2.4; 02 §3.8). It creates and edits record
  types, structs, enums, `ScriptState` and other components, events and view-models in dynamic packages,
  with no compiler. Every edit is a ToolsFramework transaction on the `.hschema` file under a T30 lock (a
  pinned document of the data session, so the lock is project-wide, §1.8.2). The
  migration preview shows added, removed, `@was`-renamed and widened fields against live data, and it flags
  client-visible changes as compat-epoch changes. Lint errors (SEC-1, the `schema.dynamic-hot` budget,
  native-only constructs) block the save. A change reaches `main` only through the publish PR (§1.8), where
  CI reruns schemac's `--check-lock` and lint and the project's reviewers approve it. Saved changes are
  live in PIE in ≤ 5 s.
- *`worldscript` blocks (Ph3; 02 §3.8 allows them in dynamic packages; 05 §1.23):* the schema editor also
  edits world-script declarations as forms:
  - the script header (`@version`, `@partitions`) and its escrows;
  - tables: fields, `@key`, `@partitionKey`, up to 4 `@index`es, `@escrowBacked(field, when: …)` with a
    condition builder, `@ttl`, `@maxRows`, `@subject` and `@currency`;
  - RPCs: request and reply records, `@callers`, `@rate`, `@partitionBy`, `@readOnly` and `@cacheSecs`;
  - event subscriptions, picked from 05's subscribable list, and timers;
  - reason codes: `EscrowRelease`, `Sink`, or a faucet with a `dailyCap`.

  Adding an RPC, event, timer or `migrate` step generates a typed Luau handler stub under
  `scripts/world/<name>/` and opens it in T10's code pane. A quota meter shows tables, indexes, worst-case row
  bytes and partitions against 05's quotas. The `ws.*` rules (§1.6.2), including the quota and escrow-backing
  checks, block the save. The migration preview classifies each table change as *hot*, *migrate* or *refused*
  (§1.6.2's hot-reload table) and counts the rows it affects in the PIE store. A *migrate* change opens its
  handler stub and offers the World scripts panel's dry run. Block edits are pinned data-session transactions
  (§1.8.2), like every `.hschema` edit.
- *Records:* a record inspector with the record-template inheritance tree and override
  markers; **spreadsheet grid** (filter, sort, fill-down, bulk edit, HXL column formulas such as
  `damage = base * 1.1`, CSV/TSV round trip); where used; record diff/merge; per-field visibility
  badges (`client`, `server_only`, service); `.htags` editor with hot-set meter (≤ 1,024, 06 §1.1);
  schema-migration runner with preview diff; physics customizers (§2.6.1): the `PhysicsLayersDef`
  collision matrix as a symmetric checkbox grid with "why does A hit B" tracing, and
  `PhysicalMaterialDef` with an impact-cue and footstep audition button.
- *Stable IDs:* record hash IDs are minted once at creation and stored in the file, so renames never
  change them and ledger item instances never dangle; a T28 rule rejects reuse and collisions.
- *Data → consumer:* one `.hrec` JSONC per record → record DBs per consumer (client, cell, Go);
  client cooks strip `server_only` (AAA-SEC-4).
- **MVP (Ph1):** inspector, grid, inheritance, validation, where used, CSV. **Ph2:** the governed schema
  editor (ED-22).
- **AAA (Ph3):** 100k-record tables at 60 fps, balance diffs between builds, approval workflow, XLSX
  exchange, `worldscript` blocks (ED-23); live tuning through T27 hotfixes in Ph4.

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
- *Account progression* (06 §5.2–5.3): focused editors for achievements, collections, codex and legacy
  graphs, and a season-track editor with a calendar-window preview and an XP-per-hour to rank
  simulator (catch-up curve, weekly caps). Crew-mission tables (06 §7.3) use the drop-rate simulator.
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
  conditional), stepping, watches and coroutine lists over TCP from editor, client and cell VMs, and from
  each world-script partition VM (§1.6.2).
  Visual Studio stays the C++ IDE (CMake presets, natvis). Hot reload: save → assetd type-checks
  (`Luau.Analysis`) and compiles → PIE VMs run `__reload(old)`, ≤ 2 s. Clickable file:line errors;
  capability-manifest checks at publish; per-module CPU/memory via interrupt sampling and
  `lua_setmemcat`.
- **MVP (Ph1):** DAP in every VM, LSP definitions, hot reload, error linking.
- **AAA (Ph3):** embedded code pane with LSP/DAP clients for designers, which is where world-script handlers
  are written from T08's stubs (ED-23); RBAC-gated, audited remote
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

**T12 Quest Editor** (R08-T12, ED-P1-02; G10, G12, G19, W07).
- *Features:* stage graph; objectives (Kill, Collect, Goto, Interact, Escort, Scan, Craft, Deliver,
  Survive, Custom); shared Conditions/Facts picker; rewards from T09 as ledger grants keyed by
  stage; world markers placed in T01; phase actions and data layers; group, raid and public-event
  credit; mission and site templates; `EventDef` graphs with contribution scoring; reachability
  validator; GM stage commands; **"view as state"** with party phase-sharing preview (§1.5,
  R03-P1-3).
- *Data → consumer:* `.hgraph` (quest family) → server-only `QuestDef`/`EventDef`/`EncounterDef`
  records and `ActivityDef`, `LockoutPolicyDef`, `MatchRulesDef` and `QueueDef` records (06 §6); clients
  get display data; the matchmaker reads `QueueDef` (05 §1.12).
- *Activity mode* (06 §6.6–6.13; G12, G19; R03-P1-8):
  - an **encounter graph** per `EncounterDef`: phases with enter conditions, objectives, spawn waves and
    mechanics; checkpoint flags; wipe and reset rules; revive policy. A "what is checkpointed" panel
    lists exactly the fields 06 §6.7 restores after a cell kill;
  - an **activity flow** editor (linear or graph) with key items, entry requirements and rewards;
  - **tier and modifier matrix**: rows are tiers, columns are target level or power, effects, forced
    modifiers, revive policy and loot; a sync preview shows any build's stats after sync or bolster;
  - **lockout editor**: scope, subject, reset and difficulty groups, with a timeline of what a character
    can still earn this period;
  - **match-rule editor** for `MatchRulesDef`: teams and spawns placed in T01, objectives, limits,
    rounds, respawn, mercy, overtime and tie-breaks. A bot scrimmage in PIE plays the rules, and the
    scoreboard's event-log fold is shown beside the live score;
  - **queue simulator**: runs the real Go matchmaker in process (`helios-backend`, 05 §1.12) against a
    synthetic arrival model (rate, party sizes, role mix, rating and latency distributions) or a recorded
    telemetry day. It charts wait p50/p95/p99 per role, rating spread, predicted win-probability spread
    and backfill fill time, and flags a `QueueDef` whose role supply cannot meet its demand. Its p95 must
    be within ±10 % of GP-14a's measured value;
  - validators: unreachable encounters, a `None` revive policy with no wipe condition, a lockout with no
    reset, and match rules with no reachable end state.
- **MVP (Ph2):** branching quests, objectives, rewards, validator, debug commands, view-as.
- **AAA (Ph3):** template-driven dynamic missions (R03-P2-2), world-event scheduler preview,
  per-stage funnels (§3.7), coverage bots; Activity mode with the queue simulator.

**T13 Dialogue Editor** (R08-T13, ED-P1-03; G11; R03-P0-11, R03-P1-1/2/4).
- *Features:* Line, Choice, Branch, Action and Cinematic nodes; speaker roles incl. `PlayerSpeaker`;
  group settings (resolver Roll/Leader/Vote/Owner, choice window); stable line IDs; **VO workflow**
  (written → locked → recorded → implemented; recording scripts per actor and language; placeholder
  VO via Windows speech synthesis, an external TTS command on Linux); a **simulator** with variable
  inspector and **N simulated players** for group rolls; screenplay view (§1.9); Yarn/ink/articy
  import; **coverage** (unreachable nodes, never-true conditions, missing VO or translations per
  species × gender × language) and playthrough bots (R03-P1-4).
- *Companions* (06 §7.3, R03-P1-7): per-choice `influenceDeltas` for each companion, shown as
  "approves" and "disapproves" in the simulator; companion conversations gated by influence rank and
  availability facts; bark sets.
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
  full-body IK; crowd animation LOD; **mount and rider sets** (06 §8.1a): gait clips whose mean root
  velocity and turn rate are extracted into `MountDef.gaits` at cook, cooked `RootMotionTrack`s for jumps and
  mount/dismount, and rider seated poses with rein and stirrup IK targets.
- **Physics Asset tab** (§2.6.2; R08's "cloth and physics preview"): hitbox capsules auto-fitted from
  skin weights, `HitZone.*` tagging, coverage and fairness checks, a preview of the server's ≤ 24-joint
  tick-rate sampling against the client pose, ragdoll bodies and joint limits with a drop test,
  physical materials per capsule, secondary-motion chains (Ph2) and cloth painting (Ph4).
- *Data → consumer:* `.meta` import settings + `.hanimgraph` → ozz archives and blend-tree data for
  client and cell (the cell samples hitboxes and root motion); `HitboxSetDef` → cell lag compensation
  and client hit prediction; `RagdollDef`, `SecondaryChainDef`, `ClothDef` → client only.
- **MVP:** Ph1 import and preview, with hitboxes auto-fitted by `helios-tool fit-hitboxes` and tuned
  in the T08 inspector; Ph2 compression, notifies, root motion, state machine, blend spaces, two-bone
  IK, and the Physics Asset tab (hitboxes, HitZones, ragdolls, secondary chains; ED-17).
- **AAA (Ph3):** retargeting, full-body IK, crowd LOD, powered-ragdoll hit reactions. Motion
  matching, FACS facial curves, VO lip-sync and cloth land in Ph4 with their runtime (02 §7.1–7.2)
  and T13 auto-staging (capability R04). **Ph5+:** learned motion matching.

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
  per-effect budgets** (particles, overdraw, GPU µs) for 2,000-ship battles (BENCH-3); shield, plume
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
- *Features:* RmlUi RML/RCSS with hot reload; **view models from `.hschema`** (native, or dynamic
  project view-models created in T08 with no C++, 02 §3.8) with mock-data fixtures and live PIE binding; binding inspector; diegetic preview on 3D surfaces;
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
  (06 §8.2); interior volume detection, which runs §2.6.3's portal-cell generation on the assembled
  hull and emits a *from cells* `GridVolume` (§2.6.4); seats, stations, damage sections, liveries,
  fleet HLOD; **ground vehicles** (06 §8.1a): wheel, pad, seat and saddle ports, torque and power curves,
  the gear chart, suspension natural frequencies, and an envelope preview that drives the cooked drivetrain
  on flat ground for `phys.vehicle.envelope`.
- *Data → consumer:* `.hassembly` part tree → prefab + ship record-template variant; port hierarchies
  become ledger locations on the cell (06 §8.4).
- **MVP (Ph2):** parts, ports, snapping, validation, stats (Kestrel variants, a housing kit); a speeder
  bike, a landspeeder and a rover with drivetrain tuning and the envelope preview.
- **AAA (Ph3):** multi-crew Mule, interior volumes, damage sections, shared player-builder library.
  **Ph5+:** shareable player blueprints (ED-P2-08).

**T22 Character Customization** (R08-T22, ED-P2-06; G15).
- *Features:* `SpeciesDef`/`CustomizationParamDef` editing (morphs, bone scales, palettes, texture
  layers, decals, mesh options, DNA blend); **bit-budget meter** for `CharacterAppearance` (≤ 600 B,
  06 §10); clothing layers and fit morphs per body type (digitigrade Keth); presets; an extreme-value
  stress grid rendering slider corners to catch clipping; facial-rig-safe ranges; crowd-bake
  settings: each outfit `MeshOption` gets a silhouette class (≤ 8 per body type), and `PaletteColor`
  parameters map to ≤ 6 impostor tint slots (skin, hair, 4 outfit). The bake produces 03 §7.6a's
  crowd-class impostor atlases, and the stress grid also runs through the runtime BC encoders. A
  per-tier vertex lint checks every species, body type and outfit combination against 03 §7.6a's
  maxima: hero ≤ 80k, A0 ≤ 40k, A1 ≤ 12k and A2 ≤ 5k vertices, and 50k/25k/8k/3k for the LODs MIN uses.
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
- *Followers* (06 §7.3): `FollowerDef` and command-set editing; a `CommandSelector` BT template;
  stance presets; leash, control-range and scoop-range gizmos; a drone-bandwidth and capacity check
  against sample owners; live debug of the command blackboard.
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
  licence**, 01 §5.2); reimport on change; **collision settings** per mesh: `CollisionProfileDef`,
  shape (`none`, `box`, `convex`, `decomposition(maxHulls ≤ 64)` via V-HACD 4, or `mesh`, statics
  only), physical material per material slot, previewed in the collision view mode (§1.5).
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
  budgets, 01 pillar 6); memory by asset type; Luau profiler; the **World scripts panel** for PIE and dev
  backends (§1.6.2: partitions, tables through their indexes, the invocation log with fuel, `ws_dead`, audited
  row edits and dev-data migrations), with the WSH listed under Processes.
- **MVP (Ph1):** Tracy, overlays, packet inspector.
- **AAA (Ph3):** unified cross-process timeline, automated nightly-soak captures with regression
  diffs, budget heatmaps, the World scripts panel (ED-23).

**T27 Live-Ops / GM Client & Admin** (R08-T27, ED-P1-15; G17; 05 §1.17; R02-P2-18 god client).
- *Split with 05:* the **web admin console** (05 §5, Go + htmx) owns accounts, ledger explorer,
  market, chat moderation, config, kill switches, content channels. The **native GM client** (editor
  in GM mode, or a client build with the GM role) owns in-world operations.
- *GM client:* MFA and GM connect token via the gateway; read-only entity inspection by default;
  audited GM commands with a reason (spawn, teleport, kick, modify runtime state; 05 §1.17);
  invisible observer; bug reporter filing position, entity states, log and screenshot with a
  `helios://` link; red live chrome (§1.3).
- *Hotfix pipeline* (R03-P1-11, AAA-ITR-8): data/Luau change → hotfix build → canary zone → staged →
  live, with rollback. World-script hot swaps take the same stages, counted in partitions (05 §1.23 item 9).
  The event scheduler toggles data layers from the calendar.
- *World scripts (Ph3):* §1.6.2's World scripts panel against a shard, read-only by default. Row edits,
  `ws_dead` replays and discards, and the `ws.<script>` kill switch are audited GM commands with a reason, and
  a shard migration's progress is shown read-only.
- *Case queue (Ph3):* the same queue as the web console's (05 §1.17), in a dockable panel. It filters by
  priority, category, state, shard, assignee and SLA due time. Each row shows the first-response and resolve
  due times, the time left and a breach badge, and rows sort by the nearest due time. The evidence viewer
  shows chat excerpts with their verified tags, plays voice clips, and links ledger references to the ledger
  explorer. One click runs *Go to subject* (the invisible observer at the reported character), *Claim*,
  *Reply* from a macro, or *Link sanction*. Every evidence view is audited.
- **MVP (Ph1):** GM console commands in client and PIE with audit log. **Ph3:** GM client on shards,
  observer, bug reporter, case queue, World scripts panel.
- **AAA (Ph4):** staged live-world edits with two-person approval, A/B configs, hotfix ≤ 15 min,
  telemetry heatmaps in the GM viewport, complete web admin.

**T28 Validation & Content QA** (R08-T28, ED-P0-14).
- *Features:* the rules engine (§3.6) on save, pre-commit, CI and nightly; **issue browser with quick
  fixes** (TrenchBroom model; double-click navigates to entity, record, node or line); budgets;
  reference integrity; missing localization; quest reachability, dialogue and navmesh coverage;
  **server-data leakage check on client cooks** (AAA-SEC-4); **Windows/Linux portability rules**
  (reference case must match disk, no case-only path collisions, paths ≤ 180 characters, no
  reserved names like `CON`/`AUX`); **physics, volume and partition rules** (`phys.*`, `zone.*`,
  §2.6.6), including interior leak detection with a leak path drawn in the viewport and hitbox
  coverage and fairness; **world-script rules** (`ws.*`, §1.6.2: quotas, escrow backing and flow,
  partitioning, index coverage, privacy, compatibility and migrations).
- **MVP (Ph1):** framework, ≥ 20 rules (including the Ph1 portal, grid-volume and hitbox rules), CI
  gate, issue browser.
- **AAA (Ph3):** ≥ 150 rules covering every asset class (required from Ph2, AAA-TOOL-6), auto-fix bot
  PRs, content-health dashboards.

**T29 Build / Cook / Deploy** (R08-T29, ED-P0-16).
- *Features:* `helios-assetd` (§3.1); **cook profiles** for client (win64, linux64), cell
  (collision, navmesh, gameplay data, no textures) and Go services (record tables); packaging into
  content-defined-chunked containers with signed manifests (05 §7, 08); recorded PSO lists
  (AAA-REN-4); "cook & launch standalone"; `helios-patch publish` to dev/staging channels; build
  dashboard; a **Package & publish** profile that stamps, signs and packages a branded client, launcher,
  Windows installer and Linux AppImage from the project's `product` block (08 §2.10.5).
- **MVP (Ph1):** local daemon, CI cook, client/cell/service split, manifests.
- **AAA (Ph3):** single-zone incremental cook ≤ 60 s (AAA-ITR-4, from Ph2), distributed cook workers
  on a NATS work queue, nightly full cook ≤ 4 h, patch-size reports (AAA-CNT-7).

**T30 Collaboration Service** (R08-T30, ED-P1-14, ED-P2-01; R06-ENG-31) — protocol in §1.8.
- *Editor UI:* presence avatars in viewport and outliner (who, where, selecting what); lock badges;
  **home badges** on global documents (which session holds them, with *Ask to return* and, for leads,
  *Force return*); session panel (participants, stream lag, conflicts, commits behind `main`, open
  transaction groups, checked-out documents, pending carries); in-world notes; follow-user camera.
- **MVP (Ph2):** LFS lock mirroring, who-has-what-open presence, soft locks, all project-wide in the
  data session (§1.8.2).
- **AAA (Ph3):** live edit instances, transaction stream with multi-document transactions and
  groups, conflict rules, session rebase, linearized one-PR publish, publish preview, ≤ 1 s
  propagation; the data session's stream, document homes with routing, checkout and check-in, and
  carry publishes (§1.8.2); staged live-shard editing ships through T27 (Ph4).

### 2.6 Physics, volumes and partitions (T01, T03, T08, T15, T24, T28)

Destiny-style precision damage needs per-skeleton hitboxes with hit zones (04 §5.6, 06 §8.6).
Pressurized interiors (W08, a Ph1 critical capability) need portal cells. BENCH-2 and BENCH-6 need grid
transfer volumes, and v1 multi-cell zones need region partitions (04 §6.5). All of this is authored in
the tools below. They share one schema set, preview with the real `engine/physics`, `world` and
lag-compensation code (principle 5), and report through one T28 rule family.

| Data | Schema (runtime owner) | Authored in | Consumers | Ph |
|---|---|---|---|---|
| Collision matrix | `PhysicsLayersDef` (02 §7.1) | T08 matrix customizer | Client, cell | 1 |
| Collision profiles | `CollisionProfileDef` | T08; picked in T24 `.meta`, T03 and T01 | Client, cell | 1 |
| Physical materials | `PhysicalMaterialDef` | T08; assigned in T24 slots, T03 faces, T04 material affectors, T15 capsules | Cell (friction, penetration); client (impact cues, footsteps, decals) | 1 (physics) / 2 (cues) |
| Static collision | `.meta` collision settings | T24, T03 | assetd → `.hcc` statics | 1 |
| Hitboxes and hit zones | `HitboxSetDef` | `helios-tool fit-hitboxes` + T08 (Ph1); T15 Physics Asset tab (Ph2) | Cell lag compensation; owner-client hit prediction; 06 damage (`hitZone`) | 1 |
| Ragdolls | `RagdollDef` | T15 | Client cosmetic physics | 2 |
| Secondary motion; cloth | `SecondaryChainDef`; `ClothDef` | T15 | Client only | 2; 4 |
| Portal cells and portals | `PortalCell`, `Portal` components → cooked `PortalGraph` (02 §5.1) | T01 Volumes mode | 03 culling, audio, nav, streaming, 06 pressure, maps and weather | 1 |
| Grid transfer volumes | `GridVolume` component (02 §5.4) | T01 Volumes mode | `world` transfers; 04 §5.5 checks | 1 |
| Zone partitions | `ZonePartitionDef` (02 §5.5) | T01 Partition tool | Orchestrator region leases (05 §1.4.2), `ownedRegions`, multi-cell PIE | 1 (`Whole`) / 3 |

#### 2.6.1 Schemas (sketch; 02 owns the runtime)

```
flags QueryChannel { Visibility; Weapon; Camera; Interaction; Nav }
record CollisionProfileDef @table("collprofile") {
  layer:    PhysicsObjectLayer                     // Static … Interior (02 §7.1)
  ignore:   set<PhysicsObjectLayer>                // may only narrow PhysicsLayersDef
  queries:  QueryChannel
  material: PhysicalMaterialRef?                   // default for shapes without their own
}
record PhysicalMaterialDef @table("physmat") {
  friction:    f32 = 0.6  @range(0, 2)
  restitution: f32 = 0.1  @range(0, 1)
  density:     f32 = 1000 @unit(kg/m3) @range(1, 30000)
  surface:     TagSet                              // Surface.Metal.Hull, Surface.Flesh, …
  penetration: f32 = 0    @unit(mm)                // resistance used by 06 §8.7 penetration hooks
  client { impactCues: map<Name, CueRef>; footstep: Ref<AudioEvent>?; decal: AssetRef<Decal>? }
}
struct HitCapsule {
  joint: Name;  a: vec3f;  b: vec3f               // joint-local segment
  radius:   f32 @unit(m) @range(0.01, 20)
  zone:     TagSet @max(1) @editor(widget="tag", root="HitZone")   // → DamagePacket.hitZone (06 §8.7)
  material: PhysicalMaterialRef?
}
record HitboxSetDef @table("hitbox") {
  skeleton: AssetRef<Skeleton>
  capsules: list<HitCapsule> @keyed @max(24)      // the ≤ 24 server-sampled joints (02 §7.2)
  bounds:   f32 @unit(m)                          // sphere for rewind broadphase (04 §5.6)
}
record RagdollDef @table("ragdoll") @client_only {
  hitboxes: HitboxSetRef                          // bodies are the hitbox capsules
  joints:   list<{ joint: Name; parent: Name; twist: vec2f @unit(deg); swing: vec2f @unit(deg);
                   motor: f32 = 0 }> @keyed @max(24)
  mass: f32 @unit(kg);  blendIn: Duration = 150ms;  sleepAfter: Duration = 5s
}
record SecondaryChainDef @client_only { root: Name; bones: u8 @range(2, 8); stiffness: f32;
                                        damping: f32; windScale: f32 = 1; collideSelf: bool = true }
record ClothDef @client_only {                     // Ph4, Jolt soft body with skinned constraints
  mesh: AssetRef<Mesh>;  maxDistance: AssetRef<VertexMask>;  simRange: f32 @unit(m) = 30
  stretch: f32;  bend: f32;  windScale: f32 = 1
}
record ZonePartitionDef @table("zonepart") {
  zone:    Name                                   // orchestrator zone id (05 §1.4)
  frame:   Name                                   // system, body or grid frame of the regions
  kind:    enum { Whole; Planes; Grid }           // Whole = v0, one region
  planes:  list<{ splits: u16; normal: vec3f @normalized; offset: f64 @unit(m);        // BSP: each plane
                  front: u16; back: u16 }> @keyed @max(63)   // splits one region into two
  grid:    { origin: vec3d; size: vec3d @unit(m); dims: u8[3] }?
  regions: list<{ id: u16; name: Name; tickBudgetMs: f32; standbyClass: Name }> @keyed @max(64)
}
```

`PortalCell{hull, flags: pressurized | roofed | exteriorVisible, probeZone, exposureZone}` and
`Portal{cellA, cellB | Exterior, polygon ≤ 8 verts, flags: window | airlock}` are authoring components
with the fields of 02 §5.1's table, and cell references are GUIDs. `GridVolume{shape, hostGrid,
targetGrid}` is 02 §5.4's component.

#### 2.6.2 T15 Physics Asset tab

- **Auto-fit** (`helios-tool fit-hitboxes <skeleton>` headless in Ph1, a button in Ph2):
  1. For each joint, gather the bind-pose vertices whose largest skin weight is on that joint and is
     ≥ 0.5.
  2. Joints with fewer than 32 such vertices merge into their parent.
  3. PCA over the vertices gives the capsule axis. The segment spans the 2nd to 98th percentile along
     the axis, and the radius is the 90th percentile of perpendicular distance.
  4. If more than 24 joints remain, the smallest capsules merge into their parents until 24 remain.
  5. Project rules on bone names seed the tags (`*head*` → `HitZone.Head`, spine and pelvis →
     `HitZone.Torso`, limbs → `HitZone.Limb`).
- **Editing.** Capsule gizmos for end points and radius; mirrored editing for symmetric skeletons;
  a HitZone picker with one colour per zone; weakpoints (`HitZone.Weakpoint.*`) for Destiny-style
  precision spots such as the Hollow drone's optic core; a physical material per capsule (armour plate,
  flesh, carapace).
- **Coverage and fairness.** Every clip in the skeleton's animation set is sampled at 30 Hz.
  *Coverage* counts skinned vertices within capsule radius + 2 cm. *Fairness* measures how far capsule
  surfaces lie outside the render mesh. Results show as a heat map on the mesh and as worst frames on
  the timeline; thresholds are the `phys.hitbox.*` rules (§2.6.6).
- **Server-sampling preview.** It overlays the client pose (full graph at frame rate) and the server
  pose: only the hitbox joints, from the reduced-rate server clip set (02 §6.5), at the zone profile's
  tick rate, interpolated as the lag-compensation history does. A graph shows the worst per-capsule
  deviation per clip. **Test shots**: pick an RTT (50–250 ms) and click in the preview to fire. The
  runtime lag-compensation library (04 §5.6) resolves the shot against the rewound capsules, and the
  preview shows the client-predicted zone next to the server-resolved zone.
- **Ragdoll.** Generated from the hitbox set: one body per capsule, masses from material density ×
  volume normalized to `RagdollDef.mass`, and swing-twist limits from the animation set's observed
  joint ranges plus 10°. Limits are edited with cone gizmos. *Drop test* and *impulse test* run the
  real Jolt `Ragdoll` in the preview. Ph3 adds powered ragdolls (motor strength) for hit reactions and
  blend-out.
- **Secondary motion (Ph2).** Pick a bone chain (≤ 8 bones) and set stiffness, damping and wind scale.
  The preview runs the client's Verlet chain node against 03's analytic wind field, with a wind gizmo.
- **Cloth (Ph4).** Paint per-vertex maximum distance and skinned-constraint weights on the cloth mesh;
  generate Jolt soft-body settings; preview in wind; a budget meter shows vertices and µs against
  02 §7.1's cloth budget.

#### 2.6.3 T01 Volumes mode: portal cells and portals

- **Auto-generation** (`volumes.generateCells`, also headless):
  1. Voxelize the container's static collision (T03 blockout, imported kit meshes, doors closed) in
     container space at 0.25 m, or 0.1 m for tight ship interiors.
  2. Flood-fill air from outside the container; air not reached is interior. Interior seeds come from
     player starts and nav seeds, or are placed by hand. A seed that reaches the outside without
     passing a door is a **leak** (`phys.portal.leak`).
  3. Build a 3D distance field over interior air. Door entities (`DoorDef`) and pinches with a free
     cross-section under 6 m² become portal candidates, each fitted with a convex polygon of ≤ 8
     vertices.
  4. Watershed segmentation on the distance field, bounded by portals, splits interior air into rooms.
     Rooms under 8 m³ merge. A non-convex room splits into convex cells joined by door-less portals,
     because 02 §5.1 stores one convex hull per cell.
  5. Emit `PortalCell` and `Portal` entities into the container.

  Regeneration keeps manual edits (moved hulls, split or merged cells, hand-drawn portals, flags) as
  pinned overrides, as T07 does for generated systems. Budget: a 60 × 40 × 20 m station deck at
  0.25 m in ≤ 5 s.
- **Editing.** Convex hull editing (vertex drag, plane push); portal polygons; cell flags
  `pressurized`, `roofed` and `exteriorVisible`; probe and exposure zones; portal flags `window` and
  `airlock`; drag a door onto a portal to bind `DoorDef.portal` (06 §9.6); `InteriorMapDef` room
  names and deck indices inline (06 §9.8). *Make airlock* creates the two interlocked doors, the airlock
  cell with its `AirlockSpec` and a matching `GridVolume` boundary.
- **Previews.** *View from cell* runs `visibleCells` from the camera's cell and dims the rest, which is
  exactly 03's culling. `propagate` shows audio reach as a heat overlay. The pressure preview opens a
  portal to vacuum and drains cells at 2 Hz with 06 §9.6's flow model. Door states toggle in place.

#### 2.6.4 T01 Volumes mode: grid volumes

- **Shapes:** box, capsule, convex hull, *from cells* (the union of selected pressurized portal
  cells, the usual choice for ship interiors), or *hull shell* (the hull's convex hull grown by a
  margin, 25 m by default, for a ship's zero-gravity `Exterior` EVA grid, 06 §8.2a), each with a host
  grid and a target grid. A shell whose margin is below the longest `SuitDef.tetherLength` + 5 m is a
  T28 error.
- **Hysteresis visualizer.** It draws the authored surface, the enter shell 0.5 m inside and the exit
  shell 1.0 m outside (02 §5.4) in distinct colours. It highlights walkable floor inside the exit band
  (ping-pong risk) and places where the band cuts a doorway.
- **Transfer probe.** Drag a capsule, or replay a recorded walk, through the volume while the host
  grid moves at a chosen velocity and acceleration, such as BENCH-6's 300 m/s under 1 g. The real
  `world` transfer code runs headless and reports the transfer tick, dwell and re-expression error
  against the ≤ 1 tick, < 1 mm budget.

#### 2.6.5 T01 Partition tool

- Writes one `ZonePartitionDef` per zone. `Whole` (one region) is the default and is what every v0
  zone uses in Ph1–2.
- For v1 static multi-cell zones (Ph3, 04 §6.5), the designer places planes (axis-aligned by default;
  on a body, great circles through its centre) or a grid, and names the regions with tick budgets.
- **Viewport:** region tint and boundaries, with the handoff band H (max(10 % of cell size, 50 m
  ground or 2 km space), 04 §6.3) and the ghost margin M drawn on both sides.
- **Load preview:** per region, content-placed entities, spawner maximums, host grids and expected
  players from telemetry or the latest bot run's heat map (§3.7), turned into an estimated tick cost
  with T26's content budgets. A region above 70 % of its budget at the expected population is flagged,
  which is the v2 split threshold.
- **Output:** the orchestrator creates one region lease per region (05 §1.4.2), cells receive
  `ownedRegions` (02 §5.5), and multi-cell PIE uses the same record (§1.6.1).

#### 2.6.6 T28 physics, volume and partition rules

| Rule | Fails when | Severity | Ph |
|---|---|---|---|
| `phys.portal.leak` | An interior seed reaches outside air without crossing a portal. The leak path is drawn in the viewport as a polyline, like a Quake pointfile | Error | 1 |
| `phys.portal.unbound` | An `airlock` portal has no bound door; a `DoorDef.portal` names no portal; a portal's two cells do not touch it | Error | 1 |
| `phys.cell.overlap` | Two cell hulls overlap by > 1 % of the smaller one, or a hull leaves the container bounds | Warning | 1 |
| `phys.cell.unmapped` | An interior cell has no `InteriorMapDef` entry | Warning | 2 |
| `phys.gridvolume.band` | Walkable floor lies inside an exit band for > 2 m of path, or two grid volumes' bands overlap | Error | 1 |
| `phys.gridvolume.hull` | A grid volume is not inside its host hull's collision bounds minus 0.25 m | Error | 1 |
| `phys.hitbox.budget` | More than 24 capsules, or a capsule with no `HitZone.*` tag | Error | 1 |
| `phys.hitbox.coverage` | Coverage < 95 % overall or < 90 % for any hit zone, in any sampled frame | Error | 1 |
| `phys.hitbox.fairness` | A capsule extends > 5 cm (or 3 % of skeleton height for large creatures) outside the render mesh in any sampled frame | Warning; error on PvP-flagged record templates | 2 |
| `phys.hitbox.drift` | Server-sampled capsules deviate from the client pose by > 8 cm at the zone profile's tick rate | Warning | 2 |
| `phys.material.missing` | A collision shape or terrain material layer has no physical material | Warning | 1 |
| `phys.ragdoll.limits` | The rest pose violates a joint limit, or bodies interpenetrate by > 1 cm at rest | Error | 2 |
| `phys.layers.matrix` | The `PhysicsLayersDef` matrix is not symmetric, or a `CollisionProfileDef` widens it | Error | 1 |
| `phys.vehicle.envelope` | A `VehicleDef`'s `maxSpeed` or `boost.speed` exceeds 100 m/s, or T21's envelope preview measures peak flat-ground acceleration above 8 m/s² (06 §8.1a; 02 §5.8a's workload row) | Error | 2 |
| `phys.vehicle.def` | A `VehicleDef` sets other than exactly one of `wheeled`, `hover` and `mount`, or not the one `model` names; a wheel, pad, seat or saddle names a missing port or socket; a differential or anti-roll bar names a missing wheel; differential torque shares do not sum to 1; a hover `rayLength` is below `trimRange.max` + 0.5 m | Error | 2 |
| `zone.partition.tiling` | Regions leave a gap or overlap in the zone bounds, or a region is narrower than 2H | Error | 3 |
| `zone.partition.grid` | A boundary passes within H of a static host grid's bounds (a station or anchored structure). Boundaries must route around them, or interactions inside them would constantly cross cells as effects and handoffs; moving ships cross boundaries by design (BENCH-6) | Error | 3 |
| `zone.partition.hotspot` | A spawner or interactable cluster lies within H of a boundary | Warning | 3 |

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

## 4. Budgets, UX standards, accessibility, UI testing

### 4.1 Editor performance and iteration budgets
DEV hardware, sample project, measured in CI or nightly.

| Metric | Budget | Criterion |
|---|---|---|
| Cold open to interactive viewport, warm DDC | ≤ 10 s | AAA-ITR-2 (Ph2) |
| Open a 50k-entity container | ≤ 5 s | |
| Hot reload p95: Luau and records (Ph1), shaders and assets (Ph2) | ≤ 2 s | AAA-ITR-1 |
| PIE cold start (cell + backend + 2 clients) / warm restart / stop | ≤ 15 / 5 / 1 s | AAA-ITR-3 |
| Multi-cell PIE (4 cells + gateway + backend + 2 clients) cold / warm (Ph3) | ≤ 25 / 8 s | ED-14 |
| World scripts in PIE (§1.6.2, Ph3): WSH serving after the backend is ready (within the PIE cold start); handler edit → swapped in every partition p95; additive `worldscript` block edit → live p95 | ≤ 3 s; ≤ 2 s; ≤ 5 s | ED-23, ITR-1 |
| World-script `migrate` on dev data, dry run or apply, with RPCs served throughout; World scripts panel: 500-row table page p95 over 1 M rows, invocation visible after commit, panel draw p95 | ≤ 60 s per 100k rows; ≤ 200 ms, ≤ 1 s, ≤ 1 ms | ED-23 |
| Portal-cell generation, 60 × 40 × 20 m deck at 0.25 m | ≤ 5 s | ED-16 |
| Source-control budgets at 500 GB (§1.7.1) | as listed | ED-18 |
| Gameplay `.cpp` edit → relinked and reloaded (MSVC) | ≤ 30 s | AAA-ITR-5 |
| Editor frame p95 with BENCH-1 loaded | ≤ 16.7 ms (editor CPU ≤ 2 ms, GPU passes ≤ 1 ms) | |
| Select / transform / undo 10k entities | ≤ 50 / 100 / 250 ms | |
| Save 1,000 dirty entity files | ≤ 2 s | |
| Edit visible in all editors p95; content publish | ≤ 1 s; ≤ 5 min | AAA-ITR-7 |
| Collab session actor: sustained throughput; submit → ack p99 at 50 tx/s; replica takeover (§1.8.1) | ≥ 500 tx/s; ≤ 50 ms; ≤ 8 s | ED-20 |
| Global-document checkout or check-in, request → adopt; a soft lock on a global document visible in every session (§1.8.2) | ≤ 500 ms p95, ≤ 2 s p99; ≤ 1 s p95 | ED-20 |
| Placement or blockout edit → affected nav tiles live in PIE (§4.1.1) | ≤ 2 s p95 (≤ 4 tiles) | ED-3 |
| One container's HLOD, one hull's impostor or one interior's assembly-time probes, async (§4.1.1) | ≤ 30 s p95 | ED-21 |
| Extension panel draw; C++ editor-module edit → reloaded (§1.10) | ≤ 1 ms p95; ≤ 30 s p95 | ED-19 |
| Incremental single-zone cook | ≤ 60 s | AAA-ITR-4 |
| 100k-record compile / grid scroll | ≤ 60 s / 60 fps | AAA-CNT-5 |
| Journal loss; editor crash rate | ≤ 1 tx; ≤ 1 per 40 h (Ph3), 100 h (Ph4) | AAA-STB-2 |
| Editor RSS, sample project | ≤ 16 GB | |

#### 4.1.1 Derived data after edits

Hot reload (above) covers the assets a designer edits. Edits also invalidate products the designer never
touches: a moved wall invalidates navmesh tiles, a re-assembled ship its impostor and its MIN GI probes, a
placement its container's HLOD. Level designers need the navmesh live in PIE while they move walls, so
each product has a budget from commit to live, and a defined state until then.

| Edit | Derived data invalidated | Rebuilt by | Commit → live | Until rebuilt | Ph |
|---|---|---|---|---|---|
| Place, move or delete an entity with collision; T03 blockout edit; a kit mesh reimport | Nav tiles (64 m, 02 §7.3) that intersect the old or new bounds dilated by the largest agent radius, per affected grid | assetd `nav` builder, one job per tile at viewport/PIE priority. The PIE or edit-instance cell swaps `dtMeshTile`s by handle at a tick boundary | ≤ 2 s p95 for ≤ 4 tiles; ≤ 10 s for ≤ 64 tiles. A whole-zone bake happens only in the CI cook | From the **next tick** the new collision bounds are a `DetourTileCache` obstacle (box or convex, 02 §7.3), so agents re-path around the change at once. Stale tiles are hatched in the navmesh view mode | 1 |
| T04 sculpt, stamp or layer edit | Terrain height and collision tiles; terrain nav tiles | pcg re-evaluates dirty tiles (T04); terrain nav tiles build on the Pathfind pool (≤ 5 ms each, 02 §7.3) | ≤ 2 s p95 for a 50 m brush stroke | Old tiles | 1 |
| Blockout or kit edit inside an interior | That container's `PortalGraph`; `phys.portal.*` rules | T01 Volumes-mode regeneration of the affected cells, keeping pinned overrides (§2.6.3), then cook | ≤ 5 s for a 60 × 40 × 20 m deck | The old graph, plus a T28 "portal graph stale" warning | 1 |
| T05 rule or brush edit; a placement inside a scatter exclusion | Scatter instance caches of the affected 64 m scatter cells (visual on the client; collidable and gameplay instances on the cell) | assetd `scatter` builder per scatter cell | Visual ≤ 2 s p95; collidable on the PIE cell ≤ 5 s p95 | Old instances; a dotted outline marks the brushed region | 2 |
| Any placement or mesh change in a container | The container's HLOD: the 1/8-triangle merged mesh and its impostor (02 §5.6) | assetd `hlod` builder, asynchronous and low priority, GPU offscreen; it starts 10 s after the container's last edit, so a burst of edits costs one rebuild | ≤ 30 s p95 for a container of ≤ 5,000 entities | The stale proxy, with a "stale HLOD" badge in the outliner and stats overlay; the near field shows the live meshes | 3 |
| T21 part or assembly edit; livery change | The hull/livery group's octahedral impostor atlas (03 §3.4) and fleet HLOD | The `helios-bake` impostor baker (03), run by assetd as a builder | ≤ 30 s p95 per hull/livery group | The stale impostor, badged; brackets are unaffected | 3 |
| T21 assembly edit; placement of an authored settlement | Assembly-time L2 SH probes, the MIN GI path (03 §4.6) | The `helios-bake` probe baker, run by assetd (raster capture on the 1.5 m / 2 m grids) | ≤ 30 s p95 for a Mule-size interior (≈ 1,800 probes) | Stale probes, still relit from the current sky and sun. REF viewports show DDGI, which needs no bake; a "MIN GI stale" badge shows in the MIN preview | 3 |
| Reflection-probe inputs change | Relightable probe captures (03 §4.6) | assetd | ≤ 5 s p95 per probe | The stale capture | 3 |
| T22 outfit or palette edit | Crowd-class impostor atlas (03 §7.6a) | T22 crowd bake | ≤ 60 s p95 per silhouette class | The stale atlas | 4 |

**Rules.**
- **Invalidation comes from transactions, not saves.** A transaction's bounds diff (old and new world bounds
  of every touched entity) selects the tiles and containers. Unsaved PIE overlay edits and collab
  transactions therefore trigger rebuilds too. In an edit instance, the instance host's assetd worker builds
  each product once. Editors fetch it from the shared DDC by key, and the cell swaps it.
- **Priority and coalescing.** Nav > collision > scatter > portal graph > HLOD, impostors and probes, and
  regions visible in a viewport or PIE come first. Nav jobs coalesce edits within 250 ms. A newer edit to the
  same tile or container cancels a build in flight.
- **Incremental equals clean.** Nightly, `helios-tool bake-nav --verify-incremental` and
  `helios-assetd --verify-determinism` compare incrementally rebuilt tiles and proxies with a clean rebuild of
  the sample project; they must be byte-identical.
- **Visibility.** A status-bar indicator counts stale products per kind; clicking it lists them, with
  *Rebuild now*. PIE never waits for an asynchronous product, and a tile swap never costs PIE a frame over
  budget.

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
WCAG 2.2 AA with screen readers; T19 applies the same checks to player UI. §4.4's layout lints enforce
the scale range, contrast and keyboard reachability every night.

### 4.4 Editor UI test harness (`helios-uitest`)

Headless ToolsFramework tests prove that commands work; they do not prove that a designer can reach
them. Agents build the tools in Linux containers, and the one user's time for manual sessions is
scarce (09 K31). Broken layouts, DPI and theme regressions and flows that work only through commands
would otherwise go unseen between rare human sessions. The UI is therefore driven by injected input
every night, and humans run only the M-class phase-exit sessions.

- **Driver.** Built in-house on `edui` hooks (WP-0.18). Dear ImGui Test Engine was evaluated: it is
  source-available under its own licence with paid commercial terms, so it is not on the 01 §5.2
  allow-list. Helios follows its design (items addressed by path, queued input, wait helpers) without
  its code.
  - *Item table.* In test mode `edui` records every interactive item each frame: a stable test path
    built from the ImGui ID stack and the widget's semantic label
    (`T08/Grid/row[hull/kestrel]/col[mass]`), rect, kind, enabled state, value text and tooltip. It
    costs ≤ 0.2 ms per frame and nothing when test mode is off.
  - *Input injection.* `ui.*` methods on the remote-control socket (§1.2) and the Luau `UiTest` module:
    `click`, `doubleClick`, `drag`, `type`, `key`, `scroll`, `waitFor(path | predicate, timeout)` and
    `capture(panel | window)`. They queue events into ImGui's input queue through the SDL3 backend
    seam, so they take the same path as a real mouse and keyboard. Menus, the command palette,
    drag-and-drop between panels, gizmo drags (handle positions come from the item table) and
    multi-viewport tear-offs are all reachable.
  - *Determinism.* Fixed dt, caret blink and animations off, tooltip delay 0, pinned fonts and default
    layouts, fixed window sizes (1920 × 1080 at 100 %, 3840 × 2160 at 200 %), and a seeded sample
    project fixture.
- **Screenshot goldens.** Linux runs lavapipe under Xvfb on every CI tier. Windows runs nightly on
  `win-gpu`, post-merge only (09 WP-0.4 policy).
  - Every shipped tool's default layout is captured empty and with the fixture loaded, at 100 % and
    200 % DPI, in the dark and high-contrast themes: 30 tools × 2 states × 2 scales × 2 themes = 240
    images at AAA.
  - Comparison uses ꟻLIP, as 03's goldens do: mean error ≤ 0.01 and ≤ 0.1 % of pixels above 0.1.
    Viewport rectangles are masked, because 03's goldens own the renderer.
  - Scale is forced with `HELIOS_EDITOR_SCALE`, which uses the same font re-rasterization path as
    PerMonitorV2. One real drag between a 100 % and a 200 % monitor stays an M-class check per phase.
- **Layout lints** (no golden needed), at 75, 100, 150, 200 and 250 % scale and with +40 %
  pseudo-localized labels. Each of these is an issue:
  - text clipped inside an item, overlapping interactive items, or an item outside its window;
  - a command that no menu, toolbar or context menu exposes, unless it is marked palette-only;
  - an interactive item with no label or tooltip;
  - theme-token text contrast below 4.5:1, or 7:1 in high contrast;
  - a panel that keyboard focus cycling cannot reach.

  Panels, tools and commands registered by gems through §1.10 are linted the same way, and a studio's CI
  runs `helios-uitest` over its own gems unchanged (ED-19).
- **Dual-path scenarios.** Each ED scenario script runs twice, once through commands and once through
  `ui.*` input, and both runs must end at identical document hashes. A workflow that works only through
  commands fails.
- **Scripted replays.** ED-2 and ED-16 (Ph1), ED-7 and ED-17 (Ph2), and ED-10 (Ph3, with three editor
  processes co-editing through the dev collab service) and ED-23 (Ph3) run nightly as UI-level scripts. They are
  N-class evidence for the flows and record a wall-time trend. Humans run the timed M-class versions
  only at phase exits.
- **Tiers.** PR: open every shipped tool at 100 % in the dark theme and run the lints (≤ 6 min).
  Nightly: the full golden matrix, lints and replays (≤ 60 min). Flaky tests follow 09 §5.8.

---

## 5. Delivery, acceptance, risks, traceability

### 5.1 Phase-by-phase delivery
**M** = MVP, **A** = AAA-complete, **lite** = generic-inspector or text authoring. Gates from 01 §3
(AAA- prefix omitted).

| Tool | Ph0 | Ph1 | Ph2 | Ph3 | Ph4 | Ph5 |
|---|---|---|---|---|---|---|
| Framework / UI | Transactions, journal, property grid, shell, `helios-tool`, `helios-uitest` driver | Viewport, PIE, automation; incremental nav tiles live in PIE; UI goldens, ED-2 and ED-16 replays | Crash reports, role layouts, source-control UI, LFS scale set-up; two-zone PIE; editor extension API `0.x` with Luau `EditorUI` panels; ED-7 and ED-17 replays | Web tools, remote control; multi-cell PIE; world scripts in PIE (one WSH, per-partition DAP, §1.6.2); collab multi-document transactions, rebase, linearized publish, journal durability (§1.8.1), and the data session with document homes (§1.8.2); editor extension API `1.0`; asynchronous HLOD, impostor and probe rebuilds; ED-10 and ED-23 replays | Accessibility pass; optional Perforce adapter; N−2 → N extension upgrades | |
| T01 World | | M (incl. cells, portals, grid volumes) | Data layers | A (incl. multi-cell partitions) | | |
| T02 Prefabs | | M | | A | | |
| T03 Blockout | | | M | A | | |
| T04 Terrain/Planet | `pcg` lib | M (cube-sphere) | Ecosystems | A (Earth-size) | | |
| T05 Scatter | | | M | A | | |
| T06 Splines | | | M | A | | |
| T07 System/Galaxy | | M (Tallis) | Galaxy (Osk) | A | | Telemetry overlays |
| T08 Data | Headless inspector | M | Migrations; governed schema editor (ED-22) | A (incl. `worldscript` blocks, ED-23) | Live tuning | |
| T09 Gameplay | HXL test runner | lite (ASM compile, HXL) | M (loot, crafting, economy) | A | Economy model | |
| T10 Script | | M | | A (incl. code pane, WSH DAP) | | |
| T11 Graphs | Graph model | | M | A | | |
| T12 Quest | | | M | A (incl. Activity mode, queue simulator) | | UGC missions |
| T13 Dialogue | | | M | Complete | A (auto-staging) | |
| T14 Sequencer | | | | M | A | |
| T15 Animation | | M (import, headless hitbox fit) | M (Physics Asset tab; mount and rider sets) | A | Motion matching, facial, lip-sync, cloth | Learned MM |
| T16 Material | | M (instances) | Graph | A | | |
| T17 VFX | | | M | A | | |
| T18 Environment | | lite | M | A | Clouds (03) | |
| T19 UI Designer | | lite | M | A | | Addon sandbox |
| T20 Audio | | | M | A | | |
| T21 Assembly | | | M (incl. ground vehicles) | A | | Player blueprints |
| T22 Character | | lite | M | | A | |
| T23 AI/Nav | | lite | M | A | | |
| T24 Assets | | M | | A | | HDA, USD |
| T25 Localization | | | M | A | String hotfix | |
| T26 Profiling | Tracy | M | | A (incl. World scripts panel) | | |
| T27 GM/Live-ops | | M (GM cmds) | | GM client, case queue, World scripts panel | A | |
| T28 Validation | Framework | M (≥ 20 rules) | All asset classes | A (≥ 150, incl. `ws.*`) | | |
| T29 Build/Cook | assetd skeleton | M | Incremental ≤ 60 s | A | | |
| T30 Collab | | | M (locks, presence; project-wide data session) | A (edit instance; document homes, checkout, carries) | Live shards via T27 | UGC publish |
| Docs, projects, templates (09 §2.7) | | Generated reference, manual page per tool, F1 help | Project Browser, New Project, SDK, `upgrade-project`, executable tutorials, `starter-blank` | 3 starter templates, C++ API reference | 5 templates, N−2 → N upgrades | |
| **Gate** | PLT-1 | TOOL-1, TOOL-7 (Ph1) | TOOL-2, TOOL-6, TOOL-7/8 (Ph2); ED-22 | TOOL-3 (26/30), TOOL-5, TOOL-7/9 (Ph3, TOOL-7 including the editor API), ITR-7; ED-19…21, ED-23 | TOOL-4, TOOL-8/9 (Ph4), TOOL-10 (content-team zone, UI provenance), ITR-8 | |

At the end of Ph3, 26 tools are AAA-complete; the four finishing in Ph4 (T13, T14, T22, T27) are
exactly those AAA-TOOL-4 names.

### 5.2 Acceptance criteria
Automated tests or QA-timed scripted designer tasks, on Windows and Linux. Designer tasks marked "no
text editor" or "editor only" also run nightly as UI-level replays (§4.4, ED-15); the timed human runs
are M-class evidence at phase exits (09 §5.6).

| ID | Criterion | Ph | Refs |
|---|---|---|---|
| ED-1 | Headless ToolsFramework round-trips 10k random transactions (apply, undo, redo) to byte-identical JSONC on MSVC, GCC and Clang; `kill -9` mid-burst loses ≤ 1 transaction | 0 | PLT-1, STB-2 |
| ED-2 | **Saltmarch slice, editor only:** a designer places the Saltmarch outpost on Harrow (terrain stamp, ≥ 40 entities, landing pad), a Hollow-drone spawn region with navmesh and a Kestrel variant record, then flies the BENCH-2 descent in PIE with 2 clients and 4 bots; no text editor, no engineer | 1 | TOOL-1, ITR-3 |
| ED-3 | 100 scripted Luau/record edits: save → visible in running PIE cell and client ≤ 2 s p95; placement edits apply without reload; PIE cold start ≤ 15 s. **Nav stays live:** with 8 bots pathing through Saltmarch in PIE, 50 scripted placements, moves and deletions of blockout walls and kit pieces each have their affected nav tiles live in the PIE cell in ≤ 2 s p95 (≤ 10 s for a 64-tile edit); from the next tick a `DetourTileCache` obstacle blocks each new piece, so no bot walks through it; tile swaps cause no frame over budget; and the incrementally rebuilt tiles are byte-identical to a clean bake (§4.1.1) | 1 | ITR-1, ITR-3 |
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
| ED-14 | **Seams at a desk:** multi-cell PIE with Cells: 2 and a plane partition across the *Mule*'s route. A Luau-scripted Mule with 3 crew and a 2-player boarding party (the BENCH-6 set-up) crosses the boundary 100 times, including 10 forced handoffs mid-boarding and 2 cell kills: 0 Luau errors, 0 duplicated effects (every idempotency ID applied exactly once), 0 lost input sequences and 0 seam errors in the effect log. The `.hrepro` of one crossing replays bit-exactly. 4-cell PIE cold start ≤ 25 s. Runs nightly headless on Windows and Linux | 3 | 06 §0 rule 4; NS-3.2, NS-3.6; M09 |
| ED-15 | **UI harness (§4.4):** every shipped tool opens through `ui.*` input and passes the layout lints at five scales; goldens at 100 % and 200 % in the dark and high-contrast themes match; dual-path scenario hashes match; the UI-level replays of ED-2 and ED-16 (Ph1), ED-7 and ED-17 (Ph2) and ED-10 and ED-23 (Ph3) pass on 3 consecutive nightlies on Windows and Linux | 1–3 | TOOL-1/2/3, PLT-3 |
| ED-16 | **Harrow High interior, no text editor:** a designer generates and edits the hangar and concourse portal cells (≥ 12 cells, ≥ 16 portals, 1 airlock with bound doors) from imported kit collision, sets the pressurized and roofed flags, and authors the station-interior and hangar `GridVolume`s. T28 reports 0 leaks and 0 band errors. A scripted walk from the concourse through the airlock to the *Kestrel*'s boarding point transfers at every volume in ≤ 1 tick with < 1 mm error and no repeated transfer within the dwell, and *view from cell* matches 03's visible-cell set | 1 | TOOL-1; W02, W08 |
| ED-17 | **Hollow drone precision hits:** a designer authors the Hollow drone's `HitboxSetDef` in T15 (auto-fit, a `HitZone.Weakpoint.Core`, physical materials) with no text editor; T28 shows coverage ≥ 95 % and 0 fairness errors. NS-2.2's lag-compensation harness, run against that set with bots at 50–150 ms RTT firing 10k hitscan shots at a drone playing its full clip set, agrees with the local-hit reference on the hit **zone** for ≥ 98 % of shots, and every server-registered weakpoint hit applies its `WeaponDef` `HitZone` multiplier | 2 | M05, M08; NS-2.2; 06 §8.6 |
| ED-18 | **Repository scale:** every §1.7.1 budget holds on the 500 GB reference repository (W-class, weekly, lab hardware) and on its 50 GB nightly cut, on Windows (Dev Drive) and Linux | 2 | ITR-6, PLT-3 |
| ED-19 | **Editor extension SDK (§1.10):** starting from the binary SDK with no engine source, the `sample-editor-ext` gem adds a C++ dock panel and a Luau panel, a command with a menu entry and shortcut, a `.hsurvey` document type, an importer for a CSV-based survey format, a post-cook step, a viewport mode with a gizmo, a property customizer, a T28 rule with a quick fix, and a node library for the Logic family. All of them work in the editor, and their UI-less parts run in `helios-tool` and assetd in CI. Every mutation appears in undo, the crash journal and a collab session. The gem's panels pass §4.4's lints at five scales and its goldens. A one-`.cpp` edit hot-reloads the panel in ≤ 30 s p95 with its state kept. The doc-coverage gate reports 100 % for `include/helios/editor/`, `upgrade-test` moves the gem N → N+1 with zero manual edits, and T06, T20 and T25 pass the `edtools-public-only` lint. Nightly on Windows and Linux; the Luau half gates Ph2 | 2 (Luau), 3 | TOOL-7, TOOL-8; 01 §1.1; R08 §4 item 9 |
| ED-20 | **Collab scale, durability and session scope (§1.8.1, §1.8.2):** 25 editor processes (22 automation editors driven by `helios-tool collab-bot` plus 3 `helios-uitest` UI-level replays) co-edit one Saltmarch edit instance at 50 transactions/s sustained for 4 h, with 10 s bursts of 500/s every 30 min; 20 % of transactions are multi-document, and one transaction group over 1 MB lands every 10 min. Propagation ≤ 1 s p95 and ≤ 2 s p99; submit → ack ≤ 50 ms p99 outside failovers. During the run the owning collab replica is killed 4 times and one NATS node is killed and restored twice. Each takeover completes in ≤ 8 s, and every submitted transaction ID appears in the stream exactly once. At the end all 25 editors hold identical document hashes and journal vs git export diff = 0. The stream is then deleted: a restore from object storage alone reproduces every transaction up to the last archived segment, and with tail recovery from the editors it reproduces the hashes exactly. **Cross-session scope (§1.8.2):** in the same 4 h window two more sessions run beside Saltmarch: a second zone session (Harrow High, 6 collab-bots) and the project's data session (8 collab-bots, 2 scripted Writers' Room web clients and 1 `helios-uitest` T08 replay, 30 transactions/s). All three sessions edit an overlapping set of 2,000 global documents (items, abilities, loot tables, quests, dialogue and string tables). 5 % of zone transactions also touch a spatial document, which forces a checkout, and a checkout, check-in or *Force return* happens every 10 s. The data session's owning replica is killed twice, once between a handoff and its adopt. Checkout request → adopt ≤ 500 ms p95 (≤ 10 s across a takeover), and a soft lock on a global document is visible in all three sessions in ≤ 1 s p95. Every submit that reaches an actor that does not home its documents is rejected with `moved` or `inTransit`, and none is accepted. A home audit over the three streams finds 0 documents with overlapping write epochs, and every handoff and release adopted exactly once. The three sessions then publish (data, Saltmarch, Harrow High, with the carries the service opens): **0 publish-time or rebase conflicts on documents held under another session's home or lock**, 0 dangling cross-session references, journal vs git export diff = 0 for each session, and `main`'s final tree equals the base plus the three sessions' composed overlays. A seeded direct commit to one global document on `main` yields exactly 1 reported conflict, which shows the check can fail. Weekly on lab hardware (W-class); nightly, a 10-editor 30-minute cut with one failover and one restore, and a 30-minute cross-session cut (2 zone sessions and the data session, 3 editors each, one data-actor failover, then a publish of all three) | 3 | ITR-7, STB-2 |
| ED-21 | **Derived data stays live (§4.1.1):** in a running PIE session a scripted designer moves 200 entities across 5 Saltmarch containers, re-assembles the *Mule* in T21 and repaints 20 scatter regions. Each container's HLOD, the Mule's impostor and its assembly-time SH probes are rebuilt in ≤ 30 s p95, and scatter caches are live in ≤ 2 s (visual) and ≤ 5 s (collidable). Until then the stale proxy is shown with its badge, never a hole or a pop to nothing. PIE never blocks on a rebuild, and incremental products equal a clean rebuild byte for byte. Nightly on Windows and Linux (lavapipe for the GPU bakes; timings on `win-gpu` and the lab) | 3 | ITR-1, ITR-4; 03 §3.4, §4.6 |
| ED-22 | **Project types with no compiler (02 §3.8):** on the packaged artist editor (AAA-ITR-6), on a Windows 11 and an Ubuntu 24.04 machine with no C++ compiler or Go toolchain installed, a designer uses T08's schema editor to add (a) a record type with 3 fields, including a list, with a `@range` and a `@validate` rule, (b) a `ScriptState` component that a Luau module reads and writes, and (c) a view-model bound in a new T19 screen through `@source` on that component. All three are live in the running PIE cell and client in ≤ 5 s p95 from save, with no DLL built. A record that breaks the rule is rejected by T28 and by the collab service. The project then packages with the SDK's prebuilt stamped client and cell through T29 (the CL-24 route), and the packaged build runs the screen against a cell whose `helios-backend` is the unmodified SDK binary. No engine, backend or C++ source is edited, and no native package is added. Nightly headless (the designer's steps as `helios-uitest` replays) on Windows and Linux | 2 | TOOL-9; ITR-1, ITR-6; 01 §1.1 |
| ED-23 | **A cross-zone system, editor only (§1.6.2; 05 §1.23):** in a project created from `starter-sandbox`, a designer uses only the editor: T08's schema editor, T10's embedded code pane and the World scripts panel. No external text editor, CLI, compiler, or engine, backend or C++ source edit is used. **(a) Author.** In the bounty board's `worldscript` block the designer adds a `claimZone: ZoneId?` field to `Bounty` and a `Cancel` RPC (`@callers(cell)`, `@partitionBy(target)`, a `@rate`). They write its handler from T08's generated stub: the poster cancels an `Open` bounty, the row closes as `Cancelled`, the `Expire` timer is cancelled and the escrow is released to the poster under `BountyRefund`. They also edit the `Killmail` handler to set `claimZone`. The block edit is live in the running PIE in ≤ 5 s p95, and each of 20 handler edits is live in every partition in ≤ 2 s p95, with no PIE restart. T28 rejects two seeded variants: a `Cancel` handler that releases under a `Sink` reason, and a second `@currency` field that no `@escrowBacked` declaration covers. **(b) Play.** In two-zone PIE (one WSH, 2 clients and a bot hunter), client 1 posts a bounty on client 2's character at the terminal in zone A. The bot kills client 2 in zone B, and the bounty pays exactly once, with `claimZone` = B. Client 2 then posts a second bounty at zone B's terminal, crosses to zone A and cancels it there, and it is refunded exactly once. **(c) Debug.** With a breakpoint in the `Killmail` handler, the next kill stops that partition's VM in the code pane, where `ctx` and the row are inspectable. Zone cells keep ticking and the other partitions keep serving. After 60 s at the breakpoint and a continue, the payout commits once, with no `ws_dead` entry and no partition takeover. **(d) Inspect.** The Tables tab, queried through the `(target, status)` index, shows the row with `status = Paid` and `claimZone` = B. The Invocations tab shows the `Post`, `Killmail` and `Cancel` invocations with their fuel and ledger intents, and *Check escrow backing now* matches. A GM row edit with a reason appears in the Audit tab, and an edit to `amount` is refused by the panel and by the service. **(e) Migrate.** With 100k v1 rows loaded from the template fixture, the designer splits `status` into `status: enum { Open, Closed }` and `outcome: enum { Paid, Expired, Cancelled }?`, and fixes the handlers that the code pane's type checker then flags. T28 blocks the save until a `migrate` handler exists. The migration preview classifies the change as *migrate* and counts 100k affected rows, and the dry run shows 20 sample row diffs and 0 failures. *Run on dev data* migrates every row in ≤ 60 s while 4 bots keep posting, with every `Post` served, 0 rows in `ws_dead` and a matching escrow-backing check. The migrated rows hash identically to `helios-tool upgrade-project` run on the same fixture. Nightly on Windows and Linux as a dual-path `helios-uitest` replay (§4.4); a timed human run is M-class evidence at the Ph3 exit | 3 | TOOL-9, ITR-1; 01 §1.1; 05 A20 |

### 5.3 Risks and mitigations

| Risk | Mitigation |
|---|---|
| ImGui widget work (property grid, graphs, timeline, code pane, data grid) is the largest hidden cost (R06 §6.15) | Widget library first (Ph0–1) under one owner; reuse vendored ImGuizmo, ImPlot, imgui-node-editor; review gate requiring shared widgets; ~25% of tools effort reserved |
| 30 tools is too much scope | Generic T08 inspector makes every type editable on day one; bespoke tools only for measured workflows; cut Ph5+ items, then AAA polish, never MVP items |
| Co-editing corrupts or loses edits | Property-level ops, keyed lists, one sequencer; CI check journal replay = git export; soaks with scripted "bot editors" via the automation API |
| Global records, quests or dialogue edited in two sessions conflict at publish, or zone sessions strand them | One home session per document with routing; project-wide soft locks and presence in the data session; home changes ordered on `COLLAB_data` with fenced commit points; carry publishes and per-document merge bases; automatic check-in (§1.8.2); ED-20's cross-session run |
| Unpublished edit-instance work is lost with the bus, or the session actor saturates | R3 streams; journal segments in object storage every ≤ 30 s, snapshots, a WIP git ref every 10 min and tail recovery from editors' journals; nightly restore test and quarterly drill; a ≥ 500 tx/s actor with pipelined publishes and unstored gesture previews (§1.8.1); ED-20 |
| Studios fork the engine to build bespoke tools, or engine releases break their plug-ins | A public editor extension API with no third-party types, the deprecation policy, doc coverage, and `sample-editor-ext` in `upgrade-test`; three built-in tools built on public headers only (§1.10); ED-19 |
| Edits leave derived data stale, so PIE shows a navmesh or HLOD that no longer matches the level | Transaction-driven invalidation, per-product budgets, next-tick TileCache obstacles, marked stale proxies, incremental = clean checks (§4.1.1); ED-3, ED-21 |
| Editor previews drift from the game | Shared libraries (`pcg`, `assembly`, HXL, ASM, roller); golden-hash tests |
| Iteration regresses as content grows (Destiny) | §4.1 budgets nightly with a ±10% gate; warm cell; overlays instead of recooks |
| ImGui lacks accessibility and complex text | Web tools for text-heavy roles; runtime text previews; scaling, themes |
| Licences: Blender add-on must be GPL; Maya/Houdini/Wwise/FMOD SDKs are proprietary; libgit2 is GPL-with-exception | Separate optional plug-ins or processes; git CLI; legal sign-off before distribution |
| SWG terrain patents (ADR-006) | Node-DAG formulation; legal review before Ph4 (01 §5.2) |
| Web tools add a stack | Go + htmx as in 05, no Node toolchain; one validation path (`helios-tool`) |
| UI regressions go unseen: agents build tools headless and human sessions are rare (09 K31) | `helios-uitest` (§4.4): injected-input replays of the ED designer flows, goldens at two DPIs and two themes, layout lints; ED-15 nightly from Ph1 |
| Split-authority bugs are reproducible only by CI torture bots | Multi-cell PIE with forced handoffs, per-cell kill and DAP, effect log and `.hrepro` bundles (§1.6.1); ED-14 |
| Git + LFS does not scale to AAA content (hundreds of GB, millions of files) | Sparse index, fsmonitor, on-demand LFS, DDC-first consumption (§1.7.1); ED-18 weekly at 500 GB; a missed budget twice promotes the Perforce adapter, whose scope is fixed (§1.7.2) |
| Authored physics data drifts from what the server samples (unfair hitboxes, leaking interiors) | Previews run the runtime lag-compensation, transfer and portal code; `phys.*` rules; ED-16, ED-17 |
| World-script bugs surface only on shards: designers cannot see the rows, step through a handler or rehearse a migration at a desk, so they fall back to engineers and the CLI | A production-path WSH in every PIE mode, per-partition DAP with dev-only pause hooks, *Re-run under debugger*, the World scripts panel, dry-run migrations on dev data and the `ws.*` rules (§1.6.2); ED-23 nightly |

### 5.4 Traceability
- **R08:** §3 T01–T30 → §2; §4.1–4.10 → §1, §3, with §4 item 9 (C++ plugin API for modes, panels,
  importers, validators and node libraries; Lua automation under transactions) → §1.10 and ED-19; ED-P0-01…16 → ED-6; ED-P1-01…15 → §2; ED-P2-01
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
  state" (§1.5, T12); AI and spawn tool (T23); `HitZone.*` hitboxes for FPS multipliers (§8.6 →
  §2.6.2); portal cells for pressure, maps and weather shelter (§9.6–9.9 → §2.6.3); the
  split-authority model exercised at a desk (§0 rule 4 → §1.6.1).
- **04 asks:** hitbox capsule sets for lag compensation (§5.6 → §2.6.2); static multi-cell partitions
  (§6.5 → §2.6.5); grid volumes that are the only valid transfer points (§5.5 → §2.6.4).
- **05 asks:** world scripts as the studio's backend extension surface (§1.23: a dev WSH with DAP, tables
  browsed with `helios-admin ws rows`, GM `call` and `edit`, `migrate` handlers run by `upgrade-project`) →
  §1.6.2, T08, T26, T27, T28 and ED-23.
- **R08 §3 T15** "cloth and physics preview" → §2.6.2.

### 5.5 Cross-section dependencies
- **02 Engine runtime:** `EDITOR_ONLY` in `helios_module`; `.hcont/.hent/.hprefab/.hrec` formats;
  **keyed list elements** (`@keyed` GUIDs) in `.hschema`; **record hash IDs minted once and stored
  in source**, never recomputed from paths; schemac editor metadata (`@editor`, units, doc comments)
  and Go validators; DAP adapter in the script host, served by each world-script partition VM; schemac's
  `worldscript` blocks in dynamic packages with the `world` realm's `.d.luau` (§3.8; §1.6.2); `engine/pcg` determinism; hot swap by handle;
  the §2.6.1 records (`CollisionProfileDef`, `PhysicalMaterialDef`, `HitboxSetDef`, `RagdollDef`,
  `SecondaryChainDef`, `ClothDef`, `ZonePartitionDef`), the `PortalCell`/`Portal` authoring components
  with a `roofed` cell flag, the client-only cosmetic physics system and the cloth decision (02 §7.1),
  and `ownedRegions` built from `ZonePartitionDef` (02 §5.5); gem module kinds `editor-core` and
  `editor-ui`, built as `game_<gem>_edcore`/`_edui` against the dev link groups, with editor registrations
  recorded in `ModuleRegistrationScope` (§1.10; 02 §1.2, §1.4, ADR-016); nav tiles swappable by handle at a
  tick boundary and `DetourTileCache` obstacles for fresh edits (§4.1.1, 02 §7.3); HLOD proxies rebuildable
  per container (02 §5.6).
- **03 Rendering:** editor passes (ID buffer, outlines, grid, debug draw); offscreen thumbnails; an
  ImGui backend on the RHI; the Slang material `interface` contract; the GPU-particle module
  contract; cloud and nebula preview paths; impostor and assembly-time probe bakers callable per hull or
  interior from assetd within §4.1.1's budgets (03 §3.4, §4.6).
- **04 Networking:** tools channel for PIE overlays; GM connect tokens and RBAC; NetSim toolbar
  hooks; server debug-draw stream; DAP hooks in `helios-cell --replay`; dev-only multi-cell PIE hooks
  (`DevForceHandoff`, zone-leader debug hold, `DebugPaused`, trunk NetSim; 04 §10.3); the
  lag-compensation library linkable by the editor for T15 test shots.
- **05 Backend:** the **collab service** (sessions, one-subject JetStream stream with
  multi-document transactions and groups, session rebase, submit/validate, locks, notes, snapshots,
  linearized git export); overlay content versions; telemetry query API; web-tools hosting; region
  leases created from `ZonePartitionDef` (05 §1.4.2); the dev orchestrator's standby pool for
  multi-cell PIE; world scripts in PIE (§1.6.2): `--spawn ws` (05 §5), the WSH's `--dev` hooks (`DebugPaused`
  for `ws` regions, in-progress acks for held deliveries, the invocation trace and re-run records), the GM API
  behind `helios-admin ws` with its refusal of edits that touch escrow-backed fields, and dev-data migration and
  fixture loading; collab durability (§1.8.1): R3 `COLLAB_*` streams on the `ha` Compose profile and
  Kubernetes, journal segments and snapshots in object storage, a WIP git ref, a `collab_owner` PG lease
  with 05 §1.4.1's term fencing, and the restore runbook; session scope (§1.8.2): one project-wide data
  session beside the zone sessions, the `HOMES_data` mirror, handoff and adopt events, and carry
  publishes.
- **08 Client:** RmlUi view-model codegen and hot reload; runtime text rendering for editor
  previews.
- **09 Roadmap:** gates ED-1…23, with ED-23 owned by WP-3.12 beside the world-script API; carry publishes (§1.8.2) queued ahead of their dependent `collab/*` PR in
  §5.2a's content tier; a dedicated widget-library owner; nightly editor-performance CI;
  bot-editor soaks; the developer-experience track (09 §2.7: docs pipeline, Project Browser and New Project,
  `upgrade-project`, starter templates; AAA-TOOL-7/8/9), which reuses ToolsFramework automation and T28;
  the editor extension API in 09 §2.7.2's public surface, its doc coverage and the `sample-editor-ext` gem in
  `upgrade-test` (§1.10); dev-flavour `helios-editor`, `helios-tool`, `helios-assetd` and `helios-cook` in the
  SDK.
- **R10 manifest:** vendor **imgui-node-editor** (MIT, WP-2.10); **nats.c** in the editor (now in ADR-013);
  vendor the CodeMirror bundle and Apache-2.0 fonts; vendor **V-HACD 4** (BSD-3-Clause, header-only,
  tools only) for convex decomposition (WP-2.11); Dear ImGui Test Engine is not vendored (§4.4).
