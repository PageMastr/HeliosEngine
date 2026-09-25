# helios_module(<name> [HEADLESS] SOURCES ... DEPS ... PRIVATE_DEPS ...)
#   Declares engine module library `helios_<name>` with alias `helios::<name>`.
#   Public headers live in engine/<name>/include/helios/<name>/..., sources in engine/<name>/src.
#   HEADLESS modules must never depend on graphics libraries so the dedicated server stays lean.
#
# helios_test(<name> SOURCES ... DEPS ...)
#   Declares a doctest executable registered with CTest.

function(helios_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(HELIOS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(HELIOS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(helios_module name)
  cmake_parse_arguments(M "HEADLESS" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  set(target helios_${name})
  add_library(${target} STATIC ${M_SOURCES})
  add_library(helios::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC ${M_DEPS} PRIVATE ${M_PRIVATE_DEPS})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER engine)
  helios_apply_warnings(${target})
endfunction()

function(helios_executable name)
  cmake_parse_arguments(E "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${E_SOURCES})
  target_link_libraries(${name} PRIVATE ${E_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER apps)
  helios_apply_warnings(${name})
endfunction()

function(helios_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEPS" ${ARGN})
  if(NOT HELIOS_BUILD_TESTS)
    return()
  endif()
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE helios::tp::doctest ${T_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER tests)
  helios_apply_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()
