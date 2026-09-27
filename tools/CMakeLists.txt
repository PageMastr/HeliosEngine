# Build-time tools. Each tool directory with a CMakeLists.txt is added (headless: tools run in
# every build configuration, including servers and cross builds).
include(HeliosSchema)

foreach(tool schemac scorecard status milestone)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${tool}/CMakeLists.txt)
    add_subdirectory(${tool})
  endif()
endforeach()
