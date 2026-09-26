#!/usr/bin/env python3
"""Perf history and its regression comparator (09 §5.8; 02 §8.3; WP-0.3).

  python3 tools/scorecard/perf.py extract --results DIR --sha SHA [--date ISO] [--scorecard FILE] --out entry.json
  python3 tools/scorecard/perf.py compare --entry entry.json [--history old.json] [--window 5]
                                          --out new.json [--markdown FILE]

`extract` reads the numbers the perf gates print: every metric in scorecard.jsonc's `perf_metrics`
(a regex over the MESSAGE lines of a doctest case or the output of a gate) in every result set that has
it, plus the wall time of every `perf:` doctest case, which is recorded but not gated.

`compare` appends the entry to the history (the previous night's perf-history artifact) and flags a
regression when a gated metric is worse than its baseline by more than its category's budget: 5 % for
render passes and engine runtime benchmarks, 10 % for backend, editor and iteration (09 §5.8; 02 §8.3 "a
regression of more than 5 % fails"). The baseline is the median of the metric's last --window values that
were not themselves regressions: the median keeps one noisy night from moving it, and excluding flagged
nights means an unfixed regression fails every night instead of becoming the baseline, and a slow creep
fails once it passes the budget and keeps failing. Only a reviewed `perf_accept` record in scorecard.jsonc
moves the baseline to a new level: the value of the night it names, and later values, replace the older
ones. Every verdict is stored in the history. A declared gated metric that had a value and stops being
produced is `missing`, and stays missing night after night until it comes back or the registry drops the
metric or its run. It exits 1 on a regression or a missing metric. Standard library only.
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
    return {"sha": sha, "date": date, "declared": sorted(m["id"] for m in data.get("perf_metrics") or []),
            "runs": sorted(data.get("runs") or {}), "accepted": list(data.get("perf_accept") or []),
            "metrics": metrics}


def _night(entry: dict) -> str:
    return str(entry.get("date", ""))[:10]


def _split(key: str) -> tuple[str | None, str]:
    """(run, metric id) of a history key `run/metric`."""
    run, sep, metric = key.partition("/")
    return (run, metric) if sep else (None, key)


def _accepted_night(entry: dict, key: str) -> str | None:
    """The latest night, up to the entry's own, that a perf_accept record makes the baseline of `key`."""
    run, metric = _split(key)
    nights = [a["night"] for a in entry.get("accepted") or [] if isinstance(a, dict) and a.get("metric") == metric
              and a.get("run", run) == run and isinstance(a.get("night"), str) and a["night"] <= _night(entry)]
    return max(nights, default=None)


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


def compare(history: dict, entry: dict, window: int) -> tuple[dict, list[dict]]:
    """(new history, one row per metric of the entry with baseline, change and verdict)."""
    past = history.get("entries", [])
    stored = json.loads(json.dumps(entry))  # the entry as appended, with every verdict
    rows = []
    for key, m in sorted(entry["metrics"].items()):
        accepted = _accepted_night(entry, key)
        row = {"metric": key, "value": m["value"], "unit": m["unit"], "gate": m["gate"], "baseline": None,
               "change": None, "limit": BUDGET_PERCENT[m["category"]], "verdict": "new"}
        values = baseline_values(past, key, m["gate"], accepted, window)
        if accepted is not None and accepted == _night(entry):
            row["verdict"] = "accepted"
        elif values:
            base = statistics.median(values)
            change = 100.0 * (m["value"] - base) / base if base else 0.0
            worse = change if m["better"] == "lower" else -change
            row.update(baseline=base, change=change,
                       verdict=("regression" if m["gate"] else "slower") if worse > row["limit"] else "ok")
        stored["metrics"][key]["verdict"] = row["verdict"]
        rows.append(row)
    # A gated metric that had a value last night, or was already missing, is missing until it is produced
    # again or the registry drops the metric or its run (the entry records what was declared tonight).
    declared, runs = set(entry.get("declared", [])), entry.get("runs")
    last = past[-1] if past else {}
    carried = {k for k, v in (last.get("metrics") or {}).items() if v.get("gate")} | set(last.get("missing") or [])
    missing = sorted(k for k in carried - set(entry["metrics"])
                     if _split(k)[1] in declared and (runs is None or _split(k)[0] in (*runs, None)))
    stored["missing"] = missing
    rows += [{"metric": k, "value": None, "unit": "", "gate": True, "baseline": None, "change": None,
              "limit": None, "verdict": "missing"} for k in missing]
    new = {"version": 1, "entries": (past + [stored])[-HISTORY_LIMIT:]}
    return new, rows


def markdown(rows: list[dict], entry: dict) -> str:
    bad = [r for r in rows if r["verdict"] in ("regression", "missing")]
    lines = ["## Perf history", "",
             f"Commit `{entry['sha'][:12]}`: {len(bad)} gated metric(s) regressed or missing; budgets are 5 % "
             f"(render, runtime) and 10 % (backend, editor, iteration) against the median of recent nights that "
             f"were not regressions (09 §5.8); only a `perf_accept` record moves a baseline.",
             "", "| Metric | Tonight | Baseline | Change | Budget | Verdict |", "|---|---|---|---|---|---|"]
    for r in sorted(rows, key=lambda r: (not r["gate"], r["verdict"] == "ok", r["metric"])):
        fmt = (lambda v: "—" if v is None else f"{v:,.0f}" if abs(v) >= 1000 else f"{v:.4g}")
        change = "—" if r["change"] is None else f"{r['change']:+.1f} %"
        limit = "—" if r["limit"] is None else f"{r['limit']:.0f} %" + ("" if r["gate"] else " (not gated)")
        verdict = f"**{r['verdict']}**" if r["verdict"] in ("regression", "missing") else r["verdict"]
        lines.append(f"| `{r['metric']}` | {fmt(r['value'])} {r['unit']} | {fmt(r['baseline'])} | {change} | "
                     f"{limit} | {verdict} |")
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
    new, rows = compare(history, entry, args.window)
    args.out.write_text(json.dumps(new, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    text = markdown(rows, entry)
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    print(text)
    return 1 if any(r["verdict"] in ("regression", "missing") for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
