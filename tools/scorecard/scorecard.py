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
import itertools
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
CLASSES = {"N", "H", "W", "M", "N+H"}
PLATFORMS = {"linux", "windows", "any"}
OSES = ("linux", "windows")
STATUSES = {"measured", "partial", "unmeasured"}
GAP_STATES = {"unmeasured", "failing"}
REF_KINDS = {"ctest": set(), "doctest": {"case"}, "go": {"test"}, "gate": set(), "ci_job": set(), "evidence": set()}
REF_OPTIONAL = {"platforms", "run", "note", "tags"}
TEST_KINDS = {"ctest", "doctest", "go", "gate"}  # references that run on a platform
ENTRY_REQUIRED = {"id", "phase", "source", "owner", "title", "class", "platforms", "threshold", "status", "tests"}
ENTRY_OPTIONAL = {"gaps", "notes"}
GAP_KEYS = {"clause", "state", "owner", "pinned_by"}
TOP_KEYS = {"version", "plan_rev", "covers", "runs", "gates", "criteria", "exit", "perf_metrics"}
RUN_KEYS = {"os", "default", "nightly", "description"}
GATE_KEYS = {"runs", "min_seconds", "description"}
METRIC_FIELDS = {"id", "criterion", "doctest", "case", "gate", "pattern", "unit", "better", "category", "run", "note"}
METRIC_CATEGORIES = {"render", "runtime", "backend", "editor", "iteration"}
OWNER = re.compile(r"^(?:WP-\d+\.\d+[a-z0-9]*|User|Director)(?:, (?:WP-\d+\.\d+[a-z0-9]*|User|Director))*$")
SOURCE = re.compile(r"^0\d §\d+(?:\.\d+)*[a-z]?$")
EXIT_ID = re.compile(r"^EXIT-(\d)\.[a-z0-9][a-z0-9-]*$")
# A test that pins a known divergence passes while its clause fails, so it can only be a gap's pinned_by.
PINNED_NAME = re.compile(r"(?i)known (?:codegen )?divergence")
AAA_FAMILIES = {"REN", "SRV", "ITR", "STB", "CNT", "SEC", "TOOL", "PLT"}
FAMILIES = {"RT", "RC", "NS", "BE", "GP", "ED", "CL"}
# (file, strict criterion ID pattern, prefix of anything that looks like an ID, prefix added to the ID,
#  section the table lives in or None for the whole file)
PLAN_TABLES = (
    ("01-vision-and-scope.md", r"AAA-[A-Z]+-\d+", r"AAA-", "", None),
    ("02-engine-runtime.md", r"RT-\d+", r"RT-", "", "### 8.2 "),
    ("03-rendering.md", r"RC-\d+", r"RC-", "", "### 9.3 "),
    ("05-backend-services.md", r"A\d+[a-z]?", r"A\d", "BE-", "## 10. "),
    ("07-editor-and-tools.md", r"ED-\d+", r"ED-", "", "### 5.2 "),
    ("08-client-and-launcher.md", r"CL-\d+", r"CL-", "", "### 4.4 "),
)


class ScorecardError(Exception):
    """A registry or input file that cannot be read at all."""


class Plan(NamedTuple):
    """Criteria parsed from docs/plan: {ID: {"phases", "section", "class", "where"}}, the IDs each 09 §2
    phase exit names, where each exit paragraph is, and rows that look like criteria but do not parse."""
    criteria: dict
    exits: dict
    exit_where: dict
    problems: list


def display(path: Path) -> str:
    try:
        return Path(path).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def is_int(value) -> bool:
    """An integer that is not a bool (JSON true would otherwise pass as 1)."""
    return isinstance(value, int) and not isinstance(value, bool)


# ------------------------------------------------------------------------------------------------
# JSONC
# ------------------------------------------------------------------------------------------------
def strip_jsonc(text: str) -> str:
    """JSON text from JSONC: comments become whitespace (newlines kept, so line numbers hold) and a
    trailing comma after a value, before `]` or `}`, is dropped (`[,]` stays invalid)."""
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
            j = k - 1
            while j >= 0 and out[j].strip() == "":
                j -= 1
            if k >= 0 and out[k] == "," and j >= 0 and out[j] not in ("[", "{", ","):
                out[k] = " "
        in_str = c == '"'
        out.append(c)
        i += 1
    return "".join(out)


def load_jsonc(path: Path):
    """(data, raw text) of a JSONC file; ScorecardError with file:line on a syntax error."""
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except OSError as e:
        raise ScorecardError(f"{display(path)}: {e.strerror}") from e
    except UnicodeDecodeError as e:
        raise ScorecardError(f"{display(path)}: not UTF-8 ({e.reason})") from e
    try:
        return json.loads(strip_jsonc(text)), text
    except json.JSONDecodeError as e:
        raise ScorecardError(f"{display(path)}:{e.lineno}: {e.msg}") from e
    except ValueError as e:
        raise ScorecardError(f"{display(path)}: {e}") from e


