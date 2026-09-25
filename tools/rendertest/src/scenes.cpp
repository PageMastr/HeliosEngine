// helios-rendertest scenes (see scenes.h). RC-1 requires triangle, compute and bindless; forward,
// postchain and mips cover the forward pipeline, transient aliasing across queues and
// per-subresource (mip) barriers on real GPUs.

#include "scenes.h"

#include <array>

#include "helios-rendertest_shaders.h"
#include "helios/render/forward.h"
#include "helios/rhi/utils.h"

namespace helios::rendertest {

using namespace helios::render;
namespace shaders = rendertest_shaders;

namespace {

constexpr rhi::Format kHdr = rhi::Format::RGBA16Float;

u32 groups(u32 size) { return (size + 7) / 8; }

rhi::GraphicsPipelineDesc fullscreenPipeline(std::string_view fragment, rhi::Format format, std::string_view name) {
    rhi::GraphicsPipelineDesc d;
    d.vertex = {shaders::blit(), "vsFullscreen"};
    d.fragment = {shaders::blit(), fragment};
    d.colorCount = 1;
    d.colorFormats[0] = format;
    d.name = name;
    return d;
}

rhi::ComputePipelineDesc computePipeline(std::span<const u32> spirv, std::string_view entry, std::string_view name) {
    rhi::ComputePipelineDesc d;
    d.compute = {spirv, entry};
    d.name = name;
    return d;
}

struct PatternPush {
    u32 target, width, height, variant;
};
struct BlitPush {
    u32 source, samplerIndex, mipCount, pad;
};
struct BlurPush {
    u32 source, target, width, height;
    i32 dx, dy;
};

/// Compute pass writing the procedural pattern into a new transient.
RgTexture addPattern(RenderGraph& graph, std::string_view pass, std::string_view name, u32 w, u32 h, u32 variant,
                     PassFlags kind, rhi::PipelineH pso) {
    struct Data {
        RgTexture target;
    };
    const Data& d = graph.addPass<Data>(
        pass, kind, [&](RgBuilder& b, Data& data) { data.target = b.write(b.create(name, RgTextureDesc::tex2D(kHdr, w, h))); },
        [pso, w, h, variant](const Data& data, RgContext& ctx) {
            ctx.cmd().bindPipeline(pso);
            ctx.cmd().pushConstants(PatternPush{ctx.uav(data.target), w, h, variant});
            ctx.cmd().dispatch(groups(w), groups(h));
        });
    return d.target;
}

/// Fullscreen blit (Load, same size) of `source` into `output`.
RgTexture addBlit(RenderGraph& graph, std::string_view pass, RgTexture source, RgTexture output, rhi::PipelineH pso) {
    struct Data {
        RgTexture source, output;
    };
    const Data& d = graph.addPass<Data>(
        pass, PassFlags::Raster,
        [&](RgBuilder& b, Data& data) {
            data.source = b.read(source);
            data.output = b.colorAttachment(output, 0, rhi::LoadOp::DontCare);
        },
        [pso](const Data& data, RgContext& ctx) {
            ctx.cmd().bindPipeline(pso);
            ctx.cmd().pushConstants(BlitPush{ctx.srv(data.source), 0, 0, 0});
            ctx.cmd().draw(3);
        });
    return d.output;
}

// ---------------------------------------------------------------------------------------------
class TriangleScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        rhi::GraphicsPipelineDesc d;
        d.vertex = {shaders::triangle(), "vsMain"};
        d.fragment = {shaders::triangle(), "psMain"};
        d.colorCount = 1;
        d.colorFormats[0] = format;
        d.name = "Triangle";
        HELIOS_TRY_ASSIGN(m_pso, device.createGraphicsPipeline(d));
        return {};
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override {
        const rhi::PipelineH pso = m_pso;
        graph.addPass("Triangle", PassFlags::Raster,
                      [&](RgBuilder& b) { b.colorAttachment(output, 0, rhi::LoadOp::Clear, {0.02f, 0.02f, 0.08f, 1.0f}); },
                      [pso](RgContext& ctx) {
                          ctx.cmd().bindPipeline(pso);
                          ctx.cmd().draw(3);
                      });
    }
    void destroy(rhi::Device& device) override { device.destroy(m_pso); }

private:
    SceneInfo m_info{"triangle", "Vertex-colored triangle, one raster pass (RC-1)", 320, 180, 0.01, 0.5f, 0};
    rhi::PipelineH m_pso;
};

// ---------------------------------------------------------------------------------------------
class ComputeScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        HELIOS_TRY_ASSIGN(m_pattern, device.createComputePipeline(computePipeline(shaders::pattern(), "csPattern", "Pattern")));
        HELIOS_TRY_ASSIGN(m_blit, device.createGraphicsPipeline(fullscreenPipeline("psBlit", format, "Blit")));
        return {};
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override {
        const RgTexture pattern =
            addPattern(graph, "Pattern", "Pattern", m_info.width, m_info.height, 0, PassFlags::AsyncCompute, m_pattern);
        addBlit(graph, "Blit", pattern, output, m_blit);
    }
    void destroy(rhi::Device& device) override {
        device.destroy(m_pattern);
        device.destroy(m_blit);
    }

private:
    SceneInfo m_info{"compute", "Async-compute storage-image pattern, sampled by a fullscreen pass (RC-1)", 320, 180,
                     0.01, 0.5f, 0};
    rhi::PipelineH m_pattern, m_blit;
};

