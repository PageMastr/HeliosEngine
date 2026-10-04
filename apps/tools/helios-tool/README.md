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
| `apply --file=<jsonl>` | `{"command", "args"}` lines as one transaction; `--no-save` leaves the files alone (the edit lives in the journal only). The batch runs as one transaction group, so a `doc.save` line for a document the batch has edited is refused (exit 3, nothing written): the files are saved once the group has committed |
| `undo [--steps=n]`, `redo [--steps=n]` | Revert or re-apply helios-tool's own last transactions **across processes**. The linear CLI undo stack is rebuilt from the project's journals: Do pushes, Undo pops, Redo moves back. Only transactions that reached the files count (every document they touch is saved later in the same session), so a journal-only `--no-save` edit is never on the stack. Each revert is committed with its kind and target (`TxBuilder::markRevert`), and every run continues the project's Lamport counter, so transaction ids never repeat across runs made one after another (runs that overlap, or the editor and the CLI at once, can repeat an id; the undo stack assumes runs do not overlap). If the file changed since, the op's precondition fails (exit 3) and nothing is written. Each run reads the project's journals at start-up; nothing prunes clean sessions yet, so that cost grows with the project's history (follow-up) |
| `journal list [--all]` | The project's sessions (unclean ones only without `--all`) |
| `journal show <file|latest> [--json]`, `journal verify <file|latest>` | Records of a journal, and its integrity (torn tail, clean end). Strings from the journal are printed with control characters escaped; `--json` writes DEL and C1 as `\u00NN` (JSON escapes C0) |
| `journal replay <file|auto> [--save] [--ignore-source-changes] [--allow-other-project]` | Crash recovery without the editor. A journal is untrusted input (engine/toolsfw README, "The journal is untrusted input"): every file it names must be a `.hrec` inside the project (no `..`, absolute, drive-letter, UNC or device path, no link out of the project), and its header must name this project; `--allow-other-project` accepts a journal of a project that has since been renamed. One bad entry refuses the whole replay (exit 3) and nothing is read or written. Read a journal you did not write first (`journal show`), and `validate` after replaying it |
| `run <script.luau> [--no-save]` | A Luau automation script (`Editor.*`, `Record.*`, `Validate.*`) |
| `commands [--json]` | The command registry |

Exit codes: 0 ok, 1 validation or check failed, 2 usage error, 3 the operation failed or conflicted.
Errors, the replay report and the journal verbs print control characters escaped (`tf::printable`),
since they can quote a journal (engine/toolsfw README, "Output").

`helios_tool_cli` (CTest, `tests/cli_test.cmake`, every platform) runs the verbs over a copy of the
sample project (`engine/toolsfw/samples/project`). Among its checks: `apply`, then `undo`, `undo`
restores the file byte for byte, `redo --steps=2`, a refused undo after a conflicting external edit,
batch transactions, an undo that skips a journal-only edit (with distinct transaction ids), a
journal-only edit replayed from its journal and then undone, a replay refused for another
project's journal and then allowed with `--allow-other-project`, `doc.create` refused outside the
project and for a non-`.hrec` file, terminal escape sequences that `--user` and `--project` put
into journals and that `journal show` (text and `--json`), the replay report and a refusal print
escaped, and a Luau script. Crafted journals (paths outside the project)
are tested in `toolsfw_tests` (`test_confine.cpp`), since `journal replay` is `Framework::recover`.

## Plan conformance

Plan-Rev: 12

Written for plan revision 6 (07 §1.1, §1.2; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this directory.
