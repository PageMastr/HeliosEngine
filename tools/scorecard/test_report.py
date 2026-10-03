"""Seeded-fixture tests for the nightly report, the perf comparator and the result runners."""

import contextlib
import copy
import io
import json
import re
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from datetime import date, datetime, timedelta, timezone
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


    def test_a_night_without_a_report_restarts_the_streaks(self):
        data = dict(DATA, criteria=[crit("NS-0.1", [{"ctest": "net_tests"}])], exit=[])
        day = datetime(2026, 10, 1, 4, 0, tzinfo=timezone.utc)
        prev = None
        for night in range(2):
            prev = report.build_report(data, self.runs, None, prev, True, 0, self.root, now=day + timedelta(days=night))
        self.assertEqual(prev["criteria"][0]["streak"], 2)
        # A dispatch run in between carries the last scheduled time forward, not its own.
        manual = report.build_report(data, self.runs, None, prev, False, 0, self.root, now=day + timedelta(days=2))
        self.assertEqual(manual["last_scheduled"], prev["generated"])
        # The scheduled night of day 2 left no report, so day 3 starts again at 1, not 3.
        late = report.build_report(data, self.runs, None, manual, True, 0, self.root, now=day + timedelta(days=3))
        self.assertEqual((late["criteria"][0]["streak"], late["criteria"][0]["green"]), (1, False))
        self.assertIn("no scheduled report for 48 h", late["streaks_restarted"])
        self.assertIn("Every streak restarted tonight", report.markdown(late))
        self.assertEqual(len(late["criteria"][0]["history"]), 4)
        # A delayed schedule within the gap keeps the streak.
        slow = report.build_report(data, self.runs, None, prev, True, 0, self.root,
                                   now=day + timedelta(days=1, hours=30))
        self.assertEqual(slow["criteria"][0]["streak"], 3)

    def approved(self, platforms=("linux", "windows"), record="docs/record.md", runs=("linux-gcc",)):
        entry = crit("NS-0.2", [{"gate": "net_bench_gate"},
                                {"evidence": record, "owner_approval": "2026-09-30", "advisory": ["net.trunk_pps"],
                                 "advisory_runs": list(runs)}],
                     platforms=platforms)
        entry["follow_ups"] = [{"clause": "re-test on fixed hardware", "owner": "WP-0.4"}]
        return entry

    def test_an_owner_approval_labels_the_pass_and_shows_the_advisory_level(self):
        r = self.status(self.approved(platforms=("linux",)))
        self.assertEqual(r["status"], "pass")
        self.assertEqual(r["approval"], {"date": "2026-09-30", "evidence": "docs/record.md", "runs": ["linux-gcc"]})
        self.assertEqual(r["platforms"]["linux"]["advisory"], ["linux-gcc net.trunk_pps 20,000 pps"])
        self.assertEqual(r["follow_ups"], ["re-test on fixed hardware (WP-0.4)"])
        data = dict(DATA, criteria=[self.approved(platforms=("linux",)), crit("NS-0.1", [{"ctest": "net_tests"}])],
                    exit=[])
        rep = report.build_report(data, self.runs, None, None, True, 0, self.root)
        self.assertEqual((rep["summary"]["pass"], rep["summary"]["approved"]), (2, 1))
        text = report.markdown(rep)
        self.assertIn("2 of 2 criteria pass tonight (1 of them on an owner approval)", text)
        self.assertIn("| NS-0.2 | passed (owner approval 2026-09-30, evidence docs/record.md) | passed (approval) | — |",
                      text)
        self.assertIn("advisory under the approval: linux: linux-gcc net.trunk_pps 20,000 pps; "
                      "follow-up, not blocking: re-test on fixed hardware (WP-0.4) |", text)
        self.assertIn("| NS-0.1 | pass | pass |", text)  # no approval, no label
        # A phase exit is not carried by the approval alone (09 §5.6 "Phase exits").
        self.assertIn("While its re-test is open it carries no phase exit on its own", text)

    def test_an_owner_approval_changes_no_result(self):
        # Windows' gate ran 12 s of its 600: it fails with or without the approval, which still shows what
        # it leaves open on the runs it covers.
        r = self.status(self.approved(runs=("linux-gcc", "windows-vs2026")))
        self.assertEqual((r["status"], r["platforms"]["linux"]["status"], r["platforms"]["windows"]["status"]),
                         ("fail", "pass", "fail"))
        self.assertEqual(r["platforms"]["windows"]["advisory"], ["net.trunk_pps not measured"])
        rep = report.build_report(dict(DATA, criteria=[self.approved(runs=("linux-gcc", "windows-vs2026"))], exit=[]),
                                  self.runs, None, None, True, 0, self.root)
        self.assertEqual(rep["summary"]["approved"], 0)
        text = report.markdown(rep)
        self.assertIn("| NS-0.2 | **FAIL** | passed (approval) | **FAIL** |", text)
        self.assertIn("advisory under the approval: linux: linux-gcc net.trunk_pps 20,000 pps; "
                      "windows: net.trunk_pps not measured; follow-up, not blocking", text)
        self.assertIn("ran 12 s of the required 600 s", text)
        self.assertNotIn("criteria pass tonight (", text)

    def test_a_platform_the_approval_does_not_cover_shows_its_plain_result(self):
        # The approval names linux-gcc only: Windows' gate is strict, its levels are not the approval's, and
        # its pass is a plain pass.
        junit(self.results / "b-win" / "gates" / "net_bench_gate.xml",
              [("net_bench_gate", "pass", 611, "trunk: 600 s (19990 pps, x)")])
        self.runs = report.load_results(self.results, MODULE)
        r = self.status(self.approved())
        self.assertEqual((r["status"], r["by_approval"]), ("pass", True))
        self.assertNotIn("advisory", r["platforms"]["windows"])
        self.assertNotIn("by_approval", r["platforms"]["windows"])
        text = report.markdown(report.build_report(dict(DATA, criteria=[self.approved()], exit=[]), self.runs, None,
                                                   None, True, 0, self.root))
        self.assertIn("| NS-0.2 | passed (owner approval 2026-09-30, evidence docs/record.md) | passed (approval) | "
                      "pass |", text)
        self.assertIn("advisory under the approval: linux: linux-gcc net.trunk_pps 20,000 pps; follow-up", text)
        self.assertNotIn("19,990", text)
        # Without Windows' result set the criterion is unmeasured, approval or not.
        self.runs = [res for res in self.runs if res.name != "windows-vs2026"]
        self.assertEqual(self.status(self.approved())["status"], "unmeasured")

    def test_a_pass_without_a_covered_run_is_a_plain_pass(self):
        # validate.ps1's local run (09 §5.9) gates every clause, so its pass is never labelled as an approval's,
        # even on an OS where the approval covers the hosted run.
        local = self.root / "local" / "a-local"
        local.mkdir(parents=True)
        (local / "run.json").write_text(json.dumps({"run": "windows-local"}), encoding="utf-8")
        junit(local / "gates" / "net_bench_gate.xml", [("net_bench_gate", "pass", 612, "trunk: 600 s (20000 pps, x)")])
        data = copy.deepcopy(DATA)
        data["runs"]["windows-local"] = {"os": "windows", "default": True, "nightly": False}
        entry = self.approved(platforms=("windows",), runs=("windows-vs2026",))
        runs = report.load_results(self.root / "local", MODULE)
        r = report.evaluate(entry, data, runs, None, self.root)
        self.assertEqual((r["status"], r["by_approval"]), ("pass", False))
        self.assertEqual(r["platforms"]["windows"]["advisory"], ["net.trunk_pps not measured"])
        rep = report.build_report(dict(data, criteria=[entry], exit=[]), runs, None, None, False, 0, self.root)
        self.assertEqual(rep["summary"]["approved"], 0)
        text = report.markdown(rep)
        self.assertIn("| NS-0.2 | pass | — | pass |", text)
        self.assertNotIn("passed (", text)

    def test_an_approval_without_its_record_is_not_shown(self):
        r = self.status(self.approved(platforms=("linux",), record="docs/missing.md"))
        self.assertEqual(r["status"], "unmeasured")
        self.assertNotIn("approval", r)
        self.assertNotIn("advisory", r["platforms"]["linux"])
        text = report.markdown(report.build_report(
            dict(DATA, criteria=[self.approved(platforms=("linux",), record="docs/missing.md")], exit=[]),
            self.runs, None, None, True, 0, self.root))
        self.assertIn("| NS-0.2 | unmeasured |", text)
        self.assertNotIn("owner approval", text)
        self.assertNotIn("follow-up", text)
        self.assertNotIn("phase exit", text)


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
        self.assertEqual(entry["runs"], sorted(DATA["runs"]))
        self.assertEqual(entry["accepted"], [])
        self.assertEqual(entry["metric_runs"], {})
        one_run = dict(DATA, perf_metrics=[dict(DATA["perf_metrics"][0], run="linux-gcc")])
        self.assertEqual(perf.extract(one_run, [], "abc", "2026-09-26")["metric_runs"], {"net.pps": "linux-gcc"})

    @staticmethod
    def entry(sha, **values):
        return {"sha": sha, "date": "", "declared": ["t", "p", "b"], "metrics": {
            k: {"value": v, "unit": "u", "better": {"t": "lower", "p": "higher", "b": "lower", "d": "lower"}[k[0]],
                "category": {"t": "runtime", "p": "runtime", "b": "backend", "d": "runtime"}[k[0]], "gate": k[0] != "d"}
            for k, v in values.items()}}

    def verdicts(self, history, entry, window=5):
        _, rows = perf.compare({"entries": history}, entry, window)
        return {r["metric"]: r["verdict"] for r in rows if r["verdict"] != "never measured"}

    def test_within_budget_passes_and_beyond_fails(self):
        history = [self.entry(str(i), t=100.0, p=1000.0, b=100.0, d=1.0) for i in range(5)]
        ok = self.verdicts(history, self.entry("x", t=104.9, p=951.0, b=109.0, d=9.0))
        self.assertEqual(ok, {"t": "ok", "p": "ok", "b": "ok", "d": "slower"})
        bad = self.verdicts(history, self.entry("y", t=105.5, p=940.0, b=111.0, d=1.0))
        self.assertEqual(bad, {"t": "regression", "p": "regression", "b": "regression", "d": "ok"})

    def test_baseline_is_the_median_of_the_window(self):
        history = [self.entry(str(i), t=v) for i, v in enumerate([100.0, 100.0, 300.0, 100.0, 100.0])]
        self.assertEqual(self.verdicts(history, self.entry("x", t=104.0)), {"t": "ok"})
        # A 2 % creep per night passes against last night alone, but the anchor still catches it.
        creep = [self.entry(str(i), t=100.0 * 1.02 ** i) for i in range(5)]
        tonight = self.entry("x", t=100.0 * 1.02 ** 5)
        _, rows = perf.compare({"entries": creep}, tonight, 1)
        self.assertAlmostEqual(rows[0]["change"], 2.0)
        self.assertAlmostEqual(rows[0]["anchor_change"], 100.0 * (1.02 ** 5 - 1))
        self.assertEqual((rows[0]["verdict"], rows[0]["note"]), ("regression", "drift against the anchor"))

    def test_first_night_calibration_and_missing_metrics(self):
        self.assertEqual(self.verdicts([], self.entry("x", t=1.0)), {"t": "new"})
        history = [self.entry("0", t=1.0, p=2.0)]
        self.assertEqual(self.verdicts(history, self.entry("x", t=1.0)), {"t": "calibrating", "p": "missing"})
        undeclared = self.entry("x", t=1.0)
        undeclared["declared"] = ["t"]
        self.assertEqual(self.verdicts(history, undeclared), {"t": "calibrating"})
        # Nights 2 to --window are calibration nights: recorded, never failed, even at 10x.
        _, verdicts = self.replay([self.night(1, t=1.0), self.night(2, t=10.0), self.night(3, t=1.0),
                                   self.night(4, t=1.0), self.night(5, t=1.0), self.night(6, t=1.0)])
        self.assertEqual([v["t"] for v in verdicts], ["new"] + ["calibrating"] * 4 + ["ok"])

    @staticmethod
    def day(n):
        return (date(2026, 1, 1) + timedelta(days=n)).isoformat()

    @staticmethod
    def night(n, accepted=(), declared=("t", "p"), runs=("linux-gcc",), metric_runs=None, run="linux-gcc",
              **values):
        """A nightly entry as `extract` writes it: dated, run-qualified keys, the registry's declarations."""
        e = PerfTests.entry(f"sha{n}", **values)
        e.update(date=PerfTests.day(n) + "T03:17:00Z", declared=list(declared), runs=list(runs),
                 accepted=list(accepted), metric_runs=dict(metric_runs or {}))
        e["metrics"] = {f"{run}/{k}": v for k, v in e["metrics"].items()}
        return e

    def replay(self, nights, window=5, history=None):
        """Feed nights through compare as the nightly does; the verdicts per night."""
        history, out = history or {}, []
        for e in nights:
            history, rows = perf.compare(history, e, window)
            out.append({r["metric"].split("/", 1)[-1]: r["verdict"] for r in rows if r["verdict"] != "never measured"})
        return history, out

    def accept(self, n, value, **extra):
        return dict({"metric": "t", "night": self.day(n), "value": value, "reason": "accepted in review"}, **extra)

    def test_an_unfixed_step_regression_fails_every_night(self):
        # Round 1's scenario: +10 % from night 6 on, never fixed, for window + 2 nights.
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in range(6, 13)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[5:]], ["regression"] * 7)
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["verdict"], "regression")
        # Fixing it passes again, against the pre-regression baseline.
        _, rows = perf.compare(history, self.night(13, t=101.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["baseline"]), ("ok", 100.0))

    def test_a_step_held_past_the_history_limit_stays_a_regression(self):
        # Round 2's scenario: the last clean night leaves the 60-entry history; the stored levels carry on.
        steps = perf.HISTORY_LIMIT + 10
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in range(6, 6 + steps)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[5:]], ["regression"] * steps)
        self.assertEqual(verdicts.count({"t": "new"}), 1)
        last = history["entries"][-1]["metrics"]["linux-gcc/t"]
        self.assertEqual((last["baseline"], last["anchor"]), (100.0, 100.0))
        self.assertNotIn(100.0, [e["metrics"]["linux-gcc/t"]["value"] for e in history["entries"]])
        # A history whose only values are regressions and that carries no levels fails instead of restarting.
        bare = {"entries": [dict(self.night(1, t=110.0), metrics={"linux-gcc/t": dict(
            self.night(1, t=110.0)["metrics"]["linux-gcc/t"], verdict="regression")})]}
        _, rows = perf.compare(bare, self.night(2, t=110.0), 5)
        self.assertEqual(rows[0]["verdict"], "no-baseline")

    def test_slow_creep_is_caught_against_the_anchor(self):
        # Rolling medians lag, so creep below about a third of the budget per night never trips them; the
        # anchor does, on the night the drift passes the budget, and keeps failing.
        for rate, category, key, first_bad in ((0.015, "runtime", "t", 4), (0.03, "backend", "b", 4)):
            nights = [self.night(n, **{key: 100.0}) for n in range(1, 6)]
            nights += [self.night(5 + k, **{key: 100.0 * (1 + rate) ** k}) for k in range(1, 26)]
            _, verdicts = self.replay(nights)
            creep = [v[key] for v in verdicts[5:]]
            self.assertEqual(creep, ["ok"] * (first_bad - 1) + ["regression"] * (26 - first_bad), (rate, category))
            # The rolling baseline alone would have passed that night: the anchor is what caught it.
            before, _ = self.replay(nights[:4 + first_bad])
            _, rows = perf.compare(before, nights[4 + first_bad], 5)
            row = next(r for r in rows if r["metric"].endswith("/" + key))
            self.assertEqual((row["verdict"], row["note"]), ("regression", "drift against the anchor"))
            self.assertLess(row["change"], perf.BUDGET_PERCENT[category])

    def test_sub_budget_steps_stack_against_the_anchor_unless_accepted(self):
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=104.0) for n in range(6, 12)]
        steps = nights + [self.night(n, t=108.2) for n in range(12, 16)]
        _, verdicts = self.replay(steps)
        self.assertEqual([v["t"] for v in verdicts[5:]], ["ok"] * 6 + ["regression"] * 4)
        # Accepting the first step (night 6 at 104) makes it the anchor: it and the second step then pass.
        record = [self.accept(6, 104.0)]
        accepted = nights[:6] + [self.night(n, accepted=record, t=104.0) for n in range(7, 30)]
        _, verdicts = self.replay(accepted)
        self.assertEqual({v["t"] for v in verdicts[5:]}, {"ok"})
        _, verdicts = self.replay(accepted + [self.night(n, accepted=record, t=108.2) for n in range(30, 34)])
        self.assertEqual({v["t"] for v in verdicts[-4:]}, {"ok"})

    def test_a_dip_or_an_unaccepted_improvement_does_not_lock_the_normal_level_red(self):
        # Round 3's nit 1: three nights more than 5 % better used to move the rolling median, so every
        # later night at the normal level failed. The rolling baseline is held at the anchor.
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=94.0) for n in (6, 7, 8)]
        nights += [self.night(n, t=100.0) for n in range(9, 69)]
        history, verdicts = self.replay(nights)
        self.assertEqual({v["t"] for v in verdicts[5:]}, {"ok"})
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["anchor"], 100.0)
        # Higher-is-better the same way round.
        nights = [self.night(n, p=100.0) for n in range(1, 6)] + [self.night(n, p=106.0) for n in (6, 7, 8)]
        _, verdicts = self.replay(nights + [self.night(n, p=100.0) for n in range(9, 20)])
        self.assertEqual({v["p"] for v in verdicts[5:]}, {"ok"})
        # An improvement that was accepted moved the anchor, so losing it is still caught.
        record = [self.accept(6, 80.0)]
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(6, accepted=record, t=80.0)]
        nights += [self.night(n, accepted=record, t=80.0) for n in range(7, 12)]
        history, _ = self.replay(nights)
        _, rows = perf.compare(history, self.night(12, accepted=record, t=100.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor"]), ("regression", 80.0))
        # The rolling baseline restarts at the accepted night (older values do not count).
        six, _ = self.replay(nights[:6])
        _, rows = perf.compare(six, self.night(7, accepted=record, t=80.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["baseline"]), ("ok", 80.0))

    def test_one_outlier_among_the_calibration_nights_does_not_set_the_anchor(self):
        # A lucky first night (94) and a bad one (150) among five: the anchor is their median, 100, so the
        # normal level passes and a slow creep from it is still caught.
        nights = [self.night(1, t=94.0), self.night(2, t=150.0)] + [self.night(n, t=100.0) for n in (3, 4, 5)]
        nights += [self.night(5 + k, t=100.0 * 1.015 ** k) for k in range(1, 12)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[:5]], ["new"] + ["calibrating"] * 4)
        self.assertEqual([v["t"] for v in verdicts[5:]], ["ok"] * 3 + ["regression"] * 8)
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["anchor"], 100.0)
        # Regressions after calibration are not part of the anchor.
        nights = [self.night(n, t=100.0) for n in range(1, 4)] + [self.night(n, t=150.0) for n in (4, 5, 6, 7)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[5:]], ["regression"] * 2)

    def test_an_accepted_level_outlives_its_record(self):
        # Once the accepted level is stored, the record can be removed: the anchor stays at the new level.
        record = [self.accept(6, 110.0)]
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(6, accepted=record, t=110.0)]
        nights += [self.night(n, accepted=record, t=110.0) for n in range(7, 13)]
        history, verdicts = self.replay(nights)
        self.assertEqual({v["t"] for v in verdicts[6:]}, {"ok"})
        _, verdicts = self.replay([self.night(n, t=110.0) for n in range(13, 20)], history=history)
        self.assertEqual({v["t"] for v in verdicts}, {"ok"})

    def test_a_kept_accept_outlives_the_history_limit(self):
        # Round 3's blocking 1, the review's test: the record stays in scorecard.jsonc for good.
        record = [self.accept(6, 110.0)]
        nights = [self.night(n, t=100.0) for n in range(1, 6)]
        nights += [self.night(n, accepted=record, t=110.0) for n in range(6, 6 + perf.HISTORY_LIMIT + 10)]
        history, verdicts = self.replay(nights)
        self.assertEqual({v["t"] for v in verdicts[6:]}, {"ok"})
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["accepted"],
                         {"night": self.day(6), "value": 110.0})
        # Its night has left the history, but the record counts as applied, not as stale.
        self.assertNotIn(self.day(6), {e["date"][:10] for e in history["entries"]})
        _, rows = perf.compare(history, self.night(90, accepted=record, t=110.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["note"], rows[0]["anchor"]), ("ok", "", 110.0))

    def test_a_record_older_than_a_fresh_history_is_stale_not_failing(self):
        record = [self.accept(6, 110.0)]
        _, verdicts = self.replay([self.night(n, accepted=record, t=100.0) for n in range(40, 47)])
        self.assertEqual([v["t"] for v in verdicts], ["new"] + ["calibrating"] * 4 + ["ok"] * 2)
        _, rows = perf.compare({}, self.night(40, accepted=record, t=100.0), 5)
        self.assertIn(f"stale perf_accept for {self.day(6)}", rows[0]["note"])

    def test_a_tonight_accept_holds_after_its_record_is_removed(self):
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in (6, 7, 8)]
        history, _ = self.replay(nights + [self.night(9, accepted=[self.accept(9, 110.0)], t=110.0)])
        _, verdicts = self.replay([self.night(10, t=110.0), self.night(11, t=110.0), self.night(12, t=121.0)],
                                  history=history)
        self.assertEqual([v["t"] for v in verdicts], ["ok", "ok", "regression"])

    def test_an_accept_naming_the_oldest_night_without_the_value_fails(self):
        # Round 4's nit 3: the oldest retained night has no value for `t`, so the record is unmatched, not stale.
        nights = [self.night(1, p=2.0)] + [self.night(n, t=100.0, p=2.0) for n in range(2, 8)]
        history, _ = self.replay(nights)
        self.assertEqual(history["entries"][0]["date"][:10], self.day(1))
        _, rows = perf.compare(history, self.night(8, accepted=[self.accept(1, 100.0)], t=100.0, p=2.0), 5)
        row = next(r for r in rows if r["metric"] == "linux-gcc/t")
        self.assertEqual(row["verdict"], "accept-unmatched")

    def test_an_applied_accept_anchors_at_the_stored_value_not_the_records(self):
        # The record's value only has to be within budget of what the night measured; the measurement is the level.
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in (6, 7, 8)]
        history, _ = self.replay(nights)
        _, rows = perf.compare(history, self.night(9, accepted=[self.accept(7, 108.0)], t=110.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor"], rows[0]["accepted"]),
                         ("ok", 110.0, {"night": self.day(7), "value": 110.0}))

    def test_an_improvement_is_noted_and_an_older_record_is_reported(self):
        nights = [self.night(n, t=100.0) for n in range(1, 6)]
        history, _ = self.replay(nights)
        _, rows = perf.compare(history, self.night(6, t=80.0), 5)
        self.assertEqual(rows[0]["verdict"], "ok")
        self.assertIn("better than the anchor by 20.0 %: accept it with perf_accept", rows[0]["note"])
        _, rows = perf.compare(history, self.night(6, t=96.0), 5)
        self.assertEqual(rows[0]["note"], "")
        # A record older than the applied accept has no effect, and says so.
        nights += [self.night(n, t=110.0) for n in (6, 7, 8, 9)] + [self.night(10, accepted=[self.accept(9, 110.0)], t=110.0)]
        history, _ = self.replay(nights)
        _, rows = perf.compare(history, self.night(11, accepted=[self.accept(8, 110.0)], t=110.0), 5)
        self.assertEqual(rows[0]["verdict"], "ok")
        self.assertIn(f"perf_accept for {self.day(8)} is older than the applied accept of {self.day(9)}", rows[0]["note"])

    def test_a_history_records_when_and_why_it_started(self):
        history, _ = perf.compare({}, self.night(1, t=1.0), 5, "Perf history restarted on request")
        self.assertEqual(history["started"], {"night": self.day(1), "note": "Perf history restarted on request"})
        history, rows = perf.compare(history, self.night(2, t=1.0), 5)
        self.assertEqual(history["started"]["night"], self.day(1))
        text = perf.markdown(rows, self.night(2, t=1.0), [], history["started"])
        self.assertIn(f"History since {self.day(1)} (Perf history restarted on request).", text)
        self.assertEqual(perf.compare({}, self.night(1, t=1.0), 5)[0]["started"]["note"], "first night")
        legacy = {"entries": [self.night(3, t=1.0)]}
        self.assertEqual(perf.compare(legacy, self.night(4, t=1.0), 5)[0]["started"], {"night": self.day(3), "note": ""})

    def test_only_an_accepted_night_moves_the_baseline(self):
        record = [self.accept(7, 110.0)]
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in (6, 7, 8)]
        nights += [self.night(9, accepted=record, t=110.0), self.night(10, accepted=record, t=122.0),
                   self.night(11, accepted=record, t=122.0), self.night(12, accepted=record, t=111.0)]
        _, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[5:]],
                         ["regression"] * 3 + ["ok", "regression", "regression", "ok"])
        eight, _ = self.replay(nights[:8])
        # A record for tonight's own night takes tonight's value as the new level, if it is the value accepted.
        _, rows = perf.compare(eight, self.night(9, accepted=[self.accept(9, 110.0)], t=110.0), 5)
        self.assertEqual(rows[0]["verdict"], "accepted")
        _, rows = perf.compare(eight, self.night(9, accepted=[self.accept(9, 110.0)], t=330.0), 5)
        self.assertEqual(rows[0]["verdict"], "accept-unmatched")
        self.assertIn("accepts 110.0, but that night measured 330", rows[0]["note"])
        # A record for another run, another metric or a later night changes nothing.
        for other in ({"run": "windows-vs2026"}, {"metric": "p"}, {"night": self.day(30)}):
            _, rows = perf.compare(eight, self.night(9, accepted=[self.accept(7, 110.0, **other)], t=110.0), 5)
            self.assertEqual(rows[0]["verdict"], "regression", other)

    def test_the_latest_accept_wins(self):
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in (6, 7, 8)]
        nights += [self.night(9, t=120.0)]
        history, _ = self.replay(nights)
        both = [self.accept(9, 120.0), self.accept(7, 110.0)]
        _, rows = perf.compare(history, self.night(10, accepted=both, t=120.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["baseline"], rows[0]["anchor"]), ("ok", 120.0, 120.0))

    def test_an_accept_for_a_night_without_the_value_fails(self):
        # Round 2's scenario: the record names night 7, which has no stored value (its nightly failed).
        nights = [self.night(n, t=100.0) for n in range(1, 6)] + [self.night(n, t=110.0) for n in (6, 8, 9)]
        history, _ = self.replay(nights)
        record = [self.accept(7, 110.0)]
        history, verdicts = self.replay([self.night(10, accepted=record, t=130.0),
                                         self.night(11, accepted=record, t=130.0)], history=history)
        self.assertEqual([v["t"] for v in verdicts], ["accept-unmatched"] * 2)
        _, rows = perf.compare(history, self.night(11, accepted=record, t=130.0), 5)
        self.assertIn(f"perf_accept for {self.day(7)} names a night with no stored value", rows[0]["note"])
        # 130 never became the baseline: with the record fixed or removed, it is still a regression against 100.
        _, rows = perf.compare(history, self.night(12, t=130.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["baseline"], rows[0]["anchor"]), ("regression", 100.0, 100.0))
        # A record whose value is not what that night measured fails the same way.
        _, rows = perf.compare(history, self.night(12, accepted=[self.accept(8, 150.0)], t=130.0), 5)
        self.assertEqual(rows[0]["verdict"], "accept-unmatched")

    def test_a_vanished_metric_stays_missing(self):
        nights = [self.night(n, t=1.0, p=2.0) for n in range(1, 6)]
        nights += [self.night(6, t=1.0), self.night(7, t=1.0), self.night(8, t=1.0)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v.get("p") for v in verdicts[5:]], ["missing", "missing", "missing"])
        self.assertEqual(history["entries"][-1]["missing"], ["linux-gcc/p"])
        _, rows = perf.compare(history, self.night(9, t=1.0, p=2.0), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows}["linux-gcc/p"], "ok")
        # Dropping the metric or its run, or moving the metric to another run, ends it.
        for tonight in (self.night(5, declared=("t",), t=1.0), self.night(5, runs=("linux-clang",), t=1.0),
                        self.night(5, runs=("linux-gcc", "linux-clang"), metric_runs={"p": "linux-clang"}, t=1.0)):
            _, rows = perf.compare(history, tonight, 5)
            self.assertNotIn("missing", [r["verdict"] for r in rows])
        _, rows = perf.compare(history, self.night(5, metric_runs={"p": "linux-gcc"}, t=1.0), 5)
        self.assertIn("missing", [r["verdict"] for r in rows])

    def test_a_metric_missing_past_the_history_limit_keeps_its_levels(self):
        # Round 3's nit 4: missing for more than the retained entries, then back 20 % worse.
        both = ("t", "b")
        nights = [self.night(n, declared=both, t=1.0, b=100.0) for n in range(1, 6)]
        nights += [self.night(n, declared=both, t=1.0) for n in range(6, 6 + perf.HISTORY_LIMIT + 5)]
        history, verdicts = self.replay(nights)
        self.assertEqual({v.get("b") for v in verdicts[5:]}, {"missing"})
        self.assertNotIn("linux-gcc/b", {k for e in history["entries"] for k in e["metrics"]})
        _, rows = perf.compare(history, self.night(80, declared=both, t=1.0, b=120.0), 5)
        row = next(r for r in rows if r["metric"] == "linux-gcc/b")
        self.assertEqual((row["verdict"], row["anchor"]), ("regression", 100.0))

    def test_a_declared_metric_never_measured_is_listed_not_failed(self):
        with tempfile.TemporaryDirectory() as d:
            entry, out = Path(d) / "e.json", Path(d) / "n.json"
            entry.write_text(json.dumps(self.night(1, declared=("t", "p", "q"), t=1.0)), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as text:
                rc = perf.main(["compare", "--entry", str(entry), "--out", str(out)])
        self.assertEqual(rc, 0)
        self.assertIn("| `p` | — ", text.getvalue())
        self.assertIn("never measured (declared, but no night has produced it yet)", text.getvalue())
        self.assertIn(f"Night {self.day(1)} (UTC", text.getvalue())

    def test_notes_head_the_summary(self):
        with tempfile.TemporaryDirectory() as d:
            entry, out = Path(d) / "e.json", Path(d) / "n.json"
            entry.write_text(json.dumps(self.night(1, t=1.0)), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as text:
                perf.main(["compare", "--entry", str(entry), "--out", str(out), "--note", "First night of the perf history"])
        self.assertTrue(text.getvalue().startswith("## Perf history\n\n**First night of the perf history**\n\nNight "))

    def test_require_history_refuses_to_reset_the_baselines(self):
        with tempfile.TemporaryDirectory() as d:
            entry, out, gone = Path(d) / "e.json", Path(d) / "n.json", Path(d) / "absent.json"
            entry.write_text(json.dumps(self.night(2, t=110.0)), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as text:
                rc = perf.main(["compare", "--entry", str(entry), "--history", str(gone), "--out", str(out),
                                "--require-history"])
            self.assertEqual(rc, 2)
            self.assertIn("perf history not found", text.getvalue())
            self.assertFalse(out.exists())
            # Without the flag (the first night), an absent history is a fresh start.
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(perf.main(["compare", "--entry", str(entry), "--history", str(gone), "--out", str(out)]), 0)

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

    # --- WP-0.3, the nightly of 2026-10-03: name the failing rows, keep levels per host class, and gate a
    # --- ratio such as RT-13's metering overhead against its plan bound.

    @staticmethod
    def on(cpu, e, run="linux-gcc"):
        """`e` as measured by a result set whose host.json names `cpu` (4 logical CPUs)."""
        fingerprint = {"cpu": cpu, "logical_cpus": 4}
        e["hosts"] = {run: dict(fingerprint, **{"class": runners.host_class(fingerprint)})}
        return e

    def test_extract_records_each_runs_host_fingerprint(self):
        with tempfile.TemporaryDirectory() as d:
            results = Path(d)
            for name, run in (("a", "linux-gcc"), ("b", "windows-vs2026"), ("c", "linux-go")):
                (results / name).mkdir()
                (results / name / "run.json").write_text(json.dumps({"run": run}), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as text:
                self.assertEqual(runners.main(["host", "--out", str(results / "a" / "host.json")]), 0)
            (results / "b" / "host.json").write_text(
                '{"cpu": "Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz", "logical_cpus": 4}', encoding="utf-8")
            entry = perf.extract(DATA, report.load_results(results, MODULE), "abc", "2026-09-26",
                                 perf.load_hosts(results))
        mine = runners.host_fingerprint()
        self.assertTrue(mine["cpu"] and mine["logical_cpus"] >= 1, mine)
        self.assertIn(f"host: {runners.host_class(mine)}", text.getvalue())
        self.assertEqual(entry["hosts"]["linux-gcc"], dict(mine, **{"class": runners.host_class(mine)}))
        self.assertEqual(perf.run_class(entry, "windows-vs2026"),
                         "Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz, 4 logical CPUs")
        self.assertEqual(perf.run_class(entry, "linux-go"), perf.UNRECORDED)  # no host.json
        cpuinfo = "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD EPYC 7763 64-Core Processor\n"
        self.assertEqual(runners._first_field(cpuinfo, ("model name",)), "AMD EPYC 7763 64-Core Processor")

    def test_each_host_class_keeps_its_own_anchor_and_calibration(self):
        a, b = "AMD EPYC 7763 64-Core Processor", "Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz"
        nights = [self.on(a, self.night(n, t=100.0)) for n in range(1, 6)]
        nights += [self.on(b, self.night(6, t=150.0))]  # another CPU: not a regression, B's first night
        nights += [self.on(a, self.night(7, t=110.0))]  # back on A: A's anchor still gates
        nights += [self.on(b, self.night(n, t=150.0)) for n in range(8, 12)]  # B calibrates on its own nights
        nights += [self.on(b, self.night(12, t=151.0)), self.on(b, self.night(13, t=170.0))]
        nights += [self.on(a, self.night(14, t=101.0))]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts], ["new"] + ["calibrating"] * 4 + ["new-host-class", "regression"] +
                         ["calibrating"] * 4 + ["ok", "regression", "ok"])
        stored = [e["metrics"]["linux-gcc/t"] for e in history["entries"]]
        self.assertEqual([m["anchor"] for m in stored[5:7]], [150.0, 100.0])
        self.assertEqual({m["host"] for m in stored}, {f"{a}, 4 logical CPUs", f"{b}, 4 logical CPUs"})
        self.assertEqual({c: v["anchor"] for c, v in history["levels"]["linux-gcc/t"].items()},
                         {f"{a}, 4 logical CPUs": 100.0, f"{b}, 4 logical CPUs": 150.0})
        _, rows = perf.compare(*self.replay(nights[:5])[:1], self.on(b, self.night(6, t=150.0)), 5)
        self.assertNotIn(rows[0]["verdict"], perf.FAILING)
        self.assertIn(f"first night on this host class ({b}, 4 logical CPUs)", rows[0]["note"])
        # Without host classes, night 6 would have been a 50 % regression against A's anchor.
        _, plain = self.replay([self.night(n, t=100.0) for n in range(1, 6)] + [self.night(6, t=150.0)])
        self.assertEqual(plain[-1]["t"], "regression")

    def test_a_class_keeps_its_levels_past_the_history_limit(self):
        a, b = "CPU A", "CPU B"
        nights = [self.on(a, self.night(n, t=100.0)) for n in range(1, 6)]
        nights += [self.on(b, self.night(n, t=150.0)) for n in range(6, 6 + perf.HISTORY_LIMIT + 5)]
        history, _ = self.replay(nights)
        self.assertNotIn(f"{a}, 4 logical CPUs", {e["metrics"]["linux-gcc/t"]["host"] for e in history["entries"]})
        _, rows = perf.compare(history, self.on(a, self.night(80, t=110.0)), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor"]), ("regression", 100.0))
        _, rows = perf.compare(history, self.on(a, self.night(80, t=100.0)), 5)
        self.assertEqual(rows[0]["verdict"], "ok")

    def test_an_accept_moves_the_level_of_its_nights_class_only(self):
        a, b = "CPU A", "CPU B"
        nights = [self.on(a, self.night(n, t=100.0)) for n in range(1, 6)]
        nights += [self.on(b, self.night(n, t=200.0)) for n in range(6, 11)]
        nights += [self.on(a, self.night(11, t=110.0))]
        history, verdicts = self.replay(nights)
        self.assertEqual(verdicts[-1]["t"], "regression")
        record = [self.accept(11, 110.0)]
        history, verdicts = self.replay([self.on(b, self.night(12, accepted=record, t=200.0)),
                                         self.on(a, self.night(13, accepted=record, t=110.0)),
                                         self.on(b, self.night(14, accepted=record, t=200.0))], history=history)
        self.assertEqual([v["t"] for v in verdicts], ["ok", "ok", "ok"])
        self.assertEqual([e["metrics"]["linux-gcc/t"]["anchor"] for e in history["entries"][-3:]], [200.0, 110.0, 200.0])

    def test_a_history_from_before_fingerprints_starts_each_class_without_failing(self):
        legacy, _ = self.replay([self.night(n, t=100.0, p=50.0) for n in range(1, 7)])
        for e in legacy["entries"]:  # as the nightly wrote it before host fingerprints
            for m in e["metrics"].values():
                del m["host"]
        del legacy["levels"]
        _, rows = perf.compare(legacy, self.on("CPU A", self.night(7, t=300.0, p=10.0)), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows},
                         {"linux-gcc/t": "new-host-class", "linux-gcc/p": "new-host-class"})
        # A night whose result set has no fingerprint is still compared with the levels from before.
        _, rows = perf.compare(legacy, self.night(7, t=120.0, p=50.0), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows},
                         {"linux-gcc/t": "regression", "linux-gcc/p": "ok"})

    def test_a_single_class_history_gives_the_verdicts_it_gave_before(self):
        record = [self.accept(9, 104.0)]
        series = [100.0, 94.0, 150.0, 100.0, 101.0, 103.0, 106.0, None, 104.0, 104.0, 109.0, 99.0, 104.0, 112.0]

        def nights(cpu):
            out = []
            for n, t in enumerate(series, 1):
                e = self.night(n, accepted=record if n >= 10 else (), p=50.0 + n, **({} if t is None else {"t": t}))
                out.append(self.on(cpu, e) if cpu else e)
            return out

        plain, before = self.replay(nights(None))
        hosted, after = self.replay(nights("CPU A"))
        # What perf.py gave for this series before host classes (main at 803ac23), with or without fingerprints.
        self.assertEqual([v.get("t") for v in before], ["new"] + ["calibrating"] * 4 +
                         ["ok", "regression", "missing", "ok", "ok", "ok", "ok", "ok", "regression"])
        self.assertEqual([v.get("p") for v in before], ["new"] + ["calibrating"] * 4 + ["ok"] * 9)
        self.assertEqual(before, after)
        levels = (lambda h: [{k: {f: m.get(f) for f in perf.LEVELS + ("verdict",)} for k, m in e["metrics"].items()}
                             for e in h["entries"]])
        self.assertEqual(levels(plain), levels(hosted))

    def bounded(self, n, value, cpu="CPU A"):
        e = self.on(cpu, self.night(n, t=value))
        e["metrics"]["linux-gcc/t"].update(bound=10, unit="%")
        return e

    def test_a_bound_metric_is_gated_against_its_bound_on_every_night_and_class(self):
        # RT-13's overhead: a 2-8 % difference of two timings, so a 5 % relative budget (0.2 points) is noise.
        values = [4.0, 4.0, 4.0, 4.0, 4.0, 8.0, -0.2, 10.0, 10.5]
        nights = [self.bounded(n, v) for n, v in enumerate(values, 1)]
        nights += [self.bounded(10, 4.0, "CPU B"), self.bounded(11, 11.0, "CPU C")]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts], ["new"] + ["calibrating"] * 4 + ["ok", "ok", "ok", "regression"] +
                         ["new-host-class", "regression"])
        _, rows = perf.compare(*self.replay(nights[:5])[:1], nights[5], 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor_change"]), ("ok", 100.0))
        self.assertIn("within its bound; drift against the anchor is reported, not gated", rows[0]["note"])
        _, rows = perf.compare({}, self.bounded(1, 12.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["note"]), ("regression", "worse than its bound of ≤ 10 %"))
        self.assertIn("| ≤ 10 % (bound) | **regression** (worse than its bound of ≤ 10 %) |",
                      perf.markdown(rows, self.bounded(1, 12.0)))
        # Without the bound, the 8 % night is a 100 % regression of the anchor.
        plain = [self.on("CPU A", self.night(n, t=v)) for n, v in enumerate(values[:6], 1)]
        self.assertEqual(self.replay(plain)[1][-1]["t"], "regression")

    def test_the_registry_reads_signed_overheads_and_three_decimal_core_counts(self):
        data, _ = sc.load_jsonc(sc.ROOT / "scorecard.jsonc")
        metrics = {m["id"]: m for m in data["perf_metrics"]}
        overhead = metrics["script.fuel_metering_overhead_pct"]
        self.assertEqual((overhead["bound"], overhead["criterion"], overhead["unit"]), (10, "RT-13", "%"))
        run = report.Results("linux-gcc")
        line = ("workload: 2400000 fuel (2398710 raw safepoints); raw 8120 us, raw+callback per safepoint 9300 us "
                "(14.5 %), raw+callback every 64 8301 us (2.2 %), Helios 8300 us (metering overhead {} %), ns/fuel 3.45")
        for printed, value in (("4.12", 4.12), ("-0.19", -0.19), ("-3", -3.0), ("1.5e-05", 1.5e-05),
                               ("-2.5e-06", -2.5e-06), ("7", 7.0)):
            run.messages[(overhead["doctest"], overhead["case"])] = [line.format(printed)]
            self.assertEqual(report.metric_values(overhead, [run]), {"linux-gcc": value}, printed)
        cores = metrics["net.ns07.trunk_cell_cores"]
        run.gates["net_bench_gate"] = ("pass", 600.0, "NS-0.7 trunk: 600 s, sent 11999995, delivered 11999995 "
                                       "(20000 pps, 190.1 Mbit/s payload, 199.7 Mbit/s wire), drops 0.0000 %, "
                                       "cell thread 0.245 cores, gateway thread 0.231 cores, rcvbuf 4096 KB")
        self.assertEqual(report.metric_values(cores, [run]), {"linux-gcc": 0.245})

    def test_each_failing_row_is_annotated_for_the_run_page(self):
        history, _ = self.replay([self.night(n, t=100.0, p=2.0) for n in range(1, 6)])
        with tempfile.TemporaryDirectory() as d:
            hist, entry, out = Path(d) / "h.json", Path(d) / "e.json", Path(d) / "n.json"
            hist.write_text(json.dumps(history), encoding="utf-8")
            for tonight, rc_want in ((self.night(6, t=119.5), 1), (self.night(6, t=101.0, p=2.0), 0)):
                entry.write_text(json.dumps(tonight), encoding="utf-8")
                with contextlib.redirect_stdout(io.StringIO()) as text:
                    rc = perf.main(["compare", "--entry", str(entry), "--history", str(hist), "--out", str(out)])
                commands = [line for line in text.getvalue().splitlines() if line.startswith("::")]
                self.assertEqual(rc, rc_want)
                if rc:
                    self.assertEqual(commands, [
                        "::error title=perf regression::linux-gcc/t 119.5 u vs anchor 100 (+19.5 %25)",
                        "::error title=perf missing::linux-gcc/p: no value tonight"])
                    self.assertIn("| `linux-gcc/t` | 119.5 u |", text.getvalue())  # the table too
                else:
                    self.assertEqual(commands, [])
        # Workflow-command escaping: data and properties cannot break out of the command.
        self.assertEqual(perf._command_data("50 %\r\n::x"), "50 %25%0D%0A::x")
        self.assertEqual(perf._command_property("perf a:b,c"), "perf a%3Ab%2Cc")
        rows = [{"metric": "linux-gcc/t", "value": 12.0, "unit": "%", "anchor": 4.0, "anchor_change": 200.0,
                 "verdict": "regression", "note": "worse than its bound of ≤ 10 %", "bound": 10, "better": "lower"}]
        self.assertEqual(perf.annotations(rows), ["::error title=perf regression::linux-gcc/t 12 %25 vs anchor 4 "
                                                  "(+200.0 %25), bound ≤ 10 %25: worse than its bound of ≤ 10 %25"])

    # --- PR #44 review round 1.

    def test_a_rare_class_finishes_its_calibration(self):
        # The runner lands on this class every 16th night, so its 5 calibration nights span more than 60 entries.
        nights = [self.on("RARE" if n % 16 == 0 else "COMMON", self.night(n, t=100.0)) for n in range(1, 160)]
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[15::16]][:6], ["new-host-class"] + ["calibrating"] * 4 + ["ok"])
        _, rows = perf.compare(history, self.on("RARE", self.night(160, t=150.0)), 5)
        self.assertEqual(rows[0]["verdict"], "regression")  # +50 % on the rare class
        # The store keeps a class's calibration values only while it calibrates.
        rare = history["levels"]["linux-gcc/t"]["RARE, 4 logical CPUs"]
        self.assertEqual((rare["anchor"], rare["anchor_n"]), (100.0, 5))
        self.assertNotIn("calibration", rare)
        # Every 30th night: the anchor is the median of the class's first 5 nights, not of the ones still held.
        values = {30: 100.0, 60: 300.0, 90: 101.0, 120: 99.0, 150: 102.0, 180: 400.0}
        nights = [self.on("RARE" if n in values else "COMMON", self.night(n, t=values.get(n, 50.0)))
                  for n in range(1, 181)]
        history, verdicts = self.replay(nights)
        self.assertEqual([verdicts[n - 1]["t"] for n in values],
                         ["new-host-class"] + ["calibrating"] * 4 + ["regression"])
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["anchor"], 101.0)
        self.assertEqual(history["levels"]["linux-gcc/t"]["COMMON, 4 logical CPUs"]["anchor"], 50.0)
        # A class whose entries have all left the history still has its levels: the runner's return to it is
        # no first night on a class (no warning) and does not count towards host churn.
        nights = [self.on("RARE" if n in (1, 70) else "COMMON", self.night(n, t=100.0)) for n in range(1, 71)]
        history, verdicts = self.replay(nights)
        held = {perf.run_class(e, "linux-gcc") for e in history["entries"][:-1]}
        self.assertNotIn("RARE, 4 logical CPUs", held)
        self.assertEqual((verdicts[-1]["t"], history["entries"][-1]["new_class_runs"]), ("calibrating", []))

    def test_a_class_in_calibration_keeps_its_values_in_the_store(self):
        history, _ = self.replay([self.on("CPU A", self.night(n, t=100.0 + n)) for n in range(1, 4)])
        self.assertEqual(history["levels"]["linux-gcc/t"]["CPU A, 4 logical CPUs"]["calibration"],
                         [101.0, 102.0, 103.0])
        # A bound breach is not a clean value: it is left out of the calibration.
        history, verdicts = self.replay([self.bounded(1, 4.0), self.bounded(2, 12.0), self.bounded(3, 5.0)])
        self.assertEqual([v["t"] for v in verdicts], ["new", "regression", "ok"])
        self.assertEqual(history["levels"]["linux-gcc/t"]["CPU A, 4 logical CPUs"]["calibration"], [4.0, 5.0])

    def test_the_nightly_extract_carries_the_registry_bound_to_compare(self):
        data, _ = sc.load_jsonc(sc.ROOT / "scorecard.jsonc")
        overhead = next(m for m in data["perf_metrics"] if m["id"] == "script.fuel_metering_overhead_pct")
        key, history = f"{overhead['run']}/{overhead['id']}", {}
        with tempfile.TemporaryDirectory() as d:
            results = Path(d)
            (results / "a").mkdir()
            (results / "a" / "run.json").write_text(json.dumps({"run": overhead["run"]}), encoding="utf-8")
            (results / "a" / "host.json").write_text('{"cpu": "CPU A", "logical_cpus": 4}', encoding="utf-8")
            for n, pct in enumerate((4.0, 4.0, 4.0, 4.0, 4.0, 8.0, 10.5), 1):
                message = f"raw 8120 us, Helios 8300 us (metering overhead {pct:g} %), ns/fuel 3.45"
                doctest_xml(results / "a" / "doctest" / f"{overhead['doctest']}.perf.xml",
                            [(overhead["case"], True, message)])
                entry = perf.extract(data, report.load_results(results, MODULE), f"sha{n}",
                                     self.day(n) + "T03:17:00Z", perf.load_hosts(results))
                self.assertEqual((entry["metrics"][key]["value"], entry["metrics"][key]["bound"]), (pct, 10))
                history, rows = perf.compare(history, entry, 5)
                row = next(r for r in rows if r["metric"] == key)
                if n == 6:  # +100 % against the 4 % anchor, but within RT-13's bound
                    self.assertEqual((row["verdict"], row["anchor_change"]), ("ok", 100.0))
        self.assertEqual((row["verdict"], row["note"]), ("regression", "worse than its bound of ≤ 10 %"))

    def test_no_table_line_can_become_a_workflow_command(self):
        history, _ = self.replay([self.night(n, t=100.0) for n in range(1, 6)])
        reason = "faster allocator\n::error::x\r::warning file=a::y"
        tonight = self.night(6, accepted=[self.accept(6, 80.0, reason=reason)], t=80.0)
        _, rows = perf.compare(history, tonight, 5)
        self.assertEqual(rows[0]["verdict"], "accepted")
        text = perf.markdown(rows, tonight)
        self.assertIn("| accepted (perf_accept for", text)
        self.assertEqual([line for line in text.splitlines() if line.lstrip().startswith("::")], [])
        self.assertEqual(perf.annotations(rows), [])

    def test_a_missing_night_keeps_the_class_of_its_levels(self):
        nights = [self.on("CPU A", self.night(n, t=100.0, p=50.0)) for n in range(1, 6)]
        nights += [self.on("CPU A", self.night(6, p=50.0))]  # t is missing tonight
        history, verdicts = self.replay(nights)
        self.assertEqual(verdicts[-1]["t"], "missing")
        self.assertEqual(history["entries"][-1]["missing_levels"]["linux-gcc/t"]["host"], "CPU A, 4 logical CPUs")
        # So a night without host.json is not compared with CPU A's levels.
        _, rows = perf.compare(history, self.night(7, t=150.0, p=50.0), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows},
                         {"linux-gcc/t": "new-host-class", "linux-gcc/p": "new-host-class"})

    def test_a_legacy_historys_levels_outlive_its_entries(self):
        legacy, _ = self.replay([self.night(n, t=100.0) for n in range(1, 7)])
        for e in legacy["entries"]:  # as the nightly wrote it before host fingerprints
            for m in e["metrics"].values():
                del m["host"]
        del legacy["levels"]
        legacy["version"] = 1
        history, verdicts = self.replay([self.on("CPU A", self.night(n, t=300.0)) for n in range(7, 7 + 61)],
                                        history=legacy)
        self.assertEqual(verdicts[0]["t"], "new-host-class")
        self.assertNotIn(perf.UNRECORDED, {e["metrics"]["linux-gcc/t"]["host"] for e in history["entries"]})
        # A result set without host.json is still held to the levels from before fingerprints.
        _, rows = perf.compare(history, self.night(70, t=120.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor"]), ("regression", 100.0))

    def test_a_redeclared_metric_keeps_its_levels_past_the_history_limit(self):
        nights = [self.on("CPU A", self.night(n, t=100.0, p=50.0)) for n in range(1, 6)]
        nights += [self.on("CPU A", self.night(6, declared=("p",), p=50.0))]  # dropped from the registry
        # Declared again but not produced: it was not missing last night, so it is not missing now.
        nights += [self.on("CPU A", self.night(n, p=50.0)) for n in range(7, 7 + perf.HISTORY_LIMIT)]
        history, verdicts = self.replay(nights)
        self.assertNotIn("t", verdicts[-1])
        self.assertNotIn("linux-gcc/t", {k for e in history["entries"] for k in e["metrics"]})
        _, rows = perf.compare(history, self.on("CPU A", self.night(70, t=120.0, p=50.0)), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows}, {"linux-gcc/t": "regression", "linux-gcc/p": "ok"})

    def test_a_bounded_metric_is_not_offered_a_perf_accept(self):
        history, _ = self.replay([self.bounded(n, 4.0) for n in range(1, 6)])
        _, rows = perf.compare(history, self.bounded(6, 1.0), 5)
        self.assertEqual((rows[0]["verdict"], rows[0]["anchor_change"]), ("ok", -75.0))
        self.assertNotIn("perf_accept", rows[0]["note"])
        plain, _ = self.replay([self.on("CPU A", self.night(n, t=4.0)) for n in range(1, 6)])
        _, rows = perf.compare(plain, self.on("CPU A", self.night(6, t=1.0)), 5)
        self.assertIn("accept it with perf_accept", rows[0]["note"])

    def test_the_hosts_line_counts_each_runs_host_classes(self):
        nights = [self.on(cpu, self.night(n, t=100.0)) for n, cpu in enumerate(("CPU A", "CPU B", "CPU C", "CPU B"), 1)]
        history, rows = perf.compare(self.replay(nights[:-1])[0], nights[-1], 5)
        self.assertIn("Hosts: `linux-gcc` on CPU B, 4 logical CPUs (3 host classes with levels).",
                      perf.markdown(rows, nights[-1], levels=history["levels"]))
        history, rows = perf.compare({}, nights[0], 5)
        self.assertIn("Hosts: `linux-gcc` on CPU A, 4 logical CPUs (1 host class with levels).",
                      perf.markdown(rows, nights[0], levels=history["levels"]))

    def test_failing_rows_of_a_history_from_before_they_were_named_are_listed_once(self):
        old, _ = self.replay([self.night(n, t=100.0, p=50.0) for n in range(1, 6)] +
                             [self.night(6, t=130.0, p=50.0), self.night(7, t=100.0, p=50.0), self.night(8, t=100.0)])
        for e in old["entries"]:  # as perf.py wrote it before host fingerprints (history version 1)
            for m in e["metrics"].values():
                del m["host"]
        del old["levels"]
        old["version"] = 1
        lines = perf.unnamed_failures(old)
        self.assertEqual(lines, [
            f"{self.day(6)}, commit `sha6`: `linux-gcc/t` regression (130 u vs anchor 100, +30.0 %)",
            f"{self.day(8)}, commit `sha8`: `linux-gcc/p` missing"])
        with tempfile.TemporaryDirectory() as d:
            hist, entry, out = Path(d) / "h.json", Path(d) / "e.json", Path(d) / "n.json"
            hist.write_text(json.dumps(old), encoding="utf-8")
            entry.write_text(json.dumps(self.on("CPU A", self.night(9, t=100.0, p=50.0))), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as text:
                rc = perf.main(["compare", "--entry", str(entry), "--history", str(hist), "--out", str(out)])
            self.assertEqual(rc, 0)
            for line in lines:
                self.assertIn(f"\n- {line}\n", text.getvalue())
            # The new history names its own failing rows, so the list is not repeated on later nights.
            self.assertEqual(perf.unnamed_failures(json.loads(out.read_text(encoding="utf-8"))), [])

    # --- PR #44 review round 2.

    def test_a_fingerprint_that_never_repeats_does_not_stay_green(self):
        # Every night lands on a class the history has never seen: no class calibrates, so nothing is gated.
        nights = [self.on(f"CPU rev {n}", self.night(n, t=100.0 if n < 20 else 150.0)) for n in range(1, 40)]
        history, verdicts = self.replay(nights)
        self.assertTrue(any(v["t"] in perf.FAILING for v in verdicts), sorted({v["t"] for v in verdicts}))
        # More than --window classes without a second night: from the 6th night on, every night fails.
        self.assertEqual([v["t"] for v in verdicts], ["new"] + ["new-host-class"] * 4 + ["host-churn"] * 34)
        self.assertEqual(history["entries"][-1]["metrics"]["linux-gcc/t"]["verdict"], "host-churn")
        self.assertEqual(history["entries"][-1]["new_class_runs"], ["linux-gcc"])
        # A churning night's value is clean: the store keeps it for its class's calibration.
        self.assertEqual(history["levels"]["linux-gcc/t"]["CPU rev 39, 4 logical CPUs"]["calibration"], [150.0])

    def test_host_churn_counts_only_classes_the_runner_has_not_come_back_to(self):
        def verdicts(cpus, **values):
            return [v["t"] for v in self.replay([self.on(cpu, self.night(n, **(values or {"t": 100.0})))
                                                 for n, cpu in enumerate(cpus, 1)])[1]]

        # A stable pool of up to --window CPU models never gets there; with 6, only while none has come back.
        five, six = [f"CPU {n % 5}" for n in range(100)], [f"CPU {n % 6}" for n in range(100)]
        self.assertNotIn("host-churn", verdicts(five))
        self.assertEqual([n for n, v in enumerate(verdicts(six), 1) if v == "host-churn"], [6])
        # Under churn, a night that a level gated keeps its verdict (`ok` here) and only ungated rows fail.
        cpus = ["CPU A"] * 5 + [f"CPU rev {n}" for n in range(6)] + ["CPU A"]
        self.assertEqual(verdicts(cpus), ["new"] + ["calibrating"] * 4 + ["new-host-class"] * 5 + ["host-churn"] +
                         ["ok"])
        # The churning nights still calibrate their classes: the class the runner settles on gates from its 6th.
        cpus = [f"CPU rev {n}" for n in range(10)] + ["CPU Z"] * 7
        _, rows = perf.compare(self.replay([self.on(c, self.night(n, t=100.0)) for n, c in enumerate(cpus, 1)])[0],
                               self.on("CPU Z", self.night(18, t=150.0)), 5)
        self.assertEqual(verdicts(cpus)[-7:], ["host-churn"] * 5 + ["ok", "ok"])
        self.assertEqual(rows[0]["verdict"], "regression")
        # A metric with a bound is gated on every class, so churn does not fail it; nor a metric that is not gated.
        nights = [self.bounded(n, 4.0, f"CPU rev {n}") for n in range(1, 9)]
        self.assertEqual([v["t"] for v in self.replay(nights)[1]], ["new"] + ["new-host-class"] * 7)
        nights = [self.on(f"CPU rev {n}", self.night(n, d=1.0)) for n in range(1, 9)]
        self.assertEqual([v["d"] for v in self.replay(nights)[1]], ["new"] + ["new-host-class"] * 7)

    def test_a_new_host_class_is_a_warning_on_the_run_page_and_churn_one_error(self):
        with tempfile.TemporaryDirectory() as d:
            hist, entry, out = Path(d) / "h.json", Path(d) / "e.json", Path(d) / "n.json"

            def night(e):
                if out.is_file():
                    hist.write_text(out.read_text(encoding="utf-8"), encoding="utf-8")
                entry.write_text(json.dumps(e), encoding="utf-8")
                with contextlib.redirect_stdout(io.StringIO()) as text:
                    rc = perf.main(["compare", "--entry", str(entry), "--history", str(hist), "--out", str(out)])
                return rc, [line for line in text.getvalue().splitlines() if line.startswith("::")], text.getvalue()

            rc, commands, text = night(self.on("CPU A", self.night(1, t=100.0, p=5.0)))
            self.assertEqual((rc, commands), (0, ["::warning title=perf new-host-class::linux-gcc on CPU A, "
                                                  "4 logical CPUs (1 host class with levels)"]))
            rc, commands, text = night(self.on("CPU B", self.night(2, t=100.0, p=5.0)))
            self.assertEqual((rc, commands), (0, ["::warning title=perf new-host-class::linux-gcc on CPU B, "
                                                  "4 logical CPUs (2 host classes with levels)"]))
            self.assertIn("Hosts: `linux-gcc` on CPU B, 4 logical CPUs (first night on this class; 2 host classes "
                          "with levels).", text)
            rc, commands, text = night(self.on("CPU A", self.night(3, t=100.0, p=5.0)))  # a class with levels
            self.assertEqual((rc, commands), (0, []))
            self.assertIn("(2 host classes with levels).", text)
            for n in range(4, 8):
                rc, commands, text = night(self.on(f"CPU rev {n}", self.night(n, t=100.0, p=5.0)))
                self.assertEqual(rc, 0)  # CPU B and up to 4 new classes without a second night: not more than 5
            rc, commands, text = night(self.on("CPU rev 8", self.night(8, t=100.0, p=5.0)))
            self.assertEqual(rc, 1)
            self.assertEqual(commands, [
                "::warning title=perf new-host-class::linux-gcc on CPU rev 8, 4 logical CPUs (7 host classes with "
                "levels)",
                "::error title=perf host-churn::linux-gcc: 2 gated metric(s) not gated tonight. host churn: 6 host "
                "classes of linux-gcc had their first night in the last 8 nights and none has had another, more "
                "than --window 5, so its levels cannot calibrate (is its host fingerprint stable?)"])
            self.assertIn("**Host churn: 6 host classes of linux-gcc", text)
            self.assertIn("| **host-churn** (host churn on linux-gcc; tonight new-host-class (first night on this "
                          "host class (CPU rev 8, 4 logical CPUs)", text)

    def test_a_higher_is_better_bound_fails_only_below_it(self):
        def floor(n, value):
            e = self.on("CPU A", self.night(n, p=value))
            e["metrics"]["linux-gcc/p"].update(bound=1000, unit="pps")
            return e

        values = [1200.0] * 5 + [999.0, 1000.0, 3000.0]
        history, verdicts = self.replay([floor(n, v) for n, v in enumerate(values, 1)])
        self.assertEqual([v["p"] for v in verdicts], ["new"] + ["calibrating"] * 4 + ["regression", "ok", "ok"])
        self.assertEqual(history["entries"][5]["metrics"]["linux-gcc/p"]["verdict"], "regression")
        _, rows = perf.compare(self.replay([floor(n, 1200.0) for n in range(1, 6)])[0], floor(6, 999.0), 5)
        self.assertEqual(rows[0]["note"], "worse than its bound of ≥ 1000 pps")
        self.assertIn("| ≥ 1000 pps (bound) |", perf.markdown(rows, floor(6, 999.0)))

    def test_no_summary_line_can_become_a_workflow_command_or_split_a_table_row(self):
        # The earlier-failures list holds stored keys, units and SHAs; the notes hold CPU model names.
        old = {"version": 1, "entries": [{"date": self.day(1), "sha": "sha1\n::error::x", "metrics": {
            "linux-gcc/t": {"value": 130.0, "unit": "u\r\n::warning::y", "anchor": 100.0, "verdict": "regression",
                            "better": "lower"}}}]}
        earlier = perf.unnamed_failures(old)
        self.assertEqual(len(earlier), 1)
        history, _ = self.replay([self.on("CPU A", self.night(n, t=100.0)) for n in range(1, 6)])
        tonight = self.on("CPU | B", self.night(6, t=150.0))
        _, rows = perf.compare(history, tonight, 5)
        self.assertEqual(rows[0]["verdict"], "new-host-class")
        text = perf.markdown(rows, tonight, earlier=earlier)
        self.assertEqual([line for line in text.splitlines() if line.lstrip().startswith("::")], [])
        self.assertIn("- 2026-01-02, commit `sha1 ::error`: `linux-gcc/t` regression (130 u ::warning::y vs", text)
        table = [line for line in text.splitlines() if line.startswith("| `")]
        self.assertEqual([len(re.findall(r"(?<!\\)\|", line)) for line in table], [9, 9])  # t, and p never measured
        self.assertIn("(CPU \\| B, 4 logical CPUs)", table[0])

    # --- PR #44 review round 3.

    def test_classes_that_each_recur_a_few_times_do_not_stay_green(self):
        # The fingerprint changes every 2nd night: each class comes back once, so none stays unreturned, and
        # none reaches its 5 calibration nights. +50 % from night 60.
        nights = [self.on(f"CPU rev {n // 2}", self.night(n, t=100.0 if n < 60 else 150.0)) for n in range(1, 151)]
        history, verdicts = self.replay(nights)
        self.assertTrue(any(v["t"] in perf.FAILING for v in verdicts), sorted({v["t"] for v in verdicts}))
        # No night of the run's last 60 had a row that a level gated: every night from the 60th fails.
        failing = [n for n, v in enumerate(verdicts, 1) if v["t"] in perf.FAILING]
        self.assertEqual(failing, list(range(60, 151)))
        self.assertEqual({v["t"] for v in verdicts[59:]}, {"host-churn"})
        self.assertEqual([e["ungated_nights"] for e in history["entries"][-2:]],
                         [{"linux-gcc": 149}, {"linux-gcc": 150}])
        # Classes of 3 or 4 nights each, and pairs of classes of 2 to 4 nights each, interleaved: the same.
        interleaved = []
        for i in range(40):
            a, b, na, nb = f"CPU {i}a", f"CPU {i}b", 2 + i % 3, 2 + (i + 1) % 3
            interleaved += [a, b] * min(na, nb) + [a if na > nb else b] * abs(na - nb)
        for cpus in ([f"CPU rev {n // 3}" for n in range(1, 151)],
                     [f"CPU rev {n // 4}" for n in range(1, 151)], interleaved[:150]):
            verdicts = self.replay([self.on(cpu, self.night(n, t=100.0)) for n, cpu in enumerate(cpus, 1)])[1]
            self.assertEqual([n for n, v in enumerate(verdicts, 1) if v["t"] in perf.FAILING], failing)

    def test_a_run_fails_its_ungated_rows_after_a_full_history_without_a_gated_night(self):
        def churn(n):
            return f"CPU rev {n // 2}"  # 2 nights per class: each comes back once, and none calibrates

        def night(n, cpu, **values):
            e = self.on(cpu, self.night(n, **values))
            if "b" in values:
                e["metrics"]["linux-gcc/b"].update(bound=10, unit="%")
            return e

        # Class A calibrates on nights 1-5 and gates night 6; then 60 churning nights, A, 60 more, A. `p` is
        # first produced on night 66, so it reads `new` there; `b` has a bound and `d` is not gated, so
        # neither fails, and `b`'s bound breach on night 30 is not a night that a level gated.
        cpus = ["CPU A"] * 6 + [churn(n) for n in range(7, 67)] + ["CPU A"]
        cpus += [churn(n) for n in range(68, 128)] + ["CPU A"]
        nights = [night(n, cpu, t=100.0, b=12.0 if n == 30 else 4.0, d=1.0, **({"p": 5.0} if n >= 66 else {}))
                  for n, cpu in enumerate(cpus, 1)]
        history, verdicts = self.replay(nights)
        self.assertEqual([n for n, v in enumerate(verdicts, 1) if "host-churn" in v.values()], [66, 127])
        self.assertEqual(verdicts[29]["b"], "regression")
        self.assertEqual(verdicts[65], {"t": "host-churn", "p": "host-churn", "b": "new-host-class",
                                        "d": "new-host-class"})
        self.assertEqual((verdicts[66]["t"], verdicts[127]["t"]), ("ok", "ok"))  # a gated night: count 0
        self.assertEqual([e["ungated_nights"]["linux-gcc"] for e in history["entries"][-3:]], [59, 60, 0])
        _, rows = perf.compare(self.replay(nights[:65])[0], nights[65], 5)
        self.assertEqual(perf.annotations(rows), [
            "::error title=perf host-churn::linux-gcc: 2 gated metric(s) not gated tonight. host churn: "
            "none of the last 60 nights of linux-gcc had a metric that a level gated, a full history (60) or "
            "more, so its host classes do not stay long enough to calibrate (is its host fingerprint "
            "stable?)"])
        # A history written before the count was stored: it is counted from the stored verdicts, back to the
        # last night that a level gated (night 6 here).
        legacy, _ = self.replay(nights[:64])
        for e in legacy["entries"]:
            del e["ungated_nights"]
        _, rows = perf.compare(legacy, nights[64], 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows}["linux-gcc/t"], "calibrating")  # 59 nights
        legacy, _ = self.replay([self.on(churn(n), self.night(n, t=100.0)) for n in range(1, 60)])
        for e in legacy["entries"]:
            del e["ungated_nights"]
        _, rows = perf.compare(legacy, self.on(churn(60), self.night(60, t=100.0)), 5)
        self.assertEqual(rows[0]["verdict"], "host-churn")
        # Nights on which the run measured no gated metric without a bound do not count: a metric added after
        # 70 of them reads `new`.
        nights = [night(n, churn(n), b=4.0, d=1.0, **({"t": 100.0} if n == 71 else {})) for n in range(1, 72)]
        history, verdicts = self.replay(nights)
        self.assertEqual(verdicts[-1]["t"], "new")
        self.assertEqual(history["entries"][-1]["ungated_nights"], {"linux-gcc": 1})
        # A regression is a night that a level gated: 60 of them, then a new metric, which reads `new`.
        nights = [self.on("CPU A", self.night(n, t=100.0 if n <= 5 else 150.0, **({"p": 5.0} if n == 66 else {})))
                  for n in range(1, 67)]
        history, verdicts = self.replay(nights)
        self.assertEqual(verdicts[-1], {"t": "regression", "p": "new"})
        self.assertEqual(history["entries"][-1]["ungated_nights"], {"linux-gcc": 0})
        # Nights on which the run measured nothing do not reset the count, which outlives the entries: with 3
        # such nights, the run's 60th night without a gated row is the 63rd of the history.
        nights = [self.on(churn(n), self.night(n, **({} if n in (20, 30, 40) else {"t": 100.0})))
                  for n in range(1, 71)]
        verdicts = self.replay(nights)[1]
        self.assertEqual([n for n, v in enumerate(verdicts, 1) if v["t"] in perf.FAILING],
                         [20, 30, 40] + list(range(63, 71)))
        self.assertEqual({verdicts[n - 1]["t"] for n in (20, 30, 40)}, {"missing"})

    def test_host_churn_counts_classes_within_the_history_and_nights_the_run_measured(self):
        def verdicts(cpus, skip=()):
            nights = [self.on(cpu, self.night(n, **({} if n in skip else {"t": 100.0})))
                      for n, cpu in enumerate(cpus, 1)]
            return [v["t"] for v in self.replay(nights)[1]]

        # A class counts while its first night is among the history's 60 entries, tonight's included.
        cpus = ["CPU X0"] + ["CPU A"] * 54 + [f"CPU X{k}" for k in range(1, 6)]  # X0 is the 60th entry back
        self.assertEqual([n for n, v in enumerate(verdicts(cpus), 1) if v == "host-churn"], [60])
        cpus = ["CPU X0"] + ["CPU A"] * 55 + [f"CPU X{k}" for k in range(1, 6)]  # X0 is the 61st entry back
        self.assertNotIn("host-churn", verdicts(cpus))
        # A night on which the run measured nothing (a job that wrote host.json and crashed) is no return.
        cpus = ["CPU X1", "CPU X2", "CPU X3", "CPU X1", "CPU X4", "CPU X5", "CPU X6"]
        self.assertEqual(verdicts(cpus, skip=(4,)), ["new"] + ["new-host-class"] * 2 + ["missing"] +
                         ["new-host-class"] * 2 + ["host-churn"])

    def test_the_new_host_class_warning_escapes_its_data(self):
        # host.json is read as written, so its CPU model can hold anything.
        new, rows = perf.compare({}, self.on("CPU 100%\r\n::error::x", self.night(1, t=100.0)), 5)
        self.assertEqual(perf.annotations(rows, new["entries"][-1], new["levels"]),
                         ["::warning title=perf new-host-class::linux-gcc on CPU 100%25%0D%0A::error::x, "
                          "4 logical CPUs (1 host class with levels)"])

    # --- PR #44 review round 4 (nits 2 to 4).

    def test_no_hosts_line_becomes_a_workflow_command(self):
        tonight = self.on("CPU\n::stop-commands::x", self.night(1, t=100.0))
        new, rows = perf.compare({}, tonight, 5)
        text = perf.markdown(rows, new["entries"][-1], levels=new["levels"])
        self.assertEqual([l for l in text.splitlines() if l.lstrip().startswith("::")], [])

    def test_host_fingerprints_are_loaded_with_their_whitespace_collapsed(self):
        # `runners.py host` collapses the model; a host.json written any other way is read as written, and a
        # `::stop-commands::` line in the tee'd job log would hide every later ::error annotation.
        with tempfile.TemporaryDirectory() as d:
            run = Path(d) / "a"
            run.mkdir()
            (run / "run.json").write_text('{"run": "linux-gcc"}', encoding="utf-8")
            (run / "host.json").write_text(json.dumps({"cpu": " CPU\n::stop-commands::x\t\r\nrev 2 ",
                                                       "logical_cpus": 4}), encoding="utf-8")
            hosts = perf.load_hosts(Path(d))
        self.assertEqual(hosts, {"linux-gcc": {"cpu": "CPU ::stop-commands::x rev 2", "logical_cpus": 4}})
        # The class it names is the one `runners.py host` would have written for the same machine.
        self.assertEqual(runners.host_class(hosts["linux-gcc"]), "CPU ::stop-commands::x rev 2, 4 logical CPUs")

    def test_the_backstop_counts_each_run_on_its_own(self):
        # linux-gcc's class changes every 2nd night; windows stays on one class and is gated from its 6th
        # night. Windows' gated rows must not reset linux-gcc's count, nor the other way round.
        history, red = {}, {"linux-gcc": [], "windows": []}
        for n in range(1, 63):
            e = self.on(f"CPU rev {n // 2}", self.night(n, t=100.0, runs=("linux-gcc", "windows")))
            e["metrics"]["windows/t"] = dict(e["metrics"]["linux-gcc/t"])
            e["hosts"]["windows"] = {"cpu": "WIN", "logical_cpus": 4, "class": "WIN, 4 logical CPUs"}
            history, rows = perf.compare(history, e, 5)
            for r in rows:
                if r["verdict"] in perf.FAILING:
                    red[r["metric"].split("/")[0]].append(n)
        self.assertEqual(red, {"linux-gcc": [60, 61, 62], "windows": []})
        self.assertEqual(history["entries"][-1]["ungated_nights"], {"linux-gcc": 62, "windows": 0})

    def test_the_legacy_walk_counts_each_run_on_its_own(self):
        # The same two runs, in a history written before the count was stored: the walk back over the stored
        # verdicts must read each run's own rows, so windows' gated rows do not stop linux-gcc's count.
        def night(n):
            e = self.on(f"CPU rev {n // 2}", self.night(n, t=100.0, runs=("linux-gcc", "windows")))
            e["metrics"]["windows/t"] = dict(e["metrics"]["linux-gcc/t"])
            e["hosts"]["windows"] = {"cpu": "WIN", "logical_cpus": 4, "class": "WIN, 4 logical CPUs"}
            return e

        legacy, _ = self.replay([night(n) for n in range(1, 62)])
        for e in legacy["entries"]:
            del e["ungated_nights"]
        new, rows = perf.compare(legacy, night(62), 5)
        self.assertEqual({r["metric"]: r["verdict"] for r in rows if r["verdict"] != "never measured"},
                         {"linux-gcc/t": "host-churn", "windows/t": "ok"})
        # The walk sees the 60 retained entries (nights 2 to 61), so linux-gcc's count is 60 + tonight.
        self.assertEqual(new["entries"][-1]["ungated_nights"], {"linux-gcc": 61, "windows": 0})

    def test_an_accepted_row_is_a_night_that_a_level_gated(self):
        # A perf_accept for tonight sets the level tonight is gated against, so that night resets the backstop's
        # count like an `ok` one: 59 churning nights, an accepted night on a new class, then another new class.
        nights = [self.on(f"CPU rev {n // 2}", self.night(n, t=100.0)) for n in range(1, 60)]
        nights.append(self.on("CPU rev 30", self.night(60, accepted=[self.accept(60, 100.0)], t=100.0)))
        nights.append(self.on("CPU rev 31", self.night(61, t=100.0)))
        history, verdicts = self.replay(nights)
        self.assertEqual([v["t"] for v in verdicts[58:]], ["calibrating", "accepted", "new-host-class"])
        self.assertEqual([e["ungated_nights"] for e in history["entries"][-3:]],
                         [{"linux-gcc": 59}, {"linux-gcc": 0}, {"linux-gcc": 1}])


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

    def test_doctest_runner_skips_the_labels_ctest_excludes(self):
        # A Windows nightly job has no GPU and its CTest step runs -LE "gpu|perf", so its doctest step must
        # skip the same entries (nightly 36309067888 ran pcg_gpu_tests there, which exits 1 without Vulkan).
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            fake = work / "fake_doctest.py"
            fake.write_text(
                "import sys\n"
                "out = next(a for a in sys.argv if a.startswith('--out='))[6:]\n"
                "open(out, 'w').write('<doctest><TestCase name=\"c\"><OverallResultsAsserts "
                "test_case_success=\"true\"/></TestCase></doctest>')\n", encoding="utf-8")
            # helios_test() gives a main entry no LABELS at all: an unlabelled entry must still run.
            tests = [{"name": "cpu_tests", "command": [sys.executable, str(fake), "--test-case-exclude=perf:*"]},
                     {"name": "gpu_tests",
                      "command": [sys.executable, "-c", "raise SystemExit(7)", "--test-case-exclude=perf:*"],
                      "properties": [{"name": "LABELS", "value": ["render", "gpu"]}]},
                     # ctest -LE searches each label, as re.search does: "gpu" also excludes "vulkan-gpu".
                     {"name": "vk_tests",
                      "command": [sys.executable, "-c", "raise SystemExit(8)", "--test-case-exclude=perf:*"],
                      "properties": [{"name": "LABELS", "value": ["vulkan-gpu", "shaders"]}]}]
            original = sc.ctest_tests
            sc.ctest_tests = lambda *args: tests
            logs = {}
            try:
                for name, args in (("skip", ["--label-exclude", "gpu|perf"]), ("all", []),
                                   ("anchored", ["--label-exclude", "^gpu$"])):
                    with contextlib.redirect_stdout(io.StringIO()) as log:
                        rc = runners.main(["doctest", "--build-dir", "build", *args, "--out", str(work / name)])
                    logs[name] = (rc, log.getvalue())
            finally:
                sc.ctest_tests = original
            self.assertEqual(logs["skip"][0], 0)
            self.assertEqual(sorted(p.name for p in (work / "skip").iterdir()),
                             ["cpu_tests.status.json", "cpu_tests.xml"])
            # The skip line names the label that matched, not every label of the entry.
            self.assertIn("doctest: gpu_tests: not run (label gpu matches 'gpu|perf')\n", logs["skip"][1])
            self.assertIn("doctest: vk_tests: not run (label vulkan-gpu matches 'gpu|perf')\n", logs["skip"][1])
            self.assertEqual(logs["all"][0], 1)
            self.assertEqual(json.loads((work / "all" / "gpu_tests.status.json").read_text())["returncode"], 7)
            # PR #32's review: an anchored pattern is matched against each label on its own, as ctest -LE does,
            # so "^gpu$" skips gpu_tests (labels render, gpu) and runs vk_tests (vulkan-gpu); a search over the
            # labels joined into one string would skip neither.
            self.assertEqual(logs["anchored"][0], 1)  # vk_tests ran and exited 8
            self.assertEqual(sorted(p.name for p in (work / "anchored").iterdir()),
                             ["cpu_tests.status.json", "cpu_tests.xml", "vk_tests.status.json"])
            self.assertEqual(json.loads((work / "anchored" / "vk_tests.status.json").read_text())["returncode"], 8)
            self.assertIn("doctest: gpu_tests: not run (label gpu matches '^gpu$')\n", logs["anchored"][1])
            self.assertNotIn("vk_tests: not run", logs["anchored"][1])


# `net_bench --gate` lines as the nightly of 2026-10-03 logged them (run 37098788944: Linux job 111134044934,
# Windows job 111134044901), with the numbers each scenario needs.
def bench_output(socket=(1000000, 1000000, 200474), stack=(884608, 884608, 88464),
                 trunk=(11999995, 11999995, 20000, 0.0), advisory=False, failed=(), trunk_connected=True,
                 summary=True) -> str:
    """The output of one `net_bench --gate [--advisory ns02-stack]` run; `failed` names the clauses whose
    failure line it prints (socket, stack, trunk), as net_bench does after each measurement."""
    lines = [f"05:23:55.610 INFO  [General] NS-0.2 socket: {socket[0]}/{socket[1]} datagrams, {socket[2]} pps per "
             f"core (4988.2 ms CPU, 4989.7 ms wall)",
             "05:23:55.612 INFO  [Net] [server] listening on 127.0.0.1:43068 (public 127.0.0.1:43068), 256 slots"]
    if "socket" in failed:
        lines.insert(1, "05:23:55.611 ERROR [General] NS-0.2 FAILED: needs 100k pps per core without loss  "
                        "(net_bench.cpp:201)")
    lines.append(f"05:24:05.617 INFO  [General] NS-0.2 HTP stack: {stack[0]}/{stack[1]} encrypted packets delivered, "
                 f"{stack[2]} packets per core (send + receive, 1 thread)")
    if "stack" in failed:
        lines.append("05:24:05.617 ERROR [General] NS-0.2 FAILED: the HTP stack needs 100k encrypted packets per "
                     "core without loss  (net_bench.cpp:213)")
    elif advisory and stack[2] < 100000:
        lines.append(f"05:24:05.617 WARN  [General] NS-0.2 advisory: owner approval 2026-09-30, evidence "
                     f"docs/evidence/ns-0.2-owner-approval-2026-09-30.md. The HTP stack's {stack[2]} packets per core "
                     f"is below 100k: reported, not failing (loss still fails)  (net_bench.cpp:220)")
    lines.append("05:24:05.618 INFO  [Net] [cell] listening on 127.0.0.1:54161 (public 127.0.0.1:54161), 4 slots")
    if not trunk_connected:
        lines.append("05:24:15.618 ERROR [General] NS-0.7: trunk did not connect  (net_bench.cpp:235)")
    elif trunk is not None:
        sent, delivered, pps, drops = trunk
        lines.append(f"05:34:05.923 INFO  [General] NS-0.7 trunk: 600 s, sent {sent}, delivered {delivered} ({pps} pps, "
                     f"190.1 Mbit/s payload, 199.7 Mbit/s wire), drops {drops:.4f} %, cell thread 0.25 cores, gateway "
                     f"thread 0.23 cores, rcvbuf 2048 KB, syscalls send 1466870 recv 7501308")
        if "trunk" in failed:
            lines.append("05:34:05.923 ERROR [General] NS-0.7 FAILED: needs 20k pps, < 0.1 % drops, <= 1 core per "
                         "side  (net_bench.cpp:241)")
    if summary:
        lines.append(f"05:34:05.923 INFO  [General] gates {'FAILED' if failed or not trunk_connected else 'PASSED'}"
                     f"{' (NS-0.2' + chr(39) + 's HTP stack rate advisory)' if advisory else ''}")
    return "\n".join(lines) + "\n"


LINUX_ADVISORY = (0, bench_output(advisory=True))  # 88,464 per core: below 100k, on the approval
WINDOWS_PASS = (0, bench_output(socket=(1000000, 1000000, 344086), stack=(1248256, 1248256, 125611),
                                trunk=(11999992, 11999992, 20000, 0.0)))
WINDOWS_STACK_BELOW = (1, bench_output(socket=(1000000, 1000000, 249000), stack=(853990, 853990, 85399),
                                       failed=("stack",)))  # 2026-09-27's level, on the strict step
WINDOWS_TRUNK_FAILS = (1, bench_output(socket=(1000000, 1000000, 344086), stack=(1248256, 1248256, 125611),
                                       trunk=(11999992, 11813909, 19690, 1.5507), failed=("trunk",)))  # 2026-09-27


class GateClauseTests(unittest.TestCase):
    """#43's review, N4: NS-0.2 and NS-0.7 both read the one net_bench_gate result, so on the strict Windows step
    a stack below 100k also failed NS-0.7. With one JUnit case per clause (runners.py gate) and each criterion
    citing its own (the real scorecard.jsonc), each fails only on its own clauses, and the owner approval still
    relaxes nothing but the encrypted stack's rate on linux-gcc."""

    @classmethod
    def setUpClass(cls):
        cls.data, _ = sc.load_jsonc(sc.ROOT / "scorecard.jsonc")
        cls.cases = cls.data["gates"]["net_bench_gate"]["cases"]
        cls.entries = {e["id"]: e for e in cls.data["criteria"] if e["id"] in ("NS-0.2", "NS-0.7")}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.results = Path(self.tmp.name)

    def gate(self, run, rc, text, seconds=615.3, cases=None):
        """The gate result `runners.py gate` writes for `text`, in run `run`'s result set."""
        d = self.results / run
        (d / "gates").mkdir(parents=True, exist_ok=True)
        (d / "run.json").write_text(json.dumps({"run": run}), encoding="utf-8")
        scanner = runners.ClauseLines(self.cases if cases is None else cases)
        scanner.feed(text.encode("utf-8"))
        scanner.finish()
        runners.write_gate_junit(d / "gates" / "net_bench_gate.xml", "net_bench_gate", rc, seconds, text,
                                 runners.clause_verdicts(self.cases if cases is None else cases, scanner.hits, rc))

    def night(self, linux=LINUX_ADVISORY, windows=WINDOWS_PASS):
        """{criterion: report.evaluate(...)} for NS-0.2 and NS-0.7 with the registry's entries, the linux-gcc and
        windows-vs2026 gate results given (None: no gate result), and Linux's passing net_tests perf cases."""
        linux_dir = self.results / "linux-gcc"
        (linux_dir / "doctest").mkdir(parents=True, exist_ok=True)
        (linux_dir / "run.json").write_text('{"run": "linux-gcc"}', encoding="utf-8")
        (self.results / "windows-vs2026").mkdir(exist_ok=True)
        (self.results / "windows-vs2026" / "run.json").write_text('{"run": "windows-vs2026"}', encoding="utf-8")
        doctest_xml(linux_dir / "doctest" / "net_tests.perf.xml",
                    [("perf: NS-0.2: loopback 100k pps per core without loss", True, "-> 480000 pps per core"),
                     ("perf: NS-0.7 (short run): trunk throughput over UDP loopback", True, "")])
        for run, out in (("linux-gcc", linux), ("windows-vs2026", windows)):
            if out is not None:
                self.gate(run, *out)
        runs = report.load_results(self.results, MODULE)
        return {ident: report.evaluate(e, self.data, runs, None, sc.ROOT) for ident, e in self.entries.items()}

    @staticmethod
    def cells(result):
        return result["status"], result["platforms"]["linux"]["status"], result["platforms"]["windows"]["status"]

    @staticmethod
    def refs(result, os_name):
        return {r["ref"]: r["status"] for r in result["platforms"][os_name]["refs"]}

    def test_linux_on_the_advisory_and_ns07_pass(self):
        r = self.night()
        self.assertEqual(self.cells(r["NS-0.2"]), ("pass", "pass", "pass"))
        self.assertTrue(r["NS-0.2"]["by_approval"])  # linux-gcc's 88,464 passed on the approval
        self.assertEqual(r["NS-0.2"]["platforms"]["linux"]["advisory"],
                         ["linux-gcc net.ns02.stack_packets_per_core 88,464 packets/core"])
        self.assertEqual(self.cells(r["NS-0.7"]), ("pass", "pass", "pass"))
        self.assertEqual(self.refs(r["NS-0.7"], "windows"), {"gate net_bench_gate / NS-0.7 trunk": "pass"})

    def test_a_windows_stack_below_100k_fails_ns02_and_not_ns07(self):
        r = self.night(windows=WINDOWS_STACK_BELOW)
        self.assertEqual(self.cells(r["NS-0.2"]), ("fail", "pass", "fail"))
        self.assertEqual(self.refs(r["NS-0.2"], "windows"), {
            "gate net_bench_gate / NS-0.2 socket": "pass", "gate net_bench_gate / NS-0.2 HTP stack": "fail",
            "evidence docs/evidence/ns-0.2-owner-approval-2026-09-30.md": "pass"})
        self.assertIn("NS-0.2 FAILED: the HTP stack", "".join(report.junit_cases(
            self.results / "windows-vs2026" / "gates" / "net_bench_gate.xml")[2][3]))
        # NS-0.7's own clauses passed on Windows, so its Windows cell passes although the command exited 1.
        self.assertEqual(self.cells(r["NS-0.7"]), ("pass", "pass", "pass"))

    def test_ns07_failing_fails_only_ns07(self):
        r = self.night(windows=WINDOWS_TRUNK_FAILS)
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "pass", "fail"))
        self.assertEqual(self.cells(r["NS-0.2"]), ("pass", "pass", "pass"))
        # On linux-gcc too: the advisory relaxes NS-0.2's stack rate only, so NS-0.7 still fails there, and
        # NS-0.2 (on the approval) does not.
        linux = (1, bench_output(advisory=True, trunk=(11999995, 11000000, 18333, 8.3333), failed=("trunk",)))
        r = self.night(linux=linux)
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "fail", "pass"))
        self.assertEqual(self.cells(r["NS-0.2"]), ("pass", "pass", "pass"))
        self.assertTrue(r["NS-0.2"]["by_approval"])
        r = self.night(linux=(1, bench_output(advisory=True, trunk_connected=False)))
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "fail", "pass"))
        self.assertEqual(self.cells(r["NS-0.2"]), ("pass", "pass", "pass"))

    def test_the_advisory_still_fails_loss_a_silent_stack_and_raw_datagrams(self):
        for linux in ((1, bench_output(advisory=True, stack=(884608, 884600, 88463), failed=("stack",))),  # loss
                      (1, bench_output(advisory=True, stack=(0, 0, 0), failed=("stack",))),  # never sent
                      (1, bench_output(advisory=True, socket=(1000000, 1000000, 95000), failed=("socket",))),
                      (1, bench_output(advisory=True, socket=(1000000, 999990, 200474), failed=("socket",)))):
            r = self.night(linux=linux)
            self.assertEqual(self.cells(r["NS-0.2"]), ("fail", "fail", "pass"), linux[1])
            self.assertFalse(r["NS-0.2"]["by_approval"])
            self.assertEqual(self.cells(r["NS-0.7"]), ("pass", "pass", "pass"))

    def test_a_missing_clause_case_is_missing_never_green(self):
        # A gate result without the clause cases (written before them, or by a runner that could not read the
        # registry): every clause reference reads `missing`, so neither criterion passes or keeps a streak.
        night = self.night()
        d = self.results / "windows-vs2026" / "gates"
        runners.write_gate_junit(d / "net_bench_gate.xml", "net_bench_gate", 0, 615.3, WINDOWS_PASS[1])
        runs = report.load_results(self.results, MODULE)
        for ident in ("NS-0.2", "NS-0.7"):
            r = report.evaluate(self.entries[ident], self.data, runs, None, sc.ROOT)
            self.assertEqual(self.cells(r), ("unmeasured", "pass", "unmeasured"))
            gates = {k: v for k, v in self.refs(r, "windows").items() if k.startswith("gate")}
            self.assertEqual(set(gates.values()), {"missing"}, gates)
            detail = next(x["detail"] for x in r["platforms"]["windows"]["refs"] if x["ref"].startswith("gate"))
            self.assertIn("windows-vs2026 missing (the gate's result has no case 'NS-0.", detail)
            self.assertEqual(night[ident]["status"], "pass")  # the same night with the cases passes
        previous = {"generated": "2026-10-02T03:17:00Z", "scheduled": True,
                    "criteria": [{"id": i, "phase": 0, "streak": 2, "history": ["pass"] * 2} for i in self.entries]}
        data = dict(self.data, criteria=list(self.entries.values()), exit=[])
        rep = report.build_report(data, runs, None, previous, True, 0, sc.ROOT,
                                  now=datetime(2026, 10, 3, 3, 17, tzinfo=timezone.utc))
        self.assertEqual([(c["id"], c["status"], c["streak"], c["green"]) for c in rep["criteria"]],
                         [("NS-0.2", "unmeasured", 0, False), ("NS-0.7", "unmeasured", 0, False)])
        # No gate result at all on Windows: missing too.
        (d / "net_bench_gate.xml").unlink()
        runs = report.load_results(self.results, MODULE)
        r = report.evaluate(self.entries["NS-0.7"], self.data, runs, None, sc.ROOT)
        self.assertEqual((self.cells(r), self.refs(r, "windows")),
                         (("unmeasured", "pass", "unmeasured"), {"gate net_bench_gate / NS-0.7 trunk": "missing"}))

    def test_an_exit_no_clause_explains_fails_every_clause(self):
        # Every line printed, none of them a failure, and exit 139 (a crash at exit): no clause is known to pass.
        r = self.night(windows=(-11, WINDOWS_PASS[1]))
        self.assertEqual(self.cells(r["NS-0.2"]), ("fail", "pass", "fail"))
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "pass", "fail"))
        # A failure line no clause knows: the same (the source test below keeps the patterns in step).
        r = self.night(windows=(1, WINDOWS_PASS[1].replace("gates PASSED", "NS-0.2 FAILED: something new")))
        self.assertEqual(self.cells(r["NS-0.2"]), ("fail", "pass", "fail"))
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "pass", "fail"))
        # Killed by the timeout during the trunk run: the trunk was not measured, and NS-0.2's clauses had passed.
        killed = WINDOWS_PASS[1][:WINDOWS_PASS[1].index("05:34:05.923")] + "[gate killed after the 1800 s timeout]\n"
        r = self.night(windows=(124, killed))
        self.assertEqual(self.cells(r["NS-0.7"]), ("fail", "pass", "fail"))
        self.assertEqual(self.cells(r["NS-0.2"]), ("pass", "pass", "pass"))
        # A run shorter than the gate's min_seconds fails every clause (net_bench without --gate, say).
        self.gate("windows-vs2026", 0, WINDOWS_PASS[1], seconds=17.0)
        runs = report.load_results(self.results, MODULE)
        for ident, e in self.entries.items():
            r = report.evaluate(e, self.data, runs, None, sc.ROOT)
            self.assertEqual(self.cells(r), ("fail", "pass", "fail"), ident)
            self.assertIn("ran 17 s of the required 600 s", r["platforms"]["windows"]["refs"][0]["detail"])

    def test_the_runner_streams_one_case_per_clause(self):
        # run_gate on a command that prints the strict Windows night of 2026-09-27 in chunks that split lines.
        text = WINDOWS_STACK_BELOW[1]
        script = ("import sys, time\n"
                  f"text = {text!r}\n"
                  "for i in range(0, len(text), 37):\n"
                  "    sys.stdout.write(text[i:i + 37]); sys.stdout.flush(); time.sleep(0.001)\n"
                  "sys.exit(1)\n")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            with contextlib.redirect_stdout(io.TextIOWrapper(io.BytesIO())) as log:
                rc = runners.main(["gate", "--name", "net_bench_gate", "--out", str(out), "--",
                                   sys.executable, "-c", script])
                log.flush()
                printed = log.buffer.getvalue().decode("utf-8")
            cases = report.junit_cases(out / "net_bench_gate.xml")
            root = ET.parse(out / "net_bench_gate.xml").getroot()
        self.assertEqual(rc, 1)
        self.assertEqual([(name, status) for name, status, _, _ in cases],
                         [("net_bench_gate", "fail"), ("NS-0.2 socket", "pass"), ("NS-0.2 HTP stack", "fail"),
                          ("NS-0.7 trunk", "pass")])
        self.assertEqual((root.get("tests"), root.get("failures")), ("4", "2"))
        self.assertEqual([c.get("classname") for c in root.iter("testcase")],
                         ["gates", "net_bench_gate", "net_bench_gate", "net_bench_gate"])
        stack = next(c for c in root.iter("testcase") if c.get("name") == "NS-0.2 HTP stack")
        self.assertEqual(stack.find("failure").get("message"), "05:24:05.617 ERROR [General] NS-0.2 FAILED: the HTP "
                         "stack needs 100k encrypted packets per core without loss  (net_bench.cpp:213)")
        self.assertIn("853990/853990 encrypted packets delivered, 85399 packets per core", stack.findtext("system-out"))
        self.assertIn("gate net_bench_gate: NS-0.7 trunk: pass\n", printed)
        self.assertIn("gate net_bench_gate: NS-0.2 HTP stack: FAIL (", printed)
        # The command's own case keeps the whole output for the perf metrics.
        res = report.Results("windows-vs2026")
        res.gates["net_bench_gate"] = cases[0][1:]
        metric = next(m for m in self.data["perf_metrics"] if m["id"] == "net.ns02.stack_packets_per_core")
        self.assertEqual(report.metric_values(metric, [res]), {"windows-vs2026": 85399.0})

    def test_the_runner_fails_a_clause_the_output_does_not_show(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            with contextlib.redirect_stdout(io.TextIOWrapper(io.BytesIO())):
                # Exit 0 without the trunk line (not net_bench --gate): that clause fails, and so does the step.
                head = WINDOWS_PASS[1][:WINDOWS_PASS[1].index("05:34:05.923")]
                rc = runners.run_gate("net_bench_gate", [sys.executable, "-c", f"print({head!r})"],
                                      out, None, None, None, self.cases)
                self.assertEqual(rc, 1)
                self.assertEqual([s for _, s, _, _ in report.junit_cases(out / "net_bench_gate.xml")],
                                 ["pass", "pass", "pass", "fail"])
                # A command that cannot start fails every clause instead of leaving them missing.
                rc = runners.run_gate("net_bench_gate", [str(out / "no-such-binary")], out, None, None, None,
                                      self.cases)
                self.assertEqual(rc, 127)
                self.assertEqual([s for _, s, _, _ in report.junit_cases(out / "net_bench_gate.xml")], ["fail"] * 4)
                # A gate without cases writes its one case, as before; an unreadable registry writes only that.
                rc = runners.main(["gate", "--name", "libfuzzer_packet_parser", "--out", str(out), "--",
                                   sys.executable, "-c", "pass"])
                self.assertEqual((rc, [n for n, *_ in report.junit_cases(out / "libfuzzer_packet_parser.xml")]),
                                 (0, ["libfuzzer_packet_parser"]))
                (out / "bad.jsonc").write_text("{", encoding="utf-8")
                rc = runners.main(["gate", "--name", "net_bench_gate", "--scorecard", str(out / "bad.jsonc"),
                                   "--out", str(out), "--", sys.executable, "-c", "pass"])
                self.assertEqual((rc, [n for n, *_ in report.junit_cases(out / "net_bench_gate.xml")]),
                                 (0, ["net_bench_gate"]))

    def test_the_clause_patterns_match_what_net_bench_prints(self):
        # Each case's patterns against net_bench.cpp's own format strings ({...} filled in): every result line
        # is one case's, and every failure line one case's, so a reworded or new failure line fails here, in
        # the PR tier, instead of passing a clause on the nightly.
        source = (sc.ROOT / "engine" / "net" / "bench" / "net_bench.cpp").read_text(encoding="utf-8")
        logs = {}
        for level, literals in re.findall(r'HELIOS_LOG_(INFO|WARN|ERROR)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', source):
            text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', literals))
            logs.setdefault(level, []).append(re.sub(r"\{[^{}]*\}", "1", text))
        usage = "--advisory takes '1' and applies to --gate only"  # exits 2 before any clause runs
        self.assertIn(usage, logs["ERROR"])
        claimed = {line: [n for n, c in self.cases.items() if re.search(c["failure"], line)] for line in logs["ERROR"]}
        self.assertEqual({line: names for line, names in claimed.items() if line != usage}, {
            "NS-0.2 FAILED: needs 100k pps per core without loss": ["NS-0.2 socket"],
            "NS-0.2 FAILED: the HTP stack needs 100k encrypted packets per core without loss": ["NS-0.2 HTP stack"],
            "NS-0.7: trunk did not connect": ["NS-0.7 trunk"],
            "NS-0.7 FAILED: needs 20k pps, < 0.1 % drops, <= 1 core per side": ["NS-0.7 trunk"]})
        self.assertEqual(claimed[usage], [])
        others = logs["INFO"] + logs["WARN"]
        for name, case in self.cases.items():
            self.assertEqual(len([line for line in others if re.search(case["result"], line)]), 1, name)
            self.assertEqual([line for line in others if re.search(case["failure"], line)], [], name)
        self.assertEqual({c["criterion"] for c in self.cases.values()}, {"NS-0.2", "NS-0.7"})


if __name__ == "__main__":
    unittest.main()
