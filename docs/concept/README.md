# Concept art references

Pictures of where the editor, client, launcher and world are heading, so that the people and agents who
build them can see the target. They guide the work; they do not specify it.

## Read this first

1. **The plan beats the pixels.** Where an image and [the plan](../plan/) disagree, the plan wins. The
   image's review records the conflict, and only a plan change can move the plan towards the image.
2. **Only the owner marks anything binding.** Contributors and agents may propose a status, but a
   `binding` status (for an image or for one element of it) takes effect only when the owner sets it or
   approves the PR that sets it.
3. **On-screen text is illustrative.** Labels, names, numbers and paths drawn in an image show the kind of
   thing that goes there, not the exact text, unless the image's review says otherwise.

## Status labels

Every image has a status, and its sidecar can override it for single elements (a panel, the viewport, a
widget).

| Status | Meaning |
|---|---|
| `binding` | Build it like this. Only the owner sets it. |
| `directional` | Follow the layout and intent; details may change. |
| `mood-only` | Colour, light and feel only. Do not take layout or content from it. |

## Where images may come from

What the plan says:

- "**No extracted assets.** No assets or data come from commercial games, not even as test fixtures.
  Every asset's provenance is recorded in the asset DB." (01 §5.2)
- Agents may produce "generative art only with 01 §5.2 provenance". (09 §4.2)
- Footage of the look-and-feel reference titles "stays with the vendor and never enters the repository or
  the asset DB (§5.2)". (01 §3.3.1)
