// RT-03, Phase 0 scope (02 §8.2, 09 WP-0.9): the scripted scene's state hash is a golden constant.
//
// The SAME golden must hold on every determinism toolchain (both MSVC toolsets, clang-cl, GCC, Clang,
// MinGW; ADR-001a rule 7) at any worker count: Jolt runs with JPH_CROSS_PLATFORM_DETERMINISTIC and no
// FMA, and every Helios-side order is by (layer, key). This container builds GCC, Clang and MinGW but
// runs only the Linux binaries; CI's Windows jobs (windows-msvc, windows-msvc-floor, windows-clang-cl)
// are the cross-compiler check for MSVC and run these same assertions. Never update a golden to make
// one platform pass: find the platform's codegen difference instead (09 §5.3).
#include <doctest/doctest.h>

#include <cstdio>

#include "helios/core/jobs.h"
#include "scene.h"

using namespace helios;
using namespace helios::physics;
using namespace helios::physics::test;

namespace {

// Goldens of the scripted scene (scene.cpp) after 600 fixed steps of 1/60 s.
constexpr u64 kGoldenFullScene600 = 0xff972a409e8145e1ull;
// Goldens of the independent-islands scene after 600 steps.
constexpr u64 kGoldenIndependent600 = 0x1cb97ff0f53a5b1eull;

std::string hex(u64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llxull", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

TEST_CASE("determinism: the scripted scene matches its golden hash (RT-03 Phase 0)") {
    PhysicsRuntime runtime;
    Scene s({});
    // Intermediate hashes help bisect a divergence to the first differing second.
    for (u32 second = 1; second <= 10; ++second) {
        const u64 h = s.run(60);
        MESSAGE("t = " << second << " s: " << hex(h));
    }
    const u64 h = s.grid().stateHash();
    CHECK_MESSAGE(h == kGoldenFullScene600, "state hash " << hex(h));
}

TEST_CASE("determinism: the golden holds at 1, 2, 4 and 16 workers") {
    PhysicsRuntime runtime;
    for (u32 workers : {1u, 2u, 4u, 16u}) {
        jobs::JobSystem js({.workerCount = workers, .name = "Rt03"});
        Scene s({.jobs = &js});
        const u64 h = s.run(600);
        CHECK_MESSAGE(h == kGoldenFullScene600, "workers " << workers << ": " << hex(h));
    }
}

TEST_CASE("determinism: repeated runs in one process are identical") {
    PhysicsRuntime runtime;
    Scene a({}), b({});
    for (int i = 0; i < 300; ++i) {
        a.step();
        b.step();
        REQUIRE(a.grid().stateHash() == b.grid().stateHash());
    }
}

TEST_CASE("determinism: permuted BodyIDs give the same result when no island has two contacts") {
    // RT-03's permuted variant, the part stock Jolt already satisfies: every body rests on the
    // ground alone, so the solver order inside an island cannot matter, and everything Helios orders
    // (state hash, creation batches) uses (layer, key).
    PhysicsRuntime runtime;
    Scene plain({.independentOnly = true});
    Scene permuted({.permuteBodyIds = true, .independentOnly = true});
    const u64 a = plain.run(600), b = permuted.run(600);
    CHECK_MESSAGE(a == b, hex(a) << " vs " << hex(b));
    CHECK_MESSAGE(a == kGoldenIndependent600, "state hash " << hex(a));
}

TEST_CASE("determinism: KNOWN DIVERGENCE: permuted BodyIDs change multi-contact islands (stable-order patch pending)") {
    // Stock Jolt 5.6 orders contact constraints by a hash of BodyIDs (ContactConstraintManager's
    // sort key), makes the lower BodyID "body 1" and orders CharacterVirtual contacts by BodyID, so a
    // pyramid solved in a different BodyID order diverges in the low bits (02 §7.1). The vendored
    // third_party/jolt/patches/stable-order patch replaces those orders with (layer, key) from
    // mUserData; until it lands, this case pins the divergence. When the patch is applied, this
    // CHECK fails: flip it to CHECK(plain == permuted), which is RT-03's permuted variant, and
    // re-pin kGoldenFullScene600 (the patch changes the solver order, so the golden changes once).
    PhysicsRuntime runtime;
    Scene plain({});
    Scene permuted({.permuteBodyIds = true});
    const u64 a = plain.run(600), b = permuted.run(600);
    MESSAGE("plain " << hex(a) << ", permuted " << hex(b));
    CHECK(a == kGoldenFullScene600);
    CHECK(a != b);
}
