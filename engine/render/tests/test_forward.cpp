// Forward pipeline v0: mesh generation, the deterministic infinite reverse-Z projection,
// camera-relative precision at 10^7 m, and a full frame on the Null backend (trace golden, culled
// debug view, identical command streams near the origin and far from it).

#include <doctest/doctest.h>

#include <cstring>

#include "golden_util.h"
#include "graph_test_util.h"
#include "helios/math/geometry.h"
#include "helios/render/forward.h"

using namespace graphtest;
using rhi::Format;
using rhi::ResourceState;

namespace {

void checkOutwardCcw(const MeshData& mesh) {
    REQUIRE(mesh.indices.size() % 3 == 0);
    for (usize i = 0; i < mesh.indices.size(); i += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 n = cross(b.position - a.position, c.position - a.position);
        CHECK(dot(n, a.normal + b.normal + c.normal) > 0.0f);
    }
    for (const MeshVertex& v : mesh.vertices) CHECK(length(v.normal) == doctest::Approx(1.0f).epsilon(1e-5));
}

ForwardScene makeScene(const DVec3& origin, std::vector<GpuMesh> meshes) {
    ForwardScene scene;
    scene.meshes = std::move(meshes);
    scene.camera.position = origin + DVec3{0.0, 2.0, 6.0};
    scene.camera.rotation = Quat::fromAxisAngle({1.0f, 0.0f, 0.0f}, -0.25f);
    auto place = [&](u32 mesh, DVec3 at, Vec4 color, f32 yaw = 0.0f, Vec3 scale = {1, 1, 1}) {
        ForwardInstance inst;
        inst.mesh = mesh;
        inst.transform.position = origin + at;
        inst.transform.rotation = Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
        inst.transform.scale = scale;
        inst.color = color;
        scene.instances.push_back(inst);
    };
    place(2, {0.0, 0.0, 0.0}, {0.55f, 0.55f, 0.5f, 1.0f});
    place(0, {-1.5, 0.5, 0.0}, {0.9f, 0.25f, 0.2f, 1.0f}, 0.6f);
    place(1, {1.5, 0.75, -0.5}, {0.2f, 0.45f, 0.9f, 1.0f}, 0.0f, {1.5f, 1.5f, 1.5f});
    return scene;
}

struct ForwardFixture : NullDeviceFixture {
    std::vector<GpuMesh> meshes;
    std::unique_ptr<ForwardRenderer> renderer;
    rhi::TextureH output;
    RgResourcePool pool{*device};

    ForwardFixture() {
        meshes.push_back(uploadMesh(*device, makeCube(), "Cube").value());
        meshes.push_back(uploadMesh(*device, makeUvSphere(0.5f, 16, 8), "Sphere").value());
        meshes.push_back(uploadMesh(*device, makePlane(4.0f), "Ground").value());
        auto r = ForwardRenderer::create(*device, Format::RGBA8Unorm);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
        renderer = std::move(r).value();
        output = texture("Output", Format::RGBA8Unorm, 160, 90,
                         rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc);
        null->clearTrace();
    }
    ~ForwardFixture() {
        renderer.reset();
        for (GpuMesh& m : meshes) destroyMesh(*device, m);
    }

    RenderGraph frame(const ForwardScene& scene, const ForwardSettings& settings = {}) {
        RenderGraph graph("Forward");
        RgTexture out = graph.importTexture("Output", output, device->textureDesc(output),
                                            {.finalState = ResourceState::CopySource});
        renderer->addPasses(graph, scene, out, settings);
        REQUIRE(graph.errors().empty());
        REQUIRE(graph.compile().ok());
        return graph;
    }
};

} // namespace

TEST_CASE("forward: built-in meshes are closed, counter-clockwise from outside, with unit normals") {
    const MeshData cube = makeCube(0.5f);
    CHECK(cube.vertices.size() == 24);
    CHECK(cube.indices.size() == 36);
    checkOutwardCcw(cube);
    const MeshData sphere = makeUvSphere(1.0f, 12, 6);
    CHECK(sphere.vertices.size() == 13u * 7u);
    CHECK(sphere.indices.size() == (12u * 6u * 2u - 2u * 12u) * 3u);  // pole quads collapse to one triangle
    checkOutwardCcw(sphere);
    for (const MeshVertex& v : sphere.vertices) CHECK(length(v.position) == doctest::Approx(1.0f).epsilon(1e-5));
    const MeshData plane = makePlane(2.0f);
    CHECK(plane.indices.size() == 6);
    checkOutwardCcw(plane);
}

