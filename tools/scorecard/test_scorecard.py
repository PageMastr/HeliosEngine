"""Seeded-fixture tests for the scorecard registry checker (tools/scorecard/scorecard.py)."""

import contextlib
import copy
import io
import json
import tempfile
import unittest
from pathlib import Path

import scorecard as sc

PLAN = {
    "01-vision-and-scope.md": """# 01
### 3.10 Platform

| ID | Criterion | Ph |
|---|---|---|
| AAA-PLT-1 | Every commit builds | 0 |
| AAA-PLT-2 | Servers run natively | 0, 2 |
| AAA-REN-5 | No jitter | 1 |
| AAA-SEC-8 | **(M)** External penetration test | 4 |

| Metric | Ph1 | Ph2 | Ph3 |
|---|---|---|---|
| AAA-SRV-8 Ledger tx/s per shard | — | 2k | 2k |
""",
    "02-engine-runtime.md": """# 02
### 8.2 Acceptance criteria (automated)

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RT-03 | Physics hash | AAA-PLT-4 | 0–1 |
| RT-12 | Frame | AAA-REN-1 | 1–4 (MIN and 30/45 fps: 3; 120 fps: Ph4 midpoint) |

### 8.3 Test strategy

| ID | Not a criterion table | Ph |
|---|---|---|
| RT-99 | ignored: outside §8.2 | 0 |
""",
    "03-rendering.md": """# 03
### 9.3 Acceptance criteria

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RC-1 | Goldens | AAA-REN-7 | 0 |
""",
    "04-networking-and-servers.md": """# 04
### 11.4 Acceptance criteria (CI or bot swarm)

| ID | Criterion |
|---|---|
| **NS-0.1** | Handshake in 1.5 RTT |
| **NS-0.2** | Loopback 100k pps/core |
| **NS-1.1** | 50 players |
""",
    "05-backend-services.md": """# 05
## 10. Acceptance criteria

| # | Criterion | Phase |
|---|---|---|
| A1 | Warm start | 0 |
| A3a | Cell kill | 1 |
""",
    "06-gameplay-framework.md": """# 06
### 12.2 Acceptance criteria

1. **Kernel.** Golden builds and the HXL corpus.
2. **Ability prediction.** Round trip.
4. **Flight.**
    - (a) *FBW (Ph1).* Allocation.
    - (b) *Command flight (Ph2).* Fleets.

### 12.3 Risks
""",
    "07-editor-and-tools.md": """# 07
### 5.2 Acceptance criteria

| ID | Criterion | Ph | Refs |
|---|---|---|---|
| ED-1 | Transactions round-trip | 0 | PLT-1 |
""",
    "08-client-and-launcher.md": """# 08
### 4.4 Acceptance criteria

| ID | Criterion | Class | Maps to | Ph |
|---|---|---|---|---|
| CL-5 | Outage recovery | N + H | STB-5 | 2 |
""",
    "09-roadmap-and-process.md": """# 09
### 2.1 Phase 0 — Foundations

**Exit.** AAA-PLT-1, PLT-2 (Ph0); RC-1; ED-1; NS-0.1–0.2; BE-A1; RT-03 (Ph0 scope); GP-1 (corpus
clauses); the 01 §5.4 Phase 0 demos; the WP-0.4 runner policy check green.

### 2.2 Phase 1 — First Light

**Exit.** Every criterion with Ph ≤ 1: AAA-REN-5; RT-01…03 (Ph1 scope); BE-A3a, A4; GP-2, 4a.
""",
}

CI_YML = """name: CI
on: [pull_request]
jobs:
  build:
    name: Build (${{ matrix.preset }})
    strategy:
      matrix:
        preset: [linux-gcc, linux-clang]
    steps:
      - uses: actions/checkout@v5
        with:
          fetch-depth: 1
  msbuild:
    name: MSBuild (${{ matrix.preset }})
    strategy:
      matrix:
        include:
          - preset: vs2026
            image: windows-latest
    steps:
      - run: echo hi
  plain:
    runs-on: ubuntu-24.04
  wrapped:
    name: Fuzz ${{ matrix.gate }}
    strategy:
      matrix:
        gate: [one,
               two]
"""


def entry(ident, **fields):
    base = {"id": ident, "phase": 0, "source": "04 §11.4", "owner": "WP-0.13", "title": ident, "class": "N",
            "platforms": ["linux", "windows"], "threshold": "as specified", "status": "measured",
            "tests": [{"ctest": "net_tests"}]}
    base.update({k.rstrip("_"): v for k, v in fields.items()})
    return base


