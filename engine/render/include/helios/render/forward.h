#pragma once
// Minimal forward pipeline v0 (WP-0.12): proves the render graph end to end.
//
//   Upload (copy)      per-frame view constants and instance transforms -> transient buffers
//   Clear (copy)       HDR scene color <- sky color
//   Geometry (raster)  meshes into the HDR color with reverse-Z depth (vertex pulling via buffer
//                      device addresses, one drawIndexed per instance)
//   Exposure (async)   average log luminance of the HDR color -> exposure buffer (async compute)
//   Tonemap (raster)   fullscreen triangle: exposure, ACES fit, sRGB encode -> output
//   [DebugNormals]     optional debug view, culled unless marked as output
//
// Precision (03 §2.7, ADR-005): instance and camera positions are frame-local f64; the CPU forms
// `instance - camera` in f64 and uploads only camera-relative f32 matrices; the view matrix is
// rotation-only and the projection is infinite reverse-Z (depth = near / -z, clear 0,
// GreaterOrEqual). The projection is built with deterministic trigonometry so the uploaded bytes,
// and hence Null trace goldens, are identical on every compiler and CRT.
//
// Threading: ForwardRenderer is immutable after create(); addPasses() may be called from one thread
// per graph; the recorded callbacks only read their captured per-frame data.

#include <memory>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/math/mat.h"
#include "helios/math/transform.h"
#include "helios/math/vec.h"
#include "helios/render/render_graph.h"
#include "helios/rhi/device.h"

namespace helios::render {

/// Vertex of the built-in meshes (scalar layout, 24 bytes; matches shaders/forward.slang).
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
};
static_assert(sizeof(MeshVertex) == 24);

/// CPU mesh with counter-clockwise front faces (seen from outside).
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
};
MeshData makeCube(f32 halfExtent = 0.5f);
MeshData makeUvSphere(f32 radius = 0.5f, u32 segments = 24, u32 rings = 12);
MeshData makePlane(f32 halfSize = 5.0f);

/// Vertex and index buffers left in ShaderResource / IndexBuffer (import them with those states).
struct GpuMesh {
    rhi::BufferH vertices;
    rhi::BufferH indices;
    u32 indexCount = 0;
};
/// Uploads a mesh (blocking) and transitions it for drawing. Thread-safe like Device.
Result<GpuMesh> uploadMesh(rhi::Device& device, const MeshData& mesh, std::string_view name);
void destroyMesh(rhi::Device& device, GpuMesh& mesh);

/// Camera: frame-local f64 position, orientation (looks down its local -Z), vertical FOV, near plane.
struct Camera {
    DVec3 position{};
    Quat rotation{};
    f32 fovY = 1.0471976f;  // 60 degrees
    f32 zNear = 0.1f;
};

/// Infinite reverse-Z projection (03 §2.7): clip = (f/a x, f y, n, -z), depth = n / -z. Uses
/// det:: trigonometry, so the result is bit-identical on every platform (math's
/// Mat4::perspectiveReverseZ uses std::tan and may differ in the last bit between CRTs).
Mat4 infiniteReverseZ(f32 fovY, f32 aspect, f32 zNear) noexcept;

struct ForwardInstance {
    u32 mesh = 0;             ///< Index into ForwardScene::meshes.
    DTransform transform;     ///< Frame-local (f64 position).
    Vec4 color{1, 1, 1, 1};   ///< Linear base color.
};

struct ForwardScene {
    std::vector<GpuMesh> meshes;
    std::vector<ForwardInstance> instances;
    Camera camera;
    Vec3 lightDirection{0.3f, 0.8f, 0.5f};  ///< Towards the light (world axes; normalized by the renderer).
    Vec3 lightColor{3.0f, 2.9f, 2.7f};
    Vec3 ambient{0.25f, 0.28f, 0.35f};
    Vec4 skyColor{0.35f, 0.45f, 0.65f, 1.0f};  ///< Linear HDR clear color.
    f32 exposureBias = 0.0f;  ///< EV added to the automatic exposure.
};

struct ForwardSettings {
    /// Exposure on the async-compute queue (graphics when false).
    bool asyncExposure = true;
    /// Adds the DebugNormals view (culled unless `markDebugOutput`).
    bool debugView = false;
    bool markDebugOutput = false;
};

/// GPU layouts shared with the shaders (scalar block layout).
struct ForwardViewConstants {
    Mat4 viewProj;         ///< Projection * rotation-only view (camera-relative).
    Vec4 lightDirection;   ///< xyz towards the light, normalized.
    Vec4 lightColor;
    Vec4 ambient;
};
static_assert(sizeof(ForwardViewConstants) == 112);
struct ForwardInstanceData {
    Mat4 localToCameraRelative;
    Vec4 color;
};
static_assert(sizeof(ForwardInstanceData) == 80);

class ForwardRenderer {
public:
    /// Creates the pipelines from the embedded SPIR-V (forward, exposure, tonemap) for an output
    /// texture of `outputFormat` (an sRGB format gets hardware encoding, UNORM formats are encoded
    /// in the shader). Checks the push-constant layouts against the shaders' reflection.
    static Result<std::unique_ptr<ForwardRenderer>> create(rhi::Device& device, rhi::Format outputFormat);
    ~ForwardRenderer();
    ForwardRenderer(const ForwardRenderer&) = delete;
    ForwardRenderer& operator=(const ForwardRenderer&) = delete;

    /// Adds the pass set rendering `scene` into `output` (a color-attachable texture of the
    /// create() format, usually imported). Returns the output's new version.
    RgTexture addPasses(RenderGraph& graph, const ForwardScene& scene, RgTexture output,
                        const ForwardSettings& settings = {}) const;

    /// Per-frame data the Upload pass writes (exposed for precision tests).
    static ForwardViewConstants viewConstants(const ForwardScene& scene, f32 aspect) noexcept;
    static std::vector<ForwardInstanceData> instanceData(const ForwardScene& scene) noexcept;

private:
    ForwardRenderer() = default;
    rhi::Device* m_device = nullptr;
    rhi::Format m_outputFormat = rhi::Format::RGBA8Unorm;
    rhi::PipelineH m_geometry;
    rhi::PipelineH m_exposure;
    rhi::PipelineH m_tonemap;
    rhi::PipelineH m_debugNormals;
};

} // namespace helios::render
