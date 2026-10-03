#!/usr/bin/env python3
"""Self-hosted runner policy check for .github/workflows (09 §5.4a; WP-0.4; K33).

  python3 tools/ci/check_runner_policy.py                    # every workflow of this repository
  python3 tools/ci/check_runner_policy.py --workflows DIR    # every *.yml / *.yaml in DIR
  python3 tools/ci/check_runner_policy.py FILE...            # these workflow files

The `win-gpu` runner is the repository owner's own PC, so it may run only code that has been reviewed
and merged. A job reaches that runner when its `runs-on` names `win-gpu`, or names only labels the runner
carries (`self-hosted`, `windows`, `x64`, `win-gpu`; compared ignoring case and surrounding blanks), in any
form: a string, a list, `labels:` under a runner group, or `${{ matrix.<axis> }}` over literal matrix
values (axis lists and `include` entries). For every such job the workflow fails when:

  trigger      it has a trigger other than `schedule`, `workflow_dispatch` or `push` restricted to
               `branches: [main]` (`pull_request*`, `workflow_run`, `workflow_call`, `push` to other
               branches or to tags, ...);
  guard-ref    the job's `if:` does not require `github.ref == 'refs/heads/main'` as a top-level `&&` term
               (a value with `${{ }}` must be exactly `${{ ... }}`: GitHub keeps any text around it, even a
               blank or a block scalar's final line break, and a non-empty string is always true);
  guard-var    ... nor `vars.HELIOS_WIN_GPU == 'enabled'` (the owner's switch: no variable, no job);
  secrets      the job, a job it needs (transitively) or the workflow's top level reads `secrets`,
               `github.token` or the whole `github` context in an expression (`if:` values are
               expressions; an expression ends at the first `}}` outside a '...' literal, as GitHub reads
               it), or passes `secrets:` (including `secrets: inherit`) to a reusable workflow; or the job
               needs a reusable-workflow call, whose jobs' secrets (environment secrets need no `secrets:`)
               could reach it through outputs unseen, or a `needs` id that names no job;
  permissions  its token permissions (the job's, else the workflow's) are missing or broader than
               `contents: read`;
  pinning      a step `uses:` an action that is not local (`./...`) or pinned to a full commit SHA.

Two findings apply to every job of every workflow, because they hide whether a job reaches the runner:

  runs-on      `runs-on` is missing (in a job that calls no reusable workflow) or is an expression other
               than `${{ matrix.<axis> }}` over literal values;
  reusable     the job calls a reusable workflow other than a file directly in this repository's
               `./.github/workflows/`.

Workflows are read with the strict YAML subset parser below (standard library only; the repository's
Python tools add no dependencies). It fails closed: anchors, aliases, tags, merge keys, complex keys,
duplicate keys, tabs in indentation, multiple documents, and characters that other YAML parsers read as
line breaks (NEL, LS, PS) or refuse (other control characters, a BOM after the start) are errors rather
than places where it could read a different workflow than GitHub does; a differential test compares it
with PyYAML. Findings print as `<file>:<line>: <rule>: <message>`
(tools/ci/ctest_to_sarif.py turns them into annotations); the exit code is 1 when there is any.
CTest `lint_runner_policy` (label `lint`) runs it on every build, and tools/ci/run_lints.cmake runs it.

What it does not see, and leaves to review: the contents of actions (a local `./` action is reviewed code
of this repository; any action can read the job's token through an input default, as actions/checkout
does, which is why the token is limited to `contents: read`), and data a job fetches at run time other
than through `needs` (another job's artifacts, caches).

The check is the PR-tier half of the policy. A branch's own workflow file can still request the runner
on a push to that branch before any review, so the runner's job-started hook
(tools/ci/runner/job-started.ps1) ends every job that is not a `schedule`, `push` or
`workflow_dispatch` run of `refs/heads/main` by stopping the runner's worker process before the job's
first step; failing the hook alone would still let the job's `if: always()` steps and its actions'
`pre:` steps run (docs/runbooks/win-gpu-runner.md).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The labels the owner's runner carries (docs/runbooks/win-gpu-runner.md), lower case.
RUNNER_LABELS = frozenset({"self-hosted", "windows", "x64", "win-gpu"})
ALLOWED_EVENTS = ("schedule", "workflow_dispatch", "push")
PUSH_FILTERS_OK = {"branches", "paths", "paths-ignore"}
# Each guard rule's term, as written and in _normalized() forms (either operand order).
REQUIRED_TERMS = {
    "guard-ref": ("github.ref == 'refs/heads/main'",
                  ("github.ref=='refs/heads/main'", "'refs/heads/main'==github.ref")),
    "guard-var": ("vars.HELIOS_WIN_GPU == 'enabled'",
                  ("vars.helios_win_gpu=='enabled'", "'enabled'==vars.helios_win_gpu")),
}
GUARD_TEXT = "if: github.ref == 'refs/heads/main' && vars.HELIOS_WIN_GPU == 'enabled'"
# A reusable workflow of this repository, as GitHub reads one: a file directly in .github/workflows/.
LOCAL_WORKFLOW_RE = re.compile(r"\./\.github/workflows/[^/]+\.ya?ml")


# ------------------------------------------------------------------------------------------------
# A strict YAML subset parser (enough for GitHub workflow files; everything else is an error)
# ------------------------------------------------------------------------------------------------
class YamlError(Exception):
    def __init__(self, line: int, message: str):
        super().__init__(f"{line}: {message}")
        self.line = line
        self.message = message


class Map(dict):
    """A mapping; `lines` holds each key's 1-based line, `line` the mapping's first line."""

    def __init__(self, line: int):
        super().__init__()
        self.line = line
        self.lines: dict[str, int] = {}


class Seq(list):
    """A sequence; `lines` holds each item's 1-based line, `line` the sequence's first line."""

    def __init__(self, line: int):
        super().__init__()
        self.line = line
        self.lines: list[int] = []


ESCAPES = {"0": "\0", "a": "\a", "b": "\b", "t": "\t", "\t": "\t", "n": "\n", "v": "\v", "f": "\f",
           "r": "\r", "e": "\x1b", " ": " ", '"': '"', "/": "/", "\\": "\\", "N": "\x85", "_": "\xa0",
           "L": " ", "P": " "}
HEX_ESCAPES = {"x": 2, "u": 4, "U": 8}
UNSUPPORTED_START = {"&": "anchors", "*": "aliases", "!": "tags", "%": "directives",
                     "@": "reserved indicators", "`": "reserved indicators"}
# Characters refused anywhere in a file. NEL, LS and PS are line breaks to YAML 1.1 parsers (LibYAML, PyYAML,
# YamlDotNet), and this parser breaks lines at LF alone, so a key behind one would be a comment here and a key
# there. The other C0 and C1 controls (but tab, LF, and CR before LF), DEL, a BOM after the start and the
# non-characters U+FFFE and U+FFFF are outside YAML's printable set: parsers refuse them or read them differently.
FORBIDDEN_CHARS = re.compile("[\x00-\x08\x0b-\x1f\x7f-\x9f\u2028\u2029\ufeff\ufffe\uffff]")


def _closing_quote(text: str, quote: str) -> int | None:
    """Index of the quote that closes the scalar opened at text[0], or None."""
    i = 1
    while i < len(text):
        c = text[i]
        if quote == "'" and c == "'":
            if text[i + 1:i + 2] == "'":
                i += 2
                continue
            return i
        if quote == '"' and c == "\\":
            i += 2
            continue
        if quote == '"' and c == '"':
            return i
        i += 1
    return None


def _quoted_value(body: str, double: bool, line: int) -> str:
    """The value of a quoted scalar's body (between the quotes), folding line breaks as YAML does."""
    out, i, n = [], 0, len(body)
    while i < n:
        c = body[i]
        if not double and c == "'":  # '' (the closing quote was found already)
            out.append("'")
            i += 2
        elif double and c == "\\":
            e = body[i + 1:i + 2]
            if e == "\n":  # escaped line break: no space, leading blanks of the next line dropped
                i += 2
                while i < n and body[i] in " \t":
                    i += 1
                while i < n and body[i] == "\n":
                    out.append("\n")
                    i += 1
                    while i < n and body[i] in " \t":
                        i += 1
            elif e in ESCAPES:
                out.append(ESCAPES[e])
                i += 2
            elif e in HEX_ESCAPES:
                digits = body[i + 2:i + 2 + HEX_ESCAPES[e]]
                if len(digits) != HEX_ESCAPES[e] or not re.fullmatch(r"[0-9A-Fa-f]+", digits):
                    raise YamlError(line, f"bad escape '\\{e}{digits}'")
                out.append(chr(int(digits, 16)))
                i += 2 + len(digits)
            else:
                raise YamlError(line, f"unknown escape '\\{e}'")
        elif c in " \t":
            j = i
            while j < n and body[j] in " \t":
                j += 1
            if j < n and body[j] == "\n":
                i = j  # blanks before a line break are dropped
            else:
                out.append(body[i:j])
                i = j
        elif c == "\n":
            i += 1
            while i < n and body[i] in " \t":
                i += 1
            empty = 0
            while i < n and body[i] == "\n":
                empty += 1
                i += 1
                while i < n and body[i] in " \t":
                    i += 1
            out.append("\n" * empty if empty else " ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


class _Incomplete(Exception):
    """A flow collection continues on the next line."""


class _Flow:
    """Flow collections (`[a, b]`, `{k: v}`), possibly spanning lines."""

    def __init__(self, text: str, line: int):
        self.s, self.p, self.line = text, 0, line

    def _ws(self) -> None:
        s = self.s
        while self.p < len(s):
            c = s[self.p]
            if c in " \t\n":
                self.p += 1
            elif c == "#" and (self.p == 0 or s[self.p - 1] in " \t\n"):
                end = s.find("\n", self.p)
                self.p = len(s) if end < 0 else end
            else:
                return

    def _peek(self) -> str:
        if self.p >= len(self.s):
            raise _Incomplete()
        return self.s[self.p]

    def node(self):
        self._ws()
        c = self._peek()
        if c == "[":
            self.p += 1
            seq = Seq(self.line)
            while True:
                self._ws()
                if self._peek() == "]":
                    self.p += 1
                    return seq
                seq.append(self.node())
                seq.lines.append(self.line)
                self._ws()
                c = self._peek()
                if c == ",":
                    self.p += 1
                elif c != "]":
                    raise YamlError(self.line, f"unexpected '{c}' in a flow sequence")
        if c == "{":
            self.p += 1
            mapping = Map(self.line)
            while True:
                self._ws()
                if self._peek() == "}":
                    self.p += 1
                    return mapping
                key = self.node()
                if not isinstance(key, str):
                    raise YamlError(self.line, "complex keys are not supported")
                self._ws()
                value = ""
                if self._peek() == ":":
                    self.p += 1
                    self._ws()
                    if self._peek() not in ",}":
                        value = self.node()
                if key in mapping:
                    raise YamlError(self.line, f"duplicate key '{key}'")
                mapping[key] = value
                mapping.lines[key] = self.line
                self._ws()
                c = self._peek()
                if c == ",":
                    self.p += 1
                elif c != "}":
                    raise YamlError(self.line, f"unexpected '{c}' in a flow mapping")
        if c in "'\"":
            end = _closing_quote(self.s[self.p:], c)
            if end is None:
                raise _Incomplete()
            value = _quoted_value(self.s[self.p + 1:self.p + end], c == '"', self.line)
            self.p += end + 1
            return value
        if c in UNSUPPORTED_START:
            raise YamlError(self.line, f"YAML {UNSUPPORTED_START[c]} are not supported")
        if c in ",]}|>#?:" or (c == "-" and self.s[self.p + 1:self.p + 2] in ("", " ", "\t", "\n", ",", "]", "}")):
            raise YamlError(self.line, f"unexpected '{c}' in a flow collection (complex keys are not supported)")
        start = self.p
        s = self.s
        while self.p < len(s):
            c = s[self.p]
            if c in "?\t":  # YAML 1.1 and 1.2 parsers disagree about both here: refuse the ambiguity
                raise YamlError(self.line, f"{c!r} inside a flow collection's plain scalar: quote it")
            if c in ",[]{}":
                break
            if c == ":" and (self.p + 1 >= len(s) or s[self.p + 1] in " \t\n,[]{}"):
                break
            if c == "#" and s[self.p - 1] in " \t\n":
                break
            self.p += 1
        if self.p >= len(s):
            raise _Incomplete()
        lines = [part.strip(" \t") for part in s[start:self.p].split("\n")]
        out, empty = lines[0], 0
        for part in lines[1:]:  # line breaks fold to a space, or to one newline per empty line
            if not part:
                empty += 1
                continue
            out += ("\n" * empty if empty else " ") + part
            empty = 0
        return out


class _Parser:
    def __init__(self, text: str):
        if text.startswith("\ufeff"):
            text = text[1:]
        text = text.replace("\r\n", "\n")
        if "\r" in text:
            raise YamlError(text[:text.index("\r")].count("\n") + 1, "bare carriage return")
        bad = FORBIDDEN_CHARS.search(text)
        if bad:
            kind = "a line break to other YAML parsers" if bad[0] in "\x85\u2028\u2029" else "a control character"
            raise YamlError(text[:bad.start()].count("\n") + 1, f"U+{ord(bad[0]):04X} ({kind}) is not allowed")
        self.lines = text.split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()
        self.i = 0

    # -- lines ----------------------------------------------------------------------------------
    def _indent(self, k: int) -> int:
        line = self.lines[k]
        n = len(line) - len(line.lstrip(" "))
        if line[n:n + 1] == "\t":
            raise YamlError(k + 1, "tab in indentation")
        if line[:3] in ("---", "...") and line[3:4] in ("", " ", "\t"):
            raise YamlError(k + 1, "multiple documents are not supported")
        return n

    def _blank(self, k: int) -> bool:
        stripped = self.lines[k].strip(" \t")
        return stripped == "" or stripped.startswith("#")

    def _next(self) -> int | None:
        while self.i < len(self.lines) and self._blank(self.i):
            self.i += 1
        return self.i if self.i < len(self.lines) else None

    @staticmethod
    def _is_item(content: str) -> bool:
        return content == "-" or content.startswith("- ") or content.startswith("-\t")

    def _split_key(self, content: str, k: int) -> tuple[str, str] | None:
        """(key, rest after the colon) when `content` is a mapping entry."""
        c = content[:1]
        if c in ("'", '"'):
            end = _closing_quote(content, c)
            if end is None:
                return None
            after = content[end + 1:].lstrip(" \t")
            if after.startswith(":") and (len(after) == 1 or after[1] in " \t"):
                return _quoted_value(content[1:end], c == '"', k + 1), after[1:]
            return None
        if c == "?" and (len(content) == 1 or content[1] in " \t"):
            raise YamlError(k + 1, "complex keys ('? ') are not supported")
        if c in "[{" or self._is_item(content):
            return None
        m = re.search(r":(?=[ \t]|$)", content)
        if not m or re.search(r"(^|[ \t])#", content[:m.start()]):
            return None
        key = content[:m.start()].rstrip(" \t")
        if "\t" in key:
            raise YamlError(k + 1, "a tab inside a key: quote the key")
        if key[:1] in UNSUPPORTED_START:
            raise YamlError(k + 1, f"YAML {UNSUPPORTED_START[key[0]]} are not supported")
        if not key or key[0] in ",]}|>" or (key[0] in "-:" and key[1:2] in ("", " ", "\t")):
            raise YamlError(k + 1, f"a key cannot start with '{key[:1] or ':'}'")
        if key == "<<":
            raise YamlError(k + 1, "merge keys ('<<') are not supported")
        return key, content[m.end():]

    # -- document -------------------------------------------------------------------------------
    def document(self) -> Map:
        k = self._next()
        if k is None:
            raise YamlError(1, "empty workflow")
        if self.lines[k].startswith("%"):
            raise YamlError(k + 1, "YAML directives are not supported")
        if self.lines[k].rstrip(" \t") == "---":
            self.i += 1
            k = self._next()
            if k is None:
                raise YamlError(1, "empty workflow")
        node = self._block(self._indent(k), -1)
        k = self._next()
        if k is not None:
            raise YamlError(k + 1, "content after the document (a second document or bad indentation)")
        if not isinstance(node, Map):
            raise YamlError(1, "a workflow must be a mapping")
        return node

    def _block(self, n: int, parent: int):
        """The node whose first line (self.i) is indented by n."""
        k = self.i
        content = self.lines[k][n:]
        if self._is_item(content):
            return self._seq(n)
        if self._split_key(content, k) is not None:
            return self._map(n)
        self.i = k + 1
        return self._value(content, parent, k, False)

    def _map(self, n: int) -> Map:
        mapping = Map(self.i + 1)
        while (k := self._next()) is not None:
            indent = self._indent(k)
            if indent < n:
                break
            if indent > n:
                raise YamlError(k + 1, "unexpected indentation")
            content = self.lines[k][n:]
            if content.rstrip(" \t") in ("---", "..."):
                raise YamlError(k + 1, "multiple documents are not supported")
            entry = self._split_key(content, k)
            if entry is None:
                raise YamlError(k + 1, "expected 'key: value'")
            key, rest = entry
            if key in mapping:
                raise YamlError(k + 1, f"duplicate key '{key}'")
            self.i = k + 1
            mapping[key] = self._value(rest, n, k, True)
            mapping.lines[key] = k + 1
        return mapping

    def _seq(self, n: int) -> Seq:
        seq = Seq(self.i + 1)
        while (k := self._next()) is not None:
            indent = self._indent(k)
            if indent < n:
                break
            if indent > n:
                raise YamlError(k + 1, "unexpected indentation")
            content = self.lines[k][n:]
            if not self._is_item(content):
                break
            rest = content[1:]
            r = rest.lstrip(" \t")
            if r and not r.startswith("#") and (self._is_item(r) or self._split_key(r, k) is not None):
                # A compact nested node (`- key: v` or `- - x`): re-read the line from the item's column.
                col = n + 1 + len(rest) - len(r)
                if "\t" in rest[:len(rest) - len(r)]:
                    raise YamlError(k + 1, "tab in indentation")
                self.lines[k] = " " * col + r
                self.i = k
                item = self._block(col, n)
            else:
                self.i = k + 1
                item = self._value(rest, n, k, False)
            seq.append(item)
            seq.lines.append(k + 1)
        return seq

    def _value(self, rest: str, n: int, k: int, same_indent_seq: bool):
        """The value after `key:` or `- ` on line k, whose parent is indented by n."""
        r = rest.strip(" \t")
        if r == "" or r.startswith("#"):
            nxt = self._next()
            if nxt is None:
                return ""
            indent = self._indent(nxt)
            if indent > n:
                return self._block(indent, n)
            if indent == n and same_indent_seq and self._is_item(self.lines[nxt][n:]):
                return self._seq(n)
            return ""
        c = r[0]
        if c in "|>":
            return self._block_scalar(r, n, k)
        if c in "[{":
            return self._flow(r, k)
        if c in ("'", '"'):
            return self._quoted(r, k)
        if c in UNSUPPORTED_START:
            raise YamlError(k + 1, f"YAML {UNSUPPORTED_START[c]} are not supported")
        if c in ",]}" or (c in "?-:" and (len(r) == 1 or r[1] in " \t")):
            raise YamlError(k + 1, f"unexpected '{c}'")
        return self._plain(r, n, k)

    def _plain(self, r: str, n: int, k: int) -> str:
        m = re.search(r"[ \t]#", r)
        text = (r[:m.start()] if m else r).rstrip(" \t")
        parts = [text]
        self._check_plain(text, k)
        if not m:  # a comment ends a plain scalar; otherwise more-indented lines continue it
            while True:
                j, empty = self.i, 0
                while j < len(self.lines) and self.lines[j].strip(" \t") == "":
                    j, empty = j + 1, empty + 1
                if j >= len(self.lines) or self._indent(j) <= n:
                    break
                line = self.lines[j].strip(" \t")
                if line.startswith("#"):
                    break
                cm = re.search(r"[ \t]#", line)
                line = (line[:cm.start()] if cm else line).rstrip(" \t")
                self._check_plain(line, j)
                parts.append("\n" * empty if empty else " ")
                parts.append(line)
                self.i = j + 1
                if cm:
                    break
        return "".join(parts)

    @staticmethod
    def _check_plain(text: str, k: int) -> None:
        if re.search(r":[ \t]", text) or text.endswith(":"):
            raise YamlError(k + 1, "': ' inside a plain scalar: quote the value")
        if "\t" in text:  # parsers disagree about tabs in plain scalars
            raise YamlError(k + 1, "a tab inside a plain scalar: quote the value")

    def _quoted(self, r: str, k: int) -> str:
        q, text, last = r[0], r, k
        while (end := _closing_quote(text, q)) is None:
            last += 1
            if last >= len(self.lines):
                raise YamlError(k + 1, "unterminated quoted scalar")
            text += "\n" + self.lines[last]
        after = text[end + 1:].strip(" \t")
        if after and not after.startswith("#"):
            raise YamlError(last + 1, f"unexpected text after a quoted scalar: '{after}'")
        self.i = last + 1
        return _quoted_value(text[1:end], q == '"', k + 1)

    def _flow(self, r: str, k: int):
        text, last = r, k
        while True:
            parser = _Flow(text, k + 1)
            try:
                node = parser.node()
                parser._ws()
                if parser.p < len(text):
                    raise YamlError(last + 1, f"unexpected text after a flow collection: '{text[parser.p:]}'")
                break
            except _Incomplete:
                last += 1
                if last >= len(self.lines):
                    raise YamlError(k + 1, "unterminated flow collection") from None
                text += "\n" + self.lines[last]
        self.i = last + 1
        return node

    def _block_scalar(self, r: str, n: int, k: int) -> str:
        m = re.fullmatch(r"([|>])([+-]?)([1-9]?)([+-]?)(?:[ \t]+#.*)?[ \t]*", r)
        if not m or (m[2] and m[4]):
            raise YamlError(k + 1, f"bad block scalar header '{r}'")
        folded, chomp = m[1] == ">", m[2] or m[4]
        lines, start = self.lines, k + 1
        if m[3]:
            indent = n + int(m[3])
        else:  # the first non-empty line's indentation (and that of any longer empty line before it)
            widest, j = 0, start
            while j < len(lines) and lines[j].strip(" ") == "":
                widest = max(widest, len(lines[j]))
                j += 1
            if j < len(lines):
                widest = max(widest, len(lines[j]) - len(lines[j].lstrip(" ")))
            indent = max(n + 1, widest)

        def kind(j: int) -> str:
            line = lines[j]
            spaces = len(line) - len(line.lstrip(" "))
            if spaces == len(line):
                return "break" if len(line) <= indent else "content"
            return "content" if spaces >= indent else "end"

        chunks, breaks, line_break, j = [], [], "", start
        while j < len(lines) and kind(j) == "break":
            breaks.append("\n")
            j += 1
        while j < len(lines) and kind(j) == "content":
            chunks.extend(breaks)
            content = lines[j][indent:]
            leading_blank = content[:1] in (" ", "\t")
            chunks.append(content)
            line_break, j, breaks = "\n", j + 1, []
            while j < len(lines) and kind(j) == "break":
                breaks.append("\n")
                j += 1
            if j < len(lines) and kind(j) == "content":
                if folded and not leading_blank and lines[j][indent:][:1] not in (" ", "\t"):
                    if not breaks:
                        chunks.append(" ")
                else:
                    chunks.append(line_break)
            else:
                break
        if chomp != "-":
            chunks.append(line_break)
        if chomp == "+":
            chunks.extend(breaks)
        self.i = j
        return "".join(chunks)


def parse_yaml(text: str) -> Map:
    """Parses a workflow file. Scalars are strings (as YAML's failsafe schema reads them; an empty value
    is ""), mappings are Map and sequences Seq, both with line numbers. Raises YamlError."""
    return _Parser(text).document()


# ------------------------------------------------------------------------------------------------
# The policy
# ------------------------------------------------------------------------------------------------
class Finding:
    def __init__(self, path: str, line: int, rule: str, message: str):
        self.path, self.line, self.rule, self.message = path, line, rule, message

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.message}"


