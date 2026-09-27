# RC-1 coverage lint (03 §9.3; engine/render/include/helios/render/shader_library.h). rendertest.coverage
# sees a pipeline only when it was created through a record: createShippedPipeline() for engine/render's
# features, createLocalPipeline() for helios-rendertest's own test shaders. A pipeline created directly
# on the device, for example a state variant of already-covered shaders under a new name, would ship
# without a golden and pass unnoticed. So in engine/, apps/ and tools/, every identifier of the form
# create<X>Pipeline other than those two fails: called with `.` or `->`, split across lines (also by
# a backslash-newline splice inside the identifier; the finding names the splice's first line), or
# named in a member-function pointer or std::invoke. Not scanned, each for a reason:
#   engine/rhi/                           implements and tests the Device API itself
#   tools/prebuilt/                       downloaded third-party SDKs (gitignored)
#   tools/rendertest/tests/               this lint's fixtures
#   tools/lint/tests/                     the other lints' fixtures
#   engine/render/src/shader_library.cpp  implements the record
# A use that must stay carries a waiver on its own line, or alone on the line above:
#   // shipped-pipelines-lint: allow <reason, at least 12 characters>
# Waived uses are listed in the output.
# Threat model: this is a best-effort textual check against accidental direct pipeline creation. It is
# not a guarantee against deliberately adversarial source; code review and rendertest.coverage are the
# backstops. Limits: an identifier built by token pasting; a creation path (a wrapper or a member
# pointer) defined outside the scanned files, which includes engine/rhi/; source in a file whose
# extension the scan does not take (it takes .cpp .cc .cxx .cppm .ixx .h .hh .hpp .hxx .inl .inc .ipp).
#
#   cmake -DSOURCE_DIR=<repository root> -P shipped_pipelines_lint.cmake

cmake_minimum_required(VERSION 3.28)
if(NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "shipped_pipelines_lint: SOURCE_DIR is not a directory: '${SOURCE_DIR}'")
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

set(ident "[A-Za-z0-9_]*create[A-Za-z0-9_]*Pipeline[A-Za-z0-9_]*")  # whole identifiers containing it
set(waiver "shipped-pipelines-lint:[ \t]*allow")
string(ASCII 1 splice)  # stands for a removed backslash-newline (no source character is U+0001)
set(findings "")
set(waived "")
set(scanned 0)
foreach(f IN LISTS files)
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${f}")
  if(rel MATCHES "^(engine/rhi|tools/prebuilt|tools/rendertest/tests|tools/lint/tests)/" OR
     rel STREQUAL "engine/render/src/shader_library.cpp")
    continue()
  endif()
  math(EXPR scanned "${scanned} + 1")
  file(READ "${f}" text)
  # One list element per logical line: a backslash-newline splice (blanks before the newline too, as
  # GCC, Clang and C++23 accept) joins two lines as the compiler does, marked so that the physical
  # line numbers stay right; characters with a meaning in CMake lists cannot split or join lines.
  string(REPLACE "\r" "" text "${text}")
  string(REGEX REPLACE "\\\\[ \t]*\n" "${splice}" text "${text}")
  string(REPLACE "${splice}" "" joined "${text}")
  if(NOT joined MATCHES "create[A-Za-z0-9_]+Pipeline")
    continue()
  endif()
  string(REPLACE "\\" "/" text "${text}")
  string(REPLACE ";" "," text "${text}")
  string(REPLACE "[" "(" text "${text}")
  string(REPLACE "]" ")" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(next 1)
  set(previous "")
  foreach(spliced IN LISTS lines)
    set(number ${next})
    string(REGEX MATCHALL "${splice}" joins "${spliced}")
    list(LENGTH joins joinCount)
    math(EXPR next "${next} + 1 + ${joinCount}")
    string(REPLACE "${splice}" "" line "${spliced}")
    string(REGEX MATCHALL "${ident}" names "${line}")
    set(hit "")
    foreach(name IN LISTS names)
      if(name MATCHES "^create[A-Za-z0-9_]+Pipeline$" AND NOT name MATCHES "^create(Shipped|Local)Pipeline$")
        set(hit "${name}")
      endif()
    endforeach()
    if(hit)
      # The waiver sits on this line, or alone on the previous one (a waiver after code covers its line only).
      set(reason "")
      set(candidates "${line}")
      if(previous MATCHES "^[ \t]*//")
        list(APPEND candidates "${previous}")
      endif()
      foreach(candidate IN LISTS candidates)
        if(reason STREQUAL "" AND candidate MATCHES "${waiver}(.*)$")
          string(STRIP "${CMAKE_MATCH_1}" reason)
          if(reason STREQUAL "")
            set(reason "<none>")
          endif()
        endif()
      endforeach()
      string(STRIP "${line}" shown)
      if(reason STREQUAL "")
        list(APPEND findings
             "${rel}:${number}: ${hit} outside createShippedPipeline/createLocalPipeline: ${shown}")
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
