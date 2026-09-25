# engine/net — L2 HTP transport (HEADLESS; 04 §2, ADR-007, ADR-013): UdpSocket (Winsock2 / BSD
# sockets), IDatagramTransport (sockets, in-process VirtualNetwork, NetSim), netcode 1.4.8
# client/server wrappers with Helios allocators and logging, one reliable 1.4.5 endpoint per
# session, HTP channels, packing, fragmentation, budgets/AIMD, stats, trunk profile.
#
# OS-specific code lives only in src/platform/win32 (selected when WIN32, including MinGW) and
# src/platform/posix.

file(GLOB HELIOS_NET_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(WIN32)
  file(GLOB HELIOS_NET_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/*.cpp)
else()
  file(GLOB HELIOS_NET_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/*.cpp)
endif()

helios_module(net HEADLESS
  SOURCES ${HELIOS_NET_SOURCES} ${HELIOS_NET_PLATFORM_SOURCES}
  DEPS helios::core helios::math
  PRIVATE_DEPS helios::tp::netcode)

if(WIN32)
  target_link_libraries(helios_net PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------------------------
# Tests (doctest). Go-issued golden token vectors are read from services/testdata/vectors.
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_NET_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(net_tests SOURCES ${HELIOS_NET_TEST_SOURCES} DEPS helios::net helios::tp::netcode helios::tp::yyjson)
if(TARGET net_tests)
  target_include_directories(net_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/bench)
  target_compile_definitions(net_tests PRIVATE
    HELIOS_NET_TOKEN_VECTORS_DIR="${PROJECT_SOURCE_DIR}/services/testdata/vectors"
    HELIOS_NET_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus")
  set_tests_properties(net_tests PROPERTIES TIMEOUT 600)
endif()

# ---------------------------------------------------------------------------------------------
# Fuzz targets (04 §9, NS-0.4). Each target is a libFuzzer entry point (LLVMFuzzerTestOneInput).
# With HELIOS_NET_LIBFUZZER=ON and Clang they link libFuzzer's driver (-fsanitize=fuzzer); give
# the whole build coverage instrumentation and sanitizers with
#   -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" (and the same CMAKE_CXX_FLAGS)
# so helios_net and the vendored netcode/reliable read paths are instrumented too. Otherwise the
# targets build as plain executables that replay files/directories given on the command line (or
# the committed seed corpus) plus a deterministic mutation smoke run, registered as CTests.
# ---------------------------------------------------------------------------------------------
option(HELIOS_NET_LIBFUZZER "Build engine/net fuzz targets as libFuzzer binaries (Clang only)" OFF)
set(HELIOS_NET_FUZZ_TARGETS packet_parser connection_receive connect_token server_datagram address_parse)
foreach(t ${HELIOS_NET_FUZZ_TARGETS})
  set(exe net_fuzz_${t})
  if(HELIOS_NET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(${exe} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_${t}.cpp)
    target_compile_options(${exe} PRIVATE -fsanitize=fuzzer)
    target_link_options(${exe} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${exe} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_${t}.cpp ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_main.cpp)
  endif()
  target_link_libraries(${exe} PRIVATE helios::net helios::tp::netcode)
  target_include_directories(${exe} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/fuzz)
  set_target_properties(${exe} PROPERTIES FOLDER fuzz)
  helios_apply_warnings(${exe})
  if(HELIOS_BUILD_TESTS AND NOT (HELIOS_NET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    add_test(NAME ${exe} COMMAND ${exe} --smoke 20000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${t})
    set_tests_properties(${exe} PROPERTIES LABELS fuzz TIMEOUT 600)
  endif()
endforeach()
add_custom_target(net_fuzz)
foreach(t ${HELIOS_NET_FUZZ_TARGETS})
  add_dependencies(net_fuzz net_fuzz_${t})
endforeach()

# ---------------------------------------------------------------------------------------------
# Benchmarks / gates: NS-0.2 (loopback pps/core) and NS-0.7 (trunk throughput). Executable only;
# CI runs `net_bench --gate` in the nightly tier (10 min at 20k pps).
# ---------------------------------------------------------------------------------------------
helios_executable(net_bench SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/bench/net_bench.cpp DEPS helios::net)
target_include_directories(net_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/bench)
set_target_properties(net_bench PROPERTIES FOLDER bench)
