#!/usr/bin/env python3
"""Perf history and its regression comparator (09 §5.8; 02 §8.3; WP-0.3).

  python3 tools/scorecard/perf.py extract --results DIR --sha SHA [--date ISO] [--scorecard FILE] --out entry.json
  python3 tools/scorecard/perf.py compare --entry entry.json [--history old.json] [--window 5]
                                          --out new.json [--markdown FILE]

`extract` reads the numbers the perf gates print: every metric in scorecard.jsonc's `perf_metrics`
(a regex over the MESSAGE lines of a doctest case or the output of a gate) in every result set that has
it, plus the wall time of every `perf:` doctest case, which is recorded but not gated, and each result
set's host fingerprint (host.json from `runners.py host`: CPU model name and logical CPU count).

`compare` appends the entry to the history (the previous night's perf-history artifact) and fails a gated
metric that is worse than its anchor by more than its category's budget (5 % for render passes and engine
runtime benchmarks, 10 % for backend, editor and iteration; 09 §5.8, 02 §8.3). The anchor does not follow:
it is the median of the metric's first --window values (its calibration nights, recorded but not gated,
so one outlier among them does not set it), or the value of the night a `perf_accept` record accepted. A
step, a slow creep and sub-budget steps that stack therefore all fail once they are over budget. The
rolling baseline, the median of the last --window values that were not regressions, is held so that it is
never better than the anchor (a dip or an improvement nobody accepted must not make the normal level fail
later); a night over budget against it is then also over budget against the anchor, so it decides
nothing on its own and is reported to tell a step (over budget against both) from drift (only against
the anchor).
Levels are kept per run and host class (the fingerprint of the result set that measured them; entries
from before fingerprints read `unrecorded`): hosted runners of one image come on several CPU models whose
timings differ by more than the budgets, so a night is only compared with nights on its own class, and
everything above holds within a class. A metric's first night on a class that has none of its levels
reads `new-host-class` (not failing) and starts that class's calibration; the levels of other classes
are kept for when the runner comes back to them. A metric with an absolute `bound` (a plan criterion such
as RT-13's ≤ 10 % overhead) fails when it is worse than the bound, on every night and class, and its drift
is reported, not gated.
Every verdict, both levels and the applied accept are stored in the history and carried forward (a
missing metric carries them too, and the history keeps every class's newest levels), so none of them
heals or expires as old entries leave it. Only a reviewed `perf_accept` record in scorecard.jsonc moves
the levels (of the host class its night ran on): it names a night and the value accepted, is applied
once, and then stays applied whether or not the record is kept. A record whose night is inside the
history but has no stored value for the metric, or measured something else, fails (`accept-unmatched`);
one older than the whole history is reported as stale, not failed. A declared gated metric that had a
value and stops being produced is `missing` until it comes back or the registry drops the metric, its run
or its run assignment; a declared metric that never had a value is listed, not failed.
It prints the summary and then one `::error` workflow command per failing row (a check-run annotation
that names the metric), exits 1 on any failing verdict, and exits 2 under --require-history when there
is no history to compare with (so a lost artifact cannot reset every level); --note lines head the
summary (a first night, or a restart, which the nightly allows only when no history can be fetched), and
a new history records when and why it started, which every later summary shows.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

import report
import runners
import scorecard

BUDGET_PERCENT = {"render": 5.0, "runtime": 5.0, "backend": 10.0, "editor": 10.0, "iteration": 10.0}
HISTORY_LIMIT = 60
UNRECORDED = "unrecorded"  # the host class of entries written before host fingerprints


def load_hosts(root: Path) -> dict[str, dict]:
    """{run name: host fingerprint} from each result set's host.json (the first one per run)."""
    hosts = {}
    for d in sorted(p for p in root.iterdir() if p.is_dir() and (p / "run.json").is_file()):
        try:
            run = json.loads((d / "run.json").read_text(encoding="utf-8"))["run"]
            fingerprint = json.loads((d / "host.json").read_text(encoding="utf-8"))
        except (OSError, ValueError, KeyError, TypeError):
            continue
        if isinstance(fingerprint, dict) and isinstance(fingerprint.get("cpu"), str):
            hosts.setdefault(run, {"cpu": fingerprint["cpu"], "logical_cpus": fingerprint.get("logical_cpus")})
    return hosts


