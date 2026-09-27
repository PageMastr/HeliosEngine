#!/usr/bin/env python3
"""Result producers for the nightly scorecard report (09 §5.6; WP-0.3).

  python3 tools/scorecard/runners.py doctest --build-dir DIR [--config CFG] [--perf] --out DIR
  python3 tools/scorecard/runners.py gate --name NAME --out DIR [--build-dir DIR [--config CFG]]
                                          [--timeout SECONDS] -- COMMAND [ARGS...]

`doctest` runs the doctest binaries that helios_test() registers, one CTest entry at a time (the main
entry, or with --perf the `perf:` entry), with doctest's XML reporter: DIR/<binary>.xml or
<binary>.perf.xml, plus <stem>.status.json with the exit code and wall time. It uses the working
directory, environment and timeout CTest would. CTest's own JUnit gives per-binary results; these
files give per-case results and the MESSAGE lines perf.py reads.

`gate` runs a command that is not a CTest (net_bench --gate, a libFuzzer run) and writes DIR/<NAME>.xml,
a one-case JUnit file with the exit code, wall time and the output's tail. Output is also streamed to the
console. A bare command name is looked up in the build's bin directory. It exits with the command's code.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import scorecard

TAIL_BYTES = 64 * 1024


def find_binary(name: str, build_dir: str | None, config: str | None) -> str:
    """`name` itself if it is a path or on PATH, else <build>/bin[/<config>]/<name>[.exe]."""
    if os.sep in name or "/" in name or not build_dir:
        return name
    for sub in ([config] if config else []) + [""]:
        for suffix in (".exe", ""):
            candidate = Path(build_dir) / "bin" / sub / (name + suffix)
            if candidate.is_file():
                return str(candidate)
    return shutil.which(name) or name


def run_doctest(build_dir: str, config: str | None, perf: bool, out: Path, ctest: str) -> int:
    out.mkdir(parents=True, exist_ok=True)
    kind = "perf" if perf else "main"
    worst = 0
    for test in scorecard.ctest_tests(Path(build_dir), config, ctest):
        if scorecard.is_doctest_entry(test) != kind:
            continue
        binary = test["name"].removesuffix("_perf")
        stem = binary + (".perf" if perf else "")
        report = out / f"{stem}.xml"
        env = dict(os.environ)
        for item in scorecard.test_property(test, "ENVIRONMENT") or []:
            key, _, value = item.partition("=")
            env[key] = value
        timeout = float(scorecard.test_property(test, "TIMEOUT") or 1500)
        cmd = test["command"] + ["--reporters=xml", f"--out={report}", "--duration=true", "--no-intro=true"]
        start = time.monotonic()
        try:
            rc = subprocess.run(cmd, cwd=scorecard.test_property(test, "WORKING_DIRECTORY"), env=env,
                                timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = -1
        seconds = time.monotonic() - start
        (out / f"{stem}.status.json").write_text(json.dumps({"returncode": rc, "seconds": round(seconds, 3)}) + "\n",
                                                 encoding="utf-8")
        print(f"doctest: {test['name']}: exit {rc} in {seconds:.1f} s -> {report.name}", flush=True)
        worst = worst or (rc != 0)
    return 1 if worst else 0


def write_gate_junit(path: Path, name: str, rc: int, seconds: float, output: str) -> None:
    suite = ET.Element("testsuite", name="gates", tests="1", failures="0" if rc == 0 else "1")
    case = ET.SubElement(suite, "testcase", name=name, classname="gates", time=f"{seconds:.3f}",
                         status="run" if rc == 0 else "fail")
    if rc != 0:
        ET.SubElement(case, "failure", message=f"exit code {rc}")
    ET.SubElement(case, "system-out").text = output
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def run_gate(name: str, command: list[str], out: Path, build_dir: str | None, config: str | None,
             timeout: float | None) -> int:
    out.mkdir(parents=True, exist_ok=True)
    command = [find_binary(command[0], build_dir, config)] + command[1:]
    tail = bytearray()
    start = time.monotonic()
    try:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as e:
        write_gate_junit(out / f"{name}.xml", name, 127, 0.0, f"cannot start {command[0]}: {e}")
        print(f"gate {name}: cannot start {command[0]}: {e}")
        return 127
    assert proc.stdout is not None
    timed_out = threading.Event()

    def kill() -> None:
        timed_out.set()
        proc.kill()

    timer = threading.Timer(timeout, kill) if timeout else None
    if timer:
        timer.start()
    for chunk in iter(lambda: proc.stdout.read1(65536), b""):
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
        tail += chunk
        del tail[:-TAIL_BYTES]
    proc.stdout.close()
    rc = proc.wait()
    if timer:
        timer.cancel()
    seconds = time.monotonic() - start
    text = tail.decode("utf-8", "replace")
    if timed_out.is_set():
        rc, text = 124, text + f"\n[gate killed after the {timeout:.0f} s timeout]\n"
    write_gate_junit(out / f"{name}.xml", name, rc, seconds, text)
    print(f"gate {name}: exit {rc} in {seconds:.1f} s", flush=True)
    return rc if 0 <= rc < 256 else 1  # a negative code is a signal (POSIX)


def main(argv: list[str] | None = None) -> int:
    scorecard._utf8_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    doc = sub.add_parser("doctest", help="run doctest binaries with the XML reporter")
    doc.add_argument("--build-dir", required=True)
    doc.add_argument("--perf", action="store_true", help="the perf: entries instead of the main ones")
    gate = sub.add_parser("gate", help="run a command as a named gate")
    gate.add_argument("--name", required=True)
    gate.add_argument("--build-dir")
    gate.add_argument("--timeout", type=float)
    gate.add_argument("cmd", nargs=argparse.REMAINDER)
    for p in (doc, gate):
        p.add_argument("--out", type=Path, required=True)
        p.add_argument("--config", default="")
        p.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)
    if args.command == "doctest":
        return run_doctest(args.build_dir, args.config or None, args.perf, args.out, args.ctest)
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        parser.error("gate needs a command after --")
    return run_gate(args.name, cmd, args.out, args.build_dir, args.config or None, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
