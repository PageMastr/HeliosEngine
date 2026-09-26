# tools/scorecard — the AAA scorecard registry (WP-0.3)

`scorecard.jsonc` at the repository root registers every acceptance criterion with its evidence class,
platforms, threshold and the tests that evidence it (09 §5.3 DoD item 8; 01 §3; 08 §4.4). It lives at the
root because the plan names it without a directory and because CI, the nightly report and
`tools/milestone/validate.ps1` all read it. The registry never says that a criterion passes: only
test results do, in the nightly report (09 §5.6–5.8).

```
python3 tools/scorecard/scorecard.py check                                  # registry vs the plan and ci.yml
python3 tools/scorecard/scorecard.py check --build-dir build/linux-gcc       # ... and vs that build's tests
python3 tools/scorecard/scorecard.py inventory --build-dir build/linux-gcc \
        --go-list go-list.txt --go-list go-list-integration.txt --out inventory.json
python3 tools/scorecard/scorecard.py check --inventory inventory.json       # ... and vs a recorded inventory
```

`go-list*.txt` are the outputs of `go test -list . ./...` and `go test -tags integration -list . ./...`
in `services/`. Standard library only (Python ≥ 3.10), no network.

## What the checker enforces

CTest `lint_scorecard` (label `lint`) runs `check` in every build that has tests. Native builds with
graphics also pass `--build-dir`, so a PR that renames or deletes a CTest or doctest case the registry
cites fails until the registry follows. Headless and cross builds check the registry only. Findings
print as `scorecard.jsonc:<line>: …`, which the SARIF converter (`tools/ci/ctest_to_sarif.py`) turns
into annotations.

- **IDs and phases come from the plan.** The checker reads the criterion tables (01 §3, 02 §8.2, 03 §9.3,
  04 §11.4, 05 §10, 06 §12.2, 07 §5.2, 08 §4.4) and the `**Exit.**` paragraph of each 09 §2 phase. An
  unknown ID fails, as does a phase the plan does not give the criterion, or a CL class that differs
  from 08 §4.4.
- **Coverage.** For every phase in `covers`, each criterion whose Ph cell includes that phase, and each ID
  its 09 exit names, must be registered. `covers` is `[0]`, which is WP-0.3's acceptance ("the nightly report
  covers every Ph0 criterion"). The Director adds a phase when its registration starts.
- **Fields.** Unknown fields fail (a typo like `platfroms` cannot silently drop a restriction). `status`
  must agree with the entry: `measured` (tests, no gaps), `partial` (tests and gaps) or `unmeasured`
  (gaps only).
- **Tests exist.** Every `ctest`, `doctest` (and `pinned_by`) reference must exist in each inventory whose
  OS it is evaluated on. `go` references are checked when the inventory has a `go` section (the nightly
  one does). `gate` references must be declared and run on the reference's platforms. `ci_job` names
  must be jobs of `.github/workflows/ci.yml` (matrix names expanded). Every declared run (except those
  marked `"nightly": false`) and gate must be produced by `.github/workflows/nightly.yml`.
- **Perf metrics.** Each has one source (a gate, or a doctest binary and case), a pattern with exactly one
  group, a unit, `better` (`lower` or `higher`) and a budget category; a doctest source must exist.

## Entry format

```jsonc
{
  "id": "NS-0.7", "phase": 0, "source": "04 §11.4", "owner": "WP-0.13",
  "title": "…", "class": "N", "platforms": ["linux", "windows"], "threshold": "…",
  "status": "partial",
  "tests": [
    {"gate": "net_bench_gate"},
    {"doctest": "net_tests", "case": "perf: NS-0.7 (short run): …", "platforms": ["linux"]}
  ],
  "gaps": [{"clause": "…", "state": "unmeasured", "owner": "WP-0.14"}]
}
```

- `class` is the 09 §5.6 evidence class: **N** nightly (3 consecutive passing nightlies on Windows and
  Linux), **H** lab hardware (unmeasured without the lab), **W** long-running (2 scheduled passes), **M**
  manual or external (a signed record in `docs/evidence/`). `N+H` is 08's mixed class.
- `platforms` are `linux` and `windows`, or `any` for records that do not run on a platform.
- A multi-phase criterion gets one entry per phase scope (`RT-03` phase 0 now, phase 1 later).
- `exit` holds the phase-exit items that are not criteria (01 §5.4 demos, spike records, the F0 decision),
  with IDs `EXIT-<phase>.<slug>`.

**Test references**, exactly one kind each:

