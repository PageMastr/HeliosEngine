// ISA-audit fixture (02 §1.1 audit check 4): a `base` image like the launcher, which helios_executable()
// builds at x86-64-v1 (ISA base). It links helios::core and helios::patch, so helios_isa_finalize() links
// their base copies instead (helios_core.base, helios_patch.base, and those of mimalloc, Monocypher and
// zstd). lint_isa_audit disassembles the linked image: an instruction above x86-64-v1 may appear only in a
// symbol on HELIOS_ISA_SELF_DISPATCH_SYMBOLS (zstd's BMI2 decoders, which check CPUID first). Never run
// by the tests; like the launcher, it would report the CPU and exit with 78 on one without AVX2.
#include <helios/core/cpu.h>
#include <helios/patch/blake2b.h>

int main() {
    const helios::CpuGateReport& cpu = helios::cpuGate();
    const unsigned char probe[4] = {0x68, 0x65, 0x6c, 0x73};
    const helios::patch::Hash256 hash = helios::patch::blake2b256(probe);
    if (hash.isZero()) {
        return 1;
    }
    return cpu.supported ? 0 : 78;
}
