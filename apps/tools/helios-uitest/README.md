# helios-uitest

The editor UI test driver (07 §4.4). It copies a fixture project and starts `helios-editor
--test-mode` on it. It then drives the editor over the remote-control socket: `ui.*` input goes
through the same SDL3 path as a real mouse and keyboard, and results are checked through the item
table, the ToolsFramework RPC methods, the layout lints and ꟻLIP image goldens. It needs no
graphics of its own (ꟻLIP and PNG come from `engine/render`). Dear ImGui Test Engine is not used
(`docs/adr/ADR-0.18-imgui-test-engine-licence.md`).

```
helios-uitest --editor=<helios-editor> --fixture=<project> --goldens=<dir> --out=<dir>
              [--update-goldens] [--present] [--width=1920] [--height=1080]
              [--flip-mean=0.01] [--flip-hot=0.001] [--dump-items] [--keep-going]
```

The built-in suite is the **ED-15 harness self-test** for the shell and the property grid:

1. **Item table.** Menus, panels and grid rows have their expected paths and kinds. Selecting the
   Frigate through a click fills the Inspector.
2. **Goldens.** The shell (the whole window) and the property grid (the Inspector) at 100 %
   (1920 × 1080) and 200 % (3840 × 2160), in the dark and high-contrast themes. They are compared
   with `engine/editorui/tests/golden` by ꟻLIP: mean ≤ 0.01 and ≤ 0.1 % of pixels above 0.1
   (07 §4.4). A capture identical to its golden passes without computing ꟻLIP, whose error is then
   0 everywhere (it costs about a minute per 1080p image in Debug sanitizer builds). The viewport
   is masked. A failing comparison writes `<name>.flip.png` next to the capture. A repeated capture
   must be pixel-identical (determinism).
3. **Layout lints** with no issues: at every golden configuration, at 75, 100, 150, 200 and 250 %,
   and with pseudo-localized labels.
4. **Dual path.** A typed UI edit (origin `ui-scripted`) and the same edit through `cmd.invoke`
   (origin `rpc`) produce identical documents. Ctrl+Z, a check box and the History panel's Undo
   restore the fixture byte for byte.
5. **Keyboard.** Ctrl+Shift+P opens the palette and runs a command. The View menu lists the theme
   commands. Ctrl+Tab reaches every panel.

It writes `report.json` and the captures to `--out`, which it deletes and recreates at start. So
`--out` must be new, empty, or the `--out` of an earlier run (it holds `helios-uitest-out.txt` or
`report.json`), and must not hold `--goldens` or `--fixture`; anything else is refused with exit
code 2 before a file is touched (CTest `helios_uitest_out_guard`). Exit code 0 when every check
passes, 1 on a failure, 2 on a setup error. CTest: `editorui_ed15` (label `gpu`; under `xvfb-run -a` on Linux,
where lavapipe is the Vulkan device). Refresh the goldens with `--update-goldens` and review every
image before committing.

## Plan conformance

Plan-Rev: 12

Written for plan revision 6 (07 §4.4, ED-15; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against plan
revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04 §3.2 and
§10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps to this
module; 07 and 09's WP-0.18 row are unchanged since revision 6.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this directory.