def _strings(node, line: int, key: str | None = None):
    """(the mapping key a scalar is the value of, or None; the text; its line) for every key and scalar."""
    if isinstance(node, Map):
        for k, value in node.items():
            k_line = node.lines.get(k, line)
            yield None, k, k_line
            yield from _strings(value, k_line, k)
    elif isinstance(node, Seq):
        for value, item_line in zip(node, node.lines):
            yield from _strings(value, item_line)
    elif isinstance(node, str):
        yield key, node, line


SECRETS_RE = re.compile(r"(?<![\w.-])secrets(?![\w-])", re.I)
GITHUB_RE = re.compile(r"(?<![\w.-])github(?![\w-])\s*(?:\.\s*([A-Za-z_][\w-]*)|\[\s*(?:'([^']*)'\s*\])?)?", re.I)


def _credential(expr: str) -> bool:
    """Whether an expression can read a credential: the `secrets` context in any form, `github.token`, or the
    whole `github` context (`toJSON(github)` includes the token) or an index into it that is not a literal."""
    if SECRETS_RE.search(expr):
        return True
    for m in GITHUB_RE.finditer(expr):
        name = m.group(1) if m.group(1) is not None else m.group(2)  # None: the bare context or a computed index
        if name is None or name.lower() == "token":
            return True
    return False


