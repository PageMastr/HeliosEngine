#!/usr/bin/env python3
"""tools/status/snapshot: the mechanical part of the 09 §8.1 status (§5.10.2 D6 and D7; WP-0.3).

  python3 tools/status/snapshot.py [check]              exit 1 when §8.1 or a module README is stale
  python3 tools/status/snapshot.py inventory [--write]  the "Tree inventory (D6 check)" row from the tree
  python3 tools/status/snapshot.py report [--inventory FILE]...
                                   modules, test counts, CI jobs, WPs in flight and rework WPs (Markdown)

`check` is the D6/D7 check that tools/status/check_status.cmake used to implement (that script now
delegates here). It fails when a module directory under engine/, apps/, tools/, services/cmd/,
services/internal/ or services/pkg/ is not named in 09 §8.1 or has no WP in §8.1's "Tree inventory
(D6 check)" row, or when a module README in those directories (or services/README.md) has no
`Plan-Rev: <n>` line or records a revision above docs/plan/PLAN-REV. Directories without a README must
still be named but need no Plan-Rev.

`inventory` prints the row's module list: every module in the tree with the WP the row gives it ("?"
for a new module), plus modules the row names that are not in the tree (work in progress elsewhere).
`--write` puts it into 09 §8.1. `report` adds what D6 asks the Director to refresh: test counts per
toolchain (from `tools/scorecard/scorecard.py inventory` files), CI jobs, WP branches known to the local
git repository and the rework WPs of 09 §2. Standard library only, no network.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "scorecard"))
import scorecard  # noqa: E402  (workflow_jobs, load_jsonc)

PARENTS = ("engine", "apps", "tools", "services/cmd", "services/internal", "services/pkg")
PLAN = Path("docs/plan/09-roadmap-and-process.md")
ROW = "| Tree inventory (D6 check) |"


class StatusError(Exception):
    pass


def status_section(root: Path) -> tuple[str, int, int]:
    """(09 text, start, end) of §8.1."""
    path = root / PLAN
    if not path.is_file():
        raise StatusError(f"{PLAN.as_posix()} not found")
    with open(path, encoding="utf-8", newline="") as f:  # keep CRLF checkouts byte-exact for --write
        text = f.read()
    begin, end = text.find("### 8.1 "), text.find("### 8.2 ")
    if begin < 0 or end < begin:
        raise StatusError("cannot find 09 §8.1 (### 8.1 … ### 8.2)")
    return text, begin, end


def plan_rev(root: Path) -> int:
    path = root / "docs" / "plan" / "PLAN-REV"
    if not path.is_file():
        raise StatusError("docs/plan/PLAN-REV is missing (09 §5.10.2 D1)")
    m = re.match(r"\s*(\d+)", path.read_text(encoding="utf-8-sig", errors="replace"))
    if not m:
        raise StatusError("docs/plan/PLAN-REV does not start with a number")
    return int(m[1])


def modules(root: Path) -> list[str]:
    """Module directories: the children of PARENTS, skipping hidden ones and __pycache__."""
    found = []
    for parent in PARENTS:
        base = root / parent
        if base.is_dir():
            found += [f"{parent}/{p.name}" for p in base.iterdir() if p.is_dir() and not p.name.startswith(".")
                      and p.name != "__pycache__"]
    return sorted(found)


def named(module: str, status: str) -> bool:
    """The path appears followed by a character that cannot continue a path segment."""
    return re.search(re.escape(module) + r"(?:[^A-Za-z0-9_./-]|$)", status) is not None


def readme_rev(root: Path, module: str) -> tuple[bool, int | None]:
    """(has a README, its Plan-Rev or None). A README that is not UTF-8 counts as having no Plan-Rev."""
    readme = root / module / "README.md"
    if not readme.is_file():
        return False, None
    try:
        text = readme.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError:
        return True, None
    m = re.search(r"^Plan-Rev: (\d+)", text, re.M)
    return True, int(m[1]) if m else None


def check(root: Path) -> tuple[list[str], list[str], int, int]:
    """(failures, notes, module count, PLAN-REV)."""
    text, begin, end = status_section(root)
    status = text[begin:end]
    rev = plan_rev(root)
    mods = modules(root)
    failures, notes, no_readme = [], [], []
    row = row_modules(status)
    if row_line(status) is None:
        failures.append(f"09 §8.1 has no '{ROW}' row (D6)")
    for m in mods:
        if not named(m, status):
            failures.append(f"not named in 09 §8.1 (D6): {m}")
        elif row_line(status) is not None and row.get(m, "?") == "?":
            failures.append(f"no WP for it in the tree inventory row (D6; `snapshot.py inventory --write`): {m}")
    for m in mods + ["services"]:
        has, n = readme_rev(root, m)
        if not has:
            if m != "services":
                no_readme.append(m)
        elif n is None:
            failures.append(f"README without a 'Plan-Rev: <n>' line (D7): {m}/README.md")
        elif n > rev:
            failures.append(f"Plan-Rev above the plan's revision: {m}/README.md (Plan-Rev {n} > PLAN-REV {rev})")
    if no_readme:
        notes.append("no README, so no Plan-Rev required: " + ", ".join(no_readme))
    absent = [m for m in row if m not in mods]
    if absent:
        notes.append("named in the tree inventory but not in this tree (in progress elsewhere?): " + ", ".join(absent))
    line = row_line(status)
    if line is not None and not failures and scorecard._cells(line)[-1] != inventory_cell(root):
        notes.append("the tree inventory row is not in the generated form; `snapshot.py inventory --write` rewrites it")
    return failures, notes, len(mods), rev


def row_line(status: str) -> str | None:
    """The row whose first cell is the tree inventory's label (a mention elsewhere does not count)."""
    return next((line for line in status.splitlines() if line.startswith(ROW) and line.rstrip().endswith("|")),
                None)


