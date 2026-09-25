# tools/rendertest — helios-rendertest (docs/plan/03-rendering.md §8.4, RC-1): golden-image scenes rendered
# offscreen through the render graph on Vulkan (lavapipe in CI) and the Null backend, each twice
# (bit-identical), compared with goldens using ꟻLIP (Vulkan) or command-stream traces (Null), with an
# HTML/Markdown report. Registered as CTests: rendertest.vulkan.<scene> (label gpu),
# rendertest.null.<scene> (every toolchain), rendertest.report (gpu).

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
  set(HELIOS_RENDERTEST_SCENES triangle compute bindless forward postchain mips)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/rendertest-output)
  string(REPLACE ";" "," scene_list "${HELIOS_RENDERTEST_SCENES}")
  add_test(NAME rendertest.scenes COMMAND helios-rendertest --list --expect-scenes ${scene_list})
  set(vulkan_tests "")
  foreach(scene ${HELIOS_RENDERTEST_SCENES})
    add_test(NAME rendertest.vulkan.${scene}
             COMMAND helios-rendertest --backend vulkan --scene ${scene} --out ${out})
    set_tests_properties(rendertest.vulkan.${scene} PROPERTIES LABELS "gpu;rendertest" TIMEOUT 300)
    add_test(NAME rendertest.null.${scene}
             COMMAND helios-rendertest --backend null --scene ${scene} --out ${out})
    set_tests_properties(rendertest.null.${scene} PROPERTIES LABELS "rendertest" TIMEOUT 120)
    list(APPEND vulkan_tests rendertest.vulkan.${scene} rendertest.null.${scene})
  endforeach()
  # GPU-less CLI checks: skips overwrite stale results, removed scenes leave the report, --budget parsing.
  add_test(NAME rendertest.cli
           COMMAND ${CMAKE_COMMAND} -DRENDERTEST=$<TARGET_FILE:helios-rendertest>
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rendertest-cli-output
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_checks.cmake)
  set_tests_properties(rendertest.cli PROPERTIES LABELS "rendertest" TIMEOUT 120)
  # Aggregates the per-scene results into report.html / report.md and checks the RC-1 budget (10 min).
  add_test(NAME rendertest.report COMMAND helios-rendertest --report-only --out ${out} --budget 600)
  set_tests_properties(rendertest.report PROPERTIES LABELS "gpu;rendertest" DEPENDS "${vulkan_tests}" TIMEOUT 60)
endif()
