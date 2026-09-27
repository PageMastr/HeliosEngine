# CLI checks of helios-rendertest that need no GPU (run with cmake -P; see ../CMakeLists.txt):
#   * a Vulkan run skipped with HELIOS_SKIP_GPU_TESTS=1 records "skip" results, so stale passing
#     results of an earlier run in the same --out directory cannot stand in for it in the report;
#   * results of scenes that no longer exist are left out of the report;
#   * --budget rejects values that are not a positive number of seconds;
#   * --coverage fails, naming what is uncovered, when the counted scenes do not bind every shipped
#     entry point, when a scene's goldens are missing, or when a rendertest pipeline takes a shipped name.
# Inputs: -DRENDERTEST=<exe> -DOUT=<scratch dir>

if(NOT RENDERTEST OR NOT OUT)
  message(FATAL_ERROR "usage: cmake -DRENDERTEST=<exe> -DOUT=<dir> -P cli_checks.cmake")
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/vulkan")
set(stale "{ \"scene\": \"triangle\", \"backend\": \"vulkan\", \"status\": \"pass\", \"milliseconds\": 1 }\n")
file(WRITE "${OUT}/vulkan/triangle.json" "${stale}")
file(WRITE "${OUT}/vulkan/removed-scene.json"
     "{ \"scene\": \"removed-scene\", \"backend\": \"vulkan\", \"status\": \"fail\", \"milliseconds\": 1 }\n")

set(ENV{HELIOS_SKIP_GPU_TESTS} 1)
set(ENV{HELIOS_RHI_ADAPTER} "no-such-adapter-for-rendertest-cli-checks")
execute_process(COMMAND "${RENDERTEST}" --backend vulkan --scene triangle --out "${OUT}" --report
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "skipped run failed (${rc}):\n${out}\n${err}")
endif()
file(READ "${OUT}/vulkan/triangle.json" result)
if(NOT result MATCHES "\"status\": \"skip\"")
  message(FATAL_ERROR "the skipped scene kept its stale result:\n${result}")
endif()
file(READ "${OUT}/report.md" report)
if(report MATCHES "removed-scene")
  message(FATAL_ERROR "the report lists a scene that no longer exists:\n${report}")
endif()
if(NOT report MATCHES "\\| triangle \\| vulkan \\| skip \\|")
  message(FATAL_ERROR "the report does not show the skip:\n${report}")
endif()

foreach(bad abc 0 -5 12x)
  execute_process(COMMAND "${RENDERTEST}" --report-only --out "${OUT}" --budget ${bad}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 2 OR NOT err MATCHES "positive number of seconds")
    message(FATAL_ERROR "--budget ${bad}: expected a usage error, got ${rc}:\n${out}\n${err}")
  endif()
endforeach()
# The triangle scene creates none of engine/render's pipelines: every shipped entry point is uncovered.
execute_process(COMMAND "${RENDERTEST}" --coverage --scene triangle
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "shipped entry point tonemap:psTonemap \\(fragment\\) is bound by no golden scene")
  message(FATAL_ERROR "--coverage --scene triangle: expected uncovered entry points, got ${rc}:\n${out}\n${err}")
endif()
# The forward scene creates the DebugNormals pipeline but never binds it (forward:psNormals uncovered).
execute_process(COMMAND "${RENDERTEST}" --coverage --scene forward
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "pipeline 'Forward.DebugNormals' is bound by no golden scene"
   OR NOT err MATCHES "forward:psNormals" OR err MATCHES "tonemap:psTonemap")
  message(FATAL_ERROR "--coverage --scene forward: expected only the debug view uncovered, got ${rc}:\n${out}\n${err}")
endif()
# A rendertest pipeline named like a shipped one would count as its coverage; the check rejects it.
execute_process(COMMAND "${RENDERTEST}" --coverage --seed-name-collision
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "rendertest pipeline 'Forward.Geometry' reuses the name of a shipped pipeline")
  message(FATAL_ERROR "--coverage --seed-name-collision: expected a name-collision failure, got ${rc}:\n${out}\n${err}")
endif()
# Scenes count only with committed goldens.
file(MAKE_DIRECTORY "${OUT}/no-goldens")
execute_process(COMMAND "${RENDERTEST}" --coverage --golden-dir "${OUT}/no-goldens"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "scene 'forward' has no committed lavapipe and Null goldens")
  message(FATAL_ERROR "--coverage without goldens: expected a failure, got ${rc}:\n${out}\n${err}")
endif()
message(STATUS "helios-rendertest CLI checks passed")