def entries(data: dict) -> list[dict]:
    """Criteria then exit items, each entry as written (validate() reports items that are not objects)."""
    if not isinstance(data, dict):
        return []
    return [e for key in ("criteria", "exit") if isinstance(data.get(key), list)
            for e in data[key] if isinstance(e, dict)]


def entry_lines(text: str, items: list[dict]) -> list[int]:
    """Line of each entry's `"id"` (the n-th occurrence for a repeated ID), ignoring comments."""
    try:
        text = strip_jsonc(text)
    except ValueError:
        pass
    seen: dict[str, int] = {}
    lines = []
    for e in items:
        ident = e.get("id")
        if not isinstance(ident, str):
            lines.append(1)
            continue
        hits = [m.start() for m in re.finditer(r'"id"\s*:\s*' + re.escape(json.dumps(ident, ensure_ascii=False)), text)]
        k = seen.get(ident, 0)
        seen[ident] = k + 1
        lines.append(text.count("\n", 0, hits[k]) + 1 if k < len(hits) else 1)
    return lines


def matches(name: str, pattern: str) -> bool:
    """A test name against a reference: literal, except that `*` matches any run of characters."""
    return name == pattern or ("*" in pattern and re.fullmatch(".*".join(map(re.escape, pattern.split("*"))), name,
                                                               re.S) is not None)


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


def _unfence(text: str) -> str:
    """The text with every line inside a ``` fence blanked (line numbers kept)."""
    out, fenced = [], False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
        else:
            out.append("" if fenced else line)
    return "\n".join(out)


def _tables(text: str, first_line: int = 1):
    """(header, [(line number, cells)]) for every Markdown table."""
    lines = text.splitlines()
    i = 0
    while i + 1 < len(lines):
        head, sep = _cells(lines[i]), _cells(lines[i + 1])
        if head and sep and all(re.fullmatch(r":?-{3,}:?", c) for c in sep):
            rows, i = [], i + 2
            while i < len(lines) and (row := _cells(lines[i])) is not None:
                rows.append((first_line + i, row))
                i += 1
            yield head, rows
        else:
            i += 1


def _section(text: str, heading: str | None) -> tuple[str, int]:
    """(the section under `heading` up to the next heading of its level or above, its first line)."""
    if heading is None:
        return text, 1
    lines = text.split("\n")
    level = len(heading.split(" ", 1)[0])
    start = next((i for i, line in enumerate(lines) if line.startswith(heading)), None)
    if start is None:
        return "", 1
    end = next((i for i in range(start + 1, len(lines)) if re.match(r"#{1,%d} " % level, lines[i])), len(lines))
    return "\n".join(lines[start:end]), start + 1


def parse_phases(cell: str) -> set[int]:
    """Phases a Ph cell names: "0", "0, 2", "0–1", "4 (1)" (the Ph1 slice), "2 (Luau), 3", "1–4 (…: 3; …)"."""
    outside = re.sub(r"\([^)]*\)", " ", cell)
    phases = {int(x) for x in re.findall(r"(?<![\d.])(\d)(?![\d.])", outside)}
    for a, b in re.findall(r"(\d)\s*[–-]\s*(\d)", outside):
        phases.update(range(int(a), int(b) + 1))
    phases.update(int(x) for x in re.findall(r"\((\d)\)", cell))
    return phases


ID_TAIL = re.compile(r"(A?)(\d+)(?:\.(\d+))?([a-z]?)(?:\s*[–…-]\s*A?(?:\d+\.)?(\d+))?(?!\w|\.\d)")


def exit_ids(text: str) -> set[str]:
    """Criterion IDs named in a phase-exit paragraph. A bare number continues the previous family
    ("ITR-1 (Ph1), 3, 5", "BE-A1, A2"); ranges use –, - or … ("NS-0.1–0.7", "RT-01…05", "A5–A8")."""
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


