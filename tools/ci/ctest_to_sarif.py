"""Convert failed Helios lint CTests from JUnit XML to SARIF 2.1.0."""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import quote


LOCATION = re.compile(
    r"^\s*(?P<path>(?:[A-Za-z]:)?[^:\n]+?):(?P<line>[1-9][0-9]*):\s*(?P<message>.+)$",
    re.MULTILINE,
)


def fallback_file(test_name: str) -> str:
    if test_name.startswith("lint_isa"):
        return "tools/lint/isa_audit.cmake"
    if test_name.startswith("lint_ip_names"):
        return "tools/lint/ip_names.cmake"
    if test_name.startswith("lint_licenses"):
        return "tools/lint/licenses.cmake"
    if test_name.startswith("lint_windows_manifest"):
        return "tools/lint/windows_manifest.cmake"
    if test_name.startswith("lint_layering"):
        return "cmake/HeliosLayering.cmake"
    return "tools/lint/lint_tests.cmake"


def source_location(root: Path, reported: str, line: int) -> dict | None:
    candidate = Path(reported)
    if not candidate.is_absolute():
        candidate = root / candidate
    candidate = candidate.resolve()
    if not candidate.is_file() or not candidate.is_relative_to(root):
        return None
    relative = candidate.relative_to(root).as_posix()
    return {
        "physicalLocation": {
            "artifactLocation": {"uri": quote(relative, safe="/")},
            "region": {"startLine": line},
        }
    }


def make_result(test_name: str, message: str, location: dict) -> dict:
    return {
        "ruleId": test_name,
        "level": "error",
        "message": {"text": message[:4000]},
        "locations": [location],
    }


def convert(junit: Path, root: Path) -> dict:
    root = root.resolve()
    suite = ET.parse(junit).getroot()
    results = []
    rules = {}
    for case in suite.iter("testcase"):
        name = case.get("name", "")
        if not name.startswith("lint_"):
            continue
        failures = list(case.findall("failure")) + list(case.findall("error"))
        if not failures:
            continue
        rules[name] = {"id": name, "shortDescription": {"text": name.replace("_", " ")}}
        output = case.findtext("system-out") or ""
        details = output.strip() or "\n".join(
            failure.text or failure.get("message", "Lint test failed") for failure in failures
        ).strip()
        located = False
        for match in LOCATION.finditer(details):
            location = source_location(root, match.group("path"), int(match.group("line")))
            if location is None:
                continue
            results.append(make_result(name, match.group("message").strip(), location))
            located = True
        if not located:
            # A fixture or infrastructure failure has no offending source path. Link to the
            # relevant lint implementation so the failure remains visible in code scanning.
            location = source_location(root, fallback_file(name), 1)
            if location is None:
                raise ValueError(f"Fallback source for {name} is missing")
            results.append(make_result(name, f"Lint test failed: {details}", location))
    return {
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "Helios CMake lints", "rules": list(rules.values())}},
            "results": results,
        }],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--junit", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = convert(args.junit, args.source_root)
    except (ET.ParseError, OSError, ValueError) as error:
        print(f"Cannot convert CTest JUnit to SARIF: {error}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
