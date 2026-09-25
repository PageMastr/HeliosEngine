# helios_schema(<target>
#     FILES <a.hschema> ...           schemas to compile (their imports are found through INCLUDE_DIRS)
#     [INCLUDE_DIRS <dir> ...]        import roots; also define generated paths (default: ${PROJECT_SOURCE_DIR}/schemas)
#     [LOCK <file>]                   append-only schema lock (default: ${CMAKE_CURRENT_SOURCE_DIR}/schema.lock.jsonc)
#     [CPP_OUT <dir>]                 generated C++ root (default: ${CMAKE_CURRENT_BINARY_DIR}/<target>_schema)
#     [GO_OUT <dir> [GO_PACKAGE <n>]] also generate a Go package into <dir>
#     [JSON_OUT <file>]               also write the machine-readable schema description
#     [SAMPLES])                      also generate <file>.samples.gen.h (test values shared with Go)
#
# Runs helios-schemac at build time (add_custom_command with a depfile, so edits to the schemas,
# their imports or the lock regenerate), adds the generated C++ to <target>, puts CPP_OUT on its
# include path and links helios::reflect. Include generated headers as "<path>/<file>.gen.h",
# where <path> is the schema's path relative to its include root.
#
# The lock file lives in the source tree and is updated by the build when schemas add ids; commit
# it. Configure with -DHELIOS_SCHEMA_CHECK_LOCK=ON in CI to fail instead (02 §3.4).
#
# Cross-compiling (e.g. MinGW from Linux): helios-schemac must run on the build host.
#   * -DHELIOS_HOST_SCHEMAC=<path to a host helios-schemac> uses a prebuilt host binary; or
#   * with CMAKE_CROSSCOMPILING_EMULATOR set, the target build runs through the emulator; or
#   * otherwise a host copy is built automatically with ExternalProject (build/host-schemac),
#     using the host compilers (override with HELIOS_HOST_C_COMPILER / HELIOS_HOST_CXX_COMPILER).
# Works with Ninja, Makefiles and Visual Studio generators ($<TARGET_FILE> resolution by CMake).

include_guard(GLOBAL)

set(HELIOS_HOST_SCHEMAC "" CACHE FILEPATH
    "helios-schemac executable for the build host (used when cross-compiling; empty = build or emulate)")
set(HELIOS_HOST_C_COMPILER "" CACHE STRING "Host C compiler for the automatic host helios-schemac build")
set(HELIOS_HOST_CXX_COMPILER "" CACHE STRING "Host C++ compiler for the automatic host helios-schemac build")
option(HELIOS_SCHEMA_CHECK_LOCK "helios_schema(): fail when a schema lock is out of date instead of updating it (CI)" OFF)

