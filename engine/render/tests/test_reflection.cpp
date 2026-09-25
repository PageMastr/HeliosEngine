// Shader reflection: SPIR-V produced by the pinned Slang (embedded by helios_shaders) is reflected
// into entry points, push constants, specialization constants and bindings; the .hsr binary round
// trips, corrupt input is rejected, and the JSONC rendering carries the same data.

#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <cstring>

#include "helios/core/random.h"
#include "helios/render/shader_reflection.h"
#include "render_tests_shaders.h"

using namespace helios;
using namespace helios::render;

namespace {

ShaderReflection reflect(std::span<const u32> words) {
    auto r = reflectSpirv(words);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    return std::move(r).value();
}

} // namespace

TEST_CASE("reflection: compute module (push constants, specialization constants, bindings)") {
    const ShaderReflection r = reflect(render_test_shaders::reflect_compute());
    CHECK(r.spirvVersion == 0x00010600u);
    CHECK(r.contentHash == hash128(render_test_shaders::reflect_compute().data(),
                                   render_test_shaders::reflect_compute().size_bytes()));
    REQUIRE(r.entryPoints.size() == 1);
    const ShaderEntryPoint& cs = r.entryPoints[0];
    CHECK(cs.name == "csMain");
    CHECK(cs.stage == ShaderStage::Compute);
    CHECK(cs.workgroupSize == std::array<u32, 3>{8, 4, 1});
    CHECK(cs.usesPushConstants);
    CHECK(r.findEntryPoint("csMain") == &r.entryPoints[0]);
    CHECK(r.findEntryPoint("missing") == nullptr);

    const ShaderPushConstants& pc = r.pushConstants;
    CHECK(pc.blockName.find("Push") != std::string::npos);
    CHECK(pc.size == 112);
    REQUIRE(pc.members.size() == 5);
    CHECK(pc.members[0].name == "viewProj");
    CHECK(pc.members[0].kind == ShaderValueKind::Matrix);
    CHECK(pc.members[0].offset == 0);
    CHECK(pc.members[0].size == 64);
    CHECK(pc.members[0].components == 16);
    CHECK(pc.members[1].name == "output");
    CHECK(pc.members[1].kind == ShaderValueKind::Pointer);
    CHECK(pc.members[1].offset == 64);
    CHECK(pc.members[1].size == 8);
    CHECK(pc.members[2].name == "count");
    CHECK(pc.members[2].kind == ShaderValueKind::UInt);
    CHECK(pc.members[2].offset == 72);
    CHECK(pc.members[3].name == "scale");
    CHECK(pc.members[3].kind == ShaderValueKind::Float);
    CHECK(pc.members[3].offset == 76);
    CHECK(pc.members[4].name == "tint");
    CHECK(pc.members[4].kind == ShaderValueKind::Vector);
    CHECK(pc.members[4].components == 4);
    CHECK(pc.members[4].arrayCount == 2);
    CHECK(pc.members[4].offset == 80);
    CHECK(pc.members[4].size == 32);

    REQUIRE(r.specConstants.size() == 3);
    CHECK(r.specConstants[0].id == 3);
    CHECK(r.specConstants[0].name == "kQuality");
    CHECK(r.specConstants[0].kind == ShaderValueKind::Int);
    CHECK(r.specConstants[0].defaultBits == 2);
    CHECK(r.specConstants[1].id == 7);
    CHECK(r.specConstants[1].kind == ShaderValueKind::Bool);
    CHECK(r.specConstants[1].defaultBits == 1);
    CHECK(r.specConstants[2].id == 9);
    CHECK(r.specConstants[2].kind == ShaderValueKind::Float);
    CHECK(r.specConstants[2].defaultBits == std::bit_cast<u32>(1.5f));

    REQUIRE(r.bindings.size() == 1);  // only what the entry point uses survives Slang's DCE
    const ShaderBinding& b = r.bindings[0];
    CHECK(b.name == "gBuffers");
    CHECK(b.set == 0);
    CHECK(b.binding == 2);
    CHECK(b.kind == ShaderBindingKind::StorageBuffer);
    CHECK(b.count == 0);
    CHECK(b.access == ShaderAccess::ReadWrite);
    CHECK(b.entryPointMask == 1u);
}