- 01 §5.2 rules out the names, species, factions, silhouettes and designs of the franchises it lists.
- The [README](../../README.md#legal-and-ip): "Sample content must not include other franchises' names,
  characters, art or audio. Assets you contribute must be your own or carry a compatible licence, recorded
  in their provenance metadata."

The rules of this folder (set at the owner's request, 2026-10-04), which apply the plan's rules to concept
art:

- An image is **made by the owner**, **commissioned** with the rights assigned to the owner, **CC0**
  (with the address it came from), or **AI-assisted** (below).
- **Never** a screenshot or capture of a commercial game, or of another engine's or editor's UI, in whole
  or in part, and never such an image as an input to an AI generator.
- **AI-assisted** images are allowed only when the sidecar records the tool, the model, the full prompt
  and every input image (each one made by the owner, commissioned, CC0, or an earlier AI-assisted image
  in this folder). This is how this folder records the "01 §5.2 provenance" that 09 §4.2 asks for.
- **The sidecar stands in for the asset DB** (this folder's interpretation): 01 §5.2 records provenance
  "in the asset DB", but concept art is not a shipped asset and the asset DB does not exist yet, so each
  image's provenance is recorded in its sidecar.

A recommendation, not a plan rule: do not name another game, studio or living artist in a prompt. The IP
lint (below) rejects the franchise names and titles on its list anywhere in this folder, prompts included.

**Licence.** Images are committed under the repository's MIT licence ([`LICENSE`](../../LICENSE)): the
owner's own work and commissioned work whose rights were assigned to the owner. A CC0 image keeps
`CC0-1.0`. Both are on 01 §5.2's list.

## Layout and naming

```
docs/concept/
  README.md                    this file: rules and index
  TEMPLATE.concept.jsonc       the sidecar template, field by field
  editor/  client/  launcher/  world/
    <tool-or-area>-<subject>[-<variant>]-vNN.<ext>   e.g. t01-world-saltmarch-v02.webp
    <image>.concept.jsonc                            its sidecar: t01-world-saltmarch-v02.webp.concept.jsonc
    <tool-or-area>-<subject>.review.md               the review, shared by all versions
```

- Names are lower case, words joined by `-`, and end in a two-digit version.
- **Never overwrite a version.** A changed image is a new `vNN` with its own sidecar; the old one stays.
- Paths inside a sidecar (`review`, `ai.inputs[].file`) are relative to `docs/concept/`.

## Formats and limits

- PNG for UI; WebP or JPEG (quality about 85) for paintings.
- At most 1 MB (1,048,576 bytes) and 2,560 px on the long side.
- Plain git, no LFS yet; [`.gitattributes`](.gitattributes) marks the images binary. (07 §1.7's LFS layout
  is for project content; the engine repository keeps its few images, like the golden images, in plain git.)

## Checks

- **`lint_concept_refs`** ([`tools/lint/concept_refs.cmake`](../../tools/lint/concept_refs.cmake)) fails
  on an image without a sidecar or with a wrong sha256, a sidecar without an image, a missing or unknown
  sidecar field or a value outside its set, an image over the limits or whose bytes do not match its
  extension, a file type this folder does not take, an image outside the four area directories or
  misnamed, an image whose concept is not in the index's [Concepts](#concepts) table below (by its
  concept name or a file name of the concept; a Requested row does not count), and a missing README,
  Concepts table or `TEMPLATE.concept.jsonc`.
- **The IP-name lint** ([`tools/lint/ip_names.cmake`](../../tools/lint/ip_names.cmake)) scans this folder
  like `content/`: *Cinder Reach* names are allowed here, other franchises' names and titles are not.

Both run in `ctest -L lint` and in `cmake -P tools/ci/run_lints.cmake`. A lint cannot check who set a
status, whether a version was overwritten, or whether an image is what its sidecar says; the PR review
does.

## Adding an image

1. Export it within the limits and name it as above.
2. Copy `TEMPLATE.concept.jsonc` to `<image>.concept.jsonc` next to it and fill in every field. The
   sha256 comes from `cmake -E sha256sum <image>`.
3. Add the concept's row to the Concepts table (moving it out of Requested if it is there), or update
   its latest version, status and phase.
4. Run `cmake -DSOURCE_DIR=. -P tools/lint/concept_refs.cmake` and the IP lint (or all of
   `cmake -P tools/ci/run_lints.cmake`).

## Index

The lint finds a committed image's concept in the Concepts table, by its concept name or a versioned file
name. A Requested row does not count: an image's row moves to Concepts when the image is committed.

### Concepts

| Concept | Area | Latest committed | Status | Phase | Review |
|---|---|---|---|---|---|
| `t01-world-saltmarch`: T01 World Editor on the Saltmarch container | editor | none: v01 and v02 were reviewed, not committed (provenance not recorded) | v03 pending from the owner; proposed `directional`, viewport `mood-only` | Ph2 | [review](editor/t01-world-saltmarch.review.md) |

### Requested

Images the reviews ask for, in priority order (from the [T01 review](editor/t01-world-saltmarch.review.md)
of v02). None exists yet. When one arrives, it moves to the table above.

| # | File | Title | Phase | What it shows | Must show |
|---|---|---|---|---|---|
| 1 | `editor/t01-pie-saltmarch-2clients-v01.png` | T01 PIE running | Ph1 | The v03 layout after F5 | Stop and Possess enabled, client 1 possessed, client 2 in its own window, 4 bots, NetSim per client (lan/good/mobile/awful), drop client / kill cell / TiDi, a process list, `PIE ready 11.8 s` (≤ 15 s cold) |
| 2 | `editor/t02-prefab-landing-pad-v01.png` | T02 Prefabs | Ph1 | `landing_pad` edited in context, which settles the override style | Prefab hierarchy with a nested prefab, breadcrumb, dimmed inherited values, bar + ↺ on `Light/intensity = 1200`, a variant, Where used |
| 3 | `editor/t01-volumes-harrow-high-hangar-v01.png` | T01 Volumes mode | Ph1 (ED-16) | Hangar cells and portals | Mode options, Generate cells at 0.25 m, tinted cells, portals, an airlock with bound doors, cell flags, GridVolume shells 0.5 m inside / 1.0 m outside, transfer probe, Partition: Whole |
| 4 | `editor/t08-records-kestrel-variant-v01.png` | T08 Data | Ph1 (ED-2) | A Kestrel variant record | Tables and Template tree, Grid ⇄ Inspector, fill-down, an HXL formula, CSV, override ↺, client/server_only/service badges, Where used |
| 5 | `editor/t01-simulate-saltmarch-navmesh-v01.png` | Simulate with the navmesh view | Ph1 (ED-3) | A moved wall becomes a nav obstacle | 64 m tiles, hatched stale tiles, `⟳ Stale: nav 2`, a drone's read-only live state, Keep simulation changes, the spawn region |
| 6 | `editor/t01-world-saltmarch-high-contrast-v01.png` | High-contrast theme | Ph1 (ED-15) | v03 redrawn with an unchanged layout | Text at ≥ 7:1, a focus ring, severities and badges told apart by shape |
| 7 | `editor/t28-issues-portal-leak-v01.png` | T28 Validation | Ph1 | The Issues panel on a portal leak | A Rule list (≥ 20 rules), severity by shape and colour, Fix preview and Apply, a leak polyline in the viewport, Suppress… with a written justification |
| 8 | `editor/t24-import-salt-rock-v01.png` | T24 import | Ph1 | A glTF dropped from Explorer | The thumbnail, `.meta` fields (GUID, provenance, licence), LODs, BCn/KTX2, a collision profile (decomposition greyed, Ph2), an import log |
| 9 | `editor/t04-terrain-harrow-saltmarch-v01.png` | T04 Terrain | Ph1 | Harrow's terrain around Saltmarch | The layer stack (Graph greyed, Ph3), four boundary types feathered in metres, a 2D map at 12.4° N 33.1° W, Brush, dirty tiles, the Saltmarch stamp |
| 10 | `editor/t16-material-instance-salt-rock-v01.png` | T16 Material | Ph1 (instances) | The salt-rock material instance | A link to the parent `.hmat`, parameter overrides with ↺, EV/Kelvin colour, a preview rig, PSO stats, the Graph pane greyed (Ph2) |
| 11 | `editor/t30-saltmarch-shared-session-v01.png` | T30 shared editing | Ph2 presence and locks, Ph3 edit instance | Saltmarch in a shared session | Amber accent `SHARED · zone-tallis`, other users' outlines with name tags, lock badges, Participants / Locks / Notes, `Behind main: 4 commits` |

Later, not specified yet: `editor/t07-system-tallis-v01.png`, `launcher/launcher-login-dev-v01.png`,
`client/hud-saltmarch-firefight-v01.webp` and `world/harrow-descent-sunset-v01.webp` (all Ph1); after
those, the cockpit, the concourse, character select, Saltmarch at dusk and Saltmarch in night rain (Ph3).
