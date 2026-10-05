message(STATUS "ISA (avx2 kernels")
string(REGEX REPLACE "\\(.*" "" short "${v}")
target_compile_options(helios_demo PRIVATE -mavx2)
string(APPEND CMAKE_CXX_FLAGS " -march=haswell")
#[[ a bracket comment with an unbalanced (
]]
set(doc [=[ a bracket argument with an unbalanced ( ]=])
add_compile_options(-mfma)
set(paren \()
add_compile_options(-mbmi2)