TEST_CASE("reflection: graphics module (stages and per-entry-point binding use)") {
    const ShaderReflection r = reflect(render_test_shaders::reflect_graphics());
    REQUIRE(r.entryPoints.size() == 2);
    const ShaderEntryPoint* vs = r.findEntryPoint("vsMain");
    const ShaderEntryPoint* ps = r.findEntryPoint("psMain");
    REQUIRE(vs != nullptr);
    REQUIRE(ps != nullptr);
    CHECK(vs->stage == ShaderStage::Vertex);
    CHECK(ps->stage == ShaderStage::Fragment);
    CHECK(vs->workgroupSize == std::array<u32, 3>{0, 0, 0});
    CHECK(vs->usesPushConstants);
    CHECK(ps->usesPushConstants);
    CHECK(r.pushConstants.size == 16);
    const u32 psBit = 1u << static_cast<u32>(ps - r.entryPoints.data());
    REQUIRE(r.bindings.size() == 2);
    CHECK(r.bindings[0].name == "gTexture2D");
    CHECK(r.bindings[0].binding == 0);
    CHECK(r.bindings[0].kind == ShaderBindingKind::SampledImage);
    CHECK(r.bindings[0].access == ShaderAccess::Read);
    CHECK(r.bindings[0].entryPointMask == psBit);
    CHECK(r.bindings[1].name == "gSamplers");
    CHECK(r.bindings[1].binding == 3);
    CHECK(r.bindings[1].kind == ShaderBindingKind::Sampler);
    CHECK(r.bindings[1].entryPointMask == psBit);
}

TEST_CASE("reflection: .hsr binary round trip and deterministic output") {
    for (std::string_view stem : {"reflect_compute", "reflect_graphics"}) {
        CAPTURE(stem);
        const ShaderReflection r = reflect(render_test_shaders::find(stem));
        const std::vector<u8> bytes = serializeReflection(r);
        CHECK(bytes == serializeReflection(r));
        REQUIRE(bytes.size() >= 52);
        CHECK(bytes[0] == 'H');
        CHECK(bytes[1] == 'S');
        CHECK(bytes[2] == 'R');
        CHECK(bytes[3] == '1');
        auto parsed = parseReflection(bytes);
        REQUIRE(parsed.ok());
        CHECK(parsed.value() == r);
    }
}

TEST_CASE("reflection: corrupt .hsr and SPIR-V inputs are rejected") {
    const ShaderReflection r = reflect(render_test_shaders::reflect_compute());
    const std::vector<u8> good = serializeReflection(r);
    {
        std::vector<u8> bad = good;
        bad[0] = 'X';
        CHECK(parseReflection(bad).errorCode() == ErrorCode::Corrupt);
    }
    {
        std::vector<u8> bad = good;
        bad[4] = 2;  // version 2
        CHECK(parseReflection(bad).errorCode() == ErrorCode::VersionMismatch);
    }
    for (usize cut : {usize{0}, usize{10}, usize{47}, good.size() / 2, good.size() - 1}) {
        CAPTURE(cut);
        CHECK(parseReflection(std::span<const u8>(good.data(), cut)).errorCode() == ErrorCode::Corrupt);
    }
    {
        std::vector<u8> bad = good;
        // First entry point's name offset -> far outside the string table.
        bad[48] = 0xFF;
        bad[49] = 0xFF;
        CHECK(parseReflection(bad).errorCode() == ErrorCode::Corrupt);
    }
    {
        std::vector<u8> bad = good;
        bad[52] = 200;  // entry-point stage enum
        CHECK(parseReflection(bad).errorCode() == ErrorCode::Corrupt);
    }
    // Every single-byte truncation or bit flip in the header parses or fails cleanly (no crash).
    for (usize i = 0; i < good.size(); ++i) {
        std::vector<u8> bad = good;
        bad[i] ^= 0xA5;
        (void)parseReflection(bad);
    }
    const std::vector<u32> garbage = {0xDEADBEEFu, 1, 2, 3, 4, 5};
    CHECK(reflectSpirv(garbage).errorCode() == ErrorCode::Corrupt);
    std::vector<u32> truncated(render_test_shaders::reflect_compute().begin(), render_test_shaders::reflect_compute().end());
    truncated[5] = 0xFFFF0000u | (truncated[5] & 0xFFFFu);  // first instruction claims 65535 words
    CHECK(reflectSpirv(truncated).errorCode() == ErrorCode::Corrupt);
    const u8 odd[5] = {3, 2, 0x23, 7, 0};
    CHECK(reflectSpirvBytes(odd).errorCode() == ErrorCode::Corrupt);
    CHECK(reflectSpirvBytes({}).errorCode() == ErrorCode::Corrupt);  // empty file (no memcpy from null)
    CHECK(parseReflection({}).errorCode() == ErrorCode::Corrupt);
}

