# Runs a lint on a seeded violation and passes only if the lint FAILS (non-zero exit) AND prints the
# expected diagnostic. (A PASS_REGULAR_EXPRESSION alone would also accept a lint that reports the
# finding but exits 0, which CI would never notice.)
#
#   cmake -DEXPECT=<regex> -DNARGS=<n> -DARG0=<program> -DARG1=... -P expect_fail.cmake

cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED EXPECT OR NOT NARGS)
  message(FATAL_ERROR "expect_fail: pass -DEXPECT=<regex> -DNARGS=<n> -DARG0=...")
endif()
set(cmd "")
math(EXPR last "${NARGS} - 1")
foreach(i RANGE ${last})
  list(APPEND cmd "${ARG${i}}")
endforeach()
execute_process(COMMAND ${cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(rc EQUAL 0)
  message(FATAL_ERROR "expect_fail: the lint succeeded on a seeded violation (expected a failure matching '${EXPECT}'). Output:\n${all}")
endif()
if(NOT all MATCHES "${EXPECT}")
  message(FATAL_ERROR "expect_fail: the lint failed, but without the expected diagnostic '${EXPECT}'. Output:\n${all}")
endif()
message(STATUS "failed as expected (exit ${rc}): ${EXPECT}")
