# engine/toolsfw — ToolsFramework (`helios::tf`)

The UI-less core of the editor (07 §1.2, ADR-009). Layer 4, **EDITOR_ONLY**: it is linked by
`helios-editor` and `helios-tool`, and never by the client, launcher, bot or servers
(`cmake/HeliosLayering.cmake` checks this at configure time). It includes no ImGui or RHI header.
Its dependencies are `core` and `reflect`, plus `script` privately for the Luau automation VM.

| Header | What it provides |
|---|---|
| `document.h` | `Document` (a record file over reflected data: `$rid`/`$name`/`$comment` header, canonical JSONC text, revision, content hash, dirty state) and `Workspace` (lookup by GUID, `$name`, relative path or path) |
| `transaction.h` | `Op` (Set, Insert, Remove, Move, Create, Destroy with before/after values and an exact inverse), `Transaction` (id = user + lamport, kind Do/Undo/Redo, target, origin, label, merge key) and JSON (de)serialization |
| `framework.h` | `Framework`: open/save/close/reload (3-way merge), `TxBuilder` (property-path edits, reflection diffs, raw ops), commit pipeline, history with undo/redo, groups and gesture merging, events, recovery |
| `command.h` | `CommandBus`, `CommandDesc` (id, label, shortcut, arguments, `headless`, `paletteOnly`), `CommandInvoker` per input path (07 command sources: `ui`, `ui-scripted`, `luau`, `rpc`, `cli`, plus `import` and `collab`) |
| `selection.h` | Selection with back/forward history and named sets |
| `journal.h` | Crash-recovery journal: writer with group commit, reader, sessions, unclean-session discovery |
| `validate.h` | Schema validation (`@range`, finite floats, `@max`, key uniqueness, headers) |
| `rpc.h`, `ipc.h` | JSON-RPC 2.0 remote control over a local named pipe (Windows) or Unix socket (Linux), and the framework's RPC methods |
| `automation.h` | The Luau editor VM (`HostProfile::Editor`): `Editor.cmd/transaction/undo/redo/find/log`, `Record.get/set`, `Validate.run` |
| `json_util.h` | Small JSON helpers shared with the editor UI and the tools |
| `samples/` (`helios_toolsfw_samples`) | The sample record types (`schemas/sample/*.hschema`), the Frigate hull record, and `samples/project`, the sample project (a Frigate hull and an item record) that `helios-tool`'s CLI test, the editor's tests and the ED-15 goldens open |

## Model

- **Every edit is a transaction.** A command runs through a `CommandInvoker` of one input path and
  commits a `Transaction` whose ops are property-path edits (`handling/yawRate`,
  `thrusters[#<32 hex key>]/maxForce`, `hardpoints[1]`). Undo commits the inverse ops as a new
  transaction (kind Undo, with `target`), so the log and the journal are append-only.
- **Byte-identical round trips.** Documents serialize to canonical JSONC through `reflect`. Applying
  and then undoing any transaction restores the canonical text byte for byte. Preconditions are
  checked on every op: a Set whose `before` no longer matches fails with `InvalidState` and changes
  nothing.