def plan_criteria(plan_dir: Path) -> Plan:
    """Every criterion ID with its phases, section, class (08's column; M for 01's "(M)" rows) and plan
    line, plus each 09 §2 phase exit. A row that looks like a criterion but cannot be parsed is a problem."""
    found: dict[str, dict] = {}
    problems: list[str] = []

    def add(ident: str, section: str, phases: set[int], where: str, cls: str | None = None) -> None:
        item = found.setdefault(ident, {"phases": set(), "section": section, "class": None, "where": where})
        item["phases"] |= phases
        item["class"] = item["class"] or cls

    def read(name: str) -> tuple[Path, str]:
        path = plan_dir / name
        if not path.is_file():
            raise ScorecardError(f"{display(path)}: plan section not found")
        return path, _unfence(path.read_text(encoding="utf-8-sig"))

    for name, pattern, loose, prefix, heading in PLAN_TABLES:
        path, text = read(name)
        section, first = _section(text, heading)
        for head, rows in _tables(section, first):
            ph = next((i for i, h in enumerate(head) if h in ("Ph", "Phase")), None)
            per_phase = [(i, int(h[2:])) for i, h in enumerate(head) if re.fullmatch(r"Ph\d", h)]
            cls = next((i for i, h in enumerate(head) if h == "Class"), None)
            if ph is None and not per_phase:
                continue
            for line, row in rows:
                first_cell = row[0].strip("* ")
                m = re.match(pattern + r"(?![\w.])", first_cell)
                if not m:
                    if re.match(loose, first_cell):
                        problems.append(f"{display(path)}:{line}: '{first_cell[:40]}' looks like a criterion ID "
                                        f"but does not parse")
                    continue
                if len(row) != len(head):
                    problems.append(f"{display(path)}:{line}: {prefix}{m[0]} has {len(row)} cells, its header {len(head)}")
                    continue
                phases = parse_phases(row[ph]) if ph is not None else {
                    p for i, p in per_phase if row[i].strip() not in ("—", "-", "")}
                klass = row[cls].replace(" ", "") if cls is not None else "M" if name.startswith("01") and \
                    "(M)" in row[1] else None
                add(prefix + m[0], name[:2], phases, f"{display(path)}:{line}", klass)
    path, text = read("04-networking-and-servers.md")
    section, first = _section(text, "### 11.4 ")
    for head, rows in _tables(section, first):
        for line, row in rows:
            m = re.fullmatch(r"(?:\*\*)?(NS-(\d+)\.\d+)(?:\*\*)?", row[0])
            if m:
                add(m[1], "04", {int(m[2])}, f"{display(path)}:{line}")
            elif row[0].strip("* ").startswith("NS-"):
                problems.append(f"{display(path)}:{line}: '{row[0][:40]}' looks like a criterion ID but does not parse")
    path, text = read("06-gameplay-framework.md")
    gp, first = _section(text, "### 12.2 ")
    items = list(re.finditer(r"^(\d+)\.\s+\*\*", gp, re.M))
    for k, m in enumerate(items):
        body = gp[m.end():items[k + 1].start() if k + 1 < len(items) else len(gp)]
        subs = re.findall(r"^\s+- \(([a-z])\) \*", body, re.M) if m[1] == "4" else []
        where = f"{display(path)}:{first + gp.count(chr(10), 0, m.start())}"
        for ident in ([f"GP-4{s}" for s in subs] if subs else [f"GP-{m[1]}"]):
            add(ident, "06", set(), where)
    exits: dict[int, set[str]] = {}
    exit_where: dict[int, str] = {}
    path, roadmap = read("09-roadmap-and-process.md")
    for m in re.finditer(r"^### 2\.\d+ Phase (\d)\b", roadmap, re.M):
        body = roadmap[m.end():]
        stop = re.search(r"^#{1,3} ", body, re.M)
        body = body[:stop.start() if stop else len(body)]
        para = re.search(r"^\*\*Exit\b[^*\n]*\*\*(.*?)(?:\n\s*\n|\Z)", body, re.M | re.S)
        if para:
            exits[int(m[1])] = exit_ids(para[1])
            exit_where[int(m[1])] = f"{display(path)}:{roadmap.count(chr(10), 0, m.end() + para.start()) + 1}"
    return Plan(found, exits, exit_where, problems)


# ------------------------------------------------------------------------------------------------
# Workflows and inventories
# ------------------------------------------------------------------------------------------------
def workflow_jobs(path: Path) -> list[str]:
    """Display names of a workflow's jobs, `${{ matrix.<axis> }}` expanded from inline axis lists and
    `include` entries. A line-based reader for this repository's 2-space YAML, not a YAML parser."""
    jobs: list[dict] = []
    in_jobs = in_strategy = False
    pending = ""  # a flow list continued on the next lines: `axis: [a, b,\n c]`
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if pending:
            pending += " " + line.strip()
            if "]" not in line:
                continue
            line, pending = pending, ""
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
        elif in_strategy and re.match(r"^        [\w-]+:\s*\[[^\]]*$", line):
            pending = line
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
    """{"ctest": [...], "doctest": {binary: [cases]}, "errors": [...]}. A binary that cannot list its
    cases is an error of the inventory, and its cases are unknown rather than absent."""
    tests = ctest_tests(build_dir, config, ctest)
    doctest: dict[str, list[str]] = {}
    errors: list[str] = []
    for t in tests:
        if is_doctest_entry(t) != "main":
            continue
        try:
            doctest[t["name"]] = doctest_cases(doctest_binary_command(t), test_property(t, "WORKING_DIRECTORY"))
        except (ScorecardError, subprocess.SubprocessError, OSError) as e:
            errors.append(f"{t['name']}: {e}")
    return {"ctest": sorted(t["name"] for t in tests), "doctest": doctest, "errors": errors}


