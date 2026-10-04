# apps/tools — tool executables

Executables with ROLE `tool` (from this directory, `apps/CMakeLists.txt`). They may link
EDITOR_ONLY modules, and the headless preset builds this directory, so a tool that needs graphics
is added only when `HELIOS_BUILD_GRAPHICS` is on (`CMakeLists.txt`).

| Tool | What it is | Builds in |
|---|---|---|
| [`helios-tool`](helios-tool/README.md) | The headless ToolsFramework CLI (07 §1.1, §1.2): `engine/toolsfw` without EditorUI or graphics | Every preset, headless included |
| [`helios-cook`](helios-cook/README.md) | The cook CLI (07 §1.10); v0's `records` verb cooks `.hrec` sources into `records.client.hrdb` and `records.server.hrdb` (`engine/records`, WP-0.8), and the AAA-SEC-4 sample-records lint runs it | Every preset, headless included |
| [`helios-uitest`](helios-uitest/README.md) | The editor UI test driver (07 §4.4, ED-15): starts `helios-editor --test-mode` on a copy of a project and drives it over the remote-control socket | Graphics presets |

Each tool's README documents its command line, tests and plan conformance.

## Plan conformance

Plan-Rev: 12

Written for plan revision 11 (07 §1.1, §4.4; 09 §2.1 WP-0.18) on 2026-10-03.

Re-checked against plan revision 12 on 2026-10-03: it changed 09 §0, §5.6, §5.7, §5.10.4, §8.1,
§8.2 and PLAN.md §11 (owner approvals, NS-0.2), none of which maps to this directory.

`helios-cook` added by WP-0.8 part 2 on 2026-10-04 (plan revision 13); its README records its own conformance.
