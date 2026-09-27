Fixtures of `../shipped_pipelines_lint.cmake` (CTests `lint_shipped_pipelines_fixture_*`). Each
directory is a `SOURCE_DIR` with the repository's layout; the `.cpp` files are never compiled.

* `clean` passes: the exempt `shader_library.cpp`, the excluded `engine/rhi/` and `tools/prebuilt/`,
  a reasoned waiver alone on the line above (also inside a call that spans lines) and on the same
  line, a call through `createShippedPipeline()`, identifiers that are not `create<X>Pipeline`
  (`createPipeline`, `createGraphicsPipelineState`), and direct calls under `tools/rendertest/tests/`
  and `tools/lint/tests/` (the lints' fixtures).
* `c7_state_variant` fails: round-1 review mutant C7, a state variant of covered shaders created
  directly on the device in `ForwardRenderer::create`.
* `sample_direct` fails: a direct compute pipeline in a sample, through `->`.
* `waiver_without_reason` fails: waivers with no reason and with a too-short one.
* `indirect` fails 4 times, in `engine/ui` (outside `engine/render`): a member-function pointer,
  `std::invoke`, a line break after `device.` and one before `(` (round-2 review L1, L5, L3, L3b).
* `waiver_leak` fails on the second of two calls: a waiver after code does not cover the next line.
* `splice_and_include` fails 5 times: backslash-newline splices inside the identifier, one with a
  blank before the newline (line 6 of `forward.cpp` ends in a space on purpose), reported at their
  first physical line, a direct call after them at its own line, one in an `.inc` file, and a file
  whose only direct call is spliced (round-3 review L8, L9).