def go_source_tests(services: Path) -> dict[str, dict[str, str]]:
    """{package: {test name: build constraint or ""}} from the `func TestX(` lines of every `*_test.go`
    under `services` (a source scan, so the PR tier needs no Go toolchain)."""
    found: dict[str, dict[str, str]] = {}
    for f in sorted(services.rglob("*_test.go")):
        text = f.read_text(encoding="utf-8", errors="replace")
        constraint = re.search(r"^//go:build (.+)$", text, re.M)
        pkg = f.parent.relative_to(services).as_posix()
        for m in re.finditer(r"^func ((?:Test|Fuzz|Benchmark|Example)\w*)\(", text, re.M):
            found.setdefault(pkg, {})[m[1]] = constraint[1].strip() if constraint else ""
    return found


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
def _same_ref(a: dict, b: dict) -> bool:
    kind = ref_kind(a)
    return kind is not None and kind == ref_kind(b) and all(a.get(k) == b.get(k) for k in (kind, "case", "test"))


def _check_ref(where: str, ref, entry: dict, data: dict, ctx: dict, errors: list[str]) -> None:
    kind = ref_kind(ref) if isinstance(ref, dict) else None
    if kind is None:
        errors.append(f"{where}: a test reference needs exactly one of {', '.join(sorted(REF_KINDS))}")
        return
    label = ref_label(ref)
    allowed = {kind} | REF_KINDS[kind] | REF_OPTIONAL
    for key in sorted(set(ref) - allowed):
        errors.append(f"{where}: unknown field '{key}' in {label}")
    for key in [kind, *sorted(REF_KINDS[kind])]:
        if not isinstance(ref.get(key), str) or not ref[key].strip():
            errors.append(f"{where}: {label} needs a non-empty string '{key}'")
            return
    for key in ("ctest", "case"):
        if isinstance(ref.get(key), str) and not ref[key].replace("*", "").strip():
            errors.append(f"{where}: {label}: a name of wildcards only matches anything")
    if PINNED_NAME.search(ref.get("case", "") + " " + ref.get("ctest", "")) and not ctx.get("pin"):
        errors.append(f"{where}: {label} pins a known divergence (it passes while its clause fails), so it can only "
                      f"be a gap's pinned_by")
    entry_plats = entry.get("platforms") or []
    if "any" in entry_plats and kind in TEST_KINDS:
        errors.append(f"{where}: {label}: entries on 'any' platform can cite only evidence and ci_job references")
    plats = ref.get("platforms")
    if plats is not None and (not isinstance(plats, list) or not plats or not set(plats) <= set(entry_plats)
                              or "any" in plats):
        errors.append(f"{where}: {label}: 'platforms' must be a non-empty subset of the entry's platforms")
    runs = data.get("runs") if isinstance(data.get("runs"), dict) else {}
    if "run" in ref:
        run = runs.get(ref["run"])
        if not isinstance(run, dict):
            errors.append(f"{where}: {label} names unknown run '{ref['run']}'")
        elif run.get("os") not in ref_platforms(entry, ref):
            errors.append(f"{where}: {label}: run '{ref['run']}' is not on the reference's platforms")
    if "tags" in ref and (kind != "go" or not isinstance(ref["tags"], str)):
        errors.append(f"{where}: {label}: 'tags' (a string) applies to go references only")
    if "note" in ref and not isinstance(ref["note"], str):
        errors.append(f"{where}: {label}: 'note' must be a string")
    if kind == "gate":
        gate = (data.get("gates") or {}).get(ref["gate"]) if isinstance(data.get("gates"), dict) else None
        if not isinstance(gate, dict):
            errors.append(f"{where}: {label} is not declared in 'gates'")
        else:
            gate_oses = {runs.get(r, {}).get("os") for r in gate.get("runs") or [] if isinstance(r, str)}
            for p in sorted(set(ref_platforms(entry, ref)) - gate_oses - {"any"}):
                errors.append(f"{where}: {label} never runs on {p}")
    if kind == "evidence":
        rel = Path(ref["evidence"])
        if rel.is_absolute() or ".." in rel.parts or ref["evidence"].startswith(("/", "\\")):
            errors.append(f"{where}: {label}: evidence paths are relative to the repository root")
        elif ctx.get("repo") is not None and not (ctx["repo"] / rel).is_file():
            errors.append(f"{where}: {label} does not exist; a record still to come is a gap, not evidence")
    if kind == "ci_job" and ctx.get("ci_jobs") is not None and ref["ci_job"] not in ctx["ci_jobs"]:
        errors.append(f"{where}: ci_job '{ref['ci_job']}' is not a job of .github/workflows/ci.yml")
    go = ctx.get("go_sources")
    if kind == "go" and go is not None:
        constraint = go.get(ref["go"], {}).get(ref["test"])
        if constraint is None:
            errors.append(f"{where}: {label}: no `func {ref['test']}(` in services/{ref['go']}/*_test.go")
        elif "integration" in constraint and ref.get("tags") != "integration":
            errors.append(f"{where}: {label} is built only with -tags integration: add \"tags\": \"integration\"")


