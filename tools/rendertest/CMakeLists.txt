# tools/rendertest — helios-rendertest (docs/plan/03-rendering.md §8.4, RC-1): golden-image scenes rendered
# offscreen through the render graph on Vulkan (lavapipe in CI) and the Null backend, each twice
# (bit-identical), compared with goldens using ꟻLIP (Vulkan) or command-stream traces (Null), with an
# HTML/Markdown report. Registered as CTests: rendertest.vulkan.<scene> (label gpu),
# rendertest.null.<scene> (every toolchain), rendertest.coverage (every toolchain), rendertest.report (gpu),
# rendertest.validation-layer and rendertest.validation-required (gpu); lint_shipped_pipelines* (lint) is
# registered through tools/lint/lint_tests.cmake (tests/shipped_pipelines_tests.cmake).

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
  set(HELIOS_RENDERTEST_SCENES triangle compute bindless forward forward-srgb normals postchain mips)
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
  set(layer_args "")
  if(HELIOS_VK_VALIDATION_LAYER)
    list(APPEND layer_args "-DLAYER=${HELIOS_VK_VALIDATION_LAYER}")
  endif()
  # Two fake VK_LAYER_KHRONOS_validation layers for that check (Linux test builds; never installed):
  # vkCreateInstance fails with VK_ERROR_INITIALIZATION_FAILED, or with VK_ERROR_INCOMPATIBLE_DRIVER,
  # which reads like a machine without a driver until the probe device finds one.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    foreach(variant "INITFAIL|VK_ERROR_INITIALIZATION_FAILED" "NODRIVER|VK_ERROR_INCOMPATIBLE_DRIVER")
      string(REPLACE "|" ";" parts "${variant}")
      list(GET parts 0 kind)
      list(GET parts 1 result)
      string(TOLOWER "${kind}" suffix)
      set(fake helios_rendertest_fake_layer_${suffix})
      add_library(${fake} MODULE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_layer.c)
      target_compile_definitions(${fake} PRIVATE FAKE_LAYER_RESULT=${result})
      target_link_libraries(${fake} PRIVATE helios::tp::vulkan_headers)
      set_target_properties(${fake} PROPERTIES FOLDER tests)
      helios_apply_warnings(${fake})
      set(fake_dir ${CMAKE_CURRENT_BINARY_DIR}/fake-layer-${suffix}/$<CONFIG>)
      file(GENERATE OUTPUT ${fake_dir}/VkLayer_khronos_validation.json CONTENT "{
    \"file_format_version\": \"1.1.2\",
    \"layer\": {
        \"name\": \"VK_LAYER_KHRONOS_validation\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"$<TARGET_FILE:${fake}>\",
        \"api_version\": \"1.3.275\",
        \"implementation_version\": \"1\",
        \"description\": \"rendertest.validation-required: vkCreateInstance fails with ${result}\",
        \"functions\": {
            \"vkGetInstanceProcAddr\": \"fakeGetInstanceProcAddr\",
            \"vkGetDeviceProcAddr\": \"fakeGetDeviceProcAddr\"
        }
    }
}
")
      list(APPEND layer_args "-DFAKE_LAYER_${kind}=${fake_dir}")
    endforeach()
  endif()
  add_test(NAME rendertest.validation-required
           COMMAND ${CMAKE_COMMAND} -DRENDERTEST=$<TARGET_FILE:helios-rendertest>
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rendertest-validation-output ${layer_args}
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
  # ... which holds only if no pipeline bypasses the records: engine/, apps/ and tools/ (except the
  # RHI itself) create pipelines only in createShippedPipeline() and createLocalPipeline(). That lint
  # (lint_shipped_pipelines*, label lint) needs no build and is registered by tools/lint/lint_tests.cmake
  # through tests/shipped_pipelines_tests.cmake, headless configurations included; run_lints.cmake runs it too.
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
