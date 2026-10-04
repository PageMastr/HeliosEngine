#include "../../cpugate/cpu_gate.h"
#include <windows.h>

typedef struct HcgState { int ran; } HcgState;
static HcgState hcg_state;

static void NTAPI hcg_tls(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH && !helios_cpu_gate_run()) TerminateProcess(GetCurrentProcess(), 78);
    hcg_state.ran = 1;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:helios_cpu_gate_tls_entry")
#pragma section(".CRT$XLA0", long, read)
__declspec(allocate(".CRT$XLA0")) const PIMAGE_TLS_CALLBACK helios_cpu_gate_tls_entry = hcg_tls;
