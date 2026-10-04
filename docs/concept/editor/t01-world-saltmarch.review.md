# T01 World Editor on the Saltmarch container: concept review

- **Concept:** `t01-world-saltmarch` (index: [README](../README.md#index)).
- **Reviewed:** the owner's v01, 2026-10-04 (four reviewers, merged).
- **v01 is not committed.** Its provenance is not recorded yet (01 §5.2; [README](../README.md)), so the
  image stays out of the repository. The owner is preparing v02, which this checklist is for.
- **Decided 2026-10-04 by the owner ("Option A"):** Saltmarch is an object container in the single
  Phase 1 zone `tallis` (02 §5.5–5.6, 07 §2.1 MVP). Its files are `content/zones/tallis/saltmarch.hcont`
  and `content/zones/tallis/saltmarch.entities/<guid>.hent`, and a shared editing session on it is
  `zone-tallis` (07 §1.8.2). 07 §1.7.1's example `content/zones/saltmarch/` is wrong; a separate Director
  PR (branch `agent/claude/director-saltmarch-container`) corrects it.

Tick items off as v02 covers them. Section numbers are plan sections (`docs/plan/`).

## Verdict

A good north star. The layout, theme and property grid grow straight out of today's editor shell (PR #41)
and match 07 §2's T01 row. The problem is that it shows an ordinary single-level editor rather than
Helios's MMO world: a "world" root with its own sun and sky, made-up `.ent`/`.mesh`/`.mtl` files, and no
Play-in-Editor. About a dozen labels change and four or five tabs are added; the viewport painting stays.

**Keep:** the docks (07 §2 T01); today's menu bar, Property/Value grid and `LOCAL` accent (07 §1.3); the
viewport toolbar (projection, Lit, W/E/R, space, FOV), selection outline and gizmo (07 §1.5); one
selection shown in every panel (07 §1.2); readable asset paths rather than raw `guid:…`; Saltmarch on
Harrow, the plan's own setting (01 §4.2), with no third-party IP, inside the container's 1,800 m radius
(02 §5.6).

## Must

- [ ] **Outliner scope.** The outliner scopes to frames (07 §1.5) and the MVP hierarchy is
  Tallis → Harrow → Saltmarch (07 §2.1); Saltmarch is a container on `body:harrow` in zone `tallis`
  (02 §5.6). Add the breadcrumb `Tallis › Harrow › saltmarch` and make the root `saltmarch  container`
  (not `Harrow_Saltmarch world`).
- [ ] **No sun or sky rows.** The sun is the star (03 §5.3) and the sky is Harrow's `AtmosphereDef`
  (03 §5.7). Delete `Lighting`, `sun` and `sky`; add a link `Atmosphere: harrow ↗`.
- [ ] **Terrain row.** Rename `Terrain` to `saltmarch_stamp  terrain stamp` (ED-2). Type every grouping
  row `group` (T01's "group" over `ChildOf`), not `folder`.
- [ ] **Play-in-Editor strip** under the menu, the core loop (07 §1.6, §4.2; ED-2 needs "PIE with 2
  clients and 4 bots"): `▶ Play · Simulate · ■ Stop · Possess │ Clients 2 ▾ Bots 4 ▾ NetSim: good ▾`,
  plus a greyed `Cells 1 ▾ (Ph3)`. Keys: Play F5, Simulate Alt+F5, Stop Shift+F5, Possess F8.
- [ ] **Inspector header and path.** Each entity is a GUID-named `.hent` in `saltmarch.entities/`
  (02 §5.6). Header `rock_03 … Entity` (not `Static Mesh`); path
  `content/zones/tallis/saltmarch.entities/7b2d4c1e-….hent` (not `…/rock_03.ent`).
- [ ] **Asset fields.** Meshes are glTF with a `.meta` sidecar (07 T24) and materials are
  `.hmat`/`.hmati` (07 T16; `.hmati` does exist): `mesh salt_rock_03.glb`, `material salt_rock.hmati`
  (no `*.mesh`, `.mtl`).
- [ ] **Asset Browser.** Sources live in `content/` and worlds are zones (07 §1.8.2): tree
  `content › env, materials, prefabs, records, zones` (no `assets/`, `scenes/`); tiles show the name
  plus a `Mesh` chip.

## Should

- [ ] **Outliner layers.** A `Layers` tab with editor layers (eye and lock toggles) and data layers
  (`base ✓`, `event.drone_raid ☐`); split the Details `layer` row into `editorLayer` and `dataLayers`;
  title the tab `Outliner` (07 §2, §2.1).
- [ ] **Outliner content for ED-2:** `landing_pad_a`, `hollow_drone_spawner  spawner`, `navmesh`, and
  `42 entities` in the header (ED-2 needs a landing pad, a drone spawn region with navmesh and ≥ 40
  entities).
- [ ] **Viewport frame and position:** a frame chip `⌖ Harrow › saltmarch ▾` and a planet readout
  `12.4° N 33.1° W · alt 2 m` (07 §1.5).
- [ ] **Viewport modes:** `Select | Volumes | Terrain | Scatter | Splines | Blockout` and a
  `Mode options` tab; T01 is "the hub hosting every mode" (07 §2.1).
- [ ] **Inspector title.** 07 §2 calls this panel `Details`: retitle it, or ask for the plan row to change.
- [ ] **Inspector badges and units:** the green `client` badge on Static Mesh and Rendering (07 §1.4;
  today's grid has it); `-812.4 m 1.6 m 347.9 m` and `0.0° 26.0° 0.0°`; a read-only row
  `frame  saltmarch (Harrow)`.
- [ ] **Prefab state** (07 §1.4, 02 §4.5): a `prefab  salt_rock_03.hprefab ↗` row, inherited values
  dimmed, an override bar with ↺ on rotation Y.
- [ ] **Bottom dock tabs:** `Asset Browser | Output | Issues ▲1 | History` (Issues is in 07 §2's shared
  bottom row; History exists today, 07 §4.2).
- [ ] **Status bar:** `LOCAL | cinder-reach | saltmarch | 1 unsaved | rock_03 | dark | 100%`
  (`utest-fixture` is a misspelt test-fixture name; the reference game is `cinder-reach`, 08 §2.10.1);
  the unsaved count in the dirty colour and `rock_03 *`; a stale-data chip `⟳ Stale: nav 2 · HLOD 1`
  (07 §4.1.1); `git main · 2 changes ▾` and a lock badge on one tile (07 §1.7).
- [ ] **Collaboration, as a second frame (Ph2–3; 07 §1.8, T30):** the amber accent with
  `SHARED · zone-tallis`; other users' outlines with name tags; `Participants` and `Locks / Notes` tabs;
  `Behind main: 4 commits`.
- [ ] **Fonts.** The monospace font copies the stopgap bitmap font the editor used before PR #42. The
  plan's fonts are Roboto and Roboto Mono (07 §1.3), and the editor has drawn with Roboto 2.138 since
  PR #42: Roboto at about 15 px for the chrome, Roboto Mono only for paths and GUIDs.

## Nice

- [ ] Inspector: `▼` on expanded sections and no arrow on `enabled`; in asset fields a type glyph, a `…`
  picker and a ↗ locate button instead of the folder icon (07 §1.4).
- [ ] Outliner: a mountain glyph for the terrain stamp (the current one reads as ⚠, and severities are
  shapes, 07 §4.3); one name for the rock (`rock_03` or `salt_rock_03`); rows for the derrick and the
  conveyor.
- [ ] Toolbar: Move highlighted while the gizmo shows; `Space: Local` instead of `Local ▾`;
  `Grid 1.00 m · Angle 15° · Scale 0.1`; label `XYZ ▾` or drop it (07 §1.5).
- [ ] Viewport: a faint grid, a `spawn_player` icon, a view cube; a dark halo on the selection outline so
  it reads against the white salt.
- [ ] Asset Browser: separate tile and list icons; a labelled slider.
- [ ] Chrome: move `FUTURE T01 CONCEPT` into a caption outside the viewport.
- [ ] Content: optionally redraw the tan block ruins as derelict hull sections (the Hollow, 01 §4.2).

Where the reviewers disagreed: PIE stays a must because ED-2 depends on it; fonts and badges are should,
being omissions rather than data-model errors; the focus-ring, axis-letter and check-box requests were
dropped because the plan does not require the art to show them.

## For v02's sidecar

- [ ] Record its provenance in `t01-world-saltmarch-v02.<ext>.concept.jsonc` from the template, before
  it is committed (README "Adding an image").
- [ ] Suggested statuses, for the owner to set: the image `directional`; the viewport painting
  `mood-only` ("Ph3–4 look": Phase 1 has only a 2D cloud shell, AAA-REN-6).
- [ ] A high-contrast variant (07 §1.3, §4.4); later, dusk and night-rain variants that match the
  03 §4.6 and §5.8a golden images.
- [ ] Keep Harrow and Saltmarch out of engine and app golden images: the IP lint allows those names only
  under `content/` and `docs/concept/`.

## Today against the concept

Today's editor (PRs #41 and #42) has docking, a property grid with units, enums, groups and client/server
badges, History and Output, the status bar, dark and high-contrast themes, and Roboto as its UI font. The
viewport is still empty.

| Missing | Delivered by |
|---|---|
| Viewport rendering, gizmos, outliner, frames, `.hent`, editor layers, prefabs, asset browser | WP-1.17 |
| glTF import | WP-1.4 |
| PIE | WP-1.18 |
| Issues (T28), materials (T16), terrain (T04) | WP-1.19 |
| Data layers | WP-2.1 |
| Other T01 modes, T30 presence | WP-2.11 |
| Source control | WP-2.12 |
| Edit instances, multi-cell PIE | WP-3.7 |
| Saltmarch content | WP-1.22 |
| Vegetation and weather in the painted look (Phase 1 has only a 2D cloud shell, AAA-REN-6) | Ph3, WP-3.5 |
| Volumetric clouds | Ph4, WP-4.1 |
| Roboto Mono for paths and GUIDs | With the code pane (T10, T19), per the editorui README |
