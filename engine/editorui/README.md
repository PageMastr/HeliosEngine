# engine/editorui — EditorUI (`helios::edui`)

The Dear ImGui docking shell of `helios-editor` (07 §1.3, §1.4) and the core of the editor UI test
harness (07 §4.4). Layer 4, **EDITOR_ONLY**, graphics builds only. It depends on `toolsfw`, `rhi`,
`render`, `core` and `reflect`, and privately on `tp_imgui` and SDL3. Every action it offers is a
ToolsFramework command, so it is a transaction with the `ui` origin, or `ui-scripted` in test mode.

| Header | What it provides |
|---|---|
| `editor_host.h` | `EditorHost`: the editor process. It owns the SDL3 window, the Vulkan device and swapchain, the ImGui context, the framework, the remote-control socket and the shell, and runs the frame loop |
| `shell.h` | `Shell`: main menu (File, Edit, View, Window, Tools, Help), the default dock layout, the Documents, Inspector, History, Output and Viewport panels, the status bar with the context accent, the command palette (Ctrl+Shift+P) and a generic argument form for commands. It also registers the UI commands (`view.theme.*`, `view.scale.*`, `window.*`, `app.quit`, …) |
| `property_grid.h` | `PropertyGrid`: the generic inspector over reflected records. It covers names, doc tooltips, `@unit` suffixes, `@range` clamps, enums, flags, structs, lists, keyed lists (add, remove, reorder), maps, optionals and variants, with `client`/`server` badges. A drag is one undo step: frames share a merge key |
| `imgui_renderer.h` | `ImGuiRenderer`: the ImGui renderer backend on the Helios RHI. It pulls vertices through buffer device addresses, uses bindless textures (`ImTextureID` = bindless slot) and ImGui 1.92 dynamic textures, so the font atlas re-rasterizes at a new DPI |
| `theme.h` | Dark, light and high-contrast token files (`themes/*.jsonc`, embedded at configure time; `--theme-file` loads another), `applyTheme` at a UI scale, and the WCAG contrast check (4.5:1, or 7:1 in high contrast) |
| `ui_test.h` | `UiTest`: the item table, `ui.*` actions (click, doubleClick, hover, move, drag, type, key, scroll, waitFor, frames, capture) and the layout lints |
| `localize.h` | `tr()` for UI text, with pseudo-localization (+40 %, accents and brackets; ids kept) |

## Rendering

Each frame the editor renders is one render graph (`engine/render`):

1. **Viewport** (when the panel is visible): the Phase 0 placeholder grid, drawn into a bindless
   texture that the shell shows under an invisible button. The real viewport of 07 §1.5 replaces it
   in Phase 1.
2. **EditorUI**: the ImGui draw lists go into the persistent RGBA8 `EditorFrame` texture.
3. **Capture** (screenshots and `ui.capture` only): a copy into a readback buffer.
4. **Present**: a full-screen pass onto the swapchain image. It decodes sRGB when the swapchain
   format encodes, so the bytes stay unchanged.

