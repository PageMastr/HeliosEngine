# rendertest.goldens (label rendertest; no GPU): every hardware golden set, golden/vulkan-<driver>/ other
# than the lavapipe set CI renders, carries the provenance import_goldens.cmake records: a
# PROVENANCE.json whose driverKey names the directory and which lists each PNG with its SHA-256, the
# run it came from, the adapter, the validation layer that checked it and the reason it differs from
# lavapipe. So a hardware golden cannot be added or replaced without the record a reviewer reads, and
# none exists for a scene that no longer does.
# Inputs: -DGOLDEN_DIR=<golden root> -DSCENES=<a,b,...> [-DSELF_TEST=<scratch dir>]
#   SELF_TEST also checks this script and import_goldens.cmake on synthetic inputs first: a valid
#   hardware result imports and passes; a changed or unrecorded PNG fails; results that were not
#   validated, ran on a software adapter or rendered differently twice do not import.

cmake_minimum_required(VERSION 3.28)  # policies for -P (IN_LIST, string(JSON))
if(NOT GOLDEN_DIR OR NOT SCENES)
  message(FATAL_ERROR "usage: cmake -DGOLDEN_DIR=<dir> -DSCENES=<a,b> [-DSELF_TEST=<dir>] -P golden_checks.cmake")
endif()
string(REPLACE "," ";" known_scenes "${SCENES}")
set(software "llvmpipe|lavapipe|swiftshader|softpipe|software|basic render")

function(check_golden_root root)
  set(problems "")
  file(GLOB sets LIST_DIRECTORIES true RELATIVE "${root}" "${root}/vulkan-*")
  foreach(set IN LISTS sets)
    if(NOT IS_DIRECTORY "${root}/${set}" OR set STREQUAL "vulkan-llvmpipe")
      continue()
    endif()
    set(dir "${root}/${set}")
    if(NOT EXISTS "${dir}/PROVENANCE.json")
      list(APPEND problems "${set}: no PROVENANCE.json (import hardware goldens with tools/rendertest/import_goldens.cmake)")
      continue()
    endif()
    file(READ "${dir}/PROVENANCE.json" json)
    string(JSON key ERROR_VARIABLE bad GET "${json}" driverKey)
    if(bad OR NOT key STREQUAL set)
      list(APPEND problems "${set}: PROVENANCE.json names driverKey '${key}'")
    endif()
    string(JSON count ERROR_VARIABLE bad LENGTH "${json}" scenes)
    if(bad)
      list(APPEND problems "${set}: PROVENANCE.json has no scenes object")
      continue()
    endif()
    set(recorded "")
    if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(i RANGE ${last})
        string(JSON scene MEMBER "${json}" scenes ${i})
        list(APPEND recorded "${scene}")
        if(NOT scene IN_LIST known_scenes)
          list(APPEND problems "${set}: '${scene}' is not a rendertest scene")
        endif()
        if(NOT EXISTS "${dir}/${scene}.png")
          list(APPEND problems "${set}: PROVENANCE.json lists ${scene}.png, which does not exist")
        endif()
        foreach(field sha256 source adapter validationLayer reason)
          string(JSON value ERROR_VARIABLE bad GET "${json}" scenes ${scene} ${field})
          if(bad OR value STREQUAL "")
            list(APPEND problems "${set}/${scene}: no ${field} in PROVENANCE.json")
          endif()
          set(${field} "${value}")
        endforeach()
        string(TOLOWER "${adapter}" lower_adapter)
        if(lower_adapter MATCHES "(${software})")
          list(APPEND problems "${set}/${scene}: recorded from a software adapter ('${adapter}')")
        endif()
        if(EXISTS "${dir}/${scene}.png")
          file(SHA256 "${dir}/${scene}.png" actual)
          if(NOT actual STREQUAL sha256)
            list(APPEND problems "${set}/${scene}.png: SHA-256 ${actual} differs from the recorded ${sha256} "
                                 "(re-import it, with its run, instead of replacing the file)")
          endif()
        endif()
      endforeach()
    endif()
    file(GLOB pngs RELATIVE "${dir}" "${dir}/*.png")
    foreach(png IN LISTS pngs)
      get_filename_component(scene "${png}" NAME_WE)
      if(NOT scene IN_LIST recorded)
        list(APPEND problems "${set}/${png}: not recorded in PROVENANCE.json")
      endif()
    endforeach()
  endforeach()
  if(problems)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "hardware goldens without their provenance:\n  ${text}")
  endif()
