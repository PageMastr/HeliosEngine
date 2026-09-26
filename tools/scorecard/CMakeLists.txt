# tools/scorecard — the AAA scorecard registry checker (WP-0.3; 09 §5.6). Registers:
#   lint_scorecard           scorecard.jsonc against the plan's criteria and ci.yml, and in native builds
#                            with graphics against this build's CTest and doctest inventory (so a renamed
#                            or deleted test that the scorecard cites fails the PR);
#   lint_scorecard_unittest  the checker's seeded-fixture tests.
# Python 3.10+ is optional locally (the tests are not registered without it) and required in CI.
if(NOT HELIOS_BUILD_TESTS)
  return()
endif()
find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND AND DEFINED ENV{CI})
  message(FATAL_ERROR "scorecard: Python 3.10+ is required in CI (lint_scorecard would silently disappear)")
endif()
if(NOT Python3_Interpreter_FOUND)
  message(STATUS "tools/scorecard: Python 3.10+ not found; lint_scorecard is not registered")
  return()
endif()

set(args check --scorecard ${PROJECT_SOURCE_DIR}/scorecard.jsonc)
# Headless builds lack the graphics tests and cross builds cannot run the doctest binaries, so those
# check the registry only; the nightly checks Go tests against its `go test -list` inventory.
if(HELIOS_BUILD_GRAPHICS AND NOT CMAKE_CROSSCOMPILING)
  list(APPEND args --build-dir ${PROJECT_BINARY_DIR} --config $<CONFIG> --ctest ${CMAKE_CTEST_COMMAND})
endif()
add_test(NAME lint_scorecard COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scorecard.py ${args})
add_test(NAME lint_scorecard_unittest
         COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR} -p "test_*.py")
set_tests_properties(lint_scorecard lint_scorecard_unittest PROPERTIES LABELS lint TIMEOUT 300)