// ---------------------------------------------------------------------------------------------
class BindlessScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        rhi::GraphicsPipelineDesc d;
        d.vertex = {shaders::quads(), "vsMain"};
        d.fragment = {shaders::quads(), "psMain"};
        d.colorCount = 1;
        d.colorFormats[0] = format;
        d.name = "Quads";
        HELIOS_TRY_ASSIGN(m_pso, device.createGraphicsPipeline(d));
        for (u32 t = 0; t < 4; ++t) {
            HELIOS_TRY_ASSIGN(m_textures[t], (device.createTexture(rhi::TextureDesc::tex2D(
                                                 rhi::Format::RGBA8Unorm, 16, 16,
                                                 rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst, kNames[t]))));
            const std::vector<u8> texels = pattern(t);
            HELIOS_TRY(rhi::uploadTexture(device, m_textures[t], std::as_bytes(std::span(texels))));
            m_descs[t] = device.textureDesc(m_textures[t]);
        }
        rhi::SamplerDesc nearestRepeat;
        nearestRepeat.minFilter = nearestRepeat.magFilter = nearestRepeat.mipFilter = rhi::Filter::Nearest;
        const rhi::SamplerDesc linearRepeat;
        rhi::SamplerDesc nearestClamp = nearestRepeat;
        nearestClamp.addressU = nearestClamp.addressV = nearestClamp.addressW = rhi::AddressMode::ClampToEdge;
        rhi::SamplerDesc linearMirror;
        linearMirror.addressU = linearMirror.addressV = linearMirror.addressW = rhi::AddressMode::MirroredRepeat;
        m_samplers = {device.sampler(nearestRepeat), device.sampler(linearRepeat), device.sampler(nearestClamp),
                      device.sampler(linearMirror)};
        return device.waitIdle();
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override {
        struct Data {
            std::array<RgTexture, 4> textures;
        };
        std::array<RgTexture, 4> imported;
        for (u32 t = 0; t < 4; ++t) {
            imported[t] = graph.importTexture(kNames[t], m_textures[t], m_descs[t],
                                              {.initialState = rhi::ResourceState::ShaderResource});
        }
        const rhi::PipelineH pso = m_pso;
        const std::array<rhi::BindlessIndex, 4> samplers = m_samplers;
        graph.addPass<Data>(
            "Quads", PassFlags::Raster,
            [&](RgBuilder& b, Data& data) {
                for (u32 t = 0; t < 4; ++t) data.textures[t] = b.read(imported[t]);
                b.colorAttachment(output, 0, rhi::LoadOp::Clear, {0.12f, 0.12f, 0.12f, 1.0f});
            },
            [samplers, pso](const Data& data, RgContext& ctx) {
                struct Push {
                    u32 textures[8];
                    u32 samplers[8];
                } push{};
                for (u32 q = 0; q < 8; ++q) {
                    push.textures[q] = ctx.srv(data.textures[q % 4]);
                    push.samplers[q] = samplers[(q / 2 + q) % 4];
                }
                ctx.cmd().bindPipeline(pso);
                ctx.cmd().pushConstants(push);
                ctx.cmd().draw(6, 8);
            });
    }
    void destroy(rhi::Device& device) override {
        device.destroy(m_pso);
        for (rhi::TextureH& t : m_textures) device.destroy(t);
    }

