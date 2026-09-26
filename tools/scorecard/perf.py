#!/usr/bin/env python3
"""Perf history and its regression comparator (09 §5.8; 02 §8.3; WP-0.3).

  python3 tools/scorecard/perf.py extract --results DIR --sha SHA [--date ISO] [--scorecard FILE] --out entry.json
  python3 tools/scorecard/perf.py compare --entry entry.json [--history old.json] [--window 5]
                                          --out new.json [--markdown FILE]

`extract` reads the numbers the perf gates print: every metric in scorecard.jsonc's `perf_metrics`
(a regex over the MESSAGE lines of a doctest case or the output of a gate) in every result set that has
it, plus the wall time of every `perf:` doctest case, which is recorded but not gated.

`compare` appends the entry to the history (the previous night's perf-history artifact) and flags a
regression when a gated metric is worse by more than its category's budget (5 % for render passes and
engine runtime benchmarks, 10 % for backend, editor and iteration; 09 §5.8, 02 §8.3) than either of two
levels:
  - the rolling baseline, the median of its last --window values that were not regressions, which
    catches a step and ignores one noisy night; it follows any change it does not flag;
  - the anchor, which does not follow: the median of the metric's first --window clean values, or the
    value of the night a `perf_accept` record accepts. Drift that the rolling baseline follows (a slow
    creep, or sub-budget steps that stack) fails once it is over budget against the anchor.
Every verdict, baseline and anchor is stored in the history and carried forward, so neither heals as old
nights leave it; a metric is `new` only on its first night. Only a reviewed `perf_accept` record in
scorecard.jsonc moves both levels: it names a night and the value accepted, and fails (`accept-unmatched`)
when that night has no stored value for the metric or measured something else. A declared gated metric
that had a value and stops being produced is `missing` until it comes back or the registry drops the
metric, its run or its run assignment; a declared metric that never had a value is listed, not failed.
It exits 1 on any failing verdict, and 2 under --require-history when there is no history to compare
with (so a lost artifact cannot reset every baseline). Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

import report
import scorecard

BUDGET_PERCENT = {"render": 5.0, "runtime": 5.0, "backend": 10.0, "editor": 10.0, "iteration": 10.0}
HISTORY_LIMIT = 60


def metric_values(metric: dict, runs: list[report.Results]) -> dict[str, float]:
    """{run name: value} for a declared metric."""
    pattern = re.compile(metric["pattern"])
    found = {}
    for r in runs:
        if "run" in metric and r.name != metric["run"]:
            continue
        if "doctest" in metric:
            texts = r.messages.get((metric["doctest"], metric["case"]), [])
        else:
            gate = r.gates.get(metric["gate"])
            texts = [gate[2]] if gate else []
        for text in texts:
            m = pattern.search(text)
            if m:
                found[r.name] = float(m[1])
                break
    return found


def extract(data: dict, runs: list[report.Results], sha: str, date: str) -> dict:
    metrics = {}
    for metric in data.get("perf_metrics") or []:
        for run, value in metric_values(metric, runs).items():
            metrics[f"{run}/{metric['id']}"] = {"value": value, "unit": metric["unit"], "better": metric["better"],
                                                "category": metric["category"], "gate": True}
    for r in runs:
        for (binary, case), seconds in r.durations.items():
            if case.startswith("perf:"):
                metrics[f"{r.name}/duration:{binary}:{case}"] = {"value": seconds, "unit": "s", "better": "lower",
                                                                   "category": "runtime", "gate": False}
    declared = [m for m in data.get("perf_metrics") or [] if isinstance(m, dict) and isinstance(m.get("id"), str)]
    return {"sha": sha, "date": date, "declared": sorted(m["id"] for m in declared),
            "metric_runs": {m["id"]: m["run"] for m in declared if isinstance(m.get("run"), str)},
            "runs": sorted(data.get("runs") or {}), "accepted": list(data.get("perf_accept") or []),
            "metrics": metrics}


def _night(entry: dict) -> str:
    return str(entry.get("date", ""))[:10]


def _split(key: str) -> tuple[str | None, str]:
    """(run, metric id) of a history key `run/metric`."""
    run, sep, metric = key.partition("/")
    return (run, metric) if sep else (None, key)


FAILING = ("regression", "missing", "accept-unmatched", "no-baseline")


def _accept_for(entry: dict, key: str) -> dict | None:
    """The perf_accept record for `key` with the latest night up to the entry's own (a later one is not
    in force yet)."""
    run, metric = _split(key)
    records = [a for a in entry.get("accepted") or [] if isinstance(a, dict) and a.get("metric") == metric
               and a.get("run", run) == run and isinstance(a.get("night"), str) and a["night"] <= _night(entry)]
    return max(records, key=lambda a: a["night"], default=None)


def baseline_values(past: list[dict], key: str, gate: bool, accepted: str | None, window: int) -> list[float]:
    """The values tonight is compared with: the last `window` that were not regressions (for a gated
    metric), and none from before an accepted night, whose own value counts whatever its verdict."""
    values = []
    for e in past:
        m = (e.get("metrics") or {}).get(key)
        if m is None:
            continue
        if accepted is not None and _night(e) < accepted:
            continue
        if gate and m.get("verdict") == "regression" and not (accepted is not None and _night(e) == accepted):
            continue
        values.append(m["value"])
    return values[-window:]


def _change(value: float, level: float, better: str) -> tuple[float, float]:
    """(change in %, how much worse in %) of `value` against `level`."""
    change = 100.0 * (value - level) / level if level else 0.0
    return change, (change if better == "lower" else -change)


def _last(past: list[dict], key: str, field: str) -> dict | None:
    """The newest stored metric `key` that has `field`."""
    for e in reversed(past):
        m = (e.get("metrics") or {}).get(key)
        if m is not None and m.get(field) is not None:
            return m
    return None


def _evaluate(past: list[dict], entry: dict, key: str, m: dict, window: int) -> dict:
    limit = BUDGET_PERCENT[m["category"]]
    row = {"metric": key, "value": m["value"], "unit": m["unit"], "gate": m["gate"], "baseline": None,
           "change": None, "anchor": None, "anchor_change": None, "limit": limit, "verdict": "new", "note": "",
           "anchor_n": 1}
    seen = [e for e in past if key in (e.get("metrics") or {})]
    last_base, last_anchor = _last(past, key, "baseline"), _last(past, key, "anchor")
    carried = {"baseline": last_base["baseline"] if last_base else None,
               "anchor": last_anchor["anchor"] if last_anchor else None,
               "anchor_n": last_anchor.get("anchor_n", window) if last_anchor else 0}
    record = _accept_for(entry, key) if m["gate"] else None
    accepted_night = accepted_value = None
    if record is not None:
        night, want = record["night"], record.get("value")
        if night == _night(entry):
            measured = m["value"]
        else:
            on_night = [e["metrics"][key]["value"] for e in seen if _night(e) == night]
            measured = on_night[-1] if on_night else None
        ok_want = isinstance(want, (int, float)) and not isinstance(want, bool) and want > 0
        if measured is None or not ok_want or abs(100.0 * (measured - want) / want) > limit:
            row.update(carried, verdict="accept-unmatched",
                       note=f"perf_accept for {night} " + ("names a night with no stored value for this metric"
                                                          if measured is None else
                                                          f"accepts {want!r}, but that night measured {measured:.4g}"))
            return row
        if night == _night(entry):
            row.update(verdict="accepted", baseline=m["value"], anchor=m["value"], anchor_n=window,
                       note=f"perf_accept for {night}: {record.get('reason', '')}")
            return row
        accepted_night, accepted_value = night, measured
    if not seen:
        row.update(baseline=m["value"], anchor=m["value"], anchor_n=1)
        return row
    values = baseline_values(past, key, m["gate"], accepted_night, window)
    base = statistics.median(values) if values else carried["baseline"]
    if accepted_night is not None:
        anchor, n = accepted_value, window
    elif carried["anchor"] is not None and carried["anchor_n"] >= window:
        anchor, n = carried["anchor"], window
    else:  # the first --window clean values, until there are that many
        clean = [e["metrics"][key]["value"] for e in seen
                 if not (m["gate"] and e["metrics"][key].get("verdict") == "regression")][:window]
        anchor, n = (statistics.median(clean), len(clean)) if clean else (carried["anchor"], carried["anchor_n"])
    if base is None or anchor is None:
        row.update(carried, verdict="no-baseline" if m["gate"] else "new",
                   note="history without a usable baseline" if m["gate"] else "")
        return row
    change, worse = _change(m["value"], base, m["better"])
    anchor_change, anchor_worse = _change(m["value"], anchor, m["better"])
    flagged = worse > limit or anchor_worse > limit
    row.update(baseline=base, change=change, anchor=anchor, anchor_change=anchor_change, anchor_n=n,
               verdict=("regression" if m["gate"] else "slower") if flagged else "ok",
               note="drift against the anchor" if anchor_worse > limit and worse <= limit else "")
    return row


def compare(history: dict, entry: dict, window: int) -> tuple[dict, list[dict]]:
    """(new history, one row per metric of the entry with its levels and verdict)."""
    past = history.get("entries", [])
    stored = json.loads(json.dumps(entry))  # the entry as appended, with every verdict and level
    rows = []
    for key, m in sorted(entry["metrics"].items()):
        row = _evaluate(past, entry, key, m, window)
        stored["metrics"][key].update(verdict=row["verdict"], baseline=row["baseline"], anchor=row["anchor"],
                                      anchor_n=row["anchor_n"])
        rows.append(row)
    # A gated metric that had a value last night, or was already missing, is missing until it is produced
    # again or the registry drops the metric, its run, or (for a metric read from one run) moves it to
    # another run; the entry records what was declared tonight.
    declared, runs = set(entry.get("declared", [])), entry.get("runs")
    metric_runs = entry.get("metric_runs") or {}

    def still_declared(k: str) -> bool:
        run, metric = _split(k)
        return metric in declared and (runs is None or run in (*runs, None)) and \
            (run is None or metric_runs.get(metric, run) == run)

    last = past[-1] if past else {}
    carried = {k for k, v in (last.get("metrics") or {}).items() if v.get("gate")} | set(last.get("missing") or [])
    missing = sorted(k for k in carried - set(entry["metrics"]) if still_declared(k))
    stored["missing"] = missing
    blank = {"value": None, "unit": "", "gate": True, "baseline": None, "change": None, "anchor": None,
             "anchor_change": None, "limit": None, "note": ""}
    rows += [dict(blank, metric=k, verdict="missing") for k in missing]
    ever = {_split(k)[1] for e in past + [entry] for k in (e.get("metrics") or {})}
    rows += [dict(blank, metric=d, verdict="never measured", note="declared, but no night has produced it yet")
             for d in sorted(declared - ever)]
    new = {"version": 1, "entries": (past + [stored])[-HISTORY_LIMIT:]}
    return new, rows


def markdown(rows: list[dict], entry: dict) -> str:
    bad = [r for r in rows if r["verdict"] in FAILING]
    lines = ["## Perf history", "",
             f"Night {_night(entry)} (UTC, the `night` a `perf_accept` record names), commit `{entry['sha'][:12]}`: "
             f"{len(bad)} gated metric(s) failing. Budgets are 5 % (render, runtime) and 10 % (backend, editor, "
             f"iteration), against both the rolling baseline (median of the last nights that were not regressions) "
             f"and the anchor (the first clean nights, or the last accepted night); only a `perf_accept` record "
             f"moves the anchor (09 §5.8).",
             "", "| Metric | Tonight | Baseline | Change | Anchor | Drift | Budget | Verdict |",
             "|---|---|---|---|---|---|---|---|"]
    fmt = (lambda v: "—" if v is None else f"{v:,.0f}" if abs(v) >= 1000 else f"{v:.4g}")
    pct = (lambda v: "—" if v is None else f"{v:+.1f} %")
    for r in sorted(rows, key=lambda r: (not r["gate"], r["verdict"] == "ok", r["metric"])):
        limit = "—" if r["limit"] is None else f"{r['limit']:.0f} %" + ("" if r["gate"] else " (not gated)")
        verdict = f"**{r['verdict']}**" if r["verdict"] in FAILING else r["verdict"]
        if r.get("note"):
            verdict += f" ({r['note']})"
        lines.append(f"| `{r['metric']}` | {fmt(r['value'])} {r['unit']} | {fmt(r['baseline'])} | {pct(r['change'])} | "
                     f"{fmt(r['anchor'])} | {pct(r['anchor_change'])} | {limit} | {verdict} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    scorecard._utf8_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ext = sub.add_parser("extract")
    ext.add_argument("--results", type=Path, required=True)
    ext.add_argument("--scorecard", type=Path, default=scorecard.ROOT / "scorecard.jsonc")
    ext.add_argument("--sha", required=True)
    ext.add_argument("--date", default=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    ext.add_argument("--out", type=Path, required=True)
    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("--entry", type=Path, required=True)
    cmp_.add_argument("--history", type=Path)
    cmp_.add_argument("--window", type=int, default=5)
    cmp_.add_argument("--out", type=Path, required=True)
    cmp_.add_argument("--markdown", type=Path)
    cmp_.add_argument("--require-history", action="store_true",
                      help="fail (exit 2) without a history: after the first night, a lost artifact would reset "
                           "every baseline and missing marker")
    args = parser.parse_args(argv)
    if args.command == "extract":
        data, _ = scorecard.load_jsonc(args.scorecard)
        runs = report.load_results(args.results, scorecard.go_module())
        entry = extract(data, runs, args.sha, args.date)
        args.out.write_text(json.dumps(entry, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"perf: {len(entry['metrics'])} metrics -> {args.out}")
        return 0
    entry = json.loads(args.entry.read_text(encoding="utf-8"))
    history = json.loads(args.history.read_text(encoding="utf-8")) if args.history and args.history.is_file() else {}
    if args.require_history and not history.get("entries"):
        print(f"perf: error: perf history not found ({args.history}); comparing without it would reset every "
              f"baseline, anchor and missing marker")
        return 2
    new, rows = compare(history, entry, args.window)
    args.out.write_text(json.dumps(new, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    text = markdown(rows, entry)
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    print(text)
    return 1 if any(r["verdict"] in FAILING for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
