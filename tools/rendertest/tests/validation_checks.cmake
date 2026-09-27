# A required Khronos validation layer that does not load fails helios-rendertest, and
# HELIOS_SKIP_GPU_TESTS=1 never turns that into a skip (RC-1's goldens must not pass unvalidated):
#   1. the loader hides the layer (VK_LOADER_LAYERS_DISABLE), also where no adapter matches
#      (HELIOS_RHI_ADAPTER), so a probe without validation would find no device either: a layer error
#      is never read as "no Vulkan device";
#   2. the layer is listed but its library is missing (a broken manifest on VK_LAYER_PATH), with
#      HELIOS_RHI_VALIDATION=1 as well, which once made the no-validation probe load it too;
#   3. with the real layer present (LAYER set), HELIOS_RHI_VALIDATION=1 enables it on an ordinary
#      device but not on the probe device.
# Cases 1 and 2 work whether or not the layer is installed. On a machine without any Vulkan device
# the check reports SKIPPED.
# Inputs: -DRENDERTEST=<exe> -DOUT=<scratch dir> [-DLAYER=<layer manifest found at configure time>]

if(NOT RENDERTEST OR NOT OUT)
  message(FATAL_ERROR "usage: cmake -DRENDERTEST=<exe> -DOUT=<dir> [-DLAYER=<manifest>] -P validation_checks.cmake")
endif()
file(REMOVE_RECURSE "${OUT}")
set(ENV{HELIOS_SKIP_GPU_TESTS} 1)
unset(ENV{HELIOS_RHI_VALIDATION})

# Is there a Vulkan device at all? (Without validation, so hidden or broken layers do not matter.)
set(ENV{VK_LOADER_LAYERS_DISABLE} VK_LAYER_KHRONOS_validation)
execute_process(COMMAND "${RENDERTEST}" --backend vulkan --scene triangle --out "${OUT}/probe"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(out MATCHES "SKIP vulkan")
  message(STATUS "SKIPPED: no Vulkan device (${out})")
  return()
endif()

function(expect_layer_failure what)
  foreach(args "--backend;vulkan;--scene;triangle;--require-validation" "--validation-self-test")
    execute_process(COMMAND "${RENDERTEST}" ${args} --out "${OUT}/required"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(rc EQUAL 0 OR out MATCHES "SKIP" OR NOT err MATCHES "HELIOS_SKIP_GPU_TESTS does not skip that")
      message(FATAL_ERROR "helios-rendertest ${args} with ${what}: expected a failure that is not a skip, "
                          "got ${rc}:\n${out}\n${err}")
    endif()
  endforeach()
endfunction()

# 1. Hidden layer, with and without a usable adapter.
expect_layer_failure("the layer hidden by the loader")
set(ENV{HELIOS_RHI_ADAPTER} "no-such-adapter-for-rendertest-validation-checks")
expect_layer_failure("the layer hidden and no matching adapter")
unset(ENV{HELIOS_RHI_ADAPTER})
unset(ENV{VK_LOADER_LAYERS_DISABLE})

# 2. A listed layer whose library does not exist; VK_LAYER_PATH replaces the search path, so the real
#    layer is not found either.
set(broken "${OUT}/broken-layer")
file(WRITE "${broken}/VkLayer_khronos_validation.json" [=[{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_KHRONOS_validation",
        "type": "GLOBAL",
        "library_path": "./libVkLayer_helios_rendertest_missing.so",
        "api_version": "1.3.275",
        "implementation_version": "1",
        "description": "broken manifest for rendertest.validation-required"
    }
}
]=])
set(saved_add "$ENV{VK_ADD_LAYER_PATH}")
unset(ENV{VK_ADD_LAYER_PATH})
set(ENV{VK_LAYER_PATH} "${broken}")
set(ENV{HELIOS_RHI_VALIDATION} 1)
expect_layer_failure("a broken layer manifest and HELIOS_RHI_VALIDATION=1")
unset(ENV{VK_LAYER_PATH})
if(saved_add)
  set(ENV{VK_ADD_LAYER_PATH} "${saved_add}")
endif()

# 3. The probe ignores HELIOS_RHI_VALIDATION, which does enable the layer on an ordinary device.
if(LAYER)
  execute_process(COMMAND "${RENDERTEST}" --print-probe RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "default device: Khronos validation active"
     OR NOT out MATCHES "probe device: Khronos validation inactive")
    message(FATAL_ERROR "with HELIOS_RHI_VALIDATION=1 the default device must validate and the probe must not, "
                        "got ${rc}:\n${out}\n${err}")
  endif()
endif()
unset(ENV{HELIOS_RHI_VALIDATION})
message(STATUS "a required validation layer that does not load fails, and is not skipped")
