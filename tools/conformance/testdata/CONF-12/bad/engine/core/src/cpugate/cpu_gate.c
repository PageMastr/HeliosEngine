#include "cpu_gate.h"
#include <string.h>

typedef struct HcgText { char* buf; } HcgText;

int helios_cpu_gate_run(void) { return 1; }
unsigned hcg_features(void) { return 0; }
