"""Seeded-fixture tests for the nightly report, the perf comparator and the result runners."""

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import perf
import report
import runners
import scorecard as sc

MODULE = "example.com/services"
DATA = {
    "runs": {"linux-gcc": {"os": "linux", "default": True}, "linux-asan": {"os": "linux", "default": False},
             "windows-vs2026": {"os": "windows", "default": True}, "linux-go": {"os": "linux", "default": True}},
    "gates": {"net_bench_gate": {"runs": ["linux-gcc", "windows-vs2026"], "min_seconds": 600}},
    "perf_metrics": [
        {"id": "net.pps", "doctest": "net_tests", "case": "perf: pps", "pattern": r"-> (\d+) pps",
         "unit": "pps", "better": "higher", "category": "runtime"},
        {"id": "net.trunk_pps", "gate": "net_bench_gate", "pattern": r"trunk: .*?\((\d+) pps",
         "unit": "pps", "better": "higher", "category": "runtime"},
    ],
}


def crit(ident, tests, gaps=(), platforms=("linux", "windows"), cls="N"):
    return {"id": ident, "phase": 0, "class": cls, "owner": "WP-0.13", "title": ident,
            "platforms": list(platforms), "tests": list(tests), "gaps": list(gaps)}


def junit(path, cases):
    suite = ET.Element("testsuite")
    for name, status, seconds, out in cases:
        case = ET.SubElement(suite, "testcase", name=name, time=str(seconds), status="fail" if status == "fail" else "run")
        if status == "fail":
            ET.SubElement(case, "failure", message="Failed")
        if status == "skip":
            ET.SubElement(case, "skipped")
        ET.SubElement(case, "system-out").text = out
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8")


