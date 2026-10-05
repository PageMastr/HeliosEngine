#include "../../cpugate/cpu_gate.h"
#include <signal.h>
#include <unistd.h>

static void hcg_gate(int argc, char** argv, char** envp) { (void)argc; (void)argv; (void)envp; }
__attribute__((section(".preinit_array"), used)) static void (*hcg_preinit_entry)(int, char**, char**) = hcg_gate;
__attribute__((constructor(101))) static void hcg_constructor(void) { hcg_gate(0, 0, 0); }
