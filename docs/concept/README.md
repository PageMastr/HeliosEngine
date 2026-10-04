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
- 01 §5.2 also rules out other franchises' names, species, factions, silhouettes and designs.

The rules of this folder (the owner's, 2026-10-04), which apply the plan's rules to concept art:

- An image is **made by the owner**, **commissioned** with the rights assigned to the owner, **CC0**
  (with the address it came from), or **AI-assisted** (below).
- **Never** a screenshot or capture of a commercial game, or of another engine's or editor's UI. Never use
  such an image as an input to an AI generator or as a base to paint over either.
- **AI-assisted** images are allowed only when the sidecar records the tool, the model, the full prompt
  and every input image (each one made by the owner, commissioned, CC0, or an earlier AI-assisted image
  in this folder). This is how this folder records the "01 §5.2 provenance" that 09 §4.2 asks for.

A recommendation, not a plan rule: do not name another game, studio or living artist in a prompt. The IP
lint (below) rejects other franchises' names and titles anywhere in this folder, prompts included.

**Licence.** Images are committed under the repository's MIT licence ([`LICENSE`](../../LICENSE)): the
owner's own work and commissioned work whose rights were assigned to the owner. A CC0 image keeps
`CC0-1.0`. Both are on 01 §5.2's list.

## Layout and naming

```
docs/concept/
  README.md                    this file: rules and index
  TEMPLATE.concept.jsonc       the sidecar template, field by field
  editor/  client/  launcher/  world/
    <tool-or-area>-<subject>[-<variant>]-vNN.<ext>     e.g. editor/t01-world-saltmarch-v02.webp
    <image>.concept.jsonc                              its sidecar, e.g. t01-world-saltmarch-v02.webp.concept.jsonc
    <tool-or-area>-<subject>.review.md                 the review, shared by all versions
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
  misnamed, and an image whose concept is not in the index below.
- **The IP-name lint** ([`tools/lint/ip_names.cmake`](../../tools/lint/ip_names.cmake)) scans this folder
  like `content/`: *Cinder Reach* names are allowed here, other franchises' names and titles are not.

Both run in `ctest -L lint` and in `cmake -P tools/ci/run_lints.cmake`. A lint cannot check who set a
status, whether a version was overwritten, or whether an image is what its sidecar says; the PR review
does.

## Adding an image

1. Export it within the limits and name it as above.
2. Copy `TEMPLATE.concept.jsonc` to `<image>.concept.jsonc` next to it and fill in every field. The
   sha256 comes from `cmake -E sha256sum <image>`.
3. Add or update the concept's row in the index.
4. Run `cmake -DSOURCE_DIR=. -P tools/lint/concept_refs.cmake` and the IP lint (or all of
   `cmake -P tools/ci/run_lints.cmake`).

## Index

| Concept | Area | Latest version | Status | Phase | Review |
|---|---|---|---|---|---|
| `t01-world-saltmarch`: T01 World Editor on the Saltmarch container | editor | none (v01 is not committed: its provenance is not recorded) | v02 pending from the owner | Ph1 layout | [review](editor/t01-world-saltmarch.review.md) |
