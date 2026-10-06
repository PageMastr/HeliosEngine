# content/ — *Cinder Reach*, the reference game

The sample game that ships with Helios (01 §4.2): an original setting, a frontier cluster of five star
systems reachable again through the reawakened Lattice. This folder is its content root, as
[`helios.project.jsonc`](../helios.project.jsonc) names it (`contentRoots`). WP-0.20 laid down the skeleton
that Phase 1 grows into the M1 "Descent" slice: the `tallis` zone with the Saltmarch container, and the
Kestrel hull record. WP-0.20b added the starter art: 35 CC0 placeholder files in Git LFS, listed in
[`ASSETS.md`](ASSETS.md).

## Layout

```
helios.project.jsonc                 the project file, at the project (repository) root (09 §2.7.2)
content/
  .gitattributes                     LF for the text types (.hrec .hcont .hent .meta .md .jsonc): canonical
                                     JSONC is compared byte for byte (07 §1.7); Git LFS for the binary types
                                     (.glb .png .exr, `lockable`); not content, so no sidecar
  README.md (+ .meta)                this file
  ASSETS.md (+ .meta)                every binary source: author, source, licence as stated, bytes, SHA-256
  art/                               binary sources (Git LFS), placeholders until Phase 1's art
    ships/kestrel/                   the Kestrel's placeholder hull (.glb)
    scree/                           rocks for the Scree belt (.glb)
    saltmarch/kit/                   modular pieces for Saltmarch (.glb) and their atlas, Textures/colormap.png
    materials/<name>/                PBR maps (.png): hull_plates, station_panels, harrow_regolith, harrow_rock
    environment/                     the sky over Harrow (.exr, equirectangular)
  records/<table>/<name>.hrec        records, one per file, typed by the record type's @table (02 §3.3)
    hull/kestrel.hrec                the Kestrel light fighter (sample.ship.ShipHullDef)
  zones/<zone>/                      one folder per zone of helios.project.jsonc; spatial documents only
    tallis/                          Tallis space and the Harrow surface: one zone (02 §5.5)
      saltmarch.hcont                the Saltmarch outpost, an object container (02 §5.6)
      saltmarch.entities/<GUID>.hent one file per placed entity (OFPA)
  <every file>.meta                  its provenance sidecar (below)
```

- **Records** are canonical JSONC (02 §3.7): `$rid` (a random non-zero 63-bit id, minted once and never
  reused), `$name` (`<table>/<name>`), optional `$parent` and `$comment`, then fields in schema order with
  defaults left out. `helios-tool --project-root=content fmt` writes that form; the editor's saves do too.
  Today's record types are the native `sample` package (`schemas/sample`, listed in the project file's
  `schemas.native`); the Kestrel is a `ShipHullDef` (02 §3.1's ship record). 06 defines no ship record of
  its own, so no schema type was added. Its `faction`, `Pilots`, is a placeholder from the sample enum
  `sample.common.Faction` (Neutral, Pilots, Syndicate, Drones): 01 §4.2's factions (the Meridian
  Directorate, the Free Compact, the Hollow) have no type yet, so the Kestrel names none of them.
- **Zones and containers** follow the owner's "Option A" of 2026-10-04 (02 §5.5, recorded by #51): Tallis space
  and the Harrow surface are the one zone `tallis` (zone id 1002 at 20 Hz, the cell's standalone default),
  and Saltmarch is an object container in it, not a zone. Its container sits in the zone's folder, so it is
  edited in the zone's session, `zone-tallis` (07 §1.8.2). Its frame parent is the planet Harrow
  (`body:harrow`), and the zone's root frame is the Tallis system (`system:tallis`, the project file's
  `zones[].frame`): the hierarchy Tallis → Harrow → Saltmarch. The bodies themselves (Harrow's 1,500 km
  radius, spin, atmosphere) belong in a `StarSystemDef` record (02 §5.8, W06), which has no schema yet, so
  nothing declares `body:harrow` beyond the container's reference to it.
- **Entities** are named by their GUID, which is also their `$entity` and, through `hash62`, their
  content-placed `EntityId` (02 §4.1). The one entity today is the outpost's origin marker; its components
  are not validated until the container loader exists (Phase 1).

## Provenance (01 §5.2, 07 T24)

