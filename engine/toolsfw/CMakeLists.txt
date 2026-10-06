# engine/toolsfw — L4 ToolsFramework (EDITOR_ONLY; 07 §1.2, ADR-009): the UI-less editor core —
# documents over reflected data, selection, the command bus with its input-path origins,
# property-path transactions with undo/redo and grouping, the crash-recovery journal, the
# JSON-RPC 2.0 remote-control socket and the Luau automation VM. Linked by helios-editor, helios-tool
# and helios-uitest (07 §1.1); never by the client, launcher, bot or servers (checked at configure time).
# It never includes ImGui or RHI headers.
#
# OS-specific code lives only in src/platform/win32 (named pipes; selected when WIN32, MinGW
# included) and src/platform/posix (Unix-domain sockets).

include(HeliosSchema)

file(GLOB HELIOS_TOOLSFW_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(WIN32)
  file(GLOB HELIOS_TOOLSFW_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/*.cpp)
else()
  file(GLOB HELIOS_TOOLSFW_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/*.cpp)
endif()

helios_module(toolsfw
  SOURCES ${HELIOS_TOOLSFW_SOURCES} ${HELIOS_TOOLSFW_PLATFORM_SOURCES}
  DEPS helios::core helios::reflect
  PRIVATE_DEPS helios::script)

# ---------------------------------------------------------------------------------------------
# Sample record types. Until the reference-content schemas exist (WP-0.20) and gems register
# their own types (07 §1.10), the Phase 0 editor, helios-tool, samples/project and these tests use
# the sample schemas (sample.ship.ShipHullDef is the Frigate hull record of the M0 demo).
# ---------------------------------------------------------------------------------------------
add_library(helios_toolsfw_samples STATIC)
helios_schema(helios_toolsfw_samples
  FILES ${PROJECT_SOURCE_DIR}/schemas/sample/common.hschema
        ${PROJECT_SOURCE_DIR}/schemas/sample/ship.hschema
        ${PROJECT_SOURCE_DIR}/schemas/sample/items.hschema
  INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/schemas
  LOCK ${PROJECT_SOURCE_DIR}/schemas/sample/schema.lock.jsonc)
target_sources(helios_toolsfw_samples PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/samples/register_samples.cpp)
target_include_directories(helios_toolsfw_samples PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/samples/include)
set_target_properties(helios_toolsfw_samples PROPERTIES FOLDER engine POSITION_INDEPENDENT_CODE ON)
helios_apply_warnings(helios_toolsfw_samples)

# ---------------------------------------------------------------------------------------------
# Tests (doctest). ED-1 lives in test_ed1.cpp; its kill -9 half drives toolsfw_burst_child.
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_TOOLSFW_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
set(HELIOS_TOOLSFW_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/workload.cpp
                                ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/test_project.cpp)
helios_test(toolsfw_tests
  SOURCES ${HELIOS_TOOLSFW_TEST_SOURCES} ${HELIOS_TOOLSFW_TEST_SUPPORT}
  DEPS helios::toolsfw helios_toolsfw_samples)
if(TARGET toolsfw_tests)
  target_include_directories(toolsfw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_executable(toolsfw_burst_child ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/burst_child.cpp ${HELIOS_TOOLSFW_TEST_SUPPORT})
  target_include_directories(toolsfw_burst_child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(toolsfw_burst_child PRIVATE helios::toolsfw helios_toolsfw_samples)
  set_target_properties(toolsfw_burst_child PROPERTIES FOLDER tests HELIOS_APP_ROLE test)
  helios_apply_warnings(toolsfw_burst_child)
  helios_windows_manifest(toolsfw_burst_child)

  add_dependencies(toolsfw_tests toolsfw_burst_child)
  target_compile_definitions(toolsfw_tests PRIVATE
    HELIOS_TOOLSFW_BURST_CHILD="$<TARGET_FILE:toolsfw_burst_child>"
    HELIOS_TOOLSFW_TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/toolsfw_test_work")
  # One timeout for every build: with commits O(their ops) (the history and log byte totals are running
  # sums), the whole binary takes about 12 s in RelWithDebInfo and 190 s under GCC 13 Debug +
  # ASan/UBSan on the shared 4-vCPU dev container (17 min before, when every commit re-summed the log).
  set_tests_properties(toolsfw_tests PROPERTIES TIMEOUT 900)
endif()
