# engine/toolsfw — ToolsFramework (`helios::tf`)

The UI-less core of the editor (07 §1.2, ADR-009). Layer 4, **EDITOR_ONLY**: it is linked by
`helios-editor` and `helios-tool`, and never by the client, launcher, bot or servers
(`cmake/HeliosLayering.cmake` checks this at configure time). It includes no ImGui or RHI header.
Its dependencies are `core` and `reflect`, plus `script` privately for the Luau automation VM.

| Header | What it provides |
|---|---|
| `document.h` | `Document` (a record file over reflected data: `$rid`/`$name`/`$comment` header, canonical JSONC text, revision, content hash, dirty state) and `Workspace` (lookup by GUID, `$name`, relative path or path; `confine()`, the project-confinement rule) |
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
  may run commands, so the group's origin is the origin of every edit in it. One unfinished
  transaction at a time edits a document: an op on a document that another uncommitted `TxBuilder`
  (the open group included) has edited fails with `InvalidState`, because the later transaction
  could otherwise be journaled first with preconditions taken from the earlier, unjournaled one.
  A `TxBuilder` may outlive its `Framework`: `~Framework` rolls an unfinished one back and detaches
  it, and its later calls fail with `InvalidState`. A new edit drops the redo entries of the
  documents it touches. The history is capped at 512 MB (past the cap, the oldest entries go until
  it is at 7/8 of it).
- **Commit budget.** The history and log byte totals are running sums, so no commit, undo or redo
  walks the session's log. A commit costs O(its ops + the redo stack's entries) and an undo or redo
  O(its ops + the history entries it scans past, undone ones included). A one-op commit stays
  ≤ 0.1 ms in RelWithDebInfo (`perf:` case in `test_history.cpp`, which also requires the last 1,000
  of 50,000 commits to cost at most 3× the first 1,000).
- **Journal** (07 §1.2). Each session appends to `<journal root>/<project>/<session>.hjl`. The
  format is `"HJL1"`, then the header length and header JSON, then records of the form
  `u32 length | u32 check | JSON payload`. Record kinds are open, save, close, tx and end. A
  transaction is journaled **before** it becomes visible: if the write fails, the edit is rolled
  back. Every record is one `write()`, so a crash or `kill -9` loses at most the record in flight.
  A flusher thread makes appends durable within 50 ms (group commit); it calls `fsync` without
  holding the writer's lock, so a commit never waits for the disk. A reload of an external edit
  journals a new open record (the file's new hash, plus the merged text when unsaved local edits
  survived the merge). A save or a reload is a document's replay base, so neither may be journaled
  while edits of the document are applied but not yet journaled: `save()`, `saveAll()`, `close()`
  and `reloadFromDisk()` refuse (InvalidState) while an open group or an uncommitted `TxBuilder`
  holds edits of the document, and a reload or revert refuses inside any group, since its own
  transaction would join the group. A failed journal write of a save or reload record is returned
  to the caller (the file is written or reloaded; recovery needs a later save).
  `Framework::recover()` replays an unclean session onto the files. It
  verifies each file against the session's last open/save/reload hash, restores a reload's merged
  text, re-opens documents created in the session from their snapshot, and keeps each
  transaction's origin, label, merge key and Undo/Redo kind. Two limits are reported, not hidden:
  a recovered Undo or Redo becomes an ordinary history entry in the new session, and only the
  recovered documents' ops of a multi-document transaction are replayed. Default session names are
  `<yyyymmdd-hhmmss>-<pid>`, with `-2`, `-3`, ... when that journal already exists. The journal root is
  `HELIOS_JOURNAL_DIR`, else `%LOCALAPPDATA%\Helios\journal` on Windows or
  `$XDG_STATE_HOME/helios/journal` (else `~/.local/state/helios/journal`) on Linux.
- **The journal is untrusted input** (see the section below): every file it names stays inside
  the project, and a journal of another project is refused.
