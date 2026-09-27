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

# Deferred to the end of the top-level directory (see the call below): by then every helios_shaders()
# call has appended its modules to HELIOS_SHADER_MODULES, which is written to a file (the list grows
# with the tree; a file keeps it off the command line).
function(helios_render_add_spirv_val_test)
  get_property(modules GLOBAL PROPERTY HELIOS_SHADER_MODULES)
  get_property(require GLOBAL PROPERTY HELIOS_REQUIRE_SPIRV_VAL)
  set(list_file ${CMAKE_BINARY_DIR}/helios_generated/shader_modules.txt)
  string(REPLACE ";" "\n" content "${modules}")
  file(CONFIGURE OUTPUT ${list_file} CONTENT "${content}\n" @ONLY)
  set(validator "")
  if(require)
    set(validator "-DSPIRV_VAL=${HELIOS_SPIRV_VAL_EXE}")  # the one configure found, even off PATH
  endif()
  add_test(NAME shaderc.spirv-val
           COMMAND ${CMAKE_COMMAND} -DSHADERC=$<TARGET_FILE:helios-shaderc> -DMODULES_FILE=${list_file}
                   -DREQUIRE=${require} ${validator} -P ${PROJECT_SOURCE_DIR}/tools/shaderc/tests/validate_spirv.cmake)
  set_tests_properties(shaderc.spirv-val PROPERTIES LABELS "shaders" TIMEOUT 300
                       SKIP_REGULAR_EXPRESSION "spirv-val not found: validation skipped")
  # The script's own failure modes: an empty module list, and a declared module that was not built.
  set(fixtures ${CMAKE_BINARY_DIR}/helios_generated/spirv_val_fixtures)
  file(WRITE ${fixtures}/empty.txt "\n")
  file(WRITE ${fixtures}/missing.txt "${fixtures}/not_built.spv\n")
  foreach(case "empty|lists no SPIR-V modules" "missing|declared SPIR-V modules were not built")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 fixture)
    list(GET parts 1 expect)
    add_test(NAME shaderc.spirv-val.fixture_${fixture}
             COMMAND ${CMAKE_COMMAND} "-DEXPECT=${expect}" -DNARGS=5 -DARG0=${CMAKE_COMMAND}
                     -DARG1=-DSHADERC=$<TARGET_FILE:helios-shaderc> -DARG2=-DMODULES_FILE=${fixtures}/${fixture}.txt
                     -DARG3=-P -DARG4=${PROJECT_SOURCE_DIR}/tools/shaderc/tests/validate_spirv.cmake
                     -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
    set_tests_properties(shaderc.spirv-val.fixture_${fixture} PROPERTIES LABELS "shaders" TIMEOUT 60)
  endforeach()
endfunction()

file(GLOB HELIOS_RENDER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(render_tests SOURCES ${HELIOS_RENDER_TEST_SOURCES} DEPS helios::render helios::tp::headers)
if(TARGET render_tests)
  file(GLOB HELIOS_RENDER_TEST_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/*.slang)
  helios_shaders(render_tests NAMESPACE render_test_shaders FILES ${HELIOS_RENDER_TEST_SHADERS})
  target_compile_definitions(render_tests PRIVATE
    HELIOS_RENDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    HELIOS_RENDER_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/render_test_output")
  set_tests_properties(render_tests PROPERTIES TIMEOUT 600)
  if(TARGET helios-shaderc)
    # spirv-val over every SPIR-V module the build cooks (03 §1.7): the modules of every
    # helios_shaders() call in the tree, so the test is added once the top-level directory has been
    # processed. Required where spirv-tools is installed (the Linux CI images); without spirv-val the
    # test reports Skipped, never Passed.
    find_program(HELIOS_SPIRV_VAL_EXE spirv-val)
    set(helios_require_spirv_val OFF)
    if(HELIOS_SPIRV_VAL_EXE AND NOT CMAKE_CROSSCOMPILING)
      set(helios_require_spirv_val ON)
    endif()
    set_property(GLOBAL PROPERTY HELIOS_REQUIRE_SPIRV_VAL ${helios_require_spirv_val})
    cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL helios_render_add_spirv_val_test)
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
