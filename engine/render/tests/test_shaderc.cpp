// helios-shaderc end to end (the real executable, as the build and the asset processor run it):
// SPIR-V byte-identical to the build's slangc invocation, .hsr equal to reflecting that SPIR-V,
// JSONC, depfiles, defines, entry-point selection, debug info, spirv-val (required / optional /
// catching invalid modules), --reflect-spirv, --dump and error handling.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "helios/core/fs.h"
#include "helios/render/shader_reflection.h"
#include "tool_process.h"
#include "render_tests_shaders.h"

using namespace helios;
using namespace helios::render;

namespace {

struct Temp {
    std::filesystem::path dir;
    Temp() {
        auto d = fs::createUniqueTempDirectory("shaderc-test");
        REQUIRE(d.ok());
        dir = d.value();
    }
    ~Temp() { (void)fs::removeAll(dir); }
    std::filesystem::path write(std::string_view name, std::string_view text) const {
        const std::filesystem::path p = dir / name;
        REQUIRE(fs::writeTextFile(p, text).ok());
        return p;
    }
};

shaderc::ProcessResult runTool(std::vector<std::string> args) {
    args.insert(args.begin(), HELIOS_SHADERC_EXE);
    auto r = shaderc::runProcess(args);
    REQUIRE(r.ok());
    return std::move(r).value();
}

std::vector<u8> readBytes(const std::filesystem::path& p) {
    auto bytes = fs::readFile(p);
    REQUIRE_MESSAGE(bytes.ok(), fs::pathToUtf8(p));
    return std::move(bytes).value();
}

std::string str(const std::filesystem::path& p) { return fs::pathToUtf8(p); }

// Tool stdout is a text stream: on Windows its lines end in CRLF.
std::string withLf(std::string s) {
    std::erase(s, '\r');
    return s;
}

bool spirvValAvailable() {
    auto r = shaderc::runProcess({"spirv-val", "--version"});
    return r.ok() && r->ok();
}

constexpr std::string_view kVariantShader = R"(
#ifndef WG
#define WG 8
#endif
struct Push { uint* data; uint count; };
[[vk::push_constant]] ConstantBuffer<Push> gPush;
[shader("compute")]
[numthreads(WG, 1, 1)]
void csA(uint3 id : SV_DispatchThreadID) { if (id.x < gPush.count) gPush.data[id.x] = id.x; }
[shader("compute")]
[numthreads(4, 4, 1)]
void csB(uint3 id : SV_DispatchThreadID) { if (id.x < gPush.count) gPush.data[id.x] += 1u; }
)";

} // namespace

TEST_CASE("shaderc: output is byte-identical to the build's slangc and reflects the same") {
    Temp t;
    const std::filesystem::path spv = t.dir / "out" / "reflect_compute.spv";
    const std::filesystem::path src = std::filesystem::path(HELIOS_RENDER_GOLDEN_DIR).parent_path() / "shaders" / "reflect_compute.slang";
    REQUIRE(fs::createDirectories(spv.parent_path()).ok());
    const shaderc::ProcessResult r = runTool({str(src), "-I", HELIOS_SHADER_ROOT, "-o", str(spv), "--jsonc",
                                              str(t.dir / "rc.jsonc"), "--depfile", str(t.dir / "rc.d")});
    INFO(r.output);
    REQUIRE(r.ok());
    CHECK(r.output.find("csMain (compute)") != std::string::npos);
    const std::vector<u8> bytes = readBytes(spv);
    const std::span<const u32> embedded = render_test_shaders::reflect_compute();
    REQUIRE(bytes.size() == embedded.size_bytes());
    CHECK(std::memcmp(bytes.data(), embedded.data(), bytes.size()) == 0);

    // .hsr next to the output (default name) == reflecting the SPIR-V in process.
    auto hsr = parseReflection(readBytes(t.dir / "out" / "reflect_compute.hsr"));
    REQUIRE(hsr.ok());
    CHECK(hsr.value() == reflectSpirv(embedded).value());
    auto jsonc = fs::readTextFile(t.dir / "rc.jsonc");
    REQUIRE(jsonc.ok());
    CHECK(jsonc.value() == reflectionToJsonc(hsr.value()));
    auto dep = fs::readTextFile(t.dir / "rc.d");
    REQUIRE(dep.ok());
    CHECK(dep->find("reflect_compute.spv:") != std::string::npos);
    CHECK(dep->find("reflect_compute.slang") != std::string::npos);
    CHECK(dep->find("core/bindless.slang") != std::string::npos);  // imported module

    // --dump prints the JSONC of a blob; --reflect-spirv reflects an existing module.
    const shaderc::ProcessResult dump = runTool({"--dump", str(t.dir / "out" / "reflect_compute.hsr")});
    REQUIRE(dump.ok());
    CHECK(withLf(dump.output) == reflectionToJsonc(hsr.value()));
    const shaderc::ProcessResult again = runTool({"--reflect-spirv", str(spv), "--hsr", str(t.dir / "again.hsr")});
    INFO(again.output);
    REQUIRE(again.ok());
    CHECK(readBytes(t.dir / "again.hsr") == readBytes(t.dir / "out" / "reflect_compute.hsr"));
}

