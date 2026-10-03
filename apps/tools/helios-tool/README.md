# helios-tool

The headless ToolsFramework CLI (07 §1.1). It links `engine/toolsfw` and the sample record types
only, with no EditorUI and no RHI (`CMakeLists.txt` walks its whole link closure at the end of
configure and fails on EditorUI, any other non-HEADLESS module or a graphics library), so it builds
in the headless preset and runs on build agents. Every edit is a transaction with origin `cli`, journaled like an
editor session.

```
helios-tool [--project-root=<dir>] [--project=<name>] [--user=<name>] [--journal-dir=<dir>]
            [--no-journal] [--log-level=<level>] <verb> [arguments]
```

| Verb | What it does |
|---|---|
| `validate [--json]` | Schema validation of every record; exit 1 when there are errors |
| `fmt [--check]` | Rewrites record files in canonical JSONC (atomically, like a save); `--check` only reports (exit 1) |
| `apply <doc> <path> <json>` | `doc.setProperty` on one property, then save |
| `apply --command=<id> [--args=<json>]` | Any command |
| `apply --file=<jsonl>` | `{"command", "args"}` lines as one transaction; `--no-save` leaves the files alone (the edit lives in the journal only) |
| `undo [--steps=n]`, `redo [--steps=n]` | Revert or re-apply helios-tool's own last transactions **across processes**. The linear CLI undo stack is rebuilt from the project's journals: Do pushes, Undo pops, Redo moves back. Only transactions that reached the files count (every document they touch is saved later in the same session), so a journal-only `--no-save` edit is never on the stack. Each revert is committed with its kind and target (`TxBuilder::markRevert`), and every run continues the project's Lamport counter, so transaction ids never repeat. If the file changed since, the op's precondition fails (exit 3) and nothing is written |
| `journal list [--all]` | The project's sessions (unclean ones only without `--all`) |
| `journal show <file|latest> [--json]`, `journal verify <file|latest>` | Records of a journal, and its integrity (torn tail, clean end) |
| `journal replay <file|auto> [--save] [--ignore-source-changes]` | Crash recovery without the editor |
| `run <script.luau> [--no-save]` | A Luau automation script (`Editor.*`, `Record.*`, `Validate.*`) |
| `commands [--json]` | The command registry |

Exit codes: 0 ok, 1 validation or check failed, 2 usage error, 3 the operation failed or conflicted.

`helios_tool_cli` (CTest, `tests/cli_test.cmake`, every platform) runs the verbs over a copy of the
sample project (`engine/toolsfw/samples/project`). Among its checks: `apply`, then `undo`, `undo`
restores the file byte for byte, `redo --steps=2`, a refused undo after a conflicting external edit,
batch transactions, an undo that skips a journal-only edit (with distinct transaction ids), a
journal-only edit replayed from its journal and then undone, and a Luau script.

## Plan conformance

Plan-Rev: 11

Written for plan revision 6 (07 §1.1, §1.2; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.