def doctest_xml(path, cases, rc=0):
    root = ET.Element("doctest", binary=path.stem)
    for name, ok, message in cases:
        tc = ET.SubElement(root, "TestCase", name=name)
        if ok is None:
            tc.set("skipped", "true")
            continue
        if message:
            ET.SubElement(ET.SubElement(tc, "Message", type="WARNING"), "Text").text = message
        ET.SubElement(tc, "OverallResultsAsserts", test_case_success="true" if ok else "false", duration="0.5")
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(path, encoding="utf-8")
    path.with_name(path.name[:-4] + ".status.json").write_text(json.dumps({"returncode": rc}), encoding="utf-8")


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.results = self.root / "results"
        linux, win, asan, go = (self.results / n for n in ("a-linux", "b-win", "c-asan", "d-go"))
        for d, run in ((linux, "linux-gcc"), (win, "windows-vs2026"), (asan, "linux-asan"), (go, "linux-go")):
            d.mkdir(parents=True)
            (d / "run.json").write_text(json.dumps({"run": run}), encoding="utf-8")
        junit(linux / "ctest.xml", [("net_tests", "pass", 1, ""), ("render.a", "pass", 1, ""),
                                    ("render.b", "fail", 1, "")])
        junit(win / "ctest.xml", [("net_tests", "pass", 1, "")])
        doctest_xml(linux / "doctest" / "net_tests.xml", [("handshake", True, ""), ("perf: pps", None, "")])
        doctest_xml(linux / "doctest" / "net_tests.perf.xml", [("handshake", None, ""), ("perf: pps", True, "-> 1000 pps")])
        doctest_xml(win / "doctest" / "net_tests.xml", [("handshake", True, "")])
        doctest_xml(asan / "doctest" / "script_tests.xml", [("kill", True, "")], rc=1)
        (linux / "doctest" / "crash_tests.xml").write_text("<doctest><TestCase name='x'>", encoding="utf-8")
        junit(linux / "gates" / "net_bench_gate.xml", [("net_bench_gate", "pass", 612, "trunk: 600 s (20000 pps, x)")])
        junit(win / "gates" / "net_bench_gate.xml", [("net_bench_gate", "pass", 12, "")])
        (go / "go.json").write_text("\n".join(json.dumps(e) for e in [
            {"Action": "run", "Package": f"{MODULE}/pkg/a", "Test": "TestA"},
            {"Action": "pass", "Package": f"{MODULE}/pkg/a", "Test": "TestA"},
            {"Action": "skip", "Package": f"{MODULE}/pkg/a", "Test": "TestSkipped"},
            {"Action": "fail", "Package": f"{MODULE}/pkg/broken"},
        ]) + "\nnot json\n", encoding="utf-8")
        self.runs = report.load_results(self.results, MODULE)
        (self.root / "docs").mkdir()
        (self.root / "docs" / "record.md").write_text("signed", encoding="utf-8")

    def status(self, entry, ci_jobs=None):
        return report.evaluate(entry, DATA, self.runs, ci_jobs, self.root)

    def test_all_references_passing_on_every_platform_passes(self):
        r = self.status(crit("NS-0.1", [{"doctest": "net_tests", "case": "handshake"}, {"ctest": "net_tests"}]))
        self.assertEqual(r["status"], "pass")
        self.assertEqual({k: v["status"] for k, v in r["platforms"].items()}, {"linux": "pass", "windows": "pass"})

    def test_a_failing_test_fails_the_criterion(self):
        r = self.status(crit("RC-1", [{"ctest": "render.*", "platforms": ["linux"]}]))
        self.assertEqual(r["platforms"]["linux"]["status"], "fail")
        self.assertEqual(r["status"], "fail")

    def test_a_platform_without_results_is_unmeasured(self):
        r = self.status(crit("NS-0.2", [{"doctest": "net_tests", "case": "perf: pps"}]))
        self.assertEqual(r["platforms"], {"linux": {"status": "pass", "refs": r["platforms"]["linux"]["refs"]},
                                          "windows": r["platforms"]["windows"]})
        self.assertEqual(r["platforms"]["windows"]["status"], "unmeasured")
        self.assertEqual(r["status"], "unmeasured")

    def test_gaps_are_never_green(self):
        passing = [{"ctest": "net_tests"}]
        failing_gap = [{"clause": "c", "state": "failing", "owner": "WP-0.9"}]
        self.assertEqual(self.status(crit("RT-03", passing, failing_gap))["status"], "fail")
        open_gap = [{"clause": "c", "state": "unmeasured", "owner": "WP-0.9"}]
        self.assertEqual(self.status(crit("RT-03", passing, open_gap))["status"], "unmeasured")
        self.assertEqual(self.status(crit("RT-18", [], open_gap))["status"], "unmeasured")

    def test_skipped_go_test_and_broken_package(self):
        linux = ("linux",)
        self.assertEqual(self.status(crit("X", [{"go": "pkg/a", "test": "TestA"}], platforms=linux))["status"], "pass")
        self.assertEqual(self.status(crit("X", [{"go": "pkg/a", "test": "TestSkipped"}], platforms=linux))["status"],
                         "unmeasured")
        self.assertEqual(self.status(crit("X", [{"go": "pkg/broken", "test": "TestB"}], platforms=linux))["status"],
                         "fail")

    def test_crashed_and_sanitizer_failed_doctest_binaries_fail(self):
        linux = ("linux",)
        r = self.status(crit("X", [{"doctest": "crash_tests", "case": "x"}], platforms=linux))
        self.assertEqual(r["status"], "fail")
        self.assertIn("unreadable", r["platforms"]["linux"]["refs"][0]["detail"])
        r = self.status(crit("X", [{"doctest": "script_tests", "case": "kill", "run": "linux-asan"}], platforms=linux))
        self.assertEqual(r["status"], "fail")
        self.assertIn("every case passed", r["platforms"]["linux"]["refs"][0]["detail"])

    def test_non_default_runs_count_only_when_named(self):
        r = self.status(crit("X", [{"doctest": "script_tests", "case": "kill"}], platforms=("linux",)))
        self.assertEqual(r["status"], "unmeasured")

    def test_gate_shorter_than_its_budget_fails(self):
        r = self.status(crit("NS-0.7", [{"gate": "net_bench_gate"}]))
        self.assertEqual(r["platforms"]["linux"]["status"], "pass")
        self.assertEqual(r["platforms"]["windows"]["status"], "fail")
        self.assertIn("ran 12 s of the required 600 s", r["platforms"]["windows"]["refs"][0]["detail"])

    def test_gate_without_min_seconds_fails(self):
        data = copy.deepcopy(DATA)
        del data["gates"]["net_bench_gate"]["min_seconds"]
        r = report.evaluate(crit("NS-0.7", [{"gate": "net_bench_gate"}]), data, self.runs, None, self.root)
        self.assertEqual(r["platforms"]["linux"]["status"], "fail")
        self.assertIn("no positive min_seconds", r["platforms"]["linux"]["refs"][0]["detail"])

    def test_ci_jobs_and_evidence(self):
        jobs = {"Linux (linux-gcc)": "success", "Windows (clang-cl)": "failure"}
        self.assertEqual(self.status(crit("P", [{"ci_job": "Linux (linux-gcc)"}]), jobs)["status"], "pass")
        self.assertEqual(self.status(crit("P", [{"ci_job": "Windows (clang-cl)"}]), jobs)["status"], "fail")
        self.assertEqual(self.status(crit("P", [{"ci_job": "Linux (linux-gcc)"}]), None)["status"], "unmeasured")
        record = crit("E", [{"evidence": "docs/record.md"}], platforms=("any",), cls="M")
        self.assertEqual(self.status(record)["status"], "pass")
        record["tests"] = [{"evidence": "docs/missing.md"}]
        self.assertEqual(self.status(record)["status"], "unmeasured")

    def test_m_records_are_green_once_they_exist(self):
        data = dict(DATA, criteria=[], exit=[crit("EXIT-0.f", [{"evidence": "docs/record.md"}], platforms=("any",), cls="M")])
        rep = report.build_report(data, self.runs, None, None, False, 0, self.root)
        self.assertTrue(rep["exit"][0]["green"])

    def test_broken_pin_is_reported(self):
        gap = {"clause": "c", "state": "failing", "owner": "WP-0.9", "pinned_by": {"ctest": "render.b"}}
        r = self.status(crit("RT-03", [{"ctest": "net_tests"}], [gap], platforms=("linux",)))
        self.assertTrue(any("no longer holds" in n for n in r["notes"]), r["notes"])

    def test_streaks_and_green(self):
        data = dict(DATA, criteria=[crit("NS-0.1", [{"ctest": "net_tests"}])], exit=[])
        prev = None
        for night in range(3):
            rep = report.build_report(data, self.runs, None, prev, True, 0, self.root)
            self.assertEqual(rep["criteria"][0]["streak"], night + 1)
            self.assertEqual(rep["criteria"][0]["green"], night == 2)
            prev = rep
        manual = report.build_report(data, self.runs, None, prev, False, 0, self.root)
        self.assertEqual(manual["criteria"][0]["streak"], 3)
        self.assertEqual(manual["summary"]["green_fraction"], 1.0)
        data["criteria"][0]["tests"] = [{"ctest": "render.b", "platforms": ["linux"]}]
        broken = report.build_report(data, self.runs, None, manual, True, 0, self.root)
        self.assertEqual((broken["criteria"][0]["streak"], broken["criteria"][0]["green"]), (0, False))
        text = report.markdown(broken)
        self.assertIn("0 of 1 criteria pass tonight, 1 fail", text)
        self.assertIn("| NS-0.1 | **FAIL** |", text)