def extract(data: dict, runs: list[report.Results], sha: str, date: str, hosts: dict | None = None) -> dict:
    metrics = {}
    for metric in data.get("perf_metrics") or []:
        for run, value in report.metric_values(metric, runs).items():
            metrics[f"{run}/{metric['id']}"] = {"value": value, "unit": metric["unit"], "better": metric["better"],
                                                "category": metric["category"], "gate": True}
            if "bound" in metric:
                metrics[f"{run}/{metric['id']}"]["bound"] = metric["bound"]
    for r in runs:
        for (binary, case), seconds in r.durations.items():
            if case.startswith("perf:"):
                metrics[f"{r.name}/duration:{binary}:{case}"] = {"value": seconds, "unit": "s", "better": "lower",
                                                                   "category": "runtime", "gate": False}
    declared = [m for m in data.get("perf_metrics") or [] if isinstance(m, dict) and isinstance(m.get("id"), str)]
    return {"sha": sha, "date": date, "declared": sorted(m["id"] for m in declared),
            "metric_runs": {m["id"]: m["run"] for m in declared if isinstance(m.get("run"), str)},
            "runs": sorted(data.get("runs") or {}), "accepted": list(data.get("perf_accept") or []),
            "hosts": {run: dict(host, **{"class": runners.host_class(host)})
                      for run, host in sorted((hosts or {}).items())},
            "metrics": metrics}


def _night(entry: dict) -> str:
    return str(entry.get("date", ""))[:10]


def _split(key: str) -> tuple[str | None, str]:
    """(run, metric id) of a history key `run/metric`."""
    run, sep, metric = key.partition("/")
    return (run, metric) if sep else (None, key)


def run_class(entry: dict, run: str | None) -> str:
    """The host class `run` measured on in `entry` (`unrecorded` without a fingerprint)."""
    host = (entry.get("hosts") or {}).get(run)
    return host["class"] if isinstance(host, dict) and isinstance(host.get("class"), str) else UNRECORDED


def _host_of(e: dict, key: str) -> str | None:
    """The host class of `key`'s stored value or carried levels in history entry `e`; None if it has neither."""
    for m in ((e.get("metrics") or {}).get(key), (e.get("missing_levels") or {}).get(key)):
        if isinstance(m, dict):
            return m["host"] if isinstance(m.get("host"), str) else UNRECORDED
    return None


FAILING = ("regression", "missing", "accept-unmatched", "no-baseline")


def _accept_for(entry: dict, key: str, applies=lambda night: True) -> dict | None:
    """The perf_accept record for `key` with the latest night up to the entry's own (a later one is not
    in force yet) among those that `applies` to tonight's host class."""
    run, metric = _split(key)
    records = [a for a in entry.get("accepted") or [] if isinstance(a, dict) and a.get("metric") == metric
               and a.get("run", run) == run and isinstance(a.get("night"), str) and a["night"] <= _night(entry)
               and applies(a["night"])]
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


LEVELS = ("baseline", "anchor", "anchor_n", "accepted")


def _has_levels(m) -> bool:
    return isinstance(m, dict) and any(m.get(f) is not None for f in LEVELS)


def _newest(past: list[dict], key: str) -> tuple[dict | None, str | None]:
    """(the newest stored levels of `key`, their host class): from its metric entry, or from a night it
    was missing."""
    for e in reversed(past):
        for m in ((e.get("metrics") or {}).get(key), (e.get("missing_levels") or {}).get(key)):
            if _has_levels(m):
                return m, _host_of(e, key)
    return None, None


