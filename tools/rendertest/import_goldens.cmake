# Imports hardware goldens (03 §8.4: goldens per backend and driver) from a helios-rendertest output
# directory, for a reviewed pull request; nothing commits them automatically. The win-gpu job uploads
# its output in the `results-win-gpu` artifact (rendertest/); download it, then:
#
#   cmake -DRESULTS=<artifact>/rendertest -DSCENES=bindless,mips
#         -DSOURCE=https://github.com/<owner>/<repo>/actions/runs/<id>
#         -DREASON="<why this driver's image legitimately differs from lavapipe's>"
#         -P tools/rendertest/import_goldens.cmake
#
# Each scene's result must come from a hardware adapter, validated by the Khronos layer, rendered
# twice bit-identically, and the scene must have a lavapipe golden. The PNG goes to
# golden/<driverKey>/<scene>.png (driverKey from the result, e.g. vulkan-nvidia) and its provenance
# (source run, adapter and driver, validation layer, ꟻLIP against the golden it was compared with,
# SHA-256, reason) into golden/<driverKey>/PROVENANCE.json, which rendertest.goldens checks against
# the PNGs: a hardware golden cannot land or change without its record. The pull request shows the
# ꟻLIP map (<scene>.flip.png in the same output) for review (09 §5.3 item 7). Thresholds stay as
# they are; a hardware golden only replaces the lavapipe image for that driver.
# Optional: -DGOLDEN_DIR=<golden root> (default: this directory's golden/).

cmake_minimum_required(VERSION 3.28)  # policies for -P (IN_LIST, string(JSON))
if(NOT RESULTS OR NOT SCENES OR NOT SOURCE OR NOT REASON)
  message(FATAL_ERROR "usage: cmake -DRESULTS=<rendertest output dir> -DSCENES=<a,b> -DSOURCE=<run URL> "
                      "-DREASON=<why> [-DGOLDEN_DIR=<dir>] -P import_goldens.cmake")
endif()
if(NOT GOLDEN_DIR)
  set(GOLDEN_DIR "${CMAKE_CURRENT_LIST_DIR}/golden")
endif()
set(software "llvmpipe|lavapipe|swiftshader|softpipe|software|basic render")
string(REPLACE "," ";" scenes "${SCENES}")

set(imports "")
foreach(scene IN LISTS scenes)
  set(result_file "${RESULTS}/vulkan/${scene}.json")
  if(NOT EXISTS "${result_file}")
    message(FATAL_ERROR "${scene}: no result at ${result_file}")
  endif()
  file(READ "${result_file}" json)
  foreach(key backend status message adapter goldenKey driverKey validationLayer actualFile)
    string(JSON ${key} ERROR_VARIABLE missing GET "${json}" ${key})
    if(missing)
      set(${key} "")
    endif()
  endforeach()
  foreach(key validation renderedTwiceIdentical meanFlip maxFlip)
    string(JSON ${key} GET "${json}" ${key})
  endforeach()
  string(TOLOWER "${adapter}" lower_adapter)
  set(problem "")
  if(NOT backend STREQUAL "vulkan")
    set(problem "backend is '${backend}', not vulkan")
  elseif(NOT validation)
    set(problem "the render was not validated by the Khronos layer")
  elseif(NOT renderedTwiceIdentical)
    set(problem "the two renders differed")
  elseif(NOT actualFile OR NOT EXISTS "${RESULTS}/${actualFile}")
    set(problem "no rendered image (${status}: ${message})")
  elseif(NOT status STREQUAL "pass" AND NOT message MATCHES "^ꟻLIP mean")
    set(problem "the scene failed before its comparison: ${message}")
  elseif(NOT driverKey MATCHES "^vulkan-[a-z0-9-]+$" OR driverKey STREQUAL "vulkan-llvmpipe")
    set(problem "driver key '${driverKey}' is not a hardware golden set")
  elseif(NOT adapter OR lower_adapter MATCHES "(${software})")
    set(problem "adapter '${adapter}' is not a hardware GPU")
  elseif(NOT EXISTS "${GOLDEN_DIR}/vulkan-llvmpipe/${scene}.png")
    set(problem "no lavapipe golden ${GOLDEN_DIR}/vulkan-llvmpipe/${scene}.png")
  endif()
  if(problem)
    message(FATAL_ERROR "${scene}: cannot import: ${problem}")
  endif()
  list(APPEND imports "${scene}|${driverKey}")
  set(info_${scene}_driverKey "${driverKey}")
  set(info_${scene}_png "${RESULTS}/${actualFile}")
  set(info_${scene}_adapter "${adapter}")
  set(info_${scene}_layer "${validationLayer}")
  set(info_${scene}_compared "${goldenKey}")
  set(info_${scene}_mean "${meanFlip}")
  set(info_${scene}_max "${maxFlip}")
endforeach()

# All checked: copy and record. `record` is the JSON object being filled.
function(set_text field value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(JSON record SET "${record}" ${field} "\"${escaped}\"")
  set(record "${record}" PARENT_SCOPE)
endfunction()
foreach(entry IN LISTS imports)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 scene)
  list(GET parts 1 key)
  set(dir "${GOLDEN_DIR}/${key}")
  file(MAKE_DIRECTORY "${dir}")
  file(COPY_FILE "${info_${scene}_png}" "${dir}/${scene}.png")
  file(SHA256 "${dir}/${scene}.png" sha)
  set(provenance_file "${dir}/PROVENANCE.json")
  if(EXISTS "${provenance_file}")
    file(READ "${provenance_file}" provenance)
  else()
    set(provenance "{\"driverKey\": \"${key}\", \"scenes\": {}}")
  endif()
  set(record "{}")
  set_text(sha256 "${sha}")
  set_text(source "${SOURCE}")
  set_text(adapter "${info_${scene}_adapter}")
  set_text(validationLayer "${info_${scene}_layer}")
  set_text(comparedWith "${info_${scene}_compared}")
  set_text(reason "${REASON}")
  string(JSON record SET "${record}" meanFlip "${info_${scene}_mean}")
  string(JSON record SET "${record}" maxFlip "${info_${scene}_max}")
  string(JSON provenance SET "${provenance}" scenes ${scene} "${record}")
  file(WRITE "${provenance_file}" "${provenance}\n")
  message(STATUS "${scene}: ${dir}/${scene}.png (${info_${scene}_adapter}; ꟻLIP against "
                 "${info_${scene}_compared}: mean ${info_${scene}_mean}, max ${info_${scene}_max})")
endforeach()
message(STATUS "Review the images and their ꟻLIP maps (<scene>.flip.png next to each result) in the pull request.")
