# engine/script — L3 Luau scripting host (HEADLESS; ADR-002, 02 §7.4, 04 §3.1/§10.2, 06 §11):
# sandboxed per-zone VMs with a capped, tagged allocator; fuel-metered budgets with sticky kills and
# no involuntary yields; a coroutine task scheduler on the zone clock; modules with hot reload;
# generational-handle object bindings and WorldPos userdata; bytecode cache and opt-in codegen.

file(GLOB HELIOS_SCRIPT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

helios_module(script HEADLESS
  SOURCES ${HELIOS_SCRIPT_SOURCES}
  DEPS helios::core helios::math helios::tp::luau)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_SCRIPT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(script_tests SOURCES ${HELIOS_SCRIPT_TEST_SOURCES} DEPS helios::script)
if(TARGET script_tests)
  # White-box checks (wrapped-builtin list, private state) include src/vm_state.h.
  target_include_directories(script_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # The Luau type-definition stub is parsed by a test (declaration syntax + API coverage).
  target_compile_definitions(script_tests PRIVATE
    HELIOS_SCRIPT_DEFS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/defs/helios.d.luau")
  set_tests_properties(script_tests PROPERTIES TIMEOUT 600)
endif()