def _levels(past: list[dict], key: str) -> dict | None:
    return _newest(past, key)[0]


def _seed_levels(past: list[dict]) -> dict:
    """{key: {host class: levels}}, the newest of each, from the entries of a history that predates the
    per-class level store."""
    store: dict = {}
    for e in past:
        for part in ("metrics", "missing_levels"):
            for key, m in (e.get(part) or {}).items():
                if _has_levels(m):
                    store.setdefault(key, {})[_host_of(e, key)] = {f: m.get(f) for f in LEVELS}
    return store


def _evaluate(full: list[dict], entry: dict, key: str, m: dict, window: int, cls: str = UNRECORDED,
              store: dict | None = None) -> dict:
    """One row: `key`'s value tonight against its levels on host class `cls`. `full` is the whole history,
    `store` the newest levels of every class ({key: {class: levels}}), which outlive the entries."""
    limit = BUDGET_PERCENT[m["category"]]
    row = {"metric": key, "value": m["value"], "unit": m["unit"], "gate": m["gate"], "baseline": None,
           "change": None, "anchor": None, "anchor_change": None, "limit": limit, "verdict": "new", "note": "",
           "anchor_n": 1, "accepted": None, "host": cls, "better": m["better"],
           "bound": m.get("bound") if m["gate"] else None}
    past = [e for e in full if _host_of(e, key) == cls]
    seen = [e for e in past if key in (e.get("metrics") or {})]
    stored = ((store or {}).get(key) or {}).get(cls)
    levels = _levels(past, key) or (stored if _has_levels(stored) else None) or {}
    carried = {f: levels.get(f) for f in LEVELS}
    carried["anchor_n"] = carried["anchor_n"] or 0
    applied = carried["accepted"] if isinstance(carried["accepted"], dict) else None

    def applies(night: str) -> bool:
        """A record accepts the level of its night's host class (any record whose night has no value of
        the metric, or is tonight, is weighed on every class, so it cannot go unnoticed)."""
        classes = {_host_of(e, key) for e in full if _night(e) == night and key in (e.get("metrics") or {})}
        return night == _night(entry) or not classes or cls in classes

    record = _accept_for(entry, key, applies) if m["gate"] else None
    notes = []
    if record is not None and applied is not None and record["night"] < applied.get("night", ""):
        notes.append(f"perf_accept for {record['night']} is older than the applied accept of {applied['night']}, "
                     f"so it has no effect; accept the level you want with a newer night")
    if record is not None and (applied is None or record["night"] > applied.get("night", "")):
        night, want, tonight = record["night"], record.get("value"), _night(entry)
        if night == tonight:
            measured = m["value"]
        else:
            on_night = [e["metrics"][key]["value"] for e in seen if _night(e) == night]
            measured = on_night[-1] if on_night else None
        oldest = min((_night(e) for e in full), default=None)
        ok_want = isinstance(want, (int, float)) and not isinstance(want, bool) and want > 0
        if measured is None and night != tonight and (oldest is None or night < oldest):
            notes.append(f"stale perf_accept for {night}: older than the whole history, nothing to apply; "
                         f"remove it")
        elif measured is None or not ok_want or abs(100.0 * (measured - want) / want) > limit:
            why = ("names a night with no stored value for this metric" if measured is None else
                   f"accepts {want!r}, but that night measured {measured:.4g}")
            row.update(carried, verdict="accept-unmatched", note=f"perf_accept for {night} {why}")
            return row
        elif night == tonight:
            row.update(verdict="accepted", baseline=m["value"], anchor=m["value"], anchor_n=window,
                       accepted={"night": night, "value": m["value"]},
                       note=f"perf_accept for {night}: {record.get('reason', '')}")
            return row
        else:
            applied = {"night": night, "value": measured}
    if not seen and not levels:
        others = {_host_of(e, key) for e in full} | set((store or {}).get(key) or {})
        if others - {None, cls}:
            # The metric has a history on other classes only: tonight starts this class's calibration.
            notes.append(f"first night on this host class ({cls}); its calibration starts, and the "
                         f"levels of other classes are kept")
            row.update(verdict="new-host-class")
        row.update(baseline=m["value"], anchor=m["value"], anchor_n=1, accepted=applied, note="; ".join(notes))
        return row
    values = baseline_values(past, key, m["gate"], applied["night"] if applied else None, window)
    base = statistics.median(values) if values else carried["baseline"]
    clean = [e["metrics"][key]["value"] for e in seen
             if not (m["gate"] and e["metrics"][key].get("verdict") == "regression")][:window]
    if applied is not None:
        anchor, n = applied["value"], window
    elif carried["anchor"] is not None and carried["anchor_n"] >= window:
        anchor, n = carried["anchor"], window
    elif len(clean) >= window:
        anchor, n = statistics.median(clean), window
    elif len(clean) == len(seen) and carried["anchor_n"] < window:
        # A calibration night: the anchor is the median of the metric's first --window values, so one
        # outlier among them does not set it. Recorded, not gated.
        first = clean + [m["value"]]
        row.update(verdict="calibrating", baseline=statistics.median(first), anchor=statistics.median(first),
                   anchor_n=len(first), note="; ".join(notes + [f"calibration night {len(first)} of {window}"]))
        return row
    else:
        anchor, n = carried["anchor"], carried["anchor_n"]
    if base is None or anchor is None:
        row.update(carried, verdict="no-baseline" if m["gate"] else "new",
                   note="history without a usable baseline" if m["gate"] else "")
        return row
    # The rolling baseline is never better than the anchor: after a dip or an improvement nobody accepted,
    # the normal level must not fail; an accepted improvement moved the anchor and stays protected.
    base = max(base, anchor) if m["better"] == "lower" else min(base, anchor)
    change, worse = _change(m["value"], base, m["better"])
    anchor_change, anchor_worse = _change(m["value"], anchor, m["better"])
    flagged = worse > limit or anchor_worse > limit
    if anchor_worse > limit and worse <= limit:
        notes.append("drift against the anchor")
    elif m["gate"] and -anchor_worse > limit and row["bound"] is None:
        notes.append(f"better than the anchor by {-anchor_worse:.1f} %: accept it with perf_accept to protect it")
    row.update(baseline=base, change=change, anchor=anchor, anchor_change=anchor_change, anchor_n=n,
               accepted=applied, verdict=("regression" if m["gate"] else "slower") if flagged else "ok",
               note="; ".join(notes))
    return row


