# A required Khronos validation layer that is not in the call chain fails helios-rendertest, and
# HELIOS_SKIP_GPU_TESTS=1 never turns that into a skip (RC-1's goldens must not pass unvalidated);
# the skip is only for the RHI's no-Vulkan errors (no loader, driver or adapter):
#   1. the loader hides the layer (VK_LOADER_LAYERS_DISABLE), also where no adapter matches
#      (HELIOS_RHI_ADAPTER), so a probe without validation would find no device either: a layer error
#      is never read as "no Vulkan device";
#   2. the layer is listed but its library is missing (a broken manifest on VK_LAYER_PATH), with
#      HELIOS_RHI_VALIDATION=1 as well, which once made the no-validation probe load it too;
#   3. with the real layer present (LAYER set), HELIOS_RHI_VALIDATION=1 enables it on an ordinary
#      device but not on the probe device;
#   4. a layer that loads but whose vkCreateInstance fails (fake layers built by ../CMakeLists.txt on
#      Linux: FAKE_LAYER_INITFAIL, FAKE_LAYER_NODRIVER): with VK_ERROR_INITIALIZATION_FAILED, also
#      under HELIOS_RHI_VALIDATION=1 and when the loader forces the layer on every instance, the probe
#      included (VK_INSTANCE_LAYERS, VK_LOADER_LAYERS_ENABLE); and with VK_ERROR_INCOMPATIBLE_DRIVER,
#      a no-Vulkan error that the probe device, which ignores HELIOS_RHI_VALIDATION, tells apart, and
#      that fails outright when the loader forces layers (the probe cannot tell then); a forced layer
#      that fails also fails a run that does not require validation;
#   5. without any layer requirement, a device error that is not a no-Vulkan error (no adapter matches
#      HELIOS_RHI_ADAPTER) fails too;
#   6. a layer listed under the validator's name that loads, works and validates nothing (Linux:
#      FAKE_LAYER_PASSTHROUGH): the loader accepts it and puts it in the call chain, so only the RHI's
#      own check (the layer must report itself as a validation tool) tells; --require-validation fails,
#      and --validation runs and reports "NOT validated".
# Cases 1, 2 and 5 work whether or not the layer is installed. On a machine without any Vulkan device
# the check reports SKIPPED.
#
# Cases 1-4 and 6 change what the loader loads through environment variables, so cases 1-3 first check
# that the variables take effect (`--print-probe`: the RHI reports Khronos validation active only when
# the layer is in the call chain); cases 4 and 6 (VK_LAYER_PATH, like case 2) run when case 2's did. The Windows loader ignores VK_LOADER_LAYERS_DISABLE, VK_LAYER_PATH
# and VK_ADD_LAYER_PATH in a high-integrity process (an elevated one, or a service such as the win-gpu
# runner: the SERVICE group grants SeImpersonatePrivilege, which raises the token to High), and a loader
# settings file (Vulkan Configurator) can force the layer on or off on any platform. Where the
# environment cannot take the layer away, a "validated" pass is the truth (the layer did report itself),
# the case cannot be exercised, and the script says so: on Windows it is a NOTE (the win-gpu job prints
# the integrity level and the loader settings for the record), on Linux it fails, so CI keeps them all.
# Inputs: -DRENDERTEST=<exe> -DOUT=<scratch dir> [-DLAYER=<layer manifest found at configure time>]
#         [-DFAKE_LAYER_INITFAIL=<dir> -DFAKE_LAYER_NODRIVER=<dir> -DFAKE_LAYER_PASSTHROUGH=<dir>]
#         (directories of the fake manifests)

if(NOT RENDERTEST OR NOT OUT)
  message(FATAL_ERROR "usage: cmake -DRENDERTEST=<exe> -DOUT=<dir> [-DLAYER=<manifest>] -P validation_checks.cmake")
endif()
file(REMOVE_RECURSE "${OUT}")
set(ENV{HELIOS_SKIP_GPU_TESTS} 1)
unset(ENV{HELIOS_RHI_VALIDATION})
# Layers the caller's environment forces on would change every case; case 4 sets these itself.
# VK_LOADER_LAYERS_ALLOW exempts the layers it matches from VK_LOADER_LAYERS_DISABLE (loader 1.3.262+).
unset(ENV{VK_INSTANCE_LAYERS})
unset(ENV{VK_LOADER_LAYERS_ENABLE})
unset(ENV{VK_LOADER_LAYERS_ALLOW})

