"""Seeded-fixture tests for tools/status/snapshot.py (the D6/D7 status check)."""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import snapshot

PLAN = """# 09
### 8.1 Phase 0 status

| Item | Status | Evidence / gap |
|---|---|---|
| WP-0.1 CI | see the | Tree inventory (D6 check) | row below |
| WP-0.13 HTP transport (`engine/net`) | **Done** | tests |
| Tree inventory (D6 check) | Every module directory, with its WP | `engine/core` (0.5), `engine/net` (0.13); `services/cmd/helios-backend`, `services/pkg/clock`, `services/pkg/idgen` (all 0.15); `tools/ci` (0.1), `tools/gone` (0.7) |

### 8.2 Next

`engine/elsewhere` is named after §8.1 and does not count.
"""

WORKFLOW = """name: CI
on: [push]
jobs:
  build:
    name: Linux (${{ matrix.cc }})
    strategy:
      matrix:
        cc: [gcc, clang]
  lint:
    runs-on: ubuntu-latest
"""

REWORK = """| WP | Area | Title | Plan | Deps | Deliverables | Acceptance |
|---|---|---|---|---|---|---|
| WP-0.2r | IN | Conformance rework: ISA levels | 02 | 0.2 | x | y |
| WP-0.3 | IN | Nightly and scorecard | 09 | 0.1 | x | y |
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

    def test_missing_section_is_an_error(self):
        self.plan.write_text(PLAN.replace("### 8.1 ", "### 8.0 "), encoding="utf-8")
        with self.assertRaisesRegex(snapshot.StatusError, "cannot find 09 §8.1"):
            self.check()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(snapshot.main(["--root", str(self.root), "check"]), 2)
        self.assertIn("snapshot: error: cannot find 09 §8.1", out.getvalue())

    def test_non_numeric_plan_rev_is_an_error(self):
        (self.root / "docs" / "plan" / "PLAN-REV").write_text("six\n", encoding="utf-8")
        with self.assertRaisesRegex(snapshot.StatusError, "does not start with a number"):
            self.check()

    def test_plan_rev_with_a_bom_is_read(self):
        (self.root / "docs" / "plan" / "PLAN-REV").write_bytes(b"\xef\xbb\xbf6\r\n")
        self.assertEqual(snapshot.plan_rev(self.root), 6)
        self.assertEqual(self.check()[0], [])

    def test_hidden_and_pycache_directories_are_not_modules(self):
        for d in ("engine/.cache", "tools/__pycache__", "services/pkg/.idea"):
            (self.root / d).mkdir(parents=True)
        (self.root / "apps").mkdir()
        (self.root / "apps" / "notes.txt").write_text("a file, not a module\n", encoding="utf-8")
        self.assertNotIn("engine/.cache", snapshot.modules(self.root))
        self.assertEqual(self.check()[0], [])

    def test_the_row_label_must_be_the_first_cell(self):
        # The WP-0.1 row above mentions the label in a later cell; it must not be taken for the row.
        self.assertTrue(snapshot.row_line(PLAN).startswith("| Tree inventory (D6 check) | Every module"))
        self.assertEqual(snapshot.row_modules(PLAN)["engine/net"], "0.13")
        self.assertEqual(self.check()[0], [])

    def test_a_renamed_row_label_fails(self):
        self.plan.write_text(PLAN.replace("| Tree inventory (D6 check) | Every", "| Tree inventory | Every"),
                             encoding="utf-8")
        failures, _ = self.check()
        self.assertEqual(failures, ["09 §8.1 has no '| Tree inventory (D6 check) |' row (D6)"])
        with self.assertRaisesRegex(snapshot.StatusError, "has no"):
            snapshot.write_inventory(self.root)

    def test_bom_and_non_utf8_readmes(self):
        (self.root / "engine" / "core" / "README.md").write_bytes(b"\xef\xbb\xbfPlan-Rev: 6\n# core\n")
        self.assertEqual(snapshot.readme_rev(self.root, "engine/core"), (True, 6))
        self.assertEqual(self.check()[0], [])
        (self.root / "engine" / "core" / "README.md").write_bytes(b"# core \xff\xfe\nPlan-Rev: 6\n")
        self.assertEqual(snapshot.readme_rev(self.root, "engine/core"), (True, None))
        self.assertEqual(self.check()[0], ["README without a 'Plan-Rev: <n>' line (D7): engine/core/README.md"])

    def test_row_not_in_generated_form_is_noted_and_write_round_trips(self):
        swapped = PLAN.replace("`engine/core` (0.5), `engine/net` (0.13)", "`engine/net` (0.13), `engine/core` (0.5)")
        self.plan.write_text(swapped, encoding="utf-8")
        failures, notes = self.check()
        self.assertEqual(failures, [])
        self.assertTrue(any("not in the generated form" in n for n in notes), notes)
        before = snapshot.row_modules(swapped)
        self.assertTrue(snapshot.write_inventory(self.root))
        text = self.plan.read_text(encoding="utf-8")
        self.assertEqual(text, PLAN)  # the fixture's row is the generator's output
        self.assertEqual(snapshot.row_modules(text), before)
        self.assertFalse(any("generated form" in n for n in self.check()[1]))
        self.assertFalse(snapshot.write_inventory(self.root))
        self.assertEqual(self.plan.read_text(encoding="utf-8"), PLAN)

    def test_inventory_keeps_other_top_level_groups(self):
        self.plan.write_text(PLAN.replace("`tools/gone` (0.7) |", "`tools/gone` (0.7); `content/demo` (0.8) |"),
                             encoding="utf-8")
        self.assertTrue(snapshot.inventory_cell(self.root).endswith("`tools/gone` (0.7); `content/demo` (0.8)"))

    def test_write_keeps_escaped_pipes_in_other_cells(self):
        self.plan.write_text(PLAN.replace("with its WP |", "with its WP (`a\\|b`) |"), encoding="utf-8")
        (self.root / "tools" / "newtool").mkdir(parents=True)
        self.assertTrue(snapshot.write_inventory(self.root))
        line = snapshot.row_line(self.plan.read_text(encoding="utf-8"))
        self.assertIn("with its WP (`a\\|b`) |", line)
        self.assertEqual(len(snapshot.scorecard._cells(line)), 3)

    def test_module_of_maps_binaries_by_name_prefix(self):
        mods = ["engine/core", "engine/pcg", "engine/pcgx", "engine/render"]
        self.assertEqual(snapshot.module_of("render_tests", mods), "engine/render")
        self.assertEqual(snapshot.module_of("pcg_gpu_tests", mods), "engine/pcg")
        self.assertEqual(snapshot.module_of("pcgx_tests", mods), "engine/pcgx")
        self.assertEqual(snapshot.module_of("rendering_tests", mods), "(other)")
        self.assertEqual(snapshot.module_of("sample_tests", mods), "(other)")

    def test_report_sections(self):
        (self.root / "engine" / "render").mkdir()
        self.plan.write_text(PLAN.replace("`engine/net` (0.13)", "`engine/net` (0.13), `engine/render` (0.12)")
                             + REWORK, encoding="utf-8")
        wf = self.root / ".github" / "workflows"
        wf.mkdir(parents=True)
        (wf / "ci.yml").write_text(WORKFLOW, encoding="utf-8")
        inv = {"toolchain": "linux-gcc", "doctest": {"render_gpu_tests": ["a", "b", "c"], "sample_tests": ["d"]}}
        text = snapshot.report(self.root, [inv])
        self.assertIn("## Status snapshot (PLAN-REV 6)", text)
        self.assertIn("| `engine/render` | 0.12 | no README | linux-gcc 3 |", text)
        self.assertNotIn("tools/gone", text)  # named in the row but not in this tree: not a module here
        self.assertIn("| (other) | — | — | linux-gcc 1 |", text)
        self.assertIn("### CI jobs\n\n- `ci.yml`: Linux (gcc), Linux (clang), lint\n", text)
        self.assertIn("- WP-0.2r: Conformance rework: ISA levels\n", text)
        self.assertNotIn("WP-0.3:", text)

    @unittest.skipUnless(shutil.which("git"), "git not on PATH")
    def test_report_lists_wp_branches(self):
        git = ["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@example.invalid"]
        subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "x"], check=True)
        subprocess.run(git + ["branch", "agent/x/wp-0.3-scorecard"], check=True)
        subprocess.run(git + ["branch", "unrelated"], check=True)
        text = snapshot.report(self.root, [])
        section = text.split("### WP branches", 1)[1].split("###", 1)[0]
        self.assertIn("- `agent/x/wp-0.3-scorecard`", section)
        self.assertNotIn("unrelated", section)


@unittest.skipIf(os.name == "nt" or not shutil.which("cmake"), "POSIX fake interpreters need /bin/sh and cmake")
class CheckStatusScriptTests(unittest.TestCase):
    """check_status.cmake's interpreter search, against a copy of the script and a stand-in snapshot.py."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        status = self.root / "tools" / "status"
        status.mkdir(parents=True)
        shutil.copy(Path(snapshot.__file__).with_name("check_status.cmake"), status)
        self.snapshot_rc(0)

    def snapshot_rc(self, rc):
        (self.root / "tools" / "status" / "snapshot.py").write_text(f"import sys\nsys.exit({rc})\n", encoding="utf-8")

    def fake(self, directory, name, body):
        d = self.root / directory
        d.mkdir(exist_ok=True)
        exe = d / name
        exe.write_text("#!/bin/sh\n" + body + "\n", encoding="utf-8")
        exe.chmod(0o755)
        return d

    def stub(self, directory, name="python3"):  # like the Windows Store alias: never runs Python
        return self.fake(directory, name, "exit 49")

    def real(self, directory, name="python"):
        return self.fake(directory, name, f'exec "{sys.executable}" "$@"')

    def run_script(self, path_dirs, *defines):
        cmd = [shutil.which("cmake"), *defines, "-P", str(self.root / "tools" / "status" / "check_status.cmake")]
        env = dict(os.environ, PATH=os.pathsep.join(str(d) for d in path_dirs))
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)
        return r.returncode, " ".join((r.stdout + r.stderr).split())  # CMake wraps long messages

    def test_a_stub_earlier_on_path_is_skipped(self):
        rc, out = self.run_script([self.stub("a"), self.real("b")])
        self.assertEqual(rc, 0, out)

    def test_a_stub_for_an_earlier_name_is_skipped(self):
        # The review's reproduction: python3 is looked for in every directory before python, so the stub in
        # the later directory is found first; it must be rejected and the working python used.
        rc, out = self.run_script([self.real("a"), self.stub("b")])
        self.assertEqual(rc, 0, out)

    def test_no_working_python_is_reported(self):
        rc, out = self.run_script([self.stub("a"), self.stub("b", "python")])
        self.assertNotEqual(rc, 0)
        self.assertIn("no Python 3.10+ found on PATH (tried python3, python)", out)

    def test_an_old_python_is_skipped(self):
        old = self.fake("a", "python3", 'case "$1" in -c) exit 1;; esac; exit 0')
        rc, out = self.run_script([old])
        self.assertIn("no Python 3.10+ found", out)
        rc, out = self.run_script([old, self.real("b")])
        self.assertEqual(rc, 0, out)

    def test_stale_and_failed_are_distinct(self):
        self.snapshot_rc(1)
        rc, out = self.run_script([self.real("a")])
        self.assertNotEqual(rc, 0)
        self.assertIn("is stale", out)
        self.snapshot_rc(2)
        rc, out = self.run_script([self.real("a")])
        self.assertNotEqual(rc, 0)
        self.assertIn("snapshot.py failed (exit 2", out)
        self.assertNotIn("stale", out)

    def test_explicit_interpreter(self):
        stub = self.stub("a") / "python3"
        rc, out = self.run_script([], f"-DHELIOS_STATUS_PYTHON={stub}")
        self.assertIn("does not run Python 3.10+", out)
        real = self.real("b") / "python"
        rc, out = self.run_script([self.stub("c")], f"-DHELIOS_STATUS_PYTHON={real}")
        self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
