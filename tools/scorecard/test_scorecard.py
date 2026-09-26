"""Seeded-fixture tests for the scorecard registry checker (tools/scorecard/scorecard.py)."""

import copy
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
    base.update(fields)
    return base


GAP = [{"clause": "not built yet", "state": "unmeasured", "owner": "WP-0.18"}]
VALID = {
    "version": 1, "plan_rev": 6, "covers": [0],
    "runs": {"linux-gcc": {"os": "linux", "default": True}, "linux-asan": {"os": "linux", "default": False},
             "windows-vs2026": {"os": "windows", "default": True}},
    "gates": {"net_bench_gate": {"runs": ["linux-gcc", "windows-vs2026"], "min_seconds": 600},
              "fuzz_linux": {"runs": ["linux-gcc"], "min_seconds": 3600}},
    "criteria": [
        entry("AAA-PLT-1", source="01 §3.10", tests=[{"ci_job": "Build (linux-gcc)"}, {"ci_job": "MSBuild (vs2026)"}]),
        entry("AAA-PLT-2", source="01 §3.10", platforms=["windows"]),
        entry("RC-1", source="03 §9.3", tests=[{"ctest": "rendertest.vulkan.*", "platforms": ["linux"]}]),
        entry("ED-1", source="07 §5.2", status="unmeasured", tests=[], gaps=GAP),
        entry("NS-0.1", tests=[{"doctest": "net_tests", "case": "NS-0.1: handshake"}]),
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
VALID["exit"][0]["class"] = VALID["exit"][0].pop("class_")

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
        for name, text in PLAN.items():
            (self.plan_dir / name).write_text(text, encoding="utf-8")
        self.ci = self.root / "ci.yml"
        self.ci.write_text(CI_YML, encoding="utf-8")
        self.plan = sc.plan_criteria(self.plan_dir)

    def run_check(self, data, inventories=()):
        path = self.root / "scorecard.jsonc"
        text = "// fixture\n" + json.dumps(data, indent=2, ensure_ascii=False)
        path.write_text(text, encoding="utf-8")
        loaded, raw = sc.load_jsonc(path)
        errors, _ = sc.validate(loaded, raw, path, self.plan, sc.workflow_jobs(self.ci))
        for inv in inventories:
            errors += sc.check_inventory(loaded, raw, path, inv)
        return errors

    def assertFinding(self, errors, fragment):
        self.assertTrue(any(fragment in e for e in errors), f"no finding with {fragment!r} in {errors}")

    def mutate(self, ident, **fields):
        data = copy.deepcopy(VALID)
        for e in data["criteria"]:
            if e["id"] == ident:
                e.update(fields)
        return data


class PlanTests(Fixture):
    def test_criterion_tables_and_phases(self):
        known, exits = self.plan
        self.assertEqual(known["AAA-PLT-2"]["phases"], {0, 2})
        self.assertEqual(known["AAA-SRV-8"]["phases"], {2, 3})
        self.assertEqual(known["RT-03"]["phases"], {0, 1})
        self.assertEqual(known["RT-12"]["phases"], {1, 2, 3, 4})
        self.assertEqual(known["NS-1.1"]["phases"], {1})
        self.assertEqual(known["BE-A3a"]["phases"], {1})
        self.assertEqual(known["CL-5"]["class"], "N+H")
        self.assertNotIn("RT-99", known)
        self.assertIn("GP-4a", known)
        self.assertIn("GP-4b", known)
        self.assertNotIn("GP-4", known)
        self.assertEqual(exits[0], {"AAA-PLT-1", "AAA-PLT-2", "RC-1", "ED-1", "NS-0.1", "NS-0.2", "BE-A1",
                                    "RT-03", "GP-1"})
        self.assertEqual(exits[1], {"AAA-REN-5", "RT-01", "RT-02", "RT-03", "BE-A3a", "BE-A4", "GP-2", "GP-4a"})

    def test_parse_phases(self):
        self.assertEqual(sc.parse_phases("0"), {0})
        self.assertEqual(sc.parse_phases("1–5"), {1, 2, 3, 4, 5})
        self.assertEqual(sc.parse_phases("4 (1)"), {1, 4})
        self.assertEqual(sc.parse_phases("1 (Earth-size: 3)"), {1})
        self.assertEqual(sc.parse_phases("4 (tooling 3)"), {4})


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

    def test_unregistered_phase0_criterion_fails(self):
        data = copy.deepcopy(VALID)
        data["criteria"] = [e for e in data["criteria"] if e["id"] != "NS-0.2"]
        self.assertFinding(self.run_check(data), "phase-0 criterion NS-0.2 (04) is not registered")

    def test_exit_only_criterion_must_be_registered(self):
        data = copy.deepcopy(VALID)
        data["criteria"] = [e for e in data["criteria"] if e["id"] != "GP-1"]
        self.assertFinding(self.run_check(data), "phase-0 criterion GP-1")

    def test_phase_outside_the_plan_fails(self):
        self.assertFinding(self.run_check(self.mutate("RC-1", phase=3)), "RC-1 has no phase-3 scope")

    def test_status_must_match_tests_and_gaps(self):
        self.assertFinding(self.run_check(self.mutate("ED-1", status="measured")), "so it is 'unmeasured'")
        self.assertFinding(self.run_check(self.mutate("RT-03", gaps=[])), "so it is 'measured'")

    def test_entry_without_tests_or_gaps_fails(self):
        errors = self.run_check(self.mutate("ED-1", gaps=[]))
        self.assertFinding(errors, "without tests must list its gaps")

    def test_unknown_reference_field_fails(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "case": "x", "platfroms": ["linux"]}])
        self.assertFinding(self.run_check(data), "unknown field 'platfroms'")

    def test_reference_needs_one_kind(self):
        data = self.mutate("NS-0.1", tests=[{"doctest": "net_tests", "ctest": "net_tests", "case": "x"}])
        self.assertFinding(self.run_check(data), "needs exactly one of")

    def test_gates_must_be_declared_and_run_on_the_platform(self):
        self.assertFinding(self.run_check(self.mutate("NS-0.2", tests=[{"gate": "nope"}])), "not declared")
        self.assertFinding(self.run_check(self.mutate("NS-0.2", tests=[{"gate": "fuzz_linux"}])),
                           "never runs on windows")

    def test_run_must_exist_and_match_platform(self):
        data = self.mutate("NS-0.1", tests=[{"ctest": "net_tests", "run": "linux-tsan"}])
        self.assertFinding(self.run_check(data), "unknown run 'linux-tsan'")
        data = self.mutate("AAA-PLT-2", tests=[{"ctest": "net_tests", "run": "linux-gcc"}])
        self.assertFinding(self.run_check(data), "not on the reference's platforms")

    def test_class_must_match_the_plan(self):
        data = copy.deepcopy(VALID)
        data["covers"] = []
        data["criteria"].append(entry("CL-5", phase=2, source="08 §4.4", class_="N"))
        data["criteria"][-1]["class"] = data["criteria"][-1].pop("class_")
        self.assertFinding(self.run_check(data), "class 'N' differs from the plan's 'N+H'")

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

    def test_metric_case_must_exist(self):
        self.assertFinding(self.run_check(self.with_metric(case="NS-0.1: renamed"), [INVENTORY]),
                           "doctest case doctest net_tests / NS-0.1: renamed does not exist")

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


class FormatTests(unittest.TestCase):
    def test_jsonc_comments_and_trailing_commas(self):
        text = '{\n  // c\n  "a": [1, 2,], /* x\n y */ "b": "//not a comment",\n}\n'
        self.assertEqual(json.loads(sc.strip_jsonc(text)), {"a": [1, 2], "b": "//not a comment"})
        self.assertEqual(sc.strip_jsonc(text).count("\n"), text.count("\n"))

    def test_jsonc_syntax_error_reports_the_line(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "bad.jsonc"
            path.write_text('{\n  "a": 1\n  "b": 2\n}\n', encoding="utf-8")
            with self.assertRaisesRegex(sc.ScorecardError, r"bad.jsonc:3: "):
                sc.load_jsonc(path)

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