# Is there a Vulkan device at all? (Without validation, so hidden or broken layers do not matter.)
set(ENV{VK_LOADER_LAYERS_DISABLE} VK_LAYER_KHRONOS_validation)
execute_process(COMMAND "${RENDERTEST}" --backend vulkan --scene triangle --out "${OUT}/probe"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(out MATCHES "SKIP vulkan")
  message(STATUS "SKIPPED: no Vulkan device (${out})")
  return()
endif()

function(expect_failure_not_skip what)
  execute_process(COMMAND "${RENDERTEST}" ${ARGN} --out "${OUT}/required"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR out MATCHES "SKIP" OR NOT err MATCHES "HELIOS_SKIP_GPU_TESTS does not skip that")
    message(FATAL_ERROR "helios-rendertest ${ARGN} with ${what}: expected a failure that is not a skip, "
                        "got ${rc}:\n${out}\n${err}")
  endif()
  set(last_err "${err}" PARENT_SCOPE)
endfunction()
function(expect_layer_failure what)
  expect_failure_not_skip("${what}" --backend vulkan --scene triangle --require-validation)
  set(last_err "${last_err}" PARENT_SCOPE)
  expect_failure_not_skip("${what}" --validation-self-test)
endfunction()

# The RHI's verdict under the current environment for a default device with HELIOS_RHI_VALIDATION set
# to `request`: <out_var> is "active" (the layer is in the call chain), "inactive", or "error" (no
# default device at all, e.g. the layer fails to load). `probe_output` keeps what the tool printed.
function(probe_validation out_var request)
  set(saved "$ENV{HELIOS_RHI_VALIDATION}")
  set(ENV{HELIOS_RHI_VALIDATION} ${request})
  execute_process(COMMAND "${RENDERTEST}" --print-probe RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(saved STREQUAL "")
    unset(ENV{HELIOS_RHI_VALIDATION})
  else()
    set(ENV{HELIOS_RHI_VALIDATION} "${saved}")
  endif()
  if(NOT rc EQUAL 0)
    set(${out_var} error PARENT_SCOPE)
  elseif(out MATCHES "default device: Khronos validation active")
    set(${out_var} active PARENT_SCOPE)
  else()
    set(${out_var} inactive PARENT_SCOPE)
  endif()
  set(probe_output "${out}${err}" PARENT_SCOPE)
endfunction()

# A case the environment cannot set up (see the header): fails on Linux, a NOTE on Windows. Appends
# to `not_checked` in the caller.
set(not_checked "")
macro(cannot_check what why)
  if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "${what}: ${why}\n${probe_output}")
  endif()
  message(STATUS "NOTE: not checked (${what}): ${why}")
  string(APPEND not_checked "\n  - ${what}")
endmacro()

# Whether the environment set up for a case takes the layer away. Sets `run` to TRUE when the case
# can be exercised.
macro(environment_takes_effect what)
  probe_validation(state 1)
  if(state STREQUAL "active")
    cannot_check("${what}" "the loader kept Khronos validation in the call chain (the layer reported itself as a validation tool): it ignores these variables in a high-integrity process (elevated, or a Windows service), or a loader settings file forces the layer on")
    set(run FALSE)
  else()
    set(run TRUE)
  endif()
endmacro()

# 1. Hidden layer, with and without a usable adapter (the latter fails on the adapter either way).
environment_takes_effect("the layer hidden by the loader (VK_LOADER_LAYERS_DISABLE)")
if(run)
  expect_layer_failure("the layer hidden by the loader")
endif()
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
environment_takes_effect("a broken layer manifest on VK_LAYER_PATH")
set(layer_path_works ${run})  # cases 4 and 6 rely on VK_LAYER_PATH too
if(run)
  set(ENV{HELIOS_RHI_VALIDATION} 1)
  expect_layer_failure("a broken layer manifest and HELIOS_RHI_VALIDATION=1")
  unset(ENV{HELIOS_RHI_VALIDATION})
endif()
unset(ENV{VK_LAYER_PATH})
if(saved_add)
  set(ENV{VK_ADD_LAYER_PATH} "${saved_add}")
endif()

# 3. The probe ignores HELIOS_RHI_VALIDATION, which does enable the layer on an ordinary device. A
#    loader that puts the layer into every instance unasked validates the probe device too.
if(LAYER)
  probe_validation(unrequested 0)
  if(unrequested STREQUAL "active")
    cannot_check("HELIOS_RHI_VALIDATION=1 leaves the probe device unvalidated"
                 "the loader puts Khronos validation into every instance without a request (a loader settings file with the layer \"on\")")
  else()
    set(ENV{HELIOS_RHI_VALIDATION} 1)
    execute_process(COMMAND "${RENDERTEST}" --print-probe RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "default device: Khronos validation active"
       OR NOT out MATCHES "probe device: Khronos validation inactive")
      message(FATAL_ERROR "with HELIOS_RHI_VALIDATION=1 the default device must validate and the probe must not, "
                          "got ${rc}:\n${out}\n${err}")
    endif()
    unset(ENV{HELIOS_RHI_VALIDATION})
  endif()
endif()

# 4. A layer that loads and fails. VK_LAYER_PATH holds only the fake layer.
if(FAKE_LAYER_INITFAIL AND FAKE_LAYER_NODRIVER)
  unset(ENV{VK_ADD_LAYER_PATH})
  set(ENV{VK_LAYER_PATH} "${FAKE_LAYER_INITFAIL}")
  if(layer_path_works)
    expect_layer_failure("a layer whose vkCreateInstance fails")
    set(ENV{HELIOS_RHI_VALIDATION} 1)
    expect_layer_failure("a layer whose vkCreateInstance fails and HELIOS_RHI_VALIDATION=1")
    unset(ENV{HELIOS_RHI_VALIDATION})
    set(ENV{VK_INSTANCE_LAYERS} VK_LAYER_KHRONOS_validation)
    expect_layer_failure("a failing layer the loader forces on every instance (VK_INSTANCE_LAYERS)")
    # Without a layer requirement only the no-Vulkan errors skip, so nothing but that rule fails this.
    expect_failure_not_skip("a failing layer the loader forces on every instance, validation not required"
                            --backend vulkan --scene triangle)
    unset(ENV{VK_INSTANCE_LAYERS})
    set(ENV{VK_LOADER_LAYERS_ENABLE} "*validation")
    expect_layer_failure("a failing layer the loader forces on every instance (VK_LOADER_LAYERS_ENABLE)")
    unset(ENV{VK_LOADER_LAYERS_ENABLE})
    set(ENV{VK_LAYER_PATH} "${FAKE_LAYER_NODRIVER}")
    expect_layer_failure("a layer that fails with VK_ERROR_INCOMPATIBLE_DRIVER")
    set(ENV{HELIOS_RHI_VALIDATION} 1)
    expect_layer_failure("a layer that fails with VK_ERROR_INCOMPATIBLE_DRIVER and HELIOS_RHI_VALIDATION=1")
    unset(ENV{HELIOS_RHI_VALIDATION})
    set(ENV{VK_INSTANCE_LAYERS} VK_LAYER_KHRONOS_validation)
    expect_layer_failure("a layer forced on every instance that fails with VK_ERROR_INCOMPATIBLE_DRIVER")
    unset(ENV{VK_INSTANCE_LAYERS})
  endif()
  unset(ENV{VK_LAYER_PATH})
  if(saved_add)
    set(ENV{VK_ADD_LAYER_PATH} "${saved_add}")
  endif()
endif()

# 5. No layer involved: an adapter selection that matches nothing is an error, not a missing device.
set(ENV{HELIOS_RHI_ADAPTER} "no-such-adapter-for-rendertest-validation-checks")
expect_failure_not_skip("no adapter matching HELIOS_RHI_ADAPTER" --backend vulkan --scene triangle)
unset(ENV{HELIOS_RHI_ADAPTER})

# 6. A layer named like the validator that loads, works and validates nothing.
#    (Its precondition is case 2's: the RHI's own verdict cannot vouch for this environment.)
if(FAKE_LAYER_PASSTHROUGH)
  unset(ENV{VK_ADD_LAYER_PATH})
  set(ENV{VK_LAYER_PATH} "${FAKE_LAYER_PASSTHROUGH}")
  if(layer_path_works)
    expect_layer_failure("a pass-through layer named VK_LAYER_KHRONOS_validation")
    if(NOT last_err MATCHES "not in the instance's call chain")
      message(FATAL_ERROR "a pass-through layer must fail for not being in the call chain, got:\n${last_err}")
    endif()
    set(ENV{HELIOS_RHI_VALIDATION} 1)
    expect_layer_failure("a pass-through layer and HELIOS_RHI_VALIDATION=1")
    unset(ENV{HELIOS_RHI_VALIDATION})
    # Not required: the scene runs, and says it was not validated.
    execute_process(COMMAND "${RENDERTEST}" --backend vulkan --scene triangle --validation --out "${OUT}/passthrough"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "NOT validated")
      message(FATAL_ERROR "--validation with a pass-through layer must run and report \"NOT validated\", "
                          "got ${rc}:\n${out}\n${err}")
    endif()
  endif()
  unset(ENV{VK_LAYER_PATH})
  if(saved_add)
    set(ENV{VK_ADD_LAYER_PATH} "${saved_add}")
  endif()
endif()

if(not_checked)
  message(STATUS "NOTE: cases this machine's loader does not let the environment set up:${not_checked}")
endif()
message(STATUS "a required validation layer that is not in the call chain fails, and is not skipped")