class PerfTests(unittest.TestCase):
    def test_extract_reads_declared_metrics_and_durations(self):
        with tempfile.TemporaryDirectory() as d:
            results = Path(d)
            run = results / "a"
            run.mkdir()
            (run / "run.json").write_text('{"run": "linux-gcc"}', encoding="utf-8")
            doctest_xml(run / "doctest" / "net_tests.perf.xml", [("perf: pps", True, "sent -> 1234 pps")])
            junit(run / "gates" / "net_bench_gate.xml", [("net_bench_gate", "pass", 600, "trunk: 600 s (19990 pps, …)")])
            entry = perf.extract(DATA, report.load_results(results, MODULE), "abc", "2026-09-26")
        m = entry["metrics"]
        self.assertEqual(m["linux-gcc/net.pps"]["value"], 1234.0)
        self.assertEqual(m["linux-gcc/net.trunk_pps"]["value"], 19990.0)
        self.assertFalse(m["linux-gcc/duration:net_tests:perf: pps"]["gate"])
        self.assertEqual(entry["declared"], ["net.pps", "net.trunk_pps"])

    @staticmethod
    def entry(sha, **values):
        return {"sha": sha, "date": "", "declared": ["t", "p", "b"], "metrics": {
            k: {"value": v, "unit": "u", "better": {"t": "lower", "p": "higher", "b": "lower", "d": "lower"}[k[0]],
                "category": {"t": "runtime", "p": "runtime", "b": "backend", "d": "runtime"}[k[0]], "gate": k[0] != "d"}
            for k, v in values.items()}}

    def verdicts(self, history, entry):
        _, rows = perf.compare({"entries": history}, entry, 5)
        return {r["metric"]: r["verdict"] for r in rows}

    def test_within_budget_passes_and_beyond_fails(self):
        history = [self.entry(str(i), t=100.0, p=1000.0, b=100.0, d=1.0) for i in range(5)]
        ok = self.verdicts(history, self.entry("x", t=104.9, p=951.0, b=109.0, d=9.0))
        self.assertEqual(ok, {"t": "ok", "p": "ok", "b": "ok", "d": "slower"})
        bad = self.verdicts(history, self.entry("y", t=105.5, p=940.0, b=111.0, d=1.0))
        self.assertEqual(bad, {"t": "regression", "p": "regression", "b": "regression", "d": "ok"})

    def test_baseline_is_the_median_of_the_window(self):
        history = [self.entry(str(i), t=v) for i, v in enumerate([100.0, 100.0, 300.0, 100.0, 100.0])]
        self.assertEqual(self.verdicts(history, self.entry("x", t=104.0)), {"t": "ok"})
        # A 2 % creep per night passes against last night alone but not against the window's median.
        creep = [self.entry(str(i), t=100.0 * 1.02 ** i) for i in range(5)]
        tonight = self.entry("x", t=100.0 * 1.02 ** 5)
        _, rows = perf.compare({"entries": creep}, tonight, 1)
        self.assertEqual(rows[0]["verdict"], "ok")
        self.assertEqual(self.verdicts(creep, tonight), {"t": "regression"})

    def test_first_night_and_missing_metrics(self):
        self.assertEqual(self.verdicts([], self.entry("x", t=1.0)), {"t": "new"})
        history = [self.entry("0", t=1.0, p=2.0)]
        self.assertEqual(self.verdicts(history, self.entry("x", t=1.0)), {"t": "ok", "p": "missing"})
        undeclared = self.entry("x", t=1.0)
        undeclared["declared"] = ["t"]
        self.assertEqual(self.verdicts(history, undeclared), {"t": "ok"})

    def test_history_is_bounded_and_main_exits_nonzero_on_regression(self):
        with tempfile.TemporaryDirectory() as d:
            hist = Path(d) / "h.json"
            hist.write_text(json.dumps({"entries": [self.entry(str(i), t=1.0) for i in range(70)]}), encoding="utf-8")
            new = Path(d) / "e.json"
            new.write_text(json.dumps(self.entry("x", t=2.0)), encoding="utf-8")
            out = Path(d) / "n.json"
            with contextlib.redirect_stdout(io.StringIO()):
                rc = perf.main(["compare", "--entry", str(new), "--history", str(hist), "--out", str(out)])
            self.assertEqual(rc, 1)
            entries = json.loads(out.read_text(encoding="utf-8"))["entries"]
            self.assertEqual((len(entries), entries[-1]["sha"]), (perf.HISTORY_LIMIT, "x"))