def _expressions(key: str | None, text: str) -> list[str]:
    """The expression text in a value: all of an `if:` value, else every `${{ … }}`. As GitHub's template reader
    does, an expression ends at the first `}}` outside a '…' string literal (`''` toggles twice), so a `}}` inside
    a literal does not end it. An unclosed `${{` (which GitHub refuses) runs to the end of the value."""
    if key == "if":
        return [text]
    out, start = [], text.find("${{")
    while start >= 0:
        i, quoted, end = start + 3, False, len(text)
        while i < len(text):
            if text[i] == "'":
                quoted = not quoted
            elif not quoted and text.startswith("}}", i):
                end = i
                break
            i += 1
        out.append(text[start + 3:end])
        start = text.find("${{", end + 2)
    return out


def _label_sets(value, matrix) -> tuple[list[frozenset[str]] | None, str]:
    """The label sets `runs-on` can take, or (None, why) when they cannot be known statically."""
    expr = re.compile(r"\$\{\{\s*matrix\.([A-Za-z_][\w-]*)\s*\}\}")

    def labels(item) -> list[list[str]] | None:  # alternatives of one label entry
        if not isinstance(item, str):
            return None
        if "${{" not in item:
            return [[item]]
        m = expr.fullmatch(item.strip())
        if not m or not isinstance(matrix, Map):
            return None
        values = []
        axis = matrix.get(m[1])
        if isinstance(axis, Seq):
            values += list(axis)
        elif axis is not None:
            return None
        include = matrix.get("include")
        if include is not None and not isinstance(include, Seq):
            return None
        for entry in include or []:
            if not isinstance(entry, Map):
                return None
            if m[1] in entry:
                values.append(entry[m[1]])
        out = []
        for v in values:
            if isinstance(v, str) and "${{" not in v:
                out.append([v])
            elif isinstance(v, Seq) and all(isinstance(x, str) and "${{" not in x for x in v):
                out.append(list(v))
            else:
                return None
        return out or None

    group = None
    if isinstance(value, Map):
        group = value.get("group")
        value = value.get("labels", Seq(0))
    entries = value if isinstance(value, Seq) else [value]
    combos: list[list[str]] = [[]]
    for entry in entries:
        alternatives = labels(entry)
        if alternatives is None:
            return None, f"runs-on entry {entry!r} is an expression this check cannot resolve"
        combos = [c + a for c in combos for a in alternatives]
    sets = [frozenset(_fold(x) for x in c) for c in combos]
    if group is not None:
        return None, "runs-on names a runner group, whose runners this check cannot see"
    return sets, ""


