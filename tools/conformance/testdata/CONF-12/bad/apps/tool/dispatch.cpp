extern "C" void* resolveCopy();
void copy() __attribute__((ifunc("resolveCopy")));
__attribute__((target_clones("avx2", "default"))) int sum(const int* p, int n);