# Returns in <out_exe>/<out_dep> how to invoke helios-schemac for this build.
function(_helios_schemac_command out_exe out_dep)
  if(HELIOS_HOST_SCHEMAC)
    set(${out_exe} "${HELIOS_HOST_SCHEMAC}" PARENT_SCOPE)
    set(${out_dep} "${HELIOS_HOST_SCHEMAC}" PARENT_SCOPE)
  elseif(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(host_dir ${CMAKE_BINARY_DIR}/host-schemac)
    set(host_exe ${host_dir}/bin/helios-schemac${CMAKE_HOST_EXECUTABLE_SUFFIX})
    if(NOT TARGET helios_host_schemac)
      include(ExternalProject)
      # An empty CMAKE_TOOLCHAIN_FILE keeps a CMAKE_TOOLCHAIN_FILE environment variable (the cross
      # toolchain) out of the host build; CC/CXX/*FLAGS are unset for the same reason.
      set(host_args -DHELIOS_BUILD_GRAPHICS=OFF -DHELIOS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
                    -DCMAKE_TOOLCHAIN_FILE=)
      if(HELIOS_HOST_C_COMPILER)
        list(APPEND host_args -DCMAKE_C_COMPILER=${HELIOS_HOST_C_COMPILER})
      endif()
      if(HELIOS_HOST_CXX_COMPILER)
        list(APPEND host_args -DCMAKE_CXX_COMPILER=${HELIOS_HOST_CXX_COMPILER})
      endif()
      if(CMAKE_MAKE_PROGRAM AND NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
        list(APPEND host_args -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
      endif()
      # Explicit configure step: ExternalProject's default would forward the target platform
      # (-A / CMAKE_GENERATOR_PLATFORM) and the environment's cross compilers to the host build.
      ExternalProject_Add(helios_host_schemac
        SOURCE_DIR ${PROJECT_SOURCE_DIR}
        BINARY_DIR ${host_dir}
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env --unset=CC --unset=CXX --unset=CFLAGS --unset=CXXFLAGS --unset=LDFLAGS
                          --unset=CMAKE_TOOLCHAIN_FILE
                          ${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} -S <SOURCE_DIR> -B <BINARY_DIR> ${host_args}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target helios-schemac --config Release
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS ${host_exe}
        EXCLUDE_FROM_ALL ON)
      message(STATUS "helios_schema: cross-compiling; a host helios-schemac is built in ${host_dir} "
                     "(set HELIOS_HOST_SCHEMAC to use a prebuilt one)")
    endif()
    set(${out_exe} "${host_exe}" PARENT_SCOPE)
    # Target-level ordering plus the binary itself (a rebuilt schemac regenerates the code).
    set(${out_dep} helios_host_schemac "${host_exe}" PARENT_SCOPE)
  else()
    # A target name in COMMAND becomes its $<TARGET_FILE> (plus the emulator when one is set).
    set(${out_exe} helios-schemac PARENT_SCOPE)
    set(${out_dep} helios-schemac PARENT_SCOPE)
  endif()
endfunction()

function(helios_schema target)
  cmake_parse_arguments(S "SAMPLES" "LOCK;CPP_OUT;GO_OUT;GO_PACKAGE;JSON_OUT" "FILES;INCLUDE_DIRS" ${ARGN})
  if(NOT S_FILES)
    message(FATAL_ERROR "helios_schema(${target}): FILES is required")
  endif()
  if(NOT S_INCLUDE_DIRS)
    set(S_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/schemas)
  endif()
  if(NOT S_LOCK)
    set(S_LOCK ${CMAKE_CURRENT_SOURCE_DIR}/schema.lock.jsonc)
  endif()
  if(NOT S_CPP_OUT)
    set(S_CPP_OUT ${CMAKE_CURRENT_BINARY_DIR}/${target}_schema)
  endif()
  get_filename_component(S_LOCK "${S_LOCK}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

  set(inputs "")
  set(outputs "")
  set(cpp_sources "")
  set(go_files "")
  foreach(file IN LISTS S_FILES)
    get_filename_component(abs "${file}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    list(APPEND inputs "${abs}")
    # Logical path: relative to the first include root containing the file (as helios-schemac does).
    get_filename_component(logical "${abs}" NAME)
    foreach(root IN LISTS S_INCLUDE_DIRS)
      get_filename_component(root_abs "${root}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
      file(RELATIVE_PATH rel "${root_abs}" "${abs}")
      if(NOT rel MATCHES "^\\.\\.")
        set(logical "${rel}")
        break()
      endif()
    endforeach()
    string(REGEX REPLACE "\\.hschema$" "" base "${logical}")
    list(APPEND outputs "${S_CPP_OUT}/${base}.gen.h" "${S_CPP_OUT}/${base}.gen.cpp")
    list(APPEND cpp_sources "${S_CPP_OUT}/${base}.gen.h" "${S_CPP_OUT}/${base}.gen.cpp")
    if(S_SAMPLES)
      list(APPEND outputs "${S_CPP_OUT}/${base}.samples.gen.h")
    endif()
    if(S_GO_OUT)
      get_filename_component(stem "${abs}" NAME_WE)
      list(APPEND go_files "${S_GO_OUT}/${stem}.go")
    endif()
  endforeach()

  set(args "")
  foreach(root IN LISTS S_INCLUDE_DIRS)
    get_filename_component(root_abs "${root}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    list(APPEND args -I "${root_abs}")
  endforeach()
  set(emit cpp)
  list(APPEND args --lock "${S_LOCK}" --cpp-out "${S_CPP_OUT}" --quiet)
  if(HELIOS_SCHEMA_CHECK_LOCK)
    list(APPEND args --check-lock)
  endif()
  if(S_SAMPLES)
    list(APPEND args --samples)
  endif()
  if(S_GO_OUT)
    string(APPEND emit ",go")
    list(APPEND args --go-out "${S_GO_OUT}")
    if(S_GO_PACKAGE)
      list(APPEND args --go-package "${S_GO_PACKAGE}")
    endif()
    list(APPEND go_files "${S_GO_OUT}/helios_runtime.go" "${S_GO_OUT}/helios_codecs.go" "${S_GO_OUT}/helios_schema_test.go")
    list(APPEND outputs ${go_files})
  endif()
  if(S_JSON_OUT)
    get_filename_component(json_abs "${S_JSON_OUT}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_BINARY_DIR})
    string(APPEND emit ",json")
    list(APPEND args --json-out "${json_abs}")
    list(APPEND outputs "${json_abs}")
  endif()
  # One depfile per helios_schema() call (a target may call it several times).
  get_property(call_index TARGET ${target} PROPERTY _HELIOS_SCHEMA_CALLS)
  if(NOT call_index)
    set(call_index 0)
  endif()
  math(EXPR next_index "${call_index} + 1")
  set_property(TARGET ${target} PROPERTY _HELIOS_SCHEMA_CALLS ${next_index})
  if(call_index EQUAL 0)
    set(depfile ${CMAKE_CURRENT_BINARY_DIR}/${target}_schema.d)
  else()
    set(depfile ${CMAKE_CURRENT_BINARY_DIR}/${target}_schema${call_index}.d)
  endif()
  list(GET outputs 0 first_output)
  list(APPEND args --emit ${emit} --depfile "${depfile}" --depfile-target "${first_output}")

  set(depends ${inputs})
  if(EXISTS "${S_LOCK}")
    list(APPEND depends "${S_LOCK}")
  endif()
  _helios_schemac_command(schemac_exe schemac_dep)
  add_custom_command(
    OUTPUT ${outputs}
    COMMAND ${schemac_exe} ${args} ${inputs}
    DEPENDS ${depends} ${schemac_dep}
    DEPFILE ${depfile}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "helios-schemac: generating code for ${target}"
    VERBATIM)
  target_sources(${target} PRIVATE ${cpp_sources})
  target_include_directories(${target} PUBLIC ${S_CPP_OUT})
  target_link_libraries(${target} PUBLIC helios::reflect)
  set_property(TARGET ${target} APPEND PROPERTY HELIOS_SCHEMA_CPP_OUT ${S_CPP_OUT})
endfunction()