- **Cross-process undo** (`helios-tool undo`). `TxBuilder::markRevert(kind, target)` records a
  transaction as the Undo or Redo of a transaction from an earlier session. The CLI rebuilds its
  linear undo stack from the project's journals, counting only transactions whose documents were
  saved afterwards in the same session, and continues the project's Lamport counter
  (`FrameworkConfig::lamportFloor`) so ids never repeat across its processes when they run one
  after another. Sessions that run at the same time (two `helios-tool` runs, or the editor and the
  CLI, all user `local` by default) can still mint the same `(user, lamport)` id: ids are unique
  per session, not per project, until the collaboration work gives every session its own user or
  site id, and the CLI's cross-process undo assumes that its runs do not overlap. Each
  run reads the project's journals at start-up, and nothing prunes clean sessions yet, so start-up
  time grows with the project's journal history (follow-up: rotate or prune clean sessions).
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
  disconnected. A client may half-close (`nc -N`, `socat`: shut its write side after the last
  request); the server answers every request it read before closing the connection.

## The journal is untrusted input

A journal is a file the tools parse and then act on: `Framework::recover()` (the editor's crash
recovery), `helios-tool journal replay <file>` (any path) and `helios-tool undo`/`redo` (the
project's journal directory). It can come from anywhere: a crash journal attached to a bug report
("please replay this"), a file planted in the journal directory (`HELIOS_JOURNAL_DIR`,
`--journal-dir`, a synced profile) or one edited by hand. Its record checks detect torn writes, not
tampering, so whoever writes a journal controls every field: the header's project, every record's
file, type and snapshot, every op's file and values.

Whatever a journal says, recovering or replaying it, and saving afterwards, reads, creates,
rewrites and deletes only `.hrec` files inside the open project. Inside the project its edits are
applied by design (that is what a replay is), each behind its op's precondition and the replay
base's hash. Replayed transactions skip the pre-commit hooks, as undo and redo do: read a journal
you did not write first (`helios-tool journal show`) and run `helios-tool validate` after replaying it.
The journal also names the record type an open record is read as, as `Framework::open(file, type)`
lets a caller; that gives it nothing it lacks already, since it can rewrite or delete any record
of the project.

- **One rule, `Workspace::confine()`**, for every path a journal, a raw op or a caller names. A path
  must name a `.hrec` file below the project root: relative (a caller may also pass an absolute
  path under the root; on Windows the root prefix compares without ASCII case), with `/` and `\`
  both separators on every platform, so a journal means the same on Windows and Linux. Refused: `..`
  components; leading or trailing separators (absolute, UNC `\\server`, device `\\?\` and `\\.\` paths);
  `<>:"|?*` (drive letters such as `C:` or `c:`, NTFS streams) and control characters; components
  ending in a dot or a space (Windows drops them, so `x.hrec.` would name `x.hrec`); Windows device
  names (`CON`, `PRN`, `AUX`, `NUL`, `COM0`-`COM9`, `LPT0`-`LPT9` and their superscript forms,
  `CONIN$`, `CONOUT$`, in any case and with any extension); a file name that does not end in
  `.hrec` (in any ASCII case, as `openAll()` lists records). The spelling rule is the same on every
  platform, since a journal written on Linux may be replayed on Windows. The on-disk check then
  walks the existing components from the root without following them and resolves each link: it
  must lead to a directory or regular file inside the root's resolved path.
- **Where it applies.** `recover()` checks the header and every path the journal names (open and
  save records, Create and Destroy ops), on disk, before it reads a record file or applies
  anything; one bad entry refuses the whole recovery (InvalidArgument naming the journal, the
  record's index, offset, kind and document, and the path), so a hostile journal never
  half-applies. That includes a Create or Destroy op that names another file than its document's
  (a legitimate journal never holds one: `TxBuilder::destroy` names the document's own file, and
  an undo restores it there). Every applied op gets the rule too (`TxBuilder::apply`, which replay,
  collaboration and patches use): a Create's file, on disk for a new file, and a Destroy, which
  must also name its document's own file, the file a save deletes. `Framework::open` /
  `doc.open` and `TxBuilder::createRecord` / `doc.create` take a caller's path through it. At
  the time of use, `save()` (write or delete) and `reloadFromDisk()` check the document's path
  again, because a link may appear inside the project after the open; `helios-tool fmt`, `undo`
  and `redo` do the same.
- **The project.** The header must name `FrameworkConfig::project`, unless
  `RecoveryOptions::allowOtherProject` (`helios-tool journal replay --allow-other-project`) says
  otherwise. The header is not a security boundary (a crafted journal simply names the victim's
  project; the confinement rule is what protects the files), so this check only stops the wrong
  project's journal being replayed by mistake, where its Creates would add records to the wrong
  project. The flag exists for the one legitimate mismatch, a project renamed after the crash:
  without it that journal could not be recovered at all short of editing its binary header. It
  relaxes nothing else. `listJournalSessions()` skips journals whose header names another project,
  so `auto` and `latest`, the editor's recovery offer and the CLI undo stack never pick one up.
- **Output.** No string from a journal reaches a terminal or a log with its control characters.
  `tf::printable` escapes C0, DEL, the C1 controls (U+0080-U+009F; U+009B is CSI) and any byte
  that is not UTF-8. It is applied to `recover()`'s refusals (the transaction id, the header's
  project), to every `RecoveredDocument::message` (they quote a transaction's user and an op's
  path or type name), to `listJournalSessions()`'s warnings, and in helios-tool to every error and
  report line and to `journal list`, `show` and `verify`; `journal show --json` writes DEL and C1
  as `\u00NN` (JSON itself escapes only C0). A journal's strings are well-formed UTF-8 (the JSON
  reader refuses a record that is not), and `RecoveredDocument::file` is a confined path, which
  holds no control characters. This is for a UTF-8 terminal: one set to an 8-bit encoding such as
  Latin-1 also reads the bytes 0x80-0x9F inside multi-byte UTF-8 characters as C1 controls, which
  only ASCII-only output would avoid.