- **History.** Continuous gestures share a merge key and coalesce into one undo step. Groups
  (`beginGroup`/`endGroup`, and Luau's `Editor.transaction`) commit many commands as one step.
  Groups nest: cancelling a nested level (a failed inner `Editor.transaction` caught with `pcall`)
  rolls back only that level's edits, and while a group is open only its own input path's invoker
  may run commands, so the group's origin is the origin of every edit in it. A new edit drops the
  redo entries of the documents it touches. The history is capped at 512 MB (past the cap, the
  oldest entries go until it is at 7/8 of it).
- **Commit budget.** A commit, undo or redo costs O(its ops), whatever the session's length: the
  history and log byte totals are running sums. A one-op commit stays ≤ 0.1 ms in RelWithDebInfo
  (`perf:` case in `test_history.cpp`, which also requires the last 1,000 of 50,000 commits to cost
  at most 3× the first 1,000).
- **Journal** (07 §1.2). Each session appends to `<journal root>/<project>/<session>.hjl`. The
  format is `"HJL1"`, then the header length and header JSON, then records of the form
  `u32 length | u32 check | JSON payload`. Record kinds are open, save, close, tx and end. A
  transaction is journaled **before** it becomes visible: if the write fails, the edit is rolled
  back. Every record is one `write()`, so a crash or `kill -9` loses at most the record in flight.
  A flusher thread makes appends durable within 50 ms (group commit); it calls `fsync` without
  holding the writer's lock, so a commit never waits for the disk. A reload of an external edit
  journals a new open record (the file's new hash, plus the merged text when unsaved local edits
  survived the merge). `Framework::recover()` replays an unclean session onto the files. It
  verifies each file against the session's last open/save/reload hash, restores a reload's merged
  text, re-opens documents created in the session from their snapshot, and keeps each
  transaction's origin, label, merge key and Undo/Redo kind. Two limits are reported, not hidden:
  a recovered Undo or Redo becomes an ordinary history entry in the new session, and only the
  recovered documents' ops of a multi-document transaction are replayed. Default session names are
  `<yyyymmdd-hhmmss>-<pid>`, with `-2`, `-3`, ... when that journal already exists. The journal root is
  `HELIOS_JOURNAL_DIR`, else `%LOCALAPPDATA%\Helios\journal` on Windows or
  `$XDG_STATE_HOME/helios/journal` (else `~/.local/state/helios/journal`) on Linux.
- **Cross-process undo** (`helios-tool undo`). `TxBuilder::markRevert(kind, target)` records a
  transaction as the Undo or Redo of a transaction from an earlier session. The CLI rebuilds its
  linear undo stack from the project's journals, counting only transactions whose documents were
  saved afterwards in the same session, and continues the project's Lamport counter
  (`FrameworkConfig::lamportFloor`) so ids never repeat across its processes.
- **Remote control.** `RpcServer` accepts on its own threads and queues requests. Handlers run on
  the owner thread in `pump()`, and a handler may answer frames later through its `RpcResponder`
  (the editor's `ui.*` methods do). Endpoints are `\\.\pipe\<name>` (remote clients rejected; the
  DACL admits only the creating user and LocalSystem) or `$XDG_RUNTIME_DIR/<name>.sock` (mode 0600).
  Only the user who started the process can connect, so the endpoint trusts its peer like the
  command line. A buggy or stuck peer is still contained (`RpcServerLimits`): at most 8
  connections; per connection at most 64 requests and 64 MiB of request text queued or unanswered
  (beyond that the server stops reading, so the client blocks in its own write), 64 MiB per message
  and JSON nesting ≤ 128; and responses go through a per-connection queue drained by a writer
  thread, so `pump()` never blocks on a client, and a client that leaves more than 64 MiB unread is
  disconnected.

Threading: a `Framework` and everything it owns are used from one owner thread. The journal
flusher and the RPC reader and writer threads never touch documents.

On Windows, `Workspace::findByPath` and `recordTypeForPath` compare paths without ASCII case, as
NTFS does, so a differently cased path finds the open document.

**Open (07 §1.2, recorded in 09 §8.1):** the platform file watcher (external edits arrive only
through `doc.reload` / `reloadFromDisk`; `core`'s `FileWatcher` is not wired in yet) and the
pre-commit reference-integrity hook (a `RecordRef` such as the Frigate's `lootTable` is not yet
checked against the project's records).

## Tests

`toolsfw_tests` (doctest, 64 cases, plus 1 `perf:` case in `toolsfw_tests_perf`): transactions and
inverses, history and merging, nested groups, commands and arguments, documents and 3-way reload,
the journal (torn tails at every cut point, group commit, recovery, recovery after a reload,
cross-session reverts), RPC over the real socket or pipe (including a client that never reads and
the connection, request and output bounds), and **ED-1** (`test_ed1.cpp`):

- 10,000 random transactions (Set, Insert, Remove and Move over the Frigate, with rejected edits
  mixed in) are applied, then all undone, then all redone. After every undo and redo the canonical
  text must hash to the text recorded at that step, and the undone document must equal the original
  byte for byte.
- A mixed workload with gestures, interleaved undo/redo and rejected edits, whose journal replays
  onto the original file to the same bytes. The golden workload's final text is pinned by a hash
  that must be identical on every compiler.
- `kill -9` mid-burst: `toolsfw_burst_child` journals a burst and is killed at three points. The
  recovered document equals the acknowledged prefix plus at most one transaction, byte for byte.

`helios_tool_cli` (apps/tools/helios-tool) runs the CLI verbs end to end.

## Plan conformance

Plan-Rev: 11

Written for plan revision 6 (07 §1.2, §4.4; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.
