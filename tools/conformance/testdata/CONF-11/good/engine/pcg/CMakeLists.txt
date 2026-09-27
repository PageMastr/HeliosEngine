# SSE4.2 is below AVX: not an AVX-class grant.
set_source_files_properties(src/kernels/vm_sse42.cpp TARGET_DIRECTORY helios_pcg PROPERTIES COMPILE_OPTIONS "-msse4.2")
target_compile_options(helios_pcg PRIVATE -Wall "-DDOC=a#b") # a quoted # is not a comment: -mavx2