def _bound_text(better: str | None, bound: float, unit: str) -> str:
    return f"{'≥' if better == 'higher' else '≤'} {bound:g} {unit}".rstrip()


def _apply_bound(row: dict, m: dict) -> dict:
    """A metric with an absolute bound (its plan criterion, in its own unit) fails when it is worse than
    the bound, on every night and host class; its drift against the anchor is reported, not gated."""
    bound = row.get("bound")
    if bound is None:
        return row
    notes = [row["note"]] if row["note"] else []
    if (m["value"] > bound) if m["better"] == "lower" else (m["value"] < bound):
        row["verdict"] = "regression"
        notes.insert(0, f"worse than its bound of {_bound_text(m['better'], bound, m['unit'])}")
    elif row["verdict"] in ("regression", "no-baseline"):
        row["verdict"] = "ok"
        notes.append("within its bound; drift against the anchor is reported, not gated")
    row["note"] = "; ".join(notes)
    return row


def compare(history: dict, entry: dict, window: int, start_note: str = "") -> tuple[dict, list[dict]]:
    """(new history, one row per metric of the entry with its levels and verdict). A history that starts
    tonight records the night and why (`start_note`, e.g. a requested restart) for as long as it lasts."""
    past = history.get("entries", [])
    store = history.get("levels") if isinstance(history.get("levels"), dict) else _seed_levels(past)
    store = json.loads(json.dumps(store))
    stored = json.loads(json.dumps(entry))  # the entry as appended, with every verdict and level
    rows = []
    for key, m in sorted(entry["metrics"].items()):
        cls = run_class(entry, _split(key)[0])
        row = _apply_bound(_evaluate(past, entry, key, m, window, cls, store), m)
        levels = {f: row[f] for f in LEVELS}
        stored["metrics"][key].update(verdict=row["verdict"], host=cls, **levels)
        if _has_levels(levels):
            store.setdefault(key, {})[cls] = levels
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
    # A missing metric keeps its levels, so it does not come back as `new` after 60 entries.
    stored["missing_levels"] = {}
    for k in missing:
        levels, host = _newest(past, k)
        stored["missing_levels"][k] = {f: v for f, v in (levels or {}).items() if f in LEVELS}
        if host is not None:
            stored["missing_levels"][k]["host"] = host
    blank = {"value": None, "unit": "", "gate": True, "baseline": None, "change": None, "anchor": None,
             "anchor_change": None, "limit": None, "note": ""}
    rows += [dict(blank, metric=k, verdict="missing") for k in missing]
    ever = {_split(k)[1] for e in past + [entry] for k in (e.get("metrics") or {})}
    rows += [dict(blank, metric=d, verdict="never measured", note="declared, but no night has produced it yet")
             for d in sorted(declared - ever)]
    started = history.get("started") or ({"night": _night(past[0]), "note": ""} if past else
                                         {"night": _night(entry), "note": start_note or "first night"})
    entries = (past + [stored])[-HISTORY_LIMIT:]
    # Every class's newest levels outlive the entries for as long as the metric is declared or retained.
    retained = {k for e in entries for part in ("metrics", "missing_levels") for k in (e.get(part) or {})}
    levels = {k: v for k, v in sorted(store.items()) if k in retained or still_declared(k)}
    new = {"version": 2, "started": started, "entries": entries, "levels": levels}
    return new, rows