**DPI.** ImGui works in framebuffer pixels. The UI scale (`--scale`, `HELIOS_EDITOR_SCALE`, or else
the scale of the display the window is on) goes into the style metrics and `FontScaleDpi`. Without a
forced scale it follows the window: `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` (moved to another
monitor, or the display's scale changed) re-rasterizes glyphs at the new display's scale
(`src/display_scale.h`). A scale the user picks under *View > UI Scale* stays put for the session,
like a forced one. Test mode rebuilds the default layout at a new scale; a user's own dock
layout is kept. Only the vector font embedded in
Dear ImGui is used (a minimal ProggyForever, MIT), so every machine renders the same glyphs.

## Test mode and the harness (07 §4.4)

`helios-editor --test-mode --rpc=<name>` is what `helios-uitest` drives:

- **Deterministic.** Fixed 1/60 s steps, no caret blink, tooltip delays 0, no layout file, no OS
  input, and a hidden window unless `--present` is given. Frames advance **only while a `ui.*`
  action runs**, so timers never depend on how fast the driver sends requests. Frames are rendered
  only when a capture asks for one. The framework clock and new keyed-list keys are seeded.
- **Item table.** Dear ImGui is built with `IMGUI_ENABLE_TEST_ENGINE`. Helios's hook shim
  (`src/imgui_item_hooks.cpp`, compiled into `tp_imgui`) forwards `ItemAdd` and `ItemInfo` to
  `UiTest`. Each frame records every interactive item with a stable path
  (`Inspector/grid/Grid/row[handling]/row[handling/yawRate]/value`), its rect, kind, enabled state,
  value text and tooltip. A path is the window path, then the semantic scopes that edui widgets push
  (`Grid`, `row[<property path>]`), then the part of the label after `###`/`##`, or the visible
  label. With the harness off, the hooks cost one branch per item. Dear ImGui Test Engine is not
  used: see `docs/adr/ADR-0.18-imgui-test-engine-licence.md`.
- **Input.** `ui.*` actions queue SDL events into `ImGui_ImplSDL3_ProcessEvent`, one step per
  frame, so they take a real mouse's and keyboard's path. `ui.type` waits until a text field has
  keyboard focus.
- **Captures.** `ui.capture` writes a PNG of the window or of a panel, with viewport items masked
  (03's goldens own the renderer) and the mask rectangles returned.
- **Layout lints** (`ui.lint`): clipped labels, overlapping widgets, widgets outside a window that
  cannot scroll there, interactive items with no label or tooltip, low-contrast theme tokens,
  commands that no menu exposes, and panels that Ctrl+Tab cannot reach. The contrast rule checks
  `drawnContrastPairs()`: every foreground token on every background the shell and grid draw it
  on. The semantic tokens (`badgeServer`, `badgeClient`, `dirty`, `error`, `statusBarBg`,
  `accentLocal`) are drawn through `semanticColor()`.

## Tests

- **`editorui_tests`** (doctest, 47 cases plus 1 `perf:` case; no GPU or display, so it runs on
  Windows CI too):
  - themes and contrast (every text pair the shell draws, including disabled text and input hints
    on selected and hovered rows and frames, the badges also on a hovered or pressed name cell,
    selected text in a text field, and the dirty and error tokens, with the ratios also computed
    outside the lint), theme metric ranges, the embedded themes against the token files,
    pseudo-localization and chords;
  - the property grid's edits run after its rows are drawn: clearing an optional and removing a
    non-last element of an expanded list, which read a cleared value and past the shortened list
    while the edits ran inside the draw;
  - crash recovery: an earlier unclean session is offered (Output line, File > Recover Unsaved
    Session) and replays, or File > Discard Unsaved Session stops offering it (its journal stays);
    both take the newest session first or a named one; the journal ends clean only after a normal
    exit with every record saved;
  - the per-monitor DPI policy, including a scale picked under View > UI Scale staying put;
  - the shell and grid on a headless ImGui context, driven by injected input: item paths, typed and
    dragged edits as `ui-scripted` transactions, Ctrl+Z/Ctrl+Y, check boxes, list buttons, the
    History panel, menus, the palette (every command listed, scrolling), the case-insensitive
    Documents filter, the argument form;
  - every lint rule against a seeded violation;
  - the renderer on the Null RHI with state validation;
  - the ꟻLIP golden policy (it passes identical images and fails a one-pixel shift);
  - the sample project being canonical;
  - `perf:` the item table costs at most 0.2 ms per frame (07 §4.4; `editorui_tests_perf`).
- **ED-15 goldens** (`tests/golden`, `editorui_ed15`, label `gpu`, run by
  `apps/tools/helios-uitest`): shell and property grid at 100 % (1920 × 1080) and 200 %
  (3840 × 2160), dark and high contrast. Refresh them with `helios-uitest --update-goldens` and
  review every image before committing. `engine/toolsfw/samples/project` is the seeded project the goldens show.
- `helios_editor_smoke` (`apps/editor`, label `gpu`): 30 presented frames and a screenshot.

## Plan conformance

Plan-Rev: 12

Written for plan revision 6 (07 §1.3, §1.4, §4.4; 09 §2.1 WP-0.18) on 2026-09-25. Re-checked against
plan revision 11 on 2026-10-03, when the work was ported onto it: revisions 7–11 changed 02 §7.4, 04
§3.2 and §10.2, 06 §1.2, ADR-004a, and 09's WP-0.10r row, §5.2a, §5.10.4, §7 and §8, none of which maps
to this module; 07 and 09's WP-0.18 row are unchanged since revision 6. The Phase 0
deviations from 07 are listed in the WP-0.18 row of 09 §8.1. Multi-viewport tear-offs, Roboto
fonts, the `schemac` editor emitter and localization (T25) come later.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this module.
