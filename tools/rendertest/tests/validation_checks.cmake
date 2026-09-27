# A required Khronos validation layer that does not load fails helios-rendertest, and
# HELIOS_SKIP_GPU_TESTS=1 never turns that into a skip (RC-1's goldens must not pass unvalidated).
# The loader hides the layer here (VK_LOADER_LAYERS_DISABLE), so the check works whether or not the
# layer is installed. On a machine without any Vulkan device the check itself reports SKIPPED.
# Inputs: -DRENDERTEST=<exe> -DOUT=<scratch dir>

if(NOT RENDERTEST OR NOT OUT)
  message(FATAL_ERROR "usage: cmake -DRENDERTEST=<exe> -DOUT=<dir> -P validation_checks.cmake")
endif()
file(REMOVE_RECURSE "${OUT}")
set(ENV{HELIOS_SKIP_GPU_TESTS} 1)
set(ENV{VK_LOADER_LAYERS_DISABLE} VK_LAYER_KHRONOS_validation)

# Is there a Vulkan device at all? (Without validation, so the hidden layer does not matter.)
execute_process(COMMAND "${RENDERTEST}" --backend vulkan --scene triangle --out "${OUT}/probe"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(out MATCHES "SKIP vulkan")
  message(STATUS "SKIPPED: no Vulkan device (${out})")
  return()
endif()

foreach(args "--backend;vulkan;--scene;triangle;--require-validation" "--validation-self-test")
  execute_process(COMMAND "${RENDERTEST}" ${args} --out "${OUT}/required"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR out MATCHES "SKIP" OR NOT err MATCHES "HELIOS_SKIP_GPU_TESTS does not skip that")
    message(FATAL_ERROR "helios-rendertest ${args} with the layer hidden: expected a failure that is not a skip, "
                        "got ${rc}:\n${out}\n${err}")
  endif()
endforeach()
message(STATUS "a required validation layer that does not load fails, and is not skipped")
