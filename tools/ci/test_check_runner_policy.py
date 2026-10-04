"""Tests for check_runner_policy.py (WP-0.4; 09 §5.4a): its YAML subset parser and every policy rule.

CTest `lint_runner_policy_unittest` runs them. Each fixture in testdata/runner_policy/ seeds one rule and says
which rules it must raise on its first line (`# expect: <rule> ...` or `# expect: (none)`). Where PyYAML is
installed, the parser is also compared with PyYAML's BaseLoader (YAML's failsafe schema: every scalar a string)
on every workflow and fixture of the repository and on the snippets below; the parser itself needs no PyYAML.
"""

import io
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_runner_policy as policy  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "testdata" / "runner_policy"
WORKFLOWS = policy.ROOT / ".github" / "workflows"
RULES = {"trigger", "guard-ref", "guard-var", "secrets", "permissions", "pinning", "runs-on", "reusable", "parse"}

try:
    import yaml  # optional: only the differential tests use it
except ImportError:  # pragma: no cover
    yaml = None


def plain(node):
    if isinstance(node, dict):
        return {k: plain(v) for k, v in node.items()}
    if isinstance(node, list):
        return [plain(v) for v in node]
    return node


def check(text: str) -> tuple[set[str], list[str]]:
    """(rules raised, runner jobs) for a workflow text."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "w.yml"
        path.write_text(textwrap.dedent(text), encoding="utf-8")
        findings, jobs = policy.check_workflow(path, "w.yml")
    return {f.rule for f in findings}, jobs


GOOD_JOB = """
    jobs:
      gpu:
        if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'
        runs-on: [self-hosted, win-gpu]
        permissions:
          contents: read
        steps:
          - run: echo gpu