TEST_CASE("shaderc: defines, entry-point selection, debug info and --no-hsr") {
    Temp t;
    const std::filesystem::path src = t.write("variant.slang", kVariantShader);
    REQUIRE(runTool({str(src), "-o", str(t.dir / "a.spv"), "-q"}).ok());
    REQUIRE(runTool({str(src), "-DWG=32", "-o", str(t.dir / "b.spv"), "-q"}).ok());
    REQUIRE(runTool({str(src), "--entry", "csB", "-o", str(t.dir / "c.spv"), "-q"}).ok());
    REQUIRE(runTool({str(src), "-g", "-o", str(t.dir / "d.spv"), "-q", "--no-hsr"}).ok());
    const ShaderReflection a = parseReflection(readBytes(t.dir / "a.hsr")).value();
    const ShaderReflection b = parseReflection(readBytes(t.dir / "b.hsr")).value();
    const ShaderReflection c = parseReflection(readBytes(t.dir / "c.hsr")).value();
    REQUIRE(a.entryPoints.size() == 2);
    CHECK(a.findEntryPoint("csA")->workgroupSize[0] == 8);
    CHECK(b.findEntryPoint("csA")->workgroupSize[0] == 32);
    REQUIRE(c.entryPoints.size() == 1);
    CHECK(c.entryPoints[0].name == "csB");
    CHECK(c.entryPoints[0].workgroupSize == std::array<u32, 3>{4, 4, 1});
    CHECK(a.pushConstants.size == 12);
    CHECK(!fs::exists(t.dir / "d.hsr"));
    CHECK(readBytes(t.dir / "d.spv").size() > readBytes(t.dir / "a.spv").size());  // debug info
}

TEST_CASE("shaderc: compile errors, usage errors and missing entry points fail cleanly") {
    Temp t;
    const std::filesystem::path bad = t.write("bad.slang", "[shader(\"compute\")] [numthreads(1,1,1)] void cs() { undefined_thing(); }\n");
    const shaderc::ProcessResult r = runTool({str(bad), "-o", str(t.dir / "bad.spv")});
    CHECK(r.status != 0);
    CHECK(r.output.find("undefined_thing") != std::string::npos);
    CHECK(r.output.find("failed to compile") != std::string::npos);
    CHECK(!fs::exists(t.dir / "bad.spv"));

    const std::filesystem::path src = t.write("variant.slang", kVariantShader);
    const shaderc::ProcessResult missing = runTool({str(src), "--entry", "nope", "-o", str(t.dir / "x.spv")});
    CHECK(missing.status != 0);
    CHECK(missing.output.find("no entry point 'nope'") != std::string::npos);

    const shaderc::ProcessResult usage = runTool({str(src)});
    CHECK(usage.status != 0);
    CHECK(usage.output.find("-o <output.spv> are required") != std::string::npos);
    const shaderc::ProcessResult unknown = runTool({"--frobnicate"});
    CHECK(unknown.status != 0);
    CHECK(unknown.output.find("unknown option '--frobnicate'") != std::string::npos);
    const shaderc::ProcessResult help = runTool({"--help"});
    CHECK(help.ok());
    CHECK(help.output.find("usage: helios-shaderc") != std::string::npos);
    const shaderc::ProcessResult noFile = runTool({str(t.dir / "missing.slang"), "-o", str(t.dir / "m.spv")});
    CHECK(noFile.status != 0);
    const shaderc::ProcessResult badDump = runTool({"--dump", str(src)});
    CHECK(badDump.status != 0);
    CHECK(badDump.output.find("bad magic") != std::string::npos);
}

