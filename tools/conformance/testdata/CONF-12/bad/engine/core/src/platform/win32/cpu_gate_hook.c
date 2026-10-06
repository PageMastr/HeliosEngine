#include "../../cpugate/cpu_gate.h"
#include <stdio.h>
#include <windows.h>

int hcg_seen = 0; /* external, and not one of the three exports */

static int hcg_gate(void) {
    if (!helios_cpu_gate_run()) ExitProcess(78);
    return 0;
}
#pragma section(".CRT$XIB", long, read)
__declspec(allocate(".CRT$XIB")) int(__cdecl* const helios_cpu_gate_crt_entry)(void) = hcg_gate;
#pragma init_seg(compiler)