endfunction()

if(SELF_TEST)
  file(REMOVE_RECURSE "${SELF_TEST}")
  set(golden "${SELF_TEST}/golden")
  set(results "${SELF_TEST}/results")
  file(WRITE "${golden}/vulkan-llvmpipe/triangle.png" "lavapipe image")
  file(WRITE "${results}/vulkan/triangle.png" "hardware image")
  set(result [=[{
  "scene": "triangle", "backend": "vulkan", "status": "fail",
  "message": "ꟻLIP mean 0.02000 (limit 0.01) max 0.6000 (limit 0.5)",
  "adapter": "Example GPU (Example 1.0)", "goldenKey": "vulkan-llvmpipe", "driverKey": "vulkan-example",
  "validationLayer": "VK_LAYER_KHRONOS_validation 1.4.300 (implementation 1); in the call chain as \"Khronos Validation Layer\" 300",
  "actualFile": "vulkan/triangle.png", "goldenFile": "", "flipFile": "",
  "meanFlip": 0.02, "maxFlip": 0.6, "maxMeanFlip": 0.01, "maxAllowedFlip": 0.5,
  "renderedTwiceIdentical": true, "validation": true, "differentPixels": 10, "milliseconds": 1
}
]=])
  function(run_import expect what)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DRESULTS=${results}" -DSCENES=triangle "-DGOLDEN_DIR=${golden}"
                            -DSOURCE=self-test "-DREASON=self-test" -P "${CMAKE_CURRENT_LIST_DIR}/../import_goldens.cmake"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if((expect STREQUAL "pass") AND NOT rc EQUAL 0)
      message(FATAL_ERROR "self-test: importing ${what} failed:\n${out}\n${err}")
    elseif((expect STREQUAL "fail") AND rc EQUAL 0)
      message(FATAL_ERROR "self-test: importing ${what} must fail, got:\n${out}\n${err}")
    endif()
  endfunction()
  function(run_check expect what)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DGOLDEN_DIR=${golden}" -DSCENES=triangle
                            -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if((expect STREQUAL "pass") AND NOT rc EQUAL 0)
      message(FATAL_ERROR "self-test: the check rejected ${what}:\n${out}\n${err}")
    elseif((expect STREQUAL "fail") AND rc EQUAL 0)
      message(FATAL_ERROR "self-test: the check accepted ${what}")
    endif()
  endfunction()
  foreach(bad "\"validation\": true|\"validation\": false|a result the layer did not validate"
              "Example GPU|llvmpipe (LLVM 19.1.7, 256 bits)|a result from a software adapter"
              "\"renderedTwiceIdentical\": true|\"renderedTwiceIdentical\": false|a result whose two renders differ"
              "\"driverKey\": \"vulkan-example\"|\"driverKey\": \"vulkan-llvmpipe\"|a result filed under the lavapipe set")
    string(REPLACE "|" ";" parts "${bad}")
    list(GET parts 0 from)
    list(GET parts 1 to)
    list(GET parts 2 what)
    string(REPLACE "${from}" "${to}" broken "${result}")
    file(WRITE "${results}/vulkan/triangle.json" "${broken}")
    run_import(fail "${what}")
  endforeach()
  file(WRITE "${results}/vulkan/triangle.json" "${result}")
  run_import(pass "a validated hardware result")
  run_check(pass "an imported hardware golden")
  file(APPEND "${golden}/vulkan-example/triangle.png" "changed")
  run_check(fail "a hardware golden changed after its import")
  run_import(pass "the same result again")
  run_check(pass "a re-imported hardware golden")
  file(WRITE "${golden}/vulkan-example/mips.png" "unrecorded")
  run_check(fail "a hardware golden that PROVENANCE.json does not list")
  file(REMOVE "${golden}/vulkan-example/mips.png" "${golden}/vulkan-example/PROVENANCE.json")
  run_check(fail "a hardware golden set without PROVENANCE.json")
  message(STATUS "self-test passed")
endif()

check_golden_root("${GOLDEN_DIR}")
message(STATUS "hardware goldens carry their provenance")
