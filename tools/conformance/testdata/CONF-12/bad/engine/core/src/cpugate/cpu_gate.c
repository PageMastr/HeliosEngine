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
struct hcg_state hcg_global_state;
struct { int a; } hcg_anon_state;
enum hcg_e { HCG_A, HCG_B } hcg_enum_var;
struct hcg_state hcg_make(void) { struct hcg_state s = {0}; return s; }
int hcg_after = 0;
char hcg_buf[sizeof(int)];
_Alignas(16) unsigned char hcg_aligned[64];
int hcg_c = 3, hcg_proto(void);