def _fold(text: str) -> str:
    """A runner label or job id as compared: without surrounding blanks and in the widest case folding (upper,
    then lower: the dotless i and the long s upper-case to I and S), so that any name that might match does."""
    return text.strip().upper().lower()


def _reaches_runner(sets: list[frozenset[str]]) -> bool:
    return any("win-gpu" in s or (s and s <= RUNNER_LABELS) for s in sets)


def _conjuncts(expr: str) -> list[str] | None:
    """The top-level `&&` terms of an expression, flattened through parentheses; None if a top-level
    `||` makes it a disjunction or it does not tokenize."""
    terms, depth, start, i = [], 0, 0, 0
    while i < len(expr):
        c = expr[i]
        if c == "'":
            j = i + 1
            while True:
                j = expr.find("'", j)
                if j < 0:
                    return None
                if expr[j + 1:j + 2] == "'":
                    j += 2
                    continue
                break
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth < 0:
                return None
        elif depth == 0 and expr.startswith("&&", i):
            terms.append(expr[start:i])
            i += 2
            start = i
            continue
        elif depth == 0 and expr.startswith("||", i):
            return None
        i += 1
    if depth != 0:
        return None
    terms.append(expr[start:])
    out = []
    for term in terms:
        term = term.strip()
        if term.startswith("(") and term.endswith(")") and _wrapped(term):
            inner = _conjuncts(term[1:-1])
            out += inner if inner is not None else [term]
        else:
            out.append(term)
    return out


