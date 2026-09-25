#include <doctest/doctest.h>

#include <string>

#include "helios/core/cpu.h"
#include "helios/core/fs.h"
#include "helios/core/process.h"

#ifndef HELIOS_CPUGATE_CHILD_PATH
#error "HELIOS_CPUGATE_CHILD_PATH must be defined by the build"
#endif

using namespace helios;

namespace {

constexpr u32 kIntel[3] = {0x756e6547u, 0x6c65746eu, 0x49656e69u}; // EBX ECX EDX: "GenuineIntel"
constexpr u32 kAmd[3] = {0x68747541u, 0x444d4163u, 0x69746e65u};   // "AuthenticAMD"

CpuidSnapshot base(const u32 vendor[3], u32 maxLeaf, u32 eax1, u32 ecx1, u32 edx1, u32 ebx7, u32 ecxExt,
                   u64 xcr0, std::array<u32, 12> brand) {
    CpuidSnapshot s;
    s.leaf0 = {maxLeaf, vendor[0], vendor[1], vendor[2]};
    s.leaf1 = {eax1, 0, ecx1, edx1};
    s.leaf7 = {0, ebx7, 0, 0};
    s.ext0 = {0x80000008u, 0, 0, 0};
    s.ext1 = {0, 0, ecxExt, 0};
    s.brand = brand;
    s.xcr0 = xcr0;
    return s;
}

// Recorded CPUID values (leaf 1 EAX/ECX/EDX, leaf 7 EBX, 0x80000001 ECX).
CpuidSnapshot haswell() { // Core i7-4770
    return base(kIntel, 0xd, 0x000306c3u, 0x7ffafbffu, 0xbfebfbffu, 0x000027abu, 0x00000021u, 0x7,
                {0x65746e49, 0x2952286c, 0x726f4320, 0x4d542865, 0x37692029, 0x3737342d, 0x50432030, 0x20402055,
                 0x30342e33, 0x007a4847, 0, 0});
}
CpuidSnapshot sandyBridge() { // Core i7-2600K: AVX but no AVX2, BMI, F16C or LZCNT
    return base(kIntel, 0xd, 0x000206a7u, 0x1fbae3ffu, 0xbfebfbffu, 0, 0x00000001u, 0x7,
                {0x65746e49, 0x2952286c, 0x726f4320, 0x4d542865, 0x37692029, 0x3036322d, 0x43204b30, 0x40205550,
                 0x342e3320, 0x7a484730, 0, 0});
}
CpuidSnapshot nehalem() { // Core i7 920: x86-64-v2, no AVX, no XSAVE
    return base(kIntel, 0xb, 0x000106a5u, 0x0098e3bdu, 0xbfebfbffu, 0, 0x00000001u, 0,
                {0x65746e49, 0x2952286c, 0x726f4320, 0x4d542865, 0x37692029, 0x55504320, 0x20202020, 0x20202020,
                 0x30323920, 0x20402020, 0x37362e32, 0x007a4847});
}
CpuidSnapshot core2() { // Core 2 Duo E8400: SSE4.1 but no SSE4.2 or POPCNT (pre-v2)
    return base(kIntel, 0xd, 0x0001067au, 0x0408e3fdu, 0xbfebfbffu, 0, 0x00000001u, 0,
                {0x65746e49, 0x2952286c, 0x726f4320, 0x4d542865, 0x44203229, 0x43206f75, 0x20205550, 0x45202020,
                 0x30303438, 0x20402020, 0x30302e33, 0x007a4847});
}
CpuidSnapshot zen2() { // Ryzen 5 3600: brand fills all 48 bytes (no terminator), leading spaces
    return base(kAmd, 0x10, 0x00870f10u, 0x7ed8320bu, 0x178bfbffu, 0x219c91a9u, 0x75c237ffu, 0x7,
                {0x20202020, 0x41202020, 0x5220444d, 0x6e657a79, 0x33203520, 0x20303036, 0x6f432d36, 0x50206572,
                 0x65636f72, 0x726f7373, 0x20202020, 0x20202020});
}

bool contains(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

TEST_CASE("cpu gate: requirement sets") {
    const CpuFeatureSet v2 = cpuRequiredFeatures(CpuRequirement::X86_64_V2);
    const CpuFeatureSet avx2 = cpuRequiredFeatures(CpuRequirement::Avx2Image);
    CHECK(avx2.containsAll(v2));
    for (CpuFeature f : {CpuFeature::AVX, CpuFeature::AVX2, CpuFeature::BMI1, CpuFeature::BMI2, CpuFeature::F16C,
                         CpuFeature::LZCNT, CpuFeature::POPCNT, CpuFeature::SSE42}) {
        CHECK(avx2.has(f));
    }
    CHECK(!avx2.has(CpuFeature::FMA)); // reported, never required (no FMA codegen, determinism)
    CHECK(!avx2.has(CpuFeature::AVX512F));
    CHECK(cpuFeatureName(CpuFeature::SSE42) == "SSE4.2");
    CHECK(cpuFeatureName(CpuFeature::Count) == "?");
    CpuFeatureSet s;
    s.add(CpuFeature::AVX2).add(CpuFeature::BMI2);
    CHECK(s.toString() == "AVX2, BMI2");
    CHECK(CpuFeatureSet{}.toString().empty());
}

TEST_CASE("cpu gate: Haswell passes, with FMA reported") {
    const CpuGateReport r = cpuGateEvaluate(haswell());
    CHECK(r.isX86);
    CHECK(r.supported);
    CHECK(r.missing.empty());
    CHECK(r.osAvxState);
    CHECK(!r.osAvx512State);
    CHECK(r.usable.has(CpuFeature::FMA));
    CHECK(r.usable.has(CpuFeature::MOVBE));
    CHECK(r.vendor == "GenuineIntel");
    CHECK(r.brand == "Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz");
    CHECK(r.family == 6);
    CHECK(r.model == 0x3c);
    CHECK(r.stepping == 3);
    CHECK(contains(r.message, "CPU supported: Intel(R) Core(TM) i7-4770"));
    CHECK(contains(r.message, "AVX2"));
}

TEST_CASE("cpu gate: Sandy Bridge is refused with the 08 §2.2 message") {
    const CpuGateReport r = cpuGateEvaluate(sandyBridge(), CpuRequirement::Avx2Image, "Cinder Test");
    CHECK(!r.supported);
    CHECK(r.osAvxState);
    CHECK(r.usable.has(CpuFeature::AVX));
    CHECK(r.missing.toString() == "AVX2, BMI1, BMI2, F16C, LZCNT");
    CHECK(r.message ==
          "Cinder Test requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: Intel(R) Core(TM) "
          "i7-2600K CPU @ 3.40GHz. Missing: AVX2, BMI1, BMI2, F16C, LZCNT.");
    // The same CPU passes the bootstrap's x86-64-v2 floor.
    CHECK(cpuGateEvaluate(sandyBridge(), CpuRequirement::X86_64_V2).supported);
}

TEST_CASE("cpu gate: a long display name is truncated on a UTF-8 boundary") {
    std::string name(62, 'a');
    name += "\xc3\xa9\xc3\xa9"; // "éé": the 63-byte cut would split the first 'é'
    const CpuGateReport r = cpuGateEvaluate(sandyBridge(), CpuRequirement::Avx2Image, name);
    CHECK(r.message.rfind(std::string(62, 'a') + " requires an AVX2 CPU", 0) == 0);
}

TEST_CASE("cpu gate: AVX without OS support (XSAVE/YMM state) is not usable") {
    CpuidSnapshot noYmm = haswell();
    noYmm.xcr0 = 0x3; // x87 + SSE only
    CpuGateReport r = cpuGateEvaluate(noYmm);
    CHECK(!r.supported);
    CHECK(!r.osAvxState);
    CHECK(r.detected.has(CpuFeature::AVX2));
    CHECK(!r.usable.has(CpuFeature::AVX2));
    CHECK(r.missing.toString() == "AVX, AVX2, F16C");
    CHECK(contains(r.message, "operating system has not enabled AVX"));

    CpuidSnapshot noOsxsave = haswell();
    noOsxsave.leaf1[2] &= ~(1u << 27);
    r = cpuGateEvaluate(noOsxsave);
    CHECK(!r.supported);
    CHECK(!r.osAvxState);
    CHECK(!r.detected.has(CpuFeature::OSXSAVE));
}

TEST_CASE("cpu gate: AVX-512 state") {
    CpuidSnapshot s = haswell();
    s.leaf7[1] |= 1u << 16; // AVX512F
    s.xcr0 = 0x7;
    CpuGateReport r = cpuGateEvaluate(s);
    CHECK(r.detected.has(CpuFeature::AVX512F));
    CHECK(!r.usable.has(CpuFeature::AVX512F));
    s.xcr0 = 0xe7;
    r = cpuGateEvaluate(s);
    CHECK(r.osAvx512State);
    CHECK(r.usable.has(CpuFeature::AVX512F));
}

TEST_CASE("cpu gate: older CPUs and the v2 floor") {
    CpuGateReport r = cpuGateEvaluate(nehalem());
    CHECK(!r.supported);
    CHECK(!r.osAvxState);
    CHECK(r.missing.has(CpuFeature::AVX2));
    CHECK(cpuGateEvaluate(nehalem(), CpuRequirement::X86_64_V2).supported);

    r = cpuGateEvaluate(core2(), CpuRequirement::X86_64_V2);
    CHECK(!r.supported);
    CHECK(r.missing.toString() == "SSE4.2, POPCNT");
    CHECK(contains(r.message, "requires an x86-64-v2 CPU"));
    CHECK(contains(r.message, "Core(TM)2 Duo CPU     E8400"));
}

TEST_CASE("cpu gate: AMD Zen 2, brand without terminator, LZCNT from the extended leaf") {
    const CpuGateReport r = cpuGateEvaluate(zen2());
    CHECK(r.supported);
    CHECK(r.vendor == "AuthenticAMD");
    CHECK(r.brand == "AMD Ryzen 5 3600 6-Core Processor");
    CHECK(r.family == 0x17);
    CHECK(r.model == 0x71);
    CHECK(r.usable.has(CpuFeature::LZCNT));
    CHECK(r.usable.has(CpuFeature::LAHF));
}

TEST_CASE("cpu gate: missing brand leaves and non-x86 builds") {
    CpuidSnapshot s = haswell();
    s.ext0[0] = 0x80000001u;
    s.brand = {};
    CpuGateReport r = cpuGateEvaluate(s);
    CHECK(r.brand == "GenuineIntel family 6 model 60");

    CpuidSnapshot arm;
    arm.isX86 = false;
    r = cpuGateEvaluate(arm);
    CHECK(r.supported);
    CHECK(!r.isX86);
    CHECK(contains(r.message, "not an x86-64 build"));
}

TEST_CASE("cpu gate: the running CPU") {
    const CpuGateReport& r = cpuGate();
    CHECK(&r == &cpuGate()); // cached
    CHECK(!r.message.empty());
    CHECK(r.required == cpuRequiredFeatures(CpuRequirement::Avx2Image));
    CHECK(r.supported == r.missing.empty());
    CHECK((r.usable.bits & ~r.detected.bits) == 0);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(r.isX86);
    CHECK(!r.vendor.empty());
    CHECK(!r.brand.empty());
    CHECK(r.detected.has(CpuFeature::SSE2)); // architectural on x86-64
    MESSAGE("running CPU: " << r.message);
#endif
    const CpuGateReport named = cpuGate(CpuRequirement::X86_64_V2, "Probe");
    CHECK(named.required == cpuRequiredFeatures(CpuRequirement::X86_64_V2));
}

TEST_CASE("cpu gate: the pre-initializer runs before C++ static initialization") {
    // Refusal path, with the hook evaluating a recorded Sandy Bridge: message on stderr, exit 78,
    // and neither the C++ static initializer nor main() ran.
    ProcessDesc snb;
    snb.executable = fs::pathFromUtf8(HELIOS_CPUGATE_CHILD_SNB_PATH);
    Result<ProcessOutput> refused = runProcess(snb);
    REQUIRE(refused.ok());
    CHECK(refused->exitCode == kCpuGateExitCode);
    CHECK(refused->out.empty());
    CHECK(contains(refused->err, "Helios requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: "
                                 "Intel(R) Core(TM) i7-2600K CPU @ 3.40GHz. Missing: AVX2, BMI1, BMI2, F16C, LZCNT."));

    // Live CPU: passes wherever this suite runs with AVX2; the process then starts normally.
    ProcessDesc live;
    live.executable = fs::pathFromUtf8(HELIOS_CPUGATE_CHILD_PATH);
    Result<ProcessOutput> ran = runProcess(live);
    REQUIRE(ran.ok());
    if (cpuGate().supported) {
        CHECK(ran->exitCode == 0);
        const std::vector<std::string> lines = linesOf(ran->out);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "static-init");
        CHECK(contains(lines[1], "main reached: CPU supported"));
    } else {
        CHECK(ran->exitCode == kCpuGateExitCode);
    }
}

TEST_CASE("cpu gate: illegal-instruction backstop") {
    if (!cpuGate().supported) return; // the gate itself refuses first
    ProcessDesc d;
    d.executable = fs::pathFromUtf8(HELIOS_CPUGATE_CHILD_PATH);
    d.args = {"--sigill"};
    Result<ProcessOutput> r = runProcess(d);
    REQUIRE(r.ok());
    if (contains(r->out, "sigill-unsupported")) {
        MESSAGE("no inline assembly on this compiler: invalid-opcode backstop not exercised");
    } else {
        CHECK(r->exitCode == kCpuGateExitCode);
        CHECK(contains(r->err, "Helios stopped: illegal instruction"));
    }
}

TEST_CASE("cpu gate: deliberate traps (ud2) are not reported as an unsupported CPU") {
    // Regression: the backstop turned every SIGILL / STATUS_ILLEGAL_INSTRUCTION into "the CPU may
    // lack AVX2" and exit code 78, including __builtin_trap() and clang-cl's trap-on-unreachable,
    // which hid real crashes from the crash handler.
    if (!cpuGate().supported) return;
    ProcessDesc d;
    d.executable = fs::pathFromUtf8(HELIOS_CPUGATE_CHILD_PATH);
    d.args = {"--trap"};
    d.stdoutMode = StdioMode::Null;
    Result<ProcessOutput> r = runProcess(d);
    REQUIRE(r.ok());
    CHECK(r->exitCode != 0);
    CHECK(r->exitCode != kCpuGateExitCode);
    CHECK(!contains(r->err, "Helios stopped: illegal instruction"));
#if !defined(_WIN32)
    CHECK(r->exitCode == 128 + 4); // killed by SIGILL, as without the gate
#endif
}
