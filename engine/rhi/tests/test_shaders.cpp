// The Slang toolchain (cmake/HeliosShaders.cmake): modules compile to valid SPIR-V, are embedded,
// keep their entry-point names and follow the layout conventions the RHI relies on.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "rhi_tests_shaders.h"

namespace {

using u32 = std::uint32_t;

struct EntryPoint {
    u32 model;
    std::string name;
};

std::vector<EntryPoint> entryPoints(std::span<const u32> words) {
    std::vector<EntryPoint> out;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        const u32 op = words[i] & 0xFFFF;
        if (count == 0) break;
        if (op == 15 /* OpEntryPoint */) {
            const char* text = reinterpret_cast<const char*>(&words[i + 3]);
            out.push_back({words[i + 1], std::string(text)});
        }
        i += count;
    }
    return out;
}

bool hasCapability(std::span<const u32> words, u32 capability) {
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        if (count == 0) break;
        if ((words[i] & 0xFFFF) == 17 /* OpCapability */ && words[i + 1] == capability) return true;
        i += count;
    }
    return false;
}

bool hasEntry(const std::vector<EntryPoint>& eps, u32 model, std::string_view name) {
    return std::any_of(eps.begin(), eps.end(), [&](const EntryPoint& e) { return e.model == model && e.name == name; });
}

} // namespace

TEST_CASE("shaders: every module is embedded SPIR-V 1.6") {
    const auto names = rhi_test_shaders::names();
    REQUIRE(names.size() >= 6);
    for (std::string_view name : names) {
        INFO("module " << name);
        const auto words = rhi_test_shaders::find(name);
        REQUIRE(words.size() > 5);
        CHECK(words[0] == 0x07230203u);  // magic
        CHECK(words[1] == 0x00010600u);  // SPIR-V 1.6
        CHECK_FALSE(entryPoints(words).empty());
    }
    CHECK(rhi_test_shaders::find("does_not_exist").empty());
}

TEST_CASE("shaders: find() and names() use the file stem, accessors its C identifier") {
    const auto names = rhi_test_shaders::names();
    CHECK(std::find(names.begin(), names.end(), std::string_view("dash-name")) != names.end());
    CHECK(rhi_test_shaders::find("dash-name").data() == rhi_test_shaders::dash_name().data());
    CHECK_FALSE(rhi_test_shaders::dash_name().empty());
    CHECK(rhi_test_shaders::find("dash_name").empty());
}

TEST_CASE("shaders: entry points keep their source names, several per module") {
    const auto triangle = entryPoints(rhi_test_shaders::triangle());
    CHECK(hasEntry(triangle, 0 /* Vertex */, "vsMain"));
    CHECK(hasEntry(triangle, 4 /* Fragment */, "psMain"));
    const auto compute = entryPoints(rhi_test_shaders::compute_buffers());
    CHECK(hasEntry(compute, 5 /* GLCompute */, "csWriteAddress"));
    CHECK(hasEntry(compute, 5, "csWriteBindless"));
    CHECK(hasEntry(compute, 5, "csIncrement"));
}

TEST_CASE("shaders: bindless and device-address code use the expected capabilities") {
    // PhysicalStorageBufferAddresses (5347): pointers in push constants lower to BDA.
    CHECK(hasCapability(rhi_test_shaders::compute_buffers(), 5347));
    CHECK(hasCapability(rhi_test_shaders::matrix(), 5347));
    // RuntimeDescriptorArray (5302): the bindless heap arrays are unsized.
    CHECK(hasCapability(rhi_test_shaders::textured_quad(), 5302));
}

TEST_CASE("shaders: the matrix push block uses column-major scalar layout") {
    // Scan OpMemberDecorate (72) for member 0 of the push block: RowMajor (4) in SPIR-V terms is
    // what Slang emits for -matrix-layout-column-major (its rows are our columns), MatrixStride 16,
    // and member 1 (float4 v) at offset 64.
    const auto words = rhi_test_shaders::matrix();
    bool rowMajorDecoration = false;
    bool stride16 = false;
    bool vAt64 = false;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        if (count == 0) break;
        if ((words[i] & 0xFFFF) == 72 && count >= 4) {
            const u32 member = words[i + 2];
            const u32 decoration = words[i + 3];
            if (member == 0 && decoration == 4) rowMajorDecoration = true;
            if (member == 0 && decoration == 7 && count >= 5 && words[i + 4] == 16) stride16 = true;
            if (member == 1 && decoration == 35 && count >= 5 && words[i + 4] == 64) vAt64 = true;
        }
        i += count;
    }
    CHECK(rowMajorDecoration);
    CHECK(stride16);
    CHECK(vAt64);
}
