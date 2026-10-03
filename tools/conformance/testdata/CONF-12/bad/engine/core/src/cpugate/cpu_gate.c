#include "cpu_gate.h"
#include <string.h>

typedef struct HcgText { char* buf; } HcgText;

int helios_cpu_gate_run(void) { return 1; }
unsigned hcg_features(void) { return 0; }
extern int hcg_declared;
extern void hcg_declared_fn(void);
extern void hcg_extra(void) { }
extern int hcg_extra_data = 5;
static int hcg_private = 1;
int hcg_a = 1, hcg_b = 2;
