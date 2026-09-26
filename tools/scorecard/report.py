#!/usr/bin/env python3
"""The nightly scorecard report: every registered criterion's pass, fail or unmeasured (09 §5.6-5.8; WP-0.3).

  python3 tools/scorecard/report.py --results DIR [--ci-jobs FILE] [--previous FILE] [--scheduled]
                                    [--scorecard FILE] [--phase N] --out report.json [--markdown FILE]

DIR holds one directory per result set (a nightly job's artifact). Each has `run.json`
({"run": "<a run of scorecard.jsonc>"}) and any of:
  ctest*.xml                  CTest JUnit (`ctest --output-junit`)
  doctest/<binary>[.perf].xml doctest XML with <stem>.status.json (runners.py doctest)
  go*.json                    `go test -json` streams
  gates/<gate>.xml            JUnit from runners.py gate
--ci-jobs is the GitHub API's jobs listing of the latest completed ci.yml run on main.

A reference passes on an OS when it has results in the runs that count for it and none of them failed;
a criterion passes when every reference passes on every platform and it has no gap. Anything else is
fail (a failed reference or a failing gap) or unmeasured (no result, a skip, or an unmeasured gap).
The streak counts consecutive passing scheduled reports (--previous is last night's report.json), and
a criterion is green under 09 §5.6 at 3 (N, H) or 2 (W) passes; an M record is green once it exists.
A scheduled night more than MAX_NIGHT_GAP_HOURS after the last scheduled report restarts every streak,
because the night in between has no report and so no known result. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

import scorecard
from scorecard import matches

GREEN_STREAK = {"N": 3, "H": 3, "N+H": 3, "W": 2}
# The nightly is scheduled daily; a scheduled report further than this from the previous scheduled one
# means a night without a report (a failed scorecard job, a skipped schedule), which breaks the streak.
MAX_NIGHT_GAP_HOURS = 36
CI_CONCLUSION = {"success": "pass", "failure": "fail", "cancelled": "fail", "timed_out": "fail",
                 "startup_failure": "fail", "skipped": "skip", "neutral": "skip"}


class Results:
    """Test results of one run, keyed by what references name."""

    def __init__(self, name: str):
        self.name = name
        self.ctest: dict[str, tuple[str, float]] = {}
        self.doctest: dict[str, dict[str, str]] = {}
        self.doctest_broken: dict[str, str] = {}
        self.messages: dict[tuple[str, str], list[str]] = {}
        self.durations: dict[tuple[str, str], float] = {}
        self.go: dict[tuple[str, str], str] = {}
        self.go_broken: set[str] = set()
        self.gates: dict[str, tuple[str, float, str]] = {}


def junit_cases(path: Path) -> list[tuple[str, str, float, str]]:
    """(name, pass|fail|skip, seconds, output) per testcase of a JUnit file."""
    cases = []
    for case in ET.parse(path).getroot().iter("testcase"):
        status = case.get("status", "run")
        if case.find("failure") is not None or case.find("error") is not None or status == "fail":
            result = "fail"
        elif case.find("skipped") is not None or status in ("notrun", "disabled"):
            result = "skip"
        else:
            result = "pass"
        cases.append((case.get("name", ""), result, float(case.get("time") or 0), case.findtext("system-out") or ""))
    return cases


def read_doctest(res: Results, path: Path) -> None:
    binary = path.name.split(".", 1)[0]
    status_file = path.with_name(path.name[:-len(".xml")] + ".status.json")
    rc = json.loads(status_file.read_text(encoding="utf-8")).get("returncode", 0) if status_file.is_file() else 0
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        res.doctest_broken[binary] = f"{path.name} is unreadable (exit {rc}; crashed?)"
        return
    cases = res.doctest.setdefault(binary, {})
    failed = False
    for case in root.iter("TestCase"):
        if case.get("skipped") == "true":
            continue
        summary = case.find("OverallResultsAsserts")
        ok = summary is not None and summary.get("test_case_success") == "true"
        failed |= not ok
        name = case.get("name", "")
        cases[name] = "pass" if ok else "fail"
        res.messages[(binary, name)] = [t.text.strip() for t in case.iter("Text") if t.text]
        if summary is not None and summary.get("duration"):
            res.durations[(binary, name)] = float(summary.get("duration"))
    if rc != 0 and not failed:
        res.doctest_broken[binary] = f"{path.name}: exit {rc} although every case passed (sanitizer report or exit crash)"


def read_go(res: Results, path: Path, module: str) -> None:
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        pkg = str(event.get("Package", ""))
        pkg = pkg[len(module) + 1:] if pkg.startswith(module + "/") else pkg
        action, test = event.get("Action"), event.get("Test")
        if action in ("pass", "fail", "skip"):
            if test:
                res.go[(pkg, test)] = action
            elif action == "fail":
                res.go_broken.add(pkg)


def load_results(root: Path, module: str) -> list[Results]:
    runs = []
    for d in sorted(p for p in root.iterdir() if p.is_dir() and (p / "run.json").is_file()):
        res = Results(json.loads((d / "run.json").read_text(encoding="utf-8"))["run"])
        for f in sorted(d.glob("ctest*.xml")):
            for name, status, seconds, _ in junit_cases(f):
                res.ctest[name] = (status, seconds)
        for f in sorted((d / "doctest").glob("*.xml")):
            read_doctest(res, f)
        for f in sorted(d.glob("go*.json")):
            read_go(res, f, module)
        for f in sorted((d / "gates").glob("*.xml")):
            for name, status, seconds, output in junit_cases(f):
                res.gates[name] = (status, seconds, output)
        runs.append(res)
    return runs


def combine(statuses: list[str]) -> str:
    if not statuses:
        return "missing"
    if "fail" in statuses:
        return "fail"
    return "pass" if all(s == "pass" for s in statuses) else "skip"


def eval_ref(ref: dict, os_name: str, entry: dict, data: dict, runs: list[Results], ci_jobs: dict | None,
             repo: Path) -> tuple[str, str]:
    """(pass|fail|skip|missing, detail) of one reference on one OS."""
    kind = scorecard.ref_kind(ref)
    if kind == "evidence":
        return ("pass", "") if (repo / ref["evidence"]).is_file() else ("missing", "no record yet")
    if kind == "ci_job":
        if ci_jobs is None:
            return "missing", "no CI run listing"
        found = ci_jobs.get(ref["ci_job"])
        return (CI_CONCLUSION.get(found, "missing"), found or "not in the latest main CI run")
    declared = data.get("runs") or {}
    counting = [r for r in runs if declared.get(r.name, {}).get("os") == os_name and
                (r.name == ref["run"] if "run" in ref else declared.get(r.name, {}).get("default"))]
    statuses, details = [], []
    for r in counting:
        why = ""
        if kind == "ctest":
            hits = [s for n, (s, _) in r.ctest.items() if matches(n, ref["ctest"])]
        elif kind == "doctest":
            if ref["doctest"] in r.doctest_broken:
                hits, why = ["fail"], r.doctest_broken[ref["doctest"]]
            else:
                hits = [s for c, s in r.doctest.get(ref["doctest"], {}).items() if matches(c, ref["case"])]
        elif kind == "go":
            hit = r.go.get((ref["go"], ref["test"]))
            hits = [hit] if hit else ["fail"] if ref["go"] in r.go_broken else []
            why = "package failed" if not hit and hits else ""
        else:  # gate
            gate = r.gates.get(ref["gate"])
            need = ((data.get("gates") or {}).get(ref["gate"]) or {}).get("min_seconds")
            hits = []
            if gate and not (scorecard.is_int(need) and need > 0):  # never a silent 0: the check rejects it too
                hits, why = ["fail"], "the gate declares no positive min_seconds"
            elif gate:
                short = gate[0] == "pass" and gate[1] < need
                hits = ["fail" if short else gate[0]]
                why = f"ran {gate[1]:.0f} s of the required {need} s" if short else ""
        statuses += hits
        bad = sorted({s for s in hits if s != "pass"})
        if bad:
            details.append(f"{r.name} {'/'.join(bad)}" + (f" ({why})" if why else ""))
    return combine(statuses), "; ".join(details)


def evaluate(entry: dict, data: dict, runs: list[Results], ci_jobs: dict | None, repo: Path) -> dict:
    plats = entry.get("platforms") or []
    oses = ["any"] if plats == ["any"] else [p for p in plats if p in scorecard.OSES]
    per_os, notes = {}, []
    for os_name in oses:
        refs = [r for r in entry.get("tests") or [] if os_name == "any" or os_name in scorecard.ref_oses(entry, r, data)
                or scorecard.ref_kind(r) in ("ci_job", "evidence")]
        results = []
        for ref in refs:
            status, detail = eval_ref(ref, os_name, entry, data, runs, ci_jobs, repo)
            results.append({"ref": scorecard.ref_label(ref), "status": status, "detail": detail})
        statuses = [r["status"] for r in results]
        gap_states = [g.get("state") for g in entry.get("gaps") or []]
        if "fail" in statuses or "failing" in gap_states:
            status = "fail"
        elif not statuses or any(s != "pass" for s in statuses) or gap_states:
            status = "unmeasured"
        else:
            status = "pass"
        per_os[os_name] = {"status": status, "refs": results}
    for gap in entry.get("gaps") or []:
        pin = gap.get("pinned_by")
        if isinstance(pin, dict):
            for os_name in [o for o in oses if o == "any" or o in scorecard.ref_oses(entry, pin, data)]:
                status, _ = eval_ref(pin, os_name, entry, data, runs, ci_jobs, repo)
                if status == "fail":
                    notes.append(f"the pin {scorecard.ref_label(pin)} no longer holds on {os_name}: "
                                 f"the clause may be fixed, so flip the test and update the registry")
    overall = [v["status"] for v in per_os.values()]
    status = "fail" if "fail" in overall else "unmeasured" if "unmeasured" in overall or not overall else "pass"
    return {"id": entry["id"], "phase": entry["phase"], "class": entry.get("class"), "owner": entry.get("owner"),
            "title": entry.get("title"), "status": status, "platforms": per_os, "notes": notes,
            "gaps": [f"{g.get('clause')} ({g.get('state')}; {g.get('owner')})" for g in entry.get("gaps") or []]}


def ci_job_conclusions(path: Path | None) -> dict | None:
    if path is None or not path.is_file():
        return None
    listing = json.loads(path.read_text(encoding="utf-8"))
    return {j.get("name"): j.get("conclusion") for j in listing.get("jobs", [])}


def _stamp(value) -> datetime | None:
    try:
        return datetime.strptime(str(value), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def build_report(data: dict, runs: list[Results], ci_jobs: dict | None, previous: dict | None, scheduled: bool,
                 phase: int, repo: Path, now: datetime | None = None) -> dict:
    now = now or datetime.now(timezone.utc)
    previous = previous or {}
    generated = now.strftime("%Y-%m-%dT%H:%M:%SZ")
    # The time of the last scheduled report: a dispatch report carries it forward unchanged.
    last_scheduled = previous.get("last_scheduled") or (previous.get("generated") if previous.get("scheduled") else None)
    gap = None
    if scheduled and last_scheduled and _stamp(last_scheduled):
        gap = (now - _stamp(last_scheduled)).total_seconds() / 3600
    restart = gap is not None and gap > MAX_NIGHT_GAP_HOURS
    before = {(c["id"], c["phase"]): c for c in previous.get("criteria", []) + previous.get("exit", [])}
    out: dict = {"generated": generated, "phase": phase, "scheduled": scheduled,
                 "last_scheduled": generated if scheduled else last_scheduled,
                 "runs": sorted({r.name for r in runs}), "criteria": [], "exit": []}
    if restart:
        out["streaks_restarted"] = f"no scheduled report for {gap:.0f} h (since {last_scheduled})"
    for key in ("criteria", "exit"):
        for entry in (e for e in data.get(key) or [] if e.get("phase") == phase):
            result = evaluate(entry, data, runs, ci_jobs, repo)
            prev = before.get((entry["id"], entry["phase"]), {})
            streak = 0 if restart else int(prev.get("streak", 0))
            if result["status"] != "pass":
                streak = 0
            elif scheduled:
                streak += 1
            result["streak"] = streak
            # An M record is green once it exists; the others need their streak of scheduled passes.
            result["green"] = result["status"] == "pass" and (
                entry.get("class") == "M" or streak >= GREEN_STREAK.get(entry.get("class"), 3))
            result["history"] = (prev.get("history", []) + [result["status"]])[-14:]
            out[key].append(result)
    crit = out["criteria"]
    count = {s: sum(1 for c in crit if c["status"] == s) for s in ("pass", "fail", "unmeasured")}
    green = sum(1 for c in crit if c["green"])
    out["summary"] = {**count, "green": green, "total": len(crit),
                      "green_fraction": round(green / len(crit), 4) if crit else 0.0}
    return out


ICON = {"pass": "pass", "fail": "**FAIL**", "unmeasured": "unmeasured"}
HEADER = ["| Criterion | Tonight | Linux | Windows | Streak | Class | Owner | Open gaps or failures |",
          "|---|---|---|---|---|---|---|---|"]


def markdown(report: dict) -> str:
    s = report["summary"]
    lines = [f"## Scorecard: Phase {report['phase']}", "",
             f"{s['pass']} of {s['total']} criteria pass tonight, {s['fail']} fail and {s['unmeasured']} are "
             f"unmeasured. **{s['green']} are green** under 09 §5.6 (the streak each class needs), so the scorecard "
             f"part of the round score (60 %, §5.7) is {100 * s['green_fraction']:.1f} % of its maximum.", ""]
    if report.get("streaks_restarted"):
        lines += [f"Every streak restarted tonight: {report['streaks_restarted']}.", ""]
    lines += HEADER

    def row(c: dict) -> str:
        cells = {os_name: ICON[v["status"]] for os_name, v in c["platforms"].items()}
        failing: dict[str, list[tuple[str, str]]] = {}
        for os_name, v in c["platforms"].items():
            for r in v["refs"]:
                if r["status"] != "pass":
                    failing.setdefault(r["ref"], []).append(
                        (os_name, r["status"] + (f": {r['detail']}" if r["detail"] else "")))
        why = list(c["gaps"])
        for ref, where in failing.items():
            same = len({text for _, text in where}) == 1 and len(where) == len(c["platforms"])
            why.append(f"{ref} ({where[0][1] if same else '; '.join(f'{o} {t}' for o, t in where)})")
        why += c["notes"]
        text = "; ".join(why[:4]) + (f"; +{len(why) - 4} more" if len(why) > 4 else "")
        return (f"| {c['id']} | {ICON[c['status']]}{' (green)' if c['green'] else ''} | "
                f"{cells.get('linux', cells.get('any', '—'))} | {cells.get('windows', cells.get('any', '—'))} | "
                f"{c['streak']} | {c['class']} | {c['owner']} | {text.replace('|', '/') or '—'} |")

    lines += [row(c) for c in report["criteria"]]
    if report["exit"]:
        lines += ["", "### Exit items", ""] + HEADER + [row(c) for c in report["exit"]]
    lines += ["", f"Result sets: {', '.join(report['runs']) or 'none'}."]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    scorecard._utf8_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--scorecard", type=Path, default=scorecard.ROOT / "scorecard.jsonc")
    parser.add_argument("--ci-jobs", type=Path)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--scheduled", action="store_true", help="a scheduled nightly: passes extend streaks")
    parser.add_argument("--phase", type=int, default=0)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args(argv)
    try:
        data, _ = scorecard.load_jsonc(args.scorecard)
        previous = json.loads(args.previous.read_text(encoding="utf-8")) if args.previous and args.previous.is_file() else None
        runs = load_results(args.results, scorecard.go_module()) if args.results.is_dir() else []
    except (scorecard.ScorecardError, OSError, ValueError, KeyError) as e:
        print(f"report: error: {e}")
        return 2
    unknown = sorted({r.name for r in runs} - set(data.get("runs") or {}))
    if unknown:
        print(f"report: error: result sets with undeclared runs: {', '.join(unknown)}")
        return 2
    report = build_report(data, runs, ci_job_conclusions(args.ci_jobs), previous, args.scheduled, args.phase,
                          scorecard.ROOT)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    text = markdown(report)
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
