# content/ — *Cinder Reach*, the reference game

The sample game that ships with Helios (01 §4.2): an original setting, a frontier cluster of five star
systems reachable again through the reawakened Lattice. This folder is its content root, as
[`helios.project.jsonc`](../helios.project.jsonc) names it (`contentRoots`). WP-0.20 laid down the skeleton
that Phase 1 grows into the M1 "Descent" slice: the `tallis` zone with the Saltmarch container, and the
Kestrel hull record.

## Layout

```
helios.project.jsonc                 the project file, at the project (repository) root (09 §2.7.2)
content/
  .gitattributes                     LF everywhere: content is canonical JSONC, compared byte for byte (07 §1.7)
  README.md (+ .meta)                this file
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
  its own, so no schema type was added.
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

Every file under `content/` has a `.meta` sidecar next to it (`kestrel.hrec.meta`), in engine/assetpipe's
format and checked by its rules ([engine/assetpipe](../engine/assetpipe/README.md#meta-sidecars)): a
GUID, the document type (`hrec`, `hcont`, `hent` or `md`), and `provenance` with `origin`, `author` and
an allowed licence (MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Zlib, BSL-1.0, ISC, PostgreSQL, CC0-1.0).

- **origin** is `original` (written for the project by a contributor), `commissioned` (with `rights`),
  `cc0` (with the `url` it came from, licence `CC0-1.0`) or `ai-assisted` (with `ai`: the generator, the
  model, the full prompt and every input). Text a contributor writes, a person or a coding agent, is
  `original`; its pull request discloses agent authorship (README, "AI contributions"). Generator output
  (images, meshes, audio, generated text bodies) is `ai-assisted`.
- **Never** anything extracted from a commercial game, not even as a test fixture, and no names, species,
  factions, silhouettes or designs of other franchises (01 §5.2). The IP-name lint scans this folder.
- **One identity per document.** A container's or entity's sidecar carries that document's own GUID
  (`$container`, `$entity`). A record's sidecar has its own GUID; the record's identity is its `$rid`.

## Checks

| CTest (label `lint`) | What |
|---|---|
| `lint_content_check` | `helios-cook check --project-root=.`: the project file (fails closed on unknown keys) and every file under `content/`: a known document type with a valid sidecar (100 % provenance), no hidden files, spatial documents only in declared zone folders and nowhere else, records under `records/`, and each container's and entity's identity, name, frame parent and streaming group |
| `lint_content_records_cook` | `helios-cook records --project-root=content`: the records compile (02 §3.3 checks, AAA-SEC-4 split) |
| `lint_content_records_canonical` | `helios-tool --project-root=content fmt --check`: the records are in the canonical form the editor writes |
| `lint_records_sec4_content` | AAA-SEC-4 on these records: the Kestrel's server-only `aiHints` keys are in the server cook and not in the client cook |
| `lint_content_check_fixture_*` | 34 copies of the project with one seeded violation each (among them a missing sidecar, a disallowed licence, an `ai-assisted` origin without its record, an unknown file type, a hidden file, an undeclared zone folder, a container outside its zone, a record in a zone folder, a wrong streaming group, a sidecar GUID that differs from `$container`, an entity named for another GUID, an unknown or duplicate project key, a 61 Hz zone, a reserved URI scheme and a record the cook refuses) must fail with its diagnostic; `valid`, the unchanged copy, and `cook_then_check` must pass ([helios-cook README](../apps/tools/helios-cook/README.md) lists them all) |
| `lint_ip_names` | No other franchise's names or titles here (01 §5.2) |

`cmake -P tools/ci/run_lints.cmake` runs the IP-name lint without a build.

## Adding content

1. Write the file where the layout above puts it. A record: a new random `$rid` (63-bit, non-zero) and a
   `$name` of `<table>/<name>`; then `helios-tool --project-root=content fmt` for the canonical form.
   A container or entity: a new random GUID (lower case) in `$container` (`"guid:<GUID>"`) or `$entity`
   and the entity's file name.
2. Write its `<file>.meta` with a new GUID (the container's or entity's own), the document type and the
   provenance. A new kind of file needs its document type in `helios-cook` first
   (`apps/tools/helios-cook/content_check.cpp`, `contentTypes()`): until then the check refuses it, so no
   file can arrive without a way to record where it came from.
3. Run `helios-cook check --project-root=.` and `helios-cook records --project-root=content`, or
   `ctest -L lint`.

Binary sources (art, audio, meshes) are not in the repository yet (the owner deferred them); when they come,
they go through Git LFS and the importers of 07 T24, with the same sidecars.

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (01 §4.2, §5.2; 02 §3.3, §3.8, §5.5, §5.6; 06 §8; 07 §1.7, §1.8.2, T24; 08 §2.10.1;
09 §2.1 WP-0.20, §2.7.2) on 2026-10-05. Choices the plan leaves open, recorded here and in the WP-0.20 PR:
`.meta` sidecars for text documents (records, containers, entities, Markdown) with the document type as the
importer id; `original` for contributor-written text, agent-written included; the project file's field names
(`$project`, `contentRoots`, `zones[]` with `name`, `id`, `tickHz` and `frame`, and the product stub); the
GUID forms of 02 §5.6's example (`"guid:<GUID>"` for `$container`, a bare GUID for `$entity`).
