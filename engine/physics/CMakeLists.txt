# engine/physics — L3 physics (HEADLESS; 02 §5.4, §7.1): Jolt 5.6 grids (one PhysicsSystem per grid),
# Helios allocators/trace/asserts, Jolt's JobSystemWithBarrier on the Helios job system, stable body
# keys, deterministic queries, the CharacterVirtual mover and the RT-03 determinism hash.
#
# Jolt is a PRIVATE dependency: its headers select AVX paths inline, so only this module's TUs see
# tp_jolt's ISA flags and defines; the public headers contain no Jolt type (02 §1.1).

helios_module(physics HEADLESS
  SOURCES
    src/grid.cpp
    src/job_system.cpp
    src/layers.cpp
    src/runtime.cpp
    src/shape.cpp
    src/temp_allocator.cpp
  DEPS helios::core helios::math
  PRIVATE_DEPS helios::tp::jolt)

helios_test(physics_tests
  SOURCES
    tests/main.cpp
    tests/scene.cpp
    tests/test_runtime.cpp
    tests/test_grid.cpp
    tests/test_queries.cpp
    tests/test_character.cpp
    tests/test_jobs.cpp
    tests/test_determinism.cpp
  DEPS helios::physics)
if(TARGET physics_tests)
  set_tests_properties(physics_tests PROPERTIES TIMEOUT 900)
endif()