def _wrapped(term: str) -> bool:
    """True when the parenthesis at term[0] closes at term[-1]."""
    depth, quoted = 0, False
    for i, c in enumerate(term):
        if c == "'":
            quoted = not quoted
        elif not quoted and c == "(":
            depth += 1
        elif not quoted and c == ")":
            depth -= 1
            if depth == 0 and i != len(term) - 1:
                return False
    return True


def _normalized(term: str) -> str:
    """A term without whitespace outside string literals, lower case (GitHub compares strings and
    context names case-insensitively)."""
    out, quoted = [], False
    for c in term:
        if c == "'":
            quoted = not quoted
        if quoted or not c.isspace():
            out.append(c)
    return "".join(out).lower()


def _guard_findings(job: Map) -> list[tuple[int, str, str]]:
    line = job.lines.get("if", job.line)
    value = job.get("if")
    if not isinstance(value, str) or not value.strip():
        return [(line, rule, f"no `if:` guard; use `{GUARD_TEXT}`") for rule in REQUIRED_TERMS]
    # With ${{ }}, GitHub keeps the scalar's text around it, blanks and a block scalar's final line break included
    # (format('{0}\n', ...)), and a non-empty string is true; so only an exact `${{ ... }}` is an expression.
    # Without, the whole value is the expression, and the expression lexer skips blanks.
    text = value if "${{" in value else value.strip()
    m = re.fullmatch(r"\$\{\{(.*)\}\}", text, re.S)
    if m and "${{" not in m[1] and "}}" not in m[1]:
        text = m[1]
    elif "${{" in text:
        return [(line, rule, "`if:` has text around `${{ }}` (even a blank, or a `|` or `>` block's final line "
                 f"break), which makes a non-empty string that is always true; use `{GUARD_TEXT}`")
                for rule in REQUIRED_TERMS]
    terms = _conjuncts(text)
    if terms is None:
        return [(line, rule, "`if:` is not a conjunction (a top-level `||`, or unbalanced quotes or "
                 f"parentheses), so it cannot require both guards; use `{GUARD_TEXT}`") for rule in REQUIRED_TERMS]
    have = {_normalized(t) for t in terms}
    out = []
    for rule, (text, forms) in REQUIRED_TERMS.items():
        if not have & set(forms):
            out.append((line, rule, f"`if:` does not require `{text}` as a top-level `&&` term; use `{GUARD_TEXT}`"))
    return out


