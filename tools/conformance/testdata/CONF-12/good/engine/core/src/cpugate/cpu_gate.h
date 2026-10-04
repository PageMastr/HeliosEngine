#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HELIOS_CPU_REQUIRE \
    (1u | 2u)
typedef struct HeliosCpuGateReport {
    int supported;
} HeliosCpuGateReport;
enum { HELIOS_CPU_AVX2 = 1 };
int helios_cpu_gate_run(void);
int helios_cpu_gate_verdict(HeliosCpuGateReport* out);
#ifdef __cplusplus
}
#endif