def _check_top(name: str, data: dict, errors: list[str]) -> list[int]:
    """Top-level keys, runs and gates; returns the covered phases."""
    for key in sorted(set(data) - TOP_KEYS):
        errors.append(f"{name}:1: unknown top-level key '{key}'")
    if not (is_int(data.get("version")) and data["version"] == 1):
        errors.append(f"{name}:1: 'version' must be the integer 1")
    if not (is_int(data.get("plan_rev")) and data["plan_rev"] >= 1):
        errors.append(f"{name}:1: 'plan_rev' must be a positive integer")
    for key in ("criteria", "exit"):
        items = data.get(key, [])
        if not isinstance(items, list):
            errors.append(f"{name}:1: '{key}' must be a list of entries")
            continue
        for k, item in enumerate(items):
            if not isinstance(item, dict):
                errors.append(f"{name}:1: item {k} of '{key}' is not an object")
    covers = data.get("covers", [])
    if not isinstance(covers, list) or not all(is_int(p) and 0 <= p <= 5 for p in covers) or 0 not in covers:
        errors.append(f"{name}:1: 'covers' must list phases 0-5, and always 0")
        covers = [p for p in covers if is_int(p)] if isinstance(covers, list) else []
    runs = data.get("runs", {})
    if not isinstance(runs, dict):
        errors.append(f"{name}:1: 'runs' must be an object")
        runs = {}
    for run_name, run in runs.items():
        if not isinstance(run, dict):
            errors.append(f"{name}:1: run '{run_name}' must be an object")
            continue
        for key in sorted(set(run) - RUN_KEYS):
            errors.append(f"{name}:1: run '{run_name}': unknown field '{key}'")
        if run.get("os") not in OSES or not isinstance(run.get("default"), bool):
            errors.append(f"{name}:1: run '{run_name}' needs 'os' (linux or windows) and a boolean 'default'")
        if not isinstance(run.get("description", ""), str):
            errors.append(f"{name}:1: run '{run_name}': 'description' must be a string")
        if not isinstance(run.get("nightly", True), bool):
            errors.append(f"{name}:1: run '{run_name}': 'nightly' (false for runs the nightly does not produce) "
                          f"must be a boolean")
    gates = data.get("gates", {})
    if not isinstance(gates, dict):
        errors.append(f"{name}:1: 'gates' must be an object")
        gates = {}
    for gate_name, gate in gates.items():
        if not isinstance(gate, dict):
            errors.append(f"{name}:1: gate '{gate_name}' must be an object")
            continue
        for key in sorted(set(gate) - GATE_KEYS):
            errors.append(f"{name}:1: gate '{gate_name}': unknown field '{key}'")
        gate_runs = gate.get("runs")
        if not isinstance(gate_runs, list) or not gate_runs or not all(isinstance(r, str) and r in runs for r in gate_runs):
            errors.append(f"{name}:1: gate '{gate_name}': 'runs' must name declared runs")
        if not (is_int(gate.get("min_seconds")) and gate["min_seconds"] > 0):
            errors.append(f"{name}:1: gate '{gate_name}': 'min_seconds' (the gate's shortest valid run) must be a "
                          f"positive integer")
        if not isinstance(gate.get("description", ""), str):
            errors.append(f"{name}:1: gate '{gate_name}': 'description' must be a string")
    return covers


