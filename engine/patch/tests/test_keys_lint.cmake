# The test-key opt-in lint (engine/patch/README.md, "Test-only and dev keys"). TrustOptions::allowTestKeys lets
# a TrustVerifier accept the shared vectors' test-only roots, whose seeds are public; only the patch tests and
# fuzz targets may name it. So in engine/, apps/ and tools/, the identifier allowTestKeys anywhere but
#   engine/patch/include/helios/patch/trust.h   declares it
#   engine/patch/src/trust.cpp                  reads it (TrustVerifier::create)
#   engine/patch/tests/                         the tests, and this lint's fixtures
#   engine/patch/fuzz/                          the fuzz targets
# fails, with path:line, comments included. Go's twin is TestTrustTestImportedOnlyByTests (services/pkg/patchtrust),
# which does the same for Options.AllowTestKeys. A lint run that finds no allowed use at all fails too (the scan
# no longer sees the declaration). Threat model: a best-effort textual check against accidental use; a name
# built by token pasting, or a file with an extension the scan does not take (.cpp .cc .cxx .cppm .ixx .h .hh
# .hpp .hxx .inl .inc .ipp), is not seen; review is the backstop, and TrustVerifier::create still refuses the
# test-only roots without the option.
#
#   cmake -DSOURCE_DIR=<repository root> -P test_keys_lint.cmake

cmake_minimum_required(VERSION 3.28)
if(NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "test_keys_lint: SOURCE_DIR is not a directory: '${SOURCE_DIR}'")
endif()
get_filename_component(SOURCE_DIR "${SOURCE_DIR}" ABSOLUTE)

set(files "")
foreach(root engine apps tools)
  if(IS_DIRECTORY "${SOURCE_DIR}/${root}")
    set(patterns "")
    foreach(ext cpp cc cxx cppm ixx h hh hpp hxx inl inc ipp)
      list(APPEND patterns "${SOURCE_DIR}/${root}/*.${ext}")
    endforeach()
    file(GLOB_RECURSE found LIST_DIRECTORIES FALSE ${patterns})
    list(APPEND files ${found})
  endif()
endforeach()
list(SORT files)

set(findings "")
set(allowed 0)
foreach(f IN LISTS files)
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${f}")
  if(rel MATCHES "^tools/prebuilt/")
    continue()
  endif()
  file(READ "${f}" text)
  string(FIND "${text}" "allowTestKeys" at)
  if(at EQUAL -1)
    continue()
  endif()
  if(rel MATCHES "^engine/patch/(tests|fuzz)/" OR rel STREQUAL "engine/patch/include/helios/patch/trust.h" OR
     rel STREQUAL "engine/patch/src/trust.cpp")
    math(EXPR allowed "${allowed} + 1")
    continue()
  endif()
  # One list element per line; characters with a meaning in CMake lists cannot split or join lines.
  string(REPLACE "\r" "" text "${text}")
  string(REPLACE "\\" "/" text "${text}")
  string(REPLACE ";" "," text "${text}")
  string(REPLACE "[" "(" text "${text}")
  string(REPLACE "]" ")" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(number 0)
  foreach(line IN LISTS lines)
    math(EXPR number "${number} + 1")
    if(line MATCHES "allowTestKeys")
      list(APPEND findings "${rel}:${number}: allowTestKeys outside engine/patch/tests and engine/patch/fuzz")
    endif()
  endforeach()
endforeach()

if(findings)
  list(LENGTH findings n)
  list(JOIN findings "\n  " text) # indented lines are not re-wrapped
  message(FATAL_ERROR "patch test-keys lint: ${n} finding(s) (only the patch tests and fuzz targets may set "
                      "TrustOptions::allowTestKeys):\n  ${text}")
endif()
if(allowed EQUAL 0)
  message(FATAL_ERROR "patch test-keys lint: no file declares or uses allowTestKeys where it may "
                      "(engine/patch/include/helios/patch/trust.h); the scan is broken")
endif()
message(STATUS "patch test-keys lint: allowTestKeys appears only in its ${allowed} allowed file(s)")