class RunnerTests(unittest.TestCase):
    def test_gate_writes_junit_with_exit_code_and_output(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            with contextlib.redirect_stdout(io.TextIOWrapper(io.BytesIO())):
                rc = runners.run_gate("g", [sys.executable, "-c", "print('hello'); raise SystemExit(3)"], out,
                                      None, None, None)
            self.assertEqual(rc, 3)
            (name, status, seconds, output), = report.junit_cases(out / "g.xml")
            self.assertEqual((name, status), ("g", "fail"))
            self.assertIn("hello", output)
            with contextlib.redirect_stdout(io.TextIOWrapper(io.BytesIO())):
                rc = runners.run_gate("slow", [sys.executable, "-c", "import time; time.sleep(30)"], out,
                                      None, None, 0.5)
            self.assertEqual(rc, 124)
            self.assertEqual(report.junit_cases(out / "slow.xml")[0][1], "fail")

    def test_bare_command_is_found_in_the_build(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / "bin" / "Release" / "tool.exe"
            exe.parent.mkdir(parents=True)
            exe.write_text("", encoding="utf-8")
            self.assertEqual(runners.find_binary("tool", d, "Release"), str(exe))
            self.assertEqual(runners.find_binary("sub/tool", d, "Release"), "sub/tool")

    def test_doctest_runner_uses_ctest_settings(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            fake = work / "fake_doctest.py"
            fake.write_text(
                "import os, sys\n"
                "out = next(a for a in sys.argv if a.startswith('--out='))[6:]\n"
                "name = '%s %s' % (os.path.samefile(os.getcwd(), WORK), os.environ.get('HX'))\n"
                "open(out, 'w').write('<doctest><TestCase name=\"' + name + '\"><OverallResultsAsserts "
                "test_case_success=\"true\"/></TestCase></doctest>')\n"
                "sys.exit(0 if '--reporters=xml' in sys.argv else 5)\n".replace("WORK", repr(str(work))),
                encoding="utf-8")
            tests = [{"name": "fake_tests", "command": [sys.executable, str(fake), "--test-case-exclude=perf:*"],
                      "properties": [{"name": "WORKING_DIRECTORY", "value": str(work)},
                                     {"name": "ENVIRONMENT", "value": ["HX=1"]}]},
                     {"name": "other", "command": [sys.executable, "-c", "raise SystemExit(9)"]}]
            original = sc.ctest_tests
            sc.ctest_tests = lambda *args: tests
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    rc = runners.run_doctest("build", None, False, work / "out", "ctest")
            finally:
                sc.ctest_tests = original
            self.assertEqual(rc, 0)
            res = report.Results("r")
            report.read_doctest(res, work / "out" / "fake_tests.xml")
            self.assertEqual(res.doctest, {"fake_tests": {"True 1": "pass"}})
            self.assertEqual(json.loads((work / "out" / "fake_tests.status.json").read_text())["returncode"], 0)


if __name__ == "__main__":
    unittest.main()
