# ADR-0.18: Dear ImGui Test Engine licence evaluation

| | |
|---|---|
| **Status** | **Recorded, 2026-09-25.** Dear ImGui Test Engine is **not vendored and not used**. `helios-uitest` is built in house on Helios hooks, as 07 §4.4 already requires |
| **Decides** | Whether WP-0.18's editor UI test driver may use Dear ImGui Test Engine ([07 §4.4](../plan/07-editor-and-tools.md#44-editor-ui-test-harness-helios-uitest)) |
| **Constraint** | [01 §5.2](../plan/01-vision-and-scope.md#52-ip-and-licensing) ("Permissive dependencies") and CLAUDE.md: shipped and vendored code must be under a permissive licence (MIT, BSD, ISC, zlib, Apache-2.0, Boost, PostgreSQL, public domain). `ctest -L lint` enforces this over `third_party/` and `MANIFEST.md` |
| **Owner** | Tools lead (07) |
| **Evidence** | The licence text and repository README of `github.com/ocornut/imgui_test_engine`, read on 2026-09-25 and re-read on 2026-10-03, still v1.04 (quoted below); `engine/editorui/src/imgui_item_hooks.{h,cpp}`; `third_party/CMakeLists.txt` (the `tp_imgui` block) |

## 1. Facts

Dear ImGui Test Engine is published by Omar Cornut (DISCO HELLO) in the `ocornut/imgui_test_engine`
repository. The repository has two licences:

- **`imgui_test_engine/`** (the engine: context, item registry, input queue, capture tool, test
  runner) is under the **"Dear ImGui Test Engine License (v1.04)"**. Its README summarizes it as
  "free for individuals, educational, open-source and small businesses uses. Paid for larger
  businesses."
- **`imgui_test_suite/`, `app_minimal/`, `shared/`** and every other folder are under the **MIT
  License**.

The v1.04 licence grants a free licence when **any** of these holds:

- the licensee is a natural person;
- the licensee is not a legal entity, or is a not-for-profit legal entity;
- the software is used for educational purposes;
- the software is used in derivative works "released publicly and under an Open Source license";
- the licensee is a legal entity "with a turnover inferior to 2 million USD (or equivalent) during
  your last fiscal year".

Everyone else gets a trial of "a maximum of 45 days, at no charge", after which a paid subscription is
required; "Paid Licenses are exclusively sold by DISCO HELLO". Redistribution and modification are
allowed on condition that "the above copyright notice and this license shall be included in all copies
or substantial portions of the Software and/or Derivative Software", and a modification "must not
directly or indirectly imply a modification of the License".

## 2. Assessment

1. **It is not a permissive licence.** Its terms depend on who the licensee is (natural person,
   non-profit, turnover below USD 2 M) and end in a paid subscription for everyone else. None of the
   licences on the 01 §5.2 allow-list has such conditions, and the licence lint would have to be
   weakened to admit it. CLAUDE.md forbids weakening that lint.
2. **Helios's own licensing would not protect downstream users.** Helios is MIT. A studio that builds
   a commercial game on Helios, with a turnover above USD 2 M, would need its own paid licence for
   any tool that links the test engine. That includes every studio CI job that runs `helios-uitest`
   over its gems (07 §4.4, ED-19). Helios's promise is that the whole editor, tests included, can be
   used under MIT.
3. **The open-source clause does not remove the problem.** Helios is open source, so the Helios
   project itself could qualify. The obligation still reaches whoever redistributes or runs a
   derivative, and the MIT licence we give downstream cannot carry that obligation.
4. **The MIT parts are not what we would need.** `imgui_test_suite/` and `shared/` are MIT, but they
   are the tests *of* Dear ImGui and helpers around the engine. They do not replace the engine's item
   registry, input queue or capture.

## 3. Decision

- **Do not vendor or link Dear ImGui Test Engine**, and do not copy code from `imgui_test_engine/`.
  `third_party/` does not contain it, and `MANIFEST.md` does not list it.
- **Build the driver in house** (WP-0.18), following the *design* described publicly for the test
  engine: items addressed by path, queued input, wait helpers, captures. Its code is not used.
  - Dear ImGui (MIT) is built with `IMGUI_ENABLE_TEST_ENGINE`. This makes `imgui.cpp` call the
    `ImGuiTestEngineHook_ItemAdd`, `ImGuiTestEngineHook_ItemInfo` and `ImGuiTestEngineHook_Log` hooks
    while a context sets `TestEngineHookItems`. Those hook points are part of Dear ImGui itself.
  - Helios defines the hook functions in `engine/editorui/src/imgui_item_hooks.cpp` (MIT, Helios
    code). They are compiled into `tp_imgui`, forward to a table that `edui::UiTest` installs in
    `ImGuiContext::TestEngine`, and do nothing when no table is installed. With the harness off, the
    cost is the one branch per item that Dear ImGui already has.
  - The item table, `ui.*` input injection through the SDL3 backend seam, the deterministic test mode,
    captures and layout lints are in `engine/editorui` (`ui_test.h`, `editor_host.h`). The driver and
    the ꟻLIP goldens are in `apps/tools/helios-uitest`.
- **Revisit** only if the test engine is relicensed under a licence on the 01 §5.2 allow-list. A
  different commercial arrangement does not reopen this decision.

## 4. Consequences

- Helios maintains about 1,700 lines of harness code: `ui_test.cpp` (≈ 900), the `ui.*` RPC surface
  in `editor_host.cpp` and `helios-uitest` (≈ 450). The test engine's richer features, such as its interactive
  test browser, perf tool and video capture, are not available. Phase 0 does not need them.
- `IMGUI_ENABLE_TEST_ENGINE` is a public compile definition of `tp_imgui`, so every image that links
  ImGui links the hook shim. A future ImGui upgrade must keep the three hook signatures in
  `imgui_item_hooks.cpp` in step with `imgui_internal.h`. A mismatch fails the build rather than
  failing silently.
- The licence lint does not change.
