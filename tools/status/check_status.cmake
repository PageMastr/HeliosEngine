# D6 status check (docs/plan/09-roadmap-and-process.md §5.10.2 D6 and D7).
#
#   cmake -P tools/status/check_status.cmake        # from anywhere; exits non-zero on a stale status
#   cmake -DHELIOS_STATUS_PYTHON=C:/Python312/python.exe -P tools/status/check_status.cmake
#
# WP-0.3's tools/status/snapshot.py absorbed this check; this entry point delegates to
# `snapshot.py check` so the round-audit command in the plan keeps working. It fails when a module
# directory under engine/, apps/, tools/, services/cmd/, services/internal/ or services/pkg/ is not named
# in 09 §8.1 (or has no WP in its tree inventory row), or a module README has no "Plan-Rev: <n>" line or
# one above docs/plan/PLAN-REV. It needs Python 3.10 or later, no build and no network.
#
# The interpreter search tries every PATH directory for each name in turn (Windows: py -3, python,
# python3; elsewhere python3, python) and keeps the first candidate that really runs Python 3.10+, so the
# Microsoft Store's python.exe/python3.exe aliases (which exit 9009) are skipped, not fatal.

cmake_minimum_required(VERSION 3.28)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(_helios_python_works exe out_ok)
  set(extra "")
  get_filename_component(stem "${exe}" NAME_WE)
  if(stem STREQUAL "py")
    set(extra -3)
  endif()
  execute_process(COMMAND "${exe}" ${extra} -c "import sys; sys.exit(sys.version_info < (3, 10))"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET TIMEOUT 60)
  if(rc EQUAL 0)
    set(${out_ok} ON PARENT_SCOPE)
  else()
    set(${out_ok} OFF PARENT_SCOPE)
  endif()
endfunction()

if(CMAKE_HOST_WIN32)
  set(names py python python3)
  set(suffix .exe)
else()
  set(names python3 python)
  set(suffix "")
endif()
set(python "")
if(HELIOS_STATUS_PYTHON)
  _helios_python_works("${HELIOS_STATUS_PYTHON}" ok)
  if(NOT ok)
    message(FATAL_ERROR "check_status: HELIOS_STATUS_PYTHON=${HELIOS_STATUS_PYTHON} does not run Python 3.10+")
  endif()
  set(python "${HELIOS_STATUS_PYTHON}")
else()
  file(TO_CMAKE_PATH "$ENV{PATH}" dirs)
  foreach(name IN LISTS names)
    foreach(dir IN LISTS dirs)
      set(candidate "${dir}/${name}${suffix}")
      if(dir STREQUAL "" OR NOT EXISTS "${candidate}" OR IS_DIRECTORY "${candidate}")
        continue()
      endif()
      _helios_python_works("${candidate}" ok)
      if(ok)
        set(python "${candidate}")
        break()
      endif()
    endforeach()
    if(python)
      break()
    endif()
  endforeach()
endif()
if(NOT python)
  string(REPLACE ";" ", " tried "${names}")
  message(FATAL_ERROR "check_status: no Python 3.10+ found on PATH (tried ${tried}); install Python or pass "
                      "-DHELIOS_STATUS_PYTHON=<path to python>")
endif()

set(launcher "")
get_filename_component(stem "${python}" NAME_WE)
if(stem STREQUAL "py")
  set(launcher -3)
endif()
execute_process(COMMAND "${python}" ${launcher} "${root}/tools/status/snapshot.py" check RESULT_VARIABLE rc)
if(rc EQUAL 1)
  message(FATAL_ERROR "check_status: the status in 09 §8.1 or a module README is stale")
elseif(NOT rc EQUAL 0)
  message(FATAL_ERROR "check_status: snapshot.py failed (exit ${rc}; ${python})")
endif()
