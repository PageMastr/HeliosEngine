# tools/scorecard — the AAA scorecard registry (WP-0.3)

`scorecard.jsonc` at the repository root registers every acceptance criterion with its evidence class,
platforms, threshold and the tests that evidence it (09 §5.3 DoD item 8; 01 §3; 08 §4.4). It lives at the
root because the plan names it without a directory and because CI, the nightly report and
`tools/milestone/validate.ps1` all read it. The registry never says that a criterion passes: only
test results do, in the nightly report (09 §5.6–5.8). An owner approval (below) labels a pass and names
what it leaves open; it changes no result.

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
  04 §11.4, 05 §10, 06 §12.2, 07 §5.2, 08 §4.4; fenced blocks skipped) and the `**Exit…**` paragraph of
  each 09 §2 phase. An unknown ID fails, as does a phase the plan does not give the criterion, a CL class
  that differs from 08 §4.4, or an 01 row marked `(M)` registered with another class. A table row that
  looks like a criterion but does not parse (a missing cell, an ID the pattern does not accept) is a
  finding at its plan line, so a plan edit cannot silently drop a criterion.
- **Coverage.** For every phase in `covers`, each criterion whose Ph cell includes that phase, and each ID
  its 09 exit names, must be registered; a missing one is reported at its plan line. `covers` must
  include 0 (WP-0.3's acceptance: "the nightly report covers every Ph0 criterion"), and a covered phase
  needs an exit paragraph. The Director adds a phase when its registration starts.
- **Fields.** Unknown fields fail everywhere: top-level keys, entries, references, gaps, `runs` and
  `gates` (a typo like `platfroms` or `min_second` cannot silently drop a restriction). `version` is 1,
  `plan_rev` a positive integer no higher than `docs/plan/PLAN-REV`, `source` a plan section
  (`04 §11.4`), `owner` WPs, `User` or `Director`. A run needs `os` and a boolean `default`; a gate needs
  declared `runs` and a positive integer `min_seconds`. `status` must agree with the entry: `measured`
  (tests, no gaps), `approved` (tests, no gaps, and an owner approval), `partial` (tests and gaps) or
  `unmeasured` (gaps only).
- **Owner approvals (09 §5.6).** `owner_approval` goes only on an entry's `evidence` reference under
  `docs/evidence/`, as a real `YYYY-MM-DD` date that is not in the future; one per entry, and never on an
  exit item. The same reference needs `advisory_runs`, a non-empty list of distinct declared runs on the
  entry's platforms that the approval covers, none of them `"nightly": false` (a local or lab run gates
  every clause). The entry also needs a test reference that runs (the approved clause is still measured)
  and at least one `follow_ups` item (the re-test the approval owes). `advisory`, on the same reference, is
  a non-empty list of distinct perf metric ids, each declared for this criterion and read from one of its
  test references (the same gate, or the same doctest case). None of these fields may sit on a gap's
  `pinned_by`. `follow_ups` without an approval are rejected: without one, an open clause is a gap.
- **References.** Entries on `any` platform cite only `evidence` and `ci_job`. A reference's `platforms`
  are a subset of its entry's; `run` names a declared run on those platforms; `tags` is for `go` only.
  `ctest` names and doctest `case`s match literally except for `*`, and a name of wildcards only is
  rejected. A test whose name says it pins a known divergence can only be a gap's `pinned_by`.
  `evidence` is a relative path to a file that exists (a record still to come is a gap).
