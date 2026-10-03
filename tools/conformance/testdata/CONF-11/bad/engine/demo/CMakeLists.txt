# Other ways to grant AVX below the image level.
set(DEMO_AVX_KERNELS a_fast.cpp b_fast.cpp)
set(simd -mavx512f)
set_property(SOURCE a_fast.cpp PROPERTY COMPILE_OPTIONS ${simd})
set_target_properties(helios_demo PROPERTIES COMPILE_OPTIONS "-march=haswell")
string(APPEND CMAKE_CXX_FLAGS " -mfma")
target_compile_options(helios_demo PRIVATE $<$<CXX_COMPILER_ID:GNU>:-mbmi2>)
helios_avx2_sources(helios_demo src/kernel_avx2.cpp)
add_definitions(-mavx2)
