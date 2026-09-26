# Runs the repository lints that need no build (licences, vendored patches, IP names, Windows
# manifest), and the ISA audit when a build directory is given. One entry point for CI jobs and
# pre-commit hooks:
#
#   cmake -P tools/ci/run_lints.cmake                         # from the repository root
#   cmake -DBUILD_DIR=build/linux-gcc -P tools/ci/run_lints.cmake
#
# Exits non-zero if any lint fails; every lint's own output is shown. See tools/lint/README.md.

cmake_minimum_required(VERSION 3.28)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(lint "${root}/tools/lint")
set(failed "")

function(_run_lint name)
  execute_process(COMMAND ${CMAKE_COMMAND} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
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
_run_lint(windows-manifest -DMANIFEST=${root}/engine/platform/win/helios.manifest -P ${lint}/windows_manifest.cmake)
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
