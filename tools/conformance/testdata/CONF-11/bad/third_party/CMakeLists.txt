# Verbatim from third_party/CMakeLists.txt at WP-0.2 (main 1425608; unchanged at c2cbff5), lines 84-94: tp_jolt's PUBLIC ISA options.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_compile_definitions(tp_jolt PUBLIC JPH_USE_AVX2 JPH_USE_AVX JPH_USE_SSE4_1 JPH_USE_SSE4_2
                                            JPH_USE_LZCNT JPH_USE_TZCNT JPH_USE_F16C)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(tp_jolt PUBLIC /arch:AVX2)
  elseif(MSVC) # clang-cl
    target_compile_options(tp_jolt PUBLIC /arch:AVX2 -mbmi -mpopcnt -mlzcnt -mf16c /clang:-ffp-contract=off)
  else()
    target_compile_options(tp_jolt PUBLIC -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfpmath=sse -ffp-contract=off)
  endif()
endif()