GAP = [{"clause": "not built yet", "state": "unmeasured", "owner": "WP-0.18"}]
VALID = {
    "version": 1, "plan_rev": 6, "covers": [0],
    "runs": {"linux-gcc": {"os": "linux", "default": True}, "linux-asan": {"os": "linux", "default": False},
             "windows-vs2026": {"os": "windows", "default": True, "description": "MSBuild"}},
    "gates": {"net_bench_gate": {"runs": ["linux-gcc", "windows-vs2026"], "min_seconds": 600},
              "fuzz_linux": {"runs": ["linux-gcc"], "min_seconds": 3600, "description": "1 h"}},
    "criteria": [
        entry("AAA-PLT-1", source="01 §3.10", tests=[{"ci_job": "Build (linux-gcc)"}, {"ci_job": "MSBuild (vs2026)"}]),
        entry("AAA-PLT-2", source="01 §3.10", platforms=["windows"]),
        entry("RC-1", source="03 §9.3", tests=[{"ctest": "rendertest.vulkan.*", "platforms": ["linux"]}]),
        entry("ED-1", source="07 §5.2", status="unmeasured", tests=[], gaps=GAP),
        entry("NS-0.1", tests=[{"doctest": "net_tests", "case": "NS-0.1: handshake"}], notes="how it is read"),
        entry("NS-0.2", tests=[{"gate": "net_bench_gate"}, {"gate": "fuzz_linux", "platforms": ["linux"]}]),
        entry("BE-A1", source="05 §10", platforms=["windows"],
              tests=[{"go": "internal/integration", "test": "TestLogin", "tags": "integration"}]),
        entry("RT-03", source="02 §8.2", status="partial",
              tests=[{"doctest": "physics_tests", "case": "determinism: golden", "run": "linux-asan"}],
              gaps=[{"clause": "permuted", "state": "failing", "owner": "WP-0.9",
                     "pinned_by": {"doctest": "physics_tests", "case": "KNOWN DIVERGENCE*"}}]),
        entry("GP-1", source="06 §12.2", status="unmeasured", tests=[], gaps=GAP),
    ],
    "exit": [entry("EXIT-0.funding", class_="M", platforms=["any"], owner="User", source="09 §4.3.6",
                   tests=[{"evidence": "docs/evidence/funding-F0.md"}])],
}

INVENTORY = {
    "os": "linux",
    "ctest": ["net_tests", "net_tests_perf", "rendertest.vulkan.triangle", "server_tests"],
    "doctest": {"net_tests": ["NS-0.1: handshake", "other"],
                "physics_tests": ["determinism: golden", "KNOWN DIVERGENCE: permuted"]},
    "go": {"internal/integration": ["TestLogin"], "pkg/connecttoken": ["TestVectors"]},
}


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.plan_dir = self.root / "plan"
        self.plan_dir.mkdir()
        self.write_plan(PLAN)
        (self.plan_dir / "PLAN-REV").write_text("6\n", encoding="utf-8")
        self.ci = self.root / "ci.yml"
        self.ci.write_text(CI_YML, encoding="utf-8")
        (self.root / "docs" / "evidence").mkdir(parents=True)
        (self.root / "docs" / "evidence" / "funding-F0.md").write_text("Signed-off-by: User\n", encoding="utf-8")
        self.go_sources = None

    def write_plan(self, plan):
        for name, text in plan.items():
            (self.plan_dir / name).write_text(text, encoding="utf-8")
        self.plan = sc.plan_criteria(self.plan_dir)

    def run_check(self, data, inventories=(), notes=None):
        path = self.root / "scorecard.jsonc"
        text = "// fixture\n" + json.dumps(data, indent=2, ensure_ascii=False)
        path.write_text(text, encoding="utf-8")
        loaded, raw = sc.load_jsonc(path)
        errors, found_notes = sc.validate(loaded, raw, path, self.plan, sc.workflow_jobs(self.ci), self.root,
                                          self.go_sources)
        for inv in inventories:
            errors += sc.check_inventory(loaded, raw, path, inv)
        if notes is not None:
            notes.extend(found_notes)
        return errors

    def assertFinding(self, errors, fragment):
        self.assertTrue(any(fragment in e for e in errors), f"no finding with {fragment!r} in {errors}")

    def mutate(self, ident, **fields):
        data = copy.deepcopy(VALID)
        for e in data["criteria"] + data["exit"]:
            if e["id"] == ident:
                e.update({k.rstrip("_"): v for k, v in fields.items()})
        return data