def _permission_problem(perms) -> str | None:
    if perms is None or perms == "":
        return "no `permissions:` for the job or the workflow, so it gets the repository's default token"
    if isinstance(perms, Map):
        broad = [f"{k}: {v}" for k, v in perms.items()
                 if not (v == "none" or (k == "contents" and v == "read"))]
        return f"permissions broader than `contents: read`: {', '.join(broad)}" if broad else None
    return f"permissions `{perms}` are broader than `contents: read`"


def _pinned(uses: str) -> bool:
    if uses.startswith("./"):
        return True
    if uses.startswith("docker://"):
        return re.search(r"@sha256:[0-9a-f]{64}$", uses) is not None
    return re.fullmatch(r"[\w.-]+/[\w./-]+@[0-9a-f]{40}", uses) is not None


def _needs(jobs: Map, job_id: str) -> tuple[list[str], list[tuple[int, str]]]:
    """(job_id and every job it needs, transitively; problems as (line, message)). A `needs` id matches every job
    whose id _fold()s to the same text, a superset of what GitHub may match. An id that matches no job, or a
    `needs` that is neither an id nor a list of ids, is a problem: what it names is unknown."""
    by_id: dict[str, list[str]] = {}
    for j in jobs:
        by_id.setdefault(_fold(j), []).append(j)
    seen, todo, problems = [], [job_id], []
    while todo:
        j = todo.pop()
        if j in seen or not isinstance(jobs.get(j), Map):
            continue
        seen.append(j)
        job = jobs[j]
        needs = job.get("needs", "")
        line = job.lines.get("needs", job.line)
        if needs == "":
            continue
        for name in list(needs) if isinstance(needs, Seq) else [needs]:
            if not isinstance(name, str):
                problems.append((line, f"job `{j}` has a `needs` entry that is not a job id"))
            elif _fold(name) not in by_id:
                problems.append((line, f"job `{j}` needs `{name}`, which is not a job of this workflow"))
            else:
                todo += by_id[_fold(name)]
    return seen, problems


