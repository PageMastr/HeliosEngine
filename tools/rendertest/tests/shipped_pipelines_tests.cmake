# CTests of the shipped-pipelines lint (shipped_pipelines_lint.cmake, label `lint`). Included by
# tools/lint/lint_tests.cmake in every configuration, headless too: the lint is textual and needs
# neither a build nor graphics, and the headless CI job turns lint failures into SARIF.
# Uses helios_lint_test() and ${LINT} from the including file.

set(pipeline_lint ${CMAKE_CURRENT_LIST_DIR}/shipped_pipelines_lint.cmake)
set(pipeline_fixtures ${CMAKE_CURRENT_LIST_DIR}/shipped_pipelines)
helios_lint_test(lint_shipped_pipelines COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${pipeline_lint})
helios_lint_test(lint_shipped_pipelines_fixture_clean
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${pipeline_fixtures}/clean -P ${pipeline_lint})
# Seeded violations: the lint must exit non-zero AND say why.
foreach(case "c7_state_variant|engine/render/src/forward.cpp:[0-9]+: .*createGraphicsPipeline"
             "sample_direct|apps/samples/demo/main.cpp:[0-9]+: .*createComputePipeline"
             "waiver_without_reason|tools/rendertest/src/scenes.cpp:[0-9]+: .*waiver needs a reason"
             "indirect|shipped-pipelines lint: 4 finding"
             "waiver_leak|engine/vfx/src/trails.cpp:4: createGraphicsPipeline outside"
             "splice_and_include|4 finding.*forward.cpp:4: createGraphicsPipeline.*forward.cpp:6: createComputePipeline.*forward.cpp:8: createGraphicsPipeline.*forward_variants.inc:2: createGraphicsPipeline")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_shipped_pipelines_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${pipeline_fixtures}/${fixture} -P ${pipeline_lint})
endforeach()