private:
    static constexpr std::array<std::string_view, 4> kNames{"Checker", "Gradient", "Stripes", "Dots"};

    /// 16x16 RGBA8 patterns: checker, gradient, stripes, dots.
    static std::vector<u8> pattern(u32 kind) {
        std::vector<u8> texels(16 * 16 * 4);
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                u8* p = &texels[(y * 16 + x) * 4];
                bool on = false;
                switch (kind) {
                case 0:
                    on = (((x / 4) ^ (y / 4)) & 1) != 0;
                    p[0] = static_cast<u8>(on ? 240 : 30);
                    p[1] = static_cast<u8>(on ? 200 : 40);
                    p[2] = static_cast<u8>(on ? 60 : 90);
                    break;
                case 1:
                    p[0] = static_cast<u8>(x * 16);
                    p[1] = static_cast<u8>(y * 16);
                    p[2] = 128;
                    break;
                case 2:
                    on = (((x + y) / 3) & 1) != 0;
                    p[0] = static_cast<u8>(on ? 20 : 220);
                    p[1] = static_cast<u8>(on ? 160 : 230);
                    p[2] = static_cast<u8>(on ? 200 : 240);
                    break;
                default: {
                    const i32 dx = static_cast<i32>(x % 8) - 4;
                    const i32 dy = static_cast<i32>(y % 8) - 4;
                    on = dx * dx + dy * dy < 7;
                    p[0] = static_cast<u8>(on ? 250 : 60);
                    p[1] = static_cast<u8>(on ? 90 : 20);
                    p[2] = static_cast<u8>(on ? 160 : 60);
                    break;
                }
                }
                p[3] = 255;
            }
        }
        return texels;
    }

    SceneInfo m_info{"bindless", "Eight quads sampling four bindless textures through four bindless samplers (RC-1)", 320,
                     180, 0.01, 0.5f, 0};
    rhi::PipelineH m_pso;
    std::array<rhi::TextureH, 4> m_textures{};
    std::array<rhi::TextureDesc, 4> m_descs{};
    std::array<rhi::BindlessIndex, 4> m_samplers{};
};

// ---------------------------------------------------------------------------------------------
class ForwardTestScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        HELIOS_TRY_ASSIGN(m_renderer, ForwardRenderer::create(device, format));
        HELIOS_TRY_ASSIGN(GpuMesh cube, uploadMesh(device, makeCube(0.5f), "Cube"));
        m_scene.meshes.push_back(cube);
        HELIOS_TRY_ASSIGN(GpuMesh sphere, uploadMesh(device, makeUvSphere(0.5f, 32, 16), "Sphere"));
        m_scene.meshes.push_back(sphere);
        HELIOS_TRY_ASSIGN(GpuMesh ground, uploadMesh(device, makePlane(6.0f), "Ground"));
        m_scene.meshes.push_back(ground);
        // Far from the frame origin on purpose: camera-relative rendering keeps it exact.
        const DVec3 origin{1.0e7, 2.5e3, -3.0e6};
        m_scene.camera.position = origin + DVec3{0.0, 2.2, 7.0};
        m_scene.camera.rotation = Quat::fromAxisAngle({1.0f, 0.0f, 0.0f}, -0.22f);
        m_scene.camera.fovY = 1.0f;
        auto place = [&](u32 mesh, DVec3 at, Vec4 color, f32 yaw, f32 scale) {
            ForwardInstance inst;
            inst.mesh = mesh;
            inst.transform.position = origin + at;
            inst.transform.rotation = Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
            inst.transform.scale = {scale, scale, scale};
            inst.color = color;
            m_scene.instances.push_back(inst);
        };
        place(2, {0.0, 0.0, 0.0}, {0.5f, 0.52f, 0.5f, 1.0f}, 0.0f, 1.0f);
        place(0, {-2.0, 0.6, 0.0}, {0.9f, 0.25f, 0.18f, 1.0f}, 0.7f, 1.2f);
        place(1, {0.0, 0.8, -1.0}, {0.2f, 0.45f, 0.95f, 1.0f}, 0.0f, 1.6f);
        place(0, {2.1, 0.45, 0.6}, {0.95f, 0.8f, 0.2f, 1.0f}, -0.4f, 0.9f);
        place(1, {1.0, 0.3, 1.8}, {0.3f, 0.85f, 0.4f, 1.0f}, 0.0f, 0.6f);
        return {};
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override { m_renderer->addPasses(graph, m_scene, output); }
    void destroy(rhi::Device& device) override {
        for (GpuMesh& m : m_scene.meshes) destroyMesh(device, m);
        m_scene.meshes.clear();
        m_scene.instances.clear();
        m_renderer.reset();
    }

