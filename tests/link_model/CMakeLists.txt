# Link-model tests (ADR-016, 02 §1.4, WP-0.6c part 1). Only a modular dev build (HELIOS_MODULAR=ON) has
# link groups, so a shipping build adds nothing here.
#
#   link_model_tests     the test executable: the group libraries export their entry points, and a plugin
#                        image built against them shares the engine's registries, heaps and CRT heap.
#   link_model_probe     that plugin: a game-like image that links helios::core (helios_runtime) and keeps
#                        the symbol audit's game rules (tools/lint/symbol_audit.cmake). WP-0.6c part 2
#                        replaces it with the Probe game module and the reload loader.
#   link_model_bad_game  a game-like image that breaks the audit's game rules on purpose (a static
#                        mimalloc, engine data, a strong copy of an engine function). It is never loaded;
#                        the lint_symbol_audit_game_* fixtures (ELF builds) must reject it.
if(NOT HELIOS_MODULAR OR NOT HELIOS_BUILD_TESTS)
  return()
endif()

add_library(link_model_probe SHARED ${CMAKE_CURRENT_SOURCE_DIR}/probe.cpp)
target_link_libraries(link_model_probe PRIVATE helios::core)
set_target_properties(link_model_probe PROPERTIES FOLDER tests)
helios_apply_warnings(link_model_probe)

# The audit reads ELF symbol tables only (a PE image has none), so the fixture is built where it is used.
# Default visibility: its strong copy of an engine function must be a global definition (rule R6).
if(NOT WIN32)
  add_library(link_model_bad_game SHARED ${CMAKE_CURRENT_SOURCE_DIR}/bad_game.cpp)
  target_link_libraries(link_model_bad_game PRIVATE helios::core helios::tp::mimalloc)
  set_target_properties(link_model_bad_game PROPERTIES FOLDER tests CXX_VISIBILITY_PRESET default)
  helios_apply_warnings(link_model_bad_game)
endif()

helios_test(link_model_tests SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_link_model.cpp DEPS helios::core)
add_dependencies(link_model_tests link_model_probe)
target_compile_definitions(link_model_tests PRIVATE HELIOS_LINK_MODEL_PROBE_PATH="$<TARGET_FILE:link_model_probe>")
# The other groups exist only in builds with graphics (client) and with editor modules (editor).
foreach(group client editor)
  if(TARGET helios_${group})
    string(TOUPPER "${group}" G)
    add_dependencies(link_model_tests helios_${group})
    target_compile_definitions(link_model_tests PRIVATE
      HELIOS_LINK_MODEL_${G}_PATH="$<TARGET_FILE:helios_${group}>")
  endif()
endforeach()
