# tools/milestone — validate.ps1, the user's Windows milestone validation (WP-0.3; 09 §5.9). CTest runs its
# self-test (toolset rule, compiler-version lookup, JUnit summary, report format, ASCII encoding) with
# Windows PowerShell on Windows and PowerShell 7 elsewhere, when one is installed.
if(NOT HELIOS_BUILD_TESTS)
  return()
endif()
if(CMAKE_HOST_WIN32)
  find_program(HELIOS_POWERSHELL NAMES powershell pwsh)
else()
  find_program(HELIOS_POWERSHELL NAMES pwsh)
endif()
if(NOT HELIOS_POWERSHELL)
  message(STATUS "tools/milestone: PowerShell not found; lint_milestone_selftest is not registered")
  return()
endif()
add_test(NAME lint_milestone_selftest
         COMMAND ${HELIOS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/validate.ps1 -SelfTest)
set_tests_properties(lint_milestone_selftest PROPERTIES LABELS lint TIMEOUT 120)