class PlanTests(Fixture):
    def test_criterion_tables_and_phases(self):
        known, exits = self.plan.criteria, self.plan.exits
        self.assertEqual(known["AAA-PLT-2"]["phases"], {0, 2})
        self.assertEqual(known["AAA-SRV-8"]["phases"], {2, 3})
        self.assertEqual(known["RT-03"]["phases"], {0, 1})
        self.assertEqual(known["RT-12"]["phases"], {1, 2, 3, 4})
        self.assertEqual(known["NS-1.1"]["phases"], {1})
        self.assertEqual(known["BE-A3a"]["phases"], {1})
        self.assertEqual(known["CL-5"]["class"], "N+H")
        self.assertEqual(known["AAA-SEC-8"]["class"], "M")
        self.assertIsNone(known["AAA-PLT-1"]["class"])
        self.assertNotIn("RT-99", known)
        self.assertIn("GP-4a", known)
        self.assertIn("GP-4b", known)
        self.assertNotIn("GP-4", known)
        self.assertEqual(exits[0], {"AAA-PLT-1", "AAA-PLT-2", "RC-1", "ED-1", "NS-0.1", "NS-0.2", "BE-A1",
                                    "RT-03", "GP-1"})
        self.assertEqual(exits[1], {"AAA-REN-5", "RT-01", "RT-02", "RT-03", "BE-A3a", "BE-A4", "GP-2", "GP-4a"})
        self.assertEqual(self.plan.problems, [])
        self.assertTrue(known["NS-0.2"]["where"].endswith("04-networking-and-servers.md:7"), known["NS-0.2"]["where"])

    def test_parse_phases(self):
        self.assertEqual(sc.parse_phases("0"), {0})
        self.assertEqual(sc.parse_phases("1–5"), {1, 2, 3, 4, 5})
        self.assertEqual(sc.parse_phases("4 (1)"), {1, 4})
        self.assertEqual(sc.parse_phases("1 (Earth-size: 3)"), {1})
        self.assertEqual(sc.parse_phases("4 (tooling 3)"), {4})
        self.assertEqual(sc.parse_phases("2 (Luau), 3"), {2, 3})          # ED-19
        self.assertEqual(sc.parse_phases("2 (MSI) / 3 (AppImage)"), {2, 3})  # CL-24

    def test_exit_ranges_and_continuations(self):
        self.assertEqual(sc.exit_ids("BE-A3b, A5–A8"), {"BE-A3b", "BE-A5", "BE-A6", "BE-A7", "BE-A8"})
        self.assertEqual(sc.exit_ids("NS-0.1-0.3; RT-01…03"), {"NS-0.1", "NS-0.2", "NS-0.3", "RT-01", "RT-02", "RT-03"})
        self.assertEqual(sc.exit_ids("the WP-0.4 check; 5 demos"), set())

    def test_exit_label_variants_are_read(self):
        self.write_plan({"09-roadmap-and-process.md": PLAN["09-roadmap-and-process.md"] +
                         "\n### 2.3 Phase 2 — Alpha\n\n**Exit — the sandbox bar.** AAA-SRV-8.\n"})
        self.assertEqual(self.plan.exits[2], {"AAA-SRV-8"})

    def test_rows_that_do_not_parse_are_problems(self):
        plan = dict(PLAN)
        plan["02-engine-runtime.md"] = plan["02-engine-runtime.md"].replace(
            "| RT-12 |", "| RT-12a | Letter suffix | x | 1 |\n| RT-14 | Missing a cell | 1 |\n| RT-12 |")
        self.write_plan(plan)
        self.assertEqual(len(self.plan.problems), 2, self.plan.problems)
        errors = self.run_check(VALID)
        self.assertFinding(errors, "02-engine-runtime.md:7: 'RT-12a' looks like a criterion ID but does not parse")
        self.assertFinding(errors, "02-engine-runtime.md:8: RT-14 has 3 cells, its header 4")

    def test_fenced_headings_do_not_end_a_section(self):
        plan = dict(PLAN)
        plan["03-rendering.md"] = plan["03-rendering.md"].replace(
            "### 9.3 Acceptance criteria\n", "### 9.3 Acceptance criteria\n\n```\n# not a heading\n```\n")
        self.write_plan(plan)
        self.assertIn("RC-1", self.plan.criteria)

    def test_exit_ids_no_table_defines_are_notes(self):
        plan = dict(PLAN)
        plan["09-roadmap-and-process.md"] = plan["09-roadmap-and-process.md"].replace("ED-1;", "ED-1; RT-99;")
        self.write_plan(plan)
        notes = []
        self.run_check(VALID, notes=notes)
        self.assertTrue(any("RT-99" in n for n in notes), notes)


