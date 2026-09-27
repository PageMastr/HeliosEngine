# engine/ecs — L2 ECS integration on flecs v4.1.6 (HEADLESS; ADR-004, ADR-011, 02 §4):
# World wrapper, flecs OS API -> Helios (log/assert/time/mimalloc heaps/JobSystem tasks), component
# registration (template + runtime descriptors), EntityId/NetHandle maps, relationships and prefabs,
# deterministic scheduler, command buffers, per-field dirty tracking. flecs stays private to src/.

file(GLOB HELIOS_ECS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

helios_module(ecs HEADLESS
  SOURCES ${HELIOS_ECS_SOURCES}
  DEPS helios::core helios::math helios::tp::flecs helios::tp::mimalloc)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_ECS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(ecs_tests SOURCES ${HELIOS_ECS_TEST_SOURCES} DEPS helios::ecs helios::tp::yyjson)
if(TARGET ecs_tests)
  # Tests may include the private flecs header (src/flecs_internal.h) to check the integration.
  target_include_directories(ecs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # Block-id golden vectors shared with Go's services/pkg/idgen (05 §1.4.5).
  target_compile_definitions(ecs_tests PRIVATE
    HELIOS_ECS_BLOCK_ID_VECTORS="${PROJECT_SOURCE_DIR}/services/testdata/vectors/block_ids.json")
  set_tests_properties(ecs_tests PROPERTIES TIMEOUT 600)
endif()

# ---------------------------------------------------------------------------------------------
# RT-01 benchmark (50k-entity zone) + Phase 0 spikes. An executable, not a test by default.
# ---------------------------------------------------------------------------------------------
helios_executable(ecs_bench
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/bench/ecs_bench.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_zone.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_spikes.cpp
  DEPS helios::ecs)
target_include_directories(ecs_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(ecs_bench PROPERTIES FOLDER bench)

# The linux-bench preset (Release, asserts off) turns this on: the ADR-004a M1 build.
option(HELIOS_ECS_BENCH_CTEST "Register ecs_bench (RT-01, --quick) as a CTest with label 'bench'" OFF)
if(HELIOS_ECS_BENCH_CTEST AND HELIOS_BUILD_TESTS)
  add_test(NAME ecs_bench_rt01 COMMAND ecs_bench --quick)
  set_tests_properties(ecs_bench_rt01 PROPERTIES LABELS bench TIMEOUT 900)
  # Instruction counts of the 9k-op burst (SPIKES.md §5.2), where valgrind exists.
  find_program(HELIOS_VALGRIND valgrind)
  if(HELIOS_VALGRIND)
    add_test(NAME ecs_bench_callgrind
      COMMAND ${CMAKE_COMMAND} -DECS_BENCH=$<TARGET_FILE:ecs_bench> -DVALGRIND=${HELIOS_VALGRIND}
              -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/bench/callgrind_burst.cmake)
    set_tests_properties(ecs_bench_callgrind PROPERTIES LABELS "bench;callgrind" TIMEOUT 1800)
  endif()
endif()