TEST_CASE("shaderc: spirv-val is run when available, can be required, and rejects invalid modules") {
    Temp t;
    const std::filesystem::path src = t.write("variant.slang", kVariantShader);
    // A validator that does not exist: optional by default, fatal with --validate on.
    const shaderc::ProcessResult optional =
        runTool({str(src), "-o", str(t.dir / "a.spv"), "--spirv-val", str(t.dir / "no-such-spirv-val")});
    INFO(optional.output);
    CHECK(optional.ok());
    CHECK(optional.output.find("validation skipped") != std::string::npos);
    const shaderc::ProcessResult required = runTool(
        {str(src), "-o", str(t.dir / "b.spv"), "--validate", "on", "--spirv-val", str(t.dir / "no-such-spirv-val")});
    CHECK(required.status != 0);
    CHECK(required.output.find("not available") != std::string::npos);
    CHECK(!fs::exists(t.dir / "b.spv"));
    CHECK(runTool({str(src), "-o", str(t.dir / "c.spv"), "--validate", "off"}).ok());

    if (!spirvValAvailable()) {
        MESSAGE("spirv-val not installed: skipping the validator checks");
        return;
    }
    REQUIRE(runTool({str(src), "-o", str(t.dir / "d.spv"), "--validate", "on"}).ok());
    // Corrupt the module in a way reflection tolerates but the validator does not: a SPIR-V
    // version that does not exist (1.7).
    std::vector<u8> bytes = readBytes(t.dir / "d.spv");
    const u32 badVersion = 0x00010700u;
    std::memcpy(bytes.data() + 4, &badVersion, 4);
    REQUIRE(reflectSpirvBytes(bytes).ok());
    REQUIRE(fs::writeFile(t.dir / "broken.spv", bytes).ok());
    const shaderc::ProcessResult rejected = runTool({"--reflect-spirv", str(t.dir / "broken.spv"), "--validate", "on"});
    CHECK(rejected.status != 0);
    CHECK(rejected.output.find("spirv-val rejected") != std::string::npos);
}

TEST_CASE("shaderc: argument quoting survives spaces and quotes") {
    Temp t;
    const std::filesystem::path dir = t.dir / "with space's dir";  // quotes are not valid in Windows names
    REQUIRE(fs::createDirectories(dir).ok());
    const std::filesystem::path src = dir / "shader file.slang";
    REQUIRE(fs::writeTextFile(src, kVariantShader).ok());
    const shaderc::ProcessResult r = runTool({str(src), "-o", str(dir / "out file.spv"), "-q"});
    INFO(r.output);
    CHECK(r.ok());
    CHECK(fs::exists(dir / "out file.hsr"));
}

TEST_CASE("shaderc: shell metacharacters and non-ASCII names reach the tools verbatim") {
    // helios-shaderc and spirv-val are started without a shell (core process API), so nothing in
    // a path can expand ($HOME, %PATH%), chain commands (&, ;) or substitute (`...`).
    Temp t;
    const std::filesystem::path dir = t.dir / fs::pathFromUtf8("odd $HOME & %PATH% ; `echo x` \xc3\xa9t\xc3\xa9");
    REQUIRE(fs::createDirectories(dir).ok());
    const std::filesystem::path src = dir / fs::pathFromUtf8("sh\xc3\xa4" "der $(id).slang");
    REQUIRE(fs::writeTextFile(src, kVariantShader).ok());
    const std::filesystem::path spv = dir / "out;rm -rf x.spv";
    const shaderc::ProcessResult r = runTool({str(src), "-o", str(spv), "--depfile", str(dir / "out.d")});
    INFO(r.output);
    REQUIRE(r.ok());
    CHECK(fs::exists(spv));
    CHECK(fs::exists(dir / "out;rm -rf x.hsr"));
    auto dep = fs::readTextFile(dir / "out.d");
    REQUIRE(dep.ok());
    CHECK(dep->find("$$HOME") != std::string::npos);  // '$' escaped for make/ninja
    if (spirvValAvailable()) {
        const shaderc::ProcessResult v = runTool({"--reflect-spirv", str(spv), "--validate", "on", "--no-hsr"});
        INFO(v.output);
        CHECK(v.ok());
    }
}
