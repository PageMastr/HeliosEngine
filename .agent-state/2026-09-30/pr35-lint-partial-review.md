# PR #35 (lint), partial findings (review not posted; paused on the owner's instruction)

Head reviewed: `9df9ee6b68c0c92de065eb1e4893c94d738bef5b` (on #33's 180b275). Intended score: **7/10, CHANGES REQUESTED** (two blocking findings).

## Blocking (draft)

### 1. `size.unbounded` misses element types that cannot carry `@max`, and the README says `T[N]` is bounded

- **Where:**
  - `tools/schemac/src/gen_lint.cpp:96-101`: `needsMax` returns false for `T[N]`.
  - `:117`: the element walk recurses only into structs and variants. It never checks string elements, map keys or inner containers.
  - `README.md:573`: "`T[N]` is bounded by its type."
- **Measured** with `helios-schemac --emit lint` (at 9df9ee6):

  | Network input | Findings | `size.unbounded-checked` |
  |---|---|---|
  | `rpc R(xs: string[4]) client->server reliable …` | none | 0 |
  | `m: map<string, u8> @max(4)` (keys unbounded) | none | 1 |
  | `xs: list<string> @max(4)` (elements unbounded; documented) | none | 1 |
  | the same `list<string>` in an *unreliable* rpc | only `size.unreliable` "unbounded" | 1 |

- The corpus's `ItemStack.history: list<list<i16>>` would have unbounded inner lists even with `@max`.
- **Why it blocks:**
  - For reliable rpcs, events and replicated fields these pass silently. That is exactly the "peer makes the receiver allocate at will" case the rule exists for.
  - The README's `T[N]` claim is false for `string[N]`, and for any `T[N]` whose element is unbounded.
- **Fix:**
  - Report elements that need a bound but cannot take `@max`: string, `Name`, text builtins, `TagSet`, and nested containers, whether inside `T[N]`, `list`, `set`, map keys or map values. For example: "elements of 'xs' (string[4]) cannot be bounded; use a struct with a `@max` field".
  - Correct the README.
  - Add `string[4]` and `map<string,u8> @max(4)` to `lint: network input without @max …`.

### 2. The lint is not a gate, and its follow-up is recorded only in the PR body

- Nothing runs `--emit lint` in CTest or CI. `grep` over `.github/`, `tools/ci/`, `cmake/` and `tools/schemac/CMakeLists.txt` finds no `--emit lint` or `--lint-out`. `helios_schema()` has no lint option.
- So `size.*` findings never fail anything. The only "gate" is the corpus golden, `tests/golden/corpus/sample/schema.lint.json.expected`, which pins the 9 `schemas/sample/items.hschema` findings. That is a ratchet for `schemas/sample` only: any change needs a golden update, which review sees.
- `--Werror` over `schemas/sample` exits 1 with 9 errors.
- `schemas/gameplay`, the one production package, compiled into `engine/gameplay`, has 0 findings, but only because it declares no rpcs, events or replicated components (`checked: size.unbounded-checked 0`).
- **Documentation:**
  - The PR body says "`ci.yml` is not changed … adding a lint job is left to the owner of the CI (WP-0.1)".
  - The repo does not: the README's "Lint report" heading says "for CI", and 09 §8.1's WP-0.7 row describes the emitter without saying it is ungated.
  - 02 §3.5 names CI as the lint's consumer.
- **The PR's reason is not quite right.** CI already runs every CTest, so a CTest in `tools/schemac`, inside this WP's directories, would gate without touching `ci.yml`.
- **Fix (either):**
  - Preferred: register a CTest labelled `lint` that runs `helios-schemac --emit lint --Werror` over `schemas/gameplay` (clean today), keeping the sample corpus pinned by its golden.
  - Or record the report-only status in the README and the 09 §8.1 WP-0.7 row. Name the WP-0.1 follow-up and the 9 known sample findings.

## Non-blocking (draft)

1. **Test gaps (mutants of `gen_lint.cpp`, run against the `lint:`, `golden:` and `cli:` cases):**
   - Killed: budget 1,200→1,300; no tag bytes in the worst case; no struct recursion; no dedup.
   - **Survived:**
     - no `Optional` unwrap in `needsMax` (`string?` network input);
     - no variant-alternative recursion;
     - no per-element list overhead in `valueSize`.
   - L1, removing `if (d->isReplicatedComponent() && !f.replicated) continue;`, is equivalent: every field in a replicated component's `fields` is replicated (`sema.cpp:1255-1258`), and `server {}` fields are not in that list. So the filter is dead code, and the test comment "server {} fields are never replicated" does not exercise it.
   - Add a `string?` rpc argument, a variant alternative with an unbounded field, and a `list<P>` case whose verdict depends on the per-entry overhead.
2. **Payload format.** `size.unreliable` sizes a *tagged* payload, while 02 §3.7 says rpcs travel bit-packed. The tagged worst case is an upper bound, so the budget is conservative. It is the same open decision as #33's blocking 1 (protocol hash vs rpc payloads).
3. **Service rpcs and NATS `message`s count as network input.** That is conservative and fine, but they are backend calls (05), so a note in the README would help readers of the report.
4. **09 §8.1's "Not started" row** (line 1364) still lists WP-0.7b among open PRs, as the PR notes. It is stale after the stack lands.
5. **`@max` on `TagSet` is counted as bytes**, which is approximate. This is documented.
6. **Size:** 518 changed lines, within the ~800 guideline.

## Verified so far

- **linux-gcc** (RelWithDebInfo, own dir, -j2) at 9df9ee6:
  - The incremental build had 0 warnings.
  - `schemac_tests` (97 cases plus 2 `perf:` = 99, matching the PR), `reflect_tests`, `script_tests`, `schemac_tests_perf` and `schemac_sql_postgres` passed twice.
  - `ctest -L lint` passed 140/140, `run_lints.cmake` reported "all lints passed", and `scorecard.py check` was OK.
- **CI** at 9df9ee6 (run 36396155576): all 11 checks succeeded.
- **Rule logic read in full** (`gen_lint.cpp`):
  - The worst-case arithmetic is conservative. Ints count as 10-byte varints; f32 as 4 and f64 as 8; Guid and AssetRef as 17; EntityId and RecordRef as 8; strings as `varint(n)+n`; lists as `n × (e + 5 + 17)`; structs and variants with their length prefixes.
  - A `P[200]` of `{s: string @max(8)}` is flagged at 3,203 B.
  - The SEC-1 table lists every client→server rpc, service rpcs included, sorted.
  - Findings are deduplicated by location and sorted by logical path, line and column.
- **09 §8.1:** the diff is status-only, the WP-0.7 row's `lint` sentence and stub list. `scorecard.jsonc` changes one line.
- **Trailers:** the suggested squash trailers are `WP: WP-0.7b`, `Plan-Rev: 11` and `Plan-Change: none (status only: 09 §8.1 WP-0.7 row)`. The branch has no closing keywords.

## Left to check

- `f3feae7`'s own commit trailers (irrelevant after a squash).
- Writing and posting the review body.
- Everything else listed in the task is done.