def validate(data: dict, text: str, path: Path, plan: Plan | None = None, ci_jobs: list[str] | None = None,
             repo: Path | None = None, go_sources: dict | None = None) -> tuple[list[str], list[str]]:
    """(errors, notes) for a loaded registry; errors are `file:line: message` strings. `repo` (evidence
    files) and `go_sources` (Go test functions) are checked when given."""
    name = display(path)
    errors: list[str] = []
    notes: list[str] = []
    if not isinstance(data, dict):
        return [f"{name}:1: the registry must be a JSON object"], notes
    covers = _check_top(name, data, errors)
    _check_metrics(name, data, errors)
    known, exits = (plan.criteria, plan.exits) if plan else ({}, {})
    ctx = {"ci_jobs": ci_jobs, "repo": repo, "go_sources": go_sources}
    items = entries(data)
    exit_items = [x for x in data.get("exit") or [] if isinstance(x, dict)] if isinstance(data.get("exit"), list) else []
    seen: set[tuple] = set()
    for entry, line in zip(items, entry_lines(text, items)):
        where = f"{name}:{line}"
        ident, phase = entry.get("id"), entry.get("phase")
        if not isinstance(ident, str) or not ident:
            errors.append(f"{where}: an entry needs a string 'id'")
            continue
        for key in sorted(ENTRY_REQUIRED - set(entry)):
            errors.append(f"{where}: {ident}: missing field '{key}'")
        for key in sorted(set(entry) - ENTRY_REQUIRED - ENTRY_OPTIONAL):
            errors.append(f"{where}: {ident}: unknown field '{key}'")
        if not (is_int(phase) and 0 <= phase <= 5):
            errors.append(f"{where}: {ident}: 'phase' must be an integer 0-5")
            phase = None
        if (ident, phase) in seen:
            errors.append(f"{where}: {ident} is registered twice for phase {phase}")
        seen.add((ident, phase))
        if any(entry is x for x in exit_items):
            m = EXIT_ID.match(ident)
            if not m or int(m[1]) != phase:
                errors.append(f"{where}: exit item IDs are EXIT-<phase>.<slug> with the entry's phase")
        elif plan:
            crit = known.get(ident)
            if crit is None:
                errors.append(f"{where}: unknown criterion ID '{ident}' (not in the plan's criterion tables)")
            elif phase is not None and crit["phases"] and phase not in crit["phases"] and \
                    ident not in exits.get(phase, set()):
                errors.append(f"{where}: {ident} has no phase-{phase} scope in {crit['section']} "
                              f"(plan phases {sorted(crit['phases'])})")
            elif crit["class"] and str(entry.get("class")).replace(" ", "") != crit["class"]:
                errors.append(f"{where}: {ident}: class '{entry.get('class')}' differs from the plan's '{crit['class']}'")
        for key in ("title", "threshold", "source", "owner"):
            if key in entry and (not isinstance(entry[key], str) or not entry[key].strip()):
                errors.append(f"{where}: {ident}: '{key}' must be a non-empty string")
        if "notes" in entry and not isinstance(entry["notes"], str):
            errors.append(f"{where}: {ident}: 'notes' must be a string")
        if isinstance(entry.get("source"), str) and not SOURCE.match(entry["source"]):
            errors.append(f"{where}: {ident}: 'source' must be a plan anchor such as '04 §11.4'")
        if isinstance(entry.get("owner"), str) and not OWNER.match(entry["owner"]):
            errors.append(f"{where}: {ident}: 'owner' must name WPs (WP-0.13), 'User' or 'Director'")
        if "class" in entry and entry["class"] not in CLASSES:
            errors.append(f"{where}: {ident}: 'class' must be one of {', '.join(sorted(CLASSES))} (09 §5.6)")
        plats = entry.get("platforms")
        if "platforms" in entry and (not isinstance(plats, list) or not plats or not set(plats) <= PLATFORMS
                                     or ("any" in plats and len(plats) > 1) or len(set(plats)) != len(plats)):
            errors.append(f"{where}: {ident}: 'platforms' must be ['any'] or a non-empty list of linux and windows")
            continue
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
            _check_ref(where, ref, entry, data, ctx, errors)
        for gap in gaps:
            if not isinstance(gap, dict) or set(gap) - GAP_KEYS or not isinstance(gap.get("clause"), str) or \
                    not gap["clause"].strip() or gap.get("state") not in GAP_STATES or \
                    not isinstance(gap.get("owner"), str) or not OWNER.match(gap["owner"]):
                errors.append(f"{where}: {ident}: a gap needs a non-empty 'clause', 'state' (unmeasured or failing), "
                              f"an 'owner' (WPs, 'User' or 'Director') and optionally 'pinned_by'")
                continue
            if "pinned_by" in gap:
                _check_ref(where, gap["pinned_by"], entry, data, {**ctx, "pin": True}, errors)
                if isinstance(gap["pinned_by"], dict) and any(isinstance(r, dict) and _same_ref(r, gap["pinned_by"])
                                                              for r in tests):
                    errors.append(f"{where}: {ident}: {ref_label(gap['pinned_by'])} pins a failing clause, so it "
                                  f"cannot also be evidence")
    if plan:
        errors += plan.problems
        for phase in covers:
            if phase not in plan.exits:
                errors.append(f"{name}:1: 09 §2 has no parseable '**Exit.**' paragraph for covered phase {phase}")
            required = {i for i, c in known.items() if phase in c["phases"]} | exits.get(phase, set())
            registered = {e["id"] for e in entries(data) if isinstance(e.get("id"), str) and e.get("phase") == phase
                          and not any(e is x for x in exit_items)}
            for ident in sorted(required - registered):
                where = known[ident]["where"] if ident in known and phase in known[ident]["phases"] else \
                    plan.exit_where.get(phase, "docs/plan/09-roadmap-and-process.md:1")
                errors.append(f"{where}: phase-{phase} criterion {ident} is not registered in {name}")
            for ident in sorted(exits.get(phase, set()) - set(known)):
                notes.append(f"09 §2 names {ident} in the phase-{phase} exit, but no criterion table defines it")
    return errors, notes


