// WINDOWS_GUI child for the CPU-gate tests (tests/test_cpu.cpp; Windows only, engine/core/CMakeLists.txt).
// Linked with the Sandy Bridge hook, so the gate refuses it before WinMain. The executable's PE subsystem is
// WINDOWS_GUI, which makes the refusal show a message box (02 §1.1 failure path, step 2) unless
// HELIOS_CPU_GATE_SILENT=1: the test sets it, and a dialog would block the child until the test times out.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return 0; // never reached: the gate ends the process with exit code 78
}
