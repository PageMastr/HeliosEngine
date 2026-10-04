# AAA-SEC-4 over cooked sample records (01 §3.8; 02 §3.3, §6.5): cooks a project with helios-cook, then
# searches the bytes of both cooks for planted sentinels. Each sentinel must occur in the server cook
# (otherwise the search proves nothing) and nowhere in the client cook. The search is on raw bytes, so it
# does not trust the cooker's or the loader's view of the layout.
#
#   cmake -DCOOK=<helios-cook> -DPROJECT=<project root> -DWORK=<scratch dir> "-DSENTINELS=a;b"
#         [-DSWAP=ON] -P sec4_check.cmake
#
# SWAP=ON scans the server cook as if it were the client cook (the seeded violation: must fail).

cmake_minimum_required(VERSION 3.28)
foreach(v COOK PROJECT WORK SENTINELS)
  if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
    message(FATAL_ERROR "sec4_check: pass -D${v}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
execute_process(COMMAND "${COOK}" records "--project-root=${PROJECT}" "--out=${WORK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sec4_check: helios-cook failed (exit ${rc}):\n${out}${err}")
endif()
set(client "${WORK}/records.client.hrdb")
set(server "${WORK}/records.server.hrdb")
if(SWAP)
  set(client "${WORK}/records.server.hrdb")
endif()
file(READ "${client}" clientHex HEX)
file(READ "${server}" serverHex HEX)

# Byte offset of `needleHex` in `hex` at a byte boundary, or -1.
function(find_bytes hex needleHex out)
  set(pos 0)
  set(found -1)
  while(TRUE)
    string(SUBSTRING "${hex}" ${pos} -1 rest)
    string(FIND "${rest}" "${needleHex}" i)
    if(i EQUAL -1)
      break()
    endif()
    math(EXPR at "${pos} + ${i}")
    math(EXPR odd "${at} % 2")
    if(odd EQUAL 0)
      math(EXPR found "${at} / 2")
      break()
    endif()
    math(EXPR pos "${at} + 1")
  endwhile()
  set(${out} ${found} PARENT_SCOPE)
endfunction()

set(findings "")
foreach(s IN LISTS SENTINELS)
  string(HEX "${s}" h)
  find_bytes("${serverHex}" "${h}" inServer)
  if(inServer EQUAL -1)
    list(APPEND findings "sentinel '${s}' is not in the server cook, so its absence from the client cook proves nothing")
  endif()
  find_bytes("${clientHex}" "${h}" inClient)
  if(NOT inClient EQUAL -1)
    list(APPEND findings "AAA-SEC-4: server-only sentinel '${s}' is in the client cook at byte ${inClient}")
  endif()
endforeach()
if(findings)
  list(JOIN findings "\n  " text)
  message(FATAL_ERROR "sec4_check (${PROJECT}):\n  ${text}")
endif()
list(LENGTH SENTINELS n)
message(STATUS "AAA-SEC-4: ${n} server-only sentinel(s) present in the server cook and absent from the client cook (${PROJECT})")