TEST_CASE("reflection: JSONC rendering") {
    const ShaderReflection r = reflect(render_test_shaders::reflect_compute());
    const std::string text = reflectionToJsonc(r);
    CHECK(text.starts_with("// Helios shader reflection (.hsr v1"));
    CHECK(text.find("\"spirvVersion\": \"1.6\"") != std::string::npos);
    CHECK(text.find("{ \"name\": \"csMain\", \"stage\": \"compute\", \"workgroupSize\": [8, 4, 1], "
                    "\"usesPushConstants\": true }") != std::string::npos);
    CHECK(text.find("\"size\": 112") != std::string::npos);
    CHECK(text.find("{ \"name\": \"tint\", \"offset\": 80, \"size\": 32, \"type\": \"vector\", \"components\": 4, "
                    "\"arrayCount\": 2 }") != std::string::npos);
    CHECK(text.find("{ \"id\": 9, \"name\": \"kGain\", \"type\": \"float\", \"default\": 1.5 }") != std::string::npos);
    CHECK(text.find("{ \"id\": 7, \"name\": \"kFancy\", \"type\": \"bool\", \"default\": true }") != std::string::npos);
    CHECK(text.find("\"kind\": \"storageBuffer\", \"count\": 0, \"name\": \"gBuffers\", \"access\": \"readWrite\", "
                    "\"entryPoints\": [\"csMain\"]") != std::string::npos);
    CHECK(text.find(r.contentHash.toHex()) != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Hostile input (review regressions): SPIR-V and .hsr come from files, so malformed data must fail
// cleanly with time and memory linear in its size.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr u32 spvOp(u32 opcode, u32 wordCount) { return (wordCount << 16) | opcode; }

std::vector<u32> spvHeader(u32 bound) { return {0x07230203u, 0x00010600u, 0u, bound, 0u}; }

f64 secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("reflection: huge SPIR-V member indices neither crash nor allocate per index") {
    // OpMemberName %5 0xFFFFFFFF "A": used to resize a vector to index + 1 == 0 and write past it.
    std::vector<u32> words = spvHeader(100);
    words.insert(words.end(), {spvOp(6, 4), 5u, 0xFFFFFFFFu, 0x41u});
    CHECK(reflectSpirv(words).ok());

    // Thousands of member decorations at index 0xFFFF (64 KiB records per struct if stored densely).
    words = spvHeader(100000);
    for (u32 id = 1; id <= 2000; ++id) words.insert(words.end(), {spvOp(72, 5), id, 0xFFFFu, 35u /*Offset*/, 0u});
    words.insert(words.end(), {spvOp(6, 4), 7u, 0x7FFFFFFFu, 0x42u});
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(reflectSpirv(words).ok());
    CHECK(secondsSince(t0) < 2.0);
}

TEST_CASE("reflection: cyclic push-constant types are rejected quickly") {
    // %5 = OpTypeStruct %5 %5 (invalid SPIR-V); %6 = OpTypePointer PushConstant %5; %7 = OpVariable %6.
    // A depth-limited walk visits 2^33 nodes; the work budget stops it.
    std::vector<u32> words = spvHeader(100);
    words.insert(words.end(), {spvOp(30, 4), 5u, 5u, 5u});
    words.insert(words.end(), {spvOp(32, 4), 6u, 9u, 5u});
    words.insert(words.end(), {spvOp(59, 4), 6u, 7u, 9u});
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(reflectSpirv(words).errorCode() == ErrorCode::Corrupt);
    CHECK(secondsSince(t0) < 5.0);

    // Exponentially shared (acyclic) structs: S(k) = { S(k-1), S(k-1) }, 30 levels deep.
    words = spvHeader(200);
    words.insert(words.end(), {spvOp(21, 4), 10u, 32u, 0u});  // %10 = uint
    words.insert(words.end(), {spvOp(30, 4), 11u, 10u, 10u});
    for (u32 k = 12; k < 42; ++k) words.insert(words.end(), {spvOp(30, 4), k, k - 1, k - 1});
    words.insert(words.end(), {spvOp(32, 4), 50u, 9u, 41u});
    words.insert(words.end(), {spvOp(59, 4), 50u, 51u, 9u});
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(reflectSpirv(words).errorCode() == ErrorCode::Corrupt);
    CHECK(secondsSince(t1) < 5.0);
}

TEST_CASE("reflection: storage buffers are read-only only when every member is NonWritable") {
    // struct %20 { uint a; uint b; } bound as a StorageBuffer; only member 0 is decorated
    // (NonWritable). Member 1 is writable, so the binding is read-write.
    std::vector<u32> words = spvHeader(100);
    words.insert(words.end(), {spvOp(71, 4), 22u, 33u, 4u});           // Binding 4
    words.insert(words.end(), {spvOp(72, 4), 20u, 0u, 24u});           // member 0 NonWritable
    words.insert(words.end(), {spvOp(21, 4), 10u, 32u, 0u});
    words.insert(words.end(), {spvOp(30, 4), 20u, 10u, 10u});
    words.insert(words.end(), {spvOp(32, 4), 21u, 12u /*StorageBuffer*/, 20u});
    words.insert(words.end(), {spvOp(59, 4), 21u, 22u, 12u});
    auto r = reflectSpirv(words);
    REQUIRE(r.ok());
    REQUIRE(r->bindings.size() == 1);
    CHECK(r->bindings[0].binding == 4);
    CHECK(r->bindings[0].kind == ShaderBindingKind::StorageBuffer);
    CHECK(r->bindings[0].access == ShaderAccess::ReadWrite);
}

TEST_CASE("reflection: .hsr string references cannot expand without bound; version 0 is rejected") {
    ShaderReflection r;
    r.entryPoints.resize(1);
    r.entryPoints[0].name = std::string(200000, 'x');
    const std::vector<u8> one = serializeReflection(r);
    REQUIRE(parseReflection(one).ok());
    // 20,000 entry records naming the same 200 KB string: a 600 KB blob that used to decode to 4 GB.
    const u32 extra = 20000;
    std::vector<u8> blob(one.begin(), one.begin() + 68);
    for (u32 i = 0; i < extra; ++i) blob.insert(blob.end(), one.begin() + 48, one.begin() + 68);
    blob.insert(blob.end(), one.begin() + 68, one.end());
    const u32 entries = 1 + extra;
    const u32 total = static_cast<u32>(blob.size());
    std::memcpy(blob.data() + 32, &entries, 4);
    std::memcpy(blob.data() + 8, &total, 4);
    const auto t0 = std::chrono::steady_clock::now();
    auto parsed = parseReflection(blob);
    CHECK(parsed.errorCode() == ErrorCode::Corrupt);
    CHECK(parsed.error().message.find("decode limit") != std::string::npos);
    CHECK(secondsSince(t0) < 5.0);

    std::vector<u8> v0 = serializeReflection(reflect(render_test_shaders::reflect_compute()));
    v0[4] = 0;
    v0[5] = 0;
    CHECK(parseReflection(v0).errorCode() == ErrorCode::Corrupt);
}

TEST_CASE("reflection: JSONC escapes control characters and non-finite floats") {
    ShaderReflection r;
    r.entryPoints.push_back({"line\nbreak\x01", ShaderStage::Compute, {1, 1, 1}, false});
    ShaderSpecConstant nan;
    nan.id = 1;
    nan.name = "kNan";
    nan.kind = ShaderValueKind::Float;
    nan.defaultBits = 0x7FC00000u;
    r.specConstants.push_back(nan);
    const std::string text = reflectionToJsonc(r);
    CHECK(text.find("\"line\\u000abreak\\u0001\"") != std::string::npos);
    CHECK(text.find('\x01') == std::string::npos);
    CHECK(text.find("\"default\": \"nan\"") != std::string::npos);
}

TEST_CASE("reflection: mutated modules fail cleanly or round-trip exactly through .hsr") {
    // Deterministic mini-fuzz (a longer run under ASan/UBSan found the >255-component and empty-input
    // cases): every module reflectSpirv accepts must serialize and parse back to the same data, and
    // mutated .hsr blobs must parse or fail without crashing.
    Pcg32 rng(0x5eedu, 12);
    u32 reflected = 0;
    for (std::string_view stem : {"reflect_compute", "reflect_graphics"}) {
        const std::span<const u32> base = render_test_shaders::find(stem);
        const auto baseBytes = std::as_bytes(base);
        const std::vector<u8> original(reinterpret_cast<const u8*>(baseBytes.data()),
                                       reinterpret_cast<const u8*>(baseBytes.data()) + baseBytes.size());
        const std::vector<u8> hsr = serializeReflection(reflect(base));
        for (u32 it = 0; it < 1500; ++it) {
            std::vector<u8> m = original;
            const u32 edits = 1 + uniformU32Below(rng, 8);
            for (u32 k = 0; k < edits; ++k) {
                const u32 at = uniformU32Below(rng, static_cast<u32>(m.size()));
                const u32 how = uniformU32Below(rng, 3);
                m[at] = how == 0 ? static_cast<u8>(m[at] ^ (1u << uniformU32Below(rng, 8)))
                                 : (how == 1 ? static_cast<u8>(rng.nextU32()) : u8{0xFF});
            }
            auto r = reflectSpirvBytes(m);
            if (r.ok()) {
                ++reflected;
                auto back = parseReflection(serializeReflection(r.value()));
                REQUIRE(back.ok());
                CHECK(back.value() == r.value());
            }
            std::vector<u8> h = hsr;
            for (u32 k = 0; k < edits; ++k) h[uniformU32Below(rng, static_cast<u32>(h.size()))] = static_cast<u8>(rng.nextU32());
            if (auto p = parseReflection(h); p.ok()) (void)reflectionToJsonc(p.value());
        }
    }
    CHECK(reflected > 100);  // the mutations must also exercise the success path
}
