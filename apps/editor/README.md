# helios-editor

The Helios editor (07 §1). The process is `edui::EditorHost` (`engine/editorui`): the Dear ImGui
docking shell over the ToolsFramework (`engine/toolsfw`), rendering through the Helios RHI. This
directory only holds the executable, which parses the command line and registers the sample record
types. ROLE `editor`: it may link EDITOR_ONLY modules.

```
helios-editor [--project-root=<dir>] [--project=<name>] [--user=<name>]
              [--recover=<journal|auto>] [--journal-dir=<dir>] [--no-journal]
              [--theme=dark|light|high-contrast] [--theme-file=<jsonc>] [--scale=<f>]
              [--width=<dip>] [--height=<dip>] [--pseudo-loc]
              [--rpc=<name>|-] [--test-mode] [--present] [--frames=<n>] [--screenshot=<png>]
              [--validation] [--log-level=<level>] [--crash-dir=<dir>] [--version] [--help]
```

- **Project.** Every `records/<table>/*.hrec` under `--project-root` is opened at start. Edits are
  journaled under the journal root (`HELIOS_JOURNAL_DIR`, else the user state directory). The
  journal ends clean only after a normal exit with every record saved (there is no save prompt
  yet), so a crash, a device loss or a quit with unsaved records leaves the session unclean. At the
  next start the Output panel offers it and *File > Recover Unsaved Session* replays it (07 §1.2);
  `--recover=auto` replays it without asking. A fully recovered journal is marked ended.
- **Display.** The UI scale comes from `--scale`, else `HELIOS_EDITOR_SCALE`, else the scale of
  the display the window is on, following the window to other monitors. Menus: *View* switches the theme and scale; *Window* shows or hides panels.
  Ctrl+Shift+P opens the command palette.
- **Remote control.** JSON-RPC on `\\.\pipe\helios-editor-<pid>` (Windows) or
  `$XDG_RUNTIME_DIR/helios-editor-<pid>.sock` (Linux), or on the name `--rpc` gives. It exposes the
  framework methods (`cmd.invoke`, `doc.get`, `tx.undo`, …) and, in test mode, the `ui.*` methods
  (see `engine/editorui/include/helios/editorui/editor_host.h`). `--rpc=-` turns it off.
- **Test mode** (`--test-mode`) is the deterministic mode `helios-uitest` drives (07 §4.4).
- **Exit codes:** 0 success, 1 setup or frame failure, 2 RHI validation errors.

The `helios_editor_smoke` CTest (label `gpu`; on Linux it runs under `xvfb-run -a` on lavapipe)
renders 30 frames of the sample project (`engine/toolsfw/samples/project`) and saves a screenshot.

## Plan conformance

Plan-Rev: 12

Written for plan revision 6 (07 §1.1–1.4; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this directory.
