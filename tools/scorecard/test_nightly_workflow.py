"""The nightly's fetch and perf steps (.github/workflows/nightly.yml), run under bash against a fake `gh`,
and the scope of its one advisory gate.

These steps decide whether tonight is compared with a perf history, so each way they can go wrong (an API
error, a lost or expired artifact, a restart over a usable history) is a scenario here. POSIX only: the
steps are bash, and the fake `gh` is a shell script. The advisory scope test reads the files and runs
everywhere.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

import scorecard

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "nightly.yml"

FAKE_GH = r"""#!/bin/bash
# Runs come from $SCEN/runs, each run's artifacts ("name expired" lines, as the step's --jq prints them)
# from $SCEN/<run>.artifacts and the files from $SCEN/<run>.<artifact>.json. FAIL_LIST and FAIL_DL fail
# the run listing or one artifact's download.
case "$1 $2" in
  "run list")
    if [ "$*" != "${*/ci.yml/}" ]; then echo 555; exit 0; fi
    if [ -n "$FAIL_LIST" ]; then echo "HTTP 502: Bad Gateway" >&2; exit 1; fi
    cat "$SCEN/runs" ;;
  "api repos/o/r/actions/runs/555/jobs?per_page=100") echo '{"jobs": []}' ;;
  "api repos/o/r/actions/runs/"*)
    id=${2#repos/o/r/actions/runs/}; id=${id%%/*}
    if [ -f "$SCEN/$id.artifacts" ]; then cat "$SCEN/$id.artifacts"; fi ;;
  "run download")
    if [ -n "$FAIL_DL" ] && [ "$7" = "$FAIL_DL" ]; then echo "error downloading $7" >&2; exit 1; fi
    cp "$SCEN/$3.$7.json" "$9/$7.json" ;;
  *) echo "unexpected gh call: $*" >&2; exit 3 ;;
