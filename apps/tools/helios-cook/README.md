# helios-cook

The cook CLI (07 §1.10 names it beside `helios-tool` and `helios-assetd`). v0 has two verbs: `records`
(WP-0.8 part 2) and `check` (WP-0.20).

```
helios-cook records [--project-root=<content root>] [--out=<dir>] [--quiet] [--log-level=<level>]
helios-cook check   [--project-root=<project>] [--quiet] [--log-level=<level>]
```

`records` reads every `records/<table>/**/*.hrec` under `--project-root`, here a content root such as the
repository's `content/` (typed by the record type with that `@table`), resolves `$parent` inheritance,
checks references, tags, formulas and the client/server split (AAA-SEC-4), and writes `records.client.hrdb`
and `records.server.hrdb` to `--out` (default `<project-root>/.cooked`, which `.gitignore` keeps out of git
for `content/` and `check` skips; before WP-0.20 it was `cooked`, inside the content tree). The rules are
[engine/records](../../../engine/records/README.md)'s. Errors are listed on stderr, sorted by file, and
nothing is written.

`check` reads `--project-root`'s `helios.project.jsonc` (09 §2.7.2) and every file under the content roots
it names, and lists every finding on stderr, sorted by file (`content_check.h` has the rules):

- **The project file.** Every key this version knows is checked and any other fails, so a field the plan
  adds arrives with its check: `$project` (format 0), `engineVersion` (semver), `gems` per build target (each
  with `gems/<name>/gem.jsonc`), `channels`, `schemas.native` (each a `schemas/<package>/` with `.hschema`
  files), `contentRoots` (existing project-relative directories, none inside another), `zones` (`name`, a
  unique `id`, `tickHz` in 1..60, a root `frame` such as `system:<name>`), and the `product` stub with 08
  §2.10.1's rules for its fields (id and install-name forms, the `helios` prefix and Windows device names
  refused, reserved URI schemes, a display name per language, a semver version).
- **Provenance, 100 %** (01 §5.2, 07 T24). Every file under a content root is a document type the cook knows
  (`hrec`, `hcont`, `hent`, `md`, registered as v0 importers without a build step) and has a valid `.meta`
  sidecar, validated by engine/assetpipe's `scanMetas` (GUID, document type, provenance with an allowed
  licence and the rules of its origin, case and portability). Any other file fails, so nothing reaches the
  content without a place to record where it came from. Hidden files and directories fail too (the sidecar
  scan skips them, and the records cook would still read a hidden `.hrec`), except at a root's top only:
  its `.gitattributes` and the ignored cook outputs `.cooked/` and `.cache/`. A `.gitattributes` deeper in
  the tree fails like any hidden file, since it could override the root's line-ending rules.
- **Layout** (07 §1.8.2's `collab.scope`; 02 §5.5–5.6). Containers (`.hcont`) and entities (`.hent`) only
  under `zones/<zone>/` of a declared zone, and nothing else there; records only under `records/`. A
  container's `$container` is `"guid:<GUID>"`, its `name` is its file name, its `frame.parent` is
  `<kind>:<name>` (system, body, grid or interior) and its `streaming.group` is `zone.<zone>`; an entity sits
  in `<container>.entities/<GUID>.hent` next to its container with `$entity` equal to that GUID; and a
  container's or entity's sidecar has the document's own GUID. The rest of a container and an entity's
  components are not checked until the container loader exists (Phase 1).

Exit codes: 0 ok, 1 the cook or the check found errors, 2 usage error, 3 I/O or setup failure.

Like `helios-tool`, it links the sample record types only (`schemas/sample`, through
`helios_toolsfw_samples`) until projects register their own types (07 §1.10). Headless: it links no
graphics module. It links engine/assetpipe (EDITOR_ONLY, which a tool may link) for the sidecar rules.

## Tests

The AAA-SEC-4 lint runs it (CTest label `lint`; registered in `CMakeLists.txt`, script
`engine/records/tests/sec4_check.cmake`):

| CTest | What |
|---|---|
| `lint_records_sec4` | Cooks `engine/records/samples/project` (a template frigate, an inheriting frigate whose `server {}` data and server-only loot table carry planted sentinels, two items) and searches the raw bytes: every sentinel is in the server cook and in no client cook |
| `lint_records_sec4_m0_sample` | The same over the M0 sample project (`engine/toolsfw/samples/project`): the Frigate's `aiHints` keys stay out of the client cook |
| `lint_records_sec4_fixture_leak` | The seeded violation: scanning the server cook as if it were the client cook must fail with the AAA-SEC-4 finding |
| `lint_records_sec4_content` | The same over the reference game's records (`content/`): the reference ship hull's server-only `aiHints` keys stay out of the client cook |

The reference project (WP-0.20; 09 §2.1, "Records compile; 100 % provenance"), label `lint` too:

| CTest | What |
|---|---|
| `lint_content_check` | `check` on the repository's `helios.project.jsonc` and `content/` |
| `lint_content_records_cook` | `records` on `content/`: the records compile |
| `lint_content_records_canonical` | `helios-tool --project-root=content fmt --check`: the records are in the canonical form the editor writes |
| `lint_content_check_fixture_<case>` | `tests/content_check_test.cmake` copies the project file, `content/` and `schemas/` into the build tree, seeds one violation and must see the verb fail with its diagnostic. 80 cases, at least one per rule that a project can break (every finding `content_check.cpp` reports except the two I/O failures, a content root that cannot be listed or scanned): provenance (`missing_sidecar`, `bad_licence`, `ai_without_record`, `unknown_type`, `unknown_type_no_extension`, `hidden_file`, `hidden_directory`, `nested_gitattributes`), layout (`undeclared_zone`, `spatial_outside_zone`, `global_in_zone`, `record_outside_records`, `file_directly_in_zones`), containers (`container_too_large`, `container_parse_error`, `container_not_object`, `container_guid_form`, `container_name`, `container_frame_missing`, `container_frame_parent`, `container_streaming_missing`, `container_group`, `container_identity`), entities (`entity_name`, `entity_file_name`, `entity_outside_folder`, `entity_without_container`, `entity_identity`), the project file (51 `project_*` cases: the file missing, too large, unparsable or not an object; `$project` newer or not an integer; an unknown, duplicate, missing or wrongly typed key; `engineVersion`; each rule of `gems`, `channels`, `schemas`, `contentRoots`, `zones` and `product`) and the records cook (`broken_record`). Three must pass: `valid`, the unchanged copy, through both verbs; `cook_then_check`, a `records` run to its default output followed by `check`; and `ignored_cache`, a cook cache at the root's top. The cases pick their targets by pattern (the first file of a kind), so they follow the content as it grows |

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (02 §3.3, §3.5, §6.5; 07 §1.10; 09 §2 WP-0.8) on 2026-10-04. The `check` verb
was written for plan revision 13 (01 §5.2; 02 §3.8, §5.1, §5.5–5.6; 07 §1.7, §1.8.2, T24; 08 §2.10.1; 09 §2
WP-0.20, §2.7.2) on 2026-10-05. Choices the plan leaves open: the project file's field names and its format
version, the product stub's subset of 08 §2.10.1, `.meta` sidecars for text documents with the document type
as the importer id, the hidden-file rule, and the GUID forms of 02 §5.6's example.
