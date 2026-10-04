# helios-cook

The cook CLI (07 §1.10 names it beside `helios-tool` and `helios-assetd`). v0 (WP-0.8 part 2) has one verb:

```
helios-cook records [--project-root=<dir>] [--out=<dir>] [--quiet] [--log-level=<level>]
```

`records` reads every `records/<table>/**/*.hrec` of the project (typed by the record type with that
`@table`), resolves `$parent` inheritance, checks references, tags, formulas and the client/server split
(AAA-SEC-4), and writes `records.client.hrdb` and `records.server.hrdb` to `--out` (default
`<project-root>/cooked`). The rules are [engine/records](../../../engine/records/README.md)'s. Errors are
listed on stderr, sorted by file, and nothing is written.

Exit codes: 0 ok, 1 the cook found errors, 2 usage error, 3 I/O or setup failure.

Like `helios-tool`, it links the sample record types only (`schemas/sample`, through
`helios_toolsfw_samples`) until projects register their own types (07 §1.10). Headless: it links no
graphics module.

## Tests

The AAA-SEC-4 lint runs it (CTest label `lint`; registered in `CMakeLists.txt`, script
`engine/records/tests/sec4_check.cmake`):

| CTest | What |
|---|---|
| `lint_records_sec4` | Cooks `engine/records/samples/project` (a template frigate, an inheriting frigate whose `server {}` data and server-only loot table carry planted sentinels, two items) and searches the raw bytes: every sentinel is in the server cook and in no client cook |
| `lint_records_sec4_m0_sample` | The same over the M0 sample project (`engine/toolsfw/samples/project`): the Frigate's `aiHints` keys stay out of the client cook |
| `lint_records_sec4_fixture_leak` | The seeded violation: scanning the server cook as if it were the client cook must fail with the AAA-SEC-4 finding |

## Plan conformance

Plan-Rev: 13

Written for plan revision 13 (02 §3.3, §3.5, §6.5; 07 §1.10; 09 §2 WP-0.8) on 2026-10-04.
