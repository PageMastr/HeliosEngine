#!/usr/bin/env python3
"""The AAA scorecard registry, scorecard.jsonc, and its checker (docs/plan/09 §5.6-5.8; WP-0.3).

  python3 tools/scorecard/scorecard.py check [--scorecard FILE] [--plan DIR] [--inventory FILE]...
                                             [--build-dir DIR [--config CFG] [--ctest EXE]]
  python3 tools/scorecard/scorecard.py inventory [--build-dir DIR [--config CFG] [--ctest EXE]]
                                                 [--go-list FILE]... [--os linux|windows] --out FILE

`check` validates every entry's fields; criterion IDs and phases against the plan's criterion tables
(01 §3, 02 §8.2, 03 §9.3, 04 §11.4, 05 §10, 06 §12.2, 07 §5.2, 08 §4.4) and 09 §2's phase exits; that
every criterion of each phase listed in "covers" is registered; `ci_job` names against ci.yml; and,
given inventories or a build directory, that every referenced CTest, doctest case and Go test exists on
the platforms it is expected on. Findings print as `file:line: message`. `inventory` records the tests
of a build (CTest names, doctest cases per binary) and of `go test -list` output as JSON.

Standard library only, no network. The report and perf tools import this module; nothing here is
thread-safe or meant to be.
"""

from __future__ import annotations

import argparse
import fnmatch
import itertools
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = {"N", "H", "W", "M", "N+H"}
PLATFORMS = {"linux", "windows", "any"}
OSES = ("linux", "windows")
STATUSES = {"measured", "partial", "unmeasured"}
GAP_STATES = {"unmeasured", "failing"}
REF_KINDS = {"ctest": set(), "doctest": {"case"}, "go": {"test"}, "gate": set(), "ci_job": set(), "evidence": set()}
REF_OPTIONAL = {"platforms", "run", "note", "tags"}
ENTRY_REQUIRED = {"id", "phase", "source", "owner", "title", "class", "platforms", "threshold", "status", "tests"}
ENTRY_OPTIONAL = {"gaps", "notes"}
TOP_KEYS = {"version", "plan_rev", "covers", "runs", "gates", "criteria", "exit"}
OWNER = re.compile(r"^(?:WP-\d+\.\d+[a-z0-9]*|User|Director)(?:, (?:WP-\d+\.\d+[a-z0-9]*|User|Director))*$")
SOURCE = re.compile(r"^0\d §\d+(?:\.\d+)*[a-z]?$")
EXIT_ID = re.compile(r"^EXIT-(\d)\.[a-z0-9][a-z0-9-]*$")
AAA_FAMILIES = {"REN", "SRV", "ITR", "STB", "CNT", "SEC", "TOOL", "PLT"}
FAMILIES = {"RT", "RC", "NS", "BE", "GP", "ED", "CL"}
# (file, criterion ID pattern, prefix added to the ID, section the table lives in or None for the file)
PLAN_TABLES = (
    ("01-vision-and-scope.md", r"AAA-[A-Z]+-\d+", "", None),
    ("02-engine-runtime.md", r"RT-\d+", "", "### 8.2 "),
    ("03-rendering.md", r"RC-\d+", "", "### 9.3 "),
    ("05-backend-services.md", r"A\d+[a-z]?", "BE-", "## 10. "),
    ("07-editor-and-tools.md", r"ED-\d+", "", "### 5.2 "),
    ("08-client-and-launcher.md", r"CL-\d+", "", "### 4.4 "),
)


class ScorecardError(Exception):
    """A registry or input file that cannot be read at all."""


def display(path: Path) -> str:
    try:
        return Path(path).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


