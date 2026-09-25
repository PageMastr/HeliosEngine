"""Regression tests for lint JUnit to SARIF reporting."""

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from ctest_to_sarif import convert


class SarifConversionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        for relative in (
            "tools/lint/ip_names.cmake",
            "tools/lint/lint_tests.cmake",
            "src/ship.cpp",
            "src/other.cpp",
        ):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("line\n" * 8, encoding="utf-8")

    def junit(self, cases: list[tuple[str, str | None]]) -> Path:
        suite = ET.Element("testsuite")
        for name, output in cases:
            case = ET.SubElement(suite, "testcase", name=name)
            if output is not None:
                ET.SubElement(case, "failure", message="Failed")
                ET.SubElement(case, "system-out").text = output
        path = self.root / "ctest.xml"
        ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)
        return path

    def test_clean_run_has_no_results(self) -> None:
        report = convert(self.junit([("lint_ip_names", None), ("core_tests", None)]), self.root)
        self.assertEqual(report["version"], "2.1.0")
        self.assertEqual(report["runs"][0]["results"], [])

    def test_reports_each_real_source_location(self) -> None:
        output = (
            "CMake Error at /tmp/ip_names.cmake:214 (message):\n"
            "  IP-name lint failed (2 finding(s)):\n\n"
            "    src/ship.cpp:3: forbidden reference 'Alpha'\n"
            "    src/other.cpp:5: forbidden reference 'Beta'\n"
        )
        report = convert(self.junit([("lint_ip_names", output), ("core_tests", "failed")]), self.root)
        results = report["runs"][0]["results"]
        self.assertEqual(len(results), 2)
        self.assertEqual(
            [(r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
              r["locations"][0]["physicalLocation"]["region"]["startLine"])
             for r in results],
            [("src/ship.cpp", 3), ("src/other.cpp", 5)],
        )
        self.assertEqual(results[0]["message"]["text"], "forbidden reference 'Alpha'")

    def test_unlocated_failure_links_to_lint_implementation(self) -> None:
        report = convert(self.junit([("lint_ip_names_fixture", "CMake Error: wrong diagnostic")]), self.root)
        result = report["runs"][0]["results"][0]
        self.assertEqual(
            result["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
            "tools/lint/ip_names.cmake",
        )
        self.assertIn("wrong diagnostic", result["message"]["text"])

    def test_path_outside_checkout_is_not_annotated(self) -> None:
        output = "../outside.cpp:2: bad name\n"
        report = convert(self.junit([("lint_ip_names", output)]), self.root)
        result = report["runs"][0]["results"][0]
        self.assertEqual(
            result["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
            "tools/lint/ip_names.cmake",
        )

    def test_malformed_junit_fails(self) -> None:
        broken = self.root / "broken.xml"
        broken.write_text("<testsuite><testcase>", encoding="utf-8")
        with self.assertRaises(ET.ParseError):
            convert(broken, self.root)


if __name__ == "__main__":
    unittest.main()
