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
/* Types and static state: no external symbol. */
struct hcg_state;
struct hcg_state { int a; };
enum { HCG_A, HCG_B };
typedef struct { int a; } HcgPair;
static struct hcg_state hcg_saved;
static struct { int b; } hcg_anon;
static struct hcg_state hcg_make(void) { struct hcg_state s = {0}; return s; }
extern struct hcg_state hcg_elsewhere;
static char hcg_buf[sizeof(int)];
static _Alignas(16) unsigned char hcg_aligned[64];
extern char hcg_table[sizeof(long)];
/* Text in static arrays: a ';' or a brace inside a literal neither ends nor opens a statement. */
static const char hcg_txt[] = "not an x86-64 build; no requirement {applies}";
static const char hcg_semi = ';', hcg_open = '{';
