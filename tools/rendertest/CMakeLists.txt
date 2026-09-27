# tools/rendertest — helios-rendertest (docs/plan/03-rendering.md §8.4, RC-1): golden-image scenes rendered
# offscreen through the render graph on Vulkan (lavapipe in CI) and the Null backend, each twice
# (bit-identical), compared with goldens using ꟻLIP (Vulkan) or command-stream traces (Null), with an
# HTML/Markdown report. Registered as CTests: rendertest.vulkan.<scene> (label gpu),
# rendertest.null.<scene> (every toolchain), rendertest.coverage (every toolchain), rendertest.report (gpu),
# rendertest.validation-layer and rendertest.validation-required (gpu), lint_shipped_pipelines* (lint).

if(TARGET helios-rendertest)
  return()  # already added (engine/render adds the tool until tools/CMakeLists.txt lists it)
endif()
include(HeliosShaders)

helios_executable(helios-rendertest ROLE tool
  SOURCES src/main.cpp src/cli.cpp src/harness.cpp src/scenes.cpp
  DEPS helios::render helios::rhi helios::core helios::math helios::tp::yyjson)
target_include_directories(helios-rendertest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(helios-rendertest PRIVATE HELIOS_RENDERTEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
file(GLOB HELIOS_RENDERTEST_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang)
helios_shaders(helios-rendertest NAMESPACE rendertest_shaders FILES ${HELIOS_RENDERTEST_SHADERS})

if(HELIOS_BUILD_TESTS)
  # Keep in sync with createScenes() (checked by rendertest.scenes).
  set(HELIOS_RENDERTEST_SCENES triangle compute bindless forward normals postchain mips)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/rendertest-output)
  string(REPLACE ";" "," scene_list "${HELIOS_RENDERTEST_SCENES}")
  add_test(NAME rendertest.scenes COMMAND helios-rendertest --list --expect-scenes ${scene_list})
  # Khronos validation of the goldens (03 §1.5): where configure finds the layer (the Linux CI images
  # install it), every Vulkan scene requires it, so a layer that stops loading fails the tests instead
  # of passing them unchecked, and rendertest.validation-layer proves it reports a seeded error.
  # Without the layer the scenes run unvalidated, rendertest.validation-layer reports Skipped and each
  # result says "validation": false. The layer's directory may also come from VK_ADD_LAYER_PATH or
  # VK_LAYER_PATH, which then must be set for the tests too.
  if(NOT CMAKE_CROSSCOMPILING)
    find_file(HELIOS_VK_VALIDATION_LAYER VkLayer_khronos_validation.json
              PATHS ENV VK_ADD_LAYER_PATH ENV VK_LAYER_PATH /usr/share/vulkan/explicit_layer.d
                    /usr/local/share/vulkan/explicit_layer.d /etc/vulkan/explicit_layer.d
                    "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d" "$ENV{VULKAN_SDK}/Bin"
              NO_DEFAULT_PATH DOC "Khronos validation layer manifest; the Vulkan goldens require it when set")
  endif()
  if(HELIOS_VK_VALIDATION_LAYER)
    set(validation_arg --require-validation)
    add_test(NAME rendertest.validation-layer COMMAND helios-rendertest --validation-self-test)
    set_tests_properties(rendertest.validation-layer PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP validation self-test")
  else()
    set(validation_arg --validation)
    add_test(NAME rendertest.validation-layer
             COMMAND ${CMAKE_COMMAND} -E echo "SKIPPED: no Khronos validation layer at configure time; the Vulkan goldens run without it")
    set_tests_properties(rendertest.validation-layer PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED: no Khronos")
  endif()
  set_tests_properties(rendertest.validation-layer PROPERTIES LABELS "gpu;rendertest" TIMEOUT 120)
  # A layer that is required but does not load fails, and HELIOS_SKIP_GPU_TESTS=1 never turns that into a skip.
  add_test(NAME rendertest.validation-required
           COMMAND ${CMAKE_COMMAND} -DRENDERTEST=$<TARGET_FILE:helios-rendertest>
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rendertest-validation-output
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation_checks.cmake)
  set_tests_properties(rendertest.validation-required PROPERTIES LABELS "gpu;rendertest" TIMEOUT 120
                       SKIP_REGULAR_EXPRESSION "SKIPPED: no Vulkan device")
  set(vulkan_tests "")
  foreach(scene ${HELIOS_RENDERTEST_SCENES})
    add_test(NAME rendertest.vulkan.${scene}
             COMMAND helios-rendertest --backend vulkan ${validation_arg} --scene ${scene} --out ${out})
    set_tests_properties(rendertest.vulkan.${scene} PROPERTIES LABELS "gpu;rendertest" TIMEOUT 300)
    add_test(NAME rendertest.null.${scene}
             COMMAND helios-rendertest --backend null --scene ${scene} --out ${out})
    set_tests_properties(rendertest.null.${scene} PROPERTIES LABELS "rendertest" TIMEOUT 120)
    list(APPEND vulkan_tests rendertest.vulkan.${scene} rendertest.null.${scene})
  endforeach()
  # RC-1 coverage, GPU-less: every shipped shader entry point and pipeline of engine/render is bound by
  # a scene whose lavapipe and Null goldens are committed (helios/render/shader_library.h).
  add_test(NAME rendertest.coverage COMMAND helios-rendertest --coverage)
  set_tests_properties(rendertest.coverage PROPERTIES LABELS "rendertest" TIMEOUT 120)
  # ... which holds only if no pipeline bypasses the records: engine/render, apps/samples and
  # tools/rendertest create pipelines only in createShippedPipeline() and createLocalPipeline().
  set(pipeline_lint ${CMAKE_CURRENT_SOURCE_DIR}/tests/shipped_pipelines_lint.cmake)
  set(pipeline_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/shipped_pipelines)
  add_test(NAME lint_shipped_pipelines
           COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${pipeline_lint})
  add_test(NAME lint_shipped_pipelines_fixture_clean
           COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${pipeline_fixtures}/clean -P ${pipeline_lint})
  # Seeded violations (tools/lint/expect_fail.cmake: the lint must exit non-zero AND say why).
  foreach(case "c7_state_variant|engine/render/src/forward.cpp:[0-9]+: .*createGraphicsPipeline"
               "sample_direct|apps/samples/demo/main.cpp:[0-9]+: .*createComputePipeline"
               "waiver_without_reason|tools/rendertest/src/scenes.cpp:[0-9]+: .*waiver needs a reason")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 fixture)
    list(GET parts 1 expect)
    add_test(NAME lint_shipped_pipelines_fixture_${fixture}
             COMMAND ${CMAKE_COMMAND} "-DEXPECT=${expect}" -DNARGS=4 -DARG0=${CMAKE_COMMAND}
                     -DARG1=-DSOURCE_DIR=${pipeline_fixtures}/${fixture} -DARG2=-P -DARG3=${pipeline_lint}
                     -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
  endforeach()
  foreach(t lint_shipped_pipelines lint_shipped_pipelines_fixture_clean lint_shipped_pipelines_fixture_c7_state_variant
          lint_shipped_pipelines_fixture_sample_direct lint_shipped_pipelines_fixture_waiver_without_reason)
    set_tests_properties(${t} PROPERTIES LABELS lint TIMEOUT 120)
  endforeach()
  # GPU-less CLI checks: skips overwrite stale results, removed scenes leave the report, --budget parsing,
  # and the coverage check fails when scenes or goldens are missing.
  add_test(NAME rendertest.cli
           COMMAND ${CMAKE_COMMAND} -DRENDERTEST=$<TARGET_FILE:helios-rendertest>
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rendertest-cli-output
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_checks.cmake)
  set_tests_properties(rendertest.cli PROPERTIES LABELS "rendertest" TIMEOUT 120)
  # Aggregates the per-scene results into report.html / report.md and checks the RC-1 budget (10 min).
  add_test(NAME rendertest.report COMMAND helios-rendertest --report-only --out ${out} --budget 600)
  set_tests_properties(rendertest.report PROPERTIES LABELS "gpu;rendertest" DEPENDS "${vulkan_tests}" TIMEOUT 60)
endif()
