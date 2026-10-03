#!/usr/bin/env python3
"""Result producers for the nightly scorecard report (09 §5.6; WP-0.3).

  python3 tools/scorecard/runners.py doctest --build-dir DIR [--config CFG] [--perf] [--label-exclude REGEX]
                                             --out DIR
  python3 tools/scorecard/runners.py gate --name NAME --out DIR [--build-dir DIR [--config CFG]]
                                          [--timeout SECONDS] -- COMMAND [ARGS...]
  python3 tools/scorecard/runners.py host --out FILE

`doctest` runs the doctest binaries that helios_test() registers, one CTest entry at a time (the main
entry, or with --perf the `perf:` entry), with doctest's XML reporter: DIR/<binary>.xml or
<binary>.perf.xml, plus <stem>.status.json with the exit code and wall time. It uses the working
directory, environment and timeout CTest would. --label-exclude skips the entries `ctest -LE REGEX` skips
(any label matching), so a job runs the same binaries in both steps: the Windows jobs, which have no GPU,
exclude `gpu`. CTest's own JUnit gives per-binary results; these files give per-case results and the
MESSAGE lines perf.py reads.

`gate` runs a command that is not a CTest (net_bench --gate, a libFuzzer run) and writes DIR/<NAME>.xml,
a one-case JUnit file with the exit code, wall time and the output's tail. Output is also streamed to the
console. A bare command name is looked up in the build's bin directory. It exits with the command's code.

`host` writes the result set's host fingerprint (host.json): the CPU model name and the logical CPU count
of the machine that measured it. perf.py keeps each run's perf levels per host class (the two together),
because hosted runners of one image come on several CPU models whose timings differ by more than the
perf budgets.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
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


def run_doctest(build_dir: str, config: str | None, perf: bool, out: Path, ctest: str,
                label_exclude: str | None = None) -> int:
    out.mkdir(parents=True, exist_ok=True)
    kind = "perf" if perf else "main"
    worst = 0
    for test in scorecard.ctest_tests(Path(build_dir), config, ctest):
        if scorecard.is_doctest_entry(test) != kind:
            continue
        labels = scorecard.test_property(test, "LABELS") or []
        # Each label on its own, as ctest -LE does: an anchored REGEX ("^gpu$") skips "gpu" but not "vulkan-gpu".
        matched = [label for label in labels if label_exclude and re.search(label_exclude, label)]
        if matched:
            print(f"doctest: {test['name']}: not run (label{'s' if len(matched) > 1 else ''} {', '.join(matched)} "
                  f"{'match' if len(matched) > 1 else 'matches'} {label_exclude!r})", flush=True)
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


def _first_field(text: str, names: tuple[str, ...]) -> str:
    """The value of the first `name: value` line whose name is one of `names` (cpuinfo, lscpu)."""
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep and key.strip() in names and value.strip():
            return value.strip()
    return ""


def cpu_model() -> str:
    """The CPU model name, from the source each OS keeps it in: /proc/cpuinfo or lscpu on Linux, the
    registry on Windows, sysctl on macOS, else platform.processor(). '' when none of them says."""
    model = ""
    if sys.platform.startswith("linux"):
        try:
            model = _first_field(Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace"),
                                 ("model name", "Model Name", "cpu model"))
        except OSError:
            pass
        if not model:
            try:
                out = subprocess.run(["lscpu"], capture_output=True, text=True, timeout=30,
                                     env=dict(os.environ, LC_ALL="C")).stdout
                model = _first_field(out, ("Model name",))
            except (OSError, subprocess.SubprocessError):
                pass
    elif sys.platform == "win32":
        try:
            import winreg
            path = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as key:
                model = str(winreg.QueryValueEx(key, "ProcessorNameString")[0])
        except OSError:
            pass
    elif sys.platform == "darwin":
        try:
            model = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True,
                                   timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            pass
    return " ".join((model or platform.processor() or "").split())


def host_fingerprint() -> dict:
    """This machine's host fingerprint: {"cpu": model name, "logical_cpus": n}."""
    return {"cpu": cpu_model() or "unknown CPU", "logical_cpus": os.cpu_count() or 0}


def host_class(fingerprint: dict) -> str:
    """The host class a fingerprint names: perf levels are kept per run and host class."""
    return f"{fingerprint.get('cpu') or 'unknown CPU'}, {fingerprint.get('logical_cpus') or '?'} logical CPUs"


def main(argv: list[str] | None = None) -> int:
    scorecard._utf8_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    doc = sub.add_parser("doctest", help="run doctest binaries with the XML reporter")
    doc.add_argument("--build-dir", required=True)
    doc.add_argument("--perf", action="store_true", help="the perf: entries instead of the main ones")
    doc.add_argument("--label-exclude", metavar="REGEX",
                     help="skip entries with a CTest label matching REGEX, as ctest -LE does")
    gate = sub.add_parser("gate", help="run a command as a named gate")
    gate.add_argument("--name", required=True)
    gate.add_argument("--build-dir")
    gate.add_argument("--timeout", type=float)
    gate.add_argument("cmd", nargs=argparse.REMAINDER)
    host = sub.add_parser("host", help="write this machine's host fingerprint (CPU model, logical CPUs)")
    host.add_argument("--out", type=Path, required=True)
    for p in (doc, gate):
        p.add_argument("--out", type=Path, required=True)
        p.add_argument("--config", default="")
        p.add_argument("--ctest", default="ctest")
    args = parser.parse_args(argv)
    if args.command == "host":
        fingerprint = host_fingerprint()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(fingerprint) + "\n", encoding="utf-8")
        print(f"host: {host_class(fingerprint)} -> {args.out}")
        return 0
    if args.command == "doctest":
        return run_doctest(args.build_dir, args.config or None, args.perf, args.out, args.ctest,
                           args.label_exclude or None)
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        parser.error("gate needs a command after --")
    return run_gate(args.name, cmd, args.out, args.build_dir, args.config or None, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