def _check_metrics(name: str, data: dict, errors: list[str]) -> None:
    """perf_metrics: numbers perf.py reads from a doctest case's MESSAGE lines or a gate's output."""
    criteria = data.get("criteria") if isinstance(data.get("criteria"), list) else []
    ids = {e["id"] for e in criteria if isinstance(e, dict) and isinstance(e.get("id"), str)}
    metrics = data.get("perf_metrics", [])
    if not isinstance(metrics, list):
        errors.append(f"{name}:1: 'perf_metrics' must be a list")
        return
    seen = set()
    for m in metrics:
        ident = m.get("id") if isinstance(m, dict) else None
        if not isinstance(m, dict) or not isinstance(ident, str) or not re.fullmatch(r"[a-z0-9_.-]+", ident):
            errors.append(f"{name}:1: every perf metric needs an 'id' of [a-z0-9_.-]")
            continue
        where = f"{name}:1: perf metric '{ident}'"
        if ident in seen:
            errors.append(f"{where} is declared twice")
        seen.add(ident)
        for key in sorted(set(m) - METRIC_FIELDS):
            errors.append(f"{where}: unknown field '{key}'")
        bad = [k for k in ("criterion", "doctest", "case", "gate", "run", "note") if k in m and not isinstance(m[k], str)]
        if bad:
            errors.append(f"{where}: {', '.join(bad)} must be strings")
            continue
        if ("gate" in m) == ("doctest" in m) or ("doctest" in m) != ("case" in m):
            errors.append(f"{where}: needs either 'gate' or 'doctest' with 'case'")
        if "gate" in m and m["gate"] not in (data.get("gates") or {}):
            errors.append(f"{where}: gate '{m['gate']}' is not declared")
        if "run" in m and m["run"] not in (data.get("runs") or {}):
            errors.append(f"{where}: unknown run '{m['run']}'")
        if "criterion" in m and m["criterion"] not in ids:
            errors.append(f"{where}: criterion '{m['criterion']}' is not registered")
        if m.get("better") not in ("lower", "higher") or m.get("category") not in METRIC_CATEGORIES or \
                not isinstance(m.get("unit"), str):
            errors.append(f"{where}: needs 'unit', 'better' (lower or higher) and 'category' "
                          f"({', '.join(sorted(METRIC_CATEGORIES))})")
        try:
            if re.compile(str(m.get("pattern"))).groups != 1:
                errors.append(f"{where}: 'pattern' must have exactly one group (the number)")
        except re.error as e:
            errors.append(f"{where}: bad 'pattern': {e}")


def check_workflow(data: dict, path: Path, workflow: Path) -> list[str]:
    """Every declared run and gate is produced by the nightly workflow (it names each one)."""
    text = workflow.read_text(encoding="utf-8")
    errors = []
    runs = {k: v for k, v in (data.get("runs") or {}).items() if not isinstance(v, dict) or v.get("nightly", True)}
    for kind, names in (("run", runs), ("gate", data.get("gates") or {})):
        for n in names:
            if not re.search(r"(?<![\w-])" + re.escape(n) + r"(?![\w-])", text):
                errors.append(f"{display(path)}:1: {kind} '{n}' is never produced by {display(workflow)}")
    return errors