def _fmt(v: float | None) -> str:
    return "—" if v is None else f"{v:,.0f}" if abs(v) >= 1000 else f"{v:.4g}"


def _pct(v: float | None) -> str:
    return "—" if v is None else f"{v:+.1f} %"


def _bound(r: dict) -> str:
    return _bound_text(r.get("better"), r["bound"], r["unit"])


def markdown(rows: list[dict], entry: dict, notes: list[str] = (), started: dict | None = None) -> str:
    bad = [r for r in rows if r["verdict"] in FAILING]
    since = []
    if started and started.get("night") and started["night"] != _night(entry):
        since = [f"History since {started['night']}" + (f" ({started['note']})" if started.get("note") else "") +
                 ".", ""]
    runs = sorted({_split(r["metric"])[0] for r in rows if r["value"] is not None} - {None})
    hosts = "; ".join(f"`{run}` on {run_class(entry, run)}" for run in runs)
    lines = ["## Perf history", ""] + [f"**{n}**" for n in notes] + ([""] if notes else []) + since + [
             f"Night {_night(entry)} (UTC, the `night` a `perf_accept` record names), commit `{entry['sha'][:12]}`: "
             f"{len(bad)} gated metric(s) failing. Budgets are 5 % (render, runtime) and 10 % (backend, editor, "
             f"iteration), against the anchor (the median of a metric's first nights, or its last accepted "
             f"night), which only a `perf_accept` record moves (09 §5.8); a metric with a bound (a plan "
             f"criterion) is gated against the bound instead. Baseline is the rolling median of recent nights that "
             f"were not regressions, never better than the anchor; Change against it shows a step. Levels are kept "
             f"per run and host class (CPU model, logical CPUs), and a night is compared only with nights on its "
             f"own class: a class's first night reads `new-host-class` and starts its calibration. Hosted runners "
             f"are not the fixed hardware the binding per-commit gates need (WP-0.4).",
             ""] + ([f"Hosts: {hosts}.", ""] if hosts else []) + [
             "| Metric | Tonight | Baseline | Change | Anchor | Drift | Budget | Verdict |",
             "|---|---|---|---|---|---|---|---|"]
    fmt, pct = _fmt, _pct
    for r in sorted(rows, key=lambda r: (not r["gate"], r["verdict"] == "ok", r["metric"])):
        limit = "—" if r["limit"] is None else f"{r['limit']:.0f} %" + ("" if r["gate"] else " (not gated)")
        if r.get("bound") is not None:
            limit = f"{_bound(r)} (bound)"
        verdict = f"**{r['verdict']}**" if r["verdict"] in FAILING else r["verdict"]
        if r.get("note"):
            # One line per row: the table also goes to the job log, where a line starting with `::` would
            # be a workflow command.
            verdict += f" ({' '.join(str(r['note']).split())})"
        lines.append(f"| `{r['metric']}` | {fmt(r['value'])} {r['unit']} | {fmt(r['baseline'])} | {pct(r['change'])} | "
                     f"{fmt(r['anchor'])} | {pct(r['anchor_change'])} | {limit} | {verdict} |")
    return "\n".join(lines) + "\n"