Every file under `content/` except the root's `.gitattributes` (git settings, not content; a deeper one
fails the check) has a `.meta` sidecar next to it (`kestrel.hrec.meta`), in engine/assetpipe's format and
checked by its rules ([engine/assetpipe](../engine/assetpipe/README.md#meta-sidecars)): a GUID, the
document type (`hrec`, `hcont`, `hent` or `md`), and `provenance` with `origin`, `author` and an allowed
licence (MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Zlib, BSL-1.0, ISC, PostgreSQL, CC0-1.0).

- **origin** is `original` (written for the project by a contributor), `commissioned` (with `rights`),
  `cc0` (with the `url` it came from, licence `CC0-1.0`) or `ai-assisted` (with `ai`: the generator, the
  model, the full prompt and every input). Text a contributor writes, a person or a coding agent, is
  `original`; its pull request discloses agent authorship (README, "AI contributions"). Generator output
  (images, meshes, audio, generated text bodies) is `ai-assisted`.
- **Never** anything extracted from a commercial game, not even as a test fixture, and no names, species,
  factions, silhouettes or designs of other franchises (01 §5.2). The IP-name lint scans this folder.
- **Third-party files** are `cc0` only today: their own page states CC0-1.0 or a public-domain dedication,
  and nothing in them (the model, the texture, a name, embedded metadata) shows a franchise design, a brand,
  insignia or a real person. A file under another allowed licence (MIT, BSD, Apache-2.0, ...) has no origin
  in engine/assetpipe's provenance yet, so it cannot be added; attribution (CC-BY), share-alike, NC, ND,
  "royalty free" and store licences are not on 01 §5.2's list at all. The sidecar's `notes` record the
  download URL, the retrieval date, the archive's SHA-256, the file's path in it and the licence as stated;
  [`ASSETS.md`](ASSETS.md) has a row per file with its bytes and SHA-256.
- **One identity per document.** A container's or entity's sidecar carries that document's own GUID
  (`$container`, `$entity`). A record's sidecar has its own GUID; the record's identity is its `$rid`.

## Checks

| CTest (label `lint`) | What |
|---|---|
| `lint_content_check` | `helios-cook check --project-root=. --require-git`: the project file (fails closed on unknown keys) and every file under `content/`: a known document type with a valid sidecar (100 % provenance), no hidden files, links or upper-case extensions, spatial documents only in declared zone folders and nowhere else, records under `records/`, each container's and entity's identity, name, frame parent and streaming group, each binary source a Git LFS pointer or a file of its type, each text document (records, containers, entities, Markdown) UTF-8 without NUL bytes, and in git: nothing tracked as cook output, every binary source stored in LFS |
| `lint_content_assets` | [`tools/lint/content_assets.cmake`](../tools/lint/content_assets.cmake): every binary source has its row in `ASSETS.md`, and each row's bytes and SHA-256 match the file, or its LFS pointer's `size` and `oid` in a checkout without the objects |
| `lint_content_records_cook` | `helios-cook records --project-root=content`: the records compile (02 §3.3 checks, AAA-SEC-4 split) |
| `lint_content_records_canonical` | `helios-tool --project-root=content fmt --check`: the records are in the canonical form the editor writes |
| `lint_records_sec4_content` | AAA-SEC-4 on these records: the Kestrel's server-only `aiHints` keys are in the server cook and not in the client cook |
| `lint_content_check_fixture_*` | 110 copies of the project: 104 with one seeded violation each, at least one per rule the check enforces (among them a missing sidecar, a disallowed licence, an attribution licence on a model, a `cc0` sidecar without its URL, an `ai-assisted` origin without its record, an unknown file type, a hidden file, a link, a `.gitattributes` below the root, a renamed file of another type, a malformed LFS pointer, a binary or cook output tracked outside LFS, an undeclared zone folder, a container outside its zone, a record in a zone folder, a wrong streaming group, a sidecar GUID that differs from `$container` or `$entity`, an entity named for another GUID, an unknown or duplicate project key, a 0 Hz or 61 Hz zone, a reserved URI scheme and a record the cook refuses) must fail with its diagnostic; six must pass: `valid` (the unchanged copy), `cook_then_check`, `ignored_cache`, `binary_real_files`, `git_lfs_valid` and `text_utf8_chunk_boundary` ([helios-cook README](../apps/tools/helios-cook/README.md) lists them all) |
| `lint_content_assets_fixture_*` | Small generated content folders: `ok` must pass; a binary without a row, a wrong hash, a wrong byte count, a pointer whose `oid` differs, a row for a missing file, a duplicate row, a row for a text document and a malformed SHA-256 must each fail |
| `lint_ip_names` | No other franchise's names or titles here (01 §5.2) |

`cmake -P tools/ci/run_lints.cmake` runs the IP-name lint and `content_assets` without a build.

## Git LFS

Binary sources are Git LFS objects (07 §1.7); the text documents, sidecars included, are plain git.

- **Working with the art.** Install git-lfs (Git for Windows bundles it) and run `git lfs install` once;
  a clone then has the real files. `lockable` makes them read-only until you take the lock:
  `git lfs lock content/art/<path>`, edit, commit, push, `git lfs unlock`.
- **Without the art.** `GIT_LFS_SKIP_SMUDGE=1 git clone ...` (or a machine without git-lfs) checks out the
  pointer files instead: three lines with the object's SHA-256 and size. The checks and the cook accept
  them, so building and testing never need the objects. CI checks out pointers only (`actions/checkout`'s
  default `lfs: false`) and fetches no LFS object.
- **Shared clones.** The first time its filter runs (and on `git lfs install`), git-lfs writes its
  `pre-push`, `post-checkout`, `post-commit` and `post-merge` hooks into the repository's hooks directory,
  which every worktree of the clone shares, even when the filter is configured only per command. Its
  `pre-push` stops `git push` wherever git-lfs is not on PATH. In a clone that several people or tools use,
  either all of them have git-lfs on PATH or the LFS work sets `core.hooksPath` to a directory of its own.
- **Budget.** The starter set is 35 files, 32.7 MB (31.2 MiB; the largest file 6.1 MB), within WP-0.20b's
  budget of 50 assets, 15 MB each and 120 MB in all (GitHub's LFS quota).

## Adding content

1. Write the file where the layout above puts it. A record: a new random `$rid` (63-bit, non-zero) and a
   `$name` of `<table>/<name>`; then `helios-tool --project-root=content fmt` for the canonical form.
   A container or entity: a new random GUID (lower case) in `$container` (`"guid:<GUID>"`) or `$entity`
   and the entity's file name. Text documents are UTF-8 with LF line ends; the check refuses a NUL byte or
   invalid UTF-8 in one, so a binary file cannot pass for a text document.
2. Write its `<file>.meta` with a new GUID (the container's or entity's own), the document type and the
   provenance. A new kind of file needs its document type in `helios-cook` first
   (`apps/tools/helios-cook/content_check.cpp`, `contentTypes()`): until then the check refuses it, so no
   file can arrive without a way to record where it came from.
3. Run `helios-cook check --project-root=.` and `helios-cook records --project-root=content`, or
   `ctest -L lint`.

A binary source (`.glb`, `.png`, `.exr`; lower-case extension) also needs git-lfs installed before
`git add`, so that git stores a pointer (the check refuses a binary that git stores itself), and its row in
[`ASSETS.md`](ASSETS.md) with the committed file's byte count and SHA-256 (`cmake -E sha256sum <file>`).
A third-party file follows the rules under Provenance above: its own page states CC0, you look at it (and
at its preview) before adding it, and its sidecar quotes where it came from. A new binary type needs its
document type in `helios-cook` and its LFS rule in `.gitattributes` first. Phase 1's importers (07 T24,
WP-1.4) read these files; until then they are sources only.

## Plan conformance

Plan-Rev: 14

Written for plan revision 13 (01 §4.2, §5.2; 02 §3.3, §3.8, §5.5, §5.6; 06 §8; 07 §1.7, §1.8.2, T24; 08 §2.10.1;
09 §2.1 WP-0.20, §2.7.2) on 2026-10-05; the starter art, Git LFS and `ASSETS.md` for revision 14 (01 §3.9.1's
"≤ 50 new placeholder or CC0 assets", §4.2, §5.2; 07 §1.7, T24; WP-0.20b) on 2026-10-06, with these choices:
`art/` grouped by use, the source files' own names kept so that each traces to its archive, files committed
byte for byte as downloaded (their embedded tool metadata included), `cc0` as the only third-party origin,
and a manifest lint for `ASSETS.md`. WP-0.20's choices, recorded here and in the WP-0.20 PR:
`.meta` sidecars for text documents (records, containers, entities, Markdown) with the document type as the
importer id; `original` for contributor-written text, agent-written included; the project file's field names
(`$project`, `contentRoots`, `zones[]` with `name`, `id`, `tickHz` and `frame`, and the product stub); the
GUID forms of 02 §5.6's example (`"guid:<GUID>"` for `$container`, a bare GUID for `$entity`); the Kestrel's
placeholder `faction` (`Pilots`, a value of the sample enum, until 01 §4.2's factions have a type).
