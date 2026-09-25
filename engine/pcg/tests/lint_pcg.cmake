# pcg lints (CTest label `lint`):
#   MODE=kernels DIR=engine/pcg/src/kernels  — only integer ops in the VM kernel TUs (02 §5.8): no
#                                             `float`, `double` or `long double` tokens.
#   MODE=twin    DIR=shaders/pcg             — the hnoise Slang twin is 32-bit only (03 §5.5a): no 64-bit
#                                             integer types, no floating-point types.
# Comments are stripped first, so documentation may name the forbidden types.
#   MODE=spirv   SPV=<hnoise_tile.spv> DIS=<spirv-dis> — the compiled twin declares no Int64/Float64
#                                             capability and no 64-bit integer or float type.
cmake_minimum_required(VERSION 3.28)

if(MODE STREQUAL "spirv")
  if(NOT EXISTS "${SPV}")
    message(FATAL_ERROR "lint_pcg: SPIR-V module '${SPV}' not built")
  endif()
  execute_process(COMMAND "${DIS}" "${SPV}" OUTPUT_VARIABLE dis RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lint_pcg: ${DIS} failed: ${err}")
  endif()
  set(violations "")
  foreach(pattern "OpCapability Int64" "OpCapability Float64" "OpCapability Int16" "OpCapability Float16"
                  "OpTypeInt 64" "OpTypeFloat")
    if(dis MATCHES "${pattern}")
      list(APPEND violations "${pattern}")
    endif()
  endforeach()
  if(violations)
    string(REPLACE ";" ", " text "${violations}")
    message(FATAL_ERROR "pcg lint (spirv): ${SPV} uses ${text}; the hnoise twin must be 32-bit integer only (03 §5.5a)")
  endif()
  message(STATUS "pcg lint (spirv): ${SPV} is 32-bit integer only")
  return()
endif()

if(NOT DIR OR NOT IS_DIRECTORY "${DIR}")
  message(FATAL_ERROR "lint_pcg: DIR '${DIR}' is not a directory")
endif()
if(MODE STREQUAL "kernels")
  file(GLOB files "${DIR}/*.cpp" "${DIR}/*.h" "${DIR}/*.inl")
  set(forbidden "float" "double" "_Float16" "__m256d" "__m256" "__m128d" "__m128")
elseif(MODE STREQUAL "twin")
  file(GLOB files "${DIR}/*.slang")
  set(forbidden "int64_t" "uint64_t" "int64" "uint64" "double" "float" "float2" "float3" "float4" "half"
                "double2" "double3" "double4" "int64_t2" "uint64_t2")
else()
  message(FATAL_ERROR "lint_pcg: MODE must be kernels or twin")
endif()
if(NOT files)
  message(FATAL_ERROR "lint_pcg: no files to check in ${DIR}")
endif()

set(violations "")
foreach(f IN LISTS files)
  file(READ "${f}" text)
  # Protect list separators, then strip /* */ and // comments.
  string(REPLACE ";" "<SEMI>" text "${text}")
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " text "${text}")
  string(REGEX REPLACE "//[^\n]*" "" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(lineNo 0)
  foreach(line IN LISTS lines)
    math(EXPR lineNo "${lineNo} + 1")
    foreach(word IN LISTS forbidden)
      if(line MATCHES "(^|[^A-Za-z0-9_])${word}([^A-Za-z0-9_]|$)")
        string(STRIP "${line}" shown)
        list(APPEND violations "${f}:${lineNo}: '${word}' is not allowed here: ${shown}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(violations)
  string(REPLACE ";" "\n  " text "${violations}")
  message(FATAL_ERROR "pcg lint (${MODE}) failed:\n  ${text}\n")
endif()
list(LENGTH files n)
message(STATUS "pcg lint (${MODE}): ${n} files clean")
