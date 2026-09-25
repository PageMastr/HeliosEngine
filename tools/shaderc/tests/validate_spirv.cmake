# Runs spirv-val (through helios-shaderc --reflect-spirv, which also reflects each module) over every
# SPIR-V module the build compiled in the given directories (03 §1.7: "spirv-val runs over every
# cooked shader in Linux CI"). With -DREQUIRE=ON a missing spirv-val fails; otherwise it is skipped.
# Inputs: -DSHADERC=<exe> -DDIRS=<dir;dir;...> [-DREQUIRE=ON]

if(NOT SHADERC OR NOT DIRS)
  message(FATAL_ERROR "usage: cmake -DSHADERC=<exe> -DDIRS=<dirs> [-DREQUIRE=ON] -P validate_spirv.cmake")
endif()
set(mode auto)
if(REQUIRE)
  set(mode on)
endif()
set(count 0)
foreach(dir ${DIRS})
  file(GLOB modules "${dir}/*.spv")
  foreach(spv ${modules})
    execute_process(COMMAND "${SHADERC}" --reflect-spirv "${spv}" --no-hsr --validate ${mode}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${spv}: helios-shaderc/spirv-val failed (${rc}):\n${out}${err}")
    endif()
    if(out MATCHES "validation skipped")
      message(STATUS "spirv-val not found: validation skipped")
      return()
    endif()
    math(EXPR count "${count} + 1")
  endforeach()
endforeach()
if(count EQUAL 0)
  message(FATAL_ERROR "no SPIR-V modules found in ${DIRS}")
endif()
message(STATUS "spirv-val: ${count} module(s) valid")
