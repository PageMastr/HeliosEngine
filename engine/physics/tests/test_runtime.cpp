// PhysicsRuntime (Jolt hooks, type registration) and shapes.
#include <doctest/doctest.h>

#include <limits>

#include "helios/core/memory.h"
#include "helios/physics/physics.h"

using namespace helios;
using namespace helios::physics;

TEST_CASE("runtime: reference-counted initialization") {
    CHECK_FALSE(PhysicsRuntime::initialized());
    {
        PhysicsRuntime a;
        CHECK(PhysicsRuntime::initialized());
        {
            PhysicsRuntime b; // nested: no re-registration
            CHECK(PhysicsRuntime::initialized());
        }
        CHECK(PhysicsRuntime::initialized());
    }
    CHECK_FALSE(PhysicsRuntime::initialized());
    // Re-initialization after a full shutdown works.
    PhysicsRuntime again;
    CHECK(PhysicsRuntime::initialized());
}

TEST_CASE("runtime: Jolt allocations go through the tagged Helios allocator") {
    PhysicsRuntime runtime;
    const MemoryTagStats before = memoryTagStats(PhysicsRuntime::memoryTag());
    {
        auto grid = PhysicsGrid::create({});
        REQUIRE(grid.ok());
        const MemoryTagStats during = memoryTagStats(PhysicsRuntime::memoryTag());
        CHECK(during.liveBytes > before.liveBytes + (1 << 20)); // the grid's temp allocator alone is 8 MB
        CHECK(during.totalAllocations > before.totalAllocations);
    }
    const MemoryTagStats after = memoryTagStats(PhysicsRuntime::memoryTag());
    CHECK(after.liveBytes == before.liveBytes); // no leak
    CHECK(memoryTagName(PhysicsRuntime::memoryTag()) == "physics.jolt");
}

TEST_CASE("runtime: grids and shapes need a live runtime") {
    CHECK(PhysicsGrid::create({}).errorCode() == ErrorCode::InvalidState);
    CHECK(createSphere(1.0f).errorCode() == ErrorCode::InvalidState);
}

TEST_CASE("shapes: creation, validation and shared references") {
    PhysicsRuntime runtime;
    auto box = createBox(Vec3(1.0f, 2.0f, 3.0f));
    REQUIRE(box.ok());
    CHECK(box->localHalfExtents().x == doctest::Approx(1.0f));
    CHECK(box->localHalfExtents().z == doctest::Approx(3.0f));
    CHECK(box->useCount() == 1);
    {
        ShapeRef copy = *box;
        CHECK(box->useCount() == 2);
        ShapeRef moved = std::move(copy);
        CHECK(box->useCount() == 2);
        CHECK_FALSE(copy.valid());
        CHECK(moved == *box);
    }
    CHECK(box->useCount() == 1);
    CHECK(createSphere(0.5f).ok());
    CHECK(createCapsule(1.0f, 0.3f).ok());
    CHECK(createCylinder(1.0f, 0.5f).ok());
    const Vec3 tetra[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    CHECK(createConvexHull(tetra).ok());
    const Vec3 verts[] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const u32 tri[] = {0, 1, 2};
    CHECK(createMesh(verts, tri).ok());
    const CompoundChild kids[] = {{*box, Vec3(1, 0, 0), Quat::identity()}, {createSphere(1.0f).value(), Vec3(-2, 0, 0), Quat::identity()}};
    auto compound = createStaticCompound(kids);
    REQUIRE(compound.ok());
    CHECK(box->useCount() >= 2); // the compound holds the child shape

    // Validation.
    CHECK(createBox(Vec3(0.01f, 1.0f, 1.0f)).errorCode() == ErrorCode::InvalidArgument); // below the convex radius
    CHECK(createSphere(0.0f).errorCode() == ErrorCode::InvalidArgument);
    CHECK(createSphere(-1.0f).errorCode() == ErrorCode::InvalidArgument);
    CHECK(createCapsule(1.0f, 0.0f).errorCode() == ErrorCode::InvalidArgument);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(createBox(Vec3(nan, 1.0f, 1.0f)).errorCode() == ErrorCode::InvalidArgument);
    CHECK(createConvexHull(std::span<const Vec3>(tetra, 3)).errorCode() == ErrorCode::InvalidArgument);
    const u32 badTri[] = {0, 1, 7};
    CHECK(createMesh(verts, badTri).errorCode() == ErrorCode::OutOfRange);
    CHECK(createMesh(verts, std::span<const u32>(tri, 2)).errorCode() == ErrorCode::InvalidArgument);
    CHECK(createStaticCompound({}).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("shapes: compound children need a finite pose and a non-zero rotation") {
    PhysicsRuntime runtime;
    const ShapeRef ball = createSphere(0.5f).value();
    CompoundChild kids[] = {{ball, Vec3(1, 0, 0), Quat(0.0f, 0.0f, 0.0f, 0.0f)}, {ball, Vec3(-1, 0, 0), Quat::identity()}};
    CHECK(createStaticCompound(kids).errorCode() == ErrorCode::InvalidArgument);
    kids[0].rotation = Quat::identity();
    kids[0].position.x = std::numeric_limits<f32>::infinity();
    CHECK(createStaticCompound(kids).errorCode() == ErrorCode::InvalidArgument);
    kids[0].position.x = 1.0f;
    CHECK(createStaticCompound(kids).ok());
}
