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

# Reads <RESULTS>/vulkan/<scene>.json into r_<field> in the caller and sets r_problem to why the
# scene cannot be imported (empty when it can).
function(read_result scene)
  set(result_file "${RESULTS}/vulkan/${scene}.json")
  if(NOT EXISTS "${result_file}")
    set(r_problem "no result at ${result_file}" PARENT_SCOPE)
    return()
  endif()
  file(READ "${result_file}" json)
  foreach(key backend status message adapter goldenKey driverKey validationLayer actualFile)
    string(JSON value ERROR_VARIABLE missing GET "${json}" ${key})
    if(missing)
      set(value "")
    endif()
    set(r_${key} "${value}")
    set(r_${key} "${value}" PARENT_SCOPE)
  endforeach()
  foreach(key validation renderedTwiceIdentical meanFlip maxFlip)
    string(JSON value ERROR_VARIABLE missing GET "${json}" ${key})
    if(missing)
      set(value "")
    endif()
    set(r_${key} "${value}")
    set(r_${key} "${value}" PARENT_SCOPE)
  endforeach()
  string(TOLOWER "${r_adapter}" lower_adapter)
  set(problem "")
  if(NOT r_backend STREQUAL "vulkan")
    set(problem "backend is '${r_backend}', not vulkan")
  elseif(NOT r_validation)
    set(problem "the render was not validated by the Khronos layer")
  elseif(NOT r_renderedTwiceIdentical)
    set(problem "the two renders differed")
  elseif(NOT r_actualFile OR NOT EXISTS "${RESULTS}/${r_actualFile}")
    set(problem "no rendered image (${r_status}: ${r_message})")
  elseif(NOT r_status STREQUAL "pass" AND NOT r_message MATCHES "^ꟻLIP mean")
    set(problem "the scene failed before its comparison: ${r_message}")
  elseif(r_meanFlip STREQUAL "" OR r_maxFlip STREQUAL "")
    set(problem "the result has no ꟻLIP scores against ${r_goldenKey}")
  elseif(NOT r_driverKey MATCHES "^vulkan-[a-z0-9-]+$" OR r_driverKey STREQUAL "vulkan-llvmpipe")
    set(problem "driver key '${r_driverKey}' is not a hardware golden set")
  elseif(NOT r_adapter OR lower_adapter MATCHES "(${software})")
    set(problem "adapter '${r_adapter}' is not a hardware GPU")
  elseif(NOT EXISTS "${GOLDEN_DIR}/vulkan-llvmpipe/${scene}.png")
    set(problem "no lavapipe golden ${GOLDEN_DIR}/vulkan-llvmpipe/${scene}.png")
  endif()
  set(r_problem "${problem}" PARENT_SCOPE)
endfunction()

# Check every scene before changing anything.
foreach(scene IN LISTS scenes)
  read_result(${scene})
  if(r_problem)
    message(FATAL_ERROR "${scene}: cannot import: ${r_problem}")
  endif()
endforeach()

# All checked: copy and record. `record` is the JSON object being filled.
function(set_text field value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(JSON record SET "${record}" ${field} "\"${escaped}\"")
  set(record "${record}" PARENT_SCOPE)
endfunction()
foreach(scene IN LISTS scenes)
  read_result(${scene})
  set(dir "${GOLDEN_DIR}/${r_driverKey}")
  set(png "${RESULTS}/${r_actualFile}")
  file(MAKE_DIRECTORY "${dir}")
  file(COPY_FILE "${png}" "${dir}/${scene}.png")
  file(SHA256 "${dir}/${scene}.png" sha)
  set(provenance_file "${dir}/PROVENANCE.json")
  if(EXISTS "${provenance_file}")
    file(READ "${provenance_file}" provenance)
  else()
    set(provenance "{\"driverKey\": \"${r_driverKey}\", \"scenes\": {}}")
  endif()
  set(record "{}")
  set_text(sha256 "${sha}")
  set_text(source "${SOURCE}")
  set_text(adapter "${r_adapter}")
  set_text(validationLayer "${r_validationLayer}")
  set_text(comparedWith "${r_goldenKey}")
  set_text(reason "${REASON}")
  string(JSON record SET "${record}" meanFlip "${r_meanFlip}")
  string(JSON record SET "${record}" maxFlip "${r_maxFlip}")
  string(JSON provenance SET "${provenance}" scenes ${scene} "${record}")
  file(WRITE "${provenance_file}" "${provenance}\n")
  message(STATUS "${scene}: ${dir}/${scene}.png (${r_adapter}; ꟻLIP against ${r_goldenKey}: mean ${r_meanFlip}, "
                 "max ${r_maxFlip})")
endforeach()
message(STATUS "Review the images and their ꟻLIP maps (<scene>.flip.png next to each result) in the pull request.")
