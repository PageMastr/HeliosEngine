# Runs the repository lints that need no build (licences, vendored patches, IP names, shipped pipelines,
# Windows manifest, test namespaces, the D6 status check of 09 §5.10.2, which needs Python 3.10+, and the
# conformance lint of 09 §5.10.3, which needs Go), and the ISA audit when a build directory is given. One
# entry point for CI jobs and pre-commit hooks:
#
#   cmake -P tools/ci/run_lints.cmake                         # from the repository root
#   cmake -DBUILD_DIR=build/linux-gcc -P tools/ci/run_lints.cmake
#   cmake -DHELIOS_STATUS_PYTHON=C:/Python312/python.exe -P tools/ci/run_lints.cmake   # Python not on PATH
#
# Exits non-zero if any lint fails; every lint's own output is shown. See tools/lint/README.md.

cmake_minimum_required(VERSION 3.28)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(lint "${root}/tools/lint")
set(failed "")

function(_run_lint name)
  _run_command(${name} ${CMAKE_COMMAND} ${ARGN})
  set(failed "${failed}" PARENT_SCOPE)
endfunction()

function(_run_command name)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(STRIP "${out}${err}" text)
  message("[${name}] ${text}")
  if(NOT rc EQUAL 0)
    set(failed "${failed} ${name}" PARENT_SCOPE)
  endif()
endfunction()

_run_lint(licenses -DLINT_POLICY=${lint}/license_policy.cmake -DTHIRD_PARTY_DIR=${root}/third_party
          -DMANIFEST=${root}/third_party/MANIFEST.md -P ${lint}/licenses.cmake)
_run_lint(vendor-patches -DTHIRD_PARTY_DIR=${root}/third_party -DMANIFEST=${root}/third_party/MANIFEST.md
          -P ${lint}/vendor_patches.cmake)
_run_lint(ip-names -DLINT_POLICY=${lint}/ip_names_policy.cmake -DSOURCE_DIR=${root} -P ${lint}/ip_names.cmake)
_run_lint(shipped-pipelines -DSOURCE_DIR=${root} -P ${root}/tools/rendertest/tests/shipped_pipelines_lint.cmake)
_run_lint(windows-manifest -DMANIFEST=${root}/engine/platform/win/helios.manifest -P ${lint}/windows_manifest.cmake)
_run_lint(test-namespaces -DSOURCE_DIR=${root} -DREQUIRE_TESTS=ON -P ${lint}/test_namespaces.cmake)
# D6 (09 §5.10.2): every module directory named in 09 §8.1, every module README with its Plan-Rev.
# A Python that is not on PATH (or only the Microsoft Store alias is) is passed through.
set(statusArgs "")
if(HELIOS_STATUS_PYTHON)
  list(APPEND statusArgs "-DHELIOS_STATUS_PYTHON=${HELIOS_STATUS_PYTHON}")
endif()
_run_lint(status ${statusArgs} -P ${root}/tools/status/check_status.cmake)
# CONF-01…12 over the working tree, with tools/conformance/known_failing.jsonc applied (09 §5.10.3).
find_program(HELIOS_CI_go go)
if(HELIOS_CI_go)
  _run_command(conformance ${CMAKE_COMMAND} -E chdir ${root}/tools/conformance
               ${HELIOS_CI_go} run ./cmd/helios-conformance -root ${root})
else()
  message("[conformance] Go is not installed: the conformance lint needs Go 1.27 (ADR-014)")
  set(failed "${failed} conformance")
endif()
if(BUILD_DIR)
  get_filename_component(buildDir "${BUILD_DIR}" ABSOLUTE BASE_DIR "${root}")
  set(tools "")
  foreach(tool objdump nm readelf)
    find_program(HELIOS_CI_${tool} ${tool})
    if(HELIOS_CI_${tool})
      string(TOUPPER "${tool}" var)
      list(APPEND tools -D${var}=${HELIOS_CI_${tool}})
    endif()
  endforeach()
  _run_lint(isa-audit -DALLOWLIST=${root}/cmake/isa_allowlist.cmake -DREQUIRE_GATE=ON ${tools}
            -DCOMPILE_COMMANDS=${buildDir}/compile_commands.json
            -DIMAGES_FILE=${buildDir}/helios_generated/isa_images.txt -P ${lint}/isa_audit.cmake)
endif()

if(failed)
  message(FATAL_ERROR "lints failed:${failed}")
endif()
message(STATUS "all lints passed")
