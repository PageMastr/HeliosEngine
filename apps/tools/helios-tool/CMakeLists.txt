# helios-tool: headless ToolsFramework CLI (07 §1.1). Links engine/toolsfw and the sample record
# types only: no EditorUI, no RHI (checked below), so the headless preset builds it. See README.md.
helios_executable(helios-tool
  SOURCES main.cpp
  DEPS helios::toolsfw helios_toolsfw_samples helios::core)

# helios-tool must stay headless: EditorUI or a graphics library in its link closure is an error.
set(helios_tool_forbidden helios_editorui helios_rhi helios_render tp_imgui SDL3-static tp_volk)
foreach(dep helios::toolsfw helios_toolsfw_samples helios::core)
  if(TARGET ${dep})
    get_target_property(real ${dep} ALIASED_TARGET)
    if(NOT real)
      set(real ${dep})
    endif()
    get_target_property(links ${real} LINK_LIBRARIES)
    get_target_property(ilinks ${real} INTERFACE_LINK_LIBRARIES)
    foreach(l ${links} ${ilinks})
      string(REGEX REPLACE "^helios::" "helios_" l "${l}")
      if(l IN_LIST helios_tool_forbidden)
        message(FATAL_ERROR "helios-tool: ${dep} links ${l}; helios-tool must not link EditorUI or graphics (07 §1.1)")
      endif()
    endforeach()
  endif()
endforeach()

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