| Kind | Fields | Result comes from |
|---|---|---|
| `ctest` | name, `*` wildcards allowed | CTest JUnit (`ctest --output-junit`) |
| `doctest` | `doctest` (the binary's CTest name), `case` | doctest XML of that binary |
| `go` | `go` (package under `services/`), `test`, optional `tags` | `go test -json` |
| `gate` | a name declared in `gates` | the JUnit file the nightly writes for that command |
| `ci_job` | a job name of ci.yml | the latest completed CI run on `main` |
| `evidence` | a path under the repository root | the file existing (M class) |

Optional on every reference: `platforms` (a subset of the entry's), `run` (only that result set counts,
for example `linux-asan` for "ASan-clean" clauses), `note`. Result sets are declared in `runs`. A
reference without `run` counts in every `default` run of its OS, and fails if any of them fails.

**Gaps** are clauses without a passing test: `state` is `unmeasured` (no test yet) or `failing` (measured
and red), with the `owner` WP. `pinned_by` names a test that pins a known divergence (such a test
*passes* while the clause fails), so it is never evidence of a pass. An entry with a gap is never green.

## The nightly report

`.github/workflows/nightly.yml` builds and tests on Linux (GCC, Clang, ASan/UBSan), Windows (VS 2026, VS 2022,
clang-cl) and Go (Linux, Windows), runs `net_bench --gate` on GCC and VS 2026, and runs each engine/net fuzz
target under libFuzzer for 1 h (NS-0.4). Each job uploads a result set; the `scorecard` job evaluates them:

```
python3 tools/scorecard/runners.py doctest --build-dir B [--config C] [--perf] --out R/doctest   # per-case XML
python3 tools/scorecard/runners.py gate --name net_bench_gate --build-dir B --out R/gates -- net_bench --gate
python3 tools/scorecard/report.py --results results --ci-jobs ci-jobs.json --previous last/scorecard-report.json \
        [--scheduled] --out scorecard-report.json --markdown scorecard.md
python3 tools/scorecard/perf.py extract --results results --sha SHA --out perf-entry.json
python3 tools/scorecard/perf.py compare --entry perf-entry.json --history last/perf-history.json --out perf-history.json
```

A result set is a directory with `run.json` (`{"run": "<a declared run>"}`) and any of `ctest*.xml`
(`ctest --output-junit`), `doctest/<binary>[.perf].xml` with its `.status.json`, `go*.json` (`go test -json`)
and `gates/<gate>.xml`. The runner uses the working directory, environment and timeout that CTest would. A
doctest binary whose XML is unreadable (a crash) fails every case it cites. So does one that exits non-zero
although every case passed (a sanitizer report at exit).

- **Per criterion.** On each platform, a reference passes when it has results in the runs that count for it
  and none failed. A missing result or a skip is *unmeasured*. A gate that ran shorter than its
  `min_seconds` fails. The criterion passes when every reference passes on every platform and it has no gap.
  A broken pin (a `pinned_by` test that now fails) is reported so that the registry is updated.
- **Green (09 §5.6).** The report keeps a streak per criterion from last night's report: consecutive passing
  *scheduled* nightlies. A manual run never extends it, and a failure resets it. N and H criteria are green
  at 3 and W at 2; an M record is green once it exists. The summary gives the green fraction of the phase,
  the 60 % part of the round score (§5.7). It is written to the job summary and the `scorecard-report`
  artifact.
- **Perf history (09 §5.8).** `perf_metrics` name the numbers the perf gates print (a regex over a doctest
  case's MESSAGE lines or a gate's output). `compare` flags a metric that is worse than the median of its
  last 5 nights by more than its category's budget: render and runtime 5 % (02 §8.3 for runtime
  benchmarks), backend, editor and iteration 10 %. The median keeps one noisy night from moving the
  baseline, and a creep of a few percent a night still fails once it passes the budget against the older
  values. A declared metric that disappears fails too. Wall times of every `perf:` case are recorded but not
  gated. The history is the `perf-history` artifact, carried forward from the previous nightly (90-day
  retention). Hosted runners are noisy, and the binding per-commit measurements move to the fixed runner
  and the lab (WP-0.4).

The scorecard job also checks the registry against tonight's inventories, Go tests included. It uses the
workflow's read-only token to read the previous nightly's artifacts and the jobs of the latest `ci.yml` run
on `main` (for `ci_job` references). The workflow uses no secret and has no write permission.

## Adding or changing a criterion

A WP that implements a criterion (09 §5.3 item 8) adds or updates its entry in the same PR: its tests
move from `gaps` to `tests`, and `status` follows. Keep IDs, phases and thresholds as the plan states them.
If the plan changes a threshold, the registry follows the plan, never the other way round.

## Plan conformance

Plan-Rev: 6
