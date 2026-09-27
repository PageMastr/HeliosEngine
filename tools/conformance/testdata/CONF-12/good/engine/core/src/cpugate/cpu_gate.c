#include "cpu_gate.h"
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

static const char* const kNames[] = {"avx2", "bmi2"};
static unsigned hcg_features(void) { return 0; }
int helios_cpu_gate_run(void) { return hcg_features() != 0 && kNames[0] != 0; }
int helios_cpu_gate_verdict(HeliosCpuGateReport* out) { out->supported = helios_cpu_gate_run(); return 0; }
