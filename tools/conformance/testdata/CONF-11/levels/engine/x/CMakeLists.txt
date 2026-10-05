set_source_files_properties(src/fast.cpp PROPERTIES COMPILE_OPTIONS "${HELIOS_ISA_AVX2}")
target_compile_options(helios_x PRIVATE ${HELIOS_ISA_AVX2})
helios_isa_flags_for(avx2 xflags)
target_compile_options(helios_x2 PRIVATE ${xflags})