def _events(on) -> Map | None:
    if isinstance(on, str):
        events = Map(0)
        events[on] = ""
        return events
    if isinstance(on, Seq):
        events = Map(on.line)
        for item, line in zip(on, on.lines):
            if not isinstance(item, str):
                return None
            events[item] = ""
            events.lines[item] = line
        return events
    return on if isinstance(on, Map) else None


def _trigger_findings(workflow: Map) -> list[tuple[int, str]]:
    on_line = workflow.lines.get("on", 1)
    events = _events(workflow.get("on"))
    if events is None:
        return [(on_line, "`on:` is missing or not a string, list or mapping of events")]
    out = []
    for event, config in events.items():
        line = events.lines.get(event, on_line)
        if event not in ALLOWED_EVENTS:
            out.append((line, f"trigger `{event}`: win-gpu jobs may run only on `schedule`, `workflow_dispatch` "
                        "and `push` to main, so that only merged code reaches the owner's PC"))
        elif event == "push":
            if not isinstance(config, Map):
                out.append((line, "`push` without `branches: [main]` runs on pushes to every branch"))
                continue
            extra = sorted(set(config) - PUSH_FILTERS_OK)
            branches = config.get("branches")
            if extra:
                out.append((config.lines.get(extra[0], line),
                            f"`push` filter `{extra[0]}`: only `branches: [main]` (and paths filters) are allowed"))
            if not ((isinstance(branches, Seq) and list(branches) == ["main"]) or branches == "main"):
                out.append((config.lines.get("branches", line), "`push` must be restricted to `branches: [main]`"))
        elif event == "schedule" and not isinstance(config, Seq):
            out.append((line, "`schedule` must be a list of `cron:` entries"))
    return out