esac
"""

HISTORY = {"version": 1, "entries": [{
    "sha": "x", "date": "2026-01-10T03:17:00Z", "declared": ["net.ns02.loopback_pps_per_core"], "runs": ["linux-gcc"],
    "metrics": {"linux-gcc/net.ns02.loopback_pps_per_core": {
        "value": 480000, "unit": "pps/core", "better": "higher", "category": "runtime", "gate": True,
        "verdict": "ok", "baseline": 480000, "anchor": 480000, "anchor_n": 5}},
    "missing": []}]}


def step_script(name: str) -> str:
    """The `run: |` block of the scorecard job step whose name starts with `name`."""
    lines = WORKFLOW.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip().startswith(f"- name: {name}"))
    run = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == "run: |")
    indent = len(lines[run]) - len(lines[run].lstrip()) + 2
    body = []
    for line in lines[run + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        body.append(line)
    return textwrap.dedent("\n".join(body)) + "\n"


@unittest.skipIf(os.name == "nt" or not shutil.which("bash"), "the nightly's steps are bash")
class NightlyPerfStepsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fetch = step_script("Fetch last night's report")
        cls.perf = step_script("Perf history")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "bin").mkdir()
        (self.dir / "bin" / "gh").write_text(FAKE_GH, encoding="utf-8")
        (self.dir / "bin" / "gh").chmod(0o755)
        (self.dir / "work" / "results").mkdir(parents=True)
        (self.dir / "work" / "tools").symlink_to(ROOT / "tools")
        self.scen = self.dir / "scen"
        self.scen.mkdir()

    def runs(self, **artifacts):
        """runs(r3=["scorecard-report"], r2=["scorecard-report", "perf-history"], r1=["perf-history:expired"])"""
        ids = sorted((int(k[1:]) for k in artifacts), reverse=True)
        (self.scen / "runs").write_text("".join(f"{i}\n" for i in ids), encoding="utf-8")
        for i in ids:
            lines = []
            for name in artifacts[f"r{i}"]:
                name, _, expired = name.partition(":")
                lines.append(f"{name} {'true' if expired else 'false'}")
                content = HISTORY if name == "perf-history" else {"criteria": [], "exit": []}
                (self.scen / f"{i}.{name}.json").write_text(json.dumps(content), encoding="utf-8")
            (self.scen / f"{i}.artifacts").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def night(self, **env):
        """(fetch exit code, perf exit code, files in previous/, new history entries or None, summary, fetch log)"""
        work = self.dir / "work"
        base = dict(os.environ, PATH=f"{self.dir / 'bin'}{os.pathsep}{os.environ.get('PATH', '')}",
                    SCEN=str(self.scen), GITHUB_REPOSITORY="o/r", GITHUB_RUN_ID="99", GITHUB_SHA="abcdef123456",
                    GITHUB_STEP_SUMMARY=str(work / "summary.md"), RESTART_PERF_HISTORY="")
        base.update(env)
        fetch = subprocess.run(["bash", "-e", "-c", self.fetch], cwd=work, env=base, capture_output=True, text=True,
                               timeout=120)
        # The perf step runs whatever the fetch step did (`if: !cancelled()`).
        perf = subprocess.run(["bash", "-e", "-c", self.perf], cwd=work, env=base, capture_output=True, text=True,
                              timeout=120)
        out = work / "out" / "perf-history.json"
        entries = len(json.loads(out.read_text(encoding="utf-8"))["entries"]) if out.is_file() else None
        summary = (work / "summary.md").read_text(encoding="utf-8") if (work / "summary.md").is_file() else ""
        previous = sorted(p.name for p in (work / "previous").iterdir()) if (work / "previous").is_dir() else []
        return fetch.returncode, perf.returncode, previous, entries, summary, fetch.stdout + fetch.stderr

    def test_a_normal_night_uses_the_newest_history_even_from_an_older_run(self):
        self.runs(r3=["scorecard-report"], r2=["scorecard-report", "perf-history"])
        fetch, perf, previous, entries, summary, log = self.night()
        self.assertEqual((fetch, perf, entries), (0, 1, 2), log)  # 1: the history's metric is missing tonight
        self.assertIn("previous scorecard-report: run 3", log)
        self.assertIn("previous perf-history: run 2", log)
        self.assertIn("**missing**", summary)

    def test_a_run_list_error_fails_instead_of_passing_for_a_first_night(self):
        self.runs(r2=["scorecard-report", "perf-history"])
        fetch, perf, previous, entries, summary, _ = self.night(FAIL_LIST="1")
        self.assertEqual((fetch, perf, previous, entries), (1, 2, [], None))
        self.assertIn("perf history not found", summary)

    def test_a_failed_download_fails_and_keeps_the_levels(self):
        self.runs(r2=["scorecard-report", "perf-history"])
        fetch, perf, _, entries, _, _ = self.night(FAIL_DL="perf-history")
        self.assertEqual((fetch, perf, entries), (1, 2, None))

    def test_first_nights(self):
        self.runs()
        fetch, perf, previous, entries, summary, log = self.night()
        self.assertEqual((fetch, perf, previous, entries), (0, 0, ["first-night"], 1))
        self.assertIn("**First night of the perf history", summary)
        self.assertIn("::warning::", log)

    def test_a_night_1_without_a_history_does_not_lock_the_next_nights(self):
        self.runs(r1=["scorecard-report"])
        fetch, perf, previous, entries, _, _ = self.night()
        self.assertEqual((fetch, perf, previous, entries), (0, 0, ["first-night", "scorecard-report.json"], 1))

    def test_an_expired_history_fails_until_restarted_on_purpose(self):
        self.runs(r2=["scorecard-report"], r1=["perf-history:expired"])
        fetch, perf, previous, entries, summary, log = self.night()
        self.assertEqual((fetch, perf, previous, entries), (0, 2, ["scorecard-report.json"], None))
        self.assertIn("::error::an earlier nightly lists a perf-history artifact", log)
        self.assertIn("restart_perf_history", summary)
        fetch, perf, previous, entries, summary, log = self.night(RESTART_PERF_HISTORY="true")
        self.assertEqual((fetch, perf, previous, entries), (0, 0, ["perf-restart", "scorecard-report.json"], 1))
        self.assertIn("**Perf history restarted on request", summary)
        self.assertIn("::warning::restart_perf_history", log)

    def test_a_restart_with_no_history_at_all(self):
        self.runs()
        fetch, perf, previous, entries, summary, _ = self.night(RESTART_PERF_HISTORY="true")
        self.assertEqual((fetch, perf, previous, entries), (0, 0, ["perf-restart"], 1))
        self.assertIn("**Perf history restarted on request", summary)

    def test_a_restart_over_a_usable_history_is_refused_and_the_history_kept(self):
        # Round 4's blocking finding: the restart must not replace levels that only perf_accept may move.
        self.runs(r3=["scorecard-report"], r2=["scorecard-report", "perf-history"])
        fetch, perf, previous, entries, summary, log = self.night(RESTART_PERF_HISTORY="true")
        self.assertEqual((fetch, perf, entries), (1, 1, 2), log)
        self.assertNotIn("perf-restart", previous)
        self.assertIn("::error::restart_perf_history refused: a perf history was fetched from run 2", log)
        self.assertIn("perf_accept", log)
        self.assertNotIn("restarted on request", summary)
        self.assertIn("**missing**", summary)  # compared with the fetched history, as on a normal night
        self.assertTrue((self.dir / "work" / "ci-jobs.json").is_file())  # the rest of the step still ran

    def test_scheduled_runs_have_no_input(self):
        self.runs(r2=["scorecard-report", "perf-history"])
        fetch, perf, previous, entries, _, _ = self.night(RESTART_PERF_HISTORY="")
        self.assertEqual((fetch, perf, entries), (0, 1, 2))
        self.assertNotIn("perf-restart", previous)


class AdvisoryScopeTests(unittest.TestCase):
    """NS-0.2's owner approval of 2026-09-30 relaxes one clause on the hosted nightly, the encrypted stack's
    100k packets per core, through `net_bench --gate --advisory ns02-stack` (09 §5.6). This pins that scope:
    another advisory in the nightly or another approval in the registry fails here until this test, and its
    review, say otherwise."""

    def test_only_ns02s_stack_rate_is_advisory(self):
        commands = [line for line in WORKFLOW.read_text(encoding="utf-8").splitlines()
                    if not line.lstrip().startswith("#")]
        self.assertEqual([m for line in commands for m in re.findall(r"--advisory\b\s*(\S*)", line)],
                         ["ns02-stack", "ns02-stack"])
        gates = [line for line in commands if "-- net_bench --gate" in line]
        self.assertEqual(len(gates), 2)  # linux-gcc and windows-vs2026
        self.assertTrue(all(line.rstrip().endswith("-- net_bench --gate --advisory ns02-stack") for line in gates),
                        gates)
        data, _ = scorecard.load_jsonc(ROOT / "scorecard.jsonc")
        approvals = {e["id"]: scorecard.approval(e) for e in data["criteria"] + data["exit"] if scorecard.approval(e)}
        self.assertEqual(list(approvals), ["NS-0.2"])
        self.assertEqual(approvals["NS-0.2"]["advisory"], ["net.ns02.stack_packets_per_core"])
        self.assertEqual(approvals["NS-0.2"]["evidence"], "docs/evidence/ns-0.2-owner-approval-2026-09-30.md")
        metric = next(m for m in data["perf_metrics"] if m["id"] == "net.ns02.stack_packets_per_core")
        self.assertEqual((metric["criterion"], metric["gate"]), ("NS-0.2", "net_bench_gate"))
        self.assertIn("NS-0\\.2 HTP stack", metric["pattern"])


if __name__ == "__main__":
    unittest.main()
