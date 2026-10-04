# The self-hosted runner policy check (09 §5.4a; WP-0.4) for tools/ci/run_lints.cmake, which needs no build:
#
#   cmake -P tools/ci/check_runner_policy.cmake
#   cmake -DHELIOS_STATUS_PYTHON=C:/Python312/python.exe -P tools/ci/check_runner_policy.cmake
#
# Runs check_runner_policy.py over .github/workflows with the first Python 3.10+ found the way
# tools/status/check_status.cmake finds it (each PATH directory for each name in turn: on Windows py -3,
# python, python3, skipping the Microsoft Store aliases), or with HELIOS_STATUS_PYTHON.

cmake_minimum_required(VERSION 3.28)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(_helios_python_ok exe out)
  set(extra "")
  get_filename_component(stem "${exe}" NAME_WE)
  if(stem STREQUAL "py")
    set(extra -3)
  endif()
  execute_process(COMMAND "${exe}" ${extra} -c "import sys; sys.exit(sys.version_info < (3, 10))"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET TIMEOUT 60)
  if(rc EQUAL 0)
    set(${out} ON PARENT_SCOPE)
  else()
    set(${out} OFF PARENT_SCOPE)
  endif()
endfunction()

set(python "")
if(HELIOS_STATUS_PYTHON)
  _helios_python_ok("${HELIOS_STATUS_PYTHON}" ok)
  if(NOT ok)
    message(FATAL_ERROR "check_runner_policy: HELIOS_STATUS_PYTHON=${HELIOS_STATUS_PYTHON} does not run Python 3.10+")
  endif()
  set(python "${HELIOS_STATUS_PYTHON}")
else()
  if(CMAKE_HOST_WIN32)
    set(names py python python3)
    set(suffix .exe)
  else()
    set(names python3 python)
    set(suffix "")
  endif()
  file(TO_CMAKE_PATH "$ENV{PATH}" dirs)
  foreach(name IN LISTS names)
    foreach(dir IN LISTS dirs)
      set(candidate "${dir}/${name}${suffix}")
      if(dir STREQUAL "" OR NOT EXISTS "${candidate}" OR IS_DIRECTORY "${candidate}")
        continue()
      endif()
      _helios_python_ok("${candidate}" ok)
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
  message(FATAL_ERROR "check_runner_policy: no Python 3.10+ found on PATH; install Python or pass "
                      "-DHELIOS_STATUS_PYTHON=<path to python>")
endif()

set(launcher "")
get_filename_component(stem "${python}" NAME_WE)
if(stem STREQUAL "py")
  set(launcher -3)
endif()
execute_process(COMMAND "${python}" ${launcher} -B "${root}/tools/ci/check_runner_policy.py"
                        --workflows "${root}/.github/workflows" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "check_runner_policy: a workflow breaks the win-gpu runner policy (09 §5.4a), exit ${rc}")
endif()
