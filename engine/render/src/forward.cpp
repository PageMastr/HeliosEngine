// Forward pipeline v0 (see forward.h): meshes, camera-relative per-frame data and the pass set.

#include "helios/render/forward.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "helios/math/scalar.h"
#include "helios/render/shader_reflection.h"
#include "helios/rhi/utils.h"
#include "helios_render_shaders.h"

namespace helios::render {

namespace {

constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;

struct ForwardPush {
    u64 view;
    u64 instances;
    u64 vertices;
    u32 instance;
    u32 pad;
};
static_assert(sizeof(ForwardPush) == 32);

struct ExposurePush {
    u64 output;
    u32 hdr;
    u32 width;
    u32 height;
    f32 bias;
};
static_assert(sizeof(ExposurePush) == 24);

struct TonemapPush {
    u64 exposure;
    u32 hdr;
    u32 encodeSrgb;
};
static_assert(sizeof(TonemapPush) == 16);

/// Makes every triangle counter-clockwise seen from outside (its normal agrees with the vertex
/// normals) and drops degenerate ones.
void fixWinding(MeshData& mesh) {
    std::vector<u32> out;
    out.reserve(mesh.indices.size());
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        const Vec3 pa = mesh.vertices[a].position, pb = mesh.vertices[b].position, pc = mesh.vertices[c].position;
        const Vec3 n = cross(pb - pa, pc - pa);
        if (dot(n, n) < 1e-12f) continue;
        const Vec3 vn = mesh.vertices[a].normal + mesh.vertices[b].normal + mesh.vertices[c].normal;
        if (dot(n, vn) < 0.0f) std::swap(b, c);
        out.insert(out.end(), {a, b, c});
    }
    mesh.indices = std::move(out);
}

Result<void> checkPush(std::span<const u32> spirv, u32 expected, std::string_view name) {
    HELIOS_TRY_ASSIGN(ShaderReflection r, reflectSpirv(spirv));
    if (r.pushConstants.size != expected) {
        return makeError(ErrorCode::VersionMismatch, "{}: shader push constants are {} bytes, C++ expects {}", name,
                         r.pushConstants.size, expected);
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------------------------
MeshData makeCube(f32 h) {
    MeshData mesh;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        const Vec3 v = std::abs(n.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        const Vec3 u = cross(v, n);  // u x v = n: corners below run counter-clockwise around n
        const u32 base = static_cast<u32>(mesh.vertices.size());
        const f32 su[4] = {-1, 1, 1, -1};
        const f32 sv[4] = {-1, -1, 1, 1};
        for (u32 k = 0; k < 4; ++k) mesh.vertices.push_back({(n + u * su[k] + v * sv[k]) * h, n});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    fixWinding(mesh);
    return mesh;
}

MeshData makeUvSphere(f32 radius, u32 segments, u32 rings) {
    MeshData mesh;
    segments = std::max(segments, 3u);
    rings = std::max(rings, 2u);
    for (u32 r = 0; r <= rings; ++r) {
        const f64 theta = kPiD * r / rings;
        f64 st = 0.0, ct = 0.0;
        det::sinCos(theta, st, ct);
        for (u32 s = 0; s <= segments; ++s) {
            const f64 phi = 2.0 * kPiD * s / segments;
            f64 sp = 0.0, cp = 0.0;
            det::sinCos(phi, sp, cp);
            const Vec3 n{static_cast<f32>(st * cp), static_cast<f32>(ct), static_cast<f32>(-st * sp)};
            mesh.vertices.push_back({n * radius, n});
        }
    }
    const u32 stride = segments + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * stride + s, b = a + 1, c = a + stride, d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    fixWinding(mesh);
    return mesh;
}

MeshData makePlane(f32 h) {
    MeshData mesh;
    const Vec3 up{0, 1, 0};
    mesh.vertices = {{{-h, 0, -h}, up}, {{-h, 0, h}, up}, {{h, 0, h}, up}, {{h, 0, -h}, up}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    fixWinding(mesh);
    return mesh;
}

Result<GpuMesh> uploadMesh(rhi::Device& device, const MeshData& mesh, std::string_view name) {
    GpuMesh out;
    const std::string vbName = std::string(name) + ".vertices";
    const std::string ibName = std::string(name) + ".indices";
    HELIOS_TRY_ASSIGN(out.vertices, (device.createBuffer({.size = mesh.vertices.size() * sizeof(MeshVertex),
                                                          .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst,
                                                          .name = vbName})));
    auto ib = device.createBuffer({.size = mesh.indices.size() * sizeof(u32),
                                   .usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst,
                                   .name = ibName});
    if (!ib) {
        device.destroy(out.vertices);
        return std::move(ib).error();
    }
    out.indices = ib.value();
    out.indexCount = static_cast<u32>(mesh.indices.size());
    auto fail = [&](Error e) -> Result<GpuMesh> {
        destroyMesh(device, out);
        return e;
    };
    auto v = rhi::uploadBuffer(device, out.vertices, 0, std::as_bytes(std::span(mesh.vertices)));
    if (!v) return fail(std::move(v).error());
    auto i = rhi::uploadBuffer(device, out.indices, 0, std::as_bytes(std::span(mesh.indices)));
    if (!i) return fail(std::move(i).error());
    rhi::CommandList* cmd = device.acquireCommandList(rhi::Queue::Graphics, "uploadMesh");
    if (!cmd) return fail(Error{ErrorCode::InvalidState, "uploadMesh: no command list"});
    const rhi::Barrier barriers[] = {
        rhi::Barrier::bufferState(out.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource),
        rhi::Barrier::bufferState(out.indices, rhi::ResourceState::CopyDest, rhi::ResourceState::IndexBuffer),
    };
    cmd->barrier(barriers);
    const rhi::TimelinePoint waits[] = {v.value(), i.value()};
    auto done = device.submit(rhi::Queue::Graphics, {&cmd, 1}, waits);
    if (!done) return fail(std::move(done).error());
    if (auto w = device.wait(done.value()); !w) return fail(std::move(w).error());
    return out;
}

void destroyMesh(rhi::Device& device, GpuMesh& mesh) {
    device.destroy(mesh.vertices);
    device.destroy(mesh.indices);
    mesh = {};
}

// ---------------------------------------------------------------------------------------------
// Camera and per-frame data
// ---------------------------------------------------------------------------------------------
Mat4 infiniteReverseZ(f32 fovY, f32 aspect, f32 zNear) noexcept {
    f64 s = 0.0, c = 0.0;
    det::sinCos(static_cast<f64>(fovY) * 0.5, s, c);
    const f32 f = static_cast<f32>(c / s);
    Mat4 m = Mat4::identity();
    m.cols[0] = {f / aspect, 0.0f, 0.0f, 0.0f};
    m.cols[1] = {0.0f, f, 0.0f, 0.0f};
    m.cols[2] = {0.0f, 0.0f, 0.0f, -1.0f};
    m.cols[3] = {0.0f, 0.0f, zNear, 0.0f};
    return m;
}

ForwardViewConstants ForwardRenderer::viewConstants(const ForwardScene& scene, f32 aspect) noexcept {
    ForwardViewConstants v{};
    v.viewProj = infiniteReverseZ(scene.camera.fovY, aspect, scene.camera.zNear) * cameraRelativeView(scene.camera.rotation);
    const Vec3 l = normalize(scene.lightDirection);
    v.lightDirection = {l.x, l.y, l.z, 0.0f};
    v.lightColor = {scene.lightColor.x, scene.lightColor.y, scene.lightColor.z, 1.0f};
    v.ambient = {scene.ambient.x, scene.ambient.y, scene.ambient.z, 1.0f};
    return v;
}

std::vector<ForwardInstanceData> ForwardRenderer::instanceData(const ForwardScene& scene) noexcept {
    std::vector<ForwardInstanceData> out;
    out.reserve(scene.instances.size());
    for (const ForwardInstance& inst : scene.instances) {
        // f64 subtraction first, one rounding to f32 (ADR-005): never an absolute f32 position.
        out.push_back({toMatrixCameraRelative(inst.transform, scene.camera.position), inst.color});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------
ForwardRenderer::~ForwardRenderer() {
    if (!m_device) return;
    m_device->destroy(m_geometry);
    m_device->destroy(m_exposure);
    m_device->destroy(m_tonemap);
    m_device->destroy(m_debugNormals);
}

Result<std::unique_ptr<ForwardRenderer>> ForwardRenderer::create(rhi::Device& device, rhi::Format outputFormat) {
    HELIOS_TRY(checkPush(helios_render_shaders::forward(), sizeof(ForwardPush), "forward.slang"));
    HELIOS_TRY(checkPush(helios_render_shaders::exposure(), sizeof(ExposurePush), "exposure.slang"));
    HELIOS_TRY(checkPush(helios_render_shaders::tonemap(), sizeof(TonemapPush), "tonemap.slang"));
    std::unique_ptr<ForwardRenderer> r(new ForwardRenderer());
    r->m_device = &device;
    r->m_outputFormat = outputFormat;

    rhi::GraphicsPipelineDesc geo;
    geo.vertex = {helios_render_shaders::forward(), "vsMain"};
    geo.fragment = {helios_render_shaders::forward(), "psMain"};
    geo.raster.cullMode = rhi::CullMode::Back;
    geo.depth = {true, true, rhi::CompareOp::GreaterOrEqual};
    geo.colorCount = 1;
    geo.colorFormats[0] = kHdrFormat;
    geo.depthFormat = kDepthFormat;
    geo.name = "Forward.Geometry";
    HELIOS_TRY_ASSIGN(r->m_geometry, device.createGraphicsPipeline(geo));

    rhi::GraphicsPipelineDesc normals = geo;
    normals.fragment = {helios_render_shaders::forward(), "psNormals"};
    normals.colorFormats[0] = rhi::Format::RGBA8Unorm;
    normals.name = "Forward.DebugNormals";
    HELIOS_TRY_ASSIGN(r->m_debugNormals, device.createGraphicsPipeline(normals));

    rhi::ComputePipelineDesc expo;
    expo.compute = {helios_render_shaders::exposure(), "csExposure"};
    expo.name = "Forward.Exposure";
    HELIOS_TRY_ASSIGN(r->m_exposure, device.createComputePipeline(expo));
    rhi::GraphicsPipelineDesc tone;
    tone.vertex = {helios_render_shaders::tonemap(), "vsFullscreen"};
    tone.fragment = {helios_render_shaders::tonemap(), "psTonemap"};
    tone.colorCount = 1;
    tone.colorFormats[0] = outputFormat;
    tone.name = "Forward.Tonemap";
    HELIOS_TRY_ASSIGN(r->m_tonemap, device.createGraphicsPipeline(tone));
    return r;
}

RgTexture ForwardRenderer::addPasses(RenderGraph& graph, const ForwardScene& scene, RgTexture output,
                                     const ForwardSettings& settings) const {
    struct Frame {
        RgBuffer view, instances, exposure;
        RgTexture color, depth, output, normals;
        std::vector<RgBuffer> vertices, indices;
        std::vector<u8> viewBytes, instanceBytes;
        std::vector<ForwardInstance> drawList;
        std::vector<GpuMesh> meshes;
        u32 width = 0, height = 0;
        f32 exposureBias = 0.0f;
        std::array<f32, 4> sky{};
    };
    auto f = std::make_shared<Frame>();
    f->output = output;
    f->drawList = scene.instances;
    f->meshes = scene.meshes;
    f->exposureBias = scene.exposureBias;
    f->sky = {scene.skyColor.x, scene.skyColor.y, scene.skyColor.z, scene.skyColor.w};

    // Meshes are external: import them in the states uploadMesh left them in.
    for (usize i = 0; i < scene.meshes.size(); ++i) {
        const GpuMesh& m = scene.meshes[i];
        f->vertices.push_back(graph.importBuffer(std::format("Mesh{}.vertices", i), m.vertices, m_device->bufferDesc(m.vertices),
                                                 {.initialState = rhi::ResourceState::ShaderResource}));
        f->indices.push_back(graph.importBuffer(std::format("Mesh{}.indices", i), m.indices, m_device->bufferDesc(m.indices),
                                                {.initialState = rhi::ResourceState::IndexBuffer}));
    }
    const rhi::PipelineH geometry = m_geometry, exposure = m_exposure, tonemap = m_tonemap, debugNormals = m_debugNormals;
    const u32 encodeSrgb = rhi::isSrgbFormat(m_outputFormat) ? 0u : 1u;

    graph.addPass("Upload", PassFlags::Copy,
                  [&](RgBuilder& b) {
                      const RgTextureDesc& out = b.desc(output);
                      f->width = out.width;
                      f->height = out.height;
                      const f32 aspect = static_cast<f32>(out.width) / static_cast<f32>(std::max(out.height, 1u));
                      const ForwardViewConstants view = viewConstants(scene, aspect);
                      f->viewBytes.resize(sizeof(view));
                      std::memcpy(f->viewBytes.data(), &view, sizeof(view));
                      const std::vector<ForwardInstanceData> inst = instanceData(scene);
                      f->instanceBytes.resize(std::max<usize>(inst.size(), 1) * sizeof(ForwardInstanceData));
                      if (!inst.empty()) std::memcpy(f->instanceBytes.data(), inst.data(), inst.size() * sizeof(ForwardInstanceData));
                      f->view = b.write(b.create("ViewConstants", RgBufferDesc{f->viewBytes.size()}), BufferWrite::CopyDest);
                      f->instances = b.write(b.create("Instances", RgBufferDesc{f->instanceBytes.size()}), BufferWrite::CopyDest);
                  },
                  [f](RgContext& ctx) {
                      auto upload = [&](RgBuffer buffer, const std::vector<u8>& bytes) {
                          const rhi::BufferH h = ctx.buffer(buffer);
                          for (usize at = 0; at < bytes.size(); at += 65536) {
                              const usize n = std::min<usize>(65536, bytes.size() - at);
                              ctx.cmd().updateBuffer(h, at, std::as_bytes(std::span(bytes)).subspan(at, n));
                          }
                      };
                      upload(f->view, f->viewBytes);
                      upload(f->instances, f->instanceBytes);
                  });

    graph.addPass("Clear", PassFlags::Copy,
                  [&](RgBuilder& b) {
                      f->color = b.write(b.create("SceneColor", RgTextureDesc::tex2D(kHdrFormat, f->width, f->height)),
                                         TextureWrite::CopyDest);
                  },
                  [f](RgContext& ctx) { ctx.cmd().clearTexture(ctx.texture(f->color), f->sky); });

    graph.addPass("Geometry", PassFlags::Raster,
                  [&](RgBuilder& b) {
                      f->color = b.colorAttachment(f->color, 0, rhi::LoadOp::Load);
                      f->depth = b.depthAttachment(b.create("SceneDepth", RgTextureDesc::tex2D(kDepthFormat, f->width, f->height)),
                                                   rhi::LoadOp::Clear, 0.0f);
                      b.read(f->view);
                      b.read(f->instances);
                      for (usize i = 0; i < f->vertices.size(); ++i) {
                          b.read(f->vertices[i]);
                          b.read(f->indices[i], BufferRead::Index);
                      }
                  },
                  [f, geometry](RgContext& ctx) {
                      rhi::CommandList& cmd = ctx.cmd();
                      cmd.bindPipeline(geometry);
                      const u64 view = ctx.deviceAddress(f->view);
                      const u64 instances = ctx.deviceAddress(f->instances);
                      for (u32 i = 0; i < f->drawList.size(); ++i) {
                          const ForwardInstance& inst = f->drawList[i];
                          if (inst.mesh >= f->meshes.size()) continue;
                          cmd.bindIndexBuffer(ctx.buffer(f->indices[inst.mesh]), 0, rhi::IndexType::Uint32);
                          cmd.pushConstants(ForwardPush{view, instances, ctx.deviceAddress(f->vertices[inst.mesh]), i, 0});
                          cmd.drawIndexed(f->meshes[inst.mesh].indexCount);
                      }
                  });

    if (settings.debugView) {
        graph.addPass("DebugNormals", PassFlags::Raster,
                      [&](RgBuilder& b) {
                          f->normals = b.colorAttachment(
                              b.create("DebugNormals", RgTextureDesc::tex2D(rhi::Format::RGBA8Unorm, f->width, f->height)), 0);
                          f->depth = b.depthAttachment(f->depth, rhi::LoadOp::Clear, 0.0f);
                          b.read(f->view);
                          b.read(f->instances);
                          for (usize i = 0; i < f->vertices.size(); ++i) {
                              b.read(f->vertices[i]);
                              b.read(f->indices[i], BufferRead::Index);
                          }
                      },
                      [f, debugNormals](RgContext& ctx) {
                          rhi::CommandList& cmd = ctx.cmd();
                          cmd.bindPipeline(debugNormals);
                          for (u32 i = 0; i < f->drawList.size(); ++i) {
                              const ForwardInstance& inst = f->drawList[i];
                              if (inst.mesh >= f->meshes.size()) continue;
                              cmd.bindIndexBuffer(ctx.buffer(f->indices[inst.mesh]), 0, rhi::IndexType::Uint32);
                              cmd.pushConstants(ForwardPush{ctx.deviceAddress(f->view), ctx.deviceAddress(f->instances),
                                                            ctx.deviceAddress(f->vertices[inst.mesh]), i, 0});
                              cmd.drawIndexed(f->meshes[inst.mesh].indexCount);
                          }
                      });
        if (settings.markDebugOutput) graph.markOutput(f->normals);
    }

    graph.addPass("Exposure", settings.asyncExposure ? PassFlags::AsyncCompute : PassFlags::Compute,
                  [&](RgBuilder& b) {
                      b.read(f->color);
                      f->exposure = b.write(b.create("Exposure", RgBufferDesc{16}));
                  },
                  [f, exposure](RgContext& ctx) {
                      ctx.cmd().bindPipeline(exposure);
                      ctx.cmd().pushConstants(
                          ExposurePush{ctx.deviceAddress(f->exposure), ctx.srv(f->color), f->width, f->height, f->exposureBias});
                      ctx.cmd().dispatch(1);
                  });

    graph.addPass("Tonemap", PassFlags::Raster,
                  [&](RgBuilder& b) {
                      b.read(f->color);
                      b.read(f->exposure);
                      f->output = b.colorAttachment(f->output, 0, rhi::LoadOp::DontCare);
                  },
                  [f, tonemap, encodeSrgb](RgContext& ctx) {
                      ctx.cmd().bindPipeline(tonemap);
                      ctx.cmd().pushConstants(TonemapPush{ctx.deviceAddress(f->exposure), ctx.srv(f->color), encodeSrgb});
                      ctx.cmd().draw(3);
                  });
    return f->output;
}

} // namespace helios::render