What the platform layer (`src/platform`) can and cannot tell:
- Detected: symbolic links on POSIX (`lstat`, resolved with `realpath`); on Windows every reparse
  point (`GetFileAttributesW`), so symbolic links, junctions and mount points, resolved with
  `GetFinalPathNameByHandleW` (MinGW builds use the same Win32 calls); dangling links; devices,
  FIFOs and sockets on POSIX.
- Not detected: hard links (a hard link to a file outside the project is that file, on NTFS and
  POSIX alike); a link that another local process creates between the check and the read or
  write (there is no handle-relative `openat`/`O_NOFOLLOW` walk, and such a process can already
  write the project); links above the project root (the root is the user's choice); NTFS 8.3
  short names, which pass the spelling rule but alias only entries of the same directory.

Threading: a `Framework` and everything it owns are used from one owner thread. The journal
flusher and the RPC reader and writer threads never touch documents.

On Windows, `Workspace::findByPath` and `recordTypeForPath` compare paths without ASCII case, as
NTFS does, so a differently cased path finds the open document.

**Open (07 §1.2, recorded in 09 §8.1):** the platform file watcher (external edits arrive only
through `doc.reload` / `reloadFromDisk`; `core`'s `FileWatcher` is not wired in yet) and the
pre-commit reference-integrity hook (a `RecordRef` such as the Frigate's `lootTable` is not yet
checked against the project's records).

## Tests

`toolsfw_tests` (doctest, 85 cases, plus 1 `perf:` case in `toolsfw_tests_perf`): transactions and
inverses, history and merging, nested groups, commands and arguments, documents and 3-way reload,
the journal (torn tails at every cut point, group commit, recovery, recovery after a reload,
cross-session reverts, and saves and reloads refused inside a group or an uncommitted builder,
with recovery checked after each), hostile journals (`test_confine.cpp`: the spelling rule,
another project's record and a `../outside/evil.sh` Create, absolute, drive-letter, UNC, device and
`..\` paths, a Destroy outside the project or of another file than its document's, a non-`.hrec`
Create, a project mismatch, symbolic links out of the project at recovery, open, create and save,
skipped with a message where links cannot be created, a FIFO, made with `mkfifo` where it exists,
and control characters from a journal in refusals, the replay report and warnings), one writer
per document and builders that outlive their `Framework`, RPC over the real socket or pipe (including a client that never reads, one that
half-closes, and the connection, request and output bounds), and **ED-1** (`test_ed1.cpp`):

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

Plan-Rev: 12

Written for plan revision 6 (07 §1.2, §4.4; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this module.
