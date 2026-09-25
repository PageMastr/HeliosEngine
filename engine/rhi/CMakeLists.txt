# engine/rhi — L3 render hardware interface (non-HEADLESS; ADR-003, docs/plan/03-rendering.md §1).
# Backends: Vulkan 1.3 (volk + VMA, src/vulkan) and Null (src/null). No Vulkan type is visible
# outside src/vulkan. SDL3 is used privately for swapchain surfaces only.

include(HeliosShaders)

file(GLOB HELIOS_RHI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/null/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/*.cpp)

helios_module(rhi
  SOURCES ${HELIOS_RHI_SOURCES}
  DEPS helios::core
  PRIVATE_DEPS helios::tp::volk helios::tp::vma helios::tp::sdl3)

# One VMA configuration for every TU that includes vk_mem_alloc.h (functions come from volk).
target_compile_definitions(helios_rhi PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

# ---------------------------------------------------------------------------------------------
# Tests. `rhi_tests` runs the Null-backend, format, shader-toolchain and utility suites by default;
# the Vulkan suites (doctest test-suite "gpu*") run as the separate CTest entry `rhi_tests_gpu`
# with the `gpu` label (lavapipe in CI: ctest -L gpu). Golden images live in tests/golden/.
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_RHI_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
# SDL3 only for test_swapchain.cpp (offscreen video driver + VK_EXT_headless_surface: swapchains
# without a display server).
helios_test(rhi_tests SOURCES ${HELIOS_RHI_TEST_SOURCES}
            DEPS helios::rhi helios::math helios::tp::headers helios::tp::sdl3)
if(TARGET rhi_tests)
  file(GLOB HELIOS_RHI_TEST_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/*.slang)
  helios_shaders(rhi_tests NAMESPACE rhi_test_shaders FILES ${HELIOS_RHI_TEST_SHADERS})
  target_compile_definitions(rhi_tests PRIVATE
    HELIOS_RHI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    HELIOS_RHI_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/rhi_test_output")
  add_test(NAME rhi_tests_gpu COMMAND rhi_tests --test-suite=gpu*)
  set_tests_properties(rhi_tests_gpu PROPERTIES LABELS gpu TIMEOUT 600)
  set_tests_properties(rhi_tests PROPERTIES TIMEOUT 300)
endif()
