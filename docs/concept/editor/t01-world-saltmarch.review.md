# T01 World Editor on the Saltmarch container: concept review

- **Concept:** `t01-world-saltmarch` (index: [README](../README.md#index)).
- **History:** v01 reviewed 2026-10-04; v02 reviewed 2026-10-04 (two frames: Outliner tab, Layers tab);
  v03 pending from the owner.
- **Not committed.** Neither v01 nor v02 is in the repository: their provenance is not recorded yet
  (01 §5.2; [README](../README.md#where-images-may-come-from)). v03 comes with its sidecar.
- **Decided 2026-10-04 by the owner ("Option A"):** Saltmarch is an object container in the single
  Phase 1 zone `tallis` (07 §2.1 MVP). Its files are `content/zones/tallis/saltmarch.hcont` and
  `content/zones/tallis/saltmarch.entities/<guid>.hent`, and a shared editing session on it is
  `zone-tallis` (07 §1.8.2). The plan records the decision in 02 §5.5–5.6 and 07 §1.7.1 (PLAN-REV 13,
  PR #51; [decision record](../../evidence/saltmarch-container-owner-decision-2026-10-04.md)).

Section numbers are plan sections (`docs/plan/`). Tick items off as v03 covers them.

## Verdict on v02

Close. All but one of v01's musts are fixed, and most of its shoulds. Two text errors are still musts.
After the musts and shoulds below, v03 can be committed as the **directional** T01 reference with the
viewport painting **mood-only**. Its phase target is **Ph2**: data layers, Scatter/Splines/Blockout and
the git chip all arrive in Ph2 (07 §5.1).

## Fixed in v02

Outliner breadcrumb `Tallis › Harrow › saltmarch`, root `saltmarch  container`, `Atmosphere: harrow ↗`
instead of sun and sky, a `terrain stamp` row, `group` rows; the PIE strip (07 §1.6; ED-2); Inspector
`Entity: rock_03` with Frame and Source prefab rows, transform units, a `client` badge and a GUID-named
`.hent` with a copy button; real formats (`.glb`, `.hmati`, `.hprefab`) and a `content/` tree; a Layers
tab with Editor and Data layers (07 §2.1); the viewport frame chip, planet readout and mode switch; the
bottom dock `Asset Browser | Output | Issues | History`; `cinder-reach`, branch and stale-data chips in
the status bar; Roboto-style chrome with mono only for the path and GUID; ▼ on open sections, ↗ locate
buttons, and the concept label moved out of the viewport.

## Open for v03

### Must

- [ ] **Inspector › Source path.** `content/zones/saltmarch/…` breaks the 2026-10-04 decision and T28
  `collab.scope` ("every spatial document sits under its zone's `content/zones/<zone>/` folder",
  07 §1.8.2). Use `content/zones/tallis/saltmarch.entities/a7f3c9e4-2d1b-4e66-8ab7-3c9f1d2e8a6c.hent`,
  wrapped only at `/` so the GUID is never split.
- [ ] **Viewport readout.** `Lat 18.42° Lon -31.08° Alt 124 m` is about 166 km from Saltmarch's anchor
  (`"lat": 12.4, "lon": -33.1`, radius 1,800 m, 02 §5.6; Harrow's radius is 1,500 km, 01 §4.2), beyond
  even the 40 km `hlodRadius`. Use `Lat 12.41°  Lon -33.13°  Alt 4 m`.

### Should

- [ ] **Inspector › `Inherited (from prefab)` grid:** delete it. Transform is instance data, not an
  override: instances "store only transforms and overrides" (02 §4.5) and the `.hent` keeps `Transform`
  beside `$overrides` (02 §5.6). Show values inherited from the prefab dimmed at the normal 15 px size,
  and mark one real override with a bar and a per-row ↺, e.g. `Cast Shadows ☐ Off ▌↺`.
- [ ] **Inspector › Mesh:** `salt_rock.glb` does not match the highlighted tile. Use
  `env/rocks/salt_rock_03.glb` and `salt_rock_03.hprefab`.
- [ ] **Right dock:** add a `Mode options` tab (07 §2 T01 row: "Details, Mode options"). Then either
  retitle the dock `Details`, or ask for the plan row to say Inspector, as today's shell does.
- [ ] **Inspector › layer membership:** add `Editor layer  props ▾` and `Data layers  [base]`.
- [ ] **Viewport toolbar:** projection, view mode and W/E/R are gone (07 §1.5 "View modes"; §4.2). Add
  `Perspective ▾ · Lit ▾ · Q W E R · Space: Local ▾ · FOV 60° ▾`, with Move highlighted.
- [ ] **Outliner selection:** expand `Rocks`, highlight `rock_03` (07 §1.2), and put `42 entities` in
  the header (ED-2: ≥ 40 entities).
- [ ] **Outliner types:** `prefab instance` for `landing_pad` and the rocks (02 §4.5 `$prefab`), and
  `hollow_drone_spawner  spawn region` (07 T23: spawn regions are T01 entities).
- [ ] **Outliner `navmesh` row:** drop it. Nav tiles are derived data baked into containers (07 T23,
  §4.1.1), shown through the navmesh view mode. This corrects v01's own advice.
- [ ] **Layers toggles:** editor layers get eye, padlock and swatch under a `Show · Lock` header; data
  layers get an eye and a `PIE` checkbox under `Show · PIE`. Every toggle has a tooltip.
- [ ] **Layers › `drone raid`:** the container manifest uses IDs (02 §5.6): show `event.drone_raid` and
  keep the `event` chip.
- [ ] **PIE strip:** `NetSim: good ▾`, plus a greyed `Cells 1 ▾` with a Ph3 tooltip (07 §1.6).
- [ ] **Status bar:** `▌LOCAL | cinder-reach | saltmarch | 1 unsaved | rock_03 * | … | dark | 100%`,
  the dirty items in the dirty colour (07 §1.3 "neutral = local"; today's shell shows all three).
- [ ] **Status bar stale chip:** `⟳ Stale: nav 2 ▾`, opening a list with Rebuild now, instead of
  `⚠ Stale data (12m)`: the chip counts stale products per kind (07 §4.1.1) and ⚠ is a severity shape
  (§4.3). Leave HLOD out; HLOD rebuilds are Ph3.
- [ ] **Asset Browser:** an LFS lock badge `salt_rock_04.glb 🔒 MK` (07 §1.7) and a small `Mesh` chip
  under each tile name.
- [ ] **Shared-editing frame** (T30): still not drawn; requested as image 11 in the
  [index](../README.md#requested).

### Nice

- [ ] **Phases:** label Scatter, Splines, Blockout and Data layers `Ph2`, or state the phase in the
  sidecar.
- [ ] **Glyphs:** a mountain glyph for terrain (it still reads as ⚠, 07 §4.3), from one permissive icon
  font, for example Lucide (ISC).
- [ ] **Viewport overlays:** a faint grid, a view cube, a spawner icon and a dark halo around the
  selection outline (07 §1.5).
- [ ] **Snapping:** `Grid 1.00 m · Angle 15° · Scale 0.1`; label `Axis: XYZ ▾` or drop it.
- [ ] **Names:** `›` in the Inspector Frame row; rename the editor layer `base`, which clashes with the
  data layer `base`; `main · 4 changes ▾` instead of `+3 ~1` (its red reads as a conflict).
- [ ] **PIE buttons:** Stop and Possess greyed while idle; F5, Alt+F5, Shift+F5 and F8 in tooltips.

**How the reviewers' disagreements were settled:** the readout stays a must because it contradicts
02 §5.6; transform is not marked as an override because the plan stores it as instance data; the terrain
glyph stays nice, as v01 rated it.

## For v03's sidecar

- [ ] Fill `t01-world-saltmarch-v03.<ext>.concept.jsonc` from the template before committing the image:
  who made it, the tools, the AI model, exact prompt and every input image if used (none may be a
  screenshot of a commercial game or editor), and the licence (MIT or CC0-1.0).
- [ ] Suggested, for the owner to set: status `directional`; element `viewport painting` `mood-only`
  ("Ph3–4 look": Phase 1 has only a 2D cloud shell, AAA-REN-6); phase `Ph2`.
- [ ] Keep Harrow and Saltmarch out of engine and app golden images: the IP lint allows those names only
  under `content/` and `docs/concept/`.

## Today against the concept

Today's editor (PRs #41 and #42) has docking, a property grid with units, enums, groups and
client/server badges, History and Output, the status bar, dark and high-contrast themes, and Roboto as
its UI font. The viewport is still empty.

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
| Volumetric clouds | WP-3.5 (fast mode); Ph4 REN-6, WP-4.1 |
| Roboto Mono for paths and GUIDs | With the code pane (T10, T19), per the editorui README |