def row_modules(status: str) -> dict[str, str]:
    """{module: WP} from the row: "`a` (0.14), `b`, `c` (all 0.15)" gives b and c 0.15."""
    line = row_line(status)
    cells = scorecard._cells(line) if line is not None else None
    if not cells:
        return {}
    cell = cells[-1]
    found: dict[str, str] = {}
    pending: list[str] = []
    for m in re.finditer(r"`([^`]+)`|\(([^()]*)\)", cell):
        if m[1]:
            pending.append(m[1])
        else:
            wp = re.sub(r"^all\s+", "", m[2].strip())
            found.update((p, wp) for p in pending)
            pending = []
    found.update((p, "?") for p in pending)
    return found


def inventory_cell(root: Path) -> str:
    text, begin, end = status_section(root)
    wps = row_modules(text[begin:end])
    mods = sorted(set(modules(root)) | set(wps))
    groups = []
    tops = ["engine", "apps", "services", "tools"]
    tops += sorted({m.split("/", 1)[0] for m in mods} - set(tops))  # e.g. content/, kept after the modules
    for top in tops:
        items = [(m, wps.get(m, "?")) for m in mods if m.split("/", 1)[0] == top]
        parts, k = [], 0
        while k < len(items):
            run = k
            while run + 1 < len(items) and items[run + 1][1] == items[k][1]:
                run += 1
            if run - k >= 2:
                names = ", ".join(f"`{m}`" for m, _ in items[k:run + 1])
                parts.append(f"{names} (all {items[k][1]})")
            else:
                parts += [f"`{m}` ({wp})" for m, wp in items[k:run + 1]]
            k = run + 1
        if parts:
            groups.append(", ".join(parts))
    return "; ".join(groups)


def write_inventory(root: Path) -> bool:
    text, begin, end = status_section(root)
    line = row_line(text[begin:end])
    if line is None:
        raise StatusError(f"09 §8.1 has no '{ROW}' row")
    cells = scorecard._cells(line)
    new = "| " + " | ".join(c.replace("|", "\\|") for c in cells[:-1] + [inventory_cell(root)]) + " |"
    if new == line:
        return False
    start = text.index(line, begin)
    with open(root / PLAN, "w", encoding="utf-8", newline="") as f:
        f.write(text[:start] + new + text[start + len(line):])
    return True


