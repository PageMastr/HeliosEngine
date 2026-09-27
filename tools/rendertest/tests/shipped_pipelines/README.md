Fixtures of `../shipped_pipelines_lint.cmake` (CTests `lint_shipped_pipelines_fixture_*`). Each
directory is a `SOURCE_DIR` with the repository's layout; the `.cpp` files are never compiled.

* `clean` passes: the exempt `shader_library.cpp`, a reasoned waiver on the line above and on the
  same line, a call through `createShippedPipeline()`, and a direct call under `tools/rendertest/tests/`.
* `c7_state_variant` fails: round-1 review mutant C7, a state variant of covered shaders created
  directly on the device in `ForwardRenderer::create`.
* `sample_direct` fails: a direct compute pipeline in a sample, through `->`.
* `waiver_without_reason` fails: waivers with no reason and with a too-short one.
