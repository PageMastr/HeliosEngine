# tools/ci — CI policy checks (WP-0.4; 09 §5.4a, K33). Registers, all labelled `lint`:
#   lint_runner_policy            check_runner_policy.py over .github/workflows: a workflow whose jobs reach the
#                                 self-hosted win-gpu runner must keep 09 §5.4a's rules (post-merge triggers, the
#                                 main and HELIOS_WIN_GPU guards, no secrets, contents: read, pinned actions);
#   lint_runner_policy_unittest   its YAML parser and seeded fixtures (testdata/runner_policy/);
#   lint_runner_scripts           runner/test_runner_scripts.ps1: the runner hook and firewall scripts parse, are
#                                 ASCII, and their logic holds (the wipe itself on Windows, where the test must
#                                 say that the wipe ran); the PowerShell blocks of docs/runbooks/win-gpu-runner.md
#                                 parse, and its drive-root ACL audit (step 4b) holds against stand-ins; with
#                                 PowerShell 7.
# Python 3.10+ is required in CI, as for tools/scorecard. pwsh is required on GitHub-hosted runners, which all
# have it; elsewhere (the dev container, the owner's win-gpu PC) lint_runner_scripts is registered only if found.
if(NOT HELIOS_BUILD_TESTS)
  return()
endif()
find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND AND DEFINED ENV{CI})
  message(FATAL_ERROR "tools/ci: Python 3.10+ is required in CI (lint_runner_policy would silently disappear)")
endif()
if(Python3_Interpreter_FOUND)
  add_test(NAME lint_runner_policy
           COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/check_runner_policy.py
                   --workflows ${PROJECT_SOURCE_DIR}/.github/workflows)
  add_test(NAME lint_runner_policy_unittest
           COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
                   -p "test_check_runner_policy.py")
  set_tests_properties(lint_runner_policy lint_runner_policy_unittest PROPERTIES LABELS lint TIMEOUT 300)
else()
  message(STATUS "tools/ci: Python 3.10+ not found; lint_runner_policy is not registered")
endif()

find_program(HELIOS_PWSH pwsh)
if(NOT HELIOS_PWSH AND "$ENV{RUNNER_ENVIRONMENT}" STREQUAL "github-hosted")
  message(FATAL_ERROR "tools/ci: pwsh is required on GitHub-hosted runners (lint_runner_scripts would silently disappear)")
endif()
if(HELIOS_PWSH)
  add_test(NAME lint_runner_scripts
           COMMAND ${HELIOS_PWSH} -NoProfile -NonInteractive -File ${CMAKE_CURRENT_SOURCE_DIR}/runner/test_runner_scripts.ps1
                   -WorkDir ${CMAKE_CURRENT_BINARY_DIR}/runner-scripts-test)
  set_tests_properties(lint_runner_scripts PROPERTIES LABELS lint TIMEOUT 300)
  if(CMAKE_HOST_WIN32)
    # The wipe must have run, not been skipped. The script prints this only when every check passed (CTest ignores
    # the exit code once PASS_REGULAR_EXPRESSION is set).
    set_tests_properties(lint_runner_scripts PROPERTIES PASS_REGULAR_EXPRESSION "checks passed \\(wipe: ran\\)")
  endif()
else()
  message(STATUS "tools/ci: pwsh not found; lint_runner_scripts is not registered")
endif()
