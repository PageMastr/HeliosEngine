# engine/server — L4 cell and gateway runtime (HEADLESS; 04 §1, §2.3–2.6, §3, §9; 05 §1.3–1.4, §2):
# ZoneHost / ZoneInstance / per-tick job graph, the trunk and client protocols, CellServer and
# GatewayServer (usable in-process), the control-plane bus (nats.c, and an in-process fake), the
# orchestrator client (lease holder rule, ID blocks) and the Go services' JSON contracts.
#
# 02 §1.1 names no module for the cell's zone host (it lists ZoneHost among what "the engine
# provides", 02 §2.4); until engine/CMakeLists.txt gets this row (and HELIOS_MODULE_ORDER the
# name, after authority), it is declared here. (Passing LAYER to helios_module() for an
# undeclared module trips a quoting bug in cmake/HeliosModule.cmake, so the row form is used.)
# nats.c and yyjson stay private (no third-party types in public headers).
get_property(_heliosServerRow GLOBAL PROPERTY HELIOS_DECL_server_LAYER SET)
if(NOT _heliosServerRow)
  helios_declare_module(server 4 HEADLESS PEERS authority)
endif()

file(GLOB HELIOS_SERVER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

helios_module(server HEADLESS
  SOURCES ${HELIOS_SERVER_SOURCES}
  DEPS helios::core helios::net helios::ecs helios::authority
  PRIVATE_DEPS helios::tp::natsc helios::tp::yyjson)

file(GLOB HELIOS_SERVER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(server_tests SOURCES ${HELIOS_SERVER_TEST_SOURCES} DEPS helios::server helios::tp::yyjson)
if(TARGET server_tests)
  target_include_directories(server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(server_tests PRIVATE
    HELIOS_SERVER_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  set_tests_properties(server_tests PROPERTIES TIMEOUT 600)
  # NS-0.3's cross-host half (tests/test_interop.cpp), which runs only in these entries
  # (HELIOS_NS03_ENTRY). server_tests_ns03_remote reports Skipped when HELIOS_NS03_GATEWAY and
  # HELIOS_NS03_KEYS are unset, or when a HELIOS_NS03_ALLOW_LOOPBACK=1 same-host smoke run passed
  # (neither is evidence), Passed only for a target on another host, and Failed for a refused
  # same-host target or a filter that matches no case (a renamed case must not turn into Skipped).
  set(_ns03Case "server.interop: NS-0.3 a probe here is served by the gateway on another host (HELIOS_NS03_GATEWAY)")
  add_test(NAME server_tests_ns03_remote COMMAND server_tests "--test-case=${_ns03Case}")
  set_tests_properties(server_tests_ns03_remote PROPERTIES
    TIMEOUT 60 ENVIRONMENT "HELIOS_NS03_ENTRY=1"
    SKIP_REGULAR_EXPRESSION "NS-0\\.3 remote: (not configured|same-host smoke run passed)"
    FAIL_REGULAR_EXPRESSION "test cases: +0 \\|")
  # The seeded refusal: a loopback target must be refused (the case never tries to connect).
  add_test(NAME server_tests_ns03_refusal COMMAND server_tests "--test-case=${_ns03Case}")
  set_tests_properties(server_tests_ns03_refusal PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "HELIOS_NS03_ENTRY=1;HELIOS_NS03_GATEWAY=127.0.0.1:7777;HELIOS_NS03_KEYS=never-read.json;HELIOS_NS03_ALLOW_LOOPBACK=0"
    PASS_REGULAR_EXPRESSION "NS-0\\.3 remote: refused: 127\\.0\\.0\\.1:7777 is a loopback address")
endif()