private:
    SceneInfo m_info{"forward",
                     "Forward pipeline v0 at 10^7 m: clear, geometry with reverse-Z depth, async exposure, tonemap",
                     320, 180, 0.01, 0.5f, 0};
    std::unique_ptr<ForwardRenderer> m_renderer;
    ForwardScene m_scene;
};

// ---------------------------------------------------------------------------------------------
class PostChainScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        HELIOS_TRY_ASSIGN(m_pattern, device.createComputePipeline(computePipeline(shaders::pattern(), "csPattern", "Pattern")));
        HELIOS_TRY_ASSIGN(m_blur, device.createComputePipeline(computePipeline(shaders::blur(), "csBlur", "Blur")));
        HELIOS_TRY_ASSIGN(m_blit, device.createGraphicsPipeline(fullscreenPipeline("psBlit", format, "Blit")));
        return {};
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override {
        const u32 w = m_info.width, h = m_info.height;
        RgTexture current = addPattern(graph, "Seed", "T0", w, h, 1, PassFlags::Compute, m_pattern);
        struct Step {
            const char* pass;
            const char* target;
            PassFlags kind;
            i32 dx, dy;
        };
        static constexpr Step kSteps[] = {{"BlurH", "T1", PassFlags::AsyncCompute, 1, 0},
                                          {"BlurV", "T2", PassFlags::Compute, 0, 1},
                                          {"BlurH2", "T3", PassFlags::AsyncCompute, 2, 0},
                                          {"BlurV2", "T4", PassFlags::Compute, 0, 2}};
        struct Data {
            RgTexture source, target;
        };
        const rhi::PipelineH blur = m_blur;
        for (const Step& s : kSteps) {
            const Data& d = graph.addPass<Data>(
                s.pass, s.kind,
                [&](RgBuilder& b, Data& data) {
                    data.source = b.read(current);
                    data.target = b.write(b.create(s.target, RgTextureDesc::tex2D(kHdr, w, h)));
                },
                [blur, w, h, dx = s.dx, dy = s.dy](const Data& data, RgContext& ctx) {
                    ctx.cmd().bindPipeline(blur);
                    ctx.cmd().pushConstants(BlurPush{ctx.srv(data.source), ctx.uav(data.target), w, h, dx, dy});
                    ctx.cmd().dispatch(groups(w), groups(h));
                });
            current = d.target;
        }
        addBlit(graph, "Blit", current, output, m_blit);
    }
    void destroy(rhi::Device& device) override {
        device.destroy(m_pattern);
        device.destroy(m_blur);
        device.destroy(m_blit);
    }

private:
    SceneInfo m_info{"postchain",
                     "Seed + four separable blurs alternating graphics and async compute (transients aliased across "
                     "queues), blit",
                     320, 180, 0.01, 0.5f, 0};
    rhi::PipelineH m_pattern, m_blur, m_blit;
};

