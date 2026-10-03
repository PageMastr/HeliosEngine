"""Seeded-fixture tests for the nightly report, the perf comparator and the result runners."""

import contextlib
import copy
import io
import json
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
            self.assertEqual(perf.metric_values(overhead, [run]), {"linux-gcc": value}, printed)
        cores = metrics["net.ns07.trunk_cell_cores"]
        run.gates["net_bench_gate"] = ("pass", 600.0, "NS-0.7 trunk: 600 s, sent 11999995, delivered 11999995 "
                                       "(20000 pps, 190.1 Mbit/s payload, 199.7 Mbit/s wire), drops 0.0000 %, "
                                       "cell thread 0.245 cores, gateway thread 0.231 cores, rcvbuf 4096 KB")
        self.assertEqual(perf.metric_values(cores, [run]), {"linux-gcc": 0.245})

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
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    skipped = runners.main(["doctest", "--build-dir", "build", "--label-exclude", "gpu|perf",
                                            "--out", str(work / "skip")])
                    every = runners.main(["doctest", "--build-dir", "build", "--out", str(work / "all")])
            finally:
                sc.ctest_tests = original
            self.assertEqual(skipped, 0)
            self.assertEqual(sorted(p.name for p in (work / "skip").iterdir()),
                             ["cpu_tests.status.json", "cpu_tests.xml"])
            self.assertEqual(every, 1)
            self.assertEqual(json.loads((work / "all" / "gpu_tests.status.json").read_text())["returncode"], 7)


if __name__ == "__main__":
    unittest.main()
