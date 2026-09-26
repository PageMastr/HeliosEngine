# Callgrind instruction counts of the RT-01 9k-op burst: the machine-independent twin of ADR-004a's
# indicator M1 (World burst / raw-flecs burst). Runs one single-threaded zone under callgrind and
# collects only the timed burst phases (bench::timed::*): 6 warm World bursts and the 6 raw-flecs
# bursts with the same apply/revert mix. The cold burst and everything else are not collected. Prints
# the instructions per burst and phase, and the World / raw ratios.
#
#   cmake -DECS_BENCH=<path/to/ecs_bench> [-DOUT_DIR=<dir>] [-DVALGRIND=<valgrind>]
#         [-DEXTRA_ARGS="--inframe-dontfragment"] -P engine/ecs/bench/callgrind_burst.cmake
#
# Use a Release build (the linux-bench preset): asserts off, as for M1. Linux only (valgrind).
cmake_minimum_required(VERSION 3.28)

if(NOT ECS_BENCH OR NOT EXISTS "${ECS_BENCH}")
  message(FATAL_ERROR "callgrind_burst: pass -DECS_BENCH=<path to the ecs_bench executable>")
endif()
if(NOT VALGRIND)
  find_program(VALGRIND valgrind REQUIRED)
endif()
get_filename_component(_valgrind_dir "${VALGRIND}" DIRECTORY)
find_program(CALLGRIND_ANNOTATE callgrind_annotate HINTS "${_valgrind_dir}" REQUIRED)
if(NOT OUT_DIR)
  get_filename_component(OUT_DIR "${ECS_BENCH}" DIRECTORY)
endif()
file(MAKE_DIRECTORY "${OUT_DIR}")
set(_out "${OUT_DIR}/callgrind.ecs_burst.out")
separate_arguments(_extra UNIX_COMMAND "${EXTRA_ARGS}")

execute_process(
  COMMAND "${VALGRIND}" --tool=callgrind "--callgrind-out-file=${_out}" "--toggle-collect=bench::timed::*"
          "${ECS_BENCH}" --workers=0 --ticks=10 --warmup=2 --no-spikes --no-warmup-pass ${_extra}
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _bench_out ERROR_VARIABLE _valgrind_err)
# ecs_bench exits 1 while RT-01 fails; only a missing profile is an error here.
if(NOT EXISTS "${_out}")
  message(FATAL_ERROR "callgrind_burst: valgrind failed (${_rc}):\n${_valgrind_err}")
endif()
string(REGEX MATCH "state hash: [0-9a-f]+" _hash "${_bench_out}")

execute_process(COMMAND "${CALLGRIND_ANNOTATE}" --inclusive=yes --threshold=100 "${_out}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _annotated ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "callgrind_burst: callgrind_annotate failed (${_rc}):\n${_err}")
endif()

set(_phases worldCreates worldDestroys worldToggles worldStatuses rawCreates rawDestroys rawToggles rawStatuses)
foreach(_p IN LISTS _phases)
  set(_ir_${_p} 0)
endforeach()
string(REPLACE "\n" ";" _lines "${_annotated}")
foreach(_line IN LISTS _lines)
  if(_line MATCHES "^ *([0-9,]+) .*bench::timed::([A-Za-z]+)\\(")
    string(REPLACE "," "" _ir "${CMAKE_MATCH_1}")
    set(_p "${CMAKE_MATCH_2}")
    if(DEFINED _ir_${_p})
      math(EXPR _ir_${_p} "${_ir_${_p}} + ${_ir}")
    endif()
  endif()
endforeach()
if(_ir_worldCreates EQUAL 0 OR _ir_rawCreates EQUAL 0)
  message(FATAL_ERROR "callgrind_burst: no bench::timed::* costs in ${_out}; is ecs_bench current?")
endif()

# ecs_bench collects World warm rounds 2..7 and the six raw bursts with the same parities
# (round 1 still creates a few tables; the rounds alternate between applying and reverting).
set(_bursts 6)
foreach(_p IN LISTS _phases)
  math(EXPR _b_${_p} "${_ir_${_p}} / ${_bursts}")
endforeach()
math(EXPR _world_tag "${_b_worldCreates} + ${_b_worldDestroys} + ${_b_worldToggles}")
math(EXPR _world_df "${_b_worldCreates} + ${_b_worldDestroys} + ${_b_worldStatuses}")
math(EXPR _raw_tag "${_b_rawCreates} + ${_b_rawDestroys} + ${_b_rawToggles}")
math(EXPR _raw_df "${_b_rawCreates} + ${_b_rawDestroys} + ${_b_rawStatuses}")

function(_ratio out world raw)
  math(EXPR _r "(${world} * 100 + ${raw} / 2) / ${raw}")
  math(EXPR _i "${_r} / 100")
  math(EXPR _f "${_r} % 100")
  if(_f LESS 10)
    set(_f "0${_f}")
  endif()
  set(${out} "${_i}.${_f}x" PARENT_SCOPE)
endfunction()
function(_row label world raw)
  _ratio(_r ${world} ${raw})
  math(EXPR _per_w "${world} / 3000")
  math(EXPR _per_r "${raw} / 3000")
  string(LENGTH "${label}" _len)
  math(EXPR _pad "30 - ${_len}")
  string(REPEAT " " ${_pad} _sp)
  message("  ${label}${_sp}${world} (${_per_w}/op)   ${raw} (${_per_r}/op)   ${_r}")
endfunction()

message("ecs_bench burst under callgrind (${ECS_BENCH}; ${_hash})")
message("instructions per warm burst (mean of ${_bursts}): World   raw flecs   ratio")
_row("3k creates" ${_b_worldCreates} ${_b_rawCreates})
_row("3k destroys" ${_b_worldDestroys} ${_b_rawDestroys})
_row("3k tag toggles" ${_b_worldToggles} ${_b_rawToggles})
_row("3k DontFragment toggles" ${_b_worldStatuses} ${_b_rawStatuses})
_ratio(_m1_tag ${_world_tag} ${_raw_tag})
_ratio(_m1_df ${_world_df} ${_raw_df})
message("  9k ops, tag toggles           ${_world_tag}   ${_raw_tag}   ${_m1_tag}")
message("  9k ops, DontFragment toggles  ${_world_df}   ${_raw_df}   ${_m1_df}")
message("profile: ${_out}")
