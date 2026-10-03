# tools/conformance — the plan-conformance lint (09 §5.10.3; WP-0.2), a Go tool (README.md). CTests, all
# labelled `lint`:
#   lint_conformance_build   builds helios-conformance once (the CTest fixture the tests below require);
#   lint_conformance_unit    `go test ./...`: every seeded tree's exact findings and the framework's tests;
#   lint_conformance         the repository, with tools/conformance/known_failing.jsonc applied;
#   lint_conformance_fixture_<rule>_<case>  each tree under testdata/ through the CLI: a case whose
#                            expect.txt lists findings must fail with them (tools/lint/expect_fail.cmake),
#                            one that lists none must pass.
# Go 1.27 is needed (ADR-014; an older `go` fetches it through GOTOOLCHAIN=auto). Optional locally, and
# required in CI so the tests cannot silently disappear.
if(NOT HELIOS_BUILD_TESTS)
  return()
endif()
find_program(HELIOS_GO go)
if(NOT HELIOS_GO)
  if(DEFINED ENV{CI})
    message(FATAL_ERROR "tools/conformance: Go is required in CI (the lint_conformance tests would silently disappear)")
  endif()
  message(STATUS "tools/conformance: Go not found; the lint_conformance tests are not registered")
  return()
endif()

set(dir ${CMAKE_CURRENT_SOURCE_DIR})
set(bin ${CMAKE_CURRENT_BINARY_DIR}/helios-conformance)
if(CMAKE_HOST_WIN32)
  string(APPEND bin .exe)
endif()
add_test(NAME lint_conformance_build COMMAND ${HELIOS_GO} build -o ${bin} ./cmd/helios-conformance
         WORKING_DIRECTORY ${dir})
set_tests_properties(lint_conformance_build PROPERTIES LABELS lint TIMEOUT 600
                     FIXTURES_SETUP helios_conformance)
add_test(NAME lint_conformance_unit COMMAND ${HELIOS_GO} test -count=1 ./... WORKING_DIRECTORY ${dir})
set_tests_properties(lint_conformance_unit PROPERTIES LABELS lint TIMEOUT 600)
add_test(NAME lint_conformance COMMAND ${bin} -root ${PROJECT_SOURCE_DIR})
set_tests_properties(lint_conformance PROPERTIES LABELS lint TIMEOUT 300 FIXTURES_REQUIRED helios_conformance)

# One CTest per seeded tree. expect.txt: an optional `#! <args>` line, then `<path>[:<line>]: <rule>` lines.
file(GLOB cases LIST_DIRECTORIES true RELATIVE ${dir}/testdata ${dir}/testdata/*/*)
foreach(case IN LISTS cases)
  if(NOT EXISTS ${dir}/testdata/${case}/expect.txt)
    continue()
  endif()
  file(STRINGS ${dir}/testdata/${case}/expect.txt lines)
  set(args "")
  set(expect "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^#!(.*)$")
      separate_arguments(extra UNIX_COMMAND "${CMAKE_MATCH_1}")
      list(APPEND args ${extra})
    elseif(NOT line STREQUAL "" AND NOT line MATCHES "^#")
      string(REGEX REPLACE "([.+*?()^$|])" "\\\\\\1" line "${line}")
      if(expect)
        string(APPEND expect ".*")
      endif()
      string(APPEND expect "${line}")
    endif()
  endforeach()
  string(REPLACE "/" "_" name "${case}")
  set(cmd ${bin} -root ${dir}/testdata/${case} ${args})
  if(expect)
    set(wrapped "")
    set(i 0)
    foreach(arg IN LISTS cmd)
      list(APPEND wrapped "-DARG${i}=${arg}")
      math(EXPR i "${i} + 1")
    endforeach()
    add_test(NAME lint_conformance_fixture_${name}
             COMMAND ${CMAKE_COMMAND} "-DEXPECT=${expect}" -DNARGS=${i} ${wrapped}
                     -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
  else()
    add_test(NAME lint_conformance_fixture_${name} COMMAND ${cmd})
  endif()
  set_tests_properties(lint_conformance_fixture_${name} PROPERTIES LABELS lint TIMEOUT 120
                       FIXTURES_REQUIRED helios_conformance)
endforeach()