def module_of(binary: str, mods: list[str]) -> str:
    """The module a doctest binary belongs to: `render_tests` and `pcg_gpu_tests` map by name prefix."""
    stem = binary.removesuffix("_tests")
    best = [m for m in mods if stem == m.rsplit("/", 1)[1] or stem.startswith(m.rsplit("/", 1)[1] + "_")]
    return best[0] if best else "(other)"


def report(root: Path, inventories: list[dict]) -> str:
    text, begin, end = status_section(root)
    mods = modules(root)
    wps = row_modules(text[begin:end])
    counts: dict[str, dict[str, int]] = {}
    labels = []
    for inv in inventories:
        label = inv.get("toolchain") or inv.get("os") or "?"
        labels.append(label)
        for binary, cases in (inv.get("doctest") or {}).items():
            counts.setdefault(module_of(binary, mods), {})[label] = \
                counts.get(module_of(binary, mods), {}).get(label, 0) + len(cases)
        for pkg, tests in (inv.get("go") or {}).items():
            mod = "services/" + "/".join(pkg.split("/")[:2]) if pkg.split("/")[0] in ("cmd", "internal", "pkg") else "services"
            counts.setdefault(mod, {})[label + " (go)"] = counts.get(mod, {}).get(label + " (go)", 0) + len(tests)
    out = [f"## Status snapshot (PLAN-REV {plan_rev(root)})", "", "| Module | WP | Plan-Rev | Tests |", "|---|---|---|---|"]
    for m in sorted((set(mods) | set(counts)) - {"(other)"}):
        has, rev = readme_rev(root, m)
        tests = ", ".join(f"{k} {v}" for k, v in sorted(counts.get(m, {}).items())) or "—"
        out.append(f"| `{m}` | {wps.get(m, '?')} | {rev if has else 'no README'} | {tests} |")
    if "(other)" in counts:  # binaries named after no module directory: counted, not dropped
        out.append("| (other) | — | — | " + ", ".join(f"{k} {v}" for k, v in sorted(counts["(other)"].items())) + " |")
    out += ["", "### CI jobs", ""]
    for wf in sorted((root / ".github" / "workflows").glob("*.yml")):
        out.append(f"- `{wf.name}`: " + ", ".join(scorecard.workflow_jobs(wf)))
    out += ["", "### WP branches in the local git refs (fetched; merged or not)", ""]
    try:
        refs = subprocess.run(["git", "-C", str(root), "for-each-ref", "--format=%(refname:short)",
                               "refs/heads", "refs/remotes"], capture_output=True, encoding="utf-8", timeout=60)
        branches = sorted({r for r in refs.stdout.split() if re.search(r"wp-\d+\.\d+", r)})
        out += [f"- `{b}`" for b in branches] or ["- none"]
    except (OSError, subprocess.SubprocessError):
        out.append("- git not available")
    out += ["", "### Rework WPs defined in 09 §2 (open or done: §8.1 says which)", ""]
    for m in re.finditer(r"^\| (WP-\d+\.\d+r) \| [A-Z]{2} \| ([^|]+)\|", text, re.M):
        out.append(f"- {m[1]}: {m[2].strip()}")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("check", help="D6/D7 check (default)")
    inv = sub.add_parser("inventory", help="print the tree inventory row")
    inv.add_argument("--write", action="store_true", help="update the row in 09 §8.1")
    rep = sub.add_parser("report", help="print the status snapshot")
    rep.add_argument("--inventory", action="append", default=[], help="scorecard.py inventory JSON")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if args.command == "inventory":
            if args.write:
                print("snapshot: 09 §8.1 tree inventory " + ("updated" if write_inventory(root) else "already current"))
            else:
                print(inventory_cell(root))
            return 0
        if args.command == "report":
            print(report(root, [scorecard.load_jsonc(Path(p))[0] for p in args.inventory]), end="")
            return 0
        failures, notes, count, rev = check(root)
    except (StatusError, scorecard.ScorecardError, OSError) as e:
        print(f"snapshot: error: {e}")
        return 2
    print(f"snapshot: {count} module directories, PLAN-REV {rev}")
    for note in notes:
        print(f"  note: {note}")
    for failure in failures:
        print(f"  FAIL: {failure}")
    if failures:
        print("snapshot: the status in 09 §8.1 or a module README is stale")
        return 1
    print("snapshot: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