- **Tests exist.** Every `ctest`, `doctest` (and `pinned_by`) reference must exist in each inventory whose
  OS it is evaluated on; an inventory with an unknown `os` or a doctest binary that failed to list is
  a finding. `go` references are checked against the `func Test…(` declarations in `services/` on every
  run (with their `//go:build integration` constraint) and against the inventory's `go` section when it
  has one (the nightly's). `gate` references must be declared and run on the reference's platforms.
  `ci_job` names must be jobs of `.github/workflows/ci.yml` (matrix names expanded); a missing ci.yml is
  a finding. Every declared run (except those marked `"nightly": false`) and gate must be produced by
  `.github/workflows/nightly.yml`.
- **Perf metrics.** Each has one source (a gate, or a doctest binary and case, named exactly), a pattern with
  exactly one group, a unit, `better` (`lower` or `higher`) and a budget category. Every doctest source must
  exist in each inventory of its run's OS (every OS without a `run`), whether or not the metric names a
  criterion. An optional `bound` (perf.py gates the value against it instead of the anchor) is a finite
  number and needs the metric's `criterion`. `perf_accept` records name a declared metric without a
  `bound`, a valid `night`, a declared `run` if any, and a `reason`.

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
- `threshold` quotes the plan (with `[…]` for elisions); how a test reads a clause goes in `notes`.
- `platforms` are `linux` and `windows`, or `any` for records that do not run on a platform.
- A multi-phase criterion gets one entry per phase scope (`RT-03` phase 0 now, phase 1 later).
- `exit` holds the phase-exit items that are not criteria (01 §5.4 demos, spike records, the F0 decision),
  with IDs `EXIT-<phase>.<slug>`. Items the exit leaves to people (the auditors' sign-off, the risk
  review, the Windows validation run, the funding decision) are `unmeasured` gaps owned by `User` or
  `Director` until their record lands in `docs/evidence/`.

**Test references**, exactly one kind each:

| Kind | Fields | Result comes from |
|---|---|---|
| `ctest` | name, `*` wildcards allowed (nothing else is special) | CTest JUnit (`ctest --output-junit`) |
| `doctest` | `doctest` (the binary's CTest name), `case` | doctest XML of that binary |
| `go` | `go` (package under `services/`), `test`, optional `tags` | `go test -json` |
| `gate` | a name declared in `gates` | the JUnit file the nightly writes for that command |
| `ci_job` | a job name of ci.yml | the latest completed CI run on `main` |
| `evidence` | a path under the repository root | the file existing, which means only "recorded": the signature and the exit window (09 §5.6) are the auditor's to confirm |

Optional on every reference: `platforms` (a subset of the entry's), `run` (only that result set counts,
for example `linux-asan` for "ASan-clean" clauses), `note`. Result sets are declared in `runs`. A
reference without `run` counts in every `default` run of its OS, and fails if any of them fails.

**Owner approvals** (09 §5.6). Only the repository owner may pass a clause that keeps failing for a reason
outside the code, and the approval is a record under `docs/evidence/` that quotes the owner verbatim. The
entry cites it and owes a re-test:

```jsonc
"status": "approved",
"tests": [
  {"gate": "net_bench_gate"},
  {"evidence": "docs/evidence/ns-0.2-owner-approval-2026-09-30.md", "owner_approval": "2026-09-30",
   "advisory": ["net.ns02.stack_packets_per_core"], "advisory_runs": ["linux-gcc"]}
],
"follow_ups": [{"clause": "Re-test … on fixed hardware …", "owner": "WP-0.4, WP-0.13"}, …]
```

The approval does not stop any reference from counting: what is relaxed, and where, is the gate command's
business (NS-0.2's is `net_bench --gate --advisory ns02-stack`, which reports the encrypted stack's rate
instead of failing on it, on the `linux-gcc` nightly step only; the `windows-vs2026` step stays strict
because the owner has not confirmed the approval for hosted Windows). `advisory_runs` names the runs whose
gate step carries the relaxation, and `test_nightly_workflow.py`'s `AdvisoryScopeTests` holds `nightly.yml`
to it. `advisory` names the perf metrics of the approved clause so that the report can show their level
beside the pass. `follow_ups` are `{"clause", "owner"}` items that the approval leaves open; they do not
block the pass, and the report lists them.

**Runs and gates.** `runs` declares the result sets: `{"os": "linux"|"windows", "default": bool,
"description": "…"}`. A `default` run counts for every reference of its OS without a `run`; others (ASan)
count only where a reference names them. `gates` declares long gate commands:
`{"runs": [run, …], "min_seconds": n, "description": "…"}`. `min_seconds` is required, a positive integer
of seconds, and is the registry's machine-readable form of a duration clause (NS-0.4's "1 h per target"
is 3600, NS-0.7's trunk run 600): the nightly fails a gate whose result is shorter. No other keys are
accepted in either.

**Gaps** are clauses without a passing test: `state` is `unmeasured` (no test yet) or `failing` (measured
and red), with the `owner` WP. `pinned_by` names a test that pins a known divergence (such a test
*passes* while the clause fails), so it is never evidence of a pass. An entry with a gap is never green.

## The nightly report

`.github/workflows/nightly.yml` builds and tests on Linux (GCC, Clang, ASan/UBSan), Windows (VS 2026, VS 2022,
clang-cl) and Go (Linux, Windows), runs `net_bench --gate --advisory ns02-stack` on GCC (NS-0.2's owner
approval; NS-0.7 and the rest of NS-0.2 still gate) and `net_bench --gate` on VS 2026, and runs each
engine/net fuzz target under libFuzzer for 1 h (NS-0.4). Each job uploads a result set; the `scorecard` job
evaluates them:

```
python3 tools/scorecard/runners.py host --out R/host.json                       # CPU model, logical CPUs
python3 tools/scorecard/runners.py doctest --build-dir B [--config C] [--perf] [--label-exclude RE] \
        --out R/doctest                                                          # per-case XML
python3 tools/scorecard/runners.py gate --name net_bench_gate --build-dir B --out R/gates -- net_bench --gate \
        [--advisory ns02-stack]     # the flag on linux-gcc only (advisory_runs)
python3 tools/scorecard/report.py --results results --ci-jobs ci-jobs.json --previous last/scorecard-report.json \
        [--scheduled] --out scorecard-report.json --markdown scorecard.md
python3 tools/scorecard/perf.py extract --results results --sha SHA --out perf-entry.json
python3 tools/scorecard/perf.py compare --entry perf-entry.json --history last/perf-history.json --out perf-history.json
```

A result set is a directory with `run.json` (`{"run": "<a declared run>"}`) and any of `host.json` (the
host fingerprint: `{"cpu": "<model name>", "logical_cpus": n}` from /proc/cpuinfo or lscpu on Linux, the
registry on Windows), `ctest*.xml` (`ctest --output-junit`), `doctest/<binary>[.perf].xml` with its
`.status.json`, `go*.json` (`go test -json`) and `gates/<gate>.xml`. The runner uses the working directory,
environment and timeout that CTest would, and `--label-exclude` skips the entries `ctest -LE` skips: the
Windows jobs have no GPU and pass `"gpu|perf"` to both steps. A doctest binary whose XML is unreadable (a
crash) fails every case it cites. So does one that exits non-zero although every case passed (a sanitizer
report at exit).

- **Per criterion.** On each platform, a reference passes when it has results in the runs that count for it
  and none failed. A missing result or a skip is *unmeasured*. A gate that ran shorter than its
  `min_seconds` fails, and so does a gate without one. The criterion passes when every reference passes on
  every platform and it has no gap.
  A broken pin (a `pinned_by` test that now fails) is reported so that the registry is updated.
- **Owner approvals.** An approved entry is evaluated like any other. While its record exists, a pass to
  which one of its `advisory_runs` contributed reads "passed (owner approval <date>, evidence <record>)",
  and that platform's cell "passed (approval)". The label marks the entry's pass on a run the approval
  covers, whether or not that night's level needed it; the advisory levels beside it show whether it did.
  A platform without a covered run, and a report without one (a local `validate.ps1` run), show the plain
  result. The row shows tonight's value of each `advisory` metric in the covered runs ("not measured" when
  none has it) and lists the follow-ups, whatever the verdict. The summary counts the labelled passes, and
  a line under the table says that an approval with an open re-test carries no phase exit on its own
  (09 §5.6, §5.7).
- **Green (09 §5.6).** The report keeps a streak per criterion from last night's report: consecutive passing
  *scheduled* nightlies. A manual run never extends it, and a failure resets it. A scheduled report more than
  36 h after the previous scheduled one restarts every streak, because the night in between left no report
  (the report says so). N and H criteria are green at 3 and W at 2; an M record is green once it exists.
  09 §5.6's further W rule ("the latest within 14 days of the exit streak") is not enforced yet; no Phase 0
  criterion is W. The summary gives the green fraction of the phase, the 60 % part of the round score (§5.7).
  It is written to the job summary and the `scorecard-report` artifact.
- **Perf history (09 §5.8).** `perf_metrics` name the numbers the perf gates print (a regex over a doctest
  case's MESSAGE lines or a gate's output; the case by its exact name). `compare` fails a gated metric that
  is worse than its **anchor** by more than its category's budget (render and runtime 5 %, 02 §8.3 for
  runtime benchmarks; backend, editor and iteration 10 %), comparing each run only with earlier nights on
  the same **host class** (below).
  - **Failing rows are named in the log.** The perf step prints the table to the job log as well as to the
    summary, and one `::error title=perf <verdict>::<metric> <value> vs anchor <a> (<drift> %)` workflow
    command per failing row, so the run page's annotations name each failing metric (GitHub shows at most
    10 error annotations per step; the table in the log has every row). The `perf-history` artifact holds
    the night's `perf.md` next to the history. On the first night that compares with a history written
    before this (history `version` 1), the summary and the log also list the failing rows stored in that
    history's entries, one line per failing night, so the nights whose failure the log did not name are
    named once.
  - **Host classes.** Hosted runners of one image come on several kinds of machine: in its first week the
    `ubuntu-24.04` runner showed at least three performance classes (raw socket ≈ 200k, 255k and 415k pps
    per core; the PCG kernel 2.51, 1.98 and 2.99 ms), so one anchor for all of them turned a change of
    machine into a regression (10-02: PCG +19.1 % on the fastest class) and a step on one class could hide
    behind another. Whether the CPU model and count separate every such class is for the first
    fingerprinted nights to show. Every native job writes its fingerprint (`runners.py host`: CPU model
    name and logical CPU count) next to `run.json`, and the history keeps anchors, calibration, the rolling
    baseline and applied accepts per run and class. Within a class every rule below holds unchanged. A
    metric's first night on a class without its levels reads **`new-host-class`** (not failing, noted with
    the class) and is that class's first calibration night; the other classes keep their levels for when
    the runner comes back, including beyond the 60 retained entries (the history keeps every class's newest
    levels while the metric is declared, and a calibrating class's clean values until it has 5, so a class
    the runner lands on once a month still finishes its calibration). A `perf_accept` record moves the level
    of the class its night ran on. Entries from before fingerprints, and a result set without `host.json`,
    are the class `unrecorded`, so the first fingerprinted night starts every metric's calibration on its
    class; the old levels, set on mixed classes, stay with `unrecorded`. The summary's hosts line names each
    run's class tonight and how many classes the history holds levels for: a count that grows every night
    means the fingerprint is not stable.
  - **New classes are warned about, and churn fails.** A run's night on a class without any of its levels
    (its first night ever included) prints `::warning title=perf new-host-class::<run> on <class> (<n> host
    classes with levels)`, so the run page shows it although the night does not fail. If classes keep
    changing (a CPU model string that is not stable, or a pool that keeps bringing CPU models the history
    has not seen), they may never reach their 5 calibration nights, so nothing would be gated and the step
    would stay green. Two rules prevent that. Under either, the run's rows that no level gated tonight
    (`new`, `new-host-class` and `calibrating`; not metrics with a bound, which are gated on every class,
    nor metrics that are not gated) fail as **`host-churn`**, with one `::error` per run, and a row that a
    class's levels did gate keeps its verdict.
    1. *Classes that do not come back:* more than `--window` (5) of the classes the run had its first night
       on within the history (60 entries, tonight's included) have had no night since. A class stops
       counting once the runner comes back to it, so a stable pool of up to 5 CPU models never gets there,
       and a fingerprint that never repeats fails from its 6th night.
    2. *A full history without a gated night:* none of the run's last 60 nights, tonight's included, had a
       row that a level gated (`ok`, `regression` or `accepted` of a gated metric without a bound). This
       catches classes that each come back a few times before the next one replaces them, which rule 1
       misses: with the class changing every 2nd, 3rd or 4th night, every night from the 60th fails. Only
       nights on which the run measured a gated metric without a bound count, and a night that a level
       gated starts the count again. Each entry stores the count (`ungated_nights`), so it outlives the
       entries, and a night on which the run measured nothing does not reset it.

    So within every 60 of its nights a run has a night that a level gated, or it fails. In a simulation
    (200 series of 150 nights per pool size, each model drawn with a random weight from 1 to 30), pools of
    3 and 5 models never failed; 6 models failed in 3 series, on 1 night each; 8 in 15 series, on at most
    5 nights; 13 in 128 series, on a median of 4 nights among those (worst 18). Rule 2 added no red night
    to any of these series. Fix the fingerprint, or look at what the pool runs on; the churning nights
    still calibrate their classes. The level store has no cap on classes per key, because a class the
    runner rarely lands on must keep its levels: churn adds up to one class per key and night (about 150
    bytes each) until the step goes red. Once the fingerprint is fixed, the old classes stay in the store,
    unused, while their metrics are declared. They do not age out, and a restart (which would drop them)
    is honoured only when no history can be fetched, so leave them: they cost only artifact size.
  - **Bounds.** A metric with a `bound` (a number in its unit, quoting the limit of the plan criterion it
    names) is gated against that bound on every night and class, calibration nights included, instead of
    against its anchor; its drift is reported, not gated, and no `perf_accept` can name it.
    `script.fuel_metering_overhead_pct` is RT-13's "≤ 10 % overhead" (04 §10.2, 02 §8.3, WP-0.10r): a
    difference of two timings of a few percent, whose relative noise no 5 % relative budget can hold. In
    12 local runs on a shared 4-vCPU VM at load ≈ 8, 11 read 0.15–5.3 % (CV ≈ 55 %) and one −38.1 %, so its
    pattern also accepts a sign and an exponent. Its timing, `script.ns_per_fuel` (the metered host's ns
    per fuel on the same workload: 11.3–11.8 ns, CV ≈ 1.1 % in the same 12 runs), carries the relative 5 %
    runtime budget. A large negative overhead means the raw run was disturbed; the bound gates only the
    high side, so such a night says nothing about the overhead (its drift column shows it).
  - The anchor does not follow: it is the median of the metric's first 5 values, or the value of the night
    a `perf_accept` record accepted. A metric's first night reads `new` and its next 4 read `calibrating`:
    they are recorded, not gated, and one outlier among them (a lucky or a bad night) does not set the
    anchor. A step, a slow creep and sub-budget steps that stack all fail once they are over budget: a
    creep of 1.5 % a night at the 5 % budget fails on its 4th night, 3 % a night at 10 % also on its 4th,
    and two steps of 4 % fail together (all tested).
  - The summary also shows the **rolling baseline**, the median of the last 5 values that were not
    regressions, held so that it is never better than the anchor, and tonight's change against it. Because
    it is never better than the anchor, a night over budget against it is over budget against the anchor
    too, so it decides nothing on its own: it tells a step (over budget against both) from drift (noted
    "drift against the anchor"). Holding it at the anchor means that a dip (three fast nights) or an
    improvement nobody accepted does not make the normal level fail afterwards; an improvement is
    protected only once it is accepted, which moves the anchor. While a gated metric is better than its
    anchor by more than its budget, its row says so ("accept it with perf_accept to protect it"), without
    failing.

  Every verdict, both levels and the applied accept are stored in the history and carried forward, and a
  missing metric carries its levels too, so no level heals or expires as old entries leave the history (60
  entries, one per nightly run, dispatch runs included): a regression nobody fixes fails every night, and
  a history that has the metric but no usable level fails (`no-baseline`). (The one exception: a metric
  that goes missing during its calibration keeps only how many calibration nights it had, so after 60
  entries it calibrates again from its return.) Within a history, only a reviewed `perf_accept` record in
  `scorecard.jsonc` moves the levels:
  `{"metric": "<id>", "night": "YYYY-MM-DD", "value": <number>, "run": "<run>", "reason": "…"}`.
  - `night` is the UTC date of the nightly (the perf summary prints it at the top) and cannot be in the
    future; `value` is what the reviewer saw that night measure; `run` is required when the metric is read
    from every run (each run has its own level) and must be the metric's run when it has one.
  - When the stored value of that night (or tonight's, when `night` is tonight) is within the metric's
    budget of `value`, the record is applied: that value becomes the anchor and starts the rolling
    baseline. It stays applied whether the record is kept or removed, including after its night has left
    the history. It is applied on a night the runner is on the host class its night ran on, so that class
    must come back while the night is in the history; otherwise the record reads stale and the class's next
    night is compared with its old anchor (accept a newer night on that class). The latest record in force
    wins; a record older than the applied accept has no effect, and its row says so. Accept a typical
    night: the anchor is that one night's value (not a median of several), so a noisy night makes a noisy
    anchor. Anchoring an accept at the median of the accepted night and the nights after it is a follow-up.
  - A record whose night is inside the history but has no stored value for the metric, or measured
    something else, fails the metric as `accept-unmatched` until it is corrected. A record older than the
    whole history (after a restart) cannot be applied and is reported as stale, not failed: remove it.

  A declared gated metric that had a value and stops being produced (a renamed case, a changed message) is
  `missing` every night until it comes back or the registry drops the metric, drops its run or moves it to
  another run; a declared metric that has never produced a value is listed as `never measured`, not failed
  (the PR tier's source check is what catches a wrong case name). Wall times of every `perf:` case are
  recorded but not gated.
- **The perf-history artifact** (90-day retention) is fetched from the newest earlier nightly that has one,
  separately from the report, among the last 100 completed runs; an API error or a failed download fails
  the fetch. The history starts without a predecessor only when none of those runs lists a `perf-history`
  artifact at all, expired or not, and the summary then opens with "First night of the perf history". If
  one is listed but cannot be fetched (it expired, for example after a long pause), the perf step fails
  every night (`compare --require-history`) rather than reset every level, until the history is restarted
  on purpose: run the Nightly workflow by hand (Actions → Nightly → Run workflow) with
  `restart_perf_history` checked. The restart is honoured only when no history can be fetched, so it never
  replaces usable levels: while one can be fetched, the fetch step fails ("restart_perf_history refused")
  and tonight is compared with that history as usual; new levels are accepted with `perf_accept`. An
  honoured restart shows a warning on the run page and opens that night's summary with "Perf history
  restarted on request"; every metric then starts again with its calibration nights, and old `perf_accept`
  records read stale.
- **The two resets that need no review.** Besides the restart, a history starts afresh only when none of
  the last 100 completed nightly runs lists a `perf-history` artifact (the very first night, or 100
  nights on which the perf step crashed before writing one, cancelled runs included). Both show a warning
  on the run page, and a new history records the night it started and why ("History since …" heads every
  later perf summary), so a reset stays visible for as long as that history lasts.
- **Known limitation: hosted-runner noise.** Budgets are per night and hosted runners are noisy. A
  simulation of this comparator (one gated metric at the 5 % budget, Gaussian noise per night, 200 seeded
  years) gives a median of 0 red nights a year at σ = 1.5 % (90th percentile 2, worst 14) and 7 at
  σ = 2.5 % (90th percentile 36, worst 112, longest red streak 7). With the 14 gated keys of today's
  registry, σ = 2.5 % would make the perf step red on a large share of nights from noise alone. Host
  classes remove the step between CPU models, not the noise within one (two VMs of one model still share
  their hosts with other tenants). A class the runner rarely lands on is ungated until it has had 5
  nights, however far apart they are. Host churn (above) fails only when it leaves a run without a gated
  night: under classes that each recur a few times, the first red is the run's 60th night without one
  (`host-churn`, not `regression`), and a run that has a gated night at least once in 60 is gated on
  those nights only. A real regression that lands within the first 3 nights of a class the runner has
  never measured on is calibrated into that class's anchor (the median of its 5 calibration nights, so 3
  regressed values set it), and if no earlier class comes back, nothing catches it. The same holds for a
  pool that keeps moving to new CPU models and never returns: each class gates a night, so neither churn
  rule fires, and a regression is red for at most a class's nights minus its 5 calibration nights before
  the next class absorbs it (at most 1 if each class lasts 6 nights, 5 if it lasts 10). Then only the
  `new-host-class` warnings on the run page show the moves. Both come from re-anchoring per class. The
  gating and the thresholds stay as the plan sets them. The nightly perf
  history on hosted runners is drift tracking, not the binding gate: the binding per-commit perf gates on
  fixed hardware are WP-0.4's (its runner and the lab), and until they exist a red or a green here is
  evidence to read, not a measurement on REF.
- **Expected red today.** `pcg_tests_perf` fails by design (K5b, armed by WP-0.9c's red outcome; 09 §8.1),
  so the GCC job's perf step is red every night, and `script_tests` is reported to abort under `linux-asan`
  (a mimalloc use-after-poison that predates the nightly; see #9). Both are real results, reported as such;
  look for anything else first.

The scorecard job also checks the registry against tonight's inventories, Go tests included. It uses the
workflow's read-only token to read the previous nightly's artifacts and the jobs of the latest `ci.yml` run
on `main` (for `ci_job` references). The workflow uses no secret and has no write permission.

## Adding or changing a criterion

A WP that implements a criterion (09 §5.3 item 8) adds or updates its entry in the same PR: its tests
move from `gaps` to `tests`, and `status` follows. Keep IDs, phases and thresholds as the plan states them.
If the plan changes a threshold, the registry follows the plan, never the other way round.

## Plan conformance

Plan-Rev: 12

Written at plan revision 6 (WP-0.3, #10, #11, #13) and re-checked at revision 12 on 2026-10-03, when 09 §5.6's
owner approval added `owner_approval`, `advisory_runs`, `advisory`, `follow_ups` and the `approved` status.
Revisions 7–11 were re-checked then too: none changes a rule this directory implements (09 §5.10.4 (c)).