"""


def workflow(on: str = "on:\n  workflow_dispatch:\n", job: str = GOOD_JOB, top: str = "") -> str:
    return textwrap.dedent(on) + textwrap.dedent(top) + textwrap.dedent(job)


def with_job(**fields: str) -> str:
    """GOOD_JOB with some of its fields (`if`, `runs-on`, `permissions`, `steps`, extra keys) replaced."""
    lines = {"if": "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'",
             "runs-on": "runs-on: [self-hosted, win-gpu]",
             "permissions": "permissions:\n  contents: read",
             "steps": "steps:\n  - run: echo gpu"}
    for key, value in fields.items():
        lines[key.replace("_", "-")] = value
    body = "\n".join(v for v in lines.values() if v)
    return "jobs:\n  gpu:\n" + textwrap.indent(body, "    ") + "\n"


class FixtureTests(unittest.TestCase):
    def test_each_fixture_raises_exactly_its_rules(self):
        files = sorted(FIXTURES.glob("*.yml"))
        self.assertGreaterEqual(len(files), 10)
        seen = set()
        for path in files:
            first = path.read_text(encoding="utf-8").splitlines()[0]
            self.assertTrue(first.startswith("# expect: "), path.name)
            expected = set(first[len("# expect: "):].split()) - {"(none)"}
            findings, _ = policy.check_workflow(path, path.name)
            with self.subTest(fixture=path.name):
                self.assertEqual(expected, {f.rule for f in findings}, [str(f) for f in findings])
            seen |= expected
        self.assertEqual(RULES, seen, "every rule needs a seeded fixture")

    def test_the_passing_fixture_has_a_runner_job(self):
        findings, jobs = policy.check_workflow(FIXTURES / "pass.yml", "pass.yml")
        self.assertEqual([], [str(f) for f in findings])
        self.assertEqual(["gpu"], jobs)

    def test_the_matrix_fixture_reaches_the_runner_through_include(self):
        _, jobs = policy.check_workflow(FIXTURES / "fail_matrix_runs_on.yml", "m.yml")
        self.assertEqual(["test"], jobs)


class RepositoryTests(unittest.TestCase):
    def test_the_repository_workflows_pass(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = policy.main(["--workflows", str(WORKFLOWS)])
        self.assertEqual(0, code, out.getvalue())
        self.assertIn(".github/workflows/win-gpu.yml:gpu", out.getvalue())

    def test_win_gpu_run_scripts_are_ascii(self):
        # The runner writes each `run:` to a .ps1 file, which Windows PowerShell 5.1 may read as ANSI.
        doc = policy.parse_yaml((WORKFLOWS / "win-gpu.yml").read_text(encoding="utf-8"))
        runs = [step["run"] for job in doc["jobs"].values() for step in job.get("steps", []) if "run" in step]
        self.assertGreater(len(runs), 5)
        for run in runs:
            with self.subTest(run=run[:60]):
                self.assertTrue(run.isascii(), [line for line in run.split("\n") if not line.isascii()])

    def test_yaml_and_yml_files_are_both_workflows(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("a.yml", "b.yaml", "c.txt"):
                (Path(tmp) / name).write_text("on: push\njobs: {}\n", encoding="utf-8")
            (Path(tmp) / "sub").mkdir()
            self.assertEqual(["a.yml", "b.yaml"], [p.name for p in policy.workflow_files(Path(tmp))])

    def test_cli_prints_file_line_rule_and_fails(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = policy.main([str(FIXTURES / "fail_trigger_pull_request.yml")])
        self.assertEqual(1, code)
        self.assertRegex(out.getvalue(), r"(?m)^\S*fail_trigger_pull_request\.yml:4: trigger: trigger `pull_request`")


class TriggerTests(unittest.TestCase):
    def test_allowed_triggers(self):
        for on in ("on: workflow_dispatch\n", "on: [workflow_dispatch]\n",
                   "on:\n  push:\n    branches: [main]\n", "on:\n  push:\n    branches: main\n",
                   "on:\n  push:\n    branches:\n      - main\n    paths: ['src/**']\n",
                   "on:\n  schedule:\n    - cron: '0 4 * * *'\n  workflow_dispatch:\n    inputs: {}\n"):
            with self.subTest(on=on):
                self.assertEqual(set(), check(workflow(on))[0])

    def test_forbidden_triggers(self):
        for on in ("on: pull_request\n", "on: [push]\n", "on:\n  push:\n", "on: [workflow_dispatch, pull_request_target]\n",
                   "on:\n  workflow_run:\n    workflows: [CI]\n", "on:\n  workflow_call:\n",
                   "on:\n  push:\n    branches-ignore: [wip]\n", "on:\n  push:\n    branches: [main]\n    tags: ['v*']\n",
                   "on:\n  push:\n    tags: ['v*']\n", "on:\n  push:\n    branches: ['**']\n",
                   "on: repository_dispatch\n", "on: issue_comment\n", "on:\n  schedule: '0 4 * * *'\n"):
            with self.subTest(on=on):
                self.assertEqual({"trigger"}, check(workflow(on))[0])

    def test_a_missing_on_fails(self):
        self.assertEqual({"trigger"}, check(workflow(on=""))[0])

    def test_triggers_of_workflows_without_runner_jobs_are_not_checked(self):
        self.assertEqual(set(), check(workflow("on: pull_request\n", with_job(**{"runs-on": "runs-on: ubuntu-24.04"})))[0])


class GuardTests(unittest.TestCase):
    def guard(self, condition: str) -> set[str]:
        return check(workflow(job=with_job(**{"if": condition})))[0]

    def test_accepted_guards(self):
        for condition in (
                "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'",
                "if: ${{ vars.HELIOS_WIN_GPU == 'enabled' && github.ref == 'refs/heads/main' }}",
                "if: \"'refs/heads/main' == github.ref && 'enabled' == vars.HELIOS_WIN_GPU\"",
                "if: (github.ref == 'refs/heads/main') && (vars.helios_win_gpu=='ENABLED') && !cancelled()",
                "if: always() && (github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled')",
                "if: >-\n  github.ref == 'refs/heads/main' &&\n  vars.HELIOS_WIN_GPU == 'enabled'"):
            with self.subTest(condition=condition):
                self.assertEqual(set(), self.guard(condition))

    def test_rejected_guards(self):
        cases = {
            "": {"guard-ref", "guard-var"},
            "if: true": {"guard-ref", "guard-var"},
            "if: vars.HELIOS_WIN_GPU == 'enabled'": {"guard-ref"},
            "if: github.ref == 'refs/heads/main'": {"guard-var"},
            "if: github.ref_name == 'main' && vars.HELIOS_WIN_GPU == 'enabled'": {"guard-ref"},
            "if: github.ref != 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'": {"guard-ref"},
            "if: \"!(github.ref == 'refs/heads/main') && vars.HELIOS_WIN_GPU == 'enabled'\"": {"guard-ref"},
            # Unquoted, the `!` is a YAML tag: the parser refuses it, as GitHub does.
            "if: !(github.ref == 'refs/heads/main') && vars.HELIOS_WIN_GPU == 'enabled'": {"parse"},
            "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU != 'enabled'": {"guard-var"},
            "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled' || always()": {"guard-ref", "guard-var"},
            "if: (github.ref == 'refs/heads/main' || true) && vars.HELIOS_WIN_GPU == 'enabled'": {"guard-ref"},
            # Text around ${{ }} makes a non-empty string: always true.
            "if: ${{ github.ref == 'refs/heads/main' }} && ${{ vars.HELIOS_WIN_GPU == 'enabled' }}": {"guard-ref", "guard-var"},
            "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled' && ('a'": {"guard-ref", "guard-var"},
        }
        for condition, rules in cases.items():
            with self.subTest(condition=condition):
                self.assertEqual(rules, self.guard(condition))

    GUARD = "github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'"

    def test_text_around_the_guard_expression_fails(self):
        # GitHub keeps the scalar's text around ${{ }} (format('{0}\n', ...)): a non-empty string, always true. A
        # block scalar keeps its final line break unless it is `|-` or `>-`.
        for condition in ("if: |\n  ${{ " + self.GUARD + " }}", "if: \"${{ " + self.GUARD + " }} \"",
                          "if: \" ${{ " + self.GUARD + " }}\"", "if: >\n  ${{ " + self.GUARD + " }}",
                          "if: \"${{ " + self.GUARD + " }}\\n\"", "if: '${{ " + self.GUARD.replace("'", "''") + " }}\t'"):
            with self.subTest(condition=condition):
                self.assertEqual({"guard-ref", "guard-var"}, self.guard(condition))

    def test_plain_and_stripped_guards_still_pass(self):
        # Without ${{ }} the whole value is the expression, and the expression lexer skips whitespace.
        for condition in ("if: ${{ " + self.GUARD + " }}", "if: |-\n  ${{ " + self.GUARD + " }}",
                          "if: >-\n  ${{ " + self.GUARD + " }}", "if: |\n  " + self.GUARD,
                          "if: \" " + self.GUARD + " \""):
            with self.subTest(condition=condition):
                self.assertEqual(set(), self.guard(condition))


class RunsOnTests(unittest.TestCase):
    def reaches(self, runs_on: str, extra: str = "") -> bool:
        rules, jobs = check(workflow(job=with_job(**{"runs-on": runs_on}) + textwrap.indent(extra, "    ")))
        self.assertNotIn("runs-on", rules)
        return jobs == ["gpu"]

    def test_labels_that_reach_the_runner(self):
        for runs_on in ("runs-on: win-gpu", "runs-on: WIN-GPU", "runs-on: [self-hosted]", "runs-on: self-hosted",
                        "runs-on: [self-hosted, Windows, X64]", "runs-on: [windows]", "runs-on: [win-gpu, linux]",
                        "runs-on:\n  labels: [self-hosted, win-gpu]", "runs-on:\n  - self-hosted\n  - x64"):
            with self.subTest(runs_on=runs_on):
                self.assertTrue(self.reaches(runs_on))

    def test_labels_that_do_not(self):
        for runs_on in ("runs-on: windows-latest", "runs-on: [self-hosted, linux]", "runs-on: ubuntu-24.04",
                        "runs-on: [self-hosted, Windows, gpu-lab]"):
            with self.subTest(runs_on=runs_on):
                self.assertFalse(self.reaches(runs_on))

    def test_labels_compare_without_blanks_and_case(self):
        # A label that might name the runner counts: blanks around it and case (including the Turkish dotless
        # i and the long s, which upper-case to I and S) do not hide it.
        for runs_on in ("runs-on: 'win-gpu '", "runs-on: [self-hosted, ' Win-GPU']", "runs-on: \"\\twin-gpu\"",
                        "runs-on: [w\u0131n-gpu]", "runs-on: [\u017felf-hosted]",
                        "runs-on: ${{ matrix.os }}\nstrategy:\n  matrix:\n    os: ['win-gpu ']"):
            with self.subTest(runs_on=runs_on):
                self.assertTrue(self.reaches(runs_on))

    def test_a_job_without_runs_on_or_uses_fails(self):
        # GitHub refuses such a job, but a key this check cannot see (a parser difference) must not make a job
        # look harmless: no `runs-on` is a finding in every workflow.
        rules, _ = check("on: pull_request\njobs:\n  build:\n    steps:\n      - run: echo x\n")
        self.assertEqual({"runs-on"}, rules)

    def test_matrix_values(self):
        strategy = "strategy:\n  matrix:\n    os: [ubuntu-24.04, windows-latest]\n"
        self.assertFalse(self.reaches("runs-on: ${{ matrix.os }}", strategy))
        self.assertTrue(self.reaches("runs-on: ${{ matrix.os }}", strategy.replace("windows-latest", "win-gpu")))
        include = "strategy:\n  matrix:\n    include:\n      - os: ubuntu-24.04\n      - os: [self-hosted, windows]\n"
        self.assertTrue(self.reaches("runs-on: ${{matrix.os}}", include))
        self.assertTrue(self.reaches("runs-on: [self-hosted, '${{ matrix.extra }}']",
                                     "strategy:\n  matrix:\n    extra: [win-gpu]\n"))

    def test_matrix_keys_compare_without_case(self):
        # matrix.os finds OS (DictionaryContextData looks keys up OrdinalIgnoreCase), and an `include` entry
        # may spell the axis differently from the axis itself: every key that might be the axis counts.
        for runs_on, strategy in (
                ("runs-on: ${{ matrix.os }}",
                 "strategy:\n  matrix:\n    os: [ubuntu-24.04]\n    include:\n      - OS: [self-hosted, win-gpu]\n"),
                ("runs-on: ${{ matrix.os }}",
                 "strategy:\n  matrix:\n    OS: [[self-hosted, win-gpu]]\n    include:\n      - os: ubuntu-24.04\n"),
                ("runs-on: ${{ matrix.Os }}",
                 "strategy:\n  matrix:\n    Os: [ubuntu-24.04]\n    include:\n      - os: [self-hosted, win-gpu]\n"),
                # Two axes that differ only in case: the lookup may find either.
                ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix:\n    os: [ubuntu-24.04]\n    OS: [win-gpu]\n"),
                # GitHub matches well-known keys exactly, so `INCLUDE` is most likely an axis; if its expansion
                # folded case it would be `include`. The check reads it both ways.
                ("runs-on: ${{ matrix.os }}",
                 "strategy:\n  matrix:\n    os: [ubuntu-24.04]\n    INCLUDE:\n      - os: [self-hosted, win-gpu]\n")):
            with self.subTest(strategy=strategy):
                self.assertTrue(self.reaches(runs_on, strategy))
        # Keys that fold to another name are other axes.
        self.assertFalse(self.reaches("runs-on: ${{ matrix.os }}",
                                      "strategy:\n  matrix:\n    os: [ubuntu-24.04]\n    os2: [win-gpu]\n"
                                      "    include:\n      - osx: [self-hosted]\n"))

    def test_case_variant_matrix_with_a_pr_trigger_fails(self):
        rules, jobs = check("on: pull_request\njobs:\n  build:\n    strategy:\n      matrix:\n        os: [ubuntu-24.04]\n"
                            "        include:\n          - OS: [self-hosted, win-gpu]\n    runs-on: ${{ matrix.os }}\n"
                            "    steps:\n      - run: echo ${{ secrets.DEPLOY_KEY }}\n")
        self.assertEqual(["build"], jobs)
        self.assertLessEqual({"trigger", "secrets", "guard-ref", "guard-var", "permissions"}, rules)

    def test_empty_and_blank_labels(self):
        # A runs-on that names no label cannot be placed by this check: fail closed, not "does not reach".
        for runs_on, extra in (("runs-on: []", ""), ("runs-on:\n  labels: []", ""), ("runs-on: ''", ""),
                               ("runs-on: [' ', \"\\t\"]", ""),
                               ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix:\n    os: [[]]\n"),
                               ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix:\n    os: [ubuntu-24.04, '']\n")):
            with self.subTest(runs_on=runs_on, extra=extra):
                rules, _ = check(workflow("on: pull_request\n", with_job(**{"runs-on": runs_on}) +
                                          textwrap.indent(extra, "    ")))
                self.assertEqual({"runs-on"}, rules)
        # A blank label beside real ones counts as absent, in case GitHub drops it.
        self.assertTrue(self.reaches("runs-on: [self-hosted, '']"))
        self.assertTrue(self.reaches("runs-on: [self-hosted, ' ', x64]"))
        self.assertFalse(self.reaches("runs-on: [ubuntu-24.04, '']"))

    def test_matrix_include_and_exclude_are_not_axes(self):
        # `matrix.include` is no axis GitHub resolves to labels; whatever it reads as, fail closed.
        for name in ("include", "Exclude"):
            with self.subTest(name=name):
                runs_on = "runs-on: ${{ matrix.%s }}" % name
                rules, _ = check(workflow("on: pull_request\n", with_job(**{"runs-on": runs_on}) +
                                          "    strategy:\n      matrix:\n        include:\n          - os: a\n"))
                self.assertEqual({"runs-on"}, rules)

    def test_unresolvable_runs_on_fails_everywhere(self):
        for runs_on, extra in (("runs-on: ${{ vars.RUNNER }}", ""),
                               ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix: ${{ fromJSON(inputs.m) }}\n"),
                               ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix:\n    other: [a]\n"),
                               ("runs-on: ${{ matrix.os }}", "strategy:\n  matrix:\n    os: ['${{ vars.X }}']\n"),
                               ("runs-on: \"${{ github.event_name == 'push' && 'win-gpu' || 'ubuntu-24.04' }}\"", ""),
                               ("runs-on:\n  group: lab", ""),
                               ("runs-on: [self-hosted, '${{ inputs.label }}']", "")):
            with self.subTest(runs_on=runs_on, extra=extra):
                rules, _ = check(workflow("on: pull_request\n", with_job(**{"runs-on": runs_on}) +
                                          textwrap.indent(extra, "    ")))
                self.assertEqual({"runs-on"}, rules)

    def test_reusable_workflow_calls(self):
        for uses in ("./.github/workflows/build.yml", "./.github/workflows/build.yaml"):
            with self.subTest(uses=uses):
                self.assertEqual(set(), check(workflow("on: pull_request\n", f"jobs:\n  call:\n    uses: {uses}\n"))[0])
        for uses in ("octo/repo/.github/workflows/build.yml@v1",
                     "PageMastr/HeliosEngine/.github/workflows/win-gpu.yml@feature",
                     # GitHub reads local reusable workflows from .github/workflows/ itself, never below or above it.
                     "./.github/workflows/sub/build.yml", "./.github/workflows/../build.yml",
                     "./.github/workflows/build.json", "./.github/workflows/build.yml@main", "./build.yml"):
            with self.subTest(uses=uses):
                self.assertEqual({"reusable"}, check(workflow("on: pull_request\n", f"jobs:\n  call:\n    uses: {uses}\n"))[0])


class SecretsAndPermissionsTests(unittest.TestCase):
    def test_secret_references(self):
        for snippet in ("env:\n  K: ${{ secrets.KEY }}", "env:\n  K: ${{ secrets['KEY'] }}", "env:\n  K: ${{ toJSON(secrets) }}",
                        "env:\n  K: ${{ Secrets.KEY }}", "env:\n  K: ${{ github.token }}", "env:\n  K: ${{ github['token'] }}",
                        "env:\n  K: ${{ toJSON(github) }}", "env:\n  K: ${{ github[format('{0}{1}', 'to', 'ken')] }}",
                        "env:\n  K: ${{ GitHub . Token }}",
                        "env:\n  K: '${{\n    secrets.KEY }}'"):
            with self.subTest(snippet=snippet):
                job = with_job(steps="steps:\n  - run: echo\n" + textwrap.indent(snippet, "    "))
                self.assertEqual({"secrets"}, check(workflow(job=job))[0])

    def test_closing_braces_inside_a_string_literal(self):
        # GitHub ends an expression at the first `}}` outside a '...' literal.
        for value in ("\"${{ format('}}', secrets.KEY) }}\"", "\"${{ format('{0}}}', secrets.KEY) }}\"",
                      "\"${{ format('}}', github.token) }}\"", "\"${{ format('it''s }}', secrets.KEY) }}\"",
                      "\"x ${{ 'a' }} y ${{ format('}}}}', toJSON(secrets)) }}\"",
                      "\"${{ secrets.KEY\"", "\"${{ 'unclosed }}' secrets.KEY\""):
            with self.subTest(value=value):
                job = with_job(steps=f"steps:\n  - run: echo\n    env:\n      K: {value}")
                self.assertEqual({"secrets"}, check(workflow(job=job))[0])
        # Text after an expression's end is not an expression.
        job = with_job(steps="steps:\n  - run: echo\n    env:\n      K: \"${{ format('}}', github.sha) }} secrets.txt\"")
        self.assertEqual(set(), check(workflow(job=job))[0])

    def test_needs_ids_match_as_github_may_and_unknown_needs_fail(self):
        prep = "jobs:\n  Prep:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: echo ${{ secrets.KEY }}\n"
        for needs in ("needs: prep", "needs: [PREP]", "needs: [' prep ']"):
            with self.subTest(needs=needs):
                gpu = with_job(needs=needs).replace("jobs:\n", "")
                self.assertEqual({"secrets"}, check(workflow(job=prep + gpu))[0])
        clean = prep.replace(" ${{ secrets.KEY }}", "")
        for needs in ("needs: missing", "needs: [Prep, missing]", "needs: [[Prep]]", "needs:\n  Prep: x"):
            with self.subTest(needs=needs):
                gpu = with_job(needs=needs).replace("jobs:\n", "")
                self.assertEqual({"secrets"}, check(workflow(job=clean + gpu))[0])
        self.assertEqual(set(), check(workflow(job=clean + with_job(needs="needs: Prep").replace("jobs:\n", "")))[0])

    def test_a_needed_reusable_workflow_call_fails(self):
        # Its jobs' secrets (environment secrets need no `secrets:` from the caller) and outputs are out of sight.
        for call in ("  prep:\n    uses: ./.github/workflows/prep.yml\n",
                     "  prep:\n    uses: ./.github/workflows/prep.yml\n    with:\n      x: y\n"):
            for needs in ("needs: prep", "needs: [mid]"):
                with self.subTest(call=call, needs=needs):
                    mid = "  mid:\n    needs: prep\n    runs-on: ubuntu-24.04\n    steps:\n      - run: echo\n"
                    gpu = with_job(needs=needs).replace("jobs:\n", "")
                    rules, jobs = check(workflow(job="jobs:\n" + call + mid + gpu))
                    self.assertEqual(({"secrets"}, ["gpu"]), (rules, jobs))
        # A call the runner job does not need is checked on its own.
        unrelated = "jobs:\n  prep:\n    uses: ./.github/workflows/prep.yml\n" + with_job().replace("jobs:\n", "")
        self.assertEqual(set(), check(workflow(job=unrelated))[0])

    def test_secrets_in_a_needed_local_reusable_workflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "caller.yml").write_text(textwrap.dedent("""\
                on:
                  schedule:
                    - cron: '0 4 * * *'
                permissions:
                  contents: read
                jobs:
                  prep:
                    uses: ./.github/workflows/prep.yml
                  gpu:
                    needs: prep
                    if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'
                    runs-on: [self-hosted, win-gpu]
                    steps:
                      - run: echo "${{ needs.prep.outputs.blob }}"
                """), encoding="utf-8")
            (d / "prep.yml").write_text(textwrap.dedent("""\
                on:
                  workflow_call:
                    outputs:
                      blob:
                        value: ${{ jobs.make.outputs.blob }}
                jobs:
                  make:
                    runs-on: ubuntu-24.04
                    environment: production
                    outputs:
                      blob: ${{ steps.s.outputs.blob }}
                    steps:
                      - id: s
                        run: echo "blob=$(echo "$K" | base64 | rev)" >> "$GITHUB_OUTPUT"
                        env:
                          K: ${{ secrets.DEPLOY_KEY }}
                """), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(1, policy.main(["--workflows", tmp]))
            self.assertIn("caller.yml:", out.getvalue())

    def test_secrets_in_if_expressions_need_no_braces(self):
        job = with_job(steps="steps:\n  - run: echo\n    if: secrets.KEY != ''")
        self.assertEqual({"secrets"}, check(workflow(job=job))[0])

    def test_text_that_is_not_an_expression_is_not_a_reference(self):
        job = with_job(steps="steps:\n  - run: echo no secrets.txt here, github.token neither\n"
                             "    env:\n      X: ${{ inputs.secrets }}\n      Y: ${{ github.sha }} ${{ github['ref'] }}\n"
                             "      Z: ${{ github.event.inputs.token }} ${{ github.tokens }}")
        self.assertEqual(set(), check(workflow(job=job))[0])

    def test_workflow_level_and_needed_jobs_count(self):
        top = "env:\n  K: ${{ secrets.KEY }}\n"
        self.assertEqual({"secrets"}, check(workflow(top=top))[0])
        needed = ("jobs:\n  prep:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: echo ${{ secrets.KEY }}\n"
                  "  mid:\n    needs: prep\n    runs-on: ubuntu-24.04\n    steps:\n      - run: echo\n")
        gpu = with_job(needs="needs: [mid]").replace("jobs:\n", "")
        self.assertEqual({"secrets"}, check(workflow(job=needed + gpu))[0])
        unrelated = needed.replace("  mid:\n    needs: prep\n", "  mid:\n")
        self.assertEqual(set(), check(workflow(job=unrelated + gpu))[0])

    def test_permissions(self):
        for perms, ok in (("permissions:\n  contents: read", True), ("permissions: {}", True),
                          ("permissions:\n  contents: none\n  actions: none", True),
                          ("permissions:\n  contents: write", False), ("permissions:\n  contents: read\n  actions: read", False),
                          ("permissions:\n  id-token: write", False), ("permissions: read-all", False),
                          ("permissions: write-all", False), ("", False)):
            with self.subTest(perms=perms):
                rules = check(workflow(job=with_job(permissions=perms)))[0]
                self.assertEqual(set() if ok else {"permissions"}, rules)

    def test_workflow_permissions_apply_when_the_job_has_none(self):
        self.assertEqual(set(), check(workflow(top="permissions:\n  contents: read\n", job=with_job(permissions="")))[0])
        self.assertEqual({"permissions"},
                         check(workflow(top="permissions: write-all\n", job=with_job(permissions="")))[0])
        # The job's own block replaces the workflow's.
        self.assertEqual(set(), check(workflow(top="permissions: write-all\n", job=with_job()))[0])

    def test_pinning(self):
        for uses, ok in (("actions/checkout@" + "a" * 40, True), ("./.github/actions/x", True),
                         ("docker://alpine@sha256:" + "0" * 64, True), ("actions/checkout@v5", False),
                         ("actions/checkout@main", False), ("actions/checkout@" + "a" * 39, False),
                         ("docker://alpine:3", False), ("org/repo/path@" + "b" * 40, True)):
            with self.subTest(uses=uses):
                rules = check(workflow(job=with_job(steps=f"steps:\n  - uses: {uses}")))[0]
                self.assertEqual(set() if ok else {"pinning"}, rules)


class ParserTests(unittest.TestCase):
    def test_scalars_and_collections(self):
        doc = policy.parse_yaml(textwrap.dedent("""\
            # comment
            name: 'it''s' # trailing
            on:
              push:
                branches: [main, "a b", 'c,d']
            jobs:
              a:
                runs-on: ${{ matrix.os }}
                steps:
                - run: |
                    echo one
                      two # not a comment

                    three
                - name: x
                  run: >-
                    folded
                    line

                    para
                - {uses: "x@y", with: {k: v, e: }}
                -
                  plain: multi
                    line value
            key with spaces: "esc \\t \\u00e9 \\
              joined"
            empty:
            """))
        self.assertEqual("it's", doc["name"])
        self.assertEqual(["main", "a b", "c,d"], doc["on"]["push"]["branches"])
        steps = doc["jobs"]["a"]["steps"]
        self.assertEqual("echo one\n  two # not a comment\n\nthree\n", steps[0]["run"])
        self.assertEqual("folded line\npara", steps[1]["run"])
        self.assertEqual({"uses": "x@y", "with": {"k": "v", "e": ""}}, plain(steps[2]))
        self.assertEqual({"plain": "multi line value"}, plain(steps[3]))
        self.assertEqual("esc \t \u00e9 joined", doc["key with spaces"])
        self.assertEqual("", doc["empty"])
        self.assertEqual(2, doc.lines["name"])
        self.assertEqual([10, 15, 21, 22], steps.lines)

    def test_unsupported_constructs_fail_closed(self):
        cases = {
            "a: &x 1\nb: *x\n": "anchors",
            "a: *x\n": "aliases",
            "a: !!str 1\n": "tags",
            "<<: {a: 1}\n": "merge keys",
            "? a\n: b\n": "complex keys",
            "a: 1\na: 2\n": "duplicate key",
            "a:\n\tb: 1\n": "tab in indentation",
            "a: 1\n---\nb: 2\n": "multiple documents",
            "a: 1\n...\n": "multiple documents",
            "%YAML 1.2\n---\na: 1\n": "directives",
            "a: b: c\n": "quote the value",
            "a: 'open\n": "unterminated",
            "a: [1, 2\n": "unterminated",
            "a: {b: 1} x\n": "unexpected text",
            "a:\n  b: 1\n c: 2\n": "",
            "- a\n- b\n": "must be a mapping",
            "": "empty",
            "a: 1\r\nb: 2\rc: 3\n": "carriage return",
            "a: |x\n  b\n": "block scalar header",
            "a: \"\\q\"\n": "unknown escape",
            "a: [b: c]\n": "",
        }
        for text, message in cases.items():
            with self.subTest(text=text):
                with self.assertRaisesRegex(policy.YamlError, message):
                    policy.parse_yaml(text)

    def test_unicode_line_breaks_fail_closed(self):
        # YAML 1.1 parsers (LibYAML, PyYAML, YamlDotNet) break lines at NEL, LS and PS; this parser does not, so
        # it refuses them: a key behind one would be a comment here and a key there.
        for br in ("\x85", "\u2028", "\u2029"):
            with self.subTest(br=repr(br)):
                text = ("on: pull_request\njobs:\n  build:\n"
                        f"    # note{br}    runs-on: [self-hosted, win-gpu]\n    steps:\n      - run: echo x\n")
                with self.assertRaisesRegex(policy.YamlError, "U\\+"):
                    policy.parse_yaml(text)
                self.assertEqual({"parse"}, check(text)[0])

    def test_control_characters_fail_closed(self):
        # Every C0 and C1 control but tab and line feed (and CR before LF), DEL, a BOM after the start, and the
        # non-characters U+FFFE and U+FFFF: YAML parsers refuse them or read them differently.
        for c in [chr(i) for i in range(0x20) if chr(i) not in "\t\n\r"] + \
                 [chr(i) for i in range(0x7F, 0xA0)] + ["\ufeff", "\ufffe", "\uffff"]:
            with self.subTest(c=repr(c)):
                for text in (f"a: 'x{c}y'\n", f"a: x\n# {c}\n", f"a: [x, \"{c}\"]\n"):
                    with self.assertRaises(policy.YamlError):
                        policy.parse_yaml(text)
        # A BOM at the start, CR LF line ends, tabs inside quoted values and U+00A0 are fine.
        self.assertEqual({"a": "x\ty\u00a0", "b": "c"}, plain(policy.parse_yaml("\ufeffa: 'x\ty\u00a0'\r\nb: c\r\n")))

    def test_a_policy_relevant_key_cannot_hide_in_a_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "w.yml"
            path.write_text("on: workflow_dispatch\non: pull_request\njobs: {}\n", encoding="utf-8")
            findings, _ = policy.check_workflow(path, "w.yml")
        self.assertEqual(["parse"], [f.rule for f in findings])

    def test_deep_nesting_is_a_parse_finding_not_a_traceback(self):
        # Every level costs the parser (and the policy walk) a Python frame; past the recursion limit the check
        # must still report the file and fail, not crash with a RecursionError.
        depth = sys.getrecursionlimit() + 200
        texts = {
            "flow": "on: pull_request\njobs:\n  a:\n    runs-on: " + "[" * depth + "win-gpu" + "]" * depth + "\n",
            "block": "on: pull_request\njobs:\n  a:\n    runs-on: win-gpu\n    env:\n" + "".join(
                " " * (6 + i) + f"k{i}:\n" for i in range(depth)) + " " * (6 + depth) + "v: 1\n",
            "sequence": "on: pull_request\njobs:\n  a:\n    runs-on: win-gpu\n    steps:\n      " + "- " * depth + "x\n",
        }
        for name, text in texts.items():
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "deep.yml"
                    path.write_text(text, encoding="utf-8")
                    findings, jobs = policy.check_workflow(path, "deep.yml")
                    self.assertEqual((["parse"], []), ([f.rule for f in findings], jobs))
                    self.assertIn("recursion limit", findings[0].message)
                    out = io.StringIO()
                    with redirect_stdout(out):
                        self.assertEqual(1, policy.main([str(path)]))
                self.assertRegex(out.getvalue(), r"(?m)^\S*deep\.yml:1: parse: nested deeper")

    @unittest.skipIf(yaml is None, "PyYAML is not installed")
    def test_agrees_with_pyyaml(self):
        texts = {p.name: p.read_text(encoding="utf-8") for p in sorted(WORKFLOWS.glob("*.y*ml"))}
        texts.update({p.name: p.read_text(encoding="utf-8") for p in sorted(FIXTURES.glob("*.yml"))
                      if p.name != "fail_parse_anchor.yml"})
        snippets = [
            "a: |+\n  x\n\n\nb: 1\n", "a: >\n  x\n   y\n  z\n\n  w\n", "a: |2-\n    x\n   y\nb: 2\n",
            "a: >+\n\n  x\n", "a:\n- 1\n- - 2\n  - 3\n-\n  k: v\n", "a: \"x\n\n  y\n z\"\n", "a: 'x\n  y'\n",
            "a: [x, {k: v}, [y], 'z', \"w\"]\n", "a: {k: [1, 2], 'q': \"r\"}\n", "a: x # c\nb: 'y' # c\n",
            "a:\n  # c\n  b: 1\n", "a: x:y\nb: http://h/p\n", "\"q k\": 1\n'r': 2\n", "a: -x\nb: ::error::y\n",
            "a: [\n  x, # c\n  y,\n]\n", "a: |\n  \n  x\n", "a: >-\n  a\n\n\n  b\n", "a: \"\\x41\\u0042\\U00000043\"\n",
            "a:\n  - b: 1\n    c:\n      - d\n  - e\n", "a: ~\nb: null\nc: true\nd: 017\n", "a: b\n  - c\n",
        ]
        texts.update({f"snippet {i}": s for i, s in enumerate(snippets)})
        for name, text in texts.items():
            with self.subTest(name=name):
                self.assertEqual(yaml.load(text, Loader=yaml.BaseLoader), plain(policy.parse_yaml(text)))


if __name__ == "__main__":
    unittest.main()
