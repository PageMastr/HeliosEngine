#!/usr/bin/env python3
"""Perf history and its regression comparator (09 §5.8; 02 §8.3; WP-0.3).

  python3 tools/scorecard/perf.py extract --results DIR --sha SHA [--date ISO] [--scorecard FILE] --out entry.json
  python3 tools/scorecard/perf.py compare --entry entry.json [--history old.json] [--window 5]
                                          --out new.json [--markdown FILE]

`extract` reads the numbers the perf gates print: every metric in scorecard.jsonc's `perf_metrics`
(a regex over the MESSAGE lines of a doctest case or the output of a gate) in every result set that has
it, plus the wall time of every `perf:` doctest case, which is recorded but not gated.

`compare` appends the entry to the history (the previous night's perf-history artifact) and flags a
regression when a gated metric is worse than the median of its last --window values by more than its
category's budget: 5 % for render passes and engine runtime benchmarks, 10 % for backend, editor and
iteration (09 §5.8; 02 §8.3 "a regression of more than 5 % fails"). The median of a window, not the last
value alone, keeps one noisy night from moving the baseline, and a slow creep is still caught once it
passes the budget against the older values. It exits 1 on a regression. Standard library only.
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
            "metrics": metrics}


def compare(history: dict, entry: dict, window: int) -> tuple[dict, list[dict]]:
    """(new history, one row per metric of the entry with baseline, change and verdict)."""
    past = history.get("entries", [])
    rows = []
    for key, m in sorted(entry["metrics"].items()):
        values = [e["metrics"][key]["value"] for e in past if key in e.get("metrics", {})][-window:]
        row = {"metric": key, "value": m["value"], "unit": m["unit"], "gate": m["gate"], "baseline": None,
               "change": None, "limit": BUDGET_PERCENT[m["category"]], "verdict": "new"}
        if values:
            base = statistics.median(values)
            change = 100.0 * (m["value"] - base) / base if base else 0.0
            worse = change if m["better"] == "lower" else -change
            row.update(baseline=base, change=change,
                       verdict=("regression" if m["gate"] else "slower") if worse > row["limit"] else "ok")
        rows.append(row)
    declared = set(entry.get("declared", []))
    missing = sorted({k for e in past[-1:] for k, v in e.get("metrics", {}).items()
                      if v.get("gate") and k.split("/", 1)[-1] in declared} - set(entry["metrics"]))
    rows += [{"metric": k, "value": None, "unit": "", "gate": True, "baseline": None, "change": None,
              "limit": None, "verdict": "missing"} for k in missing]
    new = {"version": 1, "entries": (past + [entry])[-HISTORY_LIMIT:]}
    return new, rows


def markdown(rows: list[dict], entry: dict) -> str:
    bad = [r for r in rows if r["verdict"] in ("regression", "missing")]
    lines = ["## Perf history", "",
             f"Commit `{entry['sha'][:12]}`: {len(bad)} gated metric(s) regressed or missing; budgets are 5 % "
             f"(render, runtime) and 10 % (backend, editor, iteration) against the median of recent nights (09 §5.8).",
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