class RegistryTests(Fixture):
    def test_valid_registry_passes(self):
        self.assertEqual(self.run_check(VALID, [INVENTORY, {**INVENTORY, "os": "windows"}]), [])

    def test_unknown_criterion_id_fails(self):
        data = copy.deepcopy(VALID)
        data["criteria"].append(entry("NS-0.9"))
        self.assertFinding(self.run_check(data), "unknown criterion ID 'NS-0.9'")

    def test_missing_field_fails_with_its_line(self):
        data = copy.deepcopy(VALID)
        del data["criteria"][4]["threshold"]
        errors = self.run_check(data)
        self.assertFinding(errors, "NS-0.1: missing field 'threshold'")
        path = self.root / "scorecard.jsonc"
        line = next(i for i, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1)
                    if '"id": "NS-0.1"' in text)
        self.assertFinding(errors, f"scorecard.jsonc:{line}: NS-0.1")

    def test_unknown_entry_field_fails(self):
        self.assertFinding(self.run_check(self.mutate("NS-0.1", platfroms=["linux"])), "NS-0.1: unknown field 'platfroms'")

    def test_unregistered_phase0_criterion_points_at_the_plan(self):
        data = copy.deepcopy(VALID)
        data["criteria"] = [e for e in data["criteria"] if e["id"] != "NS-0.2"]
        self.assertFinding(self.run_check(data), "04-networking-and-servers.md:7: phase-0 criterion NS-0.2 is not registered")

    def test_exit_only_criterion_must_be_registered(self):
        data = copy.deepcopy(VALID)
        data["criteria"] = [e for e in data["criteria"] if e["id"] != "GP-1"]
        self.assertFinding(self.run_check(data), "09-roadmap-and-process.md:4: phase-0 criterion GP-1")

    def test_covered_phase_needs_an_exit_paragraph(self):
        data = copy.deepcopy(VALID)
        data["covers"] = [0, 3]
        self.assertFinding(self.run_check(data), "no parseable '**Exit.**' paragraph for covered phase 3")

    def test_phase_outside_the_plan_fails(self):
        self.assertFinding(self.run_check(self.mutate("RC-1", phase=3)), "RC-1 has no phase-3 scope")

    def test_status_must_match_tests_and_gaps(self):
        self.assertFinding(self.run_check(self.mutate("ED-1", status="measured")), "so it is 'unmeasured'")
        self.assertFinding(self.run_check(self.mutate("RT-03", gaps=[])), "so it is 'measured'")

    def test_entry_without_tests_or_gaps_fails(self):
        self.assertFinding(self.run_check(self.mutate("ED-1", gaps=[])), "without tests must list its gaps")

    def test_unknown_reference_field_fails(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "case": "x", "platfroms": ["linux"]}])
        self.assertFinding(self.run_check(data), "unknown field 'platfroms'")

    def test_reference_needs_one_kind(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "ctest": "net_tests", "case": "x"}])
        self.assertFinding(self.run_check(data), "needs exactly one of")

    def test_reference_platforms_are_a_subset_of_the_entry(self):
        data = self.mutate("AAA-PLT-2", tests=[{"ctest": "net_tests", "platforms": ["linux"]}])
        self.assertFinding(self.run_check(data), "must be a non-empty subset of the entry's platforms")

    def test_entry_platforms_are_checked(self):
        for plats in (["mac"], ["any", "linux"], [], ["linux", "linux"], "linux"):
            self.assertFinding(self.run_check(self.mutate("NS-0.1", platforms=plats)), "NS-0.1: 'platforms' must be")

    def test_runs_are_checked(self):
        for run in ({"default": True}, {"os": "linux"}, {"os": "Linux", "default": True},
                    {"os": "linux", "default": True, "nightlyy": False}, "linux"):
            data = copy.deepcopy(VALID)
            data["runs"]["linux-x"] = run
            self.assertFinding(self.run_check(data), "run 'linux-x'")

    def test_unknown_gate_field_fails(self):
        data = copy.deepcopy(VALID)
        data["gates"]["fuzz_linux"] = {"runs": ["linux-gcc"], "min_secs": 3600}
        errors = self.run_check(data)
        self.assertFinding(errors, "gate 'fuzz_linux': unknown field 'min_secs'")
        self.assertFinding(errors, "gate 'fuzz_linux': 'min_seconds'")

    def test_gate_needs_a_positive_min_seconds(self):
        for value in (None, 0, -5, True, "3600", 3600.0):
            data = copy.deepcopy(VALID)
            data["gates"]["fuzz_linux"] = {"runs": ["linux-gcc"]} if value is None else \
                {"runs": ["linux-gcc"], "min_seconds": value}
            self.assertFinding(self.run_check(data), "gate 'fuzz_linux': 'min_seconds'")

    def test_gate_runs_must_be_declared(self):
        for runs in (["linux-tsan"], [], "linux-gcc"):
            data = copy.deepcopy(VALID)
            data["gates"]["fuzz_linux"] = {"runs": runs, "min_seconds": 1}
            self.assertFinding(self.run_check(data), "gate 'fuzz_linux': 'runs' must name declared runs")

    def test_gates_must_be_declared_and_run_on_the_platform(self):
        self.assertFinding(self.run_check(self.mutate("NS-0.2", tests=[{"gate": "nope"}])), "not declared")
        self.assertFinding(self.run_check(self.mutate("NS-0.2", tests=[{"gate": "fuzz_linux"}])),
                           "never runs on windows")

    def test_run_must_exist_and_match_platform(self):
        data = self.mutate("NS-0.1", tests=[{"ctest": "net_tests", "run": "linux-tsan"}])
        self.assertFinding(self.run_check(data), "unknown run 'linux-tsan'")
        data = self.mutate("AAA-PLT-2", tests=[{"ctest": "net_tests", "run": "linux-gcc"}])
        self.assertFinding(self.run_check(data), "not on the reference's platforms")

    def test_top_level_types(self):
        for key, value, fragment in (("version", 2, "'version'"), ("version", True, "'version'"),
                                     ("plan_rev", True, "'plan_rev'"), ("plan_rev", 0, "'plan_rev'"),
                                     ("covers", [7], "'covers'"), ("covers", [], "'covers'"),
                                     ("covers", [1], "'covers'"), ("runs", [], "'runs' must be an object"),
                                     ("criteria", {}, "'criteria' must be a list"), ("extra", 1, "unknown top-level key")):
            data = copy.deepcopy(VALID)
            data[key] = value
            self.assertFinding(self.run_check(data), fragment)

    def test_items_must_be_objects_with_string_ids(self):
        data = copy.deepcopy(VALID)
        data["criteria"].append("NS-0.3")
        data["exit"].append(7)
        errors = self.run_check(data)
        self.assertFinding(errors, "item 9 of 'criteria' is not an object")
        self.assertFinding(errors, "item 1 of 'exit' is not an object")
        self.assertFinding(self.run_check(self.mutate("NS-0.1", id_=["NS-0.1"])), "needs a string 'id'")

    def test_phase_is_an_integer_not_a_bool(self):
        self.assertFinding(self.run_check(self.mutate("NS-0.1", phase=False)), "'phase' must be an integer 0-5")

    def test_source_owner_class_and_notes_are_checked(self):
        self.assertFinding(self.run_check(self.mutate("NS-0.1", source="04-11.4")), "'source' must be a plan anchor")
        self.assertFinding(self.run_check(self.mutate("NS-0.1", owner="nobody")), "'owner' must name WPs")
        self.assertFinding(self.run_check(self.mutate("NS-0.1", class_="Q")), "'class' must be one of")
        self.assertFinding(self.run_check(self.mutate("NS-0.1", notes=["x"])), "'notes' must be a string")

    def test_class_must_match_the_plan(self):
        data = copy.deepcopy(VALID)
        data["criteria"].append(entry("CL-5", phase=2, source="08 §4.4"))
        self.assertFinding(self.run_check(data), "class 'N' differs from the plan's 'N+H'")
        data["criteria"][-1] = entry("AAA-SEC-8", phase=4, source="01 §3.8")
        self.assertFinding(self.run_check(data), "class 'N' differs from the plan's 'M'")

    def test_ci_job_must_exist(self):
        data = self.mutate("AAA-PLT-1", tests=[{"ci_job": "Build (linux-msvc)"}])
        self.assertFinding(self.run_check(data), "ci_job 'Build (linux-msvc)' is not a job")

    def test_duplicate_entry_fails(self):
        data = copy.deepcopy(VALID)
        data["criteria"].append(copy.deepcopy(data["criteria"][4]))
        self.assertFinding(self.run_check(data), "NS-0.1 is registered twice for phase 0")

    def test_exit_ids_carry_their_phase(self):
        data = copy.deepcopy(VALID)
        data["exit"][0]["id"] = "EXIT-1.funding"
        self.assertFinding(self.run_check(data), "EXIT-<phase>.<slug>")

    def test_gaps_are_checked(self):
        for gap in ({"clause": "", "state": "unmeasured", "owner": "WP-0.18"},
                    {"clause": "x", "state": "open", "owner": "WP-0.18"},
                    {"clause": "x", "state": "unmeasured", "owner": "nobody"},
                    {"clause": "x", "state": "unmeasured", "owner": "WP-0.18", "why": "?"}, "x"):
            self.assertFinding(self.run_check(self.mutate("ED-1", gaps=[gap])), "a gap needs a non-empty 'clause'")

    def test_pinned_by_is_checked(self):
        pin = {"clause": "permuted", "state": "failing", "owner": "WP-0.9", "pinned_by": {"doctest": "physics_tests"}}
        self.assertFinding(self.run_check(self.mutate("RT-03", gaps=[pin])), "needs a non-empty string 'case'")
        pin["pinned_by"] = {"ci_job": "Build (nope)"}
        self.assertFinding(self.run_check(self.mutate("RT-03", gaps=[pin])), "ci_job 'Build (nope)' is not a job")
        pin["pinned_by"] = {"doctest": "physics_tests", "case": "determinism: golden", "run": "linux-asan"}
        self.assertFinding(self.run_check(self.mutate("RT-03", gaps=[pin])), "cannot also be evidence")

    def test_known_divergence_is_never_evidence(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "physics_tests", "case": "KNOWN DIVERGENCE: permuted"}])
        self.assertFinding(self.run_check(data), "pins a known divergence")

    def test_tags_apply_to_go_references_only(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "case": "x", "tags": "integration"}])
        self.assertFinding(self.run_check(data), "'tags' (a string) applies to go references only")

    def test_evidence_paths_are_relative_and_must_exist(self):
        for path, fragment in (("/etc/passwd", "relative to the repository root"),
                               ("../outside.md", "relative to the repository root"),
                               ("docs/evidence/milestone-M0.txt", "does not exist")):
            data = self.mutate("EXIT-0.funding", tests=[{"evidence": path}])
            self.assertFinding(self.run_check(data), fragment)

    def test_any_entries_cite_records_only(self):
        data = self.mutate("EXIT-0.funding", tests=[{"ctest": "net_tests"}])
        self.assertFinding(self.run_check(data), "entries on 'any' platform can cite only evidence and ci_job")

    def test_wildcard_only_names_fail(self):
        self.assertFinding(self.run_check(self.mutate("RC-1", tests=[{"ctest": "*"}])), "wildcards only")
        self.assertFinding(self.run_check(self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "case": "* "}])),
                           "wildcards only")

    def test_go_references_are_checked_against_the_sources(self):
        pkg = self.root / "services" / "internal" / "integration"
        pkg.mkdir(parents=True)
        (pkg / "it_test.go").write_text("//go:build integration\n\npackage integration\n\n"
                                        "func TestLogin(t *testing.T) {}\n", encoding="utf-8")
        self.go_sources = sc.go_source_tests(self.root / "services")
        self.assertEqual(self.go_sources, {"internal/integration": {"TestLogin": "integration"}})
        self.assertEqual(self.run_check(VALID), [])
        data = self.mutate("BE-A1", tests=[{"go": "internal/integration", "test": "TestLogin"}])
        self.assertFinding(self.run_check(data), "built only with -tags integration")
        data = self.mutate("BE-A1", tests=[{"go": "internal/integration", "test": "TestLogout", "tags": "integration"}])
        self.assertFinding(self.run_check(data), "no `func TestLogout(`")


