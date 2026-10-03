# helios-tool: headless ToolsFramework CLI (07 §1.1). Links engine/toolsfw and the sample record
# types only: no EditorUI, no RHI (checked below), so the headless preset builds it. See README.md.
helios_executable(helios-tool
  SOURCES main.cpp
  DEPS helios::toolsfw helios_toolsfw_samples helios::core)

# helios-tool must stay headless: EditorUI, any other non-HEADLESS module but toolsfw, or a graphics
# library anywhere in its link closure is an error. Deferred to the end of configure (after
# helios_finalize_build), when every target's links are final, and walked transitively.
function(_helios_tool_pred_forbidden tgt out)
  set(hit OFF)
  get_property(gfx GLOBAL PROPERTY HELIOS_GRAPHICS_THIRD_PARTY)
  if(tgt IN_LIST gfx)
    set(hit ON)
  endif()
  get_target_property(m "${tgt}" HELIOS_MODULE_NAME)
  if(m AND NOT m STREQUAL "toolsfw")
    get_target_property(headless "${tgt}" HELIOS_HEADLESS)
    if(NOT headless)
      set(hit ON)
    endif()
  endif()
  set(${out} ${hit} PARENT_SCOPE)
endfunction()
function(_helios_tool_check_closure)
  _helios_find_path(helios-tool _helios_tool_pred_forbidden path)
  if(path)
    message(FATAL_ERROR "helios-tool must not link EditorUI, graphics or other non-headless modules (07 §1.1): ${path}")
  endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _helios_tool_check_closure)

# CLI round trip over a copy of the sample project (engine/toolsfw/samples/project): validate,
# fmt --check, apply, undo, redo (across processes, through the journal), journal list/show/verify,
# run, commands.
if(HELIOS_BUILD_TESTS)
  add_test(NAME helios_tool_cli
           COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:helios-tool>
                   -DFIXTURE=${PROJECT_SOURCE_DIR}/engine/toolsfw/samples/project
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/helios_tool_cli_work
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake)
  set_tests_properties(helios_tool_cli PROPERTIES TIMEOUT 300)
endif()
