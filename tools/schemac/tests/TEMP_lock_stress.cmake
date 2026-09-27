# TEMPORARY (reverted before merge): runs the schemac_tests cases matching FILTER COUNT times and
# reports how many iterations failed, to reproduce the intermittent Windows failure of
# "cli: parallel runs sharing one lock never lose each other's entries" on CI and to verify the fix.
# Usage: cmake -DEXE=<schemac_tests> -DCOUNT=<n> -DFILTER=<doctest -tc filter> -P TEMP_lock_stress.cmake
set(failed 0)
set(first_failure "")
foreach(i RANGE 1 ${COUNT})
  execute_process(COMMAND "${EXE}" "--test-case=${FILTER}" "--no-version=true"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    math(EXPR failed "${failed} + 1")
    message("iteration ${i}/${COUNT} FAILED (exit ${rc})")
    if(first_failure STREQUAL "")
      set(first_failure "${out}")
    endif()
  endif()
endforeach()
if(failed GREATER 0)
  message(FATAL_ERROR "${failed} of ${COUNT} iterations failed; first failure:\n${first_failure}")
endif()
message("all ${COUNT} iterations passed (filter: ${FILTER})")