def check_workflow(path: Path, display: str) -> tuple[list[Finding], list[str]]:
    """(findings, runner jobs) of one workflow file."""
    try:
        workflow = parse_yaml(path.read_text(encoding="utf-8"))
    except YamlError as e:
        return [Finding(display, e.line, "parse", f"{e.message} (this check accepts a strict YAML subset and "
                        "fails closed; see tools/ci/check_runner_policy.py)")], []
    except (OSError, UnicodeDecodeError) as e:
        return [Finding(display, 1, "parse", f"cannot read the workflow: {e}")], []
    findings: list[Finding] = []
    jobs = workflow.get("jobs")
    if not isinstance(jobs, Map):
        return [Finding(display, workflow.lines.get("jobs", 1), "parse", "`jobs:` is missing or not a mapping")], []
    runner_jobs = []
    for job_id, job in jobs.items():
        line = jobs.lines[job_id]
        if not isinstance(job, Map):
            findings.append(Finding(display, line, "parse", f"job `{job_id}` is not a mapping"))
            continue
        if "uses" in job:
            uses = job["uses"]
            if not (isinstance(uses, str) and LOCAL_WORKFLOW_RE.fullmatch(uses)):
                findings.append(Finding(display, job.lines["uses"], "reusable",
                                        f"job `{job_id}` calls `{uses}`: a reusable workflow outside this "
                                        "repository's ./.github/workflows/ may run jobs on win-gpu unseen"))
            continue
        if "runs-on" not in job:
            findings.append(Finding(display, line, "runs-on",
                                    f"job `{job_id}` has neither `runs-on` nor `uses`, so this check cannot tell "
                                    "where it runs"))
            continue
        strategy = job.get("strategy")
        sets, why = _label_sets(job["runs-on"], strategy.get("matrix") if isinstance(strategy, Map) else None)
        if sets is None:
            findings.append(Finding(display, job.lines["runs-on"], "runs-on",
                                    f"job `{job_id}`: {why}, so it could reach win-gpu; use literal labels or "
                                    "`${{ matrix.<axis> }}` over literal matrix values"))
            continue
        if _reaches_runner(sets):
            runner_jobs.append(job_id)
    if not runner_jobs:
        return findings, []

    # Workflow-level rules, once per workflow.
    for line, message in _trigger_findings(workflow):
        findings.append(Finding(display, line, "trigger", message))
    top = Map(1)
    for key, value in workflow.items():
        if key != "jobs":
            top[key] = value
            top.lines[key] = workflow.lines[key]
    for job_id in runner_jobs:
        job = jobs[job_id]
        for line, rule, message in _guard_findings(job):
            findings.append(Finding(display, line, rule, f"job `{job_id}`: {message}"))
        perms = job.get("permissions", workflow.get("permissions"))
        problem = _permission_problem(perms)
        if problem:
            line = job.lines.get("permissions", workflow.lines.get("permissions", jobs.lines[job_id]))
            findings.append(Finding(display, line, "permissions", f"job `{job_id}`: {problem}"))
        chain, problems = _needs(jobs, job_id)
        for line, message in problems:
            findings.append(Finding(display, line, "secrets",
                                    f"job `{job_id}`: {message}, so this check cannot follow what reaches the job"))
        for j in chain:
            if "uses" in jobs[j]:
                findings.append(Finding(display, jobs[j].lines["uses"], "secrets",
                                        f"job `{job_id}` needs `{j}`, which calls the reusable workflow "
                                        f"`{jobs[j]['uses']}`: its jobs' secrets (environment secrets need no "
                                        "`secrets:`) can reach the runner through its outputs, out of this check's "
                                        "sight; a win-gpu job may not need a reusable-workflow call"))
        scopes = [("the workflow's top level", top)] + [
            ("the job" if j == job_id else f"job `{j}` (a job it needs)", jobs[j]) for j in chain]
        for scope, node in scopes:
            if isinstance(node, Map) and "secrets" in node and node is not top:
                findings.append(Finding(display, node.lines["secrets"], "secrets",
                                        f"job `{job_id}`: {scope} passes `secrets:` to a reusable workflow"))
            for key, text, line in _strings(node, getattr(node, "line", 1)):
                if any(_credential(e) for e in _expressions(key, text)):
                    findings.append(Finding(display, line, "secrets",
                                            f"job `{job_id}`: {scope} references `secrets` or `github.token` "
                                            "in an expression; no credential may reach win-gpu jobs"))
        steps = job.get("steps")
        for step, step_line in zip(steps, steps.lines) if isinstance(steps, Seq) else ():
            uses = step.get("uses") if isinstance(step, Map) else None
            if isinstance(uses, str) and not _pinned(uses):
                findings.append(Finding(display, step.lines.get("uses", step_line), "pinning",
                                        f"job `{job_id}`: `uses: {uses}` is not pinned to a full commit SHA "
                                        "(or is not a local ./ action)"))
    # One finding per place and rule (a job and the jobs it needs can share a scope).
    unique, seen = [], set()
    for f in findings:
        if (f.line, f.rule, f.message) not in seen:
            seen.add((f.line, f.rule, f.message))
            unique.append(f)
    return unique, runner_jobs


def workflow_files(directory: Path) -> list[Path]:
    """Every workflow GitHub reads: *.yml and *.yaml directly in the directory."""
    return sorted(p for p in directory.iterdir() if p.is_file() and p.suffix.lower() in (".yml", ".yaml"))


def _display(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--workflows", type=Path, help="a directory of workflows (default: .github/workflows)")
    parser.add_argument("files", nargs="*", type=Path, help="workflow files to check instead of a directory")
    args = parser.parse_args(argv)
    if args.files and args.workflows:
        parser.error("pass a directory or files, not both")
    if args.files:
        files = args.files
    else:
        directory = args.workflows or ROOT / ".github" / "workflows"
        if not directory.is_dir():
            print(f"check_runner_policy: {directory} is not a directory", file=sys.stderr)
            return 2
        files = workflow_files(directory)
    findings, runner_jobs = [], []
    for path in files:
        display = _display(path)
        found, jobs = check_workflow(path, display)
        findings += found
        runner_jobs += [f"{display}:{j}" for j in jobs]
    for f in findings:
        print(f)
    jobs_text = ", ".join(runner_jobs) or "none"
    if findings:
        print(f"runner policy: {len(findings)} finding(s) in {len(files)} workflow(s); jobs that reach win-gpu: "
              f"{jobs_text} (09 §5.4a)")
        return 1
    print(f"runner policy: OK, {len(files)} workflow(s); jobs that reach win-gpu: {jobs_text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
