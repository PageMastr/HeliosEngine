# tools/status — the D6/D7 status snapshot (WP-0.3; 09 §5.10.2). The status check itself runs at the round
# audit (`python3 tools/status/snapshot.py`), not per build; CTest runs its seeded-fixture tests.
if(NOT HELIOS_BUILD_TESTS)
  return()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
  message(STATUS "tools/status: Python 3 not found; lint_status_unittest is not registered")
  return()
endif()
add_test(NAME lint_status_unittest
         COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR} -p "test_*.py")
set_tests_properties(lint_status_unittest PROPERTIES LABELS lint TIMEOUT 120)
