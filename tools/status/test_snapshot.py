"""Seeded-fixture tests for tools/status/snapshot.py (the D6/D7 status check)."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import snapshot

PLAN = """# 09
### 8.1 Phase 0 status

| Item | Status | Evidence / gap |
|---|---|---|
| WP-0.13 HTP transport (`engine/net`) | **Done** | tests |
| Tree inventory (D6 check) | Every module directory, with its WP | `engine/core` (0.5), `engine/net` (0.13); `services/cmd/helios-backend`, `services/pkg/clock`, `services/pkg/idgen` (all 0.15); `tools/ci` (0.1), `tools/gone` (0.7) |

### 8.2 Next

`engine/elsewhere` is named after §8.1 and does not count.
"""


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "docs" / "plan").mkdir(parents=True)
        (self.root / "docs" / "plan" / "PLAN-REV").write_text("6\n", encoding="utf-8")
        self.plan = self.root / "docs" / "plan" / "09-roadmap-and-process.md"
        self.plan.write_text(PLAN, encoding="utf-8")
        for module, rev in (("engine/core", 6), ("engine/net", 5), ("services/cmd/helios-backend", None),
                            ("services/pkg/clock", None), ("services/pkg/idgen", None), ("tools/ci", None)):
            (self.root / module).mkdir(parents=True)
            if rev is not None:
                (self.root / module / "README.md").write_text(f"# m\n\nPlan-Rev: {rev}\n", encoding="utf-8")
        (self.root / "services" / "README.md").write_text("# s\nPlan-Rev: 6\n", encoding="utf-8")

    def check(self):
        failures, notes, count, rev = snapshot.check(self.root)
        return failures, notes

    def test_current_status_passes(self):
        failures, notes = self.check()
        self.assertEqual(failures, [])
        self.assertTrue(any("tools/gone" in n for n in notes), notes)

    def test_unnamed_module_fails(self):
        (self.root / "engine" / "elsewhere").mkdir()
        failures, _ = self.check()
        self.assertEqual(failures, ["not named in 09 §8.1 (D6): engine/elsewhere"])

    def test_prefix_of_a_named_path_is_not_named(self):
        (self.root / "engine" / "ne").mkdir()  # `engine/net` must not count as naming engine/ne
        failures, _ = self.check()
        self.assertEqual(failures, ["not named in 09 §8.1 (D6): engine/ne"])

    def test_readme_without_plan_rev_fails(self):
        (self.root / "engine" / "core" / "README.md").write_text("# core\n", encoding="utf-8")
        failures, _ = self.check()
        self.assertEqual(failures, ["README without a 'Plan-Rev: <n>' line (D7): engine/core/README.md"])

    def test_plan_rev_above_the_plan_fails(self):
        (self.root / "services" / "README.md").write_text("Plan-Rev: 7\n", encoding="utf-8")
        failures, _ = self.check()
        self.assertEqual(failures, ["Plan-Rev above the plan's revision: services/README.md (Plan-Rev 7 > PLAN-REV 6)"])

    def test_missing_plan_rev_file_is_an_error(self):
        (self.root / "docs" / "plan" / "PLAN-REV").unlink()
        with self.assertRaises(snapshot.StatusError):
            self.check()

    def test_module_named_outside_the_inventory_row_fails(self):
        (self.root / "engine" / "script").mkdir()
        self.plan.write_text(PLAN.replace("| **Done** | tests |", "| **Done** | `engine/script` too |"),
                             encoding="utf-8")
        failures, _ = self.check()
        self.assertEqual(failures, ["no WP for it in the tree inventory row (D6; `snapshot.py inventory --write`): "
                                    "engine/script"])

    def test_main_exit_codes(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(snapshot.main(["--root", str(self.root), "check"]), 0)
            (self.root / "apps" / "newapp").mkdir(parents=True)
            self.assertEqual(snapshot.main(["--root", str(self.root)]), 1)
        self.assertIn("FAIL: not named in 09 §8.1 (D6): apps/newapp", out.getvalue())

    def test_inventory_row_is_generated_from_the_tree(self):
        (self.root / "apps" / "newapp").mkdir(parents=True)
        cell = snapshot.inventory_cell(self.root)
        self.assertEqual(cell, "`engine/core` (0.5), `engine/net` (0.13); `apps/newapp` (?); "
                               "`services/cmd/helios-backend`, `services/pkg/clock`, `services/pkg/idgen` (all 0.15); "
                               "`tools/ci` (0.1), `tools/gone` (0.7)")

    def test_write_updates_only_the_inventory_row(self):
        (self.root / "tools" / "newtool").mkdir(parents=True)
        self.assertTrue(snapshot.write_inventory(self.root))
        text = self.plan.read_text(encoding="utf-8")
        self.assertIn("`tools/newtool` (?)", text)
        self.assertEqual(text.replace(snapshot.row_line(text), ""), PLAN.replace(snapshot.row_line(PLAN), ""))
        self.assertFalse(snapshot.write_inventory(self.root))
        failures, _ = self.check()
        self.assertEqual(failures, ["no WP for it in the tree inventory row (D6; `snapshot.py inventory --write`): "
                                    "tools/newtool"])

    def test_write_keeps_crlf_line_endings(self):
        self.plan.write_bytes(PLAN.replace("\n", "\r\n").encode("utf-8"))
        (self.root / "tools" / "newtool").mkdir(parents=True)
        self.assertTrue(snapshot.write_inventory(self.root))
        raw = self.plan.read_bytes().decode("utf-8")
        self.assertIn("`tools/newtool` (?) |\r\n", raw)
        self.assertEqual(raw.count("\r\n"), PLAN.count("\n"))
        self.assertNotIn("\r\r", raw)

    def test_report_counts_tests_per_module(self):
        inv = {"os": "linux", "doctest": {"net_tests": ["a", "b"], "core_tests": ["c"]},
               "go": {"pkg/clock": ["TestFake"], "pkg/idgen/sub": ["TestX", "TestY"]}}
        text = snapshot.report(self.root, [inv])
        self.assertIn("| `engine/net` | 0.13 | 5 | linux 2 |", text)
        self.assertIn("| `services/pkg/idgen` | 0.15 | no README | linux (go) 2 |", text)


if __name__ == "__main__":
    unittest.main()
