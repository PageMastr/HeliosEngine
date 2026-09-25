# engine/pcg — L3 deterministic PCG runtime (HEADLESS; 02 §5.8, 03 §5.5a): fixed-point hnoise, the
# terrain graph and its register VM with scalar / SSE4.2 / AVX2 kernels. The Slang twin lives in
# shaders/pcg/ and is exercised by pcg_gpu_tests (graphics builds only; pcg itself never links rhi).

set(HELIOS_PCG_SOURCES
  src/compiler.cpp
  src/hnoise.cpp
  src/kernel.cpp
  src/reference_graphs.cpp
  src/terrain_graph.cpp
  src/tile.cpp
  src/kernels/vm_scalar.cpp)

helios_module(pcg HEADLESS
  SOURCES ${HELIOS_PCG_SOURCES}
  DEPS helios::core helios::math)

# The two SIMD kernels (02 §5.8 "SIMD kernels"). Until WP-0.2r builds whole images at the avx2
# level, the AVX2 kernel is a designated *_avx2.cpp unit on the ISA allowlist and is dispatched to
# only after CPUID (src/kernel.cpp). The SSE4.2 twin needs -msse4.2 on GCC/Clang/clang-cl (not an
# AVX-class flag); MSVC compiles SSE4.2 intrinsics at its default level.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  helios_avx2_sources(helios_pcg ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/vm_avx2.cpp)
  target_sources(helios_pcg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/vm_sse42.cpp)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/vm_sse42.cpp
      TARGET_DIRECTORY helios_pcg PROPERTIES COMPILE_OPTIONS "-msse4.2")
  endif()
else()
  target_sources(helios_pcg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/vm_avx2.cpp
                                    ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/vm_sse42.cpp)
endif()

# ---------------------------------------------------------------------------------------------
# Lints: only integer ops in the kernel TUs (02 §5.8) and a 32-bit-only Slang twin (03 §5.5a).
# ---------------------------------------------------------------------------------------------
if(HELIOS_BUILD_TESTS)
  add_test(NAME pcg_lint_kernels
           COMMAND ${CMAKE_COMMAND} -DMODE=kernels -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/src/kernels
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lint_pcg.cmake)
  add_test(NAME pcg_lint_twin
           COMMAND ${CMAKE_COMMAND} -DMODE=twin -DDIR=${PROJECT_SOURCE_DIR}/shaders/pcg
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lint_pcg.cmake)
  set_tests_properties(pcg_lint_kernels pcg_lint_twin PROPERTIES LABELS lint)
endif()

# ---------------------------------------------------------------------------------------------
# Tests. pcg_tests: CPU kernels, graph compiler, corpus (tests/corpus/hnoise). pcg_gpu_tests: the Slang
# twin on the GPU (lavapipe in CI; label gpu).
# ---------------------------------------------------------------------------------------------
set(HELIOS_HNOISE_CORPUS_DIR ${PROJECT_SOURCE_DIR}/tests/corpus/hnoise)
helios_test(pcg_tests
  SOURCES
    tests/main.cpp
    tests/corpus.cpp
    tests/test_hnoise.cpp
    tests/test_graph.cpp
    tests/test_kernels.cpp
    tests/test_corpus.cpp
    tests/test_perf.cpp
  DEPS helios::pcg helios::tp::headers helios::tp::yyjson)
if(TARGET pcg_tests)
  target_compile_definitions(pcg_tests PRIVATE HELIOS_HNOISE_CORPUS_DIR="${HELIOS_HNOISE_CORPUS_DIR}")
  set_tests_properties(pcg_tests PROPERTIES TIMEOUT 600)
endif()

if(HELIOS_BUILD_GRAPHICS)
  include(HeliosShaders)
  helios_test(pcg_gpu_tests
    SOURCES tests/main.cpp tests/corpus.cpp tests/gpu_twin.cpp tests/test_gpu.cpp
    DEPS helios::pcg helios::rhi helios::tp::yyjson)
  if(TARGET pcg_gpu_tests)
    helios_shaders(pcg_gpu_tests NAMESPACE pcg_twin_shaders FILES ${PROJECT_SOURCE_DIR}/shaders/pcg/hnoise_tile.slang)
    target_compile_definitions(pcg_gpu_tests PRIVATE HELIOS_HNOISE_CORPUS_DIR="${HELIOS_HNOISE_CORPUS_DIR}"
                                                      HELIOS_PCG_TWIN_SHADERS_HEADER="pcg_gpu_tests_shaders.h")
    # The whole suite needs a Vulkan device: it runs under the gpu label (lavapipe: ctest -L gpu).
    set_tests_properties(pcg_gpu_tests PROPERTIES LABELS gpu TIMEOUT 900)
    # The compiled twin must not need shaderInt64 or floats (spirv-dis from SPIRV-Tools; the Linux CI
    # images install it, elsewhere the check is skipped like shaderc.spirv-val).
    find_program(HELIOS_SPIRV_DIS_EXE spirv-dis)
    if(HELIOS_SPIRV_DIS_EXE AND NOT CMAKE_CROSSCOMPILING)
      add_test(NAME pcg_lint_twin_spirv
               COMMAND ${CMAKE_COMMAND} -DMODE=spirv -DSPV=${CMAKE_CURRENT_BINARY_DIR}/pcg_gpu_tests_shaders/hnoise_tile.spv
                       -DDIS=${HELIOS_SPIRV_DIS_EXE} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lint_pcg.cmake)
      set_tests_properties(pcg_lint_twin_spirv PROPERTIES LABELS "lint;shaders")
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------------------------
# WP-0.9c throughput spike (03 §5.5a): CPU ms per tile per core per kernel, the kernel-twin hashes,
# and (graphics builds) GPU tiles per 0.8 ms on the Vulkan adapter. See docs/adr/ADR-0.9c-*.md.
# ---------------------------------------------------------------------------------------------
set(HELIOS_PCG_BENCH_SOURCES bench/hnoise_bench.cpp)
set(HELIOS_PCG_BENCH_DEPS helios::pcg)
if(HELIOS_BUILD_GRAPHICS)
  list(APPEND HELIOS_PCG_BENCH_SOURCES tests/gpu_twin.cpp)
  list(APPEND HELIOS_PCG_BENCH_DEPS helios::rhi)
endif()
helios_executable(pcg_hnoise_bench ROLE bench SOURCES ${HELIOS_PCG_BENCH_SOURCES} DEPS ${HELIOS_PCG_BENCH_DEPS})
set_target_properties(pcg_hnoise_bench PROPERTIES FOLDER bench)
target_include_directories(pcg_hnoise_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
if(HELIOS_BUILD_GRAPHICS)
  include(HeliosShaders)
  helios_shaders(pcg_hnoise_bench NAMESPACE pcg_twin_shaders FILES ${PROJECT_SOURCE_DIR}/shaders/pcg/hnoise_tile.slang)
  target_compile_definitions(pcg_hnoise_bench PRIVATE HELIOS_PCG_BENCH_GPU=1
                                                  HELIOS_PCG_TWIN_SHADERS_HEADER="pcg_hnoise_bench_shaders.h")
endif()
if(HELIOS_BUILD_TESTS)
  # Smoke: a few tiles per kernel, twin hashes compared; timings are printed, not asserted.
  add_test(NAME pcg_hnoise_bench_smoke COMMAND pcg_hnoise_bench --tiles=2 --no-gpu --allow-non-avx2)
  set_tests_properties(pcg_hnoise_bench_smoke PROPERTIES LABELS bench TIMEOUT 300)
endif()
