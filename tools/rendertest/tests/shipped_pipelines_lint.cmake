# RC-1 coverage lint (03 §9.3; engine/render/include/helios/render/shader_library.h). rendertest.coverage
# sees a pipeline only when it was created through a record: createShippedPipeline() for engine/render's
# features, createLocalPipeline() for helios-rendertest's own test shaders. A pipeline created directly
# on the device, for example a state variant of already-covered shaders, would ship without a golden and
# pass unnoticed. So under engine/, apps/samples/ and tools/rendertest/, every direct
# `.createGraphicsPipeline(` / `.createComputePipeline(` call (also through `->`) fails, except in
# engine/render/src/shader_library.cpp, which implements the record.
#
# A call that must stay direct carries a waiver on its own line or the line above:
#   // shipped-pipelines-lint: allow <reason, at least 12 characters>
# Waived calls are listed in the output. tools/rendertest/tests/ (these fixtures) is not scanned.
#
#   cmake -DSOURCE_DIR=<repository root> -P shipped_pipelines_lint.cmake

cmake_minimum_required(VERSION 3.28)
if(NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "shipped_pipelines_lint: SOURCE_DIR is not a directory: '${SOURCE_DIR}'")
endif()
get_filename_component(SOURCE_DIR "${SOURCE_DIR}" ABSOLUTE)

set(files "")
foreach(root engine/render apps/samples tools/rendertest)
  if(IS_DIRECTORY "${SOURCE_DIR}/${root}")
    file(GLOB_RECURSE found LIST_DIRECTORIES FALSE
         "${SOURCE_DIR}/${root}/*.cpp" "${SOURCE_DIR}/${root}/*.cc" "${SOURCE_DIR}/${root}/*.cxx"
         "${SOURCE_DIR}/${root}/*.h" "${SOURCE_DIR}/${root}/*.hpp" "${SOURCE_DIR}/${root}/*.inl")
    list(APPEND files ${found})
  endif()
endforeach()
list(SORT files)

set(call "(\\.|->)[ \t]*create(Graphics|Compute)Pipeline[ \t]*\\(")
set(waiver "shipped-pipelines-lint:[ \t]*allow")
set(findings "")
set(waived "")
set(scanned 0)
foreach(f IN LISTS files)
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${f}")
  if(rel MATCHES "^tools/rendertest/tests/" OR rel STREQUAL "engine/render/src/shader_library.cpp")
    continue()
  endif()
  math(EXPR scanned "${scanned} + 1")
  file(READ "${f}" text)
  # One list element per line: characters with a meaning in CMake lists cannot split or join lines.
  string(REPLACE "\\" "/" text "${text}")
  string(REPLACE ";" "," text "${text}")
  string(REPLACE "[" "(" text "${text}")
  string(REPLACE "]" ")" text "${text}")
  string(REPLACE "\r" "" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(number 0)
  set(previous "")
  foreach(line IN LISTS lines)
    math(EXPR number "${number} + 1")
    if(line MATCHES "${call}")
      set(reason "")
      foreach(candidate "${line}" "${previous}")
        if(reason STREQUAL "" AND candidate MATCHES "${waiver}(.*)$")
          string(STRIP "${CMAKE_MATCH_1}" reason)
          if(reason STREQUAL "")
            set(reason "<none>")
          endif()
        endif()
      endforeach()
      string(STRIP "${line}" shown)
      if(reason STREQUAL "")
        list(APPEND findings "${rel}:${number}: direct pipeline creation (use createShippedPipeline or createLocalPipeline): ${shown}")
      else()
        string(LENGTH "${reason}" reasonLength)
        if(reason STREQUAL "<none>" OR reasonLength LESS 12)
          list(APPEND findings "${rel}:${number}: the shipped-pipelines-lint waiver needs a reason: ${shown}")
        else()
          list(APPEND waived "${rel}:${number}: ${reason}")
        endif()
      endif()
    endif()
    set(previous "${line}")
  endforeach()
endforeach()

foreach(w IN LISTS waived)
  message(STATUS "waived: ${w}")
endforeach()
if(findings)
  list(LENGTH findings count)
  string(REPLACE ";" "\n  " text "${findings}")
  message(FATAL_ERROR "shipped-pipelines lint: ${count} finding(s) (RC-1 coverage can only see recorded pipelines):\n  ${text}")
endif()
message(STATUS "shipped-pipelines lint passed: ${scanned} files")