TEST_CASE("forward: infinite reverse-Z projection") {
    const f32 fov = 1.0471976f;
    const Mat4 p = infiniteReverseZ(fov, 16.0f / 9.0f, 0.1f);
    auto depth = [&](f32 z) {
        const Vec4 c = p * Vec4{0.0f, 0.0f, z, 1.0f};
        return c.z / c.w;
    };
    CHECK(depth(-0.1f) == doctest::Approx(1.0f));   // near plane -> 1
    CHECK(depth(-0.2f) == doctest::Approx(0.5f));   // n / -z
    CHECK(depth(-1.0e9f) < 1.0e-9f);                 // -> 0 at infinity, never negative
    CHECK(depth(-1.0e9f) > 0.0f);
    // Same as the math module's builder (which uses std::tan) up to rounding.
    const Mat4 ref = Mat4::perspectiveReverseZ(fov, 16.0f / 9.0f, 0.1f);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) CHECK(p.at(r, c) == doctest::Approx(ref.at(r, c)).epsilon(1e-6));
    }
    // Deterministic constants: 1 / tan(30 deg) = sqrt(3), exactly rounded here.
    CHECK(p.at(1, 1) == doctest::Approx(1.7320508f).epsilon(1e-7));
}

TEST_CASE("forward: camera-relative data is identical near the origin and at 10^7 m") {
    std::vector<GpuMesh> none(3);
    const ForwardScene nearScene = makeScene({0.0, 0.0, 0.0}, none);
    const ForwardScene farScene = makeScene({1.0e7, 2.5e3, -3.0e6}, none);
    const std::vector<ForwardInstanceData> a = ForwardRenderer::instanceData(nearScene);
    const std::vector<ForwardInstanceData> b = ForwardRenderer::instanceData(farScene);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(ForwardInstanceData)) == 0);
    const ForwardViewConstants va = ForwardRenderer::viewConstants(nearScene, 16.0f / 9.0f);
    const ForwardViewConstants vb = ForwardRenderer::viewConstants(farScene, 16.0f / 9.0f);
    CHECK(std::memcmp(&va, &vb, sizeof(va)) == 0);
    // Naive f32 world positions at 10^7 m cannot even represent the scene (grid of 1 m).
    const f32 naive = static_cast<f32>(farScene.instances[1].transform.position.x) -
                      static_cast<f32>(farScene.camera.position.x);
    CHECK(naive != static_cast<f32>(nearScene.instances[1].transform.position.x - nearScene.camera.position.x));
}

TEST_CASE("forward: a frame on the Null backend (trace golden)") {
    ForwardFixture fx;
    const ForwardScene scene = makeScene({0.0, 0.0, 0.0}, fx.meshes);
    RenderGraph graph = fx.frame(scene, {.asyncExposure = true, .debugView = true});
    const RgPlan& plan = graph.plan();
    CHECK(pass(plan, "DebugNormals").culled);
    CHECK(pass(plan, "Exposure").queue == rhi::Queue::AsyncCompute);
    CHECK(post(plan, "Geometry") == "SceneColor:RenderTarget->ShaderResource");  // released to async compute
    CHECK(pass(plan, "Geometry").depthStore == rhi::StoreOp::DontCare);
    auto result = graph.execute(*fx.device, fx.pool);
    REQUIRE(result.ok());
    CHECK_NULL_CLEAN(fx);
    CHECK(fx.null->textureState(fx.output) == ResourceState::CopySource);
    const std::string trace = fx.null->trace();
    CHECK(trace.find("drawIndexed indices=36 instances=1") != std::string::npos);
    CHECK(trace.find("dispatch 1x1x1") != std::string::npos);
    CHECK(trace.find("draw vertices=3 instances=1") != std::string::npos);
    goldentest::checkTextGolden("forward_frame", rgDumpPlan(graph.executedPlan()) + "--- Null trace ---\n" + trace);
}

TEST_CASE("forward: the command stream does not depend on the distance from the origin") {
    std::string traces[2];
    for (int far = 0; far < 2; ++far) {
        ForwardFixture fx;
        const ForwardScene scene = makeScene(far ? DVec3{1.0e7, 2.5e3, -3.0e6} : DVec3{}, fx.meshes);
        RenderGraph graph = fx.frame(scene);
        REQUIRE(graph.execute(*fx.device, fx.pool).ok());
        CHECK_NULL_CLEAN(fx);
        traces[far] = fx.null->trace();
    }
    CHECK(traces[0] == traces[1]);
}

TEST_CASE("forward: synchronous exposure and marked debug output") {
    ForwardFixture fx;
    const ForwardScene scene = makeScene({}, fx.meshes);
    RenderGraph graph = fx.frame(scene, {.asyncExposure = false, .debugView = true, .markDebugOutput = true});
    const RgPlan& plan = graph.plan();
    CHECK(!pass(plan, "DebugNormals").culled);
    CHECK(pass(plan, "Exposure").queue == rhi::Queue::Graphics);
    CHECK(plan.batches.size() == 1);
    REQUIRE(graph.execute(*fx.device, fx.pool).ok());
    CHECK_NULL_CLEAN(fx);
}