# ------------------------------------------------------------------------------------------------
# JSONC
# ------------------------------------------------------------------------------------------------
def strip_jsonc(text: str) -> str:
    """JSON text from JSONC: comments become whitespace (newlines kept, so line numbers hold) and
    trailing commas before `]` or `}` are dropped."""
    out: list[str] = []
    i, n, in_str = 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(text[i:i + 2] if c == "\\" else c)
            i += 2 if c == "\\" else 1
            in_str = c != '"'
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unterminated /* comment")
            out.append("\n" * text.count("\n", i, end))
            i = end + 2
            continue
        if c in "]}":
            k = len(out) - 1
            while k >= 0 and out[k].strip() == "":
                k -= 1
            if k >= 0 and out[k] == ",":
                out[k] = " "
        in_str = c == '"'
        out.append(c)
        i += 1
    return "".join(out)


def load_jsonc(path: Path):
    """(data, raw text) of a JSONC file; ScorecardError with file:line on a syntax error."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as e:
        raise ScorecardError(f"{display(path)}: {e.strerror}") from e
    try:
        return json.loads(strip_jsonc(text)), text
    except json.JSONDecodeError as e:
        raise ScorecardError(f"{display(path)}:{e.lineno}: {e.msg}") from e
    except ValueError as e:
        raise ScorecardError(f"{display(path)}: {e}") from e


def entries(data: dict) -> list[dict]:
    """Criteria then exit items, each entry as written."""
    return [e for key in ("criteria", "exit") for e in (data.get(key) or []) if isinstance(e, dict)]


def entry_lines(text: str, items: list[dict]) -> list[int]:
    """Line of each entry's `"id"` in the raw text (the n-th occurrence for a repeated ID)."""
    seen: dict[str, int] = {}
    lines = []
    for e in items:
        ident = str(e.get("id"))
        hits = [m.start() for m in re.finditer(r'"id"\s*:\s*' + re.escape(json.dumps(ident)), text)]
        k = seen.get(ident, 0)
        seen[ident] = k + 1
        lines.append(text.count("\n", 0, hits[k]) + 1 if k < len(hits) else 1)
    return lines


def ref_kind(ref: dict) -> str | None:
    kinds = [k for k in REF_KINDS if k in ref]
    return kinds[0] if len(kinds) == 1 else None


def ref_label(ref: dict) -> str:
    kind = ref_kind(ref) or "?"
    extra = ref.get("case") or ref.get("test")
    return f"{kind} {ref.get(kind)}" + (f" / {extra}" if extra else "")


def ref_platforms(entry: dict, ref: dict) -> list[str]:
    return list(ref.get("platforms") or entry.get("platforms") or [])


def ref_oses(entry: dict, ref: dict, data: dict) -> list[str]:
    """The OSes a reference is evaluated on: its run's OS, else its (or its entry's) platforms."""
    run = (data.get("runs") or {}).get(ref.get("run"))
    if isinstance(run, dict):
        return [run.get("os")]
    return [p for p in ref_platforms(entry, ref) if p in OSES]


# ------------------------------------------------------------------------------------------------
# Plan
# ------------------------------------------------------------------------------------------------
def _cells(line: str) -> list[str] | None:
    s = line.strip()
    if len(s) < 2 or not (s.startswith("|") and s.endswith("|")):
        return None
    return [c.strip().replace("\0", "|") for c in s.replace("\\|", "\0")[1:-1].split("|")]


def _tables(text: str):
    lines = text.splitlines()
    i = 0
    while i + 1 < len(lines):
        head, sep = _cells(lines[i]), _cells(lines[i + 1])
        if head and sep and all(re.fullmatch(r":?-{3,}:?", c) for c in sep):
            rows, i = [], i + 2
            while i < len(lines) and (row := _cells(lines[i])) is not None:
                rows.append(row)
                i += 1
            yield head, rows
        else:
            i += 1


def _section(text: str, heading: str | None) -> str:
    if heading is None:
        return text
    start = text.find("\n" + heading)
    if start < 0:
        return ""
    level = heading.split(" ", 1)[0]
    end = re.search(r"^#{1,%d} " % len(level), text[start + 1 + len(heading):], re.M)
    return text[start:start + 1 + len(heading) + (end.start() if end else len(text))]


def parse_phases(cell: str) -> set[int]:
    """Phases a Ph cell names: "0", "0, 2", "0–1", "4 (1)" (the Ph1 slice), "1–4 (…: 3; …)" and so on."""
    head = cell.split("(", 1)[0]
    phases = {int(x) for x in re.findall(r"(?<![\d.])(\d)(?![\d.])", head)}
    for a, b in re.findall(r"(\d)\s*[–-]\s*(\d)", head):
        phases.update(range(int(a), int(b) + 1))
    phases.update(int(x) for x in re.findall(r"\((\d)\)", cell))
    return phases


ID_TAIL = re.compile(r"(A?)(\d+)(?:\.(\d+))?([a-z]?)(?:\s*[–…]\s*(?:\d+\.)?(\d+))?(?!\w|\.\d)")


def exit_ids(text: str) -> set[str]:
    """Criterion IDs named in a phase-exit paragraph. A bare number continues the previous family
    ("ITR-1 (Ph1), 3, 5", "BE-A1, A2"); ranges use – or … ("NS-0.1–0.7", "RT-01…05")."""
    ids: set[str] = set()
    prefix = None
    for token in re.split(r"[;,]", text):
        t = token.strip()
        if ": " in t and not re.match(r"(?:AAA-)?[A-Z]+-", t):
            t = t.rsplit(": ", 1)[1]  # "Every criterion with Ph ≤ 1: AAA-REN-1 (Ph1 slice)"
        m = re.match(r"(?:AAA-)?([A-Z]+)-", t)
        if m:
            prefix = f"AAA-{m[1]}-" if m[1] in AAA_FAMILIES else f"{m[1]}-" if m[1] in FAMILIES else None
            t = t[m.end():]
        r = ID_TAIL.match(t) if prefix else None
        if not r:
            prefix = None
            continue
        letter_a, major, minor, suffix, high = r.groups()
        if minor is not None:
            ids.update(f"{prefix}{major}.{k}" for k in range(int(minor), int(high or minor) + 1))
        elif high:
            ids.update(f"{prefix}{letter_a}{k:0{len(major)}d}" for k in range(int(major), int(high) + 1))
        else:
            ids.add(f"{prefix}{letter_a}{major}{suffix}")
    return ids


def plan_criteria(plan_dir: Path) -> tuple[dict[str, dict], dict[int, set[str]]]:
    """({ID: {"phases": set, "section": "02", "class": str|None}}, {phase: IDs named in 09 §2's exit})."""
    found: dict[str, dict] = {}

    def add(ident: str, section: str, phases: set[int], cls: str | None = None) -> None:
        item = found.setdefault(ident, {"phases": set(), "section": section, "class": None})
        item["phases"] |= phases
        item["class"] = item["class"] or cls

    def read(name: str) -> str:
        path = plan_dir / name
        if not path.is_file():
            raise ScorecardError(f"{display(path)}: plan section not found")
        return path.read_text(encoding="utf-8")

    for name, pattern, prefix, heading in PLAN_TABLES:
        for head, rows in _tables(_section(read(name), heading)):
            ph = next((i for i, h in enumerate(head) if h in ("Ph", "Phase")), None)
            per_phase = [(i, int(h[2:])) for i, h in enumerate(head) if re.fullmatch(r"Ph\d", h)]
            cls = next((i for i, h in enumerate(head) if h == "Class"), None)
            if ph is None and not per_phase:
                continue
            for row in rows:
                m = re.match(pattern + r"(?![\w.])", row[0].strip("* "))
                if not m or len(row) != len(head):
                    continue
                phases = parse_phases(row[ph]) if ph is not None else {
                    p for i, p in per_phase if row[i].strip() not in ("—", "-", "")}
                add(prefix + m[0], name[:2], phases, row[cls].replace(" ", "") if cls is not None else None)
    for head, rows in _tables(_section(read("04-networking-and-servers.md"), "### 11.4 ")):
        for row in rows:
            m = re.fullmatch(r"\*\*(NS-(\d+)\.\d+)\*\*", row[0])
            if m:
                add(m[1], "04", {int(m[2])})
    gp = _section(read("06-gameplay-framework.md"), "### 12.2 ")
    items = list(re.finditer(r"^(\d+)\.\s+\*\*", gp, re.M))
    for k, m in enumerate(items):
        body = gp[m.end():items[k + 1].start() if k + 1 < len(items) else len(gp)]
        subs = re.findall(r"^\s+- \(([a-z])\) \*", body, re.M) if m[1] == "4" else []
        for ident in ([f"GP-4{s}" for s in subs] if subs else [f"GP-{m[1]}"]):
            add(ident, "06", set())
    exits: dict[int, set[str]] = {}
    roadmap = read("09-roadmap-and-process.md")
    for m in re.finditer(r"^### 2\.\d+ Phase (\d)\b", roadmap, re.M):
        body = roadmap[m.end():]
        stop = re.search(r"^#{1,3} ", body, re.M)
        para = re.search(r"^\*\*Exit\.\*\*(.*?)(?:\n\s*\n|\Z)", body[:stop.start() if stop else len(body)], re.M | re.S)
        if para:
            exits[int(m[1])] = exit_ids(para[1])
    return found, exits


# ------------------------------------------------------------------------------------------------
# Workflows and inventories
# ------------------------------------------------------------------------------------------------
def workflow_jobs(path: Path) -> list[str]:
    """Display names of a workflow's jobs, `${{ matrix.<axis> }}` expanded from inline axis lists and
    `include` entries. A line-based reader for this repository's 2-space YAML, not a YAML parser."""
    jobs: list[dict] = []
    in_jobs = in_strategy = False
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if re.match(r"^\S", line):
            in_jobs = line.rstrip() == "jobs:"
            continue
        if not in_jobs:
            continue
        if m := re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line):
            jobs.append({"name": m[1], "axes": {}, "include": []})
            in_strategy = False
            continue
        if not jobs:
            continue
        job = jobs[-1]
        if m := re.match(r"^    (\w+):\s*(.*?)\s*$", line):
            in_strategy = m[1] == "strategy"
            if m[1] == "name":
                job["name"] = m[2].strip("'\"")
        elif in_strategy and (m := re.match(r"^        ([\w-]+):\s*\[(.*)\]\s*$", line)):
            job["axes"][m[1]] = [v.strip().strip("'\"") for v in m[2].split(",") if v.strip()]
        elif in_strategy and (m := re.match(r"^          - ([\w-]+):\s*(.+?)\s*$", line)):
            job["include"].append({m[1]: m[2].strip("'\"")})
        elif in_strategy and job["include"] and (m := re.match(r"^            ([\w-]+):\s*(.+?)\s*$", line)):
            job["include"][-1][m[1]] = m[2].strip("'\"")
    names = []
    for job in jobs:
        combos = [dict(zip(job["axes"], values)) for values in itertools.product(*job["axes"].values())]
        combos = (combos if job["axes"] else []) + job["include"] or [{}]
        for combo in combos:
            names.append(re.sub(r"\$\{\{\s*matrix\.([\w-]+)\s*\}\}", lambda m: combo.get(m[1], m[0]), job["name"]))
    return names


def doctest_cases(command: list[str], cwd: str | None = None) -> list[str]:
    """Test case names of a doctest binary (`--list-test-cases`); `command` runs the binary."""
    proc = subprocess.run(command + ["--list-test-cases", "--no-intro=true", "--no-version=true"], cwd=cwd,
                          capture_output=True, encoding="utf-8", errors="replace", timeout=120)
    if proc.returncode != 0:
        raise ScorecardError(f"{command[-1]}: --list-test-cases exited {proc.returncode}: {proc.stderr.strip()[:500]}")
    return parse_doctest_list(proc.stdout)


def parse_doctest_list(text: str) -> list[str]:
    return [line for line in text.splitlines()
            if line and not line.startswith("[doctest]") and not re.fullmatch(r"=+", line)]


def ctest_tests(build_dir: Path, config: str | None, ctest: str) -> list[dict]:
    """`ctest --show-only=json-v1` test entries of a build directory."""
    cmd = [ctest, "--test-dir", str(build_dir), "--show-only=json-v1"] + (["-C", config] if config else [])
    proc = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", timeout=300)
    if proc.returncode != 0:
        raise ScorecardError(f"{' '.join(cmd)} exited {proc.returncode}: {proc.stderr.strip()[:500]}")
    return json.loads(proc.stdout).get("tests", [])


DOCTEST_ENTRY = {"--test-case-exclude=perf:*": "main", "--test-case=perf:*": "perf"}


def is_doctest_entry(test: dict) -> str | None:
    """"main" or "perf" for the two CTest entries helios_test() registers per doctest binary (its filter
    argument follows the binary, which an emulator or launcher may precede)."""
    return next((DOCTEST_ENTRY[a] for a in (test.get("command") or [])[1:] if a in DOCTEST_ENTRY), None)


def doctest_binary_command(test: dict) -> list[str]:
    """The command that runs the entry's binary, without its perf filter."""
    cmd = test.get("command") or []
    return [a for a in cmd if a not in DOCTEST_ENTRY]


def test_property(test: dict, name: str):
    return next((p.get("value") for p in test.get("properties", []) if p.get("name") == name), None)


def build_inventory(build_dir: Path, config: str | None = None, ctest: str = "ctest") -> dict:
    tests = ctest_tests(build_dir, config, ctest)
    doctest = {t["name"]: doctest_cases(doctest_binary_command(t), test_property(t, "WORKING_DIRECTORY"))
               for t in tests if is_doctest_entry(t) == "main"}
    return {"ctest": sorted(t["name"] for t in tests), "doctest": doctest}


def go_module(services: Path = ROOT / "services") -> str:
    text = (services / "go.mod").read_text(encoding="utf-8")
    m = re.search(r"^module\s+(\S+)", text, re.M)
    if not m:
        raise ScorecardError(f"{display(services / 'go.mod')}: no module line")
    return m[1]


def parse_go_list(text: str, module: str) -> dict[str, list[str]]:
    """{package relative to the module: [test names]} from `go test -list . ./...` output."""
    found: dict[str, list[str]] = {}
    pending: list[str] = []
    for line in text.splitlines():
        if re.fullmatch(r"(?:Test|Fuzz|Example|Benchmark)\w*", line.strip()):
            pending.append(line.strip())
        elif m := re.match(r"^(?:ok|\?)\s+(\S+)", line):
            pkg = m[1][len(module) + 1:] if m[1].startswith(module + "/") else m[1]
            found.setdefault(pkg, []).extend(pending)
            pending = []
    return found


def host_os() -> str:
    return "windows" if sys.platform.startswith(("win", "cygwin", "msys")) else "linux"


# ------------------------------------------------------------------------------------------------
# Checks
# ------------------------------------------------------------------------------------------------
def _check_ref(where: str, ref, entry: dict, data: dict, errors: list[str]) -> None:
    kind = ref_kind(ref) if isinstance(ref, dict) else None
    if kind is None:
        errors.append(f"{where}: a test reference needs exactly one of {', '.join(sorted(REF_KINDS))}")
        return
    allowed = {kind} | REF_KINDS[kind] | REF_OPTIONAL
    for key in sorted(set(ref) - allowed):
        errors.append(f"{where}: unknown field '{key}' in {ref_label(ref)}")
    for key in [kind, *sorted(REF_KINDS[kind])]:
        if not isinstance(ref.get(key), str) or not ref[key].strip():
            errors.append(f"{where}: {ref_label(ref)} needs a non-empty string '{key}'")
    plats = ref.get("platforms")
    if plats is not None and (not isinstance(plats, list) or not plats or not set(plats) <= set(entry.get("platforms") or [])):
        errors.append(f"{where}: {ref_label(ref)}: 'platforms' must be a non-empty subset of the entry's platforms")
    runs = data.get("runs") or {}
    if "run" in ref:
        run = runs.get(ref["run"])
        if run is None:
            errors.append(f"{where}: {ref_label(ref)} names unknown run '{ref['run']}'")
        elif run.get("os") not in ref_platforms(entry, ref):
            errors.append(f"{where}: {ref_label(ref)}: run '{ref['run']}' is not on the reference's platforms")
    if "tags" in ref and kind != "go":
        errors.append(f"{where}: 'tags' applies to go references only")
    if kind == "gate":
        gate = (data.get("gates") or {}).get(ref["gate"])
        if gate is None:
            errors.append(f"{where}: {ref_label(ref)} is not declared in 'gates'")
        else:
            gate_oses = {runs.get(r, {}).get("os") for r in gate.get("runs") or []}
            for p in set(ref_platforms(entry, ref)) - gate_oses - {"any"}:
                errors.append(f"{where}: {ref_label(ref)} never runs on {p}")
    if kind == "evidence" and (Path(ref["evidence"]).is_absolute() or ".." in Path(ref["evidence"]).parts):
        errors.append(f"{where}: evidence paths are relative to the repository root")


def validate(data: dict, text: str, path: Path, plan: tuple[dict, dict] | None = None,
             ci_jobs: list[str] | None = None) -> tuple[list[str], list[str]]:
    """(errors, notes) for a loaded registry; errors are `file:line: message` strings."""
    name = display(path)
    errors: list[str] = []
    notes: list[str] = []
    if not isinstance(data, dict):
        return [f"{name}:1: the registry must be a JSON object"], notes
    for key in sorted(set(data) - TOP_KEYS):
        errors.append(f"{name}:1: unknown top-level key '{key}'")
    if data.get("version") != 1:
        errors.append(f"{name}:1: 'version' must be 1")
    if not isinstance(data.get("plan_rev"), int):
        errors.append(f"{name}:1: 'plan_rev' must be an integer")
    for key in ("criteria", "exit"):
        if not isinstance(data.get(key, []), list):
            errors.append(f"{name}:1: '{key}' must be a list of entries")
    covers = data.get("covers", [])
    if not isinstance(covers, list) or not all(isinstance(p, int) and 0 <= p <= 5 for p in covers):
        errors.append(f"{name}:1: 'covers' must list phases 0-5")
        covers = []
    for run_name, run in (data.get("runs") or {}).items():
        if not isinstance(run, dict) or run.get("os") not in OSES or not isinstance(run.get("default"), bool):
            errors.append(f"{name}:1: run '{run_name}' needs 'os' (linux or windows) and a boolean 'default'")
    for gate_name, gate in (data.get("gates") or {}).items():
        ok = isinstance(gate, dict) and isinstance(gate.get("runs"), list) and gate["runs"] and \
            all(r in (data.get("runs") or {}) for r in gate["runs"]) and \
            isinstance(gate.get("min_seconds", 0), int) and isinstance(gate.get("description", ""), str)
        if not ok:
            errors.append(f"{name}:1: gate '{gate_name}' needs 'runs' naming declared runs and an integer 'min_seconds'")
    known, exits = plan if plan else ({}, {})
    items = entries(data)
    lines = entry_lines(text, items)
    seen: set[tuple] = set()
    for entry, line in zip(items, lines):
        where = f"{name}:{line}"
        ident, phase = entry.get("id"), entry.get("phase")
        for key in sorted(ENTRY_REQUIRED - set(entry)):
            errors.append(f"{where}: {ident}: missing field '{key}'")
        for key in sorted(set(entry) - ENTRY_REQUIRED - ENTRY_OPTIONAL):
            errors.append(f"{where}: {ident}: unknown field '{key}'")
        is_exit = any(entry is x for x in data.get("exit") or [])
        if not isinstance(phase, int) or not 0 <= phase <= 5:
            errors.append(f"{where}: {ident}: 'phase' must be an integer 0-5")
        if (ident, phase) in seen:
            errors.append(f"{where}: {ident} is registered twice for phase {phase}")
        seen.add((ident, phase))
        if is_exit:
            m = EXIT_ID.match(str(ident))
            if not m or int(m[1]) != phase:
                errors.append(f"{where}: exit item IDs are EXIT-<phase>.<slug> with the entry's phase")
        elif plan:
            crit = known.get(ident)
            if crit is None:
                errors.append(f"{where}: unknown criterion ID '{ident}' (not in the plan's criterion tables)")
            elif crit["phases"] and phase not in crit["phases"] and ident not in exits.get(phase, set()):
                errors.append(f"{where}: {ident} has no phase-{phase} scope in {crit['section']} "
                              f"(plan phases {sorted(crit['phases'])})")
            elif crit["class"] and str(entry.get("class")).replace(" ", "") != crit["class"]:
                errors.append(f"{where}: {ident}: class '{entry.get('class')}' differs from the plan's '{crit['class']}'")
        for key in ("title", "threshold", "source", "owner"):
            if key in entry and (not isinstance(entry[key], str) or not entry[key].strip()):
                errors.append(f"{where}: {ident}: '{key}' must be a non-empty string")
        if isinstance(entry.get("source"), str) and not SOURCE.match(entry["source"]):
            errors.append(f"{where}: {ident}: 'source' must be a plan anchor such as '04 §11.4'")
        if isinstance(entry.get("owner"), str) and not OWNER.match(entry["owner"]):
            errors.append(f"{where}: {ident}: 'owner' must name WPs (WP-0.13), 'User' or 'Director'")
        if "class" in entry and entry["class"] not in CLASSES:
            errors.append(f"{where}: {ident}: 'class' must be one of {', '.join(sorted(CLASSES))} (09 §5.6)")
        plats = entry.get("platforms")
        if "platforms" in entry and (not isinstance(plats, list) or not plats or not set(plats) <= PLATFORMS
                                     or ("any" in plats and len(plats) > 1)):
            errors.append(f"{where}: {ident}: 'platforms' must be 'any' or a non-empty list of linux and windows")
        tests = entry.get("tests")
        gaps = entry.get("gaps", [])
        if not isinstance(tests, list) or not isinstance(gaps, list):
            errors.append(f"{where}: {ident}: 'tests' and 'gaps' must be lists")
            continue
        status = entry.get("status")
        want = "unmeasured" if not tests else "partial" if gaps else "measured"
        if status not in STATUSES:
            errors.append(f"{where}: {ident}: 'status' must be one of {', '.join(sorted(STATUSES))}")
        elif status != want:
            errors.append(f"{where}: {ident}: status '{status}' but it has {len(tests)} test(s) and "
                          f"{len(gaps)} gap(s), so it is '{want}'")
        if not tests and not gaps:
            errors.append(f"{where}: {ident}: an entry without tests must list its gaps")
        for ref in tests:
            _check_ref(where, ref, entry, data, errors)
            if isinstance(ref, dict) and ci_jobs is not None and "ci_job" in ref and ref["ci_job"] not in ci_jobs:
                errors.append(f"{where}: {ident}: ci_job '{ref['ci_job']}' is not a job of .github/workflows/ci.yml")
        for gap in gaps:
            if not isinstance(gap, dict) or set(gap) - {"clause", "state", "owner", "pinned_by"} or \
                    not isinstance(gap.get("clause"), str) or gap.get("state") not in GAP_STATES or \
                    not isinstance(gap.get("owner"), str):
                errors.append(f"{where}: {ident}: a gap needs 'clause', 'state' (unmeasured or failing), 'owner' "
                              f"and optionally 'pinned_by'")
                continue
            if "pinned_by" in gap:
                _check_ref(where, gap["pinned_by"], entry, data, errors)
    if plan:
        for phase in covers:
            required = {i for i, c in known.items() if phase in c["phases"]} | exits.get(phase, set())
            registered = {e.get("id") for e in data.get("criteria") or [] if e.get("phase") == phase}
            for ident in sorted(required - registered):
                where = known[ident]["section"] if ident in known else "09"
                errors.append(f"{name}:1: phase-{phase} criterion {ident} ({where}) is not registered")
            for ident in sorted(exits.get(phase, set()) - set(known)):
                notes.append(f"09 §2 names {ident} in the phase-{phase} exit, but no criterion table defines it")
    return errors, notes


def check_inventory(data: dict, text: str, path: Path, inventory: dict) -> list[str]:
    """Errors for references that do not exist in an inventory of one OS. Only the sections the inventory
    has ("ctest", "doctest", "go") are checked, so a Go-only inventory says nothing about CTests."""
    name = display(path)
    inv_os = inventory.get("os")
    ctests = inventory.get("ctest")
    doctest = inventory.get("doctest")
    go = inventory.get("go")
    errors = []
    items = entries(data)
    for entry, line in zip(items, entry_lines(text, items)):
        refs = [r for r in entry.get("tests") or [] if isinstance(r, dict)]
        refs += [g["pinned_by"] for g in entry.get("gaps") or [] if isinstance(g, dict) and isinstance(g.get("pinned_by"), dict)]
        for ref in refs:
            if inv_os not in ref_oses(entry, ref, data):
                continue
            kind, target = ref_kind(ref), None
            if kind == "ctest" and ctests is not None and not any(fnmatch.fnmatchcase(t, ref["ctest"]) for t in ctests):
                target = "CTest"
            elif kind == "doctest" and doctest is not None and (
                    ref["doctest"] not in doctest or
                    not any(fnmatch.fnmatchcase(c, ref["case"]) for c in doctest[ref["doctest"]])):
                target = "doctest case"
            elif kind == "go" and go is not None and ref["test"] not in go.get(ref["go"], []):
                target = "Go test"
            if target:
                errors.append(f"{name}:{line}: {entry.get('id')}: {target} {ref_label(ref)} does not exist "
                              f"in the {inv_os} inventory")
    return errors


# ------------------------------------------------------------------------------------------------
# Command line
# ------------------------------------------------------------------------------------------------
def _utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def cmd_check(args) -> int:
    data, text = load_jsonc(args.scorecard)
    plan_dir = Path(args.plan)
    plan = plan_criteria(plan_dir)
    ci = Path(args.workflows) / "ci.yml"
    errors, notes = validate(data, text, args.scorecard, plan, workflow_jobs(ci) if ci.is_file() else None)
    rev_file = plan_dir / "PLAN-REV"
    if rev_file.is_file() and isinstance(data.get("plan_rev"), int):
        rev = int(re.match(r"\s*(\d+)", rev_file.read_text(encoding="utf-8"))[1])
        if data["plan_rev"] > rev:
            errors.append(f"{display(args.scorecard)}:1: plan_rev {data['plan_rev']} is above PLAN-REV {rev}")
    inventories = [load_jsonc(Path(p))[0] for p in args.inventory]
    if args.build_dir:
        inv = build_inventory(Path(args.build_dir), args.config or None, args.ctest)
        inventories.append({"os": host_os(), **inv})
    checked = []
    for inv in inventories:
        errors += check_inventory(data, text, args.scorecard, inv)
        checked.append(f"{inv.get('os')} ({len(inv.get('ctest') or [])} CTests, "
                       f"{sum(len(v) for v in (inv.get('doctest') or {}).values())} doctest cases"
                       + (f", {sum(len(v) for v in inv['go'].values())} Go tests" if inv.get("go") is not None else "") + ")")
    for note in notes:
        print(f"note: {note}")
    for error in errors:
        print(error)
    tally = {s: sum(1 for e in data.get("criteria") or [] if e.get("status") == s) for s in sorted(STATUSES)}
    print(f"scorecard: {len(data.get('criteria') or [])} criteria ({', '.join(f'{v} {k}' for k, v in tally.items())}), "
          f"{len(data.get('exit') or [])} exit items, covers phases {data.get('covers', [])}; "
          f"tests checked against: {', '.join(checked) or 'no inventory'}")
    if errors:
        print(f"scorecard: {len(errors)} finding(s)")
        return 1
    print("scorecard: OK")
    return 0


def cmd_inventory(args) -> int:
    inv: dict = {"os": args.os or host_os()}
    if args.build_dir:
        inv.update(build_inventory(Path(args.build_dir), args.config or None, args.ctest))
    if args.go_list:
        module = go_module()
        inv["go"] = {}
        for listing in args.go_list:
            for pkg, tests in parse_go_list(Path(listing).read_text(encoding="utf-8"), module).items():
                inv["go"][pkg] = sorted(set(inv["go"].get(pkg, [])) | set(tests))
    Path(args.out).write_text(json.dumps(inv, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"inventory: {args.out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    _utf8_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="validate scorecard.jsonc")
    check.add_argument("--scorecard", type=Path, default=ROOT / "scorecard.jsonc")
    check.add_argument("--plan", default=str(ROOT / "docs" / "plan"))
    check.add_argument("--workflows", default=str(ROOT / ".github" / "workflows"))
    check.add_argument("--inventory", action="append", default=[], help="inventory JSON (repeatable)")
    inventory = sub.add_parser("inventory", help="write a test inventory")
    inventory.add_argument("--go-list", action="append", default=[], help="`go test -list . ./...` output")
    inventory.add_argument("--os", choices=OSES)
    inventory.add_argument("--out", required=True)
    for p in (check, inventory):
        p.add_argument("--build-dir")
        p.add_argument("--config", default="")
        p.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)
    try:
        return cmd_check(args) if args.command == "check" else cmd_inventory(args)
    except (ScorecardError, subprocess.SubprocessError, OSError) as e:
        print(f"scorecard: error: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
