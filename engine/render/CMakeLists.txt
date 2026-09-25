# engine/render — L3 renderer (non-HEADLESS; ADR-003, docs/plan/03-rendering.md §2): render graph,
# shader reflection (.hsr), forward pipeline v0, CPU images and the ꟻLIP image metric.
# Depends on rhi (and core, math); never linked by servers.

include(HeliosShaders)

file(GLOB HELIOS_RENDER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
helios_module(render
  SOURCES ${HELIOS_RENDER_SOURCES}
  DEPS helios::rhi helios::core helios::math
  PRIVATE_DEPS helios::tp::headers)
# Pass shaders of the forward pipeline, embedded as SPIR-V (helios_render_shaders.h).
file(GLOB HELIOS_RENDER_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang)
helios_shaders(helios_render NAMESPACE helios_render_shaders FILES ${HELIOS_RENDER_SHADERS})

# helios-shaderc and helios-rendertest live in tools/ (03 §9.1). tools/CMakeLists.txt (owned by another
# work package) does not list them yet, so the render module adds them; each tool's CMakeLists.txt
# returns early if its target already exists, so listing them there later is harmless.
foreach(helios_render_tool shaderc rendertest)
  if(NOT TARGET helios-${helios_render_tool} AND EXISTS ${PROJECT_SOURCE_DIR}/tools/${helios_render_tool}/CMakeLists.txt)
    add_subdirectory(${PROJECT_SOURCE_DIR}/tools/${helios_render_tool}
                     ${CMAKE_CURRENT_BINARY_DIR}/tools/${helios_render_tool})
  endif()
endforeach()

file(GLOB HELIOS_RENDER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(render_tests SOURCES ${HELIOS_RENDER_TEST_SOURCES} DEPS helios::render helios::tp::headers)
if(TARGET render_tests)
  file(GLOB HELIOS_RENDER_TEST_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/*.slang)
  helios_shaders(render_tests NAMESPACE render_test_shaders FILES ${HELIOS_RENDER_TEST_SHADERS})
  target_compile_definitions(render_tests PRIVATE
    HELIOS_RENDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    HELIOS_RENDER_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/render_test_output")
  set_tests_properties(render_tests PROPERTIES TIMEOUT 600)
  if(TARGET helios-shaderc AND TARGET helios-rendertest)
    # spirv-val over every module the renderer, its tests and helios-rendertest embed (required on
    # Linux CI images, where spirv-tools is installed; skipped elsewhere when it is missing).
    find_program(HELIOS_SPIRV_VAL_EXE spirv-val)
    set(helios_render_spv_dirs ${CMAKE_CURRENT_BINARY_DIR}/helios_render_shaders
        ${CMAKE_CURRENT_BINARY_DIR}/render_tests_shaders
        ${CMAKE_CURRENT_BINARY_DIR}/tools/rendertest/helios-rendertest_shaders)
    set(helios_require_spirv_val OFF)
    if(HELIOS_SPIRV_VAL_EXE AND NOT CMAKE_CROSSCOMPILING)
      set(helios_require_spirv_val ON)
    endif()
    add_test(NAME shaderc.spirv-val
             COMMAND ${CMAKE_COMMAND} -DSHADERC=$<TARGET_FILE:helios-shaderc> "-DDIRS=${helios_render_spv_dirs}"
                     -DREQUIRE=${helios_require_spirv_val}
                     -P ${PROJECT_SOURCE_DIR}/tools/shaderc/tests/validate_spirv.cmake)
    set_tests_properties(shaderc.spirv-val PROPERTIES LABELS "shaders" TIMEOUT 300)
  endif()
  if(TARGET helios-shaderc)
    # End-to-end tests drive the real tool (tests/test_shaderc.cpp).
    add_dependencies(render_tests helios-shaderc)
    target_sources(render_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/shaderc/src/tool_process.cpp)
    target_include_directories(render_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools/shaderc/src)
    target_compile_definitions(render_tests PRIVATE
      HELIOS_SHADERC_EXE="$<TARGET_FILE:helios-shaderc>"
      HELIOS_SHADER_ROOT="${PROJECT_SOURCE_DIR}/shaders")
  endif()
endif()