class CommandLineTests(Fixture):
    def main(self, data, *extra):
        path = self.root / "scorecard.jsonc"
        path.write_text(json.dumps(data), encoding="utf-8")
        workflows = self.root / "workflows"
        workflows.mkdir(exist_ok=True)
        if self.ci.is_file():
            (workflows / "ci.yml").write_text(self.ci.read_text(encoding="utf-8"), encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = sc.main(["check", "--scorecard", str(path), "--plan", str(self.plan_dir), "--workflows",
                          str(workflows), "--repo", str(self.root), "--services", str(self.root / "none"), *extra])
        return rc, out.getvalue()

    def test_check_passes_and_fails(self):
        self.assertEqual(self.main(VALID)[0], 0)
        rc, out = self.main(self.mutate("NS-0.1", platfroms=[]))
        self.assertEqual(rc, 1)
        self.assertIn("unknown field 'platfroms'", out)

    def test_plan_rev_above_plan_rev_fails(self):
        (self.plan_dir / "PLAN-REV").write_text("5\n", encoding="utf-8")
        rc, out = self.main(VALID)
        self.assertEqual(rc, 1)
        self.assertIn("plan_rev 6 is above PLAN-REV 5", out)
        (self.plan_dir / "PLAN-REV").write_text("six\n", encoding="utf-8")
        rc, out = self.main(VALID)
        self.assertEqual(rc, 1)
        self.assertIn("PLAN-REV: missing or not a number", out)

    def test_missing_ci_yml_is_a_finding(self):
        self.ci.unlink()
        rc, out = self.main(VALID)
        self.assertEqual(rc, 1)
        self.assertIn("ci.yml: not found", out)

    def test_bad_inventory_files_are_findings_not_crashes(self):
        bad = self.root / "inv.json"
        bad.write_text("{", encoding="utf-8")
        rc, out = self.main(VALID, "--inventory", str(bad))
        self.assertEqual(rc, 1)
        self.assertIn("inv.json", out)
        self.assertIn("scorecard: 9 criteria", out)  # the registry findings still print


class PerfMetricTests(Fixture):
    METRIC = {"id": "net.pps", "criterion": "NS-0.1", "doctest": "net_tests", "case": "NS-0.1: handshake",
              "pattern": r"-> (\d+) pps", "unit": "pps", "better": "higher", "category": "runtime"}

    def with_metric(self, **fields):
        data = copy.deepcopy(VALID)
        data["perf_metrics"] = [dict(self.METRIC, **fields)]
        return data

    def test_valid_metric_passes(self):
        self.assertEqual(self.run_check(self.with_metric(), [INVENTORY]), [])

    def test_metric_fields_are_checked(self):
        self.assertFinding(self.run_check(self.with_metric(pattern=r"\d+ pps")), "exactly one group")
        self.assertFinding(self.run_check(self.with_metric(pattern="(")), "bad 'pattern'")
        self.assertFinding(self.run_check(self.with_metric(gate="net_bench_gate")), "either 'gate' or 'doctest'")
        self.assertFinding(self.run_check(self.with_metric(category="speed")), "'category'")
        self.assertFinding(self.run_check(self.with_metric(criterion="NS-0.9")), "'NS-0.9' is not registered")
        self.assertFinding(self.run_check(self.with_metric(budgett="5 %")), "unknown field 'budgett'")
        data = self.with_metric()
        del data["perf_metrics"][0]["case"]
        data["perf_metrics"][0].pop("doctest")
        data["perf_metrics"][0]["gate"] = "nope"
        self.assertFinding(self.run_check(data), "gate 'nope' is not declared")

    def test_metric_shapes_are_checked(self):
        self.assertFinding(self.run_check(self.with_metric(gate=["net_bench_gate"])), "gate must be strings")
        self.assertFinding(self.run_check(self.with_metric(id="Net PPS")), "an 'id' of [a-z0-9_.-]")
        data = copy.deepcopy(VALID)
        data["perf_metrics"] = {"net.pps": self.METRIC}
        self.assertFinding(self.run_check(data), "'perf_metrics' must be a list")
        data = self.with_metric()
        data["perf_metrics"].append(dict(self.METRIC))
        self.assertFinding(self.run_check(data), "perf metric 'net.pps' is declared twice")

    def test_metric_case_must_exist(self):
        self.assertFinding(self.run_check(self.with_metric(case="NS-0.1: renamed"), [INVENTORY]),
                           "doctest case doctest net_tests / NS-0.1: renamed does not exist")

    def test_every_metric_source_is_checked_whether_or_not_it_names_a_criterion(self):
        # The review's reproduction: a metric without a criterion whose case was renamed.
        data = self.with_metric(case="NS-0.1: renamed")
        del data["perf_metrics"][0]["criterion"]
        errors = self.run_check(data, [INVENTORY])
        self.assertEqual(len(errors), 1, errors)
        self.assertFinding(errors, "perf metric 'net.pps': doctest case doctest net_tests / NS-0.1: renamed "
                                   "does not exist in the linux inventory")
        self.assertFinding(self.run_check(self.with_metric(doctest="gone_tests"), [INVENTORY]),
                           "perf metric 'net.pps': doctest binary")
        # A metric read from one run is checked only against inventories of that run's OS.
        windows = dict(INVENTORY, os="windows", doctest={"net_tests": []})
        metric_errors = (lambda data: [e for e in self.run_check(data, [windows]) if "perf metric" in e])
        self.assertEqual(metric_errors(self.with_metric(run="linux-gcc")), [])
        self.assertFinding(metric_errors(self.with_metric()), "perf metric 'net.pps': doctest case")

    def test_metric_cases_are_exact(self):
        self.assertFinding(self.run_check(self.with_metric(case="NS-0.1: *")), "take no '*'")

    def test_perf_accept_is_checked(self):
        good = {"metric": "net.pps", "night": "2026-10-03", "run": "linux-gcc", "reason": "new codec; accepted by the Director"}
        data = self.with_metric()
        data["perf_accept"] = [good]
        self.assertEqual(self.run_check(data), [])
        for bad, finding in (({"metric": "net.nope"}, "'metric' must name a declared perf metric"),
                             ({"night": "2026-13-01"}, "'night' must be the YYYY-MM-DD"),
                             ({"night": "2026-10-3"}, "'night' must be the YYYY-MM-DD"),
                             ({"run": "linux-x"}, "unknown run 'linux-x'"),
                             ({"reason": " "}, "needs a 'reason'"),
                             ({"because": "x"}, "unknown field 'because'")):
            data["perf_accept"] = [dict(good, **bad)]
            self.assertFinding(self.run_check(data), finding)
        data["perf_accept"] = {"net.pps": good}
        self.assertFinding(self.run_check(data), "'perf_accept' must be a list")
        data["perf_accept"] = ["net.pps"]
        self.assertFinding(self.run_check(data), "every perf_accept item must be an object")

    def test_nightly_must_produce_every_run_and_gate(self):
        workflow = self.root / "nightly.yml"
        workflow.write_text("jobs:\n  a:\n    steps:\n      - run: echo linux-gcc linux-asan windows-vs2026 "
                            "net_bench_gate\n", encoding="utf-8")
        errors = sc.check_workflow(VALID, self.root / "scorecard.jsonc", workflow)
        self.assertEqual(len(errors), 1)
        self.assertIn("gate 'fuzz_linux' is never produced", errors[0])


class InventoryTests(Fixture):
    def test_missing_ctest_fails(self):
        inv = dict(INVENTORY, ctest=["net_tests"])
        self.assertFinding(self.run_check(VALID, [inv]), "CTest ctest rendertest.vulkan.* does not exist")

    def test_missing_doctest_case_fails(self):
        inv = copy.deepcopy(INVENTORY)
        inv["doctest"]["net_tests"] = ["other"]
        self.assertFinding(self.run_check(VALID, [inv]), "doctest case doctest net_tests / NS-0.1: handshake")

    def test_missing_doctest_binary_fails(self):
        inv = copy.deepcopy(INVENTORY)
        del inv["doctest"]["physics_tests"]
        self.assertFinding(self.run_check(VALID, [inv]), "doctest binary doctest physics_tests")

    def test_missing_pinned_test_fails(self):
        inv = copy.deepcopy(INVENTORY)
        inv["doctest"]["physics_tests"] = ["determinism: golden"]
        self.assertFinding(self.run_check(VALID, [inv]), "KNOWN DIVERGENCE*")

    def test_missing_go_test_fails_only_with_a_go_inventory(self):
        inv = copy.deepcopy(INVENTORY)
        inv["os"] = "windows"
        inv["go"]["internal/integration"] = []
        self.assertFinding(self.run_check(VALID, [inv]), "Go test go internal/integration / TestLogin")
        del inv["go"]
        self.assertEqual(self.run_check(VALID, [inv]), [])

    def test_go_only_inventory_checks_go_only(self):
        go_only = {"os": "linux", "go": {"internal/integration": ["TestLogin"]}}
        self.assertEqual(self.run_check(VALID, [go_only]), [])
        go_only["go"] = {}
        self.assertEqual(self.run_check(VALID, [go_only]), [])  # BE-A1 is Windows-only
        go_only["os"] = "windows"
        self.assertFinding(self.run_check(VALID, [go_only]), "Go test go internal/integration / TestLogin")

    def test_references_are_checked_only_on_their_platforms(self):
        windows = dict(INVENTORY, os="windows", ctest=["net_tests"])  # no rendertest.vulkan.* on Windows
        self.assertEqual(self.run_check(VALID, [windows]), [])
        asan_only_on_linux = copy.deepcopy(windows)
        asan_only_on_linux["doctest"]["physics_tests"] = ["KNOWN DIVERGENCE: permuted"]
        self.assertEqual(self.run_check(VALID, [asan_only_on_linux]), [])

    def test_inventory_needs_a_known_os(self):
        for inv in ({"ctest": []}, {"os": "Linux", "ctest": []}):
            self.assertFinding(self.run_check(VALID, [inv]), "an inventory needs 'os'")

    def test_inventory_errors_are_findings(self):
        inv = dict(INVENTORY, errors=["pcg_tests: --list-test-cases exited 3"])
        self.assertFinding(self.run_check(VALID, [inv]), "linux inventory: pcg_tests: --list-test-cases exited 3")

    def test_a_binary_that_cannot_list_its_cases_does_not_stop_the_inventory(self):
        tests = [{"name": "a_tests", "command": ["a", "--test-case-exclude=perf:*"]},
                 {"name": "b_tests", "command": ["b", "--test-case-exclude=perf:*"]}]

        def cases(command, cwd=None):
            if command[0] == "b":
                raise sc.ScorecardError("b: --list-test-cases exited 3")
            return ["one"]

        saved = sc.ctest_tests, sc.doctest_cases
        sc.ctest_tests, sc.doctest_cases = (lambda *args: tests), cases
        try:
            inv = sc.build_inventory(Path("build"))
        finally:
            sc.ctest_tests, sc.doctest_cases = saved
        self.assertEqual(inv["doctest"], {"a_tests": ["one"]})
        self.assertEqual(inv["errors"], ["b_tests: b: --list-test-cases exited 3"])


class FormatTests(unittest.TestCase):
    def test_jsonc_comments_and_trailing_commas(self):
        text = '{\n  // c\n  "a": [1, 2,], /* x\n y */ "b": "//not a comment",\n}\n'
        self.assertEqual(json.loads(sc.strip_jsonc(text)), {"a": [1, 2], "b": "//not a comment"})
        self.assertEqual(sc.strip_jsonc(text).count("\n"), text.count("\n"))
        with self.assertRaises(json.JSONDecodeError):
            json.loads(sc.strip_jsonc("[,]"))

    def test_jsonc_syntax_error_reports_the_line_and_a_bom_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "bad.jsonc"
            path.write_text('{\n  "a": 1\n  "b": 2\n}\n', encoding="utf-8")
            with self.assertRaisesRegex(sc.ScorecardError, r"bad.jsonc:3: "):
                sc.load_jsonc(path)
            path.write_bytes(b"\xef\xbb\xbf{\"a\": 1}")
            self.assertEqual(sc.load_jsonc(path)[0], {"a": 1})

    def test_entry_lines_ignore_comments(self):
        text = '// "id": "X"\n{"criteria": [\n  {"id": "X"}\n]}\n'
        self.assertEqual(sc.entry_lines(text, [{"id": "X"}]), [3])

    def test_names_match_literally_except_star(self):
        self.assertTrue(sc.matches("net: [edge] handshake?", "net: [edge] handshake?"))
        self.assertFalse(sc.matches("abc", "a?c"))
        self.assertFalse(sc.matches("a[b]c", "a[bc]c"))
        self.assertTrue(sc.matches("rendertest.vulkan.mips", "rendertest.vulkan.*"))
        self.assertFalse(sc.matches("rendertestXvulkan.mips", "rendertest.vulkan.*"))

    def test_workflow_jobs_expand_matrices(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "ci.yml"
            path.write_text(CI_YML, encoding="utf-8")
            self.assertEqual(sc.workflow_jobs(path),
                             ["Build (linux-gcc)", "Build (linux-clang)", "MSBuild (vs2026)", "plain", "Fuzz one",
                              "Fuzz two"])

    def test_go_list_and_doctest_list_parsing(self):
        listing = ("TestA\nTestB\nok  \tm/x/pkg/a\t0.01s\n?   \tm/x/pkg/none\t[no test files]\n"
                   "FuzzC\nok  \tm/x/pkg/c\t0.02s\n")
        self.assertEqual(sc.parse_go_list(listing, "m/x"), {"pkg/a": ["TestA", "TestB"], "pkg/none": [],
                                                            "pkg/c": ["FuzzC"]})
        out = "[doctest] listing all test case names\n=====\nfirst case\nsecond — case\n=====\n[doctest] 2\n"
        self.assertEqual(sc.parse_doctest_list(out), ["first case", "second — case"])

    def test_doctest_entries_follow_helios_test(self):
        self.assertEqual(sc.is_doctest_entry({"command": ["x", "--test-case-exclude=perf:*"]}), "main")
        self.assertEqual(sc.is_doctest_entry({"command": ["x", "--test-case=perf:*"]}), "perf")
        self.assertIsNone(sc.is_doctest_entry({"command": ["x", "--smoke", "1"]}))
        emulated = {"command": ["wine", "x.exe", "--test-case-exclude=perf:*"]}
        self.assertEqual(sc.is_doctest_entry(emulated), "main")
        self.assertEqual(sc.doctest_binary_command(emulated), ["wine", "x.exe"])


if __name__ == "__main__":
    unittest.main()
