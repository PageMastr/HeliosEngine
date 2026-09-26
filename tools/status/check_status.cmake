# D6 status check (docs/plan/09-roadmap-and-process.md §5.10.2 D6 and D7).
#
#   cmake -P tools/status/check_status.cmake        # from anywhere; exits non-zero on a stale status
#
# WP-0.3's tools/status/snapshot.py absorbed this check; this entry point delegates to
# `snapshot.py check` so the round-audit command in the plan keeps working. It fails when a module
# directory under engine/, apps/, tools/, services/cmd/, services/internal/ or services/pkg/ is not named
# in 09 §8.1 (or has no WP in its tree inventory row), or a module README has no "Plan-Rev: <n>" line or
# one above docs/plan/PLAN-REV. It needs Python 3, no build and no network.

cmake_minimum_required(VERSION 3.28)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

find_program(HELIOS_STATUS_PYTHON NAMES python3 python py)
if(NOT HELIOS_STATUS_PYTHON)
  message(FATAL_ERROR "check_status: Python 3 is required (tools/status/snapshot.py)")
endif()
set(launcher "")
get_filename_component(exe "${HELIOS_STATUS_PYTHON}" NAME_WE)
if(exe STREQUAL "py")
  set(launcher -3)  # the Windows launcher
endif()
execute_process(COMMAND "${HELIOS_STATUS_PYTHON}" ${launcher} "${root}/tools/status/snapshot.py" check
                RESULT_VARIABLE rc)
if(rc EQUAL 9009)
  message(FATAL_ERROR "check_status: ${HELIOS_STATUS_PYTHON} is not a Python 3 (the Windows Store alias?)")
elseif(NOT rc EQUAL 0)
  message(FATAL_ERROR "check_status: the status in 09 §8.1 or a module README is stale (snapshot.py exit ${rc})")
endif()