def check_inventory(data: dict, text: str, path: Path, inventory: dict) -> list[str]:
    """Errors for references that do not exist in an inventory of one OS. Only the sections the inventory
    has ("ctest", "doctest", "go") are checked, so a Go-only inventory says nothing about CTests."""
    name = display(path)
    inv_os = inventory.get("os") if isinstance(inventory, dict) else None
    if inv_os not in OSES:
        return [f"{name}:1: an inventory needs 'os' ({' or '.join(OSES)}), got {inv_os!r}"]
    ctests = inventory.get("ctest")
    doctest = inventory.get("doctest")
    go = inventory.get("go")
    errors = [f"{name}:1: {inv_os} inventory: {e}" for e in inventory.get("errors") or []]
    items = entries(data)
    for entry, line in zip(items, entry_lines(text, items)):
        refs = [r for r in entry.get("tests") or [] if isinstance(r, dict)]
        refs += [g["pinned_by"] for g in entry.get("gaps") or [] if isinstance(g, dict) and isinstance(g.get("pinned_by"), dict)]
        refs += [{"doctest": m["doctest"], "case": m["case"], **({"run": m["run"]} if "run" in m else {})}
                 for m in data.get("perf_metrics") or [] if isinstance(m, dict) and "doctest" in m and "case" in m
                 and m.get("criterion") == entry.get("id")]
        for ref in refs:
            kind = ref_kind(ref)
            if kind is None or inv_os not in ref_oses(entry, ref, data) or \
                    not all(isinstance(ref.get(k), str) for k in [kind, *REF_KINDS[kind]]):
                continue
            target = None
            if kind == "ctest" and ctests is not None and not any(matches(t, ref["ctest"]) for t in ctests):
                target = "CTest"
            elif kind == "doctest" and doctest is not None and (
                    ref["doctest"] not in doctest or not any(matches(c, ref["case"]) for c in doctest[ref["doctest"]])):
                target = "doctest case" if ref["doctest"] in doctest else "doctest binary"
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
    pre: list[str] = []
    if not ci.is_file():
        pre.append(f"{display(ci)}: not found, so ci_job references cannot be checked")
    services = Path(args.services)
    go_sources = go_source_tests(services) if services.is_dir() else None
    errors, notes = validate(data, text, args.scorecard, plan, workflow_jobs(ci) if ci.is_file() else None,
                             Path(args.repo), go_sources)
    errors = pre + errors
    nightly = Path(args.workflows) / "nightly.yml"
    if nightly.is_file():
        errors += check_workflow(data, args.scorecard, nightly)
    rev_file = plan_dir / "PLAN-REV"
    rev = re.match(r"\s*(\d+)", rev_file.read_text(encoding="utf-8-sig")) if rev_file.is_file() else None
    if rev is None:
        errors.append(f"{display(rev_file)}: missing or not a number (09 §5.10.2 D1)")
    elif is_int(data.get("plan_rev")) and data["plan_rev"] > int(rev[1]):
        errors.append(f"{display(args.scorecard)}:1: plan_rev {data['plan_rev']} is above PLAN-REV {rev[1]}")
    inventories = []
    for p in args.inventory:
        try:
            inventories.append(load_jsonc(Path(p))[0])
        except ScorecardError as e:
            errors.append(str(e))
    if args.build_dir:
        try:
            inventories.append({"os": host_os(), **build_inventory(Path(args.build_dir), args.config or None, args.ctest)})
        except (ScorecardError, subprocess.SubprocessError, OSError, ValueError) as e:
            errors.append(f"{args.build_dir}: cannot list the build's tests: {e}")
    checked = []
    for inv in inventories:
        errors += check_inventory(data, text, args.scorecard, inv)
        if isinstance(inv, dict):
            checked.append(f"{inv.get('os')} ({len(inv.get('ctest') or [])} CTests, "
                           f"{sum(len(v) for v in (inv.get('doctest') or {}).values())} doctest cases"
                           + (f", {sum(len(v) for v in inv['go'].values())} Go tests" if inv.get("go") is not None else "")
                           + ")")
    for note in notes:
        print(f"note: {note}")
    for error in errors:
        print(error)
    crit = [e for e in data.get("criteria") or [] if isinstance(e, dict)] if isinstance(data, dict) else []
    tally = {s: sum(1 for e in crit if e.get("status") == s) for s in sorted(STATUSES)}
    print(f"scorecard: {len(crit)} criteria ({', '.join(f'{v} {k}' for k, v in tally.items())}), "
          f"{len(entries(data)) - len(crit)} exit items, covers phases {data.get('covers') if isinstance(data, dict) else []}; "
          f"Go tests checked against {'the sources' if go_sources is not None else 'nothing'}; "
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
    check.add_argument("--repo", default=str(ROOT), help="root that evidence paths are relative to")
    check.add_argument("--services", default=str(ROOT / "services"), help="Go module whose tests go refs name")
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