def _command_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _command_property(text: str) -> str:
    return _command_data(text).replace(":", "%3A").replace(",", "%2C")


def annotations(rows: list[dict]) -> list[str]:
    """One GitHub `::error` workflow command per failing row, so the check run names each failing metric
    (the summary and the artifact are not where a reader of the run page or the log looks first)."""
    lines = []
    for r in rows:
        if r["verdict"] not in FAILING:
            continue
        if r["value"] is None:
            text = f"{r['metric']}: no value tonight"
        else:
            text = (f"{r['metric']} {_fmt(r['value'])} {r['unit']}".rstrip() +
                    f" vs anchor {_fmt(r['anchor'])} ({_pct(r['anchor_change'])})")
            if r.get("bound") is not None:
                text += f", bound {_bound(r)}"
        if r.get("note"):
            text += f": {r['note']}"
        lines.append(f"::error title={_command_property('perf ' + r['verdict'])}::{_command_data(text)}")
    return lines


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
    cmp_.add_argument("--note", action="append", default=[], help="a line for the top of the summary")
    cmp_.add_argument("--require-history", action="store_true",
                      help="fail (exit 2) without a history: after the first night, a lost artifact would reset "
                           "every baseline and missing marker")
    args = parser.parse_args(argv)
    if args.command == "extract":
        data, _ = scorecard.load_jsonc(args.scorecard)
        runs = report.load_results(args.results, scorecard.go_module())
        entry = extract(data, runs, args.sha, args.date, load_hosts(args.results))
        args.out.write_text(json.dumps(entry, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"perf: {len(entry['metrics'])} metrics -> {args.out}")
        return 0
    entry = json.loads(args.entry.read_text(encoding="utf-8"))
    history = json.loads(args.history.read_text(encoding="utf-8")) if args.history and args.history.is_file() else {}
    if args.require_history and not history.get("entries"):
        print(f"perf: error: perf history not found ({args.history}); comparing without it would reset every "
              f"baseline, anchor and missing marker. To start a new history on purpose, dispatch the nightly "
              f"with restart_perf_history (tools/scorecard/README.md)")
        return 2
    new, rows = compare(history, entry, args.window, args.note[0] if args.note else "")
    args.out.write_text(json.dumps(new, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    text = markdown(rows, entry, args.note, new.get("started"))
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    print(text)
    for line in annotations(rows):
        print(line)
    return 1 if any(r["verdict"] in FAILING for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