// ---------------------------------------------------------------------------------------------
class MipsScene final : public Scene {
public:
    const SceneInfo& info() const noexcept override { return m_info; }
    Result<void> init(rhi::Device& device, rhi::Format format) override {
        HELIOS_TRY_ASSIGN(m_pattern, device.createComputePipeline(computePipeline(shaders::pattern(), "csPattern", "Pattern")));
        HELIOS_TRY_ASSIGN(m_down, device.createComputePipeline(computePipeline(shaders::blur(), "csDownsample", "Downsample")));
        HELIOS_TRY_ASSIGN(m_view, device.createGraphicsPipeline(fullscreenPipeline("psMips", format, "MipView")));
        rhi::SamplerDesc nearest;
        nearest.minFilter = nearest.magFilter = nearest.mipFilter = rhi::Filter::Nearest;
        nearest.addressU = nearest.addressV = nearest.addressW = rhi::AddressMode::ClampToEdge;
        m_sampler = device.sampler(nearest);
        return {};
    }
    void addPasses(RenderGraph& graph, RgTexture output, u32) override {
        constexpr u32 kSize = 128;
        constexpr u32 kMips = 4;
        struct Data {
            RgTexture texture;
        };
        const rhi::PipelineH pattern = m_pattern, down = m_down, view = m_view;
        const rhi::BindlessIndex sampler = m_sampler;
        const Data& base = graph.addPass<Data>(
            "Mip0", PassFlags::Compute,
            [&](RgBuilder& b, Data& data) {
                data.texture = b.write(b.create("Chain", RgTextureDesc::tex2D(kHdr, kSize, kSize, kMips)),
                                       TextureWrite::Storage, {0, 1, 0, 1});
            },
            [pattern](const Data& data, RgContext& ctx) {
                ctx.cmd().bindPipeline(pattern);
                ctx.cmd().pushConstants(PatternPush{ctx.uav(data.texture, 0), kSize, kSize, 2});
                ctx.cmd().dispatch(groups(kSize), groups(kSize));
            });
        RgTexture chain = base.texture;
        for (u32 m = 1; m < kMips; ++m) {
            // Mip 2 runs on async compute: per-subresource states cross queues.
            const PassFlags kind = m == 2 ? PassFlags::AsyncCompute : PassFlags::Compute;
            const Data& d = graph.addPass<Data>(
                m == 1 ? "Mip1" : (m == 2 ? "Mip2" : "Mip3"), kind,
                [&](RgBuilder& b, Data& data) {
                    b.read(chain, TextureRead::Sampled, {m - 1, 1, 0, 1});
                    data.texture = b.write(chain, TextureWrite::Storage, {m, 1, 0, 1});
                },
                [down, m](const Data& data, RgContext& ctx) {
                    const u32 size = kSize >> m;
                    const rhi::BindlessIndex source = ctx.srv(data.texture, {.baseMip = m - 1, .mipCount = 1});
                    ctx.cmd().bindPipeline(down);
                    ctx.cmd().pushConstants(BlurPush{source, ctx.uav(data.texture, m), size, size, 0, 0});
                    ctx.cmd().dispatch(groups(size), groups(size));
                });
            chain = d.texture;
        }
        struct ViewData {
            RgTexture chain;
        };
        graph.addPass<ViewData>(
            "View", PassFlags::Raster,
            [&](RgBuilder& b, ViewData& data) {
                data.chain = b.read(chain);
                b.colorAttachment(output, 0, rhi::LoadOp::DontCare);
            },
            [view, sampler](const ViewData& data, RgContext& ctx) {
                ctx.cmd().bindPipeline(view);
                ctx.cmd().pushConstants(BlitPush{ctx.srv(data.chain), sampler, kMips, 0});
                ctx.cmd().draw(3);
            });
    }
    void destroy(rhi::Device& device) override {
        device.destroy(m_pattern);
        device.destroy(m_down);
        device.destroy(m_view);
    }

private:
    SceneInfo m_info{"mips", "Mip chain built by compute (one async), per-mip barriers, viewed side by side", 320, 180,
                     0.01, 0.5f, 0};
    rhi::PipelineH m_pattern, m_down, m_view;
    rhi::BindlessIndex m_sampler = 0;
};

} // namespace

std::vector<std::unique_ptr<Scene>> createScenes() {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<TriangleScene>());
    scenes.push_back(std::make_unique<ComputeScene>());
    scenes.push_back(std::make_unique<BindlessScene>());
    scenes.push_back(std::make_unique<ForwardTestScene>());
    scenes.push_back(std::make_unique<PostChainScene>());
    scenes.push_back(std::make_unique<MipsScene>());
    return scenes;
}

} // namespace helios::rendertest
